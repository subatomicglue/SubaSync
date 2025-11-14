#include "peer_manager.hpp"
#include "connection.hpp"
#include "log.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <array>
#include <filesystem>
#include <random>
#include <exception>

namespace {

std::string normalize_relative_path(const std::string& input) {
  if(input.empty()) return "";
  std::string out = input;
  // trim whitespace
  out.erase(out.begin(), std::find_if(out.begin(), out.end(), [](unsigned char ch){ return !std::isspace(ch); }));
  out.erase(std::find_if(out.rbegin(), out.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), out.end());
  if(out.empty()) return "";
  if(out.front() == '/') out.erase(out.begin());
  while(!out.empty() && out.back() == '/') out.pop_back();
  if(out == ".") return "";
  if(out.rfind("./", 0) == 0) {
    out.erase(0, 2);
    if(out.empty()) return "";
  }
  return out;
}

bool is_internal_path(const std::string& path) {
  if(path.empty()) return false;
  static const std::array<const char*, 4> reserved = {
    ".sync_tmp",
    ".archive",
    ".config",
    ".conflict-stage"
  };
  for(const auto* prefix : reserved) {
    std::string view(prefix);
    if(path == view) return true;
    if(path.rfind(view + "/", 0) == 0) return true;
  }
  return false;
}

std::string ensure_trailing_slash(const std::string& input) {
  if(input.empty()) return "";
  if(input.back() == '/') return input;
  return input + "/";
}

} // namespace

PeerManager::PeerManager(asio::io_context& io,
                         std::string local_peer_id,
                         std::string display_name,
                         std::string local_addr,
                         std::string external_addr,
                         size_t max_peers)
    : io_(io),
      local_peer_id_(std::move(local_peer_id)),
      display_name_(std::move(display_name)),
      local_addr_(std::move(local_addr)),
      external_addr_(std::move(external_addr)),
      max_peers_(max_peers)
{
  // Add self to peers list
  PeerInfo self;
  self.peer_id = local_peer_id_;
  self.display_name = display_name_;
  self.local_addr = local_addr_;
  self.external_addr = external_addr_;
  self.state = PeerInfo::State::Connected;
  peers_[local_peer_id_] = self;

  register_directory_hierarchy("");
}


void PeerManager::add_peer_discovered(const std::string& peer_id,
                                      const std::string& display_name,
                                      const std::string& local_addr,
                                      const std::string& external_addr)
{
  std::lock_guard lg(m_);
  if(peer_id == local_peer_id_) return;

  auto it = peers_.find(peer_id);
  if(it == peers_.end()){
    PeerInfo pi;
    pi.peer_id = peer_id;
    pi.display_name = display_name;
    pi.local_addr = local_addr;
    pi.external_addr = external_addr;
    pi.state = PeerInfo::State::Disconnected;
    peers_[peer_id] = pi;
    log_info("Discovered new peer {} ({}) @ local:{} external:{}", peer_id, display_name, local_addr, external_addr);
  } else {
    // update addresses if changed
    if(it->second.local_addr != local_addr){
      it->second.local_addr = local_addr;
      log_info("Updated peer {} local_addr -> {}", peer_id, local_addr);
    }
    if(it->second.external_addr != external_addr){
      it->second.external_addr = external_addr;
      log_info("Updated peer {} external_addr -> {}", peer_id, external_addr);
    }
  }
}


void PeerManager::on_connected(const std::string& peer_id, std::shared_ptr<Connection> conn){
  // lock and update state
    {
        std::lock_guard lg(m_);
        auto it = connections_.find(peer_id);
        if(it != connections_.end() && it->second == conn){
            return; // already processed this connection
        }

        log_info("PeerManager: connected {}", peer_id);
        connections_[peer_id] = conn;

        if(peers_.count(peer_id)){
            peers_[peer_id].state = PeerInfo::State::Connected;
        } else {
            PeerInfo p;
            p.peer_id = peer_id;
            p.state = PeerInfo::State::Connected;
            peers_[peer_id] = p;
        }

        in_progress_connects_.erase(peer_id);
    }

    // --- send known peer list to newly connected peer ---
    nlohmann::json j;
    j["type"] = "peer_list";
    j["peers"] = make_peer_list_json();
    conn->async_send_json(j);
}



