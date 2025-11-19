#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "log.hpp"

inline const nlohmann::json SETTINGS_SPECIFICATION = nlohmann::json::array({
  {{"key","listen_port"},         {"aliases", {"lp"}},             {"type","int"},    {"default",9000},      {"description","TCP port to listen on"}, {"persistent", true}},
  {{"key","listen_ip"},           {"aliases", {"li"}},             {"type","string"}, {"default","127.0.0.1"},{"description","Interface/IP to bind"}, {"persistent", true}},
  {{"key","bootstrap_peer"},      {"aliases", {"bootstrap"}},      {"type","string"}, {"default",""},        {"description","Bootstrap peer host:port"}, {"persistent", true}},
  {{"key","peer_id"},             {"aliases", {"id"}},             {"type","string"}, {"default",""},        {"description","Explicit peer identifier"}, {"persistent", true}},
  {{"key","external_port"},       {"aliases", {"ep"}},             {"type","int"},    {"default",0},         {"description","External port advertised to peers"}, {"persistent", true}},
  {{"key","audio_notifications"}, {"aliases", {"audio","bell"}},   {"type","bool"},   {"default",false},     {"description","Play terminal bell on chat messages"}, {"persistent", true}},
  {{"key","verbose"},             {"aliases", {"v"}},              {"type","bool"},   {"default",false},     {"description","Enable verbose logging"}, {"persistent", true}},
  {{"key","transfer_debug"},      {"aliases", {"transfer","td"}},  {"type","bool"},   {"default",false},     {"description","Log file chunk send/receive activity"}, {"persistent", true}},
  {{"key","help"},                {"aliases", {"h","?"}},          {"type","bool"},   {"default",false},     {"description","Show command help and exit"}, {"persistent", false}},
  {{"key","save"},                {"aliases", {"persist"}},        {"type","bool"},   {"default",false},     {"description","Persist current settings to disk"}, {"persistent", false}}
});

inline const nlohmann::json ARGV_SPECIFICATION = nlohmann::json::array({
  {{"index",0},{"key","listen_port"}},
  {{"index",1},{"key","listen_ip"}},
  {{"index",2},{"key","bootstrap_peer"}},
  {{"index",3},{"key","peer_id"}},
  {{"index",4},{"key","external_port"}}
});

class SettingsManager {
public:
  SettingsManager();

  void init(int argc, char* argv[]);

  void usage() const;

  template<typename T>
  T get(const std::string& key) const;

  bool has(const std::string& key) const;

  bool set_from_string(const std::string& key, const std::string& value, std::string& error);
  bool set(const std::string& key, const nlohmann::json& value, std::string& error);

  bool save() const;
  bool load();

  bool save_requested() const { return has("save") && get<bool>("save"); }
  bool help_requested() const { return has("help") && get<bool>("help"); }

  std::vector<std::string> keys() const;
  std::string value_as_string(const std::string& key) const;
  std::optional<std::string> resolve_key(const std::string& token) const;

  std::filesystem::path settings_path() const;

private:
  struct SettingSpec {
    std::string key;
    std::vector<std::string> aliases;
    std::string normalized_key;
    std::string type;
    nlohmann::json default_value;
    std::string description;
    bool persistent = true;
  };

  struct ArgvSpec {
    std::size_t index = 0;
    std::string key;
  };

  static const std::vector<SettingSpec>& setting_specs();
  static const std::vector<ArgvSpec>& argv_specs();

  const SettingSpec* find_spec(const std::string& token) const;

  void apply_defaults();
  void merge_from_json(const nlohmann::json& doc);
  void parse_args(int argc, char* argv[]);

  bool convert_and_store(const SettingSpec& spec, const nlohmann::json& value, std::string& error);
  nlohmann::json parse_string_value(const SettingSpec& spec, const std::string& value, std::string& error) const;

  static std::string to_lower(std::string value);
  static std::string trim_copy(std::string value);
  static bool is_bool_literal(const std::string& value);
  std::string default_to_string(const SettingSpec& spec) const;

  nlohmann::json settings_;
  std::string processname_;
};

// ---- implementation -------------------------------------------------------

