#pragma once
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// protocol.hpp
inline constexpr int kPeerAnnounceDefaultTTL = 5;

json make_peer_announce(const std::string& id,
                        const std::string& local_addr,
                        const std::string& display_name,
                        const std::string& external_addr,
                        int ttl = kPeerAnnounceDefaultTTL);
json make_peer_list(const json& peers_array);
