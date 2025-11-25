#pragma once

#include <asio.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

class MeshCLI;
class PeerManager;
class SettingsManager;
class EngineLogger;

class MeshEngine {
public:
  struct Options {
    std::string display_name;
    std::string external_address;
    bool start_cli_thread = false;
    bool enable_connect_timer = true;
    std::chrono::seconds watch_interval{60};
    std::filesystem::path workspace_root = std::filesystem::current_path();
  };

  MeshEngine(std::shared_ptr<SettingsManager> settings, Options options);
  ~MeshEngine();

  void start();
  void run();
  void start_background();
  void stop();

  void execute_command(const std::string& line);

  std::shared_ptr<SettingsManager> settings() const { return settings_; }
  std::shared_ptr<EngineLogger> logger() const { return logger_; }

  struct Stats {
    std::size_t known_peers = 0;
    std::size_t connected_peers = 0;
  };

  Stats stats() const;

  uint16_t listen_port() const { return listen_port_; }
  const std::string& peer_id() const { return peer_id_; }
  const std::filesystem::path& workspace_root() const { return options_.workspace_root; }

private:
  using tcp = asio::ip::tcp;

  void start_accept();
  void schedule_connect_tick();
  void start_cli();
  void ensure_workspace() const;

  Options options_;
  std::shared_ptr<SettingsManager> settings_;
  asio::io_context io_;
  std::thread io_thread_;
  std::unique_ptr<tcp::acceptor> acceptor_;
  std::unique_ptr<asio::steady_timer> connect_timer_;
  std::shared_ptr<PeerManager> peer_manager_;
  std::unique_ptr<MeshCLI> cli_;
  bool started_ = false;
  bool cli_thread_running_ = false;
  std::string local_address_;
  std::string peer_id_;
  std::string listen_ip_;
  uint16_t listen_port_ = 0;
  std::string bootstrap_peer_;
  std::shared_ptr<EngineLogger> logger_;
};