inline const std::vector<SettingsManager::SettingSpec>& SettingsManager::setting_specs() {
  static const std::vector<SettingSpec> specs = []{
    std::vector<SettingSpec> result;
    for(const auto& entry : SETTINGS_SPECIFICATION) {
      SettingSpec spec;
      spec.key = entry.at("key").get<std::string>();
      spec.normalized_key = SettingsManager::to_lower(spec.key);
      if(entry.contains("aliases")) {
        spec.aliases = entry.at("aliases").get<std::vector<std::string>>();
        for(auto& alias : spec.aliases) {
          alias = SettingsManager::to_lower(alias);
        }
      }
      spec.type = entry.at("type").get<std::string>();
      spec.default_value = entry.at("default");
      spec.description = entry.value("description", "");
      spec.persistent = entry.value("persistent", true);
      result.push_back(std::move(spec));
    }
    return result;
  }();
  return specs;
}

inline const std::vector<SettingsManager::ArgvSpec>& SettingsManager::argv_specs() {
  static const std::vector<ArgvSpec> specs = []{
    std::vector<ArgvSpec> result;
    for(const auto& entry : ARGV_SPECIFICATION) {
      ArgvSpec spec;
      spec.index = entry.at("index").get<std::size_t>();
      spec.key = entry.at("key").get<std::string>();
      result.push_back(std::move(spec));
    }
    std::sort(result.begin(), result.end(),
      [](const ArgvSpec& a, const ArgvSpec& b){ return a.index < b.index; });
    return result;
  }();
  return specs;
}

inline SettingsManager::SettingsManager() {
  apply_defaults();
}

inline void SettingsManager::apply_defaults() {
  settings_ = nlohmann::json::object();
  for(const auto& spec : setting_specs()) {
    settings_[spec.key] = spec.default_value;
  }
}

inline const SettingsManager::SettingSpec* SettingsManager::find_spec(const std::string& token) const {
  std::string lowered = to_lower(token);
  for(const auto& spec : setting_specs()) {
    if(lowered == spec.normalized_key) return &spec;
    if(std::find(spec.aliases.begin(), spec.aliases.end(), lowered) != spec.aliases.end()) {
      return &spec;
    }
  }
  return nullptr;
}

inline void SettingsManager::init(int argc, char* argv[]) {
  processname_ = (argc > 0 && argv[0]) ? argv[0] : "sync";
  apply_defaults();
  load();
  parse_args(argc, argv);
  if(help_requested()) {
    usage();
    std::exit(0);
  }
}

inline void SettingsManager::usage() const {
  print_out("{} - mesh sync node", processname_);
  print_out("Usage:");

  std::string cmd = processname_;
  for(const auto& pos : argv_specs()) {
    cmd += " [" + pos.key + "]";
  }
  print_out("  {}", cmd);
  print_out("");
  print_out("Options:");
  for(const auto& spec : setting_specs()) {
    std::string argument_hint = (spec.type == "bool") ? "[true|false]" : "<" + spec.type + ">";
    std::ostringstream aliases;
    if(!spec.aliases.empty()) {
      aliases << " (alias: ";
      for(std::size_t i = 0; i < spec.aliases.size(); ++i) {
        if(i > 0) aliases << ", ";
        aliases << "-" << spec.aliases[i];
      }
      aliases << ")";
    }
    print_out("  --{} {:<12} {}{} (default: {})",
              spec.key,
              argument_hint,
              spec.description,
              aliases.str(),
              default_to_string(spec));
  }
  print_out("");
}

