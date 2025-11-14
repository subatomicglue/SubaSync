#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <mutex>

namespace {
std::shared_ptr<spdlog::logger> g_info_logger;
std::shared_ptr<spdlog::logger> g_error_logger;
std::shared_ptr<spdlog::logger> g_print_logger;
std::shared_ptr<spdlog::logger> g_print_err_logger;
std::once_flag g_init_once;

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
