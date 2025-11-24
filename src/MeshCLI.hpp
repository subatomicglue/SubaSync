#pragma once
#include <thread>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>
#include <future>
#include <optional>
#include <iomanip>
#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <map>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <random>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#else
#if !defined(_WIN32)
#include <termios.h>
#include <unistd.h>
#endif
#endif

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "peer_manager.hpp"
#include "utils.hpp"
#include "settings_manager.hpp"
#include "swarm_downloader.hpp"

class MeshCLI {
public:
  explicit MeshCLI(std::shared_ptr<PeerManager> pm,
                   std::shared_ptr<SettingsManager> settings,
                   std::chrono::seconds default_watch_interval = std::chrono::seconds(60),
                   bool audio_notifications = false)
    : pm_(std::move(pm)), settings_(std::move(settings)), running_(true),
      default_watch_interval_(default_watch_interval), audio_notifications_(audio_notifications) {
    ensure_directories();
    load_dir_guid_registry();
    apply_dir_guid_registry();
    index_share_root();
    load_ignore_config();
    apply_setting_side_effects("audio_notifications");
    apply_setting_side_effects("transfer_debug");
    apply_setting_side_effects("transfer_progress");
    apply_setting_side_effects("progress_meter_size");
    apply_setting_side_effects("swarm_max_parallel");
    apply_setting_side_effects("swarm_chunk_buffers");
    apply_setting_side_effects("swarm_chunk_size");
    apply_setting_side_effects("swarm_progress_interval_ms");
    pm_->set_listing_refresh_callback([this](){
      index_share_root();
    });
    load_watches();
    start_watch_thread();
    pm_->set_chat_callback([this](const std::string& from, const std::string& text){
      on_chat_message(from, text);
    });
  }

  ~MeshCLI() {
    pm_->set_chat_callback(nullptr);
    pm_->set_listing_refresh_callback(nullptr);
    save_dir_guid_registry();
  }

  void start() {
    cli_thread_ = std::thread([this](){ run_loop(); });
  }

  void stop() {
    running_ = false;
    stop_watch_thread();
    if(cli_thread_.joinable()) cli_thread_.join();
  }

private:
  struct ListCommand {
    enum class Type {
      LocalPath,
      RemotePath,
      RemoteHash,
      HashAll,
      DirectoryGuid,
      RemoteDirectoryGuid
    };
    Type type = Type::LocalPath;
    std::string peer;
    std::string path;
    std::string hash;
    std::string dir_guid;
  };

  std::shared_ptr<PeerManager> pm_;
  std::shared_ptr<SettingsManager> settings_;
  std::atomic<bool> running_;
  std::thread cli_thread_;
  std::chrono::seconds default_watch_interval_;
  bool audio_notifications_ = false;
  bool transfer_progress_enabled_ = true;
  std::size_t progress_meter_size_ = 80;
  std::size_t swarm_max_parallel_ = 0;
  std::size_t swarm_chunk_buffers_ = 1;
  std::size_t swarm_chunk_size_ = PeerManager::kMaxChunkSize;
  std::chrono::milliseconds swarm_progress_interval_{200};
  std::chrono::milliseconds swarm_provider_refresh_interval_{2000};
  std::chrono::milliseconds chunk_timeout_{10000};

  struct WatchEntry {
    std::string id;
    struct Source {
      std::string peer;
      std::string path;
    };
    std::vector<Source> sources;
    std::string dir_guid;
    std::string origin_peer;
    std::string dest_path;
    bool dest_is_directory = true;
    std::chrono::seconds interval{60};
    std::chrono::steady_clock::time_point next_run{};
  };

  struct ResourceIdentifier {
    enum class Kind {
      Listing,
      Watch
    };
    Kind kind = Kind::Listing;
    ListCommand list{};
    WatchEntry watch{};
  };

  struct RemovalStats {
    std::size_t deleted = 0;
    std::size_t missing = 0;
    std::size_t failed = 0;
  };

  struct SyncOutcome {
    bool success = true;
    bool changed = false;
  };

  struct ResolvedDirGuid {
    std::string guid;
    std::string origin_peer;
  };

  struct WatchSourceResolution {
    WatchEntry::Source source;
    ResolvedDirGuid resolved;
  };

  struct ConflictEntry {
    std::string id;
    std::string relative_path;
    std::string hash;
    std::string dir_guid;
    std::string origin_peer;
    std::chrono::system_clock::time_point detected_at{};
  };

  struct IgnoreConfig {
    std::set<std::string> paths;
    std::set<std::string> hashes;
    std::set<std::string> dir_guids;
  };

  enum class IgnoreKind {
    Path,
    Hash,
    DirGuid
  };

  struct StagedConflict {
    std::string conflict_id;
    std::string relative_path;
    std::string hash;
    std::string dir_guid;
    std::filesystem::path staged_path;
  };

  std::mutex watch_mutex_;
  std::condition_variable watch_cv_;
  std::thread watch_thread_;
  bool watch_thread_running_ = false;
  std::vector<WatchEntry> watches_;
  std::mutex share_index_mutex_;
  std::mutex conflict_mutex_;
  mutable std::mutex ignore_mutex_;
  std::vector<ConflictEntry> conflicts_;
  IgnoreConfig ignore_config_;
  std::optional<StagedConflict> staged_conflict_;

  struct DirGuidRecord {
    std::string guid;
    std::string path;
    bool active = false;
  };

  std::unordered_map<std::string, DirGuidRecord> dir_guid_records_;
  std::unordered_map<std::string, std::string> dir_guid_by_path_;
  bool dir_guid_registry_dirty_ = false;
  mutable std::mutex dir_guid_mutex_;

  enum class PeerListingState { NotConnected, Timeout, Completed };
  struct PeerListingResult {
    PeerListingState state = PeerListingState::NotConnected;
    std::vector<PeerManager::RemoteListingItem> items;
  };

  enum class SourceReachability { Online, Timeout, Missing, Offline };

#if !defined(HAVE_READLINE)
  std::vector<std::string> cli_history_;
  static constexpr std::size_t history_limit_ = 200;
#endif

  void run_loop() {
    while(running_) {
      auto input = read_command_line("> ");
      if(!input) break;
      if(input->empty()) continue;

      std::istringstream iss(*input);
      std::string cmd;
      iss >> cmd;

      if(cmd == "send") {
        std::string msg;
        std::getline(iss, msg);
        if(!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
        send_chat(msg);
      } else if(cmd == "peers") {
        list_peers();
      } else if(cmd == "list" || cmd == "ls" || cmd == "l") {
        std::string arg;
        std::getline(iss, arg);
        trim(arg);
        handle_list(arg);
      } else if(cmd == "la") {
        std::string extras;
        std::getline(iss, extras);
        trim(extras);
        if(!extras.empty()) {
          handle_list("all " + extras);
        } else {
          handle_list("all");
        }
      } else if(cmd == "share") {
        std::string path;
        std::getline(iss, path);
        trim(path);
        share_command(path);
      } else if(cmd == "sync") {
        std::string target;
        std::getline(iss, target);
        trim(target);
        sync_command(target);
      } else if(cmd == "watch") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_watch_command(args);
      } else if(cmd == "unwatch") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        std::string forwarded = "remove";
        if(!args.empty()) {
          forwarded += " ";
          forwarded += args;
        }
        handle_watch_command(forwarded);
      } else if(cmd == "bell") {
        std::cout << '\a';
        std::cout.flush();
      } else if(cmd == "settings" || cmd == "s") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        if(args.empty()) args = "list";
        handle_settings_command(args);
      } else if(cmd == "set") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        if(args.empty()) {
          handle_settings_command("list");
        } else {
          handle_settings_command("set " + args);
        }
      } else if(cmd == "get") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_settings_command(args.empty() ? "get" : "get " + args);
      } else if(cmd == "save") {
        handle_settings_command("save");
      } else if(cmd == "load") {
        handle_settings_command("load");
      } else if(cmd == "ignore") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_ignore_command(args);
      } else if(cmd == "guid") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_guid_command(args);
      } else if(cmd == "conflict") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_conflict_command(args);
      } else if(cmd == "help" || cmd == "h" || cmd == "?") {
        print_help();
      } else if(cmd == "quit") {
        std::cout << "Quitting...\n";
        exit(-1);
      } else if(cmd == "dirs") {
        print_dirs();
      } else if(cmd == "pwd" || cmd == "cwd") {
        print_current_directory();
      } else {
        print_help();
        std::cout << "Unknown command: " << cmd << "\n";
      }
    }
  }

  std::filesystem::path dir_guid_registry_path() const {
    return config_root() / "dir-guids.json";
  }

  void send_chat(const std::string& msg) {
    if(msg.empty()) return;
    nlohmann::json j;
    j["type"] = "chat";
    j["from"] = pm_->local_peer_id();
    j["text"] = msg;
    pm_->broadcast_json(j);
    std::cout << "[you] " << msg << "\n";
  }

  void on_chat_message(const std::string& from, const std::string& text) {
    if(audio_notifications_) {
      std::cout << '\a';
    }
    std::cout << "[" << from << "] " << text << "\n> ";
    std::cout.flush();
  }

  std::optional<std::string> read_command_line(const char* prompt) {
#ifdef HAVE_READLINE
    char* line = readline(prompt);
    if(!line) return std::nullopt;
    std::string result(line);
    if(!result.empty()) add_history(result.c_str());
    free(line);
    return result;
#else
    return read_command_line_fallback(prompt);
#endif
  }

#ifndef HAVE_READLINE
  void append_history_entry(const std::string& line) {
    if(line.empty()) return;
    if(!cli_history_.empty() && cli_history_.back() == line) return;
    cli_history_.push_back(line);
    if(cli_history_.size() > history_limit_) {
      cli_history_.erase(cli_history_.begin());
    }
  }

  std::optional<std::string> read_command_line_simple(const char* prompt) {
    std::cout << prompt;
    std::cout.flush();
    std::string line;
    if(!std::getline(std::cin, line)) return std::nullopt;
    append_history_entry(line);
    return line;
  }

