/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <memory>
#include <sys/stat.h>

#include "configfile.h"

#include "json_adapter.h"

#include "kis_external.h"
#include "kis_external_packet.h"

#include "endian_magic.h"

#include "timetracker.h"
#include "messagebus.h"

#include "protobuf_cpp/kismet.pb.h"
#include "protobuf_cpp/http.pb.h"
#include "protobuf_cpp/eventbus.pb.h"

kis_external_interface::kis_external_interface() :
    stopped{true},
    cancelled{false},
    timetracker{Globalreg::fetch_mandatory_global_as<time_tracker>()},
    ipctracker{Globalreg::fetch_mandatory_global_as<ipc_tracker_v2>()},
    seqno{0},
    last_pong{0},
    ping_timer_id{-1},
    ipc_in{Globalreg::globalreg->io},
    ipc_out{Globalreg::globalreg->io},
    tcpsocket{Globalreg::globalreg->io},
    eventbus{Globalreg::fetch_mandatory_global_as<event_bus>()},
    http_session_id{0} {

    ext_mutex.set_name("kis_external_interface");
}

kis_external_interface::~kis_external_interface() {
    close_external();
}

bool kis_external_interface::attach_tcp_socket(tcp::socket& socket) {
    local_locker l(&ext_mutex, "kei:attach_tcp_socket");

    stopped = true;
    in_buf.consume(in_buf.size());

    if (ipc.pid > 0) {
        _MSG_ERROR("Tried to attach a TCP socket to an external endpoint that already has "
                "an IPC instance running.");
        return false;
    }

    tcpsocket = std::move(socket);

    stopped = false;
    cancelled = false;

    start_tcp_read(shared_from_this());

    return true;
}

void kis_external_interface::close_external() {
    stopped = true;
    cancelled = true;

    local_locker l(&ext_mutex, "kei::close");

    // Kill any eventbus listeners
    for (const auto& ebid : eventbus_callback_map)
        eventbus->remove_listener(ebid.second);

    // Kill any active http sessions
    for (auto s : http_proxy_session_map) {
        // Fail them
        s.second->connection->response_stream().cancel();
        // Unlock them and let the cleanup in the thread handle it and close down 
        // the http server session
        s.second->locker->unlock();
    }

    timetracker->remove_timer(ping_timer_id);

    ipc_hard_kill();

    if (tcpsocket.is_open()) {
        try {
            tcpsocket.cancel();
            tcpsocket.close();
        } catch (const std::exception& e) {
            ;
        }
    }

    write_cb = nullptr;
    closure_cb = nullptr;
};

void kis_external_interface::ipc_soft_kill() {
    stopped = true;
    cancelled = true;

    if (ipc_in.is_open()) {
        try {
            ipc_in.cancel();
            ipc_in.close();
        } catch (const std::exception& e) {
            ;
        }
    }

    if (ipc_out.is_open()) {
        try {
            ipc_out.cancel();
            ipc_out.close();
        } catch (const std::exception& e) {
            ;
        }
    }

    if (ipc.pid > 0) {
        ipctracker->remove_ipc(ipc.pid);
        kill(ipc.pid, SIGTERM);
    }
}

void kis_external_interface::ipc_hard_kill() {
    stopped = true;
    cancelled = true;

    if (ipc_in.is_open()) {
        try {
            ipc_in.cancel();
            ipc_in.close();
        } catch (const std::exception& e) {
            ;
        }
    }

    if (ipc_out.is_open()) {
        try {
            ipc_out.cancel();
            ipc_out.close();
        } catch (const std::exception& e) {
            ;
        }
    }

    if (ipc.pid > 0) {
        ipctracker->remove_ipc(ipc.pid);
        kill(ipc.pid, SIGKILL);
    }

}

void kis_external_interface::trigger_error(const std::string& in_error) {
    // Don't loop if we're already stopped
    if (stopped)
        return;

    handle_error(in_error);

    close_external();
}

void kis_external_interface::start_ipc_read(std::shared_ptr<kis_external_interface> ref) {
    if (stopped)
        return;

    boost::asio::async_read(ipc_in, in_buf,
            boost::asio::transfer_at_least(sizeof(kismet_external_frame_t)),
            [this, ref](const boost::system::error_code& ec, std::size_t t) {
            if (handle_read(ref, ec, t) > 0)
                start_ipc_read(ref);
            else
                close_external();
            });
}