inline void SettingsManager::parse_args(int argc, char* argv[]) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  std::size_t positional_index = 0;

  auto is_option_token = [](const std::string& candidate) {
    if(candidate.rfind("--", 0) == 0) return true;
    if(candidate.size() >= 2 && candidate[0] == '-' &&
       std::isalpha(static_cast<unsigned char>(candidate[1]))) {
      return true;
    }
    return false;
  };

  for(std::size_t i = 0; i < args.size(); ++i) {
    const std::string& token = args[i];

    if(token.rfind("--", 0) == 0) {
      std::string key_token = token.substr(2);
      const auto* spec = find_spec(key_token);
      if(!spec) {
        print_err("Unknown option {}", token);
        usage();
        std::exit(1);
      }

      std::string value;
      if(spec->type == "bool") {
        if(i + 1 < args.size() && !is_option_token(args[i + 1]) && is_bool_literal(args[i + 1])) {
          value = args[++i];
        } else {
          value = "true";
        }
      } else {
        if(i + 1 >= args.size()) {
          print_err("Missing value for --{}", spec->key);
          usage();
          std::exit(1);
        }
        value = args[++i];
      }

      std::string error;
      if(!set_from_string(spec->key, value, error)) {
        print_err("Invalid value for --{}: {}", spec->key, error);
        usage();
        std::exit(1);
      }
      continue;
    }

    if(token.size() > 1 && token[0] == '-' && token[1] != '-') {
      std::string alias_token = token.substr(1);
      const auto* spec = find_spec(alias_token);
      bool is_alias = false;
      if(spec) {
        std::string lowered_alias = to_lower(alias_token);
        is_alias = std::find(spec->aliases.begin(), spec->aliases.end(), lowered_alias) != spec->aliases.end();
      }
      if(spec && is_alias) {
        std::string value;
        if(spec->type == "bool") {
          if(i + 1 < args.size() && !is_option_token(args[i + 1]) && is_bool_literal(args[i + 1])) {
            value = args[++i];
          } else {
            value = "true";
          }
        } else {
          if(i + 1 >= args.size()) {
            print_err("Missing value for -{}", alias_token);
            usage();
            std::exit(1);
          }
          value = args[++i];
        }

        std::string error;
        if(!set_from_string(spec->key, value, error)) {
          print_err("Invalid value for -{}: {}", alias_token, error);
          usage();
          std::exit(1);
        }
        continue;
      }
      // Not a recognised alias; fall through to positional handling.
    }

    if(positional_index >= argv_specs().size()) {
      print_err("Unexpected positional argument '{}'", token);
      usage();
      std::exit(1);
    }

    const auto& pos_spec = argv_specs()[positional_index++];
    std::string error;
    if(!set_from_string(pos_spec.key, token, error)) {
      print_err("Invalid value for {} '{}': {}", pos_spec.key, token, error);
      usage();
      std::exit(1);
    }
  }
}

inline bool SettingsManager::has(const std::string& key) const {
  return settings_.contains(key);
}

inline std::vector<std::string> SettingsManager::keys() const {
  std::vector<std::string> out;
  out.reserve(setting_specs().size());
  for(const auto& spec : setting_specs()) out.push_back(spec.key);
  return out;
}

inline std::string SettingsManager::value_as_string(const std::string& key) const {
  if(!has(key)) return "<unknown>";
  const auto& value = settings_.at(key);
  if(value.is_string()) return value.get<std::string>();
  if(value.is_boolean()) return value.get<bool>() ? "true" : "false";
  return value.dump();
}

inline std::filesystem::path SettingsManager::settings_path() const {
  return std::filesystem::current_path() / "share" / ".config" / "settings.json";
}

inline bool SettingsManager::load() {
  auto path = settings_path();
  std::ifstream in(path);
  if(!in) return false;
  try {
    nlohmann::json doc;
    in >> doc;
    merge_from_json(doc);
    return true;
  } catch(const std::exception& e) {
    print_err("Failed to parse settings.json: {}", e.what());
    return false;
  }
}

inline void SettingsManager::merge_from_json(const nlohmann::json& doc) {
  if(!doc.is_object()) return;
  for(const auto& item : doc.items()) {
    const auto* spec = find_spec(item.key());
    if(!spec) continue;
    std::string error;
    if(!convert_and_store(*spec, item.value(), error) && !error.empty()) {
      print_err("Ignoring invalid setting '{}': {}", item.key(), error);
    }
  }
}

inline bool SettingsManager::save() const {
  auto path = settings_path();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if(!out) {
    print_err("Unable to write {}", path.string());
    return false;
  }
  nlohmann::json doc = nlohmann::json::object();
  for(const auto& spec : setting_specs()) {
    if(!spec.persistent) continue;
    if(settings_.contains(spec.key)) {
      doc[spec.key] = settings_.at(spec.key);
    }
  }
  out << doc.dump(2);
  return true;
}

