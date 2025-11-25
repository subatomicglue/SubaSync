#pragma once
#include <asio.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <atomic>
#include <optional>
#include <mutex>
#include <set>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include "protocol.hpp"
#include "log.hpp"

struct PeerInfo {
  std::string peer_id;
  std::string display_name;    // optional user-defined
  std::string local_addr;      // LAN IP + port
  std::string external_addr;   // public IP + port (optional)
  enum class State { Disconnected, Connecting, Connected } state = State::Disconnected;

  std::string best_addr(const std::string& local_subnet="") const {
    // prefer local address if same subnet, otherwise external
    if(!local_addr.empty() && !local_subnet.empty() &&
      local_addr.compare(0, local_subnet.size(), local_subnet) == 0)
      return local_addr;
    if(!external_addr.empty())
      return external_addr;
    return local_addr; // fallback
  }
};


class Connection; // forward

class PeerManager : public std::enable_shared_from_this<PeerManager> {
public:
    using ConnectCallback = std::function<void(const std::string& /*peer_id*/, std::shared_ptr<Connection>)>;

    struct SharedFileEntry {
      std::string hash;
      std::string relative_path; // relative to share/ root
      std::filesystem::path full_path;
      uint64_t size = 0;
    };

    struct RemoteListingItem {
      std::string peer_id;
      std::string hash;
      std::string relative_path;
      uint64_t size = 0;
      bool is_directory = false;
      std::string directory_guid;
      std::string origin_peer;
      std::string listing_root_path;
    };

    struct ChunkResponse {
      std::string peer_id;
      std::string hash;
      uint64_t offset = 0;
      std::vector<char> data;
      std::string chunk_sha;
      bool success = false;
      std::string error;
    };

    using ChunkResponseHandler = std::function<void(const ChunkResponse&)>;
    static constexpr std::size_t kMaxChunkSize = 256 * 1024;

    PeerManager(asio::io_context& io,
                std::string local_peer_id,
                std::string display_name,
                std::string local_addr,
                std::string external_addr,
                size_t max_peers,
                std::shared_ptr<Logger> logger = nullptr);


    // Add or update a discovered peer (e.g. from announce)
    void add_peer_discovered(const std::string& peer_id,
                         const std::string& display_name,
                         const std::string& local_addr,
                         const std::string& external_addr);

    // Called when a connection becomes ready (incoming or outgoing)
    void on_connected(const std::string& peer_id, std::shared_ptr<Connection> conn);

    // Called when a connection closes
    void on_disconnected(const std::string& peer_id);

    // Send a JSON to all connected peers except optional exclude_peer
    void broadcast_json(const nlohmann::json& j, const std::string& exclude_peer_id = "");

    // Return a JSON array of known peers (peer_id + addr)
    nlohmann::json make_peer_list_json();

    // Attempt connections until we reach max_peers
    void attempt_connect_more();

    asio::io_context& io() { return io_; }
    std::string local_peer_id() const { return local_peer_id_; }
    std::string local_addr() const { return local_addr_; }
    std::string display_name() const { return display_name_; }
    std::string external_addr() const { return external_addr_; }
    std::shared_ptr<Logger> logger() const { return logger_; }

    void register_local_share(const std::string& hash,
                              const std::string& relative_path,
                              const std::filesystem::path& absolute_path,
                              uint64_t size);

    void register_directory(const std::string& relative_path);
  void register_directory_with_guid(const std::string& relative_path,
                                    const std::string& guid);
  void unregister_directory_guid(const std::string& guid);

    bool find_local_file_by_hash(const std::string& hash, SharedFileEntry& out_entry);
    bool find_local_file_by_path(const std::string& relative_path,
                                 SharedFileEntry& out_entry) const;

    std::vector<RemoteListingItem> local_listing_items_for_path(const std::string& relative_path);
    std::vector<RemoteListingItem> local_listing_for_dir_guid(const std::string& guid);
    std::vector<SharedFileEntry> local_listing_for_hash(const std::string& hash);
    std::optional<std::string> directory_guid_for_path(const std::string& relative_path) const;

    using ListResponseHandler = std::function<void(const std::vector<RemoteListingItem>&)>;

    bool request_peer_listing(const std::string& peer_id,
                              const std::string& relative_path,
                              const std::string& hash,
                              const std::string& dir_guid,
                              ListResponseHandler handler);

    bool request_hash_listing_all(const std::string& hash,
                                  std::function<void(const std::vector<RemoteListingItem>&)> handler);

    void handle_list_response(const std::string& peer_id,
                              const std::string& request_id,
                              const std::vector<RemoteListingItem>& items);

    void set_listing_refresh_callback(std::function<void()> cb);

    void handle_list_request(const std::string& peer_id,
                             const std::string& request_id,
                             const std::string& relative_path,
                             const std::string& hash,
                             const std::string& dir_guid);

    bool send_json_to_peer(const std::string& peer_id, const nlohmann::json& j);
    void handle_message(std::shared_ptr<Connection> conn, const nlohmann::json& message);

    std::optional<std::string> request_file_chunk(const std::string& peer_id,
                                                  const std::string& hash,
                                                  uint64_t offset,
                                                  std::size_t length,
                                                  ChunkResponseHandler handler);

    bool cancel_chunk_request(const std::string& request_id);

    void handle_file_chunk_response(const std::string& peer_id,
                                    const std::string& request_id,
                                    ChunkResponse response);

    void set_chat_callback(std::function<void(const std::string&, const std::string&)> cb);
    void dispatch_chat_message(const std::string& from_peer,
                               const std::string& text);

    void set_directory_origin(const std::string& guid, const std::string& origin_peer);
    std::optional<std::string> directory_origin(const std::string& guid) const;

    void set_transfer_debug(bool enabled);
    bool transfer_debug_enabled() const;

    std::size_t known_peer_count() const;
    std::size_t connected_peer_count() const;

private:
    asio::io_context& io_;
    std::string local_peer_id_;
    std::string local_addr_;
    size_t max_peers_;

    mutable std::mutex m_;
    std::unordered_map<std::string, PeerInfo> peers_; // discovered peers
    std::unordered_map<std::string, std::shared_ptr<Connection>> connections_; // peer_id -> Connection
    std::set<std::string> in_progress_connects_; // peer_id being connected

    std::string display_name_; 
    std::string external_addr_;

    std::unordered_map<std::string, SharedFileEntry> shared_files_by_hash_;
    std::unordered_map<std::string, std::string> path_to_hash_;
    std::unordered_map<std::string, std::string> path_to_dir_guid_;
    std::unordered_map<std::string, std::string> dir_guid_to_path_;
    std::unordered_map<std::string, std::string> dir_guid_origin_;

    std::unordered_map<std::string, ListResponseHandler> pending_list_requests_;
    std::atomic<uint64_t> list_request_counter_{0};
    std::unordered_map<std::string, ChunkResponseHandler> pending_chunk_requests_;
    std::atomic<uint64_t> chunk_request_counter_{0};

    std::function<void(const std::string&, const std::string&)> chat_callback_;
    std::shared_ptr<Logger> logger_;
    mutable std::mutex chat_callback_mutex_;
    std::function<void()> listing_refresh_callback_;
    std::atomic<bool> transfer_debug_{false};

    std::string ensure_directory_guid(const std::string& relative_path);
    std::optional<std::string> resolve_directory_guid(const std::string& guid) const;
    void register_directory_hierarchy(const std::string& relative_path);
    std::string generate_directory_guid();
};
