#include <asio.hpp>
#include <cpptrace/cpptrace.hpp>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <unistd.h>

#include "mesh_engine.hpp"
#include "command_line_parser.hpp"
#include "settings_manager.hpp"
#include "log.hpp"

using asio::ip::tcp;
constexpr std::chrono::seconds kDefaultWatchInterval{60};

std::string get_external_ip(asio::io_context& io, Logger& logger) {
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
        logger.warn("get_external_ip() failed: {}", e.what());
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
    MeshEngine::Options options;
    options.workspace_root = std::filesystem::current_path();
    options.start_cli_thread = true;
    options.watch_interval = kDefaultWatchInterval;

    MeshEngine engine(nullptr, options);
    auto settings = engine.settings();
    settings->set_settings_path(options.workspace_root / ".config" / "settings.json");
    settings->load();

    CommandLineParser parser((argc > 0 && argv && argv[0]) ? argv[0] : "sync");
    parser.parse(argc, argv, *settings);
    if(settings->help_requested()) {
      parser.usage();
      return 0;
    }

    auto logger = engine.logger();
    if(settings->get<bool>("verbose")) {
      logger->debug("Verbose logging enabled");
    }
    engine.set_display_name(get_unique_display_name());

    if(settings->save_requested()) {
      if(!settings->save()) {
        logger->error("Unable to persist settings to {}", settings->settings_path().string());
      }
    }

    std::string external_ip;
    try {
      asio::io_context ext_io;
      external_ip = get_external_ip(ext_io, *logger);
      logger->print("External IP: {}", external_ip);
    } catch(...) {
      logger->warn("Unable to detect external IP, will only advertise local_addr");
    }

    int listen_port_value = settings->get<int>("listen_port");
    if(listen_port_value <= 0 || listen_port_value > 65535) {
      logger->error("Invalid listen_port '{}'", listen_port_value);
      return 1;
    }
    unsigned short listen_port = static_cast<unsigned short>(listen_port_value);

    int external_port_value = settings->get<int>("external_port");
    unsigned short external_port = listen_port;
    if(external_port_value > 0 && external_port_value <= 65535) {
      external_port = static_cast<unsigned short>(external_port_value);
    }

    if(!external_ip.empty()) {
      engine.set_external_address(external_ip + ":" + std::to_string(external_port));
    }

    engine.start();
    engine.run();
    engine.stop();

    return 0;
  } catch(std::exception& e) {
    init(false);
    Logger logger("sync-main");
    logger.error("Exception: {}", e.what());
    cpptrace::generate_trace().print();
    return 1;
  }
}