void PeerManager::on_disconnected(const std::string& peer_id){
  std::lock_guard lg(m_);
  log_info("PeerManager: disconnected {}", peer_id);
  connections_.erase(peer_id);
  if(peers_.count(peer_id)){
    peers_[peer_id].state = PeerInfo::State::Disconnected;
  }
  in_progress_connects_.erase(peer_id);
}

void PeerManager::broadcast_json(const nlohmann::json& j, const std::string& exclude_peer_id){
  std::vector<std::shared_ptr<Connection>> targets;
  {
    std::lock_guard lg(m_);
    for(auto &kv : connections_){
      if(kv.first == exclude_peer_id) continue;
      if(kv.second){
        targets.push_back(kv.second);
      }
    }
  }

  if(targets.empty()) return;

  auto self = shared_from_this();
  nlohmann::json payload = j;
  asio::post(io_, [self, targets = std::move(targets), payload = std::move(payload)]() mutable {
    for(auto& conn : targets){
      if(conn){
        conn->async_send_json(payload);
      }
    }
  });
}

void PeerManager::set_chat_callback(std::function<void(const std::string&, const std::string&)> cb) {
  std::lock_guard lock(chat_callback_mutex_);
  chat_callback_ = std::move(cb);
}

void PeerManager::set_listing_refresh_callback(std::function<void()> cb) {
  std::lock_guard lg(m_);
  listing_refresh_callback_ = std::move(cb);
}

void PeerManager::dispatch_chat_message(const std::string& from_peer,
                                        const std::string& text) {
  std::function<void(const std::string&, const std::string&)> cb;
  {
    std::lock_guard lock(chat_callback_mutex_);
    cb = chat_callback_;
  }

  if(cb) {
    cb(from_peer, text);
  } else {
    print_out("[{}] {}", from_peer, text);
  }
}

nlohmann::json PeerManager::make_peer_list_json(){
  nlohmann::json arr = nlohmann::json::array();
  std::lock_guard lg(m_);
  for(auto &kv : peers_){
    nlohmann::json p;
    p["peer_id"] = kv.second.peer_id;
    p["display_name"] = kv.second.display_name;
    p["local_addr"] = kv.second.local_addr;
    p["external_addr"] = kv.second.external_addr;
    arr.push_back(p);
  }
  return arr;
}

void PeerManager::attempt_connect_more(){
  std::lock_guard lg(m_);
  // count connected peers (excluding ourselves)
  size_t connected = 0;
  for(const auto &kv : connections_){
    if(kv.first != local_peer_id_) ++connected;
  }
  if(connected >= max_peers_) return;

  // Extract local IP from local_addr_ (format "IP:port")
  std::string local_ip;
  auto pos = local_addr_.find(':');
  if(pos != std::string::npos){
    local_ip = local_addr_.substr(0, pos);
  }

  // pick discovered peers that are disconnected and not in-progress
  for(auto &kv : peers_){
    if(kv.first == local_peer_id_) continue;
    if(connections_.count(kv.first)) continue;
    if(in_progress_connects_.count(kv.first)) continue;

    std::string addr = kv.second.best_addr(local_ip); // or dynamically detect subnet
    if(kv.second.state == PeerInfo::State::Disconnected && !addr.empty()){
      auto pos = addr.find(':');
      if(pos == std::string::npos) continue;

      std::string host = addr.substr(0, pos);
      unsigned short port = static_cast<unsigned short>(std::stoi(addr.substr(pos+1)));
      in_progress_connects_.insert(kv.first);
      kv.second.state = PeerInfo::State::Connecting;

      asio::post(io_, [host, port, pm = shared_from_this()](){
        Connection::connect_outgoing(pm->io(), host, port, pm);
      });

      // count this as an in-progress connection
      ++connected;
      if(connected >= max_peers_) break;
    }
  }
}

