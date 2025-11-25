#include "settings_manager.hpp"
#include "mesh_engine.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
  namespace fs = std::filesystem;

  auto base = fs::temp_directory_path() / "mesh_engine_sample";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / "peerA", ec);
  fs::create_directories(base / "peerB", ec);

  auto settings_a = std::make_shared<SettingsManager>();
  settings_a->set_settings_path(base / "peerA" / ".config" / "settings.json");
  auto configure = [](const std::shared_ptr<SettingsManager>& settings,
                      const std::string& key,
                      const nlohmann::json& value){
    std::string error;
    if(!settings->set_from_json(key, value, error)) {
      throw std::runtime_error("Failed to set setting " + key + ": " + error);
    }
  };
  configure(settings_a, "listen_ip", "127.0.0.1");
  configure(settings_a, "listen_port", 0);
  configure(settings_a, "peer_id", "sample-peerA");
  configure(settings_a, "audio_notifications", false);
  configure(settings_a, "transfer_debug", false);
  MeshEngine::Options peer_a;
  peer_a.display_name = "Sample Peer A";
  peer_a.workspace_root = base / "peerA";
  peer_a.start_cli_thread = false;

  MeshEngine engine_a(settings_a, peer_a);
  engine_a.start();
  engine_a.start_background();

  auto settings_b = std::make_shared<SettingsManager>();
  settings_b->set_settings_path(base / "peerB" / ".config" / "settings.json");
  configure(settings_b, "listen_ip", "127.0.0.1");
  configure(settings_b, "listen_port", 0);
  configure(settings_b, "peer_id", "sample-peerB");
  configure(settings_b, "audio_notifications", false);
  configure(settings_b, "transfer_debug", false);
  configure(settings_b, "bootstrap_peer", "127.0.0.1:" + std::to_string(engine_a.listen_port()));
  MeshEngine::Options peer_b;
  peer_b.display_name = "Sample Peer B";
  peer_b.workspace_root = base / "peerB";
  peer_b.start_cli_thread = false;

  MeshEngine engine_b(settings_b, peer_b);
  engine_b.start();
  engine_b.start_background();

  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  engine_a.execute_command("settings");
  engine_b.execute_command("settings");

  engine_b.stop();
  engine_a.stop();

  fs::remove_all(base, ec);
  return 0;
}
