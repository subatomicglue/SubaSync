#pragma once

#include <spdlog/spdlog.h>
#include <utility>

void init(bool verbose = false);

spdlog::logger& info_logger();
spdlog::logger& error_logger();
spdlog::logger& print_logger();
spdlog::logger& print_error_logger();

template<typename... Args>
inline void log_info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  info_logger().info(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  info_logger().warn(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  error_logger().error(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  info_logger().debug(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void log_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  info_logger().log(spdlog::level::info, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void print_out(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  print_logger().info(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
inline void print_err(spdlog::format_string_t<Args...> fmt, Args&&... args) {
  print_error_logger().error(fmt, std::forward<Args>(args)...);
}