void PeerManager::register_directory_hierarchy(const std::string& relative_path) {
  std::vector<std::string> dirs;
  dirs.emplace_back(""); // root

  auto normalized = normalize_relative_path(relative_path);
  if(is_internal_path(normalized)) {
    // no registration for staging directory tree
    return;
  }
  if(!normalized.empty()){
    std::filesystem::path p(normalized);
    std::filesystem::path accum;
    for(auto it = p.begin(); it != p.end(); ++it){
      if((*it).string().empty()) continue;
      if(accum.empty()) accum = *it;
      else accum /= *it;
      auto rel = accum.generic_string();
      if(is_internal_path(rel)) {
        return;
      }
      dirs.emplace_back(rel);
    }
  }

  std::lock_guard lg(m_);
  for(const auto& dir : dirs){
    if(is_internal_path(dir)) continue;
    if(path_to_dir_guid_.count(dir)) continue;
    std::string guid;
    do {
      guid = generate_directory_guid();
    } while(dir_guid_to_path_.count(guid));
    path_to_dir_guid_[dir] = guid;
    dir_guid_to_path_[guid] = dir;
  }
}

std::string PeerManager::ensure_directory_guid(const std::string& relative_path){
  auto normalized = normalize_relative_path(relative_path);
  if(is_internal_path(normalized)) return "";
  {
    std::lock_guard lg(m_);
    auto it = path_to_dir_guid_.find(normalized);
    if(it != path_to_dir_guid_.end()) return it->second;
  }
  register_directory_hierarchy(normalized);
  std::lock_guard lg(m_);
  return path_to_dir_guid_[normalized];
}

std::optional<std::string> PeerManager::resolve_directory_guid(const std::string& guid) const{
  std::lock_guard lg(m_);
  auto it = dir_guid_to_path_.find(guid);
  if(it == dir_guid_to_path_.end()) return std::nullopt;
  if(is_internal_path(it->second)) return std::nullopt;
  return it->second;
}

void PeerManager::register_local_share(const std::string& hash,
                                       const std::string& relative_path,
                                       const std::filesystem::path& absolute_path,
                                       uint64_t size) {
  auto normalized = normalize_relative_path(relative_path);
  if(is_internal_path(normalized)) return;
  std::filesystem::path parent = std::filesystem::path(normalized).parent_path();
  register_directory_hierarchy(parent.generic_string());
  // ensure directory guid for parent directories even if parent empty handled above
  {
    std::lock_guard lg(m_);
    SharedFileEntry entry;
    entry.hash = hash;
    entry.relative_path = normalized;
    entry.full_path = absolute_path;
    entry.size = size;
    shared_files_by_hash_[hash] = entry;
    path_to_hash_[normalized] = hash;
  }
}

void PeerManager::register_directory(const std::string& relative_path){
  register_directory_hierarchy(relative_path);
}

void PeerManager::register_directory_with_guid(const std::string& relative_path,
                                               const std::string& guid){
  if(guid.empty()) return;
  auto normalized = normalize_relative_path(relative_path);
  if(is_internal_path(normalized)) return;

  if(!normalized.empty()){
    auto parent = std::filesystem::path(normalized).parent_path().generic_string();
    if(!parent.empty()){
      register_directory_hierarchy(parent);
    }
  }

  std::lock_guard lg(m_);
  auto existing_guid = dir_guid_to_path_.find(guid);
  if(existing_guid != dir_guid_to_path_.end() && existing_guid->second != normalized){
    path_to_dir_guid_.erase(existing_guid->second);
  }

  auto it = path_to_dir_guid_.find(normalized);
  if(it != path_to_dir_guid_.end()){
    if(it->second == guid) return;
    dir_guid_to_path_.erase(it->second);
  }

  path_to_dir_guid_[normalized] = guid;
  dir_guid_to_path_[guid] = normalized;
}

std::string PeerManager::generate_directory_guid(){
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist;
  uint64_t hi = dist(rng);
  uint64_t lo = dist(rng);
  std::ostringstream oss;
  oss << "dir-" << std::nouppercase << std::hex << std::setfill('0')
      << std::setw(16) << hi << std::setw(16) << lo;
  return oss.str();
}

bool PeerManager::find_local_file_by_hash(const std::string& hash, SharedFileEntry& out_entry){
  std::lock_guard lg(m_);
  auto it = shared_files_by_hash_.find(hash);
  if(it == shared_files_by_hash_.end()) return false;
  out_entry = it->second;
  return true;
}

