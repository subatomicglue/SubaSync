#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

using LogListenerHandle = std::size_t;
using LogListener = std::function<void(const std::string& channel,
                                       spdlog::level::level_enum level,
                                       const std::string& message)>;

void init(bool verbose = false);

spdlog::logger& info_logger();
spdlog::logger& error_logger();
spdlog::logger& print_logger();
spdlog::logger& print_error_logger();
LogListenerHandle add_log_listener(LogListener listener);
void remove_log_listener(LogListenerHandle handle);
void clear_log_listeners();
void set_log_passthrough(bool enabled);
bool log_passthrough();

namespace detail {
bool has_log_listeners();
void dispatch_log_message(const std::string& channel,
                          spdlog::level::level_enum level,
                          const std::string& message);

template<typename... Args>
inline void log_with_listener(spdlog::logger& logger,
                              const char* channel,
                              spdlog::level::level_enum level,
                              spdlog::format_string_t<Args...> fmt,
                              Args&&... args) {
  auto formatted = fmt::format(fmt, std::forward<Args>(args)...);
  if(log_passthrough()) {
    logger.log(level, formatted);
  }
  if(has_log_listeners()) {
    dispatch_log_message(channel, level, formatted);
  }
}
} // namespace detail

template<typename... Args>
inline void log_info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  detail::log_with_listener(info_logger(), "info", spdlog::level::info,
                            fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  detail::log_with_listener(info_logger(), "warn", spdlog::level::warn,
                            fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  detail::log_with_listener(error_logger(), "error", spdlog::level::err,
                            fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  detail::log_with_listener(info_logger(), "debug", spdlog::level::debug,
                            fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  detail::log_with_listener(info_logger(), "info", spdlog::level::info,
                            fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void print_out(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  detail::log_with_listener(print_logger(), "print", spdlog::level::info,
                            fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void print_err(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  detail::log_with_listener(print_error_logger(), "print_err", spdlog::level::err,
                            fmt, std::forward<Args>(args)...);
}

class EngineLogger {
public:
  explicit EngineLogger(std::string name);

  template<typename... Args>
  void info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log(*info_logger_, "info", spdlog::level::info, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log(*info_logger_, "warn", spdlog::level::warn, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log(*error_logger_, "error", spdlog::level::err, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log(*info_logger_, "debug", spdlog::level::debug, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void print(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log(*print_logger_, "print", spdlog::level::info, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void print_err(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log(*print_error_logger_, "print_err", spdlog::level::err, fmt, std::forward<Args>(args)...);
  }

  const std::string& name() const { return name_; }

private:
  template<typename... Args>
  void log(spdlog::logger& sink,
           const char* channel,
           spdlog::level::level_enum level,
           spdlog::format_string_t<Args...> fmt,
           Args&&... args) {
    auto formatted = fmt::format(fmt, std::forward<Args>(args)...);
    if(log_passthrough()) {
      sink.log(level, formatted);
    }
    if(detail::has_log_listeners()) {
      detail::dispatch_log_message(name_ + ":" + channel, level, formatted);
    }
  }

  std::string name_;
  std::shared_ptr<spdlog::logger> info_logger_;
  std::shared_ptr<spdlog::logger> error_logger_;
  std::shared_ptr<spdlog::logger> print_logger_;
  std::shared_ptr<spdlog::logger> print_error_logger_;
};