void kis_external_interface::start_tcp_read(std::shared_ptr<kis_external_interface> ref) {
    if (stopped)
        return;

    boost::asio::async_read(tcpsocket, in_buf,
            boost::asio::transfer_at_least(sizeof(kismet_external_frame_t)),
            [this, ref](const boost::system::error_code& ec, std::size_t t) {
            if (handle_read(ref, ec, t) >= 0)
                start_tcp_read(ref);
            });
}

int kis_external_interface::handle_read(std::shared_ptr<kis_external_interface> ref, 
        const boost::system::error_code& ec, size_t in_amt) {

    if (stopped)
        return 0;

    if (cancelled) {
        close_external();
        return 0;
    }

    if (ec) {
        stopped = true;

        // Exit on aborted errors, we've already been cancelled and this socket is closing out
        if (ec.value() == boost::asio::error::operation_aborted) {
            return -1;
        }

        // Be quiet about EOF
        if (ec.value() == boost::asio::error::eof) {
            trigger_error("External socket closed");
        } else {
            _MSG_ERROR("External API handler got error reading data: {}", ec.message());
            trigger_error(ec.message());
        }

        return -1;
    }

    return handle_packet(in_buf);
}

bool kis_external_interface::check_ipc(const std::string& in_binary) {
    struct stat fstat;

    std::vector<std::string> bin_paths = 
        Globalreg::globalreg->kismet_config->fetch_opt_vec("helper_binary_path");

    if (bin_paths.size() == 0) {
        bin_paths.push_back("%B");
    }

    for (auto rp : bin_paths) {
        std::string fp = fmt::format("{}/{}",
                Globalreg::globalreg->kismet_config->expand_log_path(rp, "", "", 0, 1),
                in_binary);

        if (stat(fp.c_str(), &fstat) != -1) {
            if (S_ISDIR(fstat.st_mode))
                continue;

            if ((S_IXUSR & fstat.st_mode))
                return true;
        }
    }

    return false;
}

