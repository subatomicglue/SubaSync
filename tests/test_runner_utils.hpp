#pragma once

#include "log.hpp"
#include "mesh_engine.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mesh::test {

inline void write_config_before_start(const std::filesystem::path& workspace,
                                      const std::string& filename,
                                      const nlohmann::json& content) {
  auto config_dir = workspace / ".config";
  std::error_code ec;
  std::filesystem::create_directories(config_dir, ec);
  std::ofstream out(config_dir / filename, std::ios::trunc);
  if(out) {
    out << content.dump(2);
  }
}

class LogCapture {
public:
  LogCapture() = default;

  ~LogCapture() {
    detach_all();
  }

  void attach(const std::shared_ptr<Logger>& logger,
              const std::string& label = std::string()) {
    if(!logger) return;
    auto handle = logger->add_listener(
      [this, label](void*,
                    const std::string& channel,
                    spdlog::level::level_enum,
                    const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!label.empty()) {
          lines_.emplace_back(label + ": " + message);
        } else {
          lines_.emplace_back(channel + ": " + message);
        }
        cv_.notify_all();
        return false;
      },
      nullptr);
    std::lock_guard<std::mutex> lock(attachments_mutex_);
    attachments_.push_back({logger, nullptr, handle});
  }

  void attach(MeshEngine& engine, const std::string& label = std::string()) {
    auto handle = engine.add_log_listener(
      [this, label](void*,
                    const std::string& channel,
                    spdlog::level::level_enum,
                    const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!label.empty()) {
          lines_.emplace_back(label + ": " + message);
        } else {
          lines_.emplace_back(channel + ": " + message);
        }
        cv_.notify_all();
        return false;
      },
      nullptr);
    std::lock_guard<std::mutex> lock(attachments_mutex_);
    attachments_.push_back({nullptr, &engine, handle});
  }

  void detach_all() {
    std::vector<Attachment> pending;
    {
      std::lock_guard<std::mutex> lock(attachments_mutex_);
      pending.swap(attachments_);
    }
    for(auto& attachment : pending) {
      if(attachment.logger && attachment.handle != 0) {
        attachment.logger->remove_listener(attachment.handle);
      }
      if(attachment.engine && attachment.handle != 0) {
        attachment.engine->remove_log_listener(attachment.handle);
      }
    }
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
  }

  std::vector<std::string> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }

  bool wait_for_substring(const std::string& needle,
                          std::chrono::milliseconds timeout) {
    auto predicate = [&]{
      return std::any_of(lines_.begin(), lines_.end(),
        [&](const std::string& line){ return line.find(needle) != std::string::npos; });
    };
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while(!predicate()) {
      if(cv_.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
    return predicate();
  }

private:
  struct Attachment {
    std::shared_ptr<Logger> logger;
    MeshEngine* engine = nullptr;
    LogListenerHandle handle = 0;
  };

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> lines_;
  std::mutex attachments_mutex_;
  std::vector<Attachment> attachments_;
};

inline bool wait_for_condition(std::function<bool()> predicate,
                               std::chrono::milliseconds timeout,
                               std::chrono::milliseconds interval = std::chrono::milliseconds(50)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while(std::chrono::steady_clock::now() < deadline) {
    if(predicate()) return true;
    std::this_thread::sleep_for(interval);
  }
  return predicate();
}

} // namespace mesh::test
