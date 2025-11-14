#include "protocol.hpp"

nlohmann::json make_peer_announce(const std::string& peer_id,
                                  const std::string& local_addr,
                                  const std::string& display_name = "",
                                  const std::string& external_addr = "",
                                  int ttl) {
    nlohmann::json j;
    j["type"] = "peer_announce";
    j["peer_id"] = peer_id;
    j["display_name"] = display_name;
    j["local_addr"] = local_addr;
    j["external_addr"] = external_addr;
    j["ttl"] = ttl;
    return j;
}



json make_peer_list(const json& peers_array){
    json j;
    j["type"] = "peer_list";
    j["peers"] = peers_array;
    return j;
}