bool PeerManager::find_local_file_by_path(const std::string& relative_path,
                                          SharedFileEntry& out_entry) const {
  auto normalized = normalize_relative_path(relative_path);
  if(is_internal_path(normalized)) return false;
  std::lock_guard lg(m_);
  auto it = path_to_hash_.find(normalized);
  if(it == path_to_hash_.end()) return false;
  auto entry_it = shared_files_by_hash_.find(it->second);
  if(entry_it == shared_files_by_hash_.end()) return false;
  out_entry = entry_it->second;
  return true;
}

std::vector<PeerManager::SharedFileEntry> PeerManager::local_listing_for_hash(const std::string& hash){
  SharedFileEntry entry;
  if(find_local_file_by_hash(hash, entry)){
    return {entry};
  }
  return {};
}

std::optional<std::string> PeerManager::directory_guid_for_path(const std::string& relative_path) const {
  auto normalized = normalize_relative_path(relative_path);
  if(is_internal_path(normalized)) return std::nullopt;
  std::string directory = normalized;
  if(!directory.empty()) {
    std::filesystem::path p(directory);
    if(p.has_filename()) {
      directory = p.parent_path().generic_string();
    }
  }
  std::lock_guard lg(m_);
  auto it = path_to_dir_guid_.find(directory);
  if(it == path_to_dir_guid_.end()) return std::nullopt;
  return it->second;
}

std::vector<PeerManager::RemoteListingItem> PeerManager::local_listing_items_for_path(const std::string& relative_path){
  auto normalized = normalize_relative_path(relative_path);
  if(is_internal_path(normalized)) return {};
  std::vector<RemoteListingItem> directories;
  std::unordered_set<std::string> seen_directories;
  std::vector<RemoteListingItem> files;
  auto is_direct_child = [](const std::string& parent, const std::string& candidate) -> std::optional<std::string> {
    if(candidate.empty()) return std::nullopt;
    if(parent.empty()){
      if(candidate.find('/') != std::string::npos) return candidate.substr(0, candidate.find('/'));
      return candidate;
    }
    if(candidate.size() <= parent.size()) return std::nullopt;
    if(candidate.compare(0, parent.size(), parent) != 0) return std::nullopt;
    if(candidate[parent.size()] != '/') return std::nullopt;
    std::string remainder = candidate.substr(parent.size() + 1);
    if(remainder.empty()) return std::nullopt;
    auto slash = remainder.find('/');
    if(slash == std::string::npos) return remainder;
    return remainder.substr(0, slash);
  };

  std::lock_guard lg(m_);

  // Directories
  for(const auto& kv : path_to_dir_guid_){
    const auto& path = kv.first;
    if(is_internal_path(path)) continue;
    if(path == normalized) continue;
    auto child = is_direct_child(normalized, path);
    if(!child) continue;
    std::string child_path = normalized.empty() ? *child : normalized + "/" + *child;
    if(!seen_directories.insert(child_path).second) continue;
    RemoteListingItem item;
    item.peer_id = local_peer_id_;
    item.hash.clear();
    item.relative_path = child_path;
    item.size = 0;
    item.is_directory = true;
    item.directory_guid = kv.second;
    directories.push_back(std::move(item));
  }

  // Files
  for(const auto& kv : shared_files_by_hash_){
    const auto& entry = kv.second;
    const auto& rel = entry.relative_path;
    if(is_internal_path(rel)) continue;
    std::string parent = std::filesystem::path(rel).parent_path().generic_string();
    std::string name = std::filesystem::path(rel).filename().generic_string();
    std::string normalized_parent = normalize_relative_path(parent);

    if(normalized_parent != normalized) continue;

    RemoteListingItem item;
    item.peer_id = local_peer_id_;
    item.hash = entry.hash;
    item.relative_path = entry.relative_path;
    item.size = entry.size;
    item.is_directory = false;
    files.push_back(std::move(item));
  }

  std::sort(directories.begin(), directories.end(), [](const RemoteListingItem& a, const RemoteListingItem& b){
    return a.relative_path < b.relative_path;
  });
  std::sort(files.begin(), files.end(), [](const RemoteListingItem& a, const RemoteListingItem& b){
    return a.relative_path < b.relative_path;
  });

  directories.insert(directories.end(),
                     std::make_move_iterator(files.begin()),
                     std::make_move_iterator(files.end()));
  return directories;
}