bool kis_external_interface::run_ipc() {
    local_locker l(&ext_mutex, "kei::run_ipc");

    struct stat fstat;

    stopped = true;
    in_buf.consume(in_buf.size());

    if (external_binary == "") {
        _MSG("Kismet external interface did not have an IPC binary to launch", MSGFLAG_ERROR);
        return false;
    }

    // Get allowed paths for binaries
    auto bin_paths = 
        Globalreg::globalreg->kismet_config->fetch_opt_vec("helper_binary_path");

    if (bin_paths.size() == 0) {
        _MSG("No helper_binary_path found in kismet.conf, make sure your config "
                "files are up to date; using the default binary path where Kismet "
                "is installed.", MSGFLAG_ERROR);
        bin_paths.push_back("%B");
    }

    std::string helper_path;

    for (auto rp : bin_paths) {
        std::string fp = fmt::format("{}/{}",
                Globalreg::globalreg->kismet_config->expand_log_path(rp, "", "", 0, 1), external_binary);

        if (stat(fp.c_str(), &fstat) != -1) {
            if (S_ISDIR(fstat.st_mode))
                continue;

            if ((S_IXUSR & fstat.st_mode)) {
                helper_path = fp;
                break;
            }
        }
    }

    if (helper_path.length() == 0) {
        _MSG_ERROR("Kismet external interface can not find IPC binary for launch: {}",
                external_binary);
        return false;
    }

    // See if we can execute the IPC tool
    if (!(fstat.st_mode & S_IXOTH)) {
        if (getuid() != fstat.st_uid && getuid() != 0) {
            bool group_ok = false;
            gid_t *groups;
            int ngroups;

            if (getgid() != fstat.st_gid) {
                ngroups = getgroups(0, NULL);

                if (ngroups > 0) {
                    groups = new gid_t[ngroups];
                    ngroups = getgroups(ngroups, groups);

                    for (int g = 0; g < ngroups; g++) {
                        if (groups[g] == fstat.st_gid) {
                            group_ok = true;
                            break;
                        }
                    }

                    delete[] groups;
                }

                if (!group_ok) {
                    _MSG_ERROR("IPC cannot run binary '{}', Kismet was installed "
                            "setgid and you are not in that group. If you recently added your "
                            "user to the kismet group, you will need to log out and back in to "
                            "activate it.  You can check your groups with the 'groups' command.",
                            helper_path);
                    return false;
                }
            }
        }
    }

    // 'in' to the spawned process, write to the server process, 
    // [1] belongs to us, [0] to them
    int inpipepair[2];
    // 'out' from the spawned process, read to the server process, 
    // [0] belongs to us, [1] to them
    int outpipepair[2];

    if (pipe(inpipepair) < 0) {
        _MSG_ERROR("IPC could not create pipe: {}", kis_strerror_r(errno));
        return false;
    }

    if (pipe(outpipepair) < 0) {
        _MSG_ERROR("IPC could not create pipe: {}", kis_strerror_r(errno));
        ::close(inpipepair[0]);
        ::close(inpipepair[1]);
        return false;
    }

    // We don't need to do signal masking because we run a dedicated signal handling thread

    pid_t child_pid;
    char **cmdarg;

    if ((child_pid = fork()) < 0) {
        _MSG_ERROR("IPC could not fork(): {}", kis_strerror_r(errno));
        ::close(inpipepair[0]);
        ::close(inpipepair[1]);
        ::close(outpipepair[0]);
        ::close(outpipepair[1]);

        return false;
    } else if (child_pid == 0) {
        // We're the child process

        // Unblock all signals in the child so nothing carries over from the parent fork
        sigset_t unblock_mask;
        sigfillset(&unblock_mask);
        pthread_sigmask(SIG_UNBLOCK, &unblock_mask, nullptr);
      
        // argv[0], "--in-fd" "--out-fd" ... NULL
        cmdarg = new char*[external_binary_args.size() + 4];
        cmdarg[0] = strdup(helper_path.c_str());

        // Child reads from inpair
        std::string argstr;

        argstr = fmt::format("--in-fd={}", inpipepair[0]);
        cmdarg[1] = strdup(argstr.c_str());

        // Child writes to writepair
        argstr = fmt::format("--out-fd={}", outpipepair[1]);
        cmdarg[2] = strdup(argstr.c_str());

        for (unsigned int x = 0; x < external_binary_args.size(); x++)
            cmdarg[x+3] = strdup(external_binary_args[x].c_str());

        cmdarg[external_binary_args.size() + 3] = NULL;

        // close the unused half of the pairs on the child
        ::close(inpipepair[1]);
        ::close(outpipepair[0]);

        execvp(cmdarg[0], cmdarg);

        exit(255);
    } 

    // Parent process
   
    // close the remote side of the pipes from the parent, they're open in the child
    ::close(inpipepair[0]);
    ::close(outpipepair[1]);

    ipc_out = boost::asio::posix::stream_descriptor(Globalreg::globalreg->io, inpipepair[1]);
    ipc_in = boost::asio::posix::stream_descriptor(Globalreg::globalreg->io, outpipepair[0]);

    ipc = kis_ipc_record(child_pid,
            [this](const std::string&) {
            close_external();
            },
            [this](const std::string& err) {
            trigger_error(err);
            });
    ipctracker->register_ipc(ipc);

    stopped = false;
    cancelled = false;

    start_ipc_read(shared_from_this());

    return true;
}


