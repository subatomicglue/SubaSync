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
  {{"key","transfer_debug"},      {"aliases", {"transfer","td"}},  {"type","bool"},   {"default",false},     {"description","Log chunk send/receive activity"}, {"persistent", true}},
  {{"key","transfer_progress"},   {"aliases", {"progress","tp"}}, {"type","bool"},   {"default",true},      {"description","Show ASCII progress meter during downloads"}, {"persistent", true}},
  {{"key","progress_meter_size"}, {"aliases", {"meter","meter_size","pms"}}, {"type","int"}, {"default",80}, {"description","Number of characters used for the transfer progress meter"}, {"persistent", true}},
  {{"key","swarm_max_parallel"},  {"aliases", {"swarm_parallel","smp"}}, {"type","int"}, {"default",0}, {"description","Maximum peers to download from in parallel (0 = all)"}, {"persistent", true}},
  {{"key","swarm_chunk_buffers"}, {"aliases", {"swarm_buffers","scb"}}, {"type","int"}, {"default",1}, {"description","Buffered chunks per swarm peer"}, {"persistent", true}},
  {{"key","swarm_chunk_size"},    {"aliases", {"swarm_chunk_bytes","scs"}}, {"type","int"}, {"default",262144}, {"description","Preferred chunk size in bytes (<= max chunk size)"}, {"persistent", true}},
  {{"key","swarm_progress_interval_ms"}, {"aliases", {"swarm_meter_interval","spim"}}, {"type","int"}, {"default",200}, {"description","Minimum milliseconds between swarm progress updates"}, {"persistent", true}},
  {{"key","help"},                {"aliases", {"h","?"}},          {"type","bool"},   {"default",false},     {"description","Show command help and exit"}, {"persistent", false}},
  {{"key","save"},                {"aliases", {"persist"}},        {"type","bool"},   {"default",false},     {"description","Persist current settings to disk"}, {"persistent", false}}
});

class SettingsManager {
public:
  SettingsManager();
  explicit SettingsManager(const nlohmann::json& specification);

  template<typename T>
  T get(const std::string& key) const;

  bool has(const std::string& key) const;

  bool set_from_string(const std::string& key, const std::string& value, std::string& error);
  bool set_from_json(const std::string& key, const nlohmann::json& value, std::string& error);

  bool save() const;
  bool load();
  bool save_to_file(const std::filesystem::path& path) const;
  bool load_from_file(const std::filesystem::path& path);

  bool save_requested() const { return has("save") && get<bool>("save"); }
  bool help_requested() const { return has("help") && get<bool>("help"); }

  std::vector<std::string> keys() const;
  std::string value_as_string(const std::string& key) const;
  std::optional<std::string> resolve_key(const std::string& token) const;
  bool is_bool_setting(const std::string& key) const;

  std::filesystem::path settings_path() const;
  void set_settings_path(const std::filesystem::path& path);

  void set_json(const nlohmann::json& doc);
  nlohmann::json get_json(bool persistent_only = true) const;
  static std::string to_lower(std::string value);
  static std::string trim_copy(std::string value);

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

  static std::vector<SettingSpec> build_setting_specs(const nlohmann::json& specification);
  const std::vector<SettingSpec>& setting_specs() const { return setting_specs_; }

  const SettingSpec* find_spec(const std::string& token) const;

  void apply_defaults();
  void merge_from_json(const nlohmann::json& doc);

  bool convert_and_store(const SettingSpec& spec, const nlohmann::json& value, std::string& error);
  nlohmann::json parse_string_value(const SettingSpec& spec, const std::string& value, std::string& error) const;

  static bool is_bool_literal(const std::string& value);
  std::string default_to_string(const SettingSpec& spec) const;

  nlohmann::json settings_;
  std::vector<SettingSpec> setting_specs_;
  std::filesystem::path settings_path_override_;
};

// ---- implementation -------------------------------------------------------

inline std::vector<SettingsManager::SettingSpec> SettingsManager::build_setting_specs(const nlohmann::json& specification) {
  std::vector<SettingSpec> result;
  for(const auto& entry : specification) {
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
}

inline SettingsManager::SettingsManager()
  : SettingsManager(SETTINGS_SPECIFICATION) {}

inline SettingsManager::SettingsManager(const nlohmann::json& specification)
  : setting_specs_(build_setting_specs(specification)) {
  apply_defaults();
}

inline void SettingsManager::apply_defaults() {
  settings_ = nlohmann::json::object();
  for(const auto& spec : setting_specs_) {
    settings_[spec.key] = spec.default_value;
  }
}

inline const SettingsManager::SettingSpec* SettingsManager::find_spec(const std::string& token) const {
  std::string lowered = to_lower(token);
  for(const auto& spec : setting_specs_) {
    if(lowered == spec.normalized_key) return &spec;
    if(std::find(spec.aliases.begin(), spec.aliases.end(), lowered) != spec.aliases.end()) {
      return &spec;
    }
  }
  return nullptr;
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

inline void SettingsManager::set_settings_path(const std::filesystem::path& path) {
  settings_path_override_ = path;
}

inline std::filesystem::path SettingsManager::settings_path() const {
  if(!settings_path_override_.empty()) {
    return settings_path_override_;
  }
  return std::filesystem::current_path() / ".config" / "settings.json";
}

inline bool SettingsManager::load() {
  return load_from_file(settings_path());
}

inline bool SettingsManager::save() const {
  return save_to_file(settings_path());
}

inline bool SettingsManager::load_from_file(const std::filesystem::path& path) {
  if(path.empty()) return false;
  std::ifstream in(path);
  if(!in) return false;
  try {
    nlohmann::json doc;
    in >> doc;
    merge_from_json(doc);
    return true;
  } catch(const std::exception& e) {
    print_err(nullptr, "Failed to parse {}: {}", path.string(), e.what());
    return false;
  }
}

inline bool SettingsManager::save_to_file(const std::filesystem::path& path) const {
  if(path.empty()) return false;
  std::error_code ec;
  if(path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream out(path);
  if(!out) {
    print_err(nullptr, "Unable to write {}", path.string());
    return false;
  }
  out << get_json(true).dump(2);
  return true;
}

inline void SettingsManager::merge_from_json(const nlohmann::json& doc) {
  if(!doc.is_object()) return;
  for(const auto& item : doc.items()) {
    const auto* spec = find_spec(item.key());
    if(!spec) continue;
    std::string error;
    if(!convert_and_store(*spec, item.value(), error) && !error.empty()) {
      print_err(nullptr, "Ignoring invalid setting '{}': {}", item.key(), error);
    }
  }
}

inline void SettingsManager::set_json(const nlohmann::json& doc) {
  merge_from_json(doc);
}

inline nlohmann::json SettingsManager::get_json(bool persistent_only) const {
  nlohmann::json doc = nlohmann::json::object();
  for(const auto& spec : setting_specs()) {
    if(persistent_only && !spec.persistent) continue;
    if(settings_.contains(spec.key)) {
      doc[spec.key] = settings_.at(spec.key);
    }
  }
  return doc;
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

inline bool SettingsManager::set_from_json(const std::string& key,
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

inline bool SettingsManager::is_bool_setting(const std::string& key) const {
  const auto* spec = find_spec(key);
  return spec && spec->type == "bool";
}

template<typename T>
inline T SettingsManager::get(const std::string& key) const {
  if(!has(key)) {
    throw std::runtime_error("Unknown setting: " + key);
  }
  return settings_.at(key).get<T>();
}
