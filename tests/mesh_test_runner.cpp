#include "settings_manager.hpp"
#include "mesh_engine.hpp"
#include "test_runner_utils.hpp"
#include "log.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

struct RunnerConfig {
  std::filesystem::path root;
  std::filesystem::path peer_a_root;
  std::filesystem::path peer_b_root;
};

RunnerConfig prepare_workspace() {
  RunnerConfig cfg;
  cfg.root = std::filesystem::temp_directory_path() / "mesh_test_runner";
  cfg.peer_a_root = cfg.root / "peerA";
  cfg.peer_b_root = cfg.root / "peerB";
  std::error_code ec;
  std::filesystem::remove_all(cfg.root, ec);
  std::filesystem::create_directories(cfg.peer_a_root, ec);
  std::filesystem::create_directories(cfg.peer_b_root, ec);
  return cfg;
}

bool wait_for_connection(MeshEngine& a, MeshEngine& b) {
  using namespace std::chrono_literals;
  return mesh::test::wait_for_condition([&]{
    auto stats_a = a.stats();
    auto stats_b = b.stats();
    return stats_a.connected_peers >= 1 && stats_b.connected_peers >= 1;
  }, 5s);
}

struct TestContext {
  mesh::test::LogCapture& logs;
  bool verbose = false;
};

bool wait_for_connection(MeshEngine& a, MeshEngine& b, bool verbose) {
  using namespace std::chrono_literals;
  const auto timeout = 5s;
  auto start = std::chrono::steady_clock::now();
  auto next_report = start;

  while(std::chrono::steady_clock::now() - start < timeout) {
    auto stats_a = a.stats();
    auto stats_b = b.stats();
    if(stats_a.connected_peers >= 1 && stats_b.connected_peers >= 1) {
      return true;
    }
    if(verbose && std::chrono::steady_clock::now() >= next_report) {
      std::cout << "    waiting... "
                << "A(known=" << stats_a.known_peers
                << ", connected=" << stats_a.connected_peers << ") "
                << "B(known=" << stats_b.known_peers
                << ", connected=" << stats_b.connected_peers << ")\n";
      next_report = std::chrono::steady_clock::now() + 500ms;
    }
    std::this_thread::sleep_for(50ms);
  }

  if(verbose) {
    auto stats_a = a.stats();
    auto stats_b = b.stats();
    std::cout << "    timeout waiting for connection. "
              << "A(known=" << stats_a.known_peers
              << ", connected=" << stats_a.connected_peers << ") "
              << "B(known=" << stats_b.known_peers
              << ", connected=" << stats_b.connected_peers << ")\n";
  }
  return false;
}

bool test_basic_connection(TestContext& ctx) {
  auto cfg = prepare_workspace();
  auto configure = [](const std::shared_ptr<SettingsManager>& settings,
                      const std::string& key,
                      const nlohmann::json& value){
    std::string error;
    if(!settings->set_from_json(key, value, error)) {
      throw std::runtime_error("Failed to set setting " + key + ": " + error);
    }
  };

  auto settings_a = std::make_shared<SettingsManager>();
  settings_a->set_settings_path(cfg.peer_a_root / ".config" / "settings.json");
  configure(settings_a, "listen_ip", "127.0.0.1");
  configure(settings_a, "listen_port", 0);
  configure(settings_a, "peer_id", "runner-peerA");
  configure(settings_a, "audio_notifications", false);
  configure(settings_a, "transfer_debug", true);

  MeshEngine::Options opt_a;
  opt_a.display_name = "runner-peerA";
  opt_a.workspace_root = cfg.peer_a_root;
  opt_a.start_cli_thread = false;

  MeshEngine engine_a(settings_a, opt_a);
  ctx.logs.attach(engine_a, opt_a.display_name);
  engine_a.start();
  engine_a.start_background();
  if(ctx.verbose) {
    std::cout << "    engine A listening on "
              << engine_a.listen_port() << "\n";
  }
  // Give peer A a brief head start so the listener is definitely up before peer B dials.
  mesh::test::wait_for_condition([&]{
    return engine_a.listen_port() != 0;
  }, std::chrono::seconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  auto settings_b = std::make_shared<SettingsManager>();
  settings_b->set_settings_path(cfg.peer_b_root / ".config" / "settings.json");
  configure(settings_b, "listen_ip", "127.0.0.1");
  configure(settings_b, "listen_port", 0);
  configure(settings_b, "peer_id", "runner-peerB");
  configure(settings_b, "audio_notifications", false);
  configure(settings_b, "transfer_debug", true);
  configure(settings_b, "bootstrap_peer", "127.0.0.1:" + std::to_string(engine_a.listen_port()));

  MeshEngine::Options opt_b;
  opt_b.display_name = "runner-peerB";
  opt_b.workspace_root = cfg.peer_b_root;
  opt_b.start_cli_thread = false;

  MeshEngine engine_b(settings_b, opt_b);
  ctx.logs.attach(engine_b, opt_b.display_name);
  engine_b.start();
  engine_b.start_background();
  if(ctx.verbose) {
    std::cout << "    engine B listening on "
              << engine_b.listen_port() << " (bootstrap -> "
              << engine_a.listen_port() << ")\n";
  }

  using namespace std::chrono_literals;
  bool ok = wait_for_connection(engine_a, engine_b, ctx.verbose);
  bool saw = ctx.logs.wait_for_substring("PeerManager: connected", 3s);

  engine_b.stop();
  engine_a.stop();
  ctx.logs.detach_all();
  std::error_code ec;
  std::filesystem::remove_all(cfg.root, ec);
  return ok && saw;
}

struct TestCase {
  const char* name;
  std::function<bool(TestContext&)> fn;
};

} // namespace

int main(int argc, char** argv) {
  bool verbose = (std::getenv("MESH_TEST_VERBOSE") != nullptr);
  for(int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if(arg == "-v" || arg == "--verbose") {
      verbose = true;
    }
  }

  bool show_logs = (std::getenv("MESH_TEST_LOGS") != nullptr) || verbose;
  const bool suppress_logs = !show_logs;
  if(suppress_logs) {
    set_log_passthrough(false);
  }
  mesh::test::LogCapture logs;
  TestContext ctx{logs, verbose};
  std::vector<TestCase> tests = {
    {"basic_connection", test_basic_connection}
  };

  std::size_t failures = 0;
  std::cout << "Running " << tests.size() << " mesh tests: " << std::flush;

  for(std::size_t idx = 0; idx < tests.size(); ++idx) {
    const auto& test = tests[idx];
    logs.clear();
    bool passed = false;
    try {
      passed = test.fn(ctx);
    } catch(const std::exception& e) {
      passed = false;
      std::cerr << "Exception in test " << test.name << ": " << e.what() << "\n";
    }
    if(passed) {
      std::cout << '.' << std::flush;
    } else {
      std::cout << 'F' << " (" << test.name << ")\n";
      failures++;
      for(const auto& line : logs.snapshot()) {
        std::cout << "    " << line << "\n";
      }
      if(idx + 1 < tests.size()) {
        std::cout << "Running " << tests.size() << " mesh tests: " << std::flush;
      }
    }
  }
  std::cout << "\n";
  if(suppress_logs) {
    set_log_passthrough(true);
  }
  if(failures == 0) {
    std::cout << "PASS (" << tests.size() << " tests)\n";
    return 0;
  }
  std::cout << "FAIL (" << failures << "/" << tests.size() << " failed)\n";
  return 1;
}