#if !defined(_WIN32)
  struct TerminalModeGuard {
    bool active = false;
    termios original{};

    bool activate() {
      if(!isatty(STDIN_FILENO)) return false;
      if(tcgetattr(STDIN_FILENO, &original) == -1) return false;
      termios raw = original;
      raw.c_lflag &= ~(ICANON | ECHO);
      raw.c_cc[VMIN] = 1;
      raw.c_cc[VTIME] = 0;
      if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return false;
      active = true;
      return true;
    }

    ~TerminalModeGuard() {
      if(active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
      }
    }
  };

  void refresh_line(const char* prompt,
                    const std::string& buffer,
                    std::size_t cursor) {
    const char* safe_prompt = prompt ? prompt : "";
    std::cout << "\r" << safe_prompt << buffer << "\x1b[K";
    std::size_t displayed = std::strlen(safe_prompt) + buffer.size();
    std::size_t target = std::strlen(safe_prompt) + cursor;
    if(displayed > target) {
      std::cout << "\x1b[" << (displayed - target) << "D";
    }
    std::cout.flush();
  }

  std::optional<std::string> read_command_line_fallback(const char* prompt) {
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
      return read_command_line_simple(prompt);
    }

    TerminalModeGuard guard;
    if(!guard.activate()) {
      return read_command_line_simple(prompt);
    }

    std::string buffer;
    std::string saved_line;
    bool browsing_history = false;
    std::size_t history_index = cli_history_.size();
    std::size_t cursor = 0;
    const char* safe_prompt = prompt ? prompt : "";
    std::cout << safe_prompt;
    std::cout.flush();

    while(true) {
      char ch = 0;
      ssize_t read_result = ::read(STDIN_FILENO, &ch, 1);
      if(read_result <= 0) {
        return std::nullopt;
      }
      if(ch == '\r' || ch == '\n') {
        std::cout << "\n";
        break;
      }
      if(ch == 4) {
        if(buffer.empty()) {
          return std::nullopt;
        }
        continue;
      }
      if(ch == 1) {
        cursor = 0;
        refresh_line(safe_prompt, buffer, cursor);
        continue;
      }
      if(ch == 5) {
        cursor = buffer.size();
        refresh_line(safe_prompt, buffer, cursor);
        continue;
      }
      if(ch == 127 || ch == 8) {
        if(cursor > 0) {
          buffer.erase(cursor - 1, 1);
          --cursor;
          refresh_line(safe_prompt, buffer, cursor);
        }
        continue;
      }
      if(ch == '\x1b') {
        char seq[2] = {0, 0};
        if(::read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
        if(seq[0] == '[') {
          if(::read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
          if(seq[1] == 'A') {
            if(cli_history_.empty()) continue;
            if(!browsing_history) {
              saved_line = buffer;
              browsing_history = true;
            }
            if(history_index == 0) continue;
            --history_index;
            buffer = cli_history_[history_index];
            cursor = buffer.size();
            refresh_line(safe_prompt, buffer, cursor);
          } else if(seq[1] == 'B') {
            if(!browsing_history) continue;
            if(history_index + 1 >= cli_history_.size()) {
              history_index = cli_history_.size();
              buffer = saved_line;
              cursor = buffer.size();
              browsing_history = false;
              refresh_line(safe_prompt, buffer, cursor);
            } else {
              ++history_index;
              buffer = cli_history_[history_index];
              cursor = buffer.size();
              refresh_line(safe_prompt, buffer, cursor);
            }
          } else if(seq[1] == 'C') {
            if(cursor < buffer.size()) {
              ++cursor;
              refresh_line(safe_prompt, buffer, cursor);
            }
          } else if(seq[1] == 'D') {
            if(cursor > 0) {
              --cursor;
              refresh_line(safe_prompt, buffer, cursor);
            }
          } else if(seq[1] == 'H') {
            cursor = 0;
            refresh_line(safe_prompt, buffer, cursor);
          } else if(seq[1] == 'F') {
            cursor = buffer.size();
            refresh_line(safe_prompt, buffer, cursor);
          } else if(seq[1] == '3') {
            char tilde = 0;
            if(::read(STDIN_FILENO, &tilde, 1) <= 0) continue;
            if(tilde == '~' && cursor < buffer.size()) {
              buffer.erase(cursor, 1);
              refresh_line(safe_prompt, buffer, cursor);
            }
          }
        }
        continue;
      }
      if(std::isprint(static_cast<unsigned char>(ch))) {
        buffer.insert(buffer.begin() + cursor, ch);
        ++cursor;
        refresh_line(safe_prompt, buffer, cursor);
      }
    }

    append_history_entry(buffer);
    return buffer;
  }
#else
  std::optional<std::string> read_command_line_fallback(const char* prompt) {
    return read_command_line_simple(prompt);
  }
#endif
#endif

  void list_peers() {
    auto arr = pm_->make_peer_list_json();
    const std::string local_id = pm_->local_peer_id();

    for(const auto& p : arr) {
      const std::string peer_id = p["peer_id"].get<std::string>();
      const std::string& name = p["display_name"].get_ref<const std::string&>();
      std::cout << peer_id << " (" << name << ")";
      if(peer_id == local_id) std::cout << " [you]";
      std::cout << "\n";
    }
  }

  void handle_list(const std::string& arg) {
    if(arg == "all") {
      auto peers = pm_->make_peer_list_json();
      if(peers.empty()) {
        std::cout << "No peers available.\n";
        return;
      }

      bool first = true;
      for(const auto& peer_entry : peers) {
        if(!first) std::cout << "\n";
        first = false;

        const std::string peer_id = peer_entry["peer_id"].get<std::string>();
        handle_list_command(peer_id);
      }
      return;
    }

    handle_list_command(arg);
  }

  void handle_list_command(const std::string& arg) {
    bool show_peers = arg.empty();
    auto parsed = parse_list_command(arg);
    if(!parsed) {
      std::cout << "Invalid list target.\n";
      return;
    }

    if(show_peers) {
      std::cout << "Peers:\n";
      list_peers();
      std::cout << "\n";
    }

    const std::string local_peer = pm_->local_peer_id();

    switch(parsed->type) {
      case ListCommand::Type::LocalPath:
        std::cout << "Local listing (" << local_peer << ")\n";
        list_local(parsed->path);
        break;
      case ListCommand::Type::RemotePath:
        if(parsed->peer == local_peer) {
          std::cout << "Local listing (" << local_peer << ")\n";
          list_local(parsed->path);
        } else {
          std::cout << "Remote listing (" << parsed->peer << ")\n";
          list_remote_path(parsed->peer, parsed->path, "");
        }
        break;
      case ListCommand::Type::RemoteHash:
        if(parsed->peer == local_peer) {
          std::cout << "Local listing (" << local_peer << ")\n";
          auto entries = pm_->local_listing_for_hash(parsed->hash);
          auto items = make_remote_listing_items(entries);
          if(items.empty()) {
            std::cout << "Hash not found on " << local_peer << ".\n";
          } else {
            print_listing(items);
          }
        } else {
          std::cout << "Remote hash listing (" << parsed->peer << ")\n";
          list_remote_hash(parsed->peer, parsed->hash);
        }
        break;
      case ListCommand::Type::HashAll:
        std::cout << "Mesh hash listing (" << parsed->hash << ")\n";
        list_all_hash(parsed->hash);
        break;
      case ListCommand::Type::DirectoryGuid:
        std::cout << "Local directory listing (" << local_peer << ")\n";
        list_local_dir_guid(parsed->dir_guid);
        break;
      case ListCommand::Type::RemoteDirectoryGuid:
        if(parsed->peer == local_peer) {
          std::cout << "Local directory listing (" << local_peer << ")\n";
          list_local_dir_guid(parsed->dir_guid);
        } else {
          std::cout << "Remote directory listing (" << parsed->peer << ")\n";
          list_remote_path(parsed->peer, "", parsed->dir_guid);
        }
        break;
    }
  }

  void list_local(const std::string& cli_path) {
    auto orphaned = index_share_root();
    auto normalized = normalize_cli_path(cli_path);
    if(is_internal_listing_path(normalized)) {
      std::cout << "Path is reserved for staging.\n";
      return;
    }
    auto items = pm_->local_listing_items_for_path(normalized);
    if(items.empty()) {
      std::cout << "Listing is empty.\n";
      return;
    }
    maybe_suggest_guid_mapping(items);
    print_orphan_hint(orphaned);
    print_listing(items, true);  // true = show peer prefix;  false dont show peer prefix
  }

  void list_local_dir_guid(const std::string& guid) {
    auto orphaned = index_share_root();
    auto items = pm_->local_listing_for_dir_guid(guid);
    if(items.empty()) {
      std::cout << "Directory not found for GUID " << guid << "\n";
      return;
    }
    maybe_suggest_guid_mapping(items);
    print_orphan_hint(orphaned);
    print_listing(items, true);  // true = show peer prefix;  false dont show peer prefix
  }

  void list_remote_path(const std::string& peer, const std::string& cli_path, const std::string& dir_guid) {
    auto normalized = normalize_cli_path(cli_path);
    if(dir_guid.empty() && !normalized.empty() && is_internal_listing_path(normalized)) {
      std::cout << "Path is reserved on " << peer << ".\n";
      return;
    }
    auto items = blocking_list_peer(peer, normalized, "", dir_guid);
    if(items.empty()) {
      std::cout << "No entries from " << peer << " for path.\n";
      return;
    }
    print_listing(items);
  }

  void list_remote_hash(const std::string& peer, const std::string& hash) {
    auto items = blocking_list_peer(peer, "", hash, "");
    if(items.empty()) {
      std::cout << "Hash not found on " << peer << ".\n";
      return;
    }
    print_listing(items);
  }

  void list_all_hash(const std::string& hash) {
    auto items = blocking_list_hash(hash);
    if(items.empty()) {
      std::cout << "Hash not found on any peer.\n";
      return;
    }
    print_listing(items);
  }

  void share_command(const std::string& raw_path) {
    auto resolved = resolve_share_path(raw_path);
    std::error_code ec;
    if(!std::filesystem::exists(resolved, ec)) {
      std::cout << "File or directory not found: " << resolved << "\n";
      return;
    }

    size_t shared_count = 0;
    if(std::filesystem::is_regular_file(resolved, ec)) {
      shared_count += share_single_file(resolved, true);
    } else if(std::filesystem::is_directory(resolved, ec)) {
      for(auto it = std::filesystem::recursive_directory_iterator(resolved, ec);
          !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if(it->is_regular_file()) {
          shared_count += share_single_file(it->path(), true);
        }
      }
      if(ec) {
        std::cout << "Error while traversing: " << ec.message() << "\n";
      }
    } else {
      std::cout << "Unsupported file type: " << resolved << "\n";
      return;
    }

    if(shared_count == 0) {
      std::cout << "No files were shared.\n";
    }
  }

  void sync_command(const std::string& target) {
    sync_command_impl(target, true);
  }

  void sync_command_impl(const std::string& target, bool allow_force) {
    std::string trimmed = target;
    trim(trimmed);

    if(allow_force) {
      std::istringstream prefix(trimmed);
      std::string maybe_force;
      prefix >> maybe_force;
      if(maybe_force == "force") {
        std::string remainder;
        std::getline(prefix, remainder);
        trim(remainder);
        if(remainder.empty()) {
          std::cout << "Usage: sync force <resource>\n";
        } else {
          sync_force_command(remainder);
        }
        return;
      }
    }

    if(trimmed.empty()) {
      std::size_t bumped = 0;
      {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        auto now = std::chrono::steady_clock::now();
        for(auto& entry : watches_) {
          entry.next_run = now;
          ++bumped;
        }
      }
      if(bumped == 0) {
        std::cout << "No watches configured.\n";
      } else {
        watch_cv_.notify_all();
        std::cout << "Triggered " << bumped << " watch" << (bumped == 1 ? "" : "es") << " for immediate sync.\n";
      }
      return;
    }

    std::string source_arg;
    std::string dest_raw;
    {
      std::istringstream iss(trimmed);
      iss >> source_arg;
      std::getline(iss, dest_raw);
    }
    trim(dest_raw);
    bool dest_is_directory = !dest_raw.empty() && dest_raw.back() == '/';
    if(dest_is_directory) {
      dest_raw.pop_back();
    }
    trim(dest_raw);

    std::optional<std::string> dest_override;
    if(!dest_raw.empty()) {
      dest_override = normalize_cli_path(dest_raw);
    }
    if(dest_override && is_internal_listing_path(*dest_override)) {
      std::cout << "Destination path is reserved for staging.\n";
      return;
    }

    auto resolved = resolve_resource_identifier(source_arg);
    if(!resolved) {
      std::cout << "Invalid sync target.\n";
      return;
    }

    if(resolved->kind == ResourceIdentifier::Kind::Watch) {
      if(dest_override) {
        std::cout << "Destination overrides are not supported when syncing a watch.\n";
        return;
      }
      auto result = run_single_watch(resolved->watch);
      if(!result.success) {
        std::cout << "[watch " << resolved->watch.id << "] sync failed.\n";
      } else if(!result.changed) {
        std::cout << "[watch " << resolved->watch.id << "] already up to date.\n";
      } else {
        std::cout << "[watch " << resolved->watch.id << "] synced.\n";
      }
      return;
    }

    auto parsed = resolved->list;

    if((parsed.type == ListCommand::Type::RemotePath ||
        parsed.type == ListCommand::Type::RemoteHash ||
        parsed.type == ListCommand::Type::RemoteDirectoryGuid) &&
        parsed.peer == pm_->local_peer_id()) {
      std::cout << "Cannot sync from self.\n";
      std::cout << "Nothing to sync.\n";
      return;
    }

    std::vector<PeerManager::RemoteListingItem> entries;
    switch(parsed.type) {
      case ListCommand::Type::RemotePath:
        if(is_internal_listing_path(parsed.path)) {
          std::cout << "Path is reserved on " << parsed.peer << ".\n";
          return;
        }
        entries = blocking_list_peer(parsed.peer, parsed.path, "", "");
        break;
      case ListCommand::Type::RemoteHash:
        entries = blocking_list_peer(parsed.peer, "", parsed.hash, "");
        break;
      case ListCommand::Type::HashAll:
        entries = blocking_list_hash(parsed.hash);
        break;
      case ListCommand::Type::DirectoryGuid:
        entries = pm_->local_listing_for_dir_guid(parsed.dir_guid);
        break;
      case ListCommand::Type::RemoteDirectoryGuid:
        entries = blocking_list_peer(parsed.peer, "", "", parsed.dir_guid);
        break;
      case ListCommand::Type::LocalPath:
        std::cout << "Sync expects a peer or hash target.\n";
        return;
    }

    entries.erase(std::remove_if(entries.begin(), entries.end(),
      [](const PeerManager::RemoteListingItem& item){
        return is_internal_listing_path(item.relative_path);
      }), entries.end());

    notify_untrusted_directory_sources(entries);

    if(entries.empty()) {
      std::cout << "Nothing to sync.\n";
      return;
    }

    auto result = perform_sync(parsed, std::move(entries), dest_override, dest_is_directory, false, nullptr);
    if(!result.success) {
      std::cout << "Sync failed.\n";
    } else if(!result.changed) {
      std::cout << "Already up to date.\n";
    }
  }

  void sync_force_command(const std::string& resource_spec) {
    std::string trimmed = resource_spec;
    trim(trimmed);
    if(trimmed.empty()) {
      std::cout << "Usage: sync force <resource>\n";
      return;
    }

    auto resolved = resolve_resource_identifier(trimmed);
    if(!resolved) {
      std::cout << "Invalid resource identifier.\n";
      return;
    }

    std::vector<std::string> warnings;
    auto targets = resource_local_targets(*resolved, &warnings);
    for(const auto& warning : warnings) {
      std::cout << warning << "\n";
    }

    if(targets.empty()) {
      std::cout << "No local data found for " << describe_resource(*resolved) << ".\n";
    } else {
      auto stats = purge_local_paths(targets);
      index_share_root();
      if(stats.deleted > 0 || stats.missing > 0 || stats.failed > 0) {
        std::cout << "force-sync removed " << stats.deleted << " target"
                  << (stats.deleted == 1 ? "" : "s")
                  << ", " << stats.missing << " already missing";
        if(stats.failed > 0) {
          std::cout << ", " << stats.failed << " failed";
        }
        std::cout << ".\n";
      }
    }

    sync_command_impl(trimmed, false);
  }

  std::optional<ResourceIdentifier> resolve_resource_identifier(const std::string& raw) {
    std::string input = raw;
    trim(input);
    if(input.empty()) return std::nullopt;

    if(auto watch = snapshot_watch_by_id(input)) {
      ResourceIdentifier id;
      id.kind = ResourceIdentifier::Kind::Watch;
      id.watch = *watch;
      return id;
    }

    auto parsed = parse_list_command(input);
    if(!parsed) return std::nullopt;

    ResourceIdentifier id;
    id.kind = ResourceIdentifier::Kind::Listing;
    id.list = *parsed;
    return id;
  }

  std::optional<WatchEntry> snapshot_watch_by_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    auto* entry = find_watch_by_id(id);
    if(!entry) return std::nullopt;
    return *entry;
  }

  std::string describe_resource(const ResourceIdentifier& resource) const {
    if(resource.kind == ResourceIdentifier::Kind::Watch) {
      return "watch " + resource.watch.id;
    }

    const auto& cmd = resource.list;
    switch(cmd.type) {
      case ListCommand::Type::LocalPath:
        return cmd.path.empty() ? "local share root" : ("/" + cmd.path);
      case ListCommand::Type::RemotePath:
        return cmd.peer + ":/" + cmd.path;
      case ListCommand::Type::RemoteHash:
        return cmd.peer + ":#" + cmd.hash.substr(0, 8);
      case ListCommand::Type::HashAll:
        return "hash " + cmd.hash.substr(0, 8);
      case ListCommand::Type::DirectoryGuid:
        return "dir-guid " + cmd.dir_guid;
      case ListCommand::Type::RemoteDirectoryGuid:
        return cmd.peer + ":/" + cmd.dir_guid;
      default:
        return "resource";
    }
  }

  std::vector<std::string> resource_local_targets(const ResourceIdentifier& resource,
                                                  std::vector<std::string>* warnings = nullptr) {
    auto normalize_candidate = [&](const std::string& raw) -> std::string {
      auto normalized = normalize_cli_path(raw);
      if(normalized.empty()) return {};
      if(is_internal_listing_path(normalized)) return {};
      return normalized;
    };

    auto add_guid_root = [&](const std::string& guid,
                             std::vector<std::string>& out){
      if(guid.empty()) return false;
      bool added = false;
      if(auto active = active_path_for_guid(guid)) {
        auto normalized = normalize_candidate(*active);
        if(!normalized.empty()) {
          out.push_back(std::move(normalized));
          added = true;
        }
      }
      if(!added) {
        auto items = pm_->local_listing_for_dir_guid(guid);
        std::unordered_set<std::string> roots;
        for(const auto& item : items) {
          if(item.relative_path.empty()) continue;
          std::string normalized = normalize_cli_path(item.relative_path);
          if(normalized.empty()) continue;
          auto slash = normalized.find('/');
          auto root = slash == std::string::npos ? normalized : normalized.substr(0, slash);
          auto cleaned = normalize_candidate(root);
          if(cleaned.empty()) continue;
          if(!roots.insert(cleaned).second) continue;
          out.push_back(cleaned);
          added = true;
        }
      }
      if(!added && warnings) {
        warnings->push_back("No local directory mapping found for GUID " + guid + ".");
      }
      return added;
    };

    std::vector<std::string> result;

    if(resource.kind == ResourceIdentifier::Kind::Watch) {
      bool added = false;
      if(auto binding = watch_binding_path_candidate(resource.watch)) {
        auto normalized = normalize_candidate(*binding);
        if(!normalized.empty()) {
          result.push_back(std::move(normalized));
          added = true;
        }
      }
      if(!resource.watch.dir_guid.empty()) {
        added = add_guid_root(resource.watch.dir_guid, result) || added;
      }
      if(!added && warnings) {
        warnings->push_back("Watch " + resource.watch.id + " has no local destination to clear.");
      }
    } else {
      const auto& cmd = resource.list;
      switch(cmd.type) {
        case ListCommand::Type::LocalPath:
          if(cmd.path.empty()) {
            if(warnings) warnings->push_back("Cannot force sync the entire local share root.");
          } else if(auto normalized = normalize_candidate(cmd.path); !normalized.empty()) {
            result.push_back(std::move(normalized));
          }
          break;
        case ListCommand::Type::RemotePath:
          if(cmd.path.empty()) {
            if(warnings) warnings->push_back("Specify a sub-path when forcing a remote sync.");
          } else if(auto normalized = normalize_candidate(cmd.path); !normalized.empty()) {
            result.push_back(std::move(normalized));
          }
          break;
        case ListCommand::Type::RemoteHash:
        case ListCommand::Type::HashAll: {
          auto entries = pm_->local_listing_for_hash(cmd.hash);
          if(entries.empty() && warnings) {
            warnings->push_back("No local files currently match hash " + cmd.hash.substr(0, 8) + ".");
          }
          for(const auto& entry : entries) {
            if(auto normalized = normalize_candidate(entry.relative_path); !normalized.empty()) {
              result.push_back(std::move(normalized));
            }
          }
          break;
        }
        case ListCommand::Type::DirectoryGuid:
        case ListCommand::Type::RemoteDirectoryGuid:
          add_guid_root(cmd.dir_guid, result);
          break;
      }
    }

    result.erase(std::remove_if(result.begin(), result.end(),
      [](const std::string& value){ return value.empty(); }), result.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }

  RemovalStats purge_local_paths(const std::vector<std::string>& rel_paths) {
    RemovalStats stats;
    std::unordered_set<std::string> unique(rel_paths.begin(), rel_paths.end());
    for(const auto& rel : unique) {
      if(rel.empty()) continue;
      auto target = share_root() / rel;
      std::error_code ec;
      bool exists = std::filesystem::exists(target, ec);
      if(ec) {
        ++stats.failed;
        std::cout << "Failed to inspect " << rel << ": " << ec.message() << "\n";
      } else if(!exists) {
        ++stats.missing;
      } else {
        auto removed = std::filesystem::remove_all(target, ec);
        if(ec) {
          ++stats.failed;
          std::cout << "Failed to remove " << rel << ": " << ec.message() << "\n";
        } else {
          ++stats.deleted;
          std::cout << "Removed " << rel << " (" << removed << " item" << (removed == 1 ? "" : "s") << ")\n";
        }
      }

      auto staging = download_root() / rel;
      std::filesystem::remove_all(staging, ec);
    }
    return stats;
  }

  SyncOutcome perform_sync(const ListCommand& cmd,
                           std::vector<PeerManager::RemoteListingItem> entries,
                           const std::optional<std::string>& dest_override,
                           bool dest_is_directory,
                           bool quiet = false,
                           const WatchEntry* watch_context = nullptr) {
    SyncOutcome outcome;
    std::filesystem::path dest_base = dest_override ? std::filesystem::path(*dest_override) : std::filesystem::path();
    dest_base = sanitize_relative(dest_base);

    entries.erase(std::remove_if(entries.begin(), entries.end(),
      [](const PeerManager::RemoteListingItem& item){
        return item.is_directory && item.relative_path.empty();
      }), entries.end());

    if(watch_context) {
      std::string guid_hint = (cmd.type == ListCommand::Type::RemoteDirectoryGuid) ? cmd.dir_guid : "";
      maybe_warn_duplicate_hashes_for_guid(watch_context, guid_hint, entries);
    }

    // Handle directories first so that child files have parents prepared.
    for(const auto& item : entries) {
      if(!item.is_directory) continue;
      if(item.peer_id.empty()) continue;
      std::filesystem::path dir_dest = dest_base / std::filesystem::path(item.relative_path);
      if(dest_override && dest_override->empty()) {
        dir_dest = std::filesystem::path(item.relative_path);
      }
      if(!dest_override) {
        dir_dest = std::filesystem::path(item.relative_path);
      }
      dir_dest = sanitize_relative(dir_dest);
      auto dir_string = dir_dest.generic_string();
      if(!dir_string.empty()) {
        ensure_directory_exists(download_root() / dir_dest);
        ensure_directory_exists(share_root() / dir_dest);
      }
        if(!dir_string.empty()) {
          if(!item.directory_guid.empty()) {
            remember_dir_guid_mapping(dir_string, item.directory_guid);
          } else {
            pm_->register_directory(dir_string);
          }
        }
      auto dir_result = sync_directory(item.peer_id,
                                       item.relative_path,
                                       item.directory_guid,
                                       dir_dest,
                                       quiet,
                                       watch_context);
      if(!item.directory_guid.empty()) {
        std::string origin_source = !item.origin_peer.empty()
          ? item.origin_peer
          : (watch_context && !watch_context->origin_peer.empty() ? watch_context->origin_peer : item.peer_id);
        if(!origin_source.empty()) {
          pm_->set_directory_origin(item.directory_guid, origin_source);
        }
      }
      outcome.success &= dir_result.success;
      outcome.changed |= dir_result.changed;
    }

    // Group files by hash to aggregate providers.
    struct DownloadTask {
      PeerManager::RemoteListingItem primary;
      std::vector<PeerManager::RemoteListingItem> providers;
      std::filesystem::path dest_relative;
    };
    std::unordered_map<std::string, DownloadTask> tasks;

    for(const auto& item : entries) {
      if(item.is_directory) continue;
      if(item.hash.empty()) continue;
      std::filesystem::path dest_rel;
      if(dest_override) {
        dest_rel = dest_base / std::filesystem::path(item.relative_path.empty() ? item.hash : item.relative_path);
        if(!dest_is_directory && entries.size() == 1) {
          dest_rel = dest_base;
        }
      } else {
        dest_rel = std::filesystem::path(item.relative_path.empty() ? item.hash : item.relative_path);
      }
      dest_rel = sanitize_relative(dest_rel);
      auto& task = tasks[item.hash];
      if(task.primary.hash.empty()) {
        task.primary = item;
        task.dest_relative = dest_rel;
      }
      task.providers.push_back(item);
    }

    for(auto& [hash, task] : tasks) {
      auto additional = blocking_list_hash(hash);
      for(auto& candidate : additional) {
        if(candidate.is_directory) continue;
        if(candidate.hash != hash) continue;
        auto it = std::find_if(task.providers.begin(), task.providers.end(),
                               [&](const auto& p){ return p.peer_id == candidate.peer_id; });
        if(it == task.providers.end()) {
          task.providers.push_back(candidate);
        }
      }
      if(task.providers.empty()) {
        if(!quiet) std::cout << "No available peers for hash " << hash << "\n";
        outcome.success = false;
        continue;
      }
      if(pm_ && pm_->transfer_debug_enabled()) {
        std::cout << "[swarm] providers for " << hash.substr(0, 8) << ": ";
        bool first = true;
        for(const auto& provider : task.providers) {
          if(!first) std::cout << ", ";
          first = false;
          std::cout << provider.peer_id;
        }
        std::cout << "\n";
      }
      auto file_result = download_file_from_providers(task.primary, task.providers, task.dest_relative, quiet, watch_context);
      outcome.success &= file_result.success;
      outcome.changed |= file_result.changed;
    }

    return outcome;
  }

  SyncOutcome sync_directory(const std::string& peer_id,
                             const std::string& remote_path,
                             const std::string& dir_guid,
                             const std::filesystem::path& dest_root,
                             bool quiet,
                             const WatchEntry* watch_context) {
    auto entries = blocking_list_peer(peer_id, remote_path, "", "");
    entries.erase(std::remove_if(entries.begin(), entries.end(),
      [](const PeerManager::RemoteListingItem& item){
        return is_internal_listing_path(item.relative_path);
      }), entries.end());
    notify_untrusted_directory_sources(entries);
    maybe_warn_duplicate_hashes_for_guid(watch_context, dir_guid, entries);
    if(entries.empty()) {
      ensure_directory_exists(download_root() / dest_root);
      ensure_directory_exists(share_root() / dest_root);
      return {};
    }

    SyncOutcome outcome;
    for(const auto& item : entries) {
      if(item.is_directory) {
        auto sub_rel = dest_root / relative_within(remote_path, item.relative_path);
        sub_rel = sanitize_relative(sub_rel);
        auto sub_string = sub_rel.generic_string();
        if(!sub_string.empty()) {
          ensure_directory_exists(download_root() / sub_rel);
          ensure_directory_exists(share_root() / sub_rel);
        }
        if(!sub_string.empty()) {
          if(!item.directory_guid.empty()) {
            remember_dir_guid_mapping(sub_string, item.directory_guid);
          } else {
            pm_->register_directory(sub_string);
          }
        }
        auto sub_result = sync_directory(item.peer_id,
                                         item.relative_path,
                                         item.directory_guid,
                                         sub_rel,
                                         quiet,
                                         watch_context);
        if(!item.directory_guid.empty()) {
          std::string origin_source = !item.origin_peer.empty()
            ? item.origin_peer
            : (watch_context && !watch_context->origin_peer.empty() ? watch_context->origin_peer : item.peer_id);
          if(!origin_source.empty()) {
            pm_->set_directory_origin(item.directory_guid, origin_source);
          }
        }
        outcome.success &= sub_result.success;
        outcome.changed |= sub_result.changed;
      } else {
        auto providers = blocking_list_hash(item.hash);
        std::vector<PeerManager::RemoteListingItem> usable;
        for(auto& p : providers) {
          if(!p.is_directory && p.hash == item.hash) {
            usable.push_back(p);
          }
        }
        if(usable.empty()) {
          usable.push_back(item);
        }
        auto sub_rel = dest_root / relative_within(remote_path, item.relative_path);
        sub_rel = sanitize_relative(sub_rel);
        auto file_result = download_file_from_providers(item, usable, sub_rel, quiet, watch_context);
        outcome.success &= file_result.success;
        outcome.changed |= file_result.changed;
      }
    }
    return outcome;
  }

  SyncOutcome download_file_from_providers(const PeerManager::RemoteListingItem& primary,
                                           const std::vector<PeerManager::RemoteListingItem>& providers,
                                           const std::filesystem::path& dest_relative,
                                           bool quiet,
                                           const WatchEntry* watch_context) {
    SyncOutcome outcome;
    if(primary.hash.empty()) {
      outcome.success = false;
      return outcome;
    }

    auto clean_relative = sanitize_relative(dest_relative);
    if(clean_relative.empty()) {
      clean_relative = std::filesystem::path(primary.hash);
    }

    PeerManager::SharedFileEntry existing;
    if(pm_->find_local_file_by_hash(primary.hash, existing)) {
      auto final_path = share_root() / clean_relative;
      if(std::filesystem::exists(final_path)) {
        return outcome;
      }
    }

    std::vector<PeerManager::RemoteListingItem> remote_providers;
    for(const auto& provider : providers) {
      if(provider.peer_id == pm_->local_peer_id()) continue;
      remote_providers.push_back(provider);
    }
    if(remote_providers.empty()) {
      remote_providers = providers;
    }

    if(remote_providers.empty()) {
      if(!quiet) std::cout << "No peers available for hash " << primary.hash << "\n";
      outcome.success = false;
      return outcome;
    }
    if(pm_ && pm_->transfer_debug_enabled()) {
      std::cout << "[swarm] remote providers for " << primary.hash.substr(0, 8) << ": ";
      bool first = true;
      for(const auto& provider : remote_providers) {
        if(!first) std::cout << ", ";
        first = false;
        std::cout << provider.peer_id;
      }
      std::cout << "\n";
    }

    auto staging_path = download_root() / clean_relative;
    auto final_path = share_root() / clean_relative;
    std::string relative_string = clean_relative.generic_string();
    auto parent_relative = clean_relative.parent_path().generic_string();
    auto dir_guid_opt = pm_->directory_guid_for_path(parent_relative);
    std::string dir_guid = dir_guid_opt ? *dir_guid_opt : std::string{};
    if(std::filesystem::exists(final_path)) {
      auto existing_hash = compute_file_hash(final_path);
      if(existing_hash && *existing_hash == primary.hash) {
        return outcome;
      }
      if(is_conflict_ignored(relative_string, primary.hash, dir_guid)) {
        if(!quiet) {
          std::cout << "Destination " << relative_string << " differs but is ignored.\n";
        }
      } else {
        auto origin = primary.peer_id.empty() ? pm_->local_peer_id() : primary.peer_id;
        add_conflict(relative_string, primary.hash, dir_guid, origin);
        if(!quiet) {
          std::cout << "Destination " << relative_string << " already exists with different contents. Added to conflict queue.\n";
        }
      }
      outcome.success = false;
      return outcome;
    }
    ensure_directory_exists(staging_path.parent_path());
    ensure_directory_exists(final_path.parent_path());

    uint64_t file_size = primary.size;
    if(file_size == 0) {
      for(const auto& provider : providers) {
        if(provider.size > 0) {
          file_size = provider.size;
          break;
        }
      }
    }
    if(file_size == 0) {
      if(!quiet) std::cout << "Unknown file size for hash " << primary.hash << "\n";
      outcome.success = false;
      return outcome;
    }

    const std::size_t chunk_size = current_chunk_size();
    const std::size_t total_chunks = static_cast<std::size_t>((file_size + chunk_size - 1) / chunk_size);
    std::vector<uint8_t> chunk_states(total_chunks, 0);
    const bool enable_meter = transfer_progress_enabled_ && !quiet && total_chunks > 0;

    auto relative_parent = sanitize_relative(final_path.parent_path().lexically_relative(share_root()));
    auto relative_parent_str = relative_parent.generic_string();
      if(!relative_parent_str.empty()) {
        if(!primary.directory_guid.empty()) {
          remember_dir_guid_mapping(relative_parent_str, primary.directory_guid);
        } else {
          pm_->register_directory(relative_parent_str);
        }
      if(!dir_guid_opt || *dir_guid_opt != primary.directory_guid) {
        dir_guid_opt = pm_->directory_guid_for_path(relative_parent_str);
      }
      if(dir_guid_opt) {
        std::string origin_source;
        if(watch_context && !watch_context->origin_peer.empty()) {
          origin_source = watch_context->origin_peer;
        } else if(!providers.empty() && !providers.front().peer_id.empty()) {
          origin_source = providers.front().peer_id;
        }
        if(!origin_source.empty()) {
          pm_->set_directory_origin(*dir_guid_opt, origin_source);
        }
      }
    }

    enum class DownloadAttemptResult {
      Success,
      HashMismatch,
      Failed
    };

    std::unordered_set<std::string> allowed_swarm_peers;
    if(watch_context) {
      for(const auto& src : watch_context->sources) {
        if(!src.peer.empty()) allowed_swarm_peers.insert(src.peer);
      }
    }

    auto download_start = std::chrono::steady_clock::now();

    auto attempt_download = [&](const std::vector<PeerManager::RemoteListingItem>& provider_set,
                                bool quiet_attempt) -> DownloadAttemptResult {
      if(provider_set.empty()) return DownloadAttemptResult::Failed;

      std::fstream out(staging_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
      if(!out) {
        if(!quiet_attempt) std::cout << "Cannot open staging file: " << staging_path << "\n";
        return DownloadAttemptResult::Failed;
      }
      if(file_size > 0) {
        char zero = 0;
        out.seekp(static_cast<std::streamoff>(file_size - 1));
        out.write(&zero, 1);
        out.seekp(0);
      }
      std::mutex file_mutex;
      auto writer = [&](uint64_t offset, const std::vector<char>& data) -> bool {
        std::lock_guard<std::mutex> lock(file_mutex);
        out.seekp(static_cast<std::streamoff>(offset));
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(out);
      };
      SwarmConfig swarm_config;
      swarm_config.chunk_size = chunk_size;
      swarm_config.total_chunks = total_chunks;
      swarm_config.max_parallel = swarm_max_parallel_;
      swarm_config.chunk_buffers = swarm_chunk_buffers_;
      swarm_config.progress_interval = swarm_progress_interval_;
      swarm_config.enable_meter = enable_meter && !quiet_attempt;
      auto peer_total = std::make_shared<std::atomic<std::size_t>>(0);
      auto active_peers = std::make_shared<std::atomic<std::size_t>>(0);
      auto seed_peer_counts = [&](const std::vector<PeerManager::RemoteListingItem>& items){
        std::unordered_set<std::string> unique;
        for(const auto& item : items) {
          if(item.peer_id.empty()) continue;
          unique.insert(item.peer_id);
        }
        auto count = unique.size();
        peer_total->store(count);
      };
      seed_peer_counts(provider_set);
      auto refresh_provider = make_swarm_refresh_provider(primary.hash, allowed_swarm_peers, peer_total);
      swarm_config.enable_dynamic_providers = static_cast<bool>(refresh_provider);
      swarm_config.provider_refresh_interval = swarm_provider_refresh_interval_;
      auto fetcher = [&](const std::string& peer_id,
                         const std::string& hash,
                         uint64_t offset,
                         std::size_t length){
        return fetch_chunk(peer_id, hash, offset, length);
      };
      std::size_t meter_line_width = 0;
      SwarmMeterCallback meter_callback;
      if(swarm_config.enable_meter) {
        meter_callback = [&](const std::vector<uint8_t>& states,
                             uint64_t downloaded,
                             bool /*force*/){
          auto meter = format_transfer_meter(states, downloaded, file_size, chunk_size);
          std::size_t peers_total = peer_total ? peer_total->load() : provider_set.size();
          if(peers_total == 0) {
            peers_total = provider_set.empty() ? 0 : provider_set.size();
          }
          std::size_t peers_active = active_peers ? active_peers->load() : 0;
          if(peers_active > peers_total) peers_total = peers_active;
          std::ostringstream line;
          line << "\rSyncing " << primary.hash.substr(0, 8) << "... " << meter
               << " P[" << peers_active << "/" << peers_total << "]";
          auto rendered = line.str();
          std::cout << rendered;
          if(rendered.size() < meter_line_width) {
            std::cout << std::string(meter_line_width - rendered.size(), ' ');
          } else {
            meter_line_width = rendered.size();
          }
          std::cout.flush();
        };
      }
      SwarmPeerStatsMap peer_stats;
      std::string swarm_failure;
      bool downloaded = run_swarm_download(primary.hash,
                                           file_size,
                                           swarm_config,
                                           provider_set,
                                           chunk_states,
                                           fetcher,
                                           writer,
                                           meter_callback,
                                           swarm_failure,
                                           refresh_provider,
                                           &peer_stats,
                                           active_peers);
      out.close();
      if(!downloaded) {
        if(!quiet_attempt && !swarm_failure.empty()) {
          std::cout << swarm_failure << "\n";
        }
        std::error_code ec;
        std::filesystem::remove(staging_path, ec);
        return DownloadAttemptResult::Failed;
      }
      if(swarm_config.enable_meter && !quiet_attempt) {
        if(meter_line_width > 0) {
          std::cout << "\r" << std::string(meter_line_width, ' ') << "\r";
        }
        std::cout << "\n";
      }

      auto computed_hash = compute_file_hash(staging_path);
      if(!computed_hash) {
        if(!quiet_attempt) std::cout << "Failed to compute hash for " << clean_relative << "\n";
        std::error_code ec;
        std::filesystem::remove(staging_path, ec);
        return DownloadAttemptResult::HashMismatch;
      }
      if(*computed_hash != primary.hash) {
        if(!quiet_attempt) {
          std::cout << "File hash mismatch for " << clean_relative
                    << " (expected " << primary.hash
                    << " but got " << *computed_hash << ")\n";
        }
        std::error_code ec;
        std::filesystem::remove(staging_path, ec);
        return DownloadAttemptResult::HashMismatch;
      }
      if(!quiet_attempt) {
        std::cout << "\nHash verified for " << clean_relative
                  << " (" << primary.hash << ")\n";
      }

      std::error_code ec;
      std::filesystem::create_directories(final_path.parent_path(), ec);
      ec.clear();
      std::filesystem::rename(staging_path, final_path, ec);
      if(ec) {
        std::filesystem::copy_file(staging_path, final_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(staging_path);
        if(ec) {
          if(!quiet_attempt) std::cout << "Failed to move downloaded file: " << ec.message() << "\n";
          return DownloadAttemptResult::Failed;
        }
      }

      std::string registered_path = clean_relative.generic_string();
      pm_->register_local_share(primary.hash, registered_path, final_path, file_size);
      auto elapsed = std::chrono::steady_clock::now() - download_start;
      auto elapsed_label = format_duration_compact(elapsed);
      auto stats_summary = format_peer_stats_summary(peer_stats);
      std::string stats_suffix;
      if(!stats_summary.empty()) {
        stats_suffix = " [" + stats_summary + "]";
      }
      if(!quiet_attempt) {
        std::cout << "Synced " << clean_relative << " (" << format_size(file_size)
                  << ") in " << elapsed_label << stats_suffix << "\n";
      }
      outcome.changed = true;
      return DownloadAttemptResult::Success;
    };

    std::vector<std::vector<PeerManager::RemoteListingItem>> attempt_sets;
    attempt_sets.push_back(remote_providers);
    if(watch_context) {
      std::unordered_set<std::string> trusted_peers;
      for(const auto& src : watch_context->sources) {
        trusted_peers.insert(src.peer);
      }
      if(!trusted_peers.empty()) {
        std::vector<PeerManager::RemoteListingItem> trusted_only;
        for(const auto& provider : remote_providers) {
          if(trusted_peers.count(provider.peer_id)) {
            trusted_only.push_back(provider);
          }
        }
        if(!trusted_only.empty() && trusted_only.size() < remote_providers.size()) {
          attempt_sets.push_back(std::move(trusted_only));
        }
      }
    }

    DownloadAttemptResult final_result = DownloadAttemptResult::Failed;
    for(size_t i = 0; i < attempt_sets.size(); ++i) {
      auto result = attempt_download(attempt_sets[i], quiet);
      final_result = result;
      if(result == DownloadAttemptResult::Success) {
        break;
      }
      if(result == DownloadAttemptResult::HashMismatch && i + 1 < attempt_sets.size()) {
        if(!quiet) {
          std::cout << "Retrying " << clean_relative << " with trusted peers only.\n";
        }
        continue;
      }
      break;
    }

    if(final_result == DownloadAttemptResult::Success) {
      return outcome;
    }

    if(final_result == DownloadAttemptResult::HashMismatch) {
      if(is_conflict_ignored(relative_string, primary.hash, dir_guid)) {
        if(!quiet) {
          std::cout << "Destination " << relative_string << " differs but is ignored.\n";
        }
      } else {
        auto origin = primary.peer_id.empty() ? pm_->local_peer_id() : primary.peer_id;
        add_conflict(relative_string, primary.hash, dir_guid, origin);
        if(!quiet) {
          std::cout << "Destination " << relative_string << " already exists with different contents. Added to conflict queue.\n";
        }
      }
    }

    outcome.success = false;
    return outcome;
  }

  PeerManager::ChunkResponse fetch_chunk(const std::string& peer_id,
                                         const std::string& hash,
                                         uint64_t offset,
                                         std::size_t length) {
    auto promise = std::make_shared<std::promise<PeerManager::ChunkResponse>>();
    auto future = promise->get_future();
    auto request_id = pm_->request_file_chunk(peer_id, hash, offset, length,
      [promise](const PeerManager::ChunkResponse& response){
        try {
          promise->set_value(response);
        } catch(...) {
          // promise already satisfied
        }
      });
    if(!request_id) {
      PeerManager::ChunkResponse resp;
      resp.peer_id = peer_id;
      resp.hash = hash;
      resp.offset = offset;
      resp.success = false;
      resp.error = "Peer unavailable";
      return resp;
    }

    if(future.wait_for(chunk_timeout_) != std::future_status::ready) {
      pm_->cancel_chunk_request(*request_id);
      PeerManager::ChunkResponse resp;
      resp.peer_id = peer_id;
      resp.hash = hash;
      resp.offset = offset;
      resp.success = false;
      resp.error = "Timeout waiting for chunk";
      return resp;
    }

    return future.get();
  }

  static std::filesystem::path sanitize_relative(const std::filesystem::path& path) {
    std::filesystem::path cleaned;
    for(const auto& part : path) {
      if(part == "." || part.empty()) continue;
      if(part == "..") continue;
      cleaned /= part;
    }
    return cleaned;
  }

  static std::string normalize_relative_string(const std::string& input) {
    if(input.empty()) return "";
    auto cleaned = sanitize_relative(std::filesystem::path(input));
    auto normalized = cleaned.generic_string();
    while(!normalized.empty() && normalized.front() == '/') {
      normalized.erase(normalized.begin());
    }
    while(!normalized.empty() && normalized.back() == '/') {
      normalized.pop_back();
    }
    return normalized;
  }

  static std::string basename_for_relative_path(const std::string& path) {
    if(path.empty()) return "";
    auto normalized = normalize_relative_string(path);
    if(normalized.empty()) return "";
    auto pos = normalized.find_last_of('/');
    if(pos == std::string::npos) return normalized;
    return normalized.substr(pos + 1);
  }

  static std::filesystem::path relative_within(const std::string& base, const std::string& full) {
    if(base.empty()) return std::filesystem::path(full);
    std::filesystem::path full_path(full);
    std::filesystem::path base_path(base);
    auto rel = full_path.lexically_relative(base_path);
    if(rel.empty() || rel == std::filesystem::path(".")) {
      return full_path.filename();
    }
    if(rel.string().rfind("..", 0) == 0) {
      return full_path;
    }
    return rel;
  }

  static void ensure_directory_exists(const std::filesystem::path& dir) {
    if(dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
  }

  bool directory_exists_relative(const std::string& relative_path) const {
    if(relative_path.empty()) return false;
    auto absolute = share_root() / relative_path;
    std::error_code ec;
    return std::filesystem::exists(absolute, ec) &&
           std::filesystem::is_directory(absolute, ec);
  }

  std::filesystem::path watch_file_path() const {
    return config_root() / "watches.json";
  }

  void load_watches() {
    std::ifstream in(watch_file_path());
    if(!in) return;
    nlohmann::json doc;
    try {
      in >> doc;
    } catch(...) {
      return;
    }

    std::vector<std::pair<std::string, std::string>> pending_bindings;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      watches_.clear();
      if(!doc.contains("watches")) return;
      for(const auto& item : doc["watches"]) {
        WatchEntry entry;
        entry.id = item.value("id", generate_watch_id());
        if(item.contains("sources")) {
          for(const auto& src : item["sources"]) {
            WatchEntry::Source s;
            s.peer = src.value("peer", "");
            s.path = normalize_cli_path(src.value("path", ""));
            if(!s.peer.empty()) entry.sources.push_back(std::move(s));
          }
        } else {
          WatchEntry::Source s;
          s.peer = item.value("peer", "");
          s.path = normalize_cli_path(item.value("path", ""));
          if(!s.peer.empty()) entry.sources.push_back(std::move(s));
        }
        entry.dir_guid = item.value("dir_guid", "");
        entry.origin_peer = item.value("origin_peer", "");
        entry.dest_path = normalize_cli_path(item.value("dest", ""));
        entry.dest_is_directory = item.value("dest_is_directory", true);
        auto interval_seconds = item.value("interval", static_cast<int>(default_watch_interval_.count()));
        entry.interval = std::chrono::seconds(interval_seconds > 0 ? interval_seconds : default_watch_interval_.count());
        entry.next_run = std::chrono::steady_clock::now();
        if(entry.sources.empty()) continue;
        entry.sources.erase(std::remove_if(entry.sources.begin(), entry.sources.end(),
          [&](const WatchEntry::Source& src){ return src.peer.empty() || is_internal_listing_path(src.path); }),
          entry.sources.end());
        if(entry.sources.empty()) continue;
        if(entry.origin_peer.empty() ||
           std::none_of(entry.sources.begin(), entry.sources.end(),
             [&](const WatchEntry::Source& src){ return src.peer == entry.origin_peer; })) {
          entry.origin_peer = entry.sources.front().peer;
        }
        if(is_internal_listing_path(entry.dest_path)) continue;
        watches_.push_back(std::move(entry));
        auto& stored = watches_.back();
        if(!stored.dir_guid.empty() && !stored.origin_peer.empty()) {
          pm_->set_directory_origin(stored.dir_guid, stored.origin_peer);
          if(auto local = watch_binding_path_candidate(stored)) {
            pending_bindings.emplace_back(*local, stored.dir_guid);
          }
        }
      }
    }

    for(const auto& [path, guid] : pending_bindings) {
      remember_dir_guid_mapping(path, guid);
    }
  }

  void save_watches() {
    nlohmann::json doc;
    doc["watches"] = nlohmann::json::array();
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      for(const auto& entry : watches_) {
        nlohmann::json item;
        item["id"] = entry.id;
        nlohmann::json sources = nlohmann::json::array();
        for(const auto& src : entry.sources) {
          nlohmann::json s;
          s["peer"] = src.peer;
          s["path"] = src.path;
          sources.push_back(std::move(s));
        }
        item["sources"] = std::move(sources);
        if(!entry.dir_guid.empty()) item["dir_guid"] = entry.dir_guid;
        if(!entry.origin_peer.empty()) item["origin_peer"] = entry.origin_peer;
        if(!entry.dest_path.empty()) item["dest"] = entry.dest_path;
        item["dest_is_directory"] = entry.dest_is_directory;
        item["interval"] = entry.interval.count();
        doc["watches"].push_back(item);
      }
    }
    auto watch_path = watch_file_path();
    std::error_code ec;
    std::filesystem::create_directories(watch_path.parent_path(), ec);
    std::ofstream out(watch_path);
    if(out) {
      out << doc.dump(2);
    }
  }

  void list_watches() {
    std::vector<WatchEntry> snapshot;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      snapshot = watches_;
    }
    if(snapshot.empty()) {
      std::cout << "No watches configured.\n";
      return;
    }
    for(const auto& entry : snapshot) {
      auto local_label = watch_local_target_label(entry);
      auto peer_statuses = gather_watch_peer_status(entry);
      std::size_t online = 0;
      for(const auto& kv : peer_statuses) {
        if(kv.second == SourceReachability::Online) ++online;
      }
      std::size_t total_peers = peer_statuses.size();
      std::cout << entry.id << " -> "
                << (entry.dir_guid.empty() ? "(guid unknown)" : entry.dir_guid)
                << "  local=" << local_label
                << "  every " << entry.interval.count() << "s"
                << "  peers=" << online << "/" << total_peers;
      if(!entry.origin_peer.empty()) {
        SourceReachability origin_state = SourceReachability::Offline;
        auto it = peer_statuses.find(entry.origin_peer);
        if(it != peer_statuses.end()) {
          origin_state = it->second;
        } else {
          WatchEntry::Source probe;
          probe.peer = entry.origin_peer;
          if(entry.dir_guid.empty() && !entry.sources.empty()) {
            probe.path = entry.sources.front().path;
          }
          origin_state = check_watch_source(entry, probe);
        }
        std::cout << "  origin=" << entry.origin_peer << "(" << reachability_label(origin_state) << ")";
      }
      std::cout << "\n";
      if(entry.sources.empty()) {
        std::cout << "  (no sources)\n";
        continue;
      }
      for(const auto& src : entry.sources) {
        auto reachability = SourceReachability::Offline;
        auto it = peer_statuses.find(src.peer);
        if(it != peer_statuses.end()) {
          reachability = it->second;
        }
        std::cout << "  - " << src.peer;
        std::string label;
        if(auto resolved_path = resolve_watch_source_path(entry, src.peer)) {
          label = *resolved_path;
        }
        if(label.empty() && !entry.dir_guid.empty()) {
          label = entry.dir_guid;
        }
        if(!label.empty()) {
          std::cout << ":/" << label;
        }
        std::cout << " (" << reachability_label(reachability) << ")";
        if(!entry.origin_peer.empty() && src.peer == entry.origin_peer) {
          std::cout << " [origin]";
        }
        std::cout << "\n";
      }
      std::cout << "\n";
    }
  }

  void start_watch_thread() {
    watch_thread_running_ = true;
    watch_thread_ = std::thread([this](){ watch_loop(); });
  }

  void watch_loop() {
    std::unique_lock<std::mutex> lock(watch_mutex_);
    while(watch_thread_running_) {
      if(watches_.empty()) {
        watch_cv_.wait_for(lock, std::chrono::seconds(5));
        continue;
      }

      auto now = std::chrono::steady_clock::now();
      auto it = std::min_element(watches_.begin(), watches_.end(),
                                 [](const WatchEntry& a, const WatchEntry& b){
                                   return a.next_run < b.next_run;
                                 });
      if(it == watches_.end()) {
        watch_cv_.wait_for(lock, std::chrono::seconds(5));
        continue;
      }

      if(it->next_run > now) {
        watch_cv_.wait_until(lock, it->next_run);
        continue;
      }

      WatchEntry entry = *it;
      it->next_run = std::chrono::steady_clock::now() + entry.interval;
      lock.unlock();
      auto result = run_single_watch(entry);
      if(!result.success) {
        std::cout << "[watch " << entry.id << "] sync failed.\n";
      } else if(result.changed) {
        std::cout << "[watch " << entry.id << "] synced.\n";
      }
      lock.lock();
    }
  }

  void stop_watch_thread() {
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      watch_thread_running_ = false;
    }
    watch_cv_.notify_all();
    if(watch_thread_.joinable()) watch_thread_.join();
  }

  SyncOutcome run_single_watch(const WatchEntry& entry) {
    SyncOutcome outcome;
    if(entry.sources.empty()) {
      outcome.success = false;
      return outcome;
    }

    WatchEntry current = entry;
    if(current.origin_peer.empty() && !current.sources.empty()) {
      current.origin_peer = current.sources.front().peer;
    }
    if(current.dir_guid.empty()) {
      for(const auto& src : current.sources) {
        if(src.peer.empty()) continue;
        if(src.peer == pm_->local_peer_id()) continue;
        if(src.path.empty()) continue;
        auto resolved = resolve_remote_dir_guid(src.peer, src.path);
        if(resolved) {
          current.dir_guid = resolved->guid;
          if(!resolved->origin_peer.empty()) {
            current.origin_peer = resolved->origin_peer;
          }
          {
            std::lock_guard<std::mutex> lock(watch_mutex_);
            auto* stored = find_watch_by_id(current.id);
            if(stored) {
              if(stored->dir_guid.empty()) {
                stored->dir_guid = current.dir_guid;
              }
              if(!current.origin_peer.empty()) {
                stored->origin_peer = current.origin_peer;
              } else if(stored->origin_peer.empty() && !stored->sources.empty()) {
                stored->origin_peer = stored->sources.front().peer;
              }
            }
          }
          if(!current.dir_guid.empty() && !current.origin_peer.empty()) {
            pm_->set_directory_origin(current.dir_guid, current.origin_peer);
          }
          break;
        }
      }
    }

    std::vector<const WatchEntry::Source*> ordered_sources;
    auto is_origin = [&](const WatchEntry::Source& src){
      return !current.origin_peer.empty() && src.peer == current.origin_peer;
    };
    if(!current.origin_peer.empty()) {
      for(const auto& src : current.sources) {
        if(is_origin(src)) {
          ordered_sources.push_back(&src);
          break;
        }
      }
    }
    for(const auto& src : current.sources) {
      if(current.origin_peer.empty() || !is_origin(src)) {
        ordered_sources.push_back(&src);
      }
    }

    std::vector<PeerManager::RemoteListingItem> collected;
    std::optional<ListCommand> selected;

    for(const auto* source_ptr : ordered_sources) {
      const auto& source = *source_ptr;
      if(source.peer.empty()) continue;
      if(source.peer == pm_->local_peer_id()) continue;

      ListCommand candidate;
      if(!current.dir_guid.empty()) {
        candidate.type = ListCommand::Type::RemoteDirectoryGuid;
        candidate.peer = source.peer;
        candidate.dir_guid = current.dir_guid;
      } else if(!source.path.empty()) {
        candidate.type = ListCommand::Type::RemotePath;
        candidate.peer = source.peer;
        candidate.path = source.path;
      } else {
        continue;
      }

      auto entries = blocking_list_peer(candidate.peer,
                                        current.dir_guid.empty() ? candidate.path : "",
                                        "",
                                        current.dir_guid);
      entries.erase(std::remove_if(entries.begin(), entries.end(),
        [](const PeerManager::RemoteListingItem& item){
          return is_internal_listing_path(item.relative_path);
        }), entries.end());

      notify_untrusted_directory_sources(entries);

      if(!selected) {
        selected = candidate;
        collected = entries;
      } else if(collected.empty() && !entries.empty()) {
        selected = candidate;
        collected = entries;
      }

      if(!collected.empty()) break;
    }

    if(!selected) {
      outcome.success = false;
      return outcome;
    }

    std::optional<std::string> remote_root_origin;
    for(const auto& item : collected) {
      if(item.is_directory && item.relative_path.empty() && !item.origin_peer.empty()) {
        remote_root_origin = item.origin_peer;
        break;
      }
    }
    if(remote_root_origin && !remote_root_origin->empty()) {
      current.origin_peer = *remote_root_origin;
      {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        auto* stored = find_watch_by_id(current.id);
        if(stored) {
          stored->origin_peer = current.origin_peer;
        }
      }
      if(!current.dir_guid.empty()) {
        pm_->set_directory_origin(current.dir_guid, current.origin_peer);
      }
    }
    collected.erase(std::remove_if(collected.begin(), collected.end(),
      [](const PeerManager::RemoteListingItem& item){
        return item.is_directory && item.relative_path.empty();
      }), collected.end());

    std::optional<std::string> dest;
    if(!current.dest_path.empty()) dest = current.dest_path;
    outcome = perform_sync(*selected, std::move(collected), dest, current.dest_is_directory, false, &current);
    return outcome;
  }

  std::string generate_watch_id() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t value = dist(rng);
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
  }

  WatchEntry* find_watch_by_guid(const std::string& guid) {
    if(guid.empty()) return nullptr;
    for(auto& entry : watches_) {
      if(entry.dir_guid == guid) return &entry;
    }
    return nullptr;
  }

  WatchEntry* find_watch_by_peer_path(const std::string& peer, const std::string& path) {
    auto normalized = normalize_cli_path(path);
    for(auto& entry : watches_) {
      for(const auto& src : entry.sources) {
        if(src.peer == peer && src.path == normalized) return &entry;
      }
    }
    return nullptr;
  }

  WatchEntry* find_watch_by_id(const std::string& id) {
    for(auto& entry : watches_) {
      if(entry.id == id) return &entry;
    }
    return nullptr;
  }

  std::optional<ResolvedDirGuid> resolve_remote_dir_guid(const std::string& peer,
                                                         const std::string& path,
                                                         bool* path_was_file = nullptr) {
    if(path_was_file) *path_was_file = false;
    auto normalized = normalize_cli_path(path);
    if(normalized.empty()) return std::nullopt;
    std::filesystem::path p(normalized);
    auto parent = normalize_cli_path(p.parent_path().generic_string());
    auto listings = blocking_list_peer(peer, parent, "", "");
    listings.erase(std::remove_if(listings.begin(), listings.end(),
      [](const PeerManager::RemoteListingItem& item){
        return is_internal_listing_path(item.relative_path);
      }), listings.end());
    notify_untrusted_directory_sources(listings);
    bool saw_file_match = false;
    for(const auto& item : listings) {
      auto candidate = normalize_cli_path(item.relative_path);
      if(candidate != normalized) continue;
      if(item.is_directory) {
        if(item.directory_guid.empty()) continue;
        ResolvedDirGuid resolved;
        resolved.guid = item.directory_guid;
        resolved.origin_peer = item.origin_peer.empty() ? peer : item.origin_peer;
        if(path_was_file) *path_was_file = false;
        return resolved;
      } else {
        saw_file_match = true;
      }
    }
    if(path_was_file) *path_was_file = saw_file_match;
    return std::nullopt;
  }

  std::optional<std::string> active_path_for_guid(const std::string& guid) const {
    if(guid.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(dir_guid_mutex_);
    auto it = dir_guid_records_.find(guid);
    if(it == dir_guid_records_.end()) return std::nullopt;
    if(!it->second.active || it->second.path.empty()) return std::nullopt;
    return it->second.path;
  }

  std::string watch_local_target_label(const WatchEntry& entry) const {
    if(!entry.dest_path.empty()) {
      return entry.dest_path + " (override)";
    }
    auto local = active_path_for_guid(entry.dir_guid);
    if(local) return *local;
    if(!entry.dir_guid.empty()) {
      if(!entry.sources.empty() && !entry.sources.front().path.empty()) {
        return entry.sources.front().path + " (default)";
      }
      return "(unmapped)";
    }
    if(!entry.sources.empty() && !entry.sources.front().path.empty()) {
      return entry.sources.front().path + " (default)";
    }
    return "(unmapped)";
  }

  std::optional<std::string> watch_binding_path_candidate(const WatchEntry& entry) const {
    if(!entry.dest_path.empty() && !is_internal_listing_path(entry.dest_path)) {
      return entry.dest_path;
    }
    if(!entry.sources.empty()) {
      for(const auto& src : entry.sources) {
        if(!src.path.empty() && !is_internal_listing_path(src.path)) {
          return src.path;
        }
      }
    }
    return std::nullopt;
  }

  std::string canonical_watch_destination(const WatchEntry& entry) const {
    if(!entry.dest_path.empty()) {
      return normalize_cli_path(entry.dest_path);
    }
    if(auto mapped = active_path_for_guid(entry.dir_guid)) {
      auto normalized = normalize_cli_path(*mapped);
      if(!normalized.empty()) return normalized;
    }
    for(const auto& src : entry.sources) {
      if(src.path.empty()) continue;
      auto normalized = normalize_cli_path(src.path);
      if(!normalized.empty()) return normalized;
    }
    return "";
  }

  std::string requested_watch_destination(const WatchEntry::Source& source,
                                          const std::optional<std::string>& dest_override,
                                          const std::string& guid) const {
    if(dest_override && !dest_override->empty()) {
      return normalize_cli_path(*dest_override);
    }
    if(!source.path.empty()) {
      auto normalized = normalize_cli_path(source.path);
      if(!normalized.empty()) return normalized;
    }
    if(!guid.empty()) {
      if(auto mapped = active_path_for_guid(guid)) {
        auto normalized = normalize_cli_path(*mapped);
        if(!normalized.empty()) return normalized;
      }
    }
    return "";
  }

  std::optional<std::string> default_peer_for_guid(const std::string& guid) {
    if(guid.empty()) return std::nullopt;
    if(auto origin = pm_->directory_origin(guid)) {
      if(!origin->empty()) return origin;
    }
    std::lock_guard<std::mutex> lock(watch_mutex_);
    auto* existing = find_watch_by_guid(guid);
    if(existing) {
      std::string peer = existing->origin_peer;
      if(peer.empty() && !existing->sources.empty()) {
        peer = existing->sources.front().peer;
      }
      if(!peer.empty()) return peer;
    }
    return std::nullopt;
  }

  static std::string format_remote_source_label(const std::string& peer,
                                                const std::string& path_or_guid) {
    if(peer.empty()) return path_or_guid;
    std::string label = peer + ":/";
    label += path_or_guid;
    return label;
  }

  std::optional<WatchSourceResolution> resolve_watch_resource(const std::string& spec,
                                                              std::string& error,
                                                              const std::string& default_peer = "") {
    auto resource = resolve_resource_identifier(spec);
    if(!resource) {
      error = "Unable to parse watch target '" + spec + "'.";
      return std::nullopt;
    }
    if(resource->kind != ResourceIdentifier::Kind::Listing) {
      error = "Watch targets must reference a peer path or directory GUID.";
      return std::nullopt;
    }

    auto ensure_peer_valid = [&](const std::string& peer) -> bool {
      if(peer.empty()) {
        error = "Peer is required for this watch target.";
        return false;
      }
      if(peer == pm_->local_peer_id()) {
        error = "Cannot watch self.";
        return false;
      }
      return true;
    };

    WatchSourceResolution result;
    const auto& cmd = resource->list;
    switch(cmd.type) {
      case ListCommand::Type::RemotePath: {
        if(!ensure_peer_valid(cmd.peer)) return std::nullopt;
        auto normalized = normalize_cli_path(cmd.path);
        if(is_internal_listing_path(normalized)) {
          error = "Cannot watch reserved staging path " + format_remote_source_label(cmd.peer, normalized) + ".";
          return std::nullopt;
        }
        bool source_was_file = false;
        auto resolved = resolve_remote_dir_guid(cmd.peer, normalized, &source_was_file);
        if(!resolved) {
          auto label = format_remote_source_label(cmd.peer, normalized);
          if(source_was_file) {
            error = "Cannot watch " + label + " because it is a file. Watches must target directories.";
          } else {
            error = "Unable to determine directory GUID for " + label + ". Ensure it exists on the remote peer.";
          }
          return std::nullopt;
        }
        result.source.peer = cmd.peer;
        result.source.path = normalized;
        result.resolved = *resolved;
        break;
      }
      case ListCommand::Type::RemoteDirectoryGuid: {
        if(!ensure_peer_valid(cmd.peer)) return std::nullopt;
        if(cmd.dir_guid.empty()) {
          error = "Directory GUID is required for watch targets.";
          return std::nullopt;
        }
        result.source.peer = cmd.peer;
        result.source.path.clear();
        result.resolved.guid = cmd.dir_guid;
        result.resolved.origin_peer = cmd.peer;
        break;
      }
      case ListCommand::Type::DirectoryGuid: {
        std::string peer_choice = default_peer;
        if(peer_choice.empty()) {
          auto inferred = default_peer_for_guid(cmd.dir_guid);
          if(inferred) peer_choice = *inferred;
        }
        if(peer_choice.empty()) {
          error = "Peer is required for directory GUID " + cmd.dir_guid + ". Specify peer:/"
                  + cmd.dir_guid + ".";
          return std::nullopt;
        }
        if(!ensure_peer_valid(peer_choice)) return std::nullopt;
        result.source.peer = peer_choice;
        result.source.path.clear();
        result.resolved.guid = cmd.dir_guid;
        result.resolved.origin_peer = peer_choice;
        break;
      }
      default:
        error = "Watch targets must reference a peer path or directory GUID.";
        return std::nullopt;
    }

    if(result.resolved.guid.empty()) {
      error = "Directory GUID is required to track a watch.";
      return std::nullopt;
    }

    return result;
  }

  std::unordered_set<std::string> active_watch_guids() {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    std::unordered_set<std::string> ids;
    ids.reserve(watches_.size());
    for(const auto& entry : watches_) {
      if(!entry.dir_guid.empty()) ids.insert(entry.dir_guid);
    }
    return ids;
  }

  std::optional<std::string> watch_guid_for_path(const std::string& relative_path) {
    auto normalized = normalize_cli_path(relative_path);
    if(normalized.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(watch_mutex_);
    for(const auto& entry : watches_) {
      if(entry.dir_guid.empty()) continue;
      auto candidate = watch_binding_path_candidate(entry);
      if(candidate && normalize_cli_path(*candidate) == normalized) {
        return entry.dir_guid;
      }
    }
    return std::nullopt;
  }

  struct WatchAnnotationDisplay {
    std::optional<std::string> preferred;
    std::optional<std::string> fallback;
    bool fallback_unavailable = false;
  };

  std::vector<std::string> watch_preference_order(const WatchEntry& entry) const {
    std::vector<std::string> peers;
    auto add = [&](const std::string& peer) {
      if(peer.empty()) return;
      if(std::find(peers.begin(), peers.end(), peer) != peers.end()) return;
      peers.push_back(peer);
    };
    add(entry.origin_peer);
    for(const auto& src : entry.sources) {
      add(src.peer);
    }
    return peers;
  }

  std::optional<WatchAnnotationDisplay> watch_annotation_for_guid(const std::string& guid) {
    if(guid.empty()) return std::nullopt;
    WatchEntry snapshot;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      auto* entry = find_watch_by_guid(guid);
      if(!entry) return std::nullopt;
      snapshot = *entry;
    }

    auto order = watch_preference_order(snapshot);
    if(order.empty()) return std::nullopt;

    auto statuses = gather_watch_peer_status(snapshot);
    auto peer_is_online = [&](const std::string& peer) {
      auto it = statuses.find(peer);
      if(it == statuses.end()) {
        return peer == pm_->local_peer_id();
      }
      return it->second == SourceReachability::Online;
    };

    auto label_for_peer = [&](const std::string& peer) -> std::optional<std::string> {
      if(peer.empty()) return std::nullopt;
      auto path = resolve_watch_source_path(snapshot, peer);
      if(!path) path = std::string{};
      return format_watch_source_label(peer, *path);
    };

    WatchAnnotationDisplay display;
    auto preferred_peer = order.front();
    display.preferred = label_for_peer(preferred_peer);
    bool preferred_online = peer_is_online(preferred_peer);

    if(preferred_online) {
      return display;
    }

    for(size_t i = 1; i < order.size(); ++i) {
      const auto& peer = order[i];
      if(peer_is_online(peer)) {
        display.fallback = label_for_peer(peer);
        return display;
      }
    }

    display.fallback_unavailable = true;
    return display;
  }

  std::optional<std::string> resolve_watch_source_path(const WatchEntry& entry,
                                                       const std::string& peer) {
    if(peer.empty()) return std::nullopt;
    if(peer == pm_->local_peer_id()) {
      if(auto local = active_path_for_guid(entry.dir_guid)) return local;
    }

    if(auto remote = fetch_remote_watch_path(entry, peer)) {
      return remote;
    }

    auto recorded = std::find_if(entry.sources.begin(), entry.sources.end(),
      [&](const WatchEntry::Source& src){
        return src.peer == peer && !src.path.empty();
      });
    if(recorded != entry.sources.end()) {
      return recorded->path;
    }

    if(peer == pm_->local_peer_id() && !entry.dest_path.empty()) {
      return entry.dest_path;
    }

    return std::nullopt;
  }

  std::optional<std::string> fetch_remote_watch_path(const WatchEntry& entry,
                                                     const std::string& peer) {
    if(entry.dir_guid.empty() || peer.empty()) return std::nullopt;
    auto resp = fetch_peer_listing(peer, "", "", entry.dir_guid, true);
    if(resp.state != PeerListingState::Completed) return std::nullopt;

    for(const auto& item : resp.items) {
      if(item.is_directory && item.relative_path.empty() &&
         item.directory_guid == entry.dir_guid &&
         !item.listing_root_path.empty()) {
        auto normalized = normalize_cli_path(item.listing_root_path);
        if(!normalized.empty()) return normalized;
        return std::string{};
      }
    }

    for(const auto& item : resp.items) {
      if(item.relative_path.empty()) continue;
      auto slash = item.relative_path.rfind('/');
      if(slash == std::string::npos) continue;
      auto parent = item.relative_path.substr(0, slash);
      auto normalized = normalize_cli_path(parent);
      if(normalized.empty()) continue;
      return normalized;
    }

    return std::nullopt;
  }

  std::optional<std::string> format_watch_source_label(const std::string& peer,
                                                       const std::string& normalized_path) const {
    if(peer.empty()) return std::nullopt;
    std::string label = peer + ":/";
    if(!normalized_path.empty()) {
      label += normalized_path;
      if(label.back() != '/') label += "/";
    }
    return label;
  }

  std::string format_transfer_meter(const std::vector<uint8_t>& chunk_states,
                                    uint64_t downloaded,
                                    uint64_t total_size,
                                    std::size_t chunk_size) const {
    const std::size_t slots = std::max<std::size_t>(1, progress_meter_size_);
    static constexpr char kMeterChars[] = {' ', '.', '_', 'v', 'Y', 'X', 'H', '#'}; // do not remove this
    //static constexpr char kMeterChars[] = { ' ', '_', '-', '=', 'c', 'o', 'O', '0', '@' }; // do not remove this
    constexpr std::size_t kMeterCharCount = sizeof(kMeterChars) / sizeof(kMeterChars[0]);
    const std::size_t chunk_width = chunk_size == 0 ? 1 : chunk_size;
    std::string bar;
    bar.reserve(slots);
    if(total_size == 0 || chunk_states.empty()) {
      bar.assign(slots, '_');
    } else {
      const std::size_t total_chunks = chunk_states.size();
      auto scaled_position = [total_size, slots](std::size_t idx) -> uint64_t {
        if(slots == 0) return 0;
        uint64_t base = (total_size / slots) * idx;
        uint64_t remainder = (total_size % slots) * idx / slots;
        return base + remainder;
      };
      for(std::size_t slot = 0; slot < slots; ++slot) {
        uint64_t slot_start = scaled_position(slot);
        if(slot_start >= total_size) {
          bar.push_back('_');
          continue;
        }
        uint64_t slot_end = scaled_position(slot + 1);
        if(slot_end <= slot_start) slot_end = slot_start + 1;
        if(slot_end > total_size) slot_end = total_size;
        const uint64_t slot_len = slot_end - slot_start;
        uint64_t filled = 0;
        std::size_t chunk_idx = static_cast<std::size_t>(slot_start / chunk_width);
        if(chunk_idx >= total_chunks) {
          chunk_idx = total_chunks - 1;
        }
        uint64_t chunk_start = static_cast<uint64_t>(chunk_idx) * chunk_width;
        while(chunk_idx < total_chunks && chunk_start < slot_end) {
          uint64_t chunk_end = std::min<uint64_t>(chunk_start + chunk_width, total_size);
          if(chunk_states[chunk_idx]) {
            uint64_t overlap_start = std::max(slot_start, chunk_start);
            uint64_t overlap_end = std::min(slot_end, chunk_end);
            if(overlap_end > overlap_start) {
              filled += overlap_end - overlap_start;
            }
          }
          ++chunk_idx;
          chunk_start += chunk_width;
        }
        double ratio = slot_len == 0
          ? 0.0
          : static_cast<double>(filled) / static_cast<double>(slot_len);
        const double clamped = std::clamp(ratio, 0.0, 1.0);
        const double scaled = clamped * static_cast<double>(kMeterCharCount);
        std::size_t index = static_cast<std::size_t>(scaled);
        if(index >= kMeterCharCount) index = kMeterCharCount - 1;
        bar.push_back(kMeterChars[index]);
      }
    }
    double percent = total_size == 0
      ? 100.0
      : (static_cast<double>(std::min<uint64_t>(downloaded, total_size)) / static_cast<double>(total_size)) * 100.0;
    std::ostringstream oss;
    oss << bar << " " << std::fixed << std::setprecision(1) << percent << "%";
    return oss.str();
  }

  std::size_t current_chunk_size() const {
    return std::clamp<std::size_t>(swarm_chunk_size_, 4096, PeerManager::kMaxChunkSize);
  }

  static std::string format_duration_compact(std::chrono::steady_clock::duration elapsed) {
    double seconds = std::chrono::duration<double>(elapsed).count();
    double value = seconds;
    char unit = 's';
    if(value >= 60.0) {
      value /= 60.0;
      unit = 'm';
      if(value >= 60.0) {
        value /= 60.0;
        unit = 'h';
        if(value >= 24.0) {
          value /= 24.0;
          unit = 'd';
        }
      }
    }
    std::ostringstream oss;
    if(value >= 10.0) {
      oss << std::fixed << std::setprecision(0) << value << unit;
    } else {
      oss << std::fixed << std::setprecision(1) << value << unit;
    }
    return oss.str();
  }

  static std::string format_peer_stats_summary(const SwarmPeerStatsMap& stats) {
    if(stats.empty()) return "";
    std::vector<std::pair<std::string, SwarmPeerStats>> entries(stats.begin(), stats.end());
    std::sort(entries.begin(), entries.end(),
      [](const auto& a, const auto& b){
        if(a.second.bytes == b.second.bytes) return a.first < b.first;
        return a.second.bytes > b.second.bytes;
      });
    std::ostringstream oss;
    bool first = true;
    for(const auto& entry : entries) {
      if(!first) oss << ", ";
      first = false;
      oss << entry.first << "=" << format_size(entry.second.bytes);
    }
    return oss.str();
  }

  SwarmPeerProvider make_swarm_refresh_provider(const std::string& hash,
                                                const std::unordered_set<std::string>& allowed_peers,
                                                const std::shared_ptr<std::atomic<std::size_t>>& available_peer_count = nullptr) {
    if(hash.empty()) return {};
    return [this, hash, allowed_peers, available_peer_count](){
      std::vector<std::string> peers;
      auto listings = blocking_list_hash(hash);
      std::unordered_set<std::string> seen;
      for(const auto& item : listings) {
        if(item.peer_id.empty() || item.peer_id == pm_->local_peer_id()) continue;
        if(item.hash != hash) continue;
        if(!allowed_peers.empty() && !allowed_peers.count(item.peer_id)) continue;
        if(seen.insert(item.peer_id).second) {
          peers.push_back(item.peer_id);
        }
      }
      if(available_peer_count) {
        available_peer_count->store(peers.size());
      }
      return peers;
    };
  }

  static bool listing_contains_dir_stub(const std::vector<PeerManager::RemoteListingItem>& items,
                                        const std::string& guid) {
    if(guid.empty()) return true;
    for(const auto& item : items) {
      if(!item.is_directory) continue;
      if(item.directory_guid == guid && item.relative_path.empty()) {
        return true;
      }
    }
    return false;
  }

  SourceReachability check_watch_source(const WatchEntry& entry, const WatchEntry::Source& src) const {
    if(src.peer.empty()) return SourceReachability::Offline;
    if(src.peer == pm_->local_peer_id()) return SourceReachability::Online;
    PeerListingResult resp;
    if(!entry.dir_guid.empty()) {
      resp = fetch_peer_listing(src.peer, "", "", entry.dir_guid, true);
    } else {
      auto normalized = normalize_cli_path(src.path);
      if(normalized.empty()) return SourceReachability::Online;
      resp = fetch_peer_listing(src.peer, normalized, "", "", true);
    }
    switch(resp.state) {
      case PeerListingState::Completed:
        if(!entry.dir_guid.empty()) {
          return listing_contains_dir_stub(resp.items, entry.dir_guid)
            ? SourceReachability::Online
            : SourceReachability::Missing;
        }
        return SourceReachability::Online;
      case PeerListingState::Timeout: return SourceReachability::Timeout;
      case PeerListingState::NotConnected:
      default: return SourceReachability::Offline;
    }
  }

  std::map<std::string, SourceReachability> gather_watch_peer_status(const WatchEntry& entry) const {
    std::map<std::string, SourceReachability> status;
    for(const auto& src : entry.sources) {
      if(src.peer.empty()) continue;
      auto reachability = check_watch_source(entry, src);
      auto it = status.find(src.peer);
      if(it == status.end()) {
        status[src.peer] = reachability;
      } else {
        auto current_priority = reachability_priority(it->second);
        auto new_priority = reachability_priority(reachability);
        if(new_priority > current_priority) {
          it->second = reachability;
        }
      }
    }
    return status;
  }

  static std::string reachability_label(SourceReachability state) {
    switch(state) {
      case SourceReachability::Online: return "ok";
      case SourceReachability::Timeout: return "timeout";
      case SourceReachability::Missing: return "missing";
      case SourceReachability::Offline:
      default: return "down";
    }
  }

  static int reachability_priority(SourceReachability state) {
    switch(state) {
      case SourceReachability::Online: return 3;
      case SourceReachability::Timeout: return 2;
      case SourceReachability::Missing: return 1;
      case SourceReachability::Offline:
      default: return 0;
    }
  }

  void notify_untrusted_directory_sources(const std::vector<PeerManager::RemoteListingItem>& entries) {
    struct Notice {
      std::string watch_id;
      std::string peer;
      std::string guid;
      std::string path;
    };

    std::vector<Notice> notices;
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      for(const auto& item : entries) {
      if(!item.is_directory) continue;
      if(item.directory_guid.empty()) continue;
      if(item.relative_path.empty()) continue;
      if(item.peer_id.empty()) continue;
        auto* watch = find_watch_by_guid(item.directory_guid);
        if(!watch) continue;
        auto trusted = std::any_of(watch->sources.begin(), watch->sources.end(),
          [&](const WatchEntry::Source& src){
            return src.peer == item.peer_id;
          });
        if(trusted) continue;
        notices.push_back(Notice{watch->id, item.peer_id, item.directory_guid, item.relative_path});
      }
    }

    for(const auto& note : notices) {
      std::cout << "[watch " << note.watch_id << "] Peer " << note.peer
                << " advertises " << note.guid;
      if(!note.path.empty()) {
        std::cout << " (" << note.path << ")";
      }
      std::cout << ". Use 'watch add " << note.peer << ":/" << note.guid
                << "' to trust that source.\n";
    }
  }

  void maybe_warn_duplicate_hashes_for_guid(const WatchEntry* watch,
                                            const std::string& guid,
                                            const std::vector<PeerManager::RemoteListingItem>& entries) {
    if(!watch) return;
    std::string effective_guid = guid.empty() ? watch->dir_guid : guid;
    if(effective_guid.empty()) return;

    std::unordered_map<std::string, std::vector<std::string>> per_hash;
    for(const auto& item : entries) {
      if(item.is_directory) continue;
      if(item.hash.empty()) continue;
      auto normalized = normalize_cli_path(item.relative_path);
      if(normalized.empty()) normalized = item.relative_path;
      if(normalized.empty()) normalized = "(root)";
      per_hash[item.hash].push_back(normalized);
    }

    for(auto& [hash, paths] : per_hash) {
      std::sort(paths.begin(), paths.end());
      paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
      if(paths.size() <= 1) continue;

      std::cout << "[watch " << watch->id << "] hash " << hash.substr(0, 8)
                << " is advertised at multiple paths under " << effective_guid << ": ";
      bool first = true;
      for(const auto& path : paths) {
        if(!first) std::cout << ", ";
        first = false;
        std::cout << path;
      }
      std::cout << ". Review before accepting duplicates.\n";
    }
  }

  void handle_watch_command(const std::string& args) {
    std::string trimmed = args;
    trim(trimmed);
    if(trimmed.empty()) {
      std::cout << "Default watch interval: " << default_watch_interval_.count() << "s\n";
      list_watches();
      return;
    }

    std::istringstream iss(trimmed);
    std::string action;
    iss >> action;

    bool action_is_known = action == "list" || action == "add" || action == "remove" || action == "interval" || action == "set";
    if(!action_is_known) {
      iss.clear();
      iss.seekg(0);
      action = "add";
    }

    if(action == "list") {
      list_watches();
    } else if(action == "add") {
      std::string source;
      iss >> source;
      if(source.empty()) {
        std::cout << "Usage: watch add <resource> [dest]\n";
        return;
      }
      std::string dest_raw;
      std::getline(iss, dest_raw);
      trim(dest_raw);
      bool dest_is_dir = !dest_raw.empty() && dest_raw.back() == '/';
      if(dest_is_dir) dest_raw.pop_back();
      trim(dest_raw);
      std::optional<std::string> dest_override;
      if(!dest_raw.empty()) dest_override = normalize_cli_path(dest_raw);

      if(dest_override && is_internal_listing_path(*dest_override)) {
        std::cout << "Cannot sync into reserved staging path.\n";
        return;
      }

      std::string parse_error;
      auto resolved_target = resolve_watch_resource(source, parse_error);
      if(!resolved_target) {
        if(!parse_error.empty()) {
          std::cout << parse_error << "\n";
        } else {
          std::cout << "Unable to resolve watch target.\n";
        }
        return;
      }
      auto new_source = resolved_target->source;
      auto resolved_guid = resolved_target->resolved;

      if(resolved_guid.guid.empty()) {
        std::cout << "Directory GUID is required to track a watch.\n";
        return;
      }

      auto requested_local_root = requested_watch_destination(new_source, dest_override, resolved_guid.guid);
      WatchEntry snapshot;
      bool created = false;
      bool updated = false;
      std::string affected_id;

      {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        WatchEntry* target = nullptr;
        for(auto& entry : watches_) {
          std::string existing_dest = canonical_watch_destination(entry);
          if(!requested_local_root.empty() && !existing_dest.empty() &&
             existing_dest == requested_local_root && entry.dir_guid != resolved_guid.guid) {
            std::cout << "Local destination " << requested_local_root
                      << " is already used by watch " << entry.id
                      << ". Choose a different destination.\n";
            return;
          }
          if(entry.dir_guid == resolved_guid.guid) {
            bool same_dest = (existing_dest == requested_local_root) ||
                             (existing_dest.empty() && requested_local_root.empty());
            if(same_dest) {
              target = &entry;
              break;
            }
          }
        }

        if(!target) {
          WatchEntry fresh;
          fresh.id = generate_watch_id();
          fresh.dir_guid = resolved_guid.guid;
          fresh.origin_peer = !resolved_guid.origin_peer.empty() ? resolved_guid.origin_peer : new_source.peer;
          fresh.dest_path = dest_override ? *dest_override : "";
          fresh.dest_is_directory = dest_is_dir || fresh.dest_path.empty();
          fresh.interval = default_watch_interval_;
          fresh.next_run = std::chrono::steady_clock::now() + fresh.interval;
          fresh.sources.push_back(new_source);
          watches_.push_back(fresh);
          snapshot = watches_.back();
          affected_id = snapshot.id;
          created = true;
          updated = true;
          if(!fresh.dir_guid.empty() && !fresh.origin_peer.empty()) {
            pm_->set_directory_origin(fresh.dir_guid, fresh.origin_peer);
          }
        } else {
          affected_id = target->id;
          auto dup = std::find_if(target->sources.begin(), target->sources.end(),
            [&](const WatchEntry::Source& src){
              return src.peer == new_source.peer &&
                     normalize_cli_path(src.path) == normalize_cli_path(new_source.path);
            });
          if(dup != target->sources.end()) {
            std::cout << "Watch " << target->id << " already trusts " << new_source.peer;
            if(!new_source.path.empty()) std::cout << ":/" << new_source.path;
            std::cout << ".\n";
            return;
          }
          target->sources.push_back(new_source);
          target->next_run = std::chrono::steady_clock::now();
          if(!resolved_guid.guid.empty() && target->dir_guid.empty()) {
            target->dir_guid = resolved_guid.guid;
          }
          if(!resolved_guid.origin_peer.empty()) {
            target->origin_peer = resolved_guid.origin_peer;
          } else if(target->origin_peer.empty()) {
            target->origin_peer = target->sources.front().peer;
          }
          if(!target->dir_guid.empty() && !target->origin_peer.empty()) {
            pm_->set_directory_origin(target->dir_guid, target->origin_peer);
          }
          snapshot = *target;
          updated = true;
        }
      }

      if(!updated) return;

      if(!snapshot.dir_guid.empty()) {
        std::string local_root;
        if(!snapshot.dest_path.empty()) {
          local_root = snapshot.dest_path;
        } else if(!snapshot.sources.empty() && !snapshot.sources.front().path.empty()) {
          local_root = snapshot.sources.front().path;
        }
        if(!local_root.empty()) {
          remember_dir_guid_mapping(local_root, snapshot.dir_guid);
        }
        if(!snapshot.origin_peer.empty()) {
          pm_->set_directory_origin(snapshot.dir_guid, snapshot.origin_peer);
        }
      }

      save_watches();
      watch_cv_.notify_all();
      if(created) {
        std::cout << "Added watch " << affected_id << " for " << resolved_guid.guid << ".\n";
      } else {
        std::cout << "Updated watch " << affected_id << " with a new trusted source.\n";
      }
      auto result = run_single_watch(snapshot);
      if(!result.success) {
        std::cout << "[watch " << affected_id << "] sync failed.\n";
      } else if(result.changed) {
        std::cout << "[watch " << affected_id << "] synced.\n";
      }
    } else if(action == "set") {
      std::string id;
      iss >> id;
      trim(id);
      std::string remainder;
      std::getline(iss, remainder);
      trim(remainder);
      if(id.empty() || remainder.empty()) {
        std::cout << "Usage: watch set <id> <resource> [dest]\n";
        return;
      }

      WatchEntry existing_snapshot;
      {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        auto* entry = find_watch_by_id(id);
        if(!entry) {
          std::cout << "No watch found with id " << id << ".\n";
          return;
        }
        existing_snapshot = *entry;
      }
      if(existing_snapshot.sources.empty()) {
        std::cout << "Watch " << id << " has no sources to infer defaults from.\n";
        return;
      }

      std::istringstream src_stream(remainder);
      std::string source_token;
      src_stream >> source_token;
      if(source_token.empty()) {
        std::cout << "Usage: watch set <id> <resource> [dest]\n";
        return;
      }
      std::string dest_raw;
      std::getline(src_stream, dest_raw);
      trim(dest_raw);
      bool dest_is_dir = !dest_raw.empty() && dest_raw.back() == '/';
      if(dest_is_dir) dest_raw.pop_back();
      trim(dest_raw);
      std::optional<std::string> dest_override;
      if(!dest_raw.empty()) dest_override = normalize_cli_path(dest_raw);

      std::string default_peer = !existing_snapshot.origin_peer.empty()
        ? existing_snapshot.origin_peer
        : existing_snapshot.sources.front().peer;

      if(dest_override && is_internal_listing_path(*dest_override)) {
        std::cout << "Cannot sync into reserved staging path.\n";
        return;
      }

      std::string parse_error;
      auto resolved_target = resolve_watch_resource(source_token, parse_error, default_peer);
      if(!resolved_target) {
        if(!parse_error.empty()) {
          std::cout << parse_error << "\n";
        } else {
          std::cout << "Unable to resolve watch target.\n";
        }
        return;
      }
      auto new_source = resolved_target->source;
      auto resolved_guid = resolved_target->resolved;

      if(resolved_guid.guid.empty()) {
        std::cout << "Directory GUID is required to track a watch.\n";
        return;
      }

      WatchEntry snapshot;
      {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        auto* entry = find_watch_by_id(id);
        if(!entry) {
          std::cout << "No watch found with id " << id << ".\n";
          return;
        }
        if(dest_override) {
          auto desired_root = *dest_override;
          for(const auto& other : watches_) {
            if(other.id == entry->id) continue;
            auto existing_dest = canonical_watch_destination(other);
            if(!desired_root.empty() && !existing_dest.empty() && desired_root == existing_dest) {
              std::cout << "Local destination " << desired_root << " is already used by watch "
                        << other.id << " (" << other.dir_guid << "). Choose a different destination.\n";
              return;
            }
          }
          if(entry->dest_path.empty() || entry->dest_path == desired_root) {
            entry->dest_path = desired_root;
            entry->dest_is_directory = dest_is_dir || entry->dest_path.empty();
          } else {
            std::cout << "Watch " << entry->id << " already uses destination "
                      << entry->dest_path << ". Remove it first to change destinations.\n";
            return;
          }
        }
        entry->sources.clear();
        entry->sources.push_back(new_source);
        entry->dir_guid = resolved_guid.guid;
        entry->origin_peer = resolved_guid.origin_peer.empty() ? new_source.peer : resolved_guid.origin_peer;
        entry->next_run = std::chrono::steady_clock::now();
        snapshot = *entry;
      }

      if(!snapshot.dir_guid.empty()) {
        std::string local_root;
        if(!snapshot.dest_path.empty()) {
          local_root = snapshot.dest_path;
        } else if(!snapshot.sources.empty() && !snapshot.sources.front().path.empty()) {
          local_root = snapshot.sources.front().path;
        }
        if(!local_root.empty()) {
          remember_dir_guid_mapping(local_root, snapshot.dir_guid);
        }
        if(!snapshot.origin_peer.empty()) {
          pm_->set_directory_origin(snapshot.dir_guid, snapshot.origin_peer);
        }
      }

      save_watches();
      watch_cv_.notify_all();
      std::cout << "Watch " << id << " updated to follow " << new_source.peer;
      if(!new_source.path.empty()) {
        std::cout << ":/" << new_source.path;
      } else {
        std::cout << ":/" << resolved_guid.guid;
      }
      std::cout << ".\n";
      auto result = run_single_watch(snapshot);
      if(!result.success) {
        std::cout << "[watch " << id << "] sync failed.\n";
      } else if(result.changed) {
        std::cout << "[watch " << id << "] synced.\n";
      }
    } else if(action == "remove") {
      std::string id;
      iss >> id;
      trim(id);
      if(id.empty()) {
        std::cout << "Usage: watch remove <id|dir-guid|resource>\n";
        return;
      }

      bool changed = false;
      bool removed_watch = false;
      std::vector<std::string> removed_ids;

      if(id.find(':') != std::string::npos) {
        std::string parse_error;
        auto resolved_target = resolve_watch_resource(id, parse_error);
        if(!resolved_target) {
          if(!parse_error.empty()) {
            std::cout << parse_error << "\n";
          } else {
            std::cout << "Unable to resolve watch target.\n";
          }
          return;
        }
        auto target_guid = resolved_target->resolved.guid;
        if(target_guid.empty()) {
          std::cout << "Unable to resolve directory GUID for that watch target.\n";
          return;
        }
        auto target_peer = resolved_target->source.peer;

        {
          std::lock_guard<std::mutex> lock(watch_mutex_);
          for(auto it = watches_.begin(); it != watches_.end();) {
            if(it->dir_guid != target_guid) {
              ++it;
              continue;
            }
            auto peer_present = std::any_of(it->sources.begin(), it->sources.end(),
              [&](const WatchEntry::Source& src){ return src.peer == target_peer; });
            if(!peer_present && !target_peer.empty()) {
              ++it;
              continue;
            }
            removed_ids.push_back(it->id);
            it = watches_.erase(it);
            removed_watch = true;
            changed = true;
          }
        }

        if(!changed) {
          std::cout << "No watch found for that source.\n";
          return;
        }

        save_watches();
        watch_cv_.notify_all();
        if(!removed_ids.empty()) {
          for(const auto& rid : removed_ids) {
            std::cout << "Removed watch " << rid << "\n";
          }
        } else {
          std::cout << "Removed source from watch.\n";
        }
      } else {
        bool treat_as_guid = looks_like_dir_guid(id);
        {
          std::lock_guard<std::mutex> lock(watch_mutex_);
          auto matcher = [&](const WatchEntry& entry){
            return treat_as_guid ? entry.dir_guid == id : entry.id == id;
          };
          auto it = std::remove_if(watches_.begin(), watches_.end(), matcher);
          if(it != watches_.end()) {
            for(auto iter = it; iter != watches_.end(); ++iter) {
              removed_ids.push_back(iter->id);
            }
            watches_.erase(it, watches_.end());
            changed = true;
            removed_watch = true;
          }
        }

        if(!changed) {
          std::cout << "No watch found with that identifier.\n";
          return;
        }

        save_watches();
        watch_cv_.notify_all();
        for(const auto& rid : removed_ids) {
          std::cout << "Removed watch " << rid << "\n";
        }
      }
    } else if(action == "interval") {
      std::string value;
      iss >> value;
      if(value.empty()) {
        std::cout << "Default watch interval: " << default_watch_interval_.count() << "s\n";
        return;
      }
      int seconds = 0;
      try {
        seconds = std::stoi(value);
      } catch(...) {
        std::cout << "Invalid interval value.\n";
        return;
      }
      if(seconds <= 0) {
        std::cout << "Interval must be positive seconds.\n";
        return;
      }
      default_watch_interval_ = std::chrono::seconds(seconds);
      {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        auto now = std::chrono::steady_clock::now();
        for(auto& watch : watches_) {
          watch.interval = default_watch_interval_;
          watch.next_run = now;
        }
      }
      save_watches();
      watch_cv_.notify_all();
      std::cout << "Default watch interval set to " << seconds << "s\n";
    } else {
      std::cout << "Unknown watch command.\n";
    }
  }

  void handle_settings_command(const std::string& args) {
    if(!settings_) {
      std::cout << "Settings manager unavailable.\n";
      return;
    }

    if(args.empty()) {
      list_settings();
      return;
    }

    std::istringstream iss(args);
    std::string action;
    iss >> action;

    if(action == "list") {
      list_settings();
      return;
    }

    if(action == "get") {
      std::string key;
      iss >> key;
      if(key.empty()) {
        std::cout << "Usage: settings get <key>\n";
        return;
      }
      auto resolved = settings_->resolve_key(key);
      if(!resolved) {
        std::cout << "Unknown setting '" << key << "'.\n";
        return;
      }
      std::cout << *resolved << " = " << settings_->value_as_string(*resolved) << "\n";
      return;
    }

    if(action == "set") {
      std::string key;
      iss >> key;
      if(key.empty()) {
        std::cout << "Usage: settings set <key> <value>\n";
        return;
      }
      std::string value;
      std::getline(iss, value);
      trim(value);
      if(value.empty()) {
        std::cout << "Usage: settings set <key> <value>\n";
        return;
      }
      auto resolved = settings_->resolve_key(key);
      if(!resolved) {
        std::cout << "Unknown setting '" << key << "'.\n";
        return;
      }
      std::string error;
      if(settings_->set_from_string(*resolved, value, error)) {
        apply_setting_side_effects(*resolved);
        std::cout << *resolved << " = " << settings_->value_as_string(*resolved) << "\n";
      } else {
        std::cout << "Failed to set " << *resolved << ": " << error << "\n";
      }
      return;
    }

    if(action == "save") {
      if(settings_->save()) {
        std::cout << "Saved settings to " << settings_->settings_path() << "\n";
      } else {
        std::cout << "Failed to save settings.\n";
      }
      return;
    }

    if(action == "load") {
      if(settings_->load()) {
        apply_setting_side_effects("audio_notifications");
        apply_setting_side_effects("transfer_debug");
        apply_setting_side_effects("transfer_progress");
        apply_setting_side_effects("progress_meter_size");
        apply_setting_side_effects("swarm_max_parallel");
        apply_setting_side_effects("swarm_chunk_buffers");
        apply_setting_side_effects("swarm_chunk_size");
        apply_setting_side_effects("swarm_progress_interval_ms");
        std::cout << "Loaded settings from " << settings_->settings_path() << "\n";
      } else {
        std::cout << "Settings file not found; defaults restored.\n";
      }
      return;
    }

    std::cout << "Unknown settings command.\n";
  }

  void list_settings() {
    if(!settings_) return;
    auto keys = settings_->keys();
    std::sort(keys.begin(), keys.end());
    for(const auto& key : keys) {
      std::cout << key << " = " << settings_->value_as_string(key) << "\n";
    }
  }

  void apply_setting_side_effects(const std::string& key) {
    if(!settings_) return;
    if(key == "audio_notifications") {
      try {
        audio_notifications_ = settings_->get<bool>("audio_notifications");
      } catch(...) {
        // ignore
      }
    } else if(key == "transfer_debug") {
      try {
        bool enabled = settings_->get<bool>("transfer_debug");
        if(pm_) {
          pm_->set_transfer_debug(enabled);
        }
      } catch(...) {
        // ignore
      }
    } else if(key == "transfer_progress") {
      try {
        transfer_progress_enabled_ = settings_->get<bool>("transfer_progress");
      } catch(...) {
        // ignore
      }
    } else if(key == "progress_meter_size") {
      try {
        int width = settings_->get<int>("progress_meter_size");
        width = std::clamp(width, 10, 400);
        progress_meter_size_ = static_cast<std::size_t>(width);
      } catch(...) {
        // ignore
      }
    } else if(key == "swarm_max_parallel") {
      try {
        int value = settings_->get<int>("swarm_max_parallel");
        if(value < 0) value = 0;
        swarm_max_parallel_ = static_cast<std::size_t>(value);
      } catch(...) {
        // ignore
      }
    } else if(key == "swarm_chunk_buffers") {
      try {
        int value = settings_->get<int>("swarm_chunk_buffers");
        if(value < 1) value = 1;
        swarm_chunk_buffers_ = static_cast<std::size_t>(value);
      } catch(...) {
        // ignore
      }
    } else if(key == "swarm_chunk_size") {
      try {
        int value = settings_->get<int>("swarm_chunk_size");
        if(value <= 0) value = static_cast<int>(PeerManager::kMaxChunkSize);
        int clamped = std::clamp(value, 4096, static_cast<int>(PeerManager::kMaxChunkSize));
        swarm_chunk_size_ = static_cast<std::size_t>(clamped);
      } catch(...) {
        // ignore
      }
    } else if(key == "swarm_progress_interval_ms") {
      try {
        int value = settings_->get<int>("swarm_progress_interval_ms");
        if(value < 50) value = 50;
        swarm_progress_interval_ = std::chrono::milliseconds(value);
      } catch(...) {
        // ignore
      }
    }
  }

  void handle_conflict_command(const std::string& args) {
    if(args.empty()) {
      list_conflicts();
      return;
    }

    std::istringstream iss(args);
    std::string action;
    iss >> action;

    if(action == "list") {
      list_conflicts();
    } else if(action == "accept") {
      std::string key;
      iss >> key;
      if(key.empty()) {
        std::cout << "Usage: conflict accept <path|hash|dir-guid>\n";
        return;
      }
      if(apply_conflict_accept(key)) {
        std::cout << "Accepted pending conflicts for " << key << ".\n";
      } else {
        std::cout << "No conflicts accepted for " << key << ".\n";
      }
    } else if(action == "stage") {
      std::string key;
      iss >> key;
      if(key.empty()) {
        std::cout << "Usage: conflict stage <path|hash|dir-guid>\n";
        return;
      }
      if(stage_conflict(key)) {
        std::filesystem::path staged_path;
        {
          std::lock_guard<std::mutex> lock(conflict_mutex_);
          if(staged_conflict_) staged_path = staged_conflict_->staged_path;
        }
        if(!staged_path.empty()) {
          std::cout << "Staged conflict for " << key << " at " << staged_path
                    << ". Use 'conflict view' to open.\n";
        } else {
          std::cout << "Staged conflict for " << key << ". Use 'conflict view' to open.\n";
        }
      }
    } else if(action == "view") {
      if(!view_staged_conflict()) {
        std::cout << "No staged conflict to view.\n";
      }
    } else if(action == "unstage") {
      if(unstage_conflict()) {
        std::cout << "Cleared staged conflict preview.\n";
      } else {
        std::cout << "No staged conflict to clear.\n";
      }
    } else if(action == "ignore") {
      std::string key;
      iss >> key;
      if(key.empty()) {
        std::cout << "Usage: conflict ignore <path|hash|dir-guid>\n";
        return;
      }
      if(apply_conflict_ignore(key)) {
        std::cout << "Ignoring future conflicts for " << key << ".\n";
      } else {
        std::cout << "No conflicts matched " << key << ".\n";
      }
    } else {
      std::cout << "Unknown conflict command.\n";
    }
  }

  void list_conflicts() {
    std::vector<ConflictEntry> snapshot;
    {
      std::lock_guard<std::mutex> lock(conflict_mutex_);
      snapshot = conflicts_;
    }
    if(snapshot.empty()) {
      std::cout << "No conflicts pending.\n";
      return;
    }

    std::optional<StagedConflict> staged_copy;
    {
      std::lock_guard<std::mutex> lock(conflict_mutex_);
      staged_copy = staged_conflict_;
    }

    std::size_t index = 1;
    for(const auto& entry : snapshot) {
      std::string hash_short = entry.hash.size() > 12 ? entry.hash.substr(0, 12) + "..." : entry.hash;
      std::string when = format_timestamp(entry.detected_at);
      std::cout << index++ << ". " << entry.relative_path
                << "  hash=" << hash_short;
      if(!entry.dir_guid.empty()) std::cout << "  dir=" << entry.dir_guid;
      if(!entry.origin_peer.empty()) std::cout << "  from=" << entry.origin_peer;
      if(staged_copy && staged_copy->conflict_id == entry.id) {
        std::cout << "  [staged -> " << staged_copy->staged_path << "]";
      }
      std::cout << "  detected=" << when << "\n";
    }
  }

  bool apply_conflict_accept(const std::string& key) {
    auto matches = collect_conflicts_matching(key);
    if(matches.empty()) return false;

    bool any_applied = false;
    for(const auto& conflict : matches) {
      if(force_conflict_download(conflict)) {
        remove_conflict_by_id(conflict.id);
        any_applied = true;
      }
    }
    return any_applied;
  }

  bool apply_conflict_ignore(const std::string& key) {
    auto classified = classify_ignore_key(key);
    add_ignore_entry(key);

    auto predicate = [&](const ConflictEntry& entry){
      switch(classified.first) {
        case IgnoreKind::Hash:
          return entry.hash == classified.second;
        case IgnoreKind::DirGuid:
          return entry.dir_guid == classified.second;
        case IgnoreKind::Path:
          if(classified.second.empty()) return entry.relative_path.empty();
          if(entry.relative_path == classified.second) return true;
          return entry.relative_path.rfind(classified.second + "/", 0) == 0;
      }
      return false;
    };
    remove_conflicts_matching(predicate);
    return true;
  }

  std::vector<ConflictEntry> collect_conflicts_matching(const std::string& key) {
    bool is_hash = looks_like_hash(key);
    bool is_dir = key.rfind("dir-", 0) == 0;
    auto normalized = key;
    if(!is_hash && !is_dir) normalized = normalize_cli_path(key);

    std::vector<ConflictEntry> result;
    std::lock_guard<std::mutex> lock(conflict_mutex_);
    for(const auto& entry : conflicts_) {
      bool matches = false;
      if(is_hash) {
        matches = entry.hash == normalized;
      } else if(is_dir) {
        matches = entry.dir_guid == normalized;
      } else if(normalized.empty()) {
        matches = entry.relative_path.empty();
      } else if(entry.relative_path == normalized) {
        matches = true;
      } else {
        matches = entry.relative_path.rfind(normalized + "/", 0) == 0;
      }
      if(matches) result.push_back(entry);
    }
    return result;
  }

  void handle_ignore_command(const std::string& args) {
    if(args.empty()) {
      list_ignore_entries();
      return;
    }

    std::istringstream iss(args);
    std::string action;
    iss >> action;

    if(action == "list") {
      list_ignore_entries();
    } else if(action == "add") {
      std::string key;
      iss >> key;
      if(key.empty()) {
        std::cout << "Usage: ignore add <path|hash|dir-guid>\n";
        return;
      }
      if(add_ignore_entry(key)) {
        std::cout << "Added ignore for " << key << ".\n";
      } else {
        std::cout << key << " was already ignored.\n";
      }
    } else if(action == "remove") {
      std::string key;
      iss >> key;
      if(key.empty()) {
        std::cout << "Usage: ignore remove <path|hash|dir-guid>\n";
        return;
      }
      if(remove_ignore_entry(key)) {
        std::cout << "Removed ignore for " << key << ".\n";
      } else {
        std::cout << "No ignore entry matched " << key << ".\n";
      }
    } else {
      std::cout << "Unknown ignore command.\n";
    }
  }

  void list_ignore_entries() {
    IgnoreConfig snapshot;
    {
      std::lock_guard<std::mutex> guard(ignore_mutex_);
      snapshot = ignore_config_;
    }
    if(snapshot.paths.empty() && snapshot.hashes.empty() && snapshot.dir_guids.empty()) {
      std::cout << "No ignore rules configured.\n";
      return;
    }
    if(!snapshot.paths.empty()) {
      std::cout << "Paths:\n";
      for(const auto& path : snapshot.paths) {
        std::cout << "  " << (path.empty() ? "/" : path) << "\n";
      }
    }
    if(!snapshot.dir_guids.empty()) {
      std::cout << "Directory GUIDs:\n";
      for(const auto& guid : snapshot.dir_guids) {
        std::cout << "  " << guid << "\n";
      }
    }
    if(!snapshot.hashes.empty()) {
      std::cout << "Hashes:\n";
      for(const auto& hash : snapshot.hashes) {
        std::cout << "  " << hash << "\n";
      }
    }
  }

  std::pair<IgnoreKind, std::string> classify_ignore_key(const std::string& raw) const {
    if(looks_like_hash(raw)) {
      return {IgnoreKind::Hash, raw};
    }
    if(raw.rfind("dir-", 0) == 0) {
      return {IgnoreKind::DirGuid, raw};
    }
    return {IgnoreKind::Path, normalize_cli_path(raw)};
  }

  bool add_ignore_entry(const std::string& raw_key) {
    auto [kind, normalized] = classify_ignore_key(raw_key);
    std::lock_guard<std::mutex> guard(ignore_mutex_);
    bool inserted = false;
    switch(kind) {
      case IgnoreKind::Hash:
        inserted = ignore_config_.hashes.insert(normalized).second;
        break;
      case IgnoreKind::DirGuid:
        inserted = ignore_config_.dir_guids.insert(normalized).second;
        break;
      case IgnoreKind::Path:
        inserted = ignore_config_.paths.insert(normalized).second;
        break;
    }
    if(inserted) save_ignore_config();
    return inserted;
  }

  bool remove_ignore_entry(const std::string& raw_key) {
    auto [kind, normalized] = classify_ignore_key(raw_key);
    std::lock_guard<std::mutex> guard(ignore_mutex_);
    bool removed = false;
    switch(kind) {
      case IgnoreKind::Hash:
        removed = ignore_config_.hashes.erase(normalized) > 0;
        break;
      case IgnoreKind::DirGuid:
        removed = ignore_config_.dir_guids.erase(normalized) > 0;
        break;
      case IgnoreKind::Path:
        removed = ignore_config_.paths.erase(normalized) > 0;
        break;
    }
    if(removed) save_ignore_config();
    return removed;
  }

  void handle_guid_command(const std::string& args) {
    if(args.empty()) {
      (void)index_share_root();
      list_dir_guids(false);
      return;
    }

    std::istringstream iss(args);
    std::string action;
    iss >> action;

    auto refresh_dir_state = [this](){
      (void)index_share_root();
    };

    if(action == "list") {
      refresh_dir_state();
      list_dir_guids(false);
    } else if(action == "orphans") {
      refresh_dir_state();
      list_dir_guids(true);
    } else if(action == "assign") {
      std::string guid;
      iss >> guid;
      std::string path;
      std::getline(iss, path);
      trim(path);
      if(guid.empty() || path.empty()) {
        std::cout << "Usage: guid assign <guid> <relative-path>\n";
        return;
      }
      assign_dir_guid(guid, path);
    } else if(action == "forget") {
      std::string guid;
      iss >> guid;
      if(guid.empty()) {
        std::cout << "Usage: guid forget <guid>\n";
        return;
      }
      forget_dir_guid(guid);
    } else {
      std::cout << "Unknown guid command. Usage: guid list|orphans|assign <guid> <path>|forget <guid>\n";
    }
  }

  void list_dir_guids(bool orphans_only) {
    std::vector<DirGuidRecord> entries;
    {
      std::lock_guard<std::mutex> lock(dir_guid_mutex_);
      entries.reserve(dir_guid_records_.size());
      for(const auto& [guid, record] : dir_guid_records_) {
        if(orphans_only && record.active) continue;
        entries.push_back(record);
      }
    }

    if(entries.empty()) {
      if(orphans_only) {
        std::cout << "No orphaned directory GUIDs.\n";
      } else {
        std::cout << "No directory GUIDs recorded yet.\n";
      }
      return;
    }

    std::sort(entries.begin(), entries.end(),
      [](const DirGuidRecord& a, const DirGuidRecord& b){
        if(a.active != b.active) return a.active > b.active;
        if(a.path != b.path) return a.path < b.path;
        return a.guid < b.guid;
      });

    std::cout << std::left << std::setw(42) << "GUID"
              << "  " << std::setw(30) << "PATH" << "  STATUS\n";
    for(const auto& entry : entries) {
      std::string path = entry.path.empty() ? "-" : entry.path;
      std::string status = entry.active ? "present" : "orphan";
      std::cout << std::left << std::setw(42) << entry.guid
                << "  " << std::setw(30) << path
                << "  " << status;
      if(auto annotation = watch_annotation_for_guid(entry.guid)) {
        if(annotation->preferred) {
          std::cout << "  watch:(" << *annotation->preferred << ")";
          if(annotation->fallback) {
            std::cout << " fallback:(" << *annotation->fallback << ")";
          } else if(annotation->fallback_unavailable) {
            std::cout << " fallback:(unavailable)";
          }
        }
      }
      std::cout << "\n";
    }
    if(!orphans_only && has_orphaned_dir_guids()) {
      std::cout << "Use 'guid orphans' to view GUIDs whose directories are missing.\n";
    }
  }

  void assign_dir_guid(const std::string& guid, const std::string& user_path) {
    auto normalized = normalize_relative_string(user_path);
    if(normalized.empty()) {
      std::cout << "Path must refer to a directory under share/.\n";
      return;
    }
    if(is_internal_listing_path(normalized)) {
      std::cout << "Cannot assign GUIDs to internal directories.\n";
      return;
    }
    auto absolute = share_root() / normalized;
    std::error_code ec;
    if(!std::filesystem::exists(absolute, ec) || !std::filesystem::is_directory(absolute, ec)) {
      std::cout << normalized << " does not exist or is not a directory.\n";
      return;
    }

    std::optional<DirGuidRecord> displaced;
    {
      std::lock_guard<std::mutex> lock(dir_guid_mutex_);
      auto rec_it = dir_guid_records_.find(guid);
      if(rec_it == dir_guid_records_.end()) {
        if(!looks_like_dir_guid(guid)) {
          std::cout << "GUID must look like dir-xxxxxxxx...; received " << guid << "\n";
          return;
        }
        DirGuidRecord record;
        record.guid = guid;
        record.active = false;
        dir_guid_records_[guid] = record;
        rec_it = dir_guid_records_.find(guid);
      } else if(rec_it->second.active && !rec_it->second.path.empty()) {
        auto existing_absolute = share_root() / rec_it->second.path;
        std::error_code exists_ec;
        bool still_exists = std::filesystem::exists(existing_absolute, exists_ec) &&
                            std::filesystem::is_directory(existing_absolute, exists_ec);
        if(still_exists) {
          std::cout << "GUID " << guid << " is still attached to '" << rec_it->second.path
                    << "'. Use 'guid forget " << guid << "' first or move that directory away.\n";
          return;
        }
      }
      // Remove any existing mapping for this path.
      auto path_it = dir_guid_by_path_.find(normalized);
      if(path_it != dir_guid_by_path_.end() && path_it->second != guid) {
        auto displaced_it = dir_guid_records_.find(path_it->second);
        if(displaced_it != dir_guid_records_.end()) {
          displaced_it->second.active = false;
          displaced = displaced_it->second;
        }
        dir_guid_by_path_.erase(path_it);
      }

      if(!rec_it->second.path.empty()) {
        dir_guid_by_path_.erase(rec_it->second.path);
      }
      rec_it->second.path = normalized;
      rec_it->second.active = true;
      dir_guid_by_path_[normalized] = guid;
      dir_guid_registry_dirty_ = true;
    }

    pm_->register_directory_with_guid(normalized, guid);
    save_dir_guid_registry();

    std::cout << "Mapped GUID " << guid << " to " << normalized << ".\n";
    if(displaced) {
      std::cout << "Previous GUID " << displaced->guid << " for " << displaced->path
                << " is now orphaned.\n";
    }
  }

  void forget_dir_guid(const std::string& guid) {
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(dir_guid_mutex_);
      auto it = dir_guid_records_.find(guid);
      if(it == dir_guid_records_.end()) {
        std::cout << "Unknown GUID " << guid << ".\n";
        return;
      }
      if(!it->second.path.empty()) {
        dir_guid_by_path_.erase(it->second.path);
      }
      dir_guid_records_.erase(it);
      dir_guid_registry_dirty_ = true;
      removed = true;
    }

    if(removed) {
      pm_->unregister_directory_guid(guid);
      save_dir_guid_registry();
      std::cout << "Forgot GUID " << guid << ".\n";
    }
  }

  void maybe_suggest_guid_mapping(const std::vector<PeerManager::RemoteListingItem>& items) {
    const std::string local_peer = pm_->local_peer_id();
    std::vector<std::string> unguided_dirs;
    {
      std::lock_guard<std::mutex> lock(dir_guid_mutex_);
      for(const auto& item : items) {
        if(!item.is_directory) continue;
        if(item.peer_id != local_peer) continue;
        if(item.relative_path.empty()) continue;
        if(is_internal_listing_path(item.relative_path)) continue;
        if(!item.directory_guid.empty()) continue;
        auto normalized = normalize_relative_string(item.relative_path);
        if(normalized.empty()) continue;
        if(dir_guid_by_path_.count(normalized)) continue;
        unguided_dirs.push_back(normalized);
      }
    }
    if(unguided_dirs.empty()) return;

    auto orphans = orphaned_dir_guids();
    if(orphans.empty()) return;

    struct Suggestion { std::string guid; std::string path; };
    std::vector<Suggestion> suggestions;
    std::unordered_set<std::string> used_guids;

    for(const auto& path : unguided_dirs) {
      std::string best_guid;
      auto path_base = basename_for_relative_path(path);
      for(const auto& orphan : orphans) {
        if(used_guids.count(orphan.guid)) continue;
        auto orphan_base = basename_for_relative_path(orphan.path);
        if(!path_base.empty() && !orphan_base.empty() && path_base == orphan_base) {
          best_guid = orphan.guid;
          break;
        }
      }
      if(best_guid.empty()) {
        for(const auto& orphan : orphans) {
          if(used_guids.count(orphan.guid)) continue;
          best_guid = orphan.guid;
          break;
        }
      }
      if(best_guid.empty()) break;
      used_guids.insert(best_guid);
      suggestions.push_back({best_guid, path});
      if(used_guids.size() == orphans.size()) break;
    }

    if(suggestions.empty()) return;

    std::cout << "[guid] Detected directories without GUIDs that might match orphaned GUIDs:\n";
    for(const auto& suggestion : suggestions) {
      std::cout << "  guid assign " << suggestion.guid << " " << suggestion.path << "\n";
    }
    std::cout << "Execute the appropriate command above (or use 'guid orphans') to preserve rename history.\n";
  }

  void remove_conflicts_matching(const std::function<bool(const ConflictEntry&)>& predicate) {
    std::lock_guard<std::mutex> lock(conflict_mutex_);
    for(const auto& entry : conflicts_) {
      if(predicate(entry)) {
        clear_staged_conflict_if_matches(entry.id, true);
      }
    }
    conflicts_.erase(std::remove_if(conflicts_.begin(), conflicts_.end(),
      [&](const ConflictEntry& entry){ return predicate(entry); }),
      conflicts_.end());
  }

  void remove_conflict_by_id(const std::string& id) {
    remove_conflicts_matching([&](const ConflictEntry& entry){ return entry.id == id; });
  }

  void add_conflict(const std::string& relative_path,
                    const std::string& hash,
                    const std::string& dir_guid,
                    const std::string& origin_peer) {
    if(is_conflict_ignored(relative_path, hash, dir_guid)) return;

    ConflictEntry entry;
    entry.id = generate_conflict_id();
    entry.relative_path = relative_path;
    entry.hash = hash;
    entry.dir_guid = dir_guid;
    entry.origin_peer = origin_peer;
    entry.detected_at = std::chrono::system_clock::now();

    bool inserted = false;
    {
      std::lock_guard<std::mutex> lock(conflict_mutex_);
      auto it = std::find_if(conflicts_.begin(), conflicts_.end(),
        [&](const ConflictEntry& existing){
          return existing.relative_path == entry.relative_path && existing.hash == entry.hash;
        });
      if(it == conflicts_.end()) {
        conflicts_.push_back(entry);
        inserted = true;
      }
    }

    if(inserted) {
      std::cout << "Conflict queued for " << relative_path
                << ". Use 'conflict accept " << relative_path
                << "' to overwrite or 'conflict ignore " << relative_path
                << "' to skip.\n";
    }
  }

  bool is_conflict_ignored(const std::string& path,
                           const std::string& hash,
                           const std::string& dir_guid) const {
    std::lock_guard<std::mutex> guard(ignore_mutex_);
    if(ignore_config_.hashes.count(hash)) return true;
    if(!dir_guid.empty() && ignore_config_.dir_guids.count(dir_guid)) return true;
    if(ignore_config_.paths.count(path)) return true;
    return false;
  }

  bool force_conflict_download(const ConflictEntry& conflict) {
    auto providers = blocking_list_hash(conflict.hash);
    if(providers.empty()) {
      std::cout << "No providers available for hash " << conflict.hash << ".\n";
      return false;
    }

    auto primary_it = std::find_if(providers.begin(), providers.end(),
      [&](const PeerManager::RemoteListingItem& item){
        return item.peer_id != pm_->local_peer_id();
      });
    if(primary_it == providers.end()) primary_it = providers.begin();
    PeerManager::RemoteListingItem primary = *primary_it;

    auto dest_relative = std::filesystem::path(conflict.relative_path);
    auto final_path = share_root() / dest_relative;
    archive_existing_file(final_path, dest_relative);

    auto outcome = download_file_from_providers(primary, providers, dest_relative, false, nullptr);
    if(!outcome.success) {
      std::cout << "Failed to download " << conflict.relative_path << ".\n";
      return false;
    }
    clear_staged_conflict_if_matches(conflict.id, true);
    return true;
  }

  void archive_existing_file(const std::filesystem::path& final_path,
                             const std::filesystem::path& relative_path) {
    std::error_code ec;
    if(!std::filesystem::exists(final_path, ec)) return;

    auto destination = archive_root() / relative_path;
    ensure_directory_exists(destination.parent_path());
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    std::string archive_name = destination.generic_string() + "." + std::to_string(timestamp);
    std::filesystem::path archive_path = archive_name;

    std::filesystem::rename(final_path, archive_path, ec);
    if(ec) {
      std::filesystem::copy_file(final_path, archive_path,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      std::filesystem::remove(final_path, ec);
    }
  }

  bool stage_conflict(const std::string& key) {
    auto matches = collect_conflicts_matching(key);
    if(matches.empty()) {
      std::cout << "No conflicts matched " << key << ".\n";
      return false;
    }
    auto conflict = matches.front();
    auto providers = blocking_list_hash(conflict.hash);
    if(providers.empty()) {
      std::cout << "No providers available for hash " << conflict.hash << ".\n";
      return false;
    }
    auto primary_it = std::find_if(providers.begin(), providers.end(),
      [&](const PeerManager::RemoteListingItem& item){
        return item.peer_id != pm_->local_peer_id();
      });
    if(primary_it == providers.end()) primary_it = providers.begin();
    PeerManager::RemoteListingItem primary = *primary_it;

    unstage_conflict(); // clear any existing preview

    std::string sanitized = conflict.relative_path;
    if(sanitized.empty()) sanitized = conflict.hash;
    std::replace(sanitized.begin(), sanitized.end(), '/', '_');
    if(sanitized.empty()) sanitized = "staged";
    auto stage_name = timestamp_for_filename() + "." + sanitized;
    auto stage_path = conflict_stage_root() / stage_name;

    if(!download_conflict_to_path(primary, providers, stage_path, false)) {
      std::error_code ec;
      std::filesystem::remove(stage_path, ec);
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(conflict_mutex_);
      staged_conflict_ = StagedConflict{
        conflict.id,
        conflict.relative_path,
        conflict.hash,
        conflict.dir_guid,
        stage_path
      };
    }
    return true;
  }

  bool view_staged_conflict() {
    std::filesystem::path staged_path;
    {
      std::lock_guard<std::mutex> lock(conflict_mutex_);
      if(!staged_conflict_) return false;
      staged_path = staged_conflict_->staged_path;
    }
    std::error_code ec;
    if(!std::filesystem::exists(staged_path, ec)) {
      std::cout << "Staged file missing: " << staged_path << "\n";
      return false;
    }

    std::string command;
#if defined(_WIN32)
    command = "start \"\" \"" + staged_path.string() + "\"";
#elif defined(__APPLE__)
    command = "open \"" + staged_path.string() + "\"";
#else
    command = "xdg-open \"" + staged_path.string() + "\"";
#endif
    int rc = std::system(command.c_str());
    if(rc != 0) {
      std::cout << "Unable to launch viewer (code " << rc << "). Inspect manually: "
                << staged_path << "\n";
    }
    return true;
  }

  bool unstage_conflict() {
    std::filesystem::path staged_path;
    {
      std::lock_guard<std::mutex> lock(conflict_mutex_);
      if(!staged_conflict_) return false;
      staged_path = staged_conflict_->staged_path;
      staged_conflict_.reset();
    }
    std::error_code ec;
    std::filesystem::remove(staged_path, ec);
    return true;
  }

  void clear_staged_conflict_if_matches(const std::string& conflict_id, bool remove_file) {
    std::optional<std::filesystem::path> to_remove;
    {
      std::lock_guard<std::mutex> lock(conflict_mutex_);
      if(!staged_conflict_ || staged_conflict_->conflict_id != conflict_id) return;
      if(remove_file) to_remove = staged_conflict_->staged_path;
      staged_conflict_.reset();
    }
    if(to_remove) {
      std::error_code ec;
      std::filesystem::remove(*to_remove, ec);
    }
  }

  bool download_conflict_to_path(const PeerManager::RemoteListingItem& primary,
                                 const std::vector<PeerManager::RemoteListingItem>& providers,
                                 const std::filesystem::path& dest_path,
                                 bool quiet) {
    if(primary.hash.empty()) return false;

    std::vector<PeerManager::RemoteListingItem> remote_providers;
    for(const auto& provider : providers) {
      if(provider.peer_id == pm_->local_peer_id()) continue;
      remote_providers.push_back(provider);
    }
    if(remote_providers.empty()) remote_providers = providers;
    if(remote_providers.empty()) {
      if(!quiet) std::cout << "No peers available for hash " << primary.hash << "\n";
      return false;
    }

    uint64_t file_size = primary.size;
    if(file_size == 0) {
      for(const auto& provider : remote_providers) {
        if(provider.size > 0) {
          file_size = provider.size;
          break;
        }
      }
    }
    if(file_size == 0) {
      if(!quiet) std::cout << "Unknown file size for hash " << primary.hash << "\n";
      return false;
    }

    ensure_directory_exists(dest_path.parent_path());
    std::fstream out(dest_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if(!out) {
      if(!quiet) std::cout << "Cannot open staging file: " << dest_path << "\n";
      return false;
    }
    if(file_size > 0) {
      char zero = 0;
      out.seekp(static_cast<std::streamoff>(file_size - 1));
      out.write(&zero, 1);
      out.seekp(0);
    }

    const std::size_t chunk_size = current_chunk_size();
    const std::size_t total_chunks = static_cast<std::size_t>((file_size + chunk_size - 1) / chunk_size);
    std::vector<uint8_t> chunk_states(total_chunks, 0);
    const bool enable_meter = transfer_progress_enabled_ && !quiet && total_chunks > 0;
    std::mutex file_mutex;
    auto writer = [&](uint64_t offset, const std::vector<char>& data) -> bool {
      std::lock_guard<std::mutex> lock(file_mutex);
      out.seekp(static_cast<std::streamoff>(offset));
      out.write(data.data(), static_cast<std::streamsize>(data.size()));
      return static_cast<bool>(out);
    };
    SwarmConfig swarm_config;
    swarm_config.chunk_size = chunk_size;
    swarm_config.total_chunks = total_chunks;
    swarm_config.max_parallel = swarm_max_parallel_;
    swarm_config.chunk_buffers = swarm_chunk_buffers_;
    swarm_config.progress_interval = swarm_progress_interval_;
    swarm_config.enable_meter = enable_meter;
    auto peer_total = std::make_shared<std::atomic<std::size_t>>(0);
    auto active_peers = std::make_shared<std::atomic<std::size_t>>(0);
    auto seed_peer_counts = [&](const std::vector<PeerManager::RemoteListingItem>& items){
      std::unordered_set<std::string> unique;
      for(const auto& item : items) {
        if(item.peer_id.empty()) continue;
        unique.insert(item.peer_id);
      }
      auto count = unique.size();
      peer_total->store(count);
    };
    seed_peer_counts(remote_providers);
    auto refresh_provider = make_swarm_refresh_provider(primary.hash, {}, peer_total);
    swarm_config.enable_dynamic_providers = static_cast<bool>(refresh_provider);
    swarm_config.provider_refresh_interval = swarm_provider_refresh_interval_;
    auto fetcher = [&](const std::string& peer_id,
                       const std::string& hash,
                       uint64_t offset,
                       std::size_t length){
      return fetch_chunk(peer_id, hash, offset, length);
    };
    SwarmMeterCallback meter_callback;
    std::size_t meter_line_width = 0;
    if(enable_meter) {
      meter_callback = [&](const std::vector<uint8_t>& states,
                           uint64_t downloaded,
                           bool /*force*/){
        auto meter = format_transfer_meter(states, downloaded, file_size, chunk_size);
        std::size_t peers_total = peer_total ? peer_total->load() : remote_providers.size();
        if(peers_total == 0) peers_total = remote_providers.empty() ? 0 : remote_providers.size();
        std::size_t peers_active = active_peers ? active_peers->load() : 0;
        if(peers_active > peers_total) peers_total = peers_active;
        std::ostringstream line;
        line << "\rStaging " << primary.hash.substr(0, 8) << "... " << meter
             << " P[" << peers_active << "/" << peers_total << "]";
        auto rendered = line.str();
        std::cout << rendered;
        if(rendered.size() < meter_line_width) {
          std::cout << std::string(meter_line_width - rendered.size(), ' ');
        } else {
          meter_line_width = rendered.size();
        }
        std::cout.flush();
      };
    }
    std::string failure_reason;
    bool downloaded = run_swarm_download(primary.hash,
                                         file_size,
                                         swarm_config,
                                         remote_providers,
                                         chunk_states,
                                         fetcher,
                                         writer,
                                         meter_callback,
                                         failure_reason,
                                         refresh_provider,
                                         nullptr,
                                         active_peers);
    out.close();
    if(!downloaded) {
      if(!quiet && !failure_reason.empty()) {
        std::cout << failure_reason << "\n";
      }
      std::error_code ec;
      std::filesystem::remove(dest_path, ec);
      return false;
    }

    auto computed_hash = compute_file_hash(dest_path);
    if(!computed_hash) {
      if(!quiet) std::cout << "Failed to compute hash for staged download " << dest_path << "\n";
      std::error_code ec;
      std::filesystem::remove(dest_path, ec);
      return false;
    }
    if(*computed_hash != primary.hash) {
      if(!quiet) {
        std::cout << "File hash mismatch for staged download " << dest_path
                  << " (expected " << primary.hash
                  << " but got " << *computed_hash << ")\n";
      }
      std::error_code ec;
      std::filesystem::remove(dest_path, ec);
      return false;
    }
    if(!quiet) {
      std::cout << "\nHash verified for staged download (" << primary.hash << ")\n";
    }

    if(enable_meter) {
      if(meter_line_width > 0) {
        std::cout << "\r" << std::string(meter_line_width, ' ') << "\r";
      }
      std::cout << "\n";
    }
    return true;
  }

  std::string generate_conflict_id() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t value = dist(rng);
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
  }

  std::filesystem::path ignore_config_path() const {
    return config_root() / "ignore.json";
  }

  void load_ignore_config() {
    std::ifstream in(ignore_config_path());
    if(!in) return;
    try {
      nlohmann::json doc;
      in >> doc;
      IgnoreConfig cfg;
      if(doc.contains("paths")) {
        for(const auto& value : doc["paths"]) {
          cfg.paths.insert(value.get<std::string>());
        }
      }
      if(doc.contains("hashes")) {
        for(const auto& value : doc["hashes"]) {
          cfg.hashes.insert(value.get<std::string>());
        }
      }
      if(doc.contains("dir_guids")) {
        for(const auto& value : doc["dir_guids"]) {
          cfg.dir_guids.insert(value.get<std::string>());
        }
      }
      std::lock_guard<std::mutex> guard(ignore_mutex_);
      ignore_config_ = std::move(cfg);
    } catch(...) {
      // ignore malformed config
    }
  }

  void save_ignore_config() const {
    nlohmann::json doc;
    {
      std::lock_guard<std::mutex> guard(ignore_mutex_);
      doc["paths"] = ignore_config_.paths;
      doc["hashes"] = ignore_config_.hashes;
      doc["dir_guids"] = ignore_config_.dir_guids;
    }
    std::ofstream out(ignore_config_path());
    if(out) {
      out << doc.dump(2);
    }
  }

  std::string format_timestamp(const std::chrono::system_clock::time_point& tp) const {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buffer[32];
    if(std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm)) {
      return buffer;
    }
    return "<unknown>";
  }

  std::string timestamp_for_filename() const {
    std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buffer[32];
    if(std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm)) {
      return buffer;
    }
    return "timestamp";
  }

  void print_help() {
    std::cout << "Available commands:\n";
    std::cout << "  help|h|?                          Show this help message\n";
    std::cout << "  quit                              Exit the application\n";
    std::cout << "  peers                             List known peers\n";
    std::cout << "  send <message>                    Broadcast a chat message\n";
    std::cout << "  list|ls|l [peer:/path|hash|all]   List local or remote files\n";
    std::cout << "  share <path>                      Recursively share files (default share/)\n";
    std::cout << "  sync <resource>                   Sync file, directory, hash, or watch\n";
    std::cout << "  sync force <resource>             Delete local copy before syncing\n";
    std::cout << "  watch [list|add|remove|set|interval]  Manage directory watches\n";
    std::cout << "  settings [list|get|set|save|load] Manage runtime settings\n";
    std::cout << "  set [key value]                   Shortcut for settings set (lists when empty)\n";
    std::cout << "  get <key>                         Shortcut for settings get\n";
    std::cout << "  save                              Shortcut for settings save\n";
    std::cout << "  load                              Shortcut for settings load\n";
    std::cout << "  ignore [list|add|remove]          Manage persistent ignore rules\n";
    std::cout << "  guid list                        Show all tracked directory GUIDs\n";
    std::cout << "  guid orphans                     Show GUIDs whose directories are missing\n";
    std::cout << "  guid assign <guid> <path>        Map an orphaned GUID to a new directory\n";
    std::cout << "  guid forget <guid>               Remove a GUID from the registry\n";
    std::cout << "  conflict [list|accept|ignore|stage|view|unstage]  Review or resolve download conflicts\n";
    std::cout << "  bell                              Play notification bell\n";
    std::cout << "  dirs                              Show important directory locations\n";
    std::cout << "  pwd|cwd                           Show current working directory\n";
  }

  void ensure_directories() {
    std::error_code ec;
    std::filesystem::create_directories(share_root(), ec);
    std::filesystem::create_directories(download_root(), ec);
    std::filesystem::create_directories(archive_root(), ec);
    std::filesystem::create_directories(conflict_stage_root(), ec);
    std::filesystem::create_directories(config_root(), ec);
    migrate_legacy_config();
  }

  std::filesystem::path share_root() const {
    return std::filesystem::current_path() / "share";
  }

  std::filesystem::path download_root() const {
    return share_root() / ".sync_tmp";
  }

  std::filesystem::path config_root() const {
    return std::filesystem::current_path() / ".config";
  }

  std::filesystem::path legacy_config_root() const {
    return share_root() / ".config";
  }

  void migrate_legacy_config() {
    auto legacy = legacy_config_root();
    auto target = config_root();
    std::error_code ec;
    if(legacy == target) return;
    std::filesystem::create_directories(target, ec);

    auto migrate_file = [&](const std::filesystem::path& src, const std::filesystem::path& dst){
      if(src == dst) return;
      if(!std::filesystem::exists(src, ec)) return;
      if(std::filesystem::exists(dst, ec)) return;
      std::error_code rename_ec;
      std::filesystem::rename(src, dst, rename_ec);
      if(rename_ec) {
        std::error_code copy_ec;
        std::filesystem::copy_file(src, dst,
                                   std::filesystem::copy_options::overwrite_existing, copy_ec);
        if(!copy_ec) {
          std::error_code remove_ec;
          std::filesystem::remove(src, remove_ec);
        }
      }
    };

    if(std::filesystem::exists(legacy, ec)) {
      static const std::array<const char*, 3> files = {"dir-guids.json", "ignore.json", "watches.json"};
      for(const auto* name : files) {
        auto legacy_file = legacy / name;
        auto target_file = target / name;
        migrate_file(legacy_file, target_file);
      }
    }

    auto legacy_watches_root = std::filesystem::current_path() / "watches.json";
    migrate_file(legacy_watches_root, target / "watches.json");
  }

  std::filesystem::path archive_root() const {
    return share_root() / ".archive";
  }

  std::filesystem::path conflict_stage_root() const {
    return share_root() / ".conflict-stage";
  }

  std::filesystem::path resolve_share_path(const std::string& input) const {
    if(input.empty()) return share_root();
    std::filesystem::path p(input);
    if(p.is_absolute()) return p;
    return share_root() / p;
  }

  static void trim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
      [](unsigned char ch){ return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
      [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
  }

  static std::string normalize_cli_path(const std::string& cli_path) {
    std::string path = cli_path;
    trim(path);
    if(path.empty()) return "";
    if(path.front() == '/') path.erase(path.begin());
    while(!path.empty() && path.back() == '/') path.pop_back();
    if(path == ".") return "";
    if(path.rfind("./", 0) == 0) {
      path.erase(0, 2);
      if(path.empty()) return "";
    }
    return path;
  }

  std::optional<std::string> relative_to_share(const std::filesystem::path& absolute) const {
    std::error_code ec;
    auto rel = std::filesystem::relative(absolute, share_root(), ec);
    if(ec) return std::nullopt;
    auto rel_str = rel.generic_string();
    if(rel_str.empty()) return std::string{};
    if(rel_str.rfind("..", 0) == 0) return std::nullopt;
    return rel_str;
  }

  static std::string format_size(uint64_t bytes) {
    if(bytes < 1024) {
      return std::to_string(bytes) + "b";
    }

    static const char* suffixes[] = {"B", "K", "M", "G", "T", "P"};
    constexpr std::size_t suffix_count = sizeof(suffixes) / sizeof(suffixes[0]);
    double value = static_cast<double>(bytes);
    size_t idx = 0;
    while(idx + 1 < suffix_count && value >= 1024.0) {
      value /= 1024.0;
      ++idx;
    }

    auto format_with_precision = [&](int precision) {
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(precision) << value;
      std::string out = oss.str();
      if(out.find('.') != std::string::npos) {
        while(!out.empty() && out.back() == '0') out.pop_back();
        if(!out.empty() && out.back() == '.') out.pop_back();
      }
      return out + suffixes[idx];
    };

    std::string result = format_with_precision(value >= 100 ? 0 : (value >= 10 ? 1 : 2));
    while(result.size() > 6 && value >= 10 && idx < suffix_count - 1) {
      value /= 1024.0;
      ++idx;
      result = format_with_precision(value >= 100 ? 0 : (value >= 10 ? 1 : 2));
    }

    if(result.size() > 6) {
      // fall back to scientific-like notation within 6 chars
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(0) << value;
      result = oss.str() + suffixes[idx];
      if(result.size() > 6) {
        result = result.substr(0, 6);
      }
    }
    return result;
  }

  std::optional<std::string> compute_file_hash(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if(!in) return std::nullopt;

    SHA256_CTX ctx;
    if(SHA256_Init(&ctx) != 1) return std::nullopt;

    std::array<char, 8192> buffer{};
    while(in) {
      in.read(buffer.data(), buffer.size());
      std::streamsize read = in.gcount();
      if(read > 0) {
        if(SHA256_Update(&ctx, reinterpret_cast<const unsigned char*>(buffer.data()),
                         static_cast<size_t>(read)) != 1) {
          return std::nullopt;
        }
      }
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    if(SHA256_Final(digest, &ctx) != 1) return std::nullopt;

    std::vector<unsigned char> bytes(digest, digest + SHA256_DIGEST_LENGTH);
    return hex_from_bytes(bytes);
  }

  size_t share_single_file(const std::filesystem::path& absolute_path, bool announce) {
    auto rel_opt = relative_to_share(absolute_path);
    if(!rel_opt) {
      if(announce) {
        std::cout << "Skipping file outside share directory: " << absolute_path << "\n";
      }
      return 0;
    }
    if(is_internal_listing_path(*rel_opt)) {
      if(announce) {
        std::cout << "Skipping staging file: " << *rel_opt << "\n";
      }
      return 0;
    }

    auto hash_opt = compute_file_hash(absolute_path);
    if(!hash_opt) {
      if(announce) {
        std::cout << "Failed to hash file: " << absolute_path << "\n";
      }
      return 0;
    }

    std::error_code ec;
    auto size = std::filesystem::file_size(absolute_path, ec);
    if(ec) {
      if(announce) {
        std::cout << "Failed to determine size: " << absolute_path << "\n";
      }
      return 0;
    }

    pm_->register_local_share(*hash_opt, *rel_opt, absolute_path, size);
    if(announce) {
      std::cout << "Shared: " << *rel_opt << " (" << format_size(size) << ") -> " << *hash_opt << "\n";
    }
    return 1;
  }

  static bool looks_like_hash(const std::string& value) {
    if(value.empty()) return false;
    for(char c : value) {
      if(!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return value.size() >= 8;
  }

  static bool looks_like_dir_guid(const std::string& value) {
    return value.rfind("dir-", 0) == 0;
  }

  static bool is_internal_listing_path(const std::string& value) {
    if(value.empty()) return false;
    static const std::array<const char*, 4> reserved = {
      ".sync_tmp",
      ".archive",
      ".config",
      ".conflict-stage"
    };
    for(const auto* prefix : reserved) {
      std::string view(prefix);
      if(value == view) return true;
      if(value.rfind(view + "/", 0) == 0) return true;
    }
    return false;
  }

  std::optional<ListCommand> parse_list_command(const std::string& raw) {
    std::string input = raw;
    trim(input);
    if(input.empty()) {
      ListCommand cmd;
      cmd.type = ListCommand::Type::LocalPath;
      cmd.path = "";
      return cmd;
    }

    auto colon = input.find(':');
    if(colon == std::string::npos) {
      if(!input.empty() && input.front() == '/') {
        ListCommand cmd;
        cmd.type = ListCommand::Type::LocalPath;
        cmd.path = normalize_cli_path(input);
        return cmd;
      }
      if(looks_like_dir_guid(input)) {
        ListCommand cmd;
        cmd.type = ListCommand::Type::DirectoryGuid;
        cmd.dir_guid = input;
        return cmd;
      }
      if(is_known_peer(input)) {
        ListCommand cmd;
        cmd.type = ListCommand::Type::RemotePath;
        cmd.peer = input;
        cmd.path = "";
        return cmd;
      } else {
        ListCommand cmd;
        if(looks_like_hash(input)) {
          cmd.type = ListCommand::Type::HashAll;
          cmd.hash = input;
        } else {
          cmd.type = ListCommand::Type::LocalPath;
          cmd.path = normalize_cli_path(input);
        }
        return cmd;
      }
    }

    ListCommand cmd;
    cmd.peer = input.substr(0, colon);
    std::string remainder = input.substr(colon + 1);

    if(cmd.peer.empty()) return std::nullopt;

    if(remainder.empty()) {
      cmd.type = ListCommand::Type::RemotePath;
      cmd.path = "";
      return cmd;
    }

    if(remainder.front() == '/') {
      cmd.type = ListCommand::Type::RemotePath;
      cmd.path = normalize_cli_path(remainder);
      if(cmd.path.find('/') == std::string::npos && looks_like_dir_guid(cmd.path)) {
        cmd.type = ListCommand::Type::RemoteDirectoryGuid;
        cmd.dir_guid = cmd.path;
        cmd.path.clear();
      }
      return cmd;
    }

    if(looks_like_dir_guid(remainder)) {
      cmd.type = ListCommand::Type::RemoteDirectoryGuid;
      cmd.dir_guid = remainder;
      return cmd;
    }

    if(looks_like_hash(remainder)) {
      cmd.type = ListCommand::Type::RemoteHash;
      cmd.hash = remainder;
      return cmd;
    }

    cmd.type = ListCommand::Type::RemotePath;
    cmd.path = normalize_cli_path(remainder);
    return cmd;
  }

  PeerListingResult fetch_peer_listing_impl(const std::string& peer,
                                            const std::string& path,
                                            const std::string& hash,
                                            const std::string& dir_guid,
                                            bool silent) const {
    PeerListingResult result;
    auto promise = std::make_shared<std::promise<std::vector<PeerManager::RemoteListingItem>>>();
    auto future = promise->get_future();
    bool requested = pm_->request_peer_listing(peer, path, hash, dir_guid,
      [promise](const std::vector<PeerManager::RemoteListingItem>& items){
        promise->set_value(items);
      });
    if(!requested) {
      if(!silent) {
        std::cout << "Peer " << peer << " is not connected.\n";
      }
      result.state = PeerListingState::NotConnected;
      return result;
    }
    auto status = future.wait_for(std::chrono::seconds(5));
    if(status != std::future_status::ready) {
      if(!silent) {
        std::cout << "Timed out waiting for response from " << peer << ".\n";
      }
      result.state = PeerListingState::Timeout;
      return result;
    }
    result.state = PeerListingState::Completed;
    result.items = future.get();
    return result;
  }

  PeerListingResult fetch_peer_listing(const std::string& peer,
                                       const std::string& path,
                                       const std::string& hash,
                                       const std::string& dir_guid,
                                       bool silent) const {
    return fetch_peer_listing_impl(peer, path, hash, dir_guid, silent);
  }

  std::vector<PeerManager::RemoteListingItem> blocking_list_peer(const std::string& peer,
                                                                 const std::string& path,
                                                                 const std::string& hash,
                                                                 const std::string& dir_guid) {
    auto resp = fetch_peer_listing_impl(peer, path, hash, dir_guid, false);
    if(resp.state != PeerListingState::Completed) return {};
    return resp.items;
  }

  std::vector<PeerManager::RemoteListingItem> blocking_list_hash(const std::string& hash) {
    auto promise = std::make_shared<std::promise<std::vector<PeerManager::RemoteListingItem>>>();
    auto future = promise->get_future();
    bool requested = pm_->request_hash_listing_all(hash,
      [promise](const std::vector<PeerManager::RemoteListingItem>& items){
        promise->set_value(items);
      });
    if(!requested) {
      promise->set_value({});
    }
    auto status = future.wait_for(std::chrono::seconds(5));
    if(status != std::future_status::ready) {
      std::cout << "Timed out waiting for responses.\n";
      return {};
    }
    return future.get();
  }

  std::vector<PeerManager::RemoteListingItem> make_remote_listing_items(
      const std::vector<PeerManager::SharedFileEntry>& entries) {
    std::vector<PeerManager::RemoteListingItem> items;
    items.reserve(entries.size());
    for(const auto& entry : entries) {
      PeerManager::RemoteListingItem item;
      item.peer_id = pm_->local_peer_id();
      item.hash = entry.hash;
      item.relative_path = entry.relative_path;
      item.size = entry.size;
      item.is_directory = false;
      items.push_back(std::move(item));
    }
    return items;
  }

  void print_listing(const std::vector<PeerManager::RemoteListingItem>& items,
                     bool show_peer_prefix = true) {
    if(items.empty()) {
      std::cout << "(empty)\n";
      return;
    }
    auto sorted = items;
    std::stable_sort(sorted.begin(), sorted.end(),
      [&](const PeerManager::RemoteListingItem& lhs, const PeerManager::RemoteListingItem& rhs){
        bool lhs_local = lhs.peer_id == pm_->local_peer_id();
        bool rhs_local = rhs.peer_id == pm_->local_peer_id();
        if(lhs_local != rhs_local) return lhs_local;
        if(lhs.is_directory != rhs.is_directory) return lhs.is_directory;
        if(lhs.peer_id != rhs.peer_id) return lhs.peer_id < rhs.peer_id;
        if(lhs.relative_path != rhs.relative_path) return lhs.relative_path < rhs.relative_path;
        return lhs.hash < rhs.hash;
      });

    std::vector<PeerManager::RemoteListingItem> filtered;
    filtered.reserve(sorted.size());
    for(const auto& item : sorted) {
      if(is_internal_listing_path(item.relative_path)) continue;
      if(item.is_directory && item.relative_path.empty()) continue;
      filtered.push_back(item);
    }

    if(filtered.empty()) {
      std::cout << "(empty)\n";
      return;
    }

    std::size_t id_width = 0;
    for(const auto& item : filtered) {
      const std::string& ref = item.is_directory ? item.directory_guid : item.hash;
      if(ref.size() > id_width) id_width = ref.size();
    }
    if(id_width < 64) id_width = 64;

    auto old_flags = std::cout.flags();
    auto old_fill = std::cout.fill();

    std::size_t size_width = 6;

    for(const auto& item : filtered) {
      std::string display_path = item.relative_path.empty() ? "/" : "/" + item.relative_path;
      if(item.is_directory && !item.relative_path.empty() && display_path.back() != '/') {
        display_path += "/";
      }

      std::string id_str = item.is_directory ? item.directory_guid : item.hash;
      if(id_str.empty()) id_str = item.is_directory ? "<dir>" : "<hash>";
      std::string size_str = item.is_directory ? "<DIR>" : format_size(item.size);
      if(size_str.size() > size_width) size_str = size_str.substr(0, size_width);

      std::optional<WatchAnnotationDisplay> watch_annotation;
      bool is_local_entry = item.peer_id == pm_->local_peer_id();
      if(is_local_entry && item.is_directory && !item.directory_guid.empty()) {
        watch_annotation = watch_annotation_for_guid(item.directory_guid);
      }

      std::cout << std::left << std::setw(static_cast<int>(id_width)) << id_str << std::right << "  "
                << std::setw(static_cast<int>(size_width)) << size_str << "  ";
      if(show_peer_prefix) {
        std::cout << item.peer_id << ":";
      }
      std::cout << display_path;
      if(watch_annotation && watch_annotation->preferred) {
        std::cout << "  watch:(" << *watch_annotation->preferred << ")";
        if(watch_annotation->fallback) {
          std::cout << " fallback:(" << *watch_annotation->fallback << ")";
        } else if(watch_annotation->fallback_unavailable) {
          std::cout << " fallback:(unavailable)";
        }
      }
      std::cout << "\n";
    }

    std::cout.flags(old_flags);
    std::cout.fill(old_fill);
  }

  void print_dirs() {
    std::cout << "Current directory : " << std::filesystem::current_path() << "\n";
    std::cout << "Share directory   : " << share_root() << "\n";
    std::cout << "Staging directory : " << download_root() << "\n";
  }

  void print_current_directory() {
    std::cout << std::filesystem::current_path() << "\n";
  }

  void load_dir_guid_registry() {
    std::lock_guard<std::mutex> lock(dir_guid_mutex_);
    dir_guid_records_.clear();
    dir_guid_by_path_.clear();
    dir_guid_registry_dirty_ = false;

    auto path = dir_guid_registry_path();
    if(!std::filesystem::exists(path)) return;

    try {
      bool pruned_duplicates = false;
      std::unordered_set<std::string> loaded_paths;
      std::ifstream in(path);
      if(!in) return;
      nlohmann::json doc;
      in >> doc;
      auto entries = doc.value("entries", nlohmann::json::array());
      for(const auto& entry : entries) {
        DirGuidRecord record;
        record.guid = entry.value("guid", "");
        record.path = entry.value("path", "");
        record.active = entry.value("active", !record.path.empty());
        if(record.guid.empty()) continue;
        if(!record.path.empty()) {
          auto normalized = normalize_cli_path(record.path);
          if(normalized.empty()) {
            record.path.clear();
            record.active = false;
          } else {
            record.path = normalized;
            if(!loaded_paths.insert(record.path).second) {
              pruned_duplicates = true;
              continue;
            }
          }
        }
        if(dir_guid_records_.count(record.guid)) continue;
        dir_guid_records_[record.guid] = record;
        if(record.active && !record.path.empty()) {
          dir_guid_by_path_[record.path] = record.guid;
        }
      }
      if(pruned_duplicates) {
        dir_guid_registry_dirty_ = true;
      }
    } catch(const std::exception& ex) {
      std::cout << "Failed to load dir-guids.json: " << ex.what() << "\n";
    }
  }

  void apply_dir_guid_registry() {
    std::lock_guard<std::mutex> lock(dir_guid_mutex_);
    for(const auto& [guid, record] : dir_guid_records_) {
      if(record.active && !record.path.empty()) {
        pm_->register_directory_with_guid(record.path, guid);
      }
    }
  }

  void save_dir_guid_registry_locked() {
    if(!dir_guid_registry_dirty_) return;
    nlohmann::json doc;
    doc["entries"] = nlohmann::json::array();
    for(const auto& [guid, record] : dir_guid_records_) {
      nlohmann::json item;
      item["guid"] = guid;
      item["path"] = record.path;
      item["active"] = record.active;
      doc["entries"].push_back(std::move(item));
    }
    std::error_code ec;
    std::filesystem::create_directories(config_root(), ec);
    try {
      std::ofstream out(dir_guid_registry_path());
      if(out) {
        out << doc.dump(2);
        dir_guid_registry_dirty_ = false;
      }
    } catch(const std::exception& ex) {
      std::cout << "Failed to save dir-guids.json: " << ex.what() << "\n";
    }
  }

  void save_dir_guid_registry() {
    std::lock_guard<std::mutex> lock(dir_guid_mutex_);
    save_dir_guid_registry_locked();
  }

  bool apply_cached_dir_guid(const std::string& relative_path) {
    if(relative_path.empty()) return true;
    std::string guid;
    {
      std::lock_guard<std::mutex> lock(dir_guid_mutex_);
      auto it = dir_guid_by_path_.find(relative_path);
      if(it == dir_guid_by_path_.end()) {
        return false;
      }
      guid = it->second;
    }
    pm_->register_directory_with_guid(relative_path, guid);
    return true;
  }

  void remember_dir_guid_mapping(const std::string& raw_relative_path,
                                 const std::string& guid,
                                 bool ensure_binding = true) {
    if(raw_relative_path.empty() || guid.empty()) return;
    auto normalized = normalize_cli_path(raw_relative_path);
    if(normalized.empty()) return;
    if(ensure_binding) {
      pm_->register_directory_with_guid(normalized, guid);
    }

    std::lock_guard<std::mutex> lock(dir_guid_mutex_);
    auto current_path_it = dir_guid_by_path_.find(normalized);
    if(current_path_it != dir_guid_by_path_.end() && current_path_it->second == guid) {
      auto& record = dir_guid_records_[guid];
      record.guid = guid;
      record.path = normalized;
      record.active = true;
      return;
    }

    if(current_path_it != dir_guid_by_path_.end() && current_path_it->second != guid) {
      auto displaced_it = dir_guid_records_.find(current_path_it->second);
      if(displaced_it != dir_guid_records_.end()) {
        dir_guid_records_.erase(displaced_it);
      }
      dir_guid_by_path_.erase(current_path_it);
    }

    DirGuidRecord& record = dir_guid_records_[guid];
    if(!record.path.empty() && record.path != normalized) {
      dir_guid_by_path_.erase(record.path);
    }
    record.guid = guid;
    record.path = normalized;
    record.active = true;
    dir_guid_by_path_[normalized] = guid;
    dir_guid_registry_dirty_ = true;
  }

  void register_directory_path(const std::string& relative_path) {
    if(relative_path.empty()) return;
    if(auto watch_guid = watch_guid_for_path(relative_path)) {
      remember_dir_guid_mapping(relative_path, *watch_guid);
      return;
    }
    if(apply_cached_dir_guid(relative_path)) return;

    pm_->register_directory(relative_path);
    auto guid_opt = pm_->directory_guid_for_path(relative_path);
    if(!guid_opt) return;

    remember_dir_guid_mapping(relative_path, *guid_opt, false);
  }

  std::vector<DirGuidRecord> orphaned_dir_guids() const {
    std::lock_guard<std::mutex> lock(dir_guid_mutex_);
    std::vector<DirGuidRecord> out;
    out.reserve(dir_guid_records_.size());
    for(const auto& [guid, record] : dir_guid_records_) {
      if(!record.active) out.push_back(record);
    }
    return out;
  }

  bool has_orphaned_dir_guids() const {
    std::lock_guard<std::mutex> lock(dir_guid_mutex_);
    for(const auto& [guid, record] : dir_guid_records_) {
      if(!record.active) return true;
    }
    return false;
  }

  void print_orphan_hint(const std::vector<DirGuidRecord>& newly_orphaned) {
    if(newly_orphaned.empty()) return;
    std::vector<const DirGuidRecord*> filtered;
    for(const auto& record : newly_orphaned) {
      if(!record.path.empty() && directory_exists_relative(record.path)) continue;
      filtered.push_back(&record);
    }
    if(filtered.empty()) return;
    std::cout << "[guid] The following directory GUIDs no longer match on-disk folders:\n";
    for(const auto* record_ptr : filtered) {
      const auto& record = *record_ptr;
      std::cout << "  " << record.guid;
      if(!record.path.empty()) {
        std::cout << " (previously '" << record.path << "')";
      }
      std::cout << "\n";
    }
    std::cout << "Use 'guid assign <guid> <relative-path>' to map a GUID to a new folder, "
                 "or 'guid orphans' to review the full list.\n";
  }

  std::vector<DirGuidRecord> reconcile_dir_guid_registry(
      const std::unordered_set<std::string>& seen_paths,
      const std::unordered_set<std::string>& watch_guids) {
    std::vector<DirGuidRecord> newly_orphaned;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(dir_guid_mutex_);
      for(auto& [guid, record] : dir_guid_records_) {
        bool seen = !record.path.empty() && seen_paths.count(record.path) > 0;
        if(!seen && watch_guids.count(record.guid) > 0) {
          seen = true;
        }
        if(!seen && directory_exists_relative(record.path)) {
          seen = true;
        }
        if(seen) {
          if(!record.active) {
            record.active = true;
            changed = true;
          }
          if(!record.path.empty()) {
            dir_guid_by_path_[record.path] = guid;
          }
        } else {
          if(record.active) {
            record.active = false;
            changed = true;
          }
          if(!record.path.empty()) {
            dir_guid_by_path_.erase(record.path);
          }
          newly_orphaned.push_back(record);
        }
      }
      if(changed) {
        dir_guid_registry_dirty_ = true;
      }
    }
    if(changed) {
      save_dir_guid_registry();
    }
    return newly_orphaned;
  }

  bool is_known_peer(const std::string& peer) const {
    auto arr = pm_->make_peer_list_json();
    for(auto& p : arr) {
      if(p["peer_id"].get<std::string>() == peer) return true;
    }
    return false;
  }

  std::vector<DirGuidRecord> index_share_root() {
    auto watch_guids = active_watch_guids();
    {
      std::lock_guard<std::mutex> guard(share_index_mutex_);
      auto root = share_root();
      std::error_code ec;
      if(!std::filesystem::exists(root, ec)) return {};
      if(std::filesystem::is_regular_file(root, ec)) {
        PeerManager::SharedFileEntry existing;
        auto rel_opt = relative_to_share(root);
        if(rel_opt && pm_->find_local_file_by_path(*rel_opt, existing)) {
          std::error_code size_ec;
          auto size = std::filesystem::file_size(root, size_ec);
          if(!size_ec && existing.size == size) {
            return {};
          }
        }
        share_single_file(root, false);
        return {};
      }
      std::unordered_set<std::string> seen_dirs;

      for(auto it = std::filesystem::recursive_directory_iterator(root, ec);
          !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if(it->is_directory()) {
          auto rel = it->path().lexically_relative(root).generic_string();
          if(is_internal_listing_path(rel)) {
            it.disable_recursion_pending();
            continue;
          }
          auto normalized = normalize_relative_string(rel);
          register_directory_path(normalized);
          if(!normalized.empty()) {
            seen_dirs.insert(normalized);
          }
        } else if(it->is_regular_file()) {
          auto rel = it->path().lexically_relative(root).generic_string();
          if(is_internal_listing_path(rel)) continue;
          PeerManager::SharedFileEntry existing;
          if(pm_->find_local_file_by_path(rel, existing)) {
            std::error_code size_ec;
            auto size = std::filesystem::file_size(it->path(), size_ec);
            if(!size_ec && existing.size == size) {
              continue;
            }
          }
          share_single_file(it->path(), false);
        }
      }

      return reconcile_dir_guid_registry(seen_dirs, watch_guids);
    }
  }
};
