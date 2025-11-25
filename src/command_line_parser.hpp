#pragma once

#include <string>
#include <vector>

#include "settings_manager.hpp"

class CommandLineParser {
public:
  CommandLineParser(std::string process_name = "sync",
                    nlohmann::json settings_spec = SETTINGS_SPECIFICATION,
                    nlohmann::json argv_spec = nlohmann::json::array({
                      {{"index",0},{"key","listen_port"}},
                      {{"index",1},{"key","listen_ip"}},
                      {{"index",2},{"key","bootstrap_peer"}},
                      {{"index",3},{"key","peer_id"}},
                      {{"index",4},{"key","external_port"}}
                    }));

  void parse(int argc, char* argv[], SettingsManager& settings) const;
  void usage() const;

private:
  struct ArgvSpec {
    std::size_t index = 0;
    std::string key;
  };

  std::vector<ArgvSpec> build_positional_specs(const nlohmann::json& spec) const;
  static bool is_option_token(const std::string& candidate);
  static bool is_bool_literal(const std::string& value);

  std::string process_name_;
  nlohmann::json settings_spec_;
  nlohmann::json argv_spec_;
  std::vector<ArgvSpec> positional_specs_;
};