unsigned int kis_external_interface::send_packet(std::shared_ptr<KismetExternal::Command> c) {
    local_locker lock(&ext_mutex, "kei::send_packet");

    // Set the sequence if one wasn't provided
    if (c->seqno() == 0) {
        if (++seqno == 0)
            seqno = 1;

        c->set_seqno(seqno);
    }

    uint32_t data_csum;

    // Get the serialized size of our message
#if GOOGLE_PROTOBUF_VERSION >= 3006001
    size_t content_sz = c->ByteSizeLong();
#else
    size_t content_sz = c->ByteSize();
#endif

    // Calc frame size
    ssize_t frame_sz = sizeof(kismet_external_frame_t) + content_sz;

    // Our actual frame
    char frame_buf[frame_sz];
    kismet_external_frame_t *frame = reinterpret_cast<kismet_external_frame_t *>(frame_buf);

    // Fill in the headers
    frame->signature = kis_hton32(KIS_EXTERNAL_PROTO_SIG);
    frame->data_sz = kis_hton32(content_sz);

    // serialize into our array
    c->SerializeToArray(frame->data, content_sz);

    // Calculate the checksum and set it in the frame
    data_csum = adler32_checksum((const char *) frame->data, content_sz); 
    frame->data_checksum = kis_hton32(data_csum);

    if (write_cb != nullptr) {
        write_cb(frame_buf, frame_sz,
                [this](int ec, std::size_t) {
                if (ec) {
                    if (ec == boost::asio::error::operation_aborted)
                        return;

                    _MSG_ERROR("Kismet external interface got error writing a packet to a callback interface.");
                    trigger_error("write failure");
                    return;
                }
                });
    } else if (ipc_out.is_open()) {
        boost::asio::async_write(ipc_out, boost::asio::buffer(frame_buf, frame_sz),
                [this](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    if (ec.value() == boost::asio::error::operation_aborted)
                        return;

                    _MSG_ERROR("Kismet external interface got an error writing a packet to an "
                            "IPC interface: {}", ec.message());
                    trigger_error("write failure");
                    return;
                }
                });
    } else if (tcpsocket.is_open()) {
        boost::asio::async_write(tcpsocket, boost::asio::buffer(frame_buf, frame_sz),
                [this](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    if (ec.value() == boost::asio::error::operation_aborted)
                        return;

                    _MSG_ERROR("Kismet external interface got an error writing a packet to a "
                            "TCP interface: {}", ec.message());
                    trigger_error("write failure");
                    return;
                }
                });
    } else {
        _MSG_ERROR("Kismet external interface got an error writing packet, no connections");
        trigger_error("no connections");
        return 0;
    }

    return c->seqno();
}

bool kis_external_interface::dispatch_rx_packet(std::shared_ptr<KismetExternal::Command> c) {
    // Simple dispatcher; this should be called by child implementations who
    // add their own commands
    if (c->command() == "MESSAGE") {
        handle_packet_message(c->seqno(), c->content());
        return true;
    } else if (c->command() == "PING") {
        handle_packet_ping(c->seqno(), c->content());
        return true;
    } else if (c->command() == "PONG") {
        handle_packet_pong(c->seqno(), c->content());
        return true;
    } else if (c->command() == "SHUTDOWN") {
        handle_packet_shutdown(c->seqno(), c->content());
        return true;
    } else if (c->command() == "HTTPREGISTERURI") {
        handle_packet_http_register(c->seqno(), c->content());
        return true;
    } else if (c->command() == "HTTPRESPONSE") {
        handle_packet_http_response(c->seqno(), c->content());
        return true;
    } else if (c->command() == "HTTPAUTHREQ") {
        handle_packet_http_auth_request(c->seqno(), c->content());
        return true;
    } else if (c->command() == "EVENTBUSREGISTER") {
        handle_packet_eventbus_register(c->seqno(), c->content());
        return true;
    } else if (c->command() == "EVENTBUSPUBLISH") {
        handle_packet_eventbus_publish(c->seqno(), c->content());
        return true;
    }

    return false;
}

void kis_external_interface::handle_packet_message(uint32_t in_seqno, const std::string& in_content) {
    KismetExternal::MsgbusMessage m;

    if (!m.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparsable MESSAGE", MSGFLAG_ERROR);
        trigger_error("Invalid MESSAGE");
        return;
    }

    handle_msg_proxy(m.msgtext(), m.msgtype());
}

void kis_external_interface::handle_msg_proxy(const std::string& msg, const int msgtype) {
    _MSG(msg, msgtype);
}

void kis_external_interface::handle_packet_ping(uint32_t in_seqno, const std::string& in_content) {
    send_pong(in_seqno);
}

void kis_external_interface::handle_packet_pong(uint32_t in_seqno, const std::string& in_content) {
    local_locker lock(&ext_mutex, "kei::handle_packet_pong");

    KismetExternal::Pong p;
    if (!p.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparsable PONG packet", MSGFLAG_ERROR);
        trigger_error("Invalid PONG");
        return;
    }

    last_pong = time(0);
}

void kis_external_interface::handle_packet_shutdown(uint32_t in_seqno, const std::string& in_content) {
    local_locker lock(&ext_mutex, "kei::handle_packet_shutdown");

    KismetExternal::ExternalShutdown s;
    if (!s.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparsable SHUTDOWN", MSGFLAG_ERROR);
        trigger_error("invalid SHUTDOWN");
        return;
    }

    _MSG(std::string("Kismet external interface shutting down: ") + s.reason(), MSGFLAG_INFO); 
    trigger_error(std::string("Remote connection requesting shutdown: ") + s.reason());
}