std::vector<PeerManager::RemoteListingItem> PeerManager::local_listing_for_dir_guid(const std::string& guid){
  auto resolved = resolve_directory_guid(guid);
  if(!resolved) return {};
  return local_listing_items_for_path(*resolved);
}

bool PeerManager::send_json_to_peer(const std::string& peer_id, const nlohmann::json& j){
  std::shared_ptr<Connection> conn;
  {
    std::lock_guard lg(m_);
    auto it = connections_.find(peer_id);
    if(it == connections_.end()) return false;
    conn = it->second;
  }

  if(!conn) return false;

  auto self = shared_from_this();
  nlohmann::json payload = j;
  asio::post(io_, [self, conn = std::move(conn), payload = std::move(payload)]() mutable {
    if(conn){
      conn->async_send_json(payload);
    }
  });
  return true;
}

bool PeerManager::request_peer_listing(const std::string& peer_id,
                                       const std::string& relative_path,
                                       const std::string& hash,
                                       const std::string& dir_guid,
                                       ListResponseHandler handler){
  std::string request_id;
  {
    std::lock_guard lg(m_);
    auto it = connections_.find(peer_id);
    if(it == connections_.end()) return false;
    request_id = local_peer_id_ + "-list-" + std::to_string(++list_request_counter_);
    pending_list_requests_[request_id] = std::move(handler);
  }

  nlohmann::json req;
  req["type"] = "list_request";
  req["request_id"] = request_id;
  if(!relative_path.empty()) req["path"] = relative_path;
  if(!hash.empty()) req["hash"] = hash;
  if(!dir_guid.empty()) req["dir_guid"] = dir_guid;
  if(!send_json_to_peer(peer_id, req)){
    std::lock_guard lg(m_);
    pending_list_requests_.erase(request_id);
    return false;
  }
  return true;
}

namespace {
struct AggregateContext {
  std::mutex m;
  size_t pending = 0;
  std::vector<PeerManager::RemoteListingItem> items;
  std::function<void(const std::vector<PeerManager::RemoteListingItem>&)> handler;
};
}

bool PeerManager::request_hash_listing_all(const std::string& hash,
                                           std::function<void(const std::vector<RemoteListingItem>&)> handler){
  std::vector<std::string> peers;
  {
    std::lock_guard lg(m_);
    for(auto& kv : connections_){
      peers.push_back(kv.first);
    }
  }

  auto ctx = std::make_shared<AggregateContext>();
  ctx->handler = std::move(handler);

  auto local_entries = local_listing_for_hash(hash);
  for(auto& entry : local_entries){
    RemoteListingItem item;
    item.peer_id = local_peer_id_;
    item.hash = entry.hash;
    item.relative_path = entry.relative_path;
    item.size = entry.size;
    item.directory_guid = ensure_directory_guid(std::filesystem::path(entry.relative_path).parent_path().generic_string());
    ctx->items.push_back(item);
  }

  if(peers.empty()){
    if(ctx->handler){
      ctx->handler(ctx->items);
    }
    return true;
  }

  ctx->pending = peers.size();

  auto notify = [ctx](const std::vector<RemoteListingItem>& new_items){
    std::vector<RemoteListingItem> collected;
    std::function<void(const std::vector<RemoteListingItem>&)> cb;
    {
      std::lock_guard lk(ctx->m);
      ctx->items.insert(ctx->items.end(), new_items.begin(), new_items.end());
      if(ctx->pending == 0) return;
      ctx->pending--;
      if(ctx->pending == 0){
        collected = ctx->items;
        cb = ctx->handler;
      }
    }
    if(cb) cb(collected);
  };

  for(const auto& peer : peers){
    auto success = request_peer_listing(peer, "", hash, "",
      [notify](const std::vector<RemoteListingItem>& items){
        notify(items);
      });

    if(!success){
      notify({});
    }
  }

  return true;
}

