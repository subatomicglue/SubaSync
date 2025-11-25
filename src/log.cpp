#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
std::shared_ptr<spdlog::logger> g_info_logger;
std::shared_ptr<spdlog::logger> g_error_logger;
std::shared_ptr<spdlog::logger> g_print_logger;
std::shared_ptr<spdlog::logger> g_print_err_logger;
std::once_flag g_init_once;
std::atomic<bool> g_log_passthrough{true};

void create_loggers() {
  if(g_info_logger) return;

  auto info_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  info_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

  auto error_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  error_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

  auto plain_out_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  plain_out_sink->set_pattern("%v");

  auto plain_err_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  plain_err_sink->set_pattern("%v");

  g_info_logger = std::make_shared<spdlog::logger>("log.info", std::move(info_sink));
  g_error_logger = std::make_shared<spdlog::logger>("log.error", std::move(error_sink));
  g_print_logger = std::make_shared<spdlog::logger>("log.print", std::move(plain_out_sink));
  g_print_err_logger = std::make_shared<spdlog::logger>("log.print_err", std::move(plain_err_sink));

  spdlog::register_logger(g_info_logger);
  spdlog::register_logger(g_error_logger);
  spdlog::register_logger(g_print_logger);
  spdlog::register_logger(g_print_err_logger);

  g_info_logger->flush_on(spdlog::level::warn);
  g_error_logger->flush_on(spdlog::level::err);
  g_print_logger->flush_on(spdlog::level::info);
  g_print_err_logger->flush_on(spdlog::level::err);
}

void ensure_loggers() {
  if(!g_info_logger) create_loggers();
}

} // namespace

void set_log_passthrough(bool enabled) {
  g_log_passthrough.store(enabled, std::memory_order_release);
}

bool log_passthrough() {
  return g_log_passthrough.load(std::memory_order_acquire);
}

Logger::Logger() = default;
Logger::Logger(std::string name) : name_(std::move(name)) {}

void Logger::set_name(std::string name) {
  name_ = std::move(name);
}

LogListenerHandle Logger::add_listener(Listener listener, void* user_data) {
  if(!listener) return 0;
  std::lock_guard<std::mutex> lock(listener_mutex_);
  const auto id = next_listener_id_++;
  listeners_.emplace(id, ListenerBinding{user_data, std::move(listener)});
  return id;
}

void Logger::remove_listener(LogListenerHandle handle) {
  std::lock_guard<std::mutex> lock(listener_mutex_);
  listeners_.erase(handle);
}

void Logger::clear_listeners() {
  std::lock_guard<std::mutex> lock(listener_mutex_);
  listeners_.clear();
}

bool Logger::dispatch(const std::string& channel,
                      spdlog::level::level_enum level,
                      const std::string& message) {
  std::vector<ListenerBinding> listeners_snapshot;
  {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    listeners_snapshot.reserve(listeners_.size());
    for(const auto& entry : listeners_) {
      listeners_snapshot.push_back(entry.second);
    }
  }
  bool handled = false;
  for(auto& binding : listeners_snapshot) {
    try {
      if(binding.callback && binding.callback(binding.user_data, channel, level, message)) {
        handled = true;
      }
    } catch(...) {
      // swallow listener exceptions
    }
  }
  return handled;
}

void Logger::fallback(const char* base_channel,
                      const std::string& channel_name,
                      spdlog::level::level_enum level,
                      const std::string& message) {
  detail::emit_to_default(base_channel, channel_name, level, message);
}

void init(bool verbose) {
  std::call_once(g_init_once, [](){
    create_loggers();
  });

  ensure_loggers();
  auto level = verbose ? spdlog::level::debug : spdlog::level::info;
  g_info_logger->set_level(level);
  g_error_logger->set_level(spdlog::level::info);
  g_print_logger->set_level(spdlog::level::info);
  g_print_err_logger->set_level(spdlog::level::info);

  spdlog::set_default_logger(g_info_logger);
  spdlog::set_level(level);
}

namespace detail {

void emit_to_default(const char* base_channel,
                     const std::string& channel_name,
                     spdlog::level::level_enum level,
                     const std::string& message) {
  ensure_loggers();
  if(!log_passthrough()) return;

  spdlog::logger* sink = nullptr;
  if(std::strcmp(base_channel, "print") == 0) {
    sink = g_print_logger.get();
  } else if(std::strcmp(base_channel, "print_err") == 0) {
    sink = g_print_err_logger.get();
  } else if(std::strcmp(base_channel, "error") == 0) {
    sink = g_error_logger.get();
  } else {
    sink = g_info_logger.get();
  }

  if(!sink) return;
  if(!channel_name.empty() && channel_name != base_channel) {
    sink->log(level, fmt::format("[{}] {}", channel_name, message));
  } else {
    sink->log(level, message);
  }
}

} // namespace detail