unsigned int kis_external_interface::send_ping() {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_command("PING");

    KismetExternal::Ping p;
    c->set_content(p.SerializeAsString());

    return send_packet(c);
}

unsigned int kis_external_interface::send_pong(uint32_t ping_seqno) {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_command("PONG");

    KismetExternal::Pong p;
    p.set_ping_seqno(ping_seqno);

    c->set_content(p.SerializeAsString());

    return send_packet(c);
}

unsigned int kis_external_interface::send_shutdown(std::string reason) {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_command("SHUTDOWN");

    KismetExternal::ExternalShutdown s;
    s.set_reason(reason);

    c->set_content(s.SerializeAsString());

    return send_packet(c);
}

void kis_external_interface::proxy_event(std::shared_ptr<eventbus_event> evt) {
    auto c = std::make_shared<KismetExternal::Command>();

    c->set_command("EVENT");

    std::stringstream ss;

    json_adapter::pack(ss, evt);

    KismetEventBus::EventbusEvent ebe;
    ebe.set_event_json(ss.str());

    c->set_content(ebe.SerializeAsString());

    send_packet(c);
}

void kis_external_interface::handle_packet_eventbus_register(uint32_t in_seqno,
        const std::string& in_content) {
    local_locker lock(&ext_mutex, "kis_external_interface::handle_packet_eventbus_register");

    KismetEventBus::EventbusRegisterListener evtlisten;

    if (!evtlisten.ParseFromString(in_content)) {
        _MSG_ERROR("Kismet external interface got an unparseable EVENTBUSREGISTER");
        trigger_error("Invalid EVENTBUSREGISTER");
        return;
    }

    for (int e = 0; e < evtlisten.event_size(); e++) {
        auto k = eventbus_callback_map.find(evtlisten.event(e));

        if (k != eventbus_callback_map.end())
            eventbus->remove_listener(k->second);

        unsigned long eid = 
            eventbus->register_listener(evtlisten.event(e), 
                    [this](std::shared_ptr<eventbus_event> e) {
                    proxy_event(e);
                    });

        eventbus_callback_map[evtlisten.event(e)] = eid;
    }
}

void kis_external_interface::handle_packet_eventbus_publish(uint32_t in_seqno,
        const std::string& in_content) {
    local_locker lock(&ext_mutex, "kis_external_interface::handle_packet_eventbus_publish");
    
    KismetEventBus::EventbusPublishEvent evtpub;

    if (!evtpub.ParseFromString(in_content)) {
        _MSG_ERROR("Kismet external interface got unparseable EVENTBUSPUBLISH");
        trigger_error("Invalid EVENTBUSPUBLISH");
        return;
    }

    auto evt = eventbus->get_eventbus_event(evtpub.event_type());
    evt->get_event_content()->insert("kismet.eventbus.event_json",
            std::make_shared<tracker_element_string>(evtpub.event_content_json()));
    eventbus->publish(evt);
}

void kis_external_interface::handle_packet_http_register(uint32_t in_seqno, 
        const std::string& in_content) {
    local_locker lock(&ext_mutex, "kei::handle_packet_http_register");

    KismetExternalHttp::HttpRegisterUri uri;

    if (!uri.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparsable HTTPREGISTERURI", MSGFLAG_ERROR);
        trigger_error("Invalid HTTPREGISTERURI");
        return;
    }

    auto httpd = Globalreg::fetch_mandatory_global_as<kis_net_beast_httpd>();

    httpd->register_route(uri.uri(), {uri.method()}, httpd->LOGON_ROLE,
            std::make_shared<kis_net_web_function_endpoint>(
                [this](std::shared_ptr<kis_net_beast_httpd_connection> con) {

                    local_demand_locker l(&ext_mutex, fmt::format("proxied req {}", con->uri()));
                    l.lock();

                    auto session = std::make_shared<kis_external_http_session>();
                    session->connection = con;
                    session->locker.reset(new conditional_locker<int>());
                    session->locker->lock();

                    auto sess_id = http_session_id++;
                    http_proxy_session_map[sess_id] = session;

                    auto var_remap = std::map<std::string, std::string>();
                    for (const auto& v : con->http_variables())
                        var_remap[v.first] = v.second;

                    send_http_request(sess_id, static_cast<std::string>(con->uri()), 
                            fmt::format("{}", con->verb()), var_remap);

                    con->set_closure_cb([session]() { session->locker->unlock(-1); });

                    // Unlock the external mutex prior to blocking
                    l.unlock();

                    // Block until we get a response
                    session->locker->block_until();

                    // Reacquire the lock on the external interface
                    l.lock();

                    auto mi = http_proxy_session_map.find(sess_id);
                    if (mi != http_proxy_session_map.end())
                        http_proxy_session_map.erase(mi);
            }));
}