void PeerManager::handle_list_response(const std::string& peer_id,
                                       const std::string& request_id,
                                       const std::vector<RemoteListingItem>& items){
  ListResponseHandler handler;
  {
    std::lock_guard lg(m_);
    auto it = pending_list_requests_.find(request_id);
    if(it == pending_list_requests_.end()) return;
    handler = std::move(it->second);
    pending_list_requests_.erase(it);
  }

  if(!handler) return;
  auto decorated = items;
  for(auto& item : decorated){
    if(item.peer_id.empty()) item.peer_id = peer_id;
  }
  handler(decorated);
}

void PeerManager::handle_list_request(const std::string& peer_id,
                                      const std::string& request_id,
                                      const std::string& relative_path,
                                      const std::string& hash,
                                      const std::string& dir_guid){
  nlohmann::json resp;
  resp["type"] = "list_response";
  resp["request_id"] = request_id;
  resp["peer_id"] = local_peer_id_;

  std::function<void()> refresh_cb;
  {
    std::lock_guard lg(m_);
    refresh_cb = listing_refresh_callback_;
  }
  if(refresh_cb) {
    try {
      refresh_cb();
    } catch(const std::exception& e) {
      log_warn("Listing refresh callback threw: {}", e.what());
    } catch(...) {
      log_warn("Listing refresh callback threw unknown exception");
    }
  }

  std::vector<RemoteListingItem> items;
  if(!hash.empty()){
    auto entries = local_listing_for_hash(hash);
    for(const auto& entry : entries){
      RemoteListingItem item;
      item.peer_id = local_peer_id_;
      item.hash = entry.hash;
      item.relative_path = entry.relative_path;
      item.size = entry.size;
      item.is_directory = false;
      items.push_back(std::move(item));
    }
  } else if(!dir_guid.empty()){
    items = local_listing_for_dir_guid(dir_guid);
  } else {
    items = local_listing_items_for_path(relative_path);
  }

  nlohmann::json arr = nlohmann::json::array();
  for(const auto& entry : items){
    nlohmann::json item;
    item["hash"] = entry.hash;
    item["path"] = entry.relative_path;
    item["size"] = entry.size;
    item["is_dir"] = entry.is_directory;
    if(!entry.directory_guid.empty()) item["dir_guid"] = entry.directory_guid;
    arr.push_back(item);
  }
  resp["entries"] = arr;
  send_json_to_peer(peer_id, resp);
}

std::optional<std::string> PeerManager::request_file_chunk(const std::string& peer_id,
                                                          const std::string& hash,
                                                          uint64_t offset,
                                                          std::size_t length,
                                                          ChunkResponseHandler handler){
  if(!handler) return std::nullopt;

  std::string request_id;
  {
    std::lock_guard lg(m_);
    auto it = connections_.find(peer_id);
    if(it == connections_.end()) return std::nullopt;
    request_id = local_peer_id_ + "-chunk-" + std::to_string(++chunk_request_counter_);
    pending_chunk_requests_[request_id] = handler;
  }

  nlohmann::json req;
  req["type"] = "file_chunk_request";
  req["request_id"] = request_id;
  req["hash"] = hash;
  req["offset"] = offset;
  req["length"] = length;
  if(send_json_to_peer(peer_id, req)){
    return request_id;
  }

  cancel_chunk_request(request_id);
  return std::nullopt;
}

bool PeerManager::cancel_chunk_request(const std::string& request_id){
  std::lock_guard lg(m_);
  return pending_chunk_requests_.erase(request_id) > 0;
}

void PeerManager::handle_file_chunk_response(const std::string& peer_id,
                                             const std::string& request_id,
                                             ChunkResponse response){
  ChunkResponseHandler handler;
  {
    std::lock_guard lg(m_);
    auto it = pending_chunk_requests_.find(request_id);
    if(it == pending_chunk_requests_.end()) return;
    handler = std::move(it->second);
    pending_chunk_requests_.erase(it);
  }

  if(!handler) return;
  if(response.peer_id.empty()) response.peer_id = peer_id;
  handler(response);
}