inline bool SettingsManager::convert_and_store(const SettingSpec& spec,
                                                  const nlohmann::json& value,
                                                  std::string& error) {
  if(spec.type == "bool") {
    if(value.is_boolean()) {
      settings_[spec.key] = value.get<bool>();
      return true;
    }
    if(value.is_number_integer()) {
      settings_[spec.key] = (value.get<int>() != 0);
      return true;
    }
    error = "expected boolean";
    return false;
  }
  if(spec.type == "int") {
    if(value.is_number_integer()) {
      settings_[spec.key] = value.get<int>();
      return true;
    }
    error = "expected integer";
    return false;
  }
  if(spec.type == "float") {
    if(value.is_number()) {
      settings_[spec.key] = value.get<double>();
      return true;
    }
    error = "expected number";
    return false;
  }
  if(spec.type == "string") {
    if(value.is_string()) {
      settings_[spec.key] = value.get<std::string>();
      return true;
    }
    error = "expected string";
    return false;
  }
  if(spec.type == "json") {
    settings_[spec.key] = value;
    return true;
  }
  error = "unknown type";
  return false;
}

inline nlohmann::json SettingsManager::parse_string_value(const SettingSpec& spec,
                                                             const std::string& value,
                                                             std::string& error) const {
  error.clear();
  std::string clean = trim_copy(value);
  if(spec.type == "bool") {
    std::string v = to_lower(clean);
    if(v == "true" || v == "1" || v == "on" || v == "yes") return true;
    if(v == "false" || v == "0" || v == "off" || v == "no") return false;
    error = "expected boolean (true|false|on|off)";
    return {};
  }
  if(spec.type == "int") {
    try {
      return std::stoi(clean);
    } catch(const std::exception& e) {
      error = e.what();
      return {};
    }
  }
  if(spec.type == "float") {
    try {
      return std::stod(clean);
    } catch(const std::exception& e) {
      error = e.what();
      return {};
    }
  }
  if(spec.type == "string") {
    return clean;
  }
  if(spec.type == "json") {
    try {
      return nlohmann::json::parse(clean);
    } catch(const std::exception& e) {
      error = e.what();
      return {};
    }
  }
  error = "unsupported type";
  return {};
}

inline bool SettingsManager::set_from_string(const std::string& key,
                                                const std::string& value,
                                                std::string& error) {
  const auto* spec = find_spec(key);
  if(!spec) {
    error = "unknown setting";
    return false;
  }
  auto parsed = parse_string_value(*spec, value, error);
  if(!error.empty()) return false;
  return convert_and_store(*spec, parsed, error);
}

inline bool SettingsManager::set(const std::string& key,
                                    const nlohmann::json& value,
                                    std::string& error) {
  const auto* spec = find_spec(key);
  if(!spec) {
    error = "unknown setting";
    return false;
  }
  error.clear();
  return convert_and_store(*spec, value, error);
}

inline std::string SettingsManager::to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
  return value;
}

inline std::string SettingsManager::trim_copy(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(),
    [](unsigned char ch){ return !std::isspace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
    [](unsigned char ch){ return !std::isspace(ch); }).base(), value.end());
  return value;
}

inline bool SettingsManager::is_bool_literal(const std::string& value) {
  std::string lowered = to_lower(trim_copy(value));
  return lowered == "true" || lowered == "false" ||
         lowered == "on" || lowered == "off" ||
         lowered == "1" || lowered == "0" ||
         lowered == "yes" || lowered == "no";
}

inline std::string SettingsManager::default_to_string(const SettingSpec& spec) const {
  if(spec.type == "bool") {
    return spec.default_value.get<bool>() ? "true" : "false";
  }
  if(spec.default_value.is_string()) {
    return spec.default_value.get<std::string>();
  }
  return spec.default_value.dump();
}

inline std::optional<std::string> SettingsManager::resolve_key(const std::string& token) const {
  if(const auto* spec = find_spec(token)) {
    return spec->key;
  }
  return std::nullopt;
}

template<typename T>
inline T SettingsManager::get(const std::string& key) const {
  if(!has(key)) {
    throw std::runtime_error("Unknown setting: " + key);
  }
  return settings_.at(key).get<T>();
}