void kis_external_interface::handle_packet_http_response(uint32_t in_seqno, 
        const std::string& in_content) {
    local_locker lock(&ext_mutex, "kei::handle_packet_http_response");

    KismetExternalHttp::HttpResponse resp;

    if (!resp.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparsable HTTPRESPONSE", MSGFLAG_ERROR);
        trigger_error("Invalid  HTTPRESPONSE");
        return;
    }

    auto si = http_proxy_session_map.find(resp.req_id());

    if (si == http_proxy_session_map.end()) {
        _MSG("Kismet external interface got a HTTPRESPONSE for an unknown session", MSGFLAG_ERROR);
        trigger_error("Invalid HTTPRESPONSE session");
        return;
    }

    auto session = si->second;

    // First off, process any headers we're trying to add, they need to come 
    // before data
    try {
        for (int hi = 0; hi < resp.header_content_size() && resp.header_content_size() > 0; hi++) {
            KismetExternalHttp::SubHttpHeader hh = resp.header_content(hi);
            session->connection->append_header(hh.header(), hh.content());
        }
    } catch (const std::runtime_error& e) {
        _MSG_ERROR("Kismet external interface failed setting HTTPRESPONSE headers - {}", e.what());
        trigger_error("Invalid HTTPRESPONSE header block");
        return;
    }

    // Set any connection state
    try {
        if (resp.has_resultcode()) {
            session->connection->set_status(resp.resultcode());
        }
    } catch (const std::runtime_error& e) {
        _MSG_ERROR("Kismet external interface failed setting HTTPRESPONSE status code- {}", e.what());
        trigger_error("invalid HTTPRESPONSE status code");
        return;
    }

    // Copy any response data
    if (resp.has_content() && resp.content().size() > 0) {
        session->connection->response_stream().put_data(resp.content().data(), resp.content().size());
    }

    // Are we finishing the connection?
    if (resp.has_close_response() && resp.close_response()) {
        session->connection->response_stream().complete();
        session->locker->unlock();
    }
}

void kis_external_interface::handle_packet_http_auth_request(uint32_t in_seqno, 
        const std::string& in_content) {
    KismetExternalHttp::HttpAuthTokenRequest rt;

    if (!rt.ParseFromString(in_content)) {
        _MSG("Kismet external interface got an unparsable HTTPAUTHREQ", MSGFLAG_ERROR);
        trigger_error("Invalid HTTPAUTHREQ");
        return;
    }

    auto httpd = Globalreg::fetch_mandatory_global_as<kis_net_beast_httpd>();
    auto token = httpd->create_auth("external", httpd->LOGON_ROLE, 0);

    send_http_auth(token);
}

unsigned int kis_external_interface::send_http_request(uint32_t in_http_sequence, std::string in_uri,
        std::string in_method, std::map<std::string, std::string> in_vardata) {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_command("HTTPREQUEST");

    KismetExternalHttp::HttpRequest r;
    r.set_req_id(in_http_sequence);
    r.set_uri(in_uri);
    r.set_method(in_method);

    for (auto pi : in_vardata) {
        KismetExternalHttp::SubHttpVariableData *pd = r.add_variable_data();
        pd->set_field(pi.first);
        pd->set_content(pi.second);
    }

    c->set_content(r.SerializeAsString());

    return send_packet(c);
}

unsigned int kis_external_interface::send_http_auth(std::string in_cookie) {
    std::shared_ptr<KismetExternal::Command> c(new KismetExternal::Command());

    c->set_command("HTTPAUTH");

    KismetExternalHttp::HttpAuthToken a;
    a.set_token(in_cookie);

    c->set_content(a.SerializeAsString());

    return send_packet(c);
}

