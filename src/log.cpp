#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
std::shared_ptr<spdlog::logger> g_info_logger;
std::shared_ptr<spdlog::logger> g_error_logger;
std::shared_ptr<spdlog::logger> g_print_logger;
std::shared_ptr<spdlog::logger> g_print_err_logger;
std::once_flag g_init_once;
std::mutex g_listener_mutex;
std::unordered_map<LogListenerHandle, LogListener> g_log_listeners;
std::atomic<LogListenerHandle> g_next_listener_id{1};
std::atomic<bool> g_has_log_listeners{false};
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

LogListenerHandle add_log_listener(LogListener listener) {
  if(!listener) return 0;
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  LogListenerHandle id = g_next_listener_id++;
  g_log_listeners.emplace(id, std::move(listener));
  g_has_log_listeners.store(!g_log_listeners.empty(), std::memory_order_release);
  return id;
}

void remove_log_listener(LogListenerHandle handle) {
  if(handle == 0) return;
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  g_log_listeners.erase(handle);
  g_has_log_listeners.store(!g_log_listeners.empty(), std::memory_order_release);
}

void clear_log_listeners() {
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  g_log_listeners.clear();
  g_has_log_listeners.store(false, std::memory_order_release);
}

namespace detail {

bool has_log_listeners() {
  return g_has_log_listeners.load(std::memory_order_acquire);
}

} // namespace detail

void set_log_passthrough(bool enabled) {
  g_log_passthrough.store(enabled, std::memory_order_release);
}

bool log_passthrough() {
  return g_log_passthrough.load(std::memory_order_acquire);
}

namespace detail {

void dispatch_log_message(const std::string& channel,
                          spdlog::level::level_enum level,
                          const std::string& message) {
  std::vector<LogListener> listeners;
  {
    std::lock_guard<std::mutex> lock(g_listener_mutex);
    for(const auto& entry : g_log_listeners) {
      if(entry.second) listeners.push_back(entry.second);
    }
  }
  for(auto& listener : listeners) {
    try {
      listener(channel, level, message);
    } catch(...) {
      // swallow exceptions from listeners
    }
  }
}

} // namespace detail

EngineLogger::EngineLogger(std::string name)
  : name_(std::move(name)) {
  ensure_loggers();

  auto info_sinks = g_info_logger->sinks();
  info_logger_ = std::make_shared<spdlog::logger>("engine.info." + name_, info_sinks.begin(), info_sinks.end());
  info_logger_->set_level(g_info_logger->level());
  info_logger_->flush_on(spdlog::level::warn);

  auto error_sinks = g_error_logger->sinks();
  error_logger_ = std::make_shared<spdlog::logger>("engine.error." + name_, error_sinks.begin(), error_sinks.end());
  error_logger_->set_level(g_error_logger->level());
  error_logger_->flush_on(spdlog::level::err);

  auto print_sinks = g_print_logger->sinks();
  print_logger_ = std::make_shared<spdlog::logger>("engine.print." + name_, print_sinks.begin(), print_sinks.end());
  print_logger_->set_level(g_print_logger->level());
  print_logger_->flush_on(spdlog::level::info);

  auto print_err_sinks = g_print_err_logger->sinks();
  print_error_logger_ = std::make_shared<spdlog::logger>("engine.print_err." + name_, print_err_sinks.begin(), print_err_sinks.end());
  print_error_logger_->set_level(g_print_err_logger->level());
  print_error_logger_->flush_on(spdlog::level::err);
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

spdlog::logger& info_logger() {
  ensure_loggers();
  return *g_info_logger;
}

spdlog::logger& error_logger() {
  ensure_loggers();
  return *g_error_logger;
}

spdlog::logger& print_logger() {
  ensure_loggers();
  return *g_print_logger;
}

spdlog::logger& print_error_logger() {
  ensure_loggers();
  return *g_print_err_logger;
}
