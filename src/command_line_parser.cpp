#include "command_line_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <stdexcept>

#include "log.hpp"

CommandLineParser::CommandLineParser(std::string process_name,
                                     nlohmann::json settings_spec,
                                     nlohmann::json argv_spec)
  : process_name_(std::move(process_name)),
    settings_spec_(std::move(settings_spec)),
    argv_spec_(std::move(argv_spec)),
    positional_specs_(build_positional_specs(argv_spec_)) {}

std::vector<CommandLineParser::ArgvSpec> CommandLineParser::build_positional_specs(const nlohmann::json& spec) const {
  std::vector<ArgvSpec> result;
  for(const auto& entry : spec) {
    ArgvSpec out;
    out.index = entry.at("index").get<std::size_t>();
    out.key = entry.at("key").get<std::string>();
    result.push_back(std::move(out));
  }
  std::sort(result.begin(), result.end(),
            [](const ArgvSpec& a, const ArgvSpec& b){ return a.index < b.index; });

  SettingsManager probe(settings_spec_);
  for(const auto& argv_entry : result) {
    if(!probe.resolve_key(argv_entry.key)) {
      throw std::runtime_error("ARGV specification references unknown setting '" + argv_entry.key + "'");
    }
  }
  return result;
}

bool CommandLineParser::is_option_token(const std::string& candidate) {
  if(candidate.rfind("--", 0) == 0) return true;
  if(candidate.size() >= 2 && candidate[0] == '-' &&
     std::isalpha(static_cast<unsigned char>(candidate[1]))) {
    return true;
  }
  return false;
}

bool CommandLineParser::is_bool_literal(const std::string& value) {
  std::string lowered = SettingsManager::to_lower(SettingsManager::trim_copy(value));
  return lowered == "true" || lowered == "false" ||
         lowered == "on" || lowered == "off" ||
         lowered == "1" || lowered == "0" ||
         lowered == "yes" || lowered == "no";
}

void CommandLineParser::parse(int argc, char* argv[], SettingsManager& settings) const {
  std::vector<std::string> args;
  if(argc > 1 && argv) {
    args.assign(argv + 1, argv + argc);
  }
  std::size_t positional_index = 0;

  auto fail = [&](const std::string& message){
    print_err(nullptr, "{}", message);
    usage();
    std::exit(1);
  };

  for(std::size_t i = 0; i < args.size(); ++i) {
    const std::string& token = args[i];

    auto handle_option = [&](const std::string& key_token, bool long_form){
      auto resolved = settings.resolve_key(key_token);
      if(!resolved) {
        if(long_form) {
          fail("Unknown option --" + key_token);
        }
        return false; // treat as positional for short tokens
      }
      bool is_bool = settings.is_bool_setting(*resolved);
      std::string value;
      if(is_bool) {
        if(i + 1 < args.size() && !is_option_token(args[i + 1]) && is_bool_literal(args[i + 1])) {
          value = args[++i];
        } else {
          value = "true";
        }
      } else {
        if(i + 1 >= args.size()) {
          fail("Missing value for option '" + key_token + "'");
        }
        value = args[++i];
      }
      std::string error;
      if(!settings.set_from_string(*resolved, value, error)) {
        fail("Invalid value for option '" + key_token + "': " + error);
      }
      return true;
    };

    if(token.rfind("--", 0) == 0) {
      std::string key = token.substr(2);
      handle_option(key, true);
      continue;
    }

    if(token.size() > 1 && token[0] == '-' && token[1] != '-') {
      std::string alias = token.substr(1);
      if(handle_option(alias, false)) {
        continue;
      }
      // fall through to positional if alias unrecognised
    }

    if(positional_index >= positional_specs_.size()) {
      fail("Unexpected positional argument '" + token + "'");
    }
    const auto& spec = positional_specs_[positional_index++];
    std::string error;
    if(!settings.set_from_string(spec.key, token, error)) {
      fail("Invalid value for " + spec.key + " '" + token + "': " + error);
    }
  }
}

void CommandLineParser::usage() const {
  print_out(nullptr, "{} - mesh sync node", process_name_);
  print_out(nullptr, "Usage:");

  std::string cmd = process_name_;
  for(const auto& pos : positional_specs_) {
    cmd += " [" + pos.key + "]";
  }
  print_out(nullptr, "  {}", cmd);
  print_out(nullptr, "");
  print_out(nullptr, "Options:");
  for(const auto& entry : settings_spec_) {
    auto key = entry.at("key").get<std::string>();
    auto type = entry.at("type").get<std::string>();
    std::string argument_hint = (type == "bool") ? "[true|false]" : "<" + type + ">";
    std::ostringstream aliases;
    if(entry.contains("aliases")) {
      const auto alias_list = entry.at("aliases").get<std::vector<std::string>>();
      if(!alias_list.empty()) {
        aliases << " (alias: ";
        for(std::size_t i = 0; i < alias_list.size(); ++i) {
          if(i > 0) aliases << ", ";
          aliases << "-" << alias_list[i];
        }
        aliases << ")";
      }
    }
    auto description = entry.value("description", "");
    auto default_value = entry.at("default");
    std::string default_str;
    if(type == "bool") {
      default_str = default_value.get<bool>() ? "true" : "false";
    } else if(default_value.is_string()) {
      default_str = default_value.get<std::string>();
    } else {
      default_str = default_value.dump();
    }
    print_out(nullptr, "  --{} {:<12} {}{} (default: {})",
              key,
              argument_hint,
              description,
              aliases.str(),
              default_str);
  }
  print_out(nullptr, "");
}
