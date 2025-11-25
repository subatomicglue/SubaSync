#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

void init(bool verbose = false);
void set_log_passthrough(bool enabled);
bool log_passthrough();

using LogListenerHandle = std::size_t;

class Logger {
public:
  using Listener = std::function<bool(void* user_data,
                                      const std::string& channel,
                                      spdlog::level::level_enum level,
                                      const std::string& message)>;

  Logger();
  explicit Logger(std::string name);

  void set_name(std::string name);
  const std::string& name() const { return name_; }

  LogListenerHandle add_listener(Listener listener, void* user_data = nullptr);
  void remove_listener(LogListenerHandle handle);
  void clear_listeners();

  template<typename... Args>
  void info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log("info", spdlog::level::info, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log("warn", spdlog::level::warn, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log("error", spdlog::level::err, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log("debug", spdlog::level::debug, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void print(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log("print", spdlog::level::info, fmt, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void print_err(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    log("print_err", spdlog::level::err, fmt, std::forward<Args>(args)...);
  }

private:
  template<typename... Args>
  void log(const char* channel,
           spdlog::level::level_enum level,
           spdlog::format_string_t<Args...> fmt,
           Args&&... args) {
    auto formatted = fmt::format(fmt, std::forward<Args>(args)...);
    std::string channel_name = name_.empty()
      ? std::string(channel)
      : name_ + ":" + channel;
    if(dispatch(channel_name, level, formatted)) return;
    fallback(channel, channel_name, level, formatted);
  }

  bool dispatch(const std::string& channel,
                spdlog::level::level_enum level,
                const std::string& message);
  void fallback(const char* base_channel,
                const std::string& channel_name,
                spdlog::level::level_enum level,
                const std::string& message);

  struct ListenerBinding {
    void* user_data = nullptr;
    Listener callback;
  };

  std::string name_;
  std::mutex listener_mutex_;
  std::unordered_map<LogListenerHandle, ListenerBinding> listeners_;
  std::atomic<LogListenerHandle> next_listener_id_{1};
};

namespace detail {
void emit_to_default(const char* base_channel,
                     const std::string& channel_name,
                     spdlog::level::level_enum level,
                     const std::string& message);

template<typename... Args>
void log_with_fallback(const char* channel,
                       spdlog::level::level_enum level,
                       spdlog::format_string_t<Args...> fmt,
                       Args&&... args) {
  auto formatted = fmt::format(fmt, std::forward<Args>(args)...);
  emit_to_default(channel, channel, level, formatted);
}
} // namespace detail

template<typename... Args>
inline void log_info(Logger* logger,
                     spdlog::format_string_t<Args...> fmt,
                     Args&&... args) {
  if(logger) {
    logger->info(fmt, std::forward<Args>(args)...);
  } else {
    detail::log_with_fallback("info", spdlog::level::info, fmt, std::forward<Args>(args)...);
  }
}

template<typename... Args>
inline void log_warn(Logger* logger,
                     spdlog::format_string_t<Args...> fmt,
                     Args&&... args) {
  if(logger) {
    logger->warn(fmt, std::forward<Args>(args)...);
  } else {
    detail::log_with_fallback("warn", spdlog::level::warn, fmt, std::forward<Args>(args)...);
  }
}

template<typename... Args>
inline void log_error(Logger* logger,
                      spdlog::format_string_t<Args...> fmt,
                      Args&&... args) {
  if(logger) {
    logger->error(fmt, std::forward<Args>(args)...);
  } else {
    detail::log_with_fallback("error", spdlog::level::err, fmt, std::forward<Args>(args)...);
  }
}

template<typename... Args>
inline void log_debug(Logger* logger,
                      spdlog::format_string_t<Args...> fmt,
                      Args&&... args) {
  if(logger) {
    logger->debug(fmt, std::forward<Args>(args)...);
  } else {
    detail::log_with_fallback("debug", spdlog::level::debug, fmt, std::forward<Args>(args)...);
  }
}

template<typename... Args>
inline void print_out(Logger* logger,
                      spdlog::format_string_t<Args...> fmt,
                      Args&&... args) {
  if(logger) {
    logger->print(fmt, std::forward<Args>(args)...);
  } else {
    detail::log_with_fallback("print", spdlog::level::info, fmt, std::forward<Args>(args)...);
  }
}

template<typename... Args>
inline void print_err(Logger* logger,
                      spdlog::format_string_t<Args...> fmt,
                      Args&&... args) {
  if(logger) {
    logger->print_err(fmt, std::forward<Args>(args)...);
  } else {
    detail::log_with_fallback("print_err", spdlog::level::err, fmt, std::forward<Args>(args)...);
  }
}
