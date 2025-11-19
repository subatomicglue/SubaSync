#include <asio.hpp>
#include <iostream>
#include <cpptrace/cpptrace.hpp>
#include <sstream>
#include <chrono>

#include "connection.hpp"
#include "peer_manager.hpp"
#include "protocol.hpp"
#include "MeshCLI.hpp"
#include "settings_manager.hpp"
#include "log.hpp"

using asio::ip::tcp;
constexpr std::chrono::seconds kDefaultWatchInterval{60};

std::string make_local_addr(const std::string& ip, unsigned short port){
    return ip + ":" + std::to_string(port);
}

std::string random_peer_id(){
    // simple random id — replace with something stable (uuid/hostname+pid) in production
    static std::atomic<int> s{1};
    return "peer-" + std::to_string(s.fetch_add(1));
}

std::string get_external_ip(asio::io_context& io) {
    using asio::ip::tcp;
    try {
        tcp::resolver resolver(io);
        tcp::socket socket(io);

        auto endpoints = resolver.resolve("api.ipify.org", "80");
        asio::connect(socket, endpoints);

        std::string request =
            "GET /?format=text HTTP/1.0\r\nHost: api.ipify.org\r\n\r\n";
        asio::write(socket, asio::buffer(request));

        std::string response;
        char buf[512];
        for (;;) {
            std::error_code ec;
            size_t n = socket.read_some(asio::buffer(buf), ec);
            if (ec == asio::error::eof) break;         // normal end of stream
            if (ec) throw std::system_error(ec);
            response.append(buf, n);
        }

        auto pos = response.find("\r\n\r\n");
        if (pos == std::string::npos) return "";

        std::string body = response.substr(pos + 4);
        // Trim CRLF
        while (!body.empty() &&
               (body.back() == '\r' || body.back() == '\n'))
            body.pop_back();

        return body;
    } catch (const std::exception& e) {
        log_warn("get_external_ip() failed: {}", e.what());
        return "";
    }
}

std::string get_unique_display_name() {
    char hostname[256];
    if(gethostname(hostname, sizeof(hostname)) != 0) {
        strcpy(hostname, "UnknownHost");
    }
    std::stringstream ss;
    ss << hostname << "-" << getpid();  // append process id
    return ss.str();
}

int main(int argc, char** argv){
  try {
    auto settings = std::make_shared<SettingsManager>();
    settings->init(argc, argv);

    init(settings->get<bool>("verbose"));
    if(settings->get<bool>("verbose")) {
      log_debug("Verbose logging enabled");
    }

    if(settings->save_requested()) {
      if(!settings->save()) {
        log_error("Unable to persist settings to {}", settings->settings_path().string());
      }
    }

    asio::io_context io;

    // Setup signal handling
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code&, int){
        log_info("Signal received, stopping io_context");
        io.stop();
    });

    std::string external_ip;
    try {
      external_ip = get_external_ip(io);
      print_out("External IP: {}", external_ip);
    } catch(...) {
      log_warn("Unable to detect external IP, will only advertise local_addr");
    }

    std::string display_name = get_unique_display_name();

    int listen_port_value = settings->get<int>("listen_port");
    if(listen_port_value <= 0 || listen_port_value > 65535) {
      log_error("Invalid listen_port '{}'", listen_port_value);
      settings->usage();
      return 1;
    }
    unsigned short listen_port = static_cast<unsigned short>(listen_port_value);

    std::string listen_ip = settings->get<std::string>("listen_ip");
    std::string bootstrap = settings->get<std::string>("bootstrap_peer");

    std::string local_peer_id = settings->get<std::string>("peer_id");
    if(local_peer_id.empty()) {
      local_peer_id = random_peer_id();
    }

    int external_port_value = settings->get<int>("external_port");
    unsigned short external_port = listen_port;
    if(external_port_value > 0 && external_port_value <= 65535) {
      external_port = static_cast<unsigned short>(external_port_value);
    }

    std::string external_addr = external_ip.empty() ? "" : external_ip + ":" + std::to_string(external_port);
    std::string local_addr = make_local_addr(listen_ip, listen_port);

    auto pm = std::make_shared<PeerManager>(io, local_peer_id, display_name,
                                      local_addr,
                                      external_addr,
                                      6);
    pm->set_transfer_debug(settings->get<bool>("transfer_debug"));

    // Start acceptor
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address(listen_ip), listen_port));
    log_info("Display Name: {}", display_name);
    log_info("Listening {}:{} as {}", listen_ip, listen_port, local_peer_id);

    std::function<void()> do_accept;
    do_accept = [&]{
        acceptor.async_accept([&](std::error_code ec, tcp::socket sock){
            if(ec){
                log_error("Accept error: {}", ec.message());
            } else {
                log_info("Accepted connection from {}", sock.remote_endpoint().address().to_string());
                auto conn = Connection::create_incoming(io, std::move(sock), pm);
                // Connection will call pm->on_connected when it learns peer_id from announce
            }
            do_accept();
        });
    };
    do_accept();

    // Optionally connect to a bootstrap peer
    if(!bootstrap.empty()) {
      auto pos = bootstrap.find(':');
      if(pos != std::string::npos) {
        std::string host = bootstrap.substr(0, pos);
        try {
          unsigned short p = static_cast<unsigned short>(std::stoi(bootstrap.substr(pos + 1)));
          Connection::connect_outgoing(io, host, p, pm);
        } catch(const std::exception& e) {
          log_error("Invalid bootstrap specification '{}': {}", bootstrap, e.what());
          settings->usage();
          return 1;
        }
      } else {
        log_error("Bootstrap peer must be in host:port format: '{}'", bootstrap);
        settings->usage();
        return 1;
      }
    }

    // Small periodic timer to attempt connect more (in case we learned peers later)
    asio::steady_timer timer(io, std::chrono::seconds(2));
    std::function<void(const std::error_code&)> tick;
    tick = [&](const std::error_code&){
        pm->attempt_connect_more();
        timer.expires_after(std::chrono::seconds(2));
        timer.async_wait(tick);
    };
    timer.async_wait(tick);

    // Start CLI thread
    MeshCLI cli(pm, settings, kDefaultWatchInterval, settings->get<bool>("audio_notifications"));
    cli.start();

    io.run();
    cli.stop();

    return 0;
  } catch(std::exception& e) {
    log_error("Exception: {}", e.what());
    cpptrace::generate_trace().print();
    return 1;
  }
}
