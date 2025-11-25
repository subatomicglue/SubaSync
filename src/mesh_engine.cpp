#include "mesh_engine.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <stdexcept>

#include "MeshCLI.hpp"
#include "connection.hpp"
#include "log.hpp"
#include "peer_manager.hpp"
#include "settings_manager.hpp"
#include "utils.hpp"

namespace {

std::string make_local_addr(const std::string& ip, uint16_t port) {
  return ip + ":" + std::to_string(port);
}

std::string random_peer_id() {
  static std::atomic<uint64_t> next_id{1};
  auto value = next_id.fetch_add(1);
  return "engine-peer-" + std::to_string(value);
}

} // namespace

MeshEngine::MeshEngine(std::shared_ptr<SettingsManager> settings, Options options)
  : options_(std::move(options)),
    settings_(settings ? std::move(settings) : std::make_shared<SettingsManager>()),
    logger_(std::make_shared<Logger>("mesh-engine")) {
  if(options_.watch_interval.count() <= 0) {
    options_.watch_interval = std::chrono::seconds(60);
  }
  if(options_.workspace_root.empty()) {
    options_.workspace_root = std::filesystem::current_path();
  }
}

MeshEngine::~MeshEngine() {
  stop();
}

void MeshEngine::ensure_workspace() const {
  std::error_code ec;
  std::filesystem::create_directories(options_.workspace_root, ec);
}

void MeshEngine::start() {
  if(started_) return;
  started_ = true;

  ensure_workspace();

  if(settings_->settings_path().empty()) {
    settings_->set_settings_path(options_.workspace_root / ".config" / "settings.json");
  }

  init(settings_->get<bool>("verbose"));

  peer_id_ = settings_->get<std::string>("peer_id");
  if(peer_id_.empty()) {
    peer_id_ = random_peer_id();
  }
  if(logger_) {
    logger_->set_name(peer_id_);
  } else {
    logger_ = std::make_shared<Logger>(peer_id_);
  }
  listen_ip_ = settings_->get<std::string>("listen_ip");
  int listen_port_value = settings_->get<int>("listen_port");
  if(listen_port_value < 0 || listen_port_value > 65535) {
    logger_->error("Invalid listen_port '{}'", listen_port_value);
    throw std::runtime_error("Invalid listen_port");
  }
  listen_port_ = static_cast<uint16_t>(listen_port_value);

  bootstrap_peer_ = settings_->get<std::string>("bootstrap_peer");
  std::string display_name = options_.display_name.empty() ? peer_id_ : options_.display_name;

  bool audio_notifications = settings_->get<bool>("audio_notifications");
  bool transfer_debug = settings_->get<bool>("transfer_debug");

  using tcp = asio::ip::tcp;
  asio::ip::address listen_address;
  try {
    listen_address = asio::ip::make_address(listen_ip_);
  } catch(const std::exception& e) {
    logger_->error("Invalid listen_ip '{}': {}", listen_ip_, e.what());
    throw;
  }

  acceptor_ = std::make_unique<tcp::acceptor>(io_);
  tcp::endpoint endpoint(listen_address, listen_port_);
  acceptor_->open(endpoint.protocol());
  acceptor_->set_option(tcp::acceptor::reuse_address(true));
  acceptor_->bind(endpoint);
  acceptor_->listen();

  if(listen_port_ == 0) {
    listen_port_ = acceptor_->local_endpoint().port();
  }
  local_address_ = make_local_addr(listen_ip_, listen_port_);

  std::string external_addr = options_.external_address;

  peer_manager_ = std::make_shared<PeerManager>(io_,
                                                peer_id_,
                                                display_name,
                                                local_address_,
                                                external_addr,
                                                6,
                                                logger_);
  peer_manager_->set_transfer_debug(transfer_debug);

  start_accept();

  if(options_.enable_connect_timer) {
    connect_timer_ = std::make_unique<asio::steady_timer>(io_);
    schedule_connect_tick();
  }

  if(!bootstrap_peer_.empty()) {
    auto pos = bootstrap_peer_.find(':');
    if(pos != std::string::npos) {
      auto host = bootstrap_peer_.substr(0, pos);
      try {
        uint16_t port = static_cast<uint16_t>(std::stoi(bootstrap_peer_.substr(pos + 1)));
        Connection::connect_outgoing(io_, host, port, peer_manager_);
      } catch(const std::exception& e) {
        logger_->error("Invalid bootstrap peer {}: {}", bootstrap_peer_, e.what());
      }
    } else {
      logger_->error("Bootstrap peer must be host:port (got '{}')", bootstrap_peer_);
    }
  }

  cli_ = std::make_unique<MeshCLI>(peer_manager_,
                                   settings_,
                                   logger_,
                                   options_.watch_interval,
                                   audio_notifications,
                                   options_.workspace_root);
  if(options_.start_cli_thread) {
    start_cli();
  }
}

void MeshEngine::start_accept() {
  if(!acceptor_) return;
  acceptor_->async_accept(
    [this](std::error_code ec, tcp::socket socket){
      if(ec) {
        logger_->error("Accept error: {}", ec.message());
      } else if(peer_manager_) {
        logger_->info("Accepted connection from {}", socket.remote_endpoint().address().to_string());
        Connection::create_incoming(io_, std::move(socket), peer_manager_);
      }
      if(started_) {
        start_accept();
      }
    });
}

void MeshEngine::schedule_connect_tick() {
  if(!connect_timer_) return;
  connect_timer_->expires_after(std::chrono::seconds(2));
  connect_timer_->async_wait([this](const std::error_code& ec){
    if(ec || !started_) return;
    if(peer_manager_) {
      peer_manager_->attempt_connect_more();
    }
    schedule_connect_tick();
  });
}

void MeshEngine::run() {
  if(!started_) start();
  io_.run();
}

void MeshEngine::start_background() {
  if(!started_) start();
  if(io_thread_.joinable()) return;
  io_thread_ = std::thread([this](){
    io_.run();
  });
}

void MeshEngine::stop() {
  if(!started_) return;
  started_ = false;

  if(cli_) {
    cli_->stop();
    cli_thread_running_ = false;
  }

  if(connect_timer_) {
    std::error_code ec;
    connect_timer_->cancel(ec);
  }
  connect_timer_.reset();
  if(acceptor_) {
    std::error_code ec;
    acceptor_->close(ec);
  }
  acceptor_.reset();

  io_.stop();
  if(io_thread_.joinable()) {
    io_thread_.join();
  }
  io_.restart();
}

LogListenerHandle MeshEngine::add_log_listener(Logger::Listener listener, void* user_data) {
  if(!logger_) return 0;
  return logger_->add_listener(std::move(listener), user_data);
}

void MeshEngine::remove_log_listener(LogListenerHandle handle) {
  if(logger_ && handle != 0) {
    logger_->remove_listener(handle);
  }
}

void MeshEngine::clear_log_listeners() {
  if(logger_) {
    logger_->clear_listeners();
  }
}

void MeshEngine::set_display_name(const std::string& name) {
  options_.display_name = name;
}

void MeshEngine::set_external_address(const std::string& address) {
  options_.external_address = address;
}

void MeshEngine::start_cli() {
  if(!cli_ || cli_thread_running_) return;
  cli_->start();
  cli_thread_running_ = true;
}

void MeshEngine::execute_command(const std::string& line) {
  if(cli_) {
    cli_->execute_command(line);
  }
}

MeshEngine::Stats MeshEngine::stats() const {
  Stats s;
  if(peer_manager_) {
    s.known_peers = peer_manager_->known_peer_count();
    s.connected_peers = peer_manager_->connected_peer_count();
  }
  return s;
}
