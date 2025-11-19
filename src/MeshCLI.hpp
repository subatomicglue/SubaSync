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
#include "CommandLineOptions.hpp"

class MeshCLI {
public:
  explicit MeshCLI(std::shared_ptr<PeerManager> pm,
                   std::shared_ptr<CommandLineOptions> settings,
                   std::chrono::seconds default_watch_interval = std::chrono::seconds(60),
                   bool audio_notifications = false)
    : pm_(std::move(pm)), settings_(std::move(settings)), running_(true),
      default_watch_interval_(default_watch_interval), audio_notifications_(audio_notifications) {
    ensure_directories();
    index_share_root();
    load_ignore_config();
    apply_setting_side_effects("audio_notifications");
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
  std::shared_ptr<CommandLineOptions> settings_;
  std::atomic<bool> running_;
  std::thread cli_thread_;
  std::chrono::seconds default_watch_interval_;
  bool audio_notifications_ = false;
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

  struct SyncOutcome {
    bool success = true;
    bool changed = false;
  };

  struct ResolvedDirGuid {
    std::string guid;
    std::string origin_peer;
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
      } else if(cmd == "sync" || cmd == "get") {
        std::string target;
        std::getline(iss, target);
        trim(target);
        sync_command(target);
      } else if(cmd == "watch") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_watch_command(args);
      } else if(cmd == "bell") {
        std::cout << '\a';
        std::cout.flush();
      } else if(cmd == "settings") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_settings_command(args);
      } else if(cmd == "ignore") {
        std::string args;
        std::getline(iss, args);
        trim(args);
        handle_ignore_command(args);
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
    index_share_root();
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
    print_listing(items, false);
  }

  void list_local_dir_guid(const std::string& guid) {
    index_share_root();
    auto items = pm_->local_listing_for_dir_guid(guid);
    if(items.empty()) {
      std::cout << "Directory not found for GUID " << guid << "\n";
      return;
    }
    print_listing(items, false);
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
    if(target.empty()) {
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
      std::istringstream iss(target);
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

    auto parsed = parse_list_command(source_arg);
    if(!parsed) {
      std::cout << "Invalid sync target.\n";
      return;
    }

    if((parsed->type == ListCommand::Type::RemotePath ||
        parsed->type == ListCommand::Type::RemoteHash ||
        parsed->type == ListCommand::Type::RemoteDirectoryGuid) &&
        parsed->peer == pm_->local_peer_id()) {
      std::cout << "Cannot sync from self.\n";
      std::cout << "Nothing to sync.\n";
      return;
    }

    std::vector<PeerManager::RemoteListingItem> entries;
    switch(parsed->type) {
      case ListCommand::Type::RemotePath:
        if(is_internal_listing_path(parsed->path)) {
          std::cout << "Path is reserved on " << parsed->peer << ".\n";
          return;
        }
        entries = blocking_list_peer(parsed->peer, parsed->path, "", "");
        break;
      case ListCommand::Type::RemoteHash:
        entries = blocking_list_peer(parsed->peer, "", parsed->hash, "");
        break;
      case ListCommand::Type::HashAll:
        entries = blocking_list_hash(parsed->hash);
        break;
      case ListCommand::Type::DirectoryGuid:
        entries = pm_->local_listing_for_dir_guid(parsed->dir_guid);
        break;
      case ListCommand::Type::RemoteDirectoryGuid:
        entries = blocking_list_peer(parsed->peer, "", "", parsed->dir_guid);
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

    auto result = perform_sync(*parsed, std::move(entries), dest_override, dest_is_directory, false, nullptr);
    if(!result.success) {
      std::cout << "Sync failed.\n";
    } else if(!result.changed) {
      std::cout << "Already up to date.\n";
    }
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
          pm_->register_directory_with_guid(dir_string, item.directory_guid);
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
            pm_->register_directory_with_guid(sub_string, item.directory_guid);
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

    auto relative_parent = sanitize_relative(final_path.parent_path().lexically_relative(share_root()));
    auto relative_parent_str = relative_parent.generic_string();
    if(!relative_parent_str.empty()) {
      if(!primary.directory_guid.empty()) {
        pm_->register_directory_with_guid(relative_parent_str, primary.directory_guid);
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

    auto attempt_download = [&](const std::vector<PeerManager::RemoteListingItem>& provider_set,
                                bool quiet_attempt) -> DownloadAttemptResult {
      if(provider_set.empty()) return DownloadAttemptResult::Failed;

      std::ofstream out(staging_path, std::ios::binary | std::ios::trunc);
      if(!out) {
        if(!quiet_attempt) std::cout << "Cannot open staging file: " << staging_path << "\n";
        return DownloadAttemptResult::Failed;
      }

      struct ProviderState {
        PeerManager::RemoteListingItem item;
        std::size_t failures = 0;
      };

      std::vector<ProviderState> provider_states;
      provider_states.reserve(provider_set.size());
      for(const auto& provider : provider_set) {
        provider_states.push_back(ProviderState{provider, 0});
      }

      const std::size_t max_failures_per_peer = 3;
      const std::size_t chunk_size = PeerManager::kMaxChunkSize;
      uint64_t offset = 0;
      std::size_t cursor = 0;

      while(offset < file_size) {
        std::size_t request_len = static_cast<std::size_t>(std::min<uint64_t>(chunk_size, file_size - offset));
        auto active_count = std::count_if(provider_states.begin(), provider_states.end(),
          [max_failures_per_peer](const ProviderState& state){
            return state.failures < max_failures_per_peer;
          });
        if(active_count == 0) {
          if(!quiet_attempt) std::cout << "\nNo responsive peers remaining for hash " << primary.hash << "\n";
          out.close();
          std::error_code ec;
          std::filesystem::remove(staging_path, ec);
          return DownloadAttemptResult::Failed;
        }

        bool chunk_ok = false;
        std::size_t attempts = 0;
        while(attempts < active_count && !chunk_ok) {
          ProviderState& state = provider_states[cursor];
          cursor = (cursor + 1) % provider_states.size();
          if(state.failures >= max_failures_per_peer) continue;
          ++attempts;

          auto resp = fetch_chunk(state.item.peer_id, primary.hash, offset, request_len);
          if(!resp.success || resp.data.empty()) {
            ++state.failures;
            if(state.failures == max_failures_per_peer && !quiet_attempt) {
              std::cout << "\nDropping provider " << state.item.peer_id << " after repeated failures.\n";
            }
            continue;
          }

          std::string local_sha = sha256_hex(std::string(resp.data.data(), resp.data.size()));
          if(!resp.chunk_sha.empty() && resp.chunk_sha != local_sha) {
            ++state.failures;
            if(!quiet_attempt) {
              std::cout << "\nHash mismatch; retrying chunk offset "
                        << offset << " from " << state.item.peer_id << "\n";
            }
            if(state.failures == max_failures_per_peer && !quiet_attempt) {
              std::cout << "Dropping provider " << state.item.peer_id << " after repeated checksum errors.\n";
            }
            continue;
          }

          out.write(resp.data.data(), static_cast<std::streamsize>(resp.data.size()));
          if(!out) {
            if(!quiet_attempt) std::cout << "\nFailed writing chunk for " << primary.hash << "\n";
            out.close();
            std::error_code ec;
            std::filesystem::remove(staging_path, ec);
            return DownloadAttemptResult::Failed;
          }

          offset += resp.data.size();
          state.failures = 0;
          if(!quiet_attempt) {
            double progress = static_cast<double>(offset) / static_cast<double>(file_size) * 100.0;
            std::cout << "\rSyncing " << primary.hash.substr(0, 8) << "... "
                      << std::fixed << std::setprecision(1) << progress << "%";
            std::cout.flush();
          }
          chunk_ok = true;
        }

        if(!chunk_ok) {
          if(!quiet_attempt) std::cout << "\nFailed to download chunk for hash " << primary.hash << "\n";
          out.close();
          std::error_code ec;
          std::filesystem::remove(staging_path, ec);
          return DownloadAttemptResult::Failed;
        }
      }

      if(!quiet_attempt) std::cout << "\n";
      out.close();

      auto computed_hash = compute_file_hash(staging_path);
      if(!computed_hash || *computed_hash != primary.hash) {
        if(!quiet_attempt) std::cout << "File hash mismatch for " << clean_relative << "\n";
        std::error_code ec;
        std::filesystem::remove(staging_path, ec);
        return DownloadAttemptResult::HashMismatch;
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
      if(!quiet_attempt) {
        std::cout << "Synced " << clean_relative << " (" << format_size(file_size) << ")\n";
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

  std::filesystem::path watch_file_path() const {
    return std::filesystem::current_path() / "watches.json";
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
      }
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
    std::ofstream out(watch_file_path());
    if(out) {
      out << doc.dump(2);
    }
  }

  void list_watches() {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    if(watches_.empty()) {
      std::cout << "No watches configured.\n";
      return;
    }
    for(const auto& entry : watches_) {
      std::cout << entry.id << " -> ";
      bool first = true;
      for(const auto& src : entry.sources) {
        if(!first) std::cout << ", ";
        first = false;
        std::cout << src.peer << ":/" << (entry.dir_guid.empty() ? src.path : entry.dir_guid);
      }
      if(first) std::cout << "(no sources)";
      std::cout << "  dest=" << (entry.dest_path.empty() ? "(default)" : entry.dest_path)
                << "  every " << entry.interval.count() << "s";
      if(!entry.origin_peer.empty()) {
        std::cout << "  origin=" << entry.origin_peer;
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
                                                         const std::string& path) {
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
    for(const auto& item : listings) {
      if(!item.is_directory) continue;
      if(item.directory_guid.empty()) continue;
      auto candidate = normalize_cli_path(item.relative_path);
      if(candidate == normalized) {
        ResolvedDirGuid resolved;
        resolved.guid = item.directory_guid;
        resolved.origin_peer = item.origin_peer.empty() ? peer : item.origin_peer;
        return resolved;
      }
    }
    return std::nullopt;
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

    bool action_is_known = action == "list" || action == "add" || action == "remove" || action == "interval";
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
        std::cout << "Usage: watch add <peer:/path|peer:dir-guid> [dest]\n";
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

      auto parsed = parse_list_command(source);
      if(!parsed || (parsed->type != ListCommand::Type::RemotePath && parsed->type != ListCommand::Type::RemoteDirectoryGuid)) {
        std::cout << "Watch currently supports peer paths or directory GUIDs.\n";
        return;
      }
      if(parsed->peer == pm_->local_peer_id()) {
        std::cout << "Cannot watch self.\n";
        return;
      }
      if(parsed->type == ListCommand::Type::RemotePath && is_internal_listing_path(parsed->path)) {
        std::cout << "Cannot watch reserved staging path.\n";
        return;
      }
      if(dest_override && is_internal_listing_path(*dest_override)) {
        std::cout << "Cannot sync into reserved staging path.\n";
        return;
      }

      WatchEntry::Source new_source;
      new_source.peer = parsed->peer;
      if(parsed->type == ListCommand::Type::RemotePath) {
        new_source.path = normalize_cli_path(parsed->path);
      } else {
        new_source.path.clear();
      }

      std::optional<ResolvedDirGuid> resolved_guid;
      if(parsed->type == ListCommand::Type::RemoteDirectoryGuid) {
        resolved_guid = ResolvedDirGuid{parsed->dir_guid, new_source.peer};
      } else {
        resolved_guid = resolve_remote_dir_guid(parsed->peer, new_source.path);
        if(!resolved_guid) {
          std::cout << "Unable to determine directory GUID for " << parsed->peer << ":/" << parsed->path << ".\n";
          std::cout << "Ensure the directory exists and has been indexed on the remote peer.\n";
          return;
        }
      }

      if(resolved_guid->guid.empty()) {
        std::cout << "Directory GUID is required to track a watch.\n";
        return;
      }

      WatchEntry snapshot;
      bool created = false;
      bool updated = false;
      std::string affected_id;

      {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        WatchEntry* existing = find_watch_by_guid(resolved_guid->guid);
        if(!existing) {
          WatchEntry fresh;
          fresh.id = generate_watch_id();
          fresh.dir_guid = resolved_guid->guid;
          fresh.origin_peer = !resolved_guid->origin_peer.empty() ? resolved_guid->origin_peer : new_source.peer;
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
          affected_id = existing->id;
          if(dest_override) {
            if(existing->dest_path.empty()) {
              existing->dest_path = *dest_override;
              existing->dest_is_directory = dest_is_dir || existing->dest_path.empty();
            } else if(existing->dest_path != *dest_override) {
              std::cout << "Watch " << existing->id << " already uses destination "
                        << existing->dest_path << ". Remove it first to change destinations.\n";
              return;
            }
          }
          auto dup = std::find_if(existing->sources.begin(), existing->sources.end(),
            [&](const WatchEntry::Source& src){
              return src.peer == new_source.peer &&
                     normalize_cli_path(src.path) == normalize_cli_path(new_source.path);
            });
          if(dup != existing->sources.end()) {
            std::cout << "Watch " << existing->id << " already trusts " << new_source.peer;
            if(!new_source.path.empty()) std::cout << ":/" << new_source.path;
            std::cout << ".\n";
            return;
          }
          existing->sources.push_back(new_source);
          existing->next_run = std::chrono::steady_clock::now();
          if(resolved_guid && existing->dir_guid.empty()) {
            existing->dir_guid = resolved_guid->guid;
          }
          if(resolved_guid && !resolved_guid->origin_peer.empty()) {
            existing->origin_peer = resolved_guid->origin_peer;
          } else if(existing->origin_peer.empty()) {
            existing->origin_peer = existing->sources.front().peer;
          }
          if(!existing->dir_guid.empty() && !existing->origin_peer.empty()) {
            pm_->set_directory_origin(existing->dir_guid, existing->origin_peer);
          }
          snapshot = *existing;
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
          pm_->register_directory_with_guid(local_root, snapshot.dir_guid);
        }
        if(!snapshot.origin_peer.empty()) {
          pm_->set_directory_origin(snapshot.dir_guid, snapshot.origin_peer);
        }
      }

      save_watches();
      watch_cv_.notify_all();
      if(created) {
        std::cout << "Added watch " << affected_id << " for " << resolved_guid->guid << ".\n";
      } else {
        std::cout << "Updated watch " << affected_id << " with a new trusted source.\n";
      }
      auto result = run_single_watch(snapshot);
      if(!result.success) {
        std::cout << "[watch " << affected_id << "] sync failed.\n";
      } else if(result.changed) {
        std::cout << "[watch " << affected_id << "] synced.\n";
      }
    } else if(action == "remove") {
      std::string id;
      iss >> id;
      trim(id);
      if(id.empty()) {
        std::cout << "Usage: watch remove <id|dir-guid|peer:/path>\n";
        return;
      }

      bool changed = false;
      bool removed_watch = false;
      std::vector<std::string> removed_ids;

      if(id.find(':') != std::string::npos) {
        auto parsed = parse_list_command(id);
        if(!parsed || (parsed->type != ListCommand::Type::RemotePath && parsed->type != ListCommand::Type::RemoteDirectoryGuid)) {
          std::cout << "Specify a watch source as peer:/path or peer:dir-guid.\n";
          return;
        }

        std::string normalized_path = parsed->type == ListCommand::Type::RemotePath
          ? normalize_cli_path(parsed->path)
          : "";

        {
          std::lock_guard<std::mutex> lock(watch_mutex_);
          for(auto it = watches_.begin(); it != watches_.end();) {
            bool modified = false;
            if(parsed->type == ListCommand::Type::RemoteDirectoryGuid) {
              if(it->dir_guid != parsed->dir_guid) {
                ++it;
                continue;
              }
              auto before = it->sources.size();
              it->sources.erase(std::remove_if(it->sources.begin(), it->sources.end(),
                [&](const WatchEntry::Source& src){
                  return src.peer == parsed->peer;
                }), it->sources.end());
              modified = before != it->sources.size();
              if(modified && !it->sources.empty()) {
                auto origin_found = std::find_if(it->sources.begin(), it->sources.end(),
                  [&](const WatchEntry::Source& src){
                    return src.peer == it->origin_peer;
                  });
                if(origin_found == it->sources.end()) {
                  it->origin_peer = it->sources.front().peer;
                }
                if(!it->dir_guid.empty() && !it->origin_peer.empty()) {
                  pm_->set_directory_origin(it->dir_guid, it->origin_peer);
                }
              }
            } else {
              auto before = it->sources.size();
              it->sources.erase(std::remove_if(it->sources.begin(), it->sources.end(),
                [&](const WatchEntry::Source& src){
                  return src.peer == parsed->peer &&
                         normalize_cli_path(src.path) == normalized_path;
                }), it->sources.end());
              modified = before != it->sources.size();
              if(modified && !it->sources.empty()) {
                auto origin_found = std::find_if(it->sources.begin(), it->sources.end(),
                  [&](const WatchEntry::Source& src){
                    return src.peer == it->origin_peer;
                  });
                if(origin_found == it->sources.end()) {
                  it->origin_peer = it->sources.front().peer;
                }
                if(!it->dir_guid.empty() && !it->origin_peer.empty()) {
                  pm_->set_directory_origin(it->dir_guid, it->origin_peer);
                }
              }
            }

            if(modified) {
              changed = true;
              if(it->sources.empty()) {
                removed_ids.push_back(it->id);
                it = watches_.erase(it);
                removed_watch = true;
                continue;
              }
            }
            ++it;
          }
        }

        if(!changed && !removed_watch) {
          std::cout << "No matching watch source found.\n";
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
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if(!out) {
      if(!quiet) std::cout << "Cannot open staging file: " << dest_path << "\n";
      return false;
    }

    const std::size_t chunk_size = PeerManager::kMaxChunkSize;
    struct ProviderState {
      PeerManager::RemoteListingItem item;
      std::size_t failures = 0;
    };
    const std::size_t max_failures_per_peer = 3;
    std::vector<ProviderState> provider_states;
    provider_states.reserve(remote_providers.size());
    for(const auto& provider : remote_providers) {
      provider_states.push_back(ProviderState{provider, 0});
    }

    uint64_t offset = 0;
    std::size_t cursor = 0;
    while(offset < file_size) {
      std::size_t request_len = static_cast<std::size_t>(std::min<uint64_t>(chunk_size, file_size - offset));
      auto active_count = std::count_if(provider_states.begin(), provider_states.end(),
        [max_failures_per_peer](const ProviderState& state){
          return state.failures < max_failures_per_peer;
        });
      if(active_count == 0) {
        if(!quiet) std::cout << "No responsive peers remaining for hash " << primary.hash << "\n";
        out.close();
        std::error_code ec;
        std::filesystem::remove(dest_path, ec);
        return false;
      }

      bool chunk_ok = false;
      std::size_t attempts = 0;
      while(attempts < active_count && !chunk_ok) {
        ProviderState& state = provider_states[cursor];
        cursor = (cursor + 1) % provider_states.size();
        if(state.failures >= max_failures_per_peer) continue;
        ++attempts;

        auto resp = fetch_chunk(state.item.peer_id, primary.hash, offset, request_len);
        if(!resp.success || resp.data.empty()) {
          ++state.failures;
          continue;
        }

        std::string local_sha = sha256_hex(std::string(resp.data.data(), resp.data.size()));
        if(!resp.chunk_sha.empty() && resp.chunk_sha != local_sha) {
          ++state.failures;
          continue;
        }

        out.write(resp.data.data(), static_cast<std::streamsize>(resp.data.size()));
        if(!out) {
          if(!quiet) std::cout << "Failed writing chunk for " << primary.hash << "\n";
          out.close();
          std::error_code ec;
          std::filesystem::remove(dest_path, ec);
          return false;
        }

        offset += resp.data.size();
        state.failures = 0;
        chunk_ok = true;
      }

      if(!chunk_ok) {
        if(!quiet) std::cout << "Failed to download chunk for hash " << primary.hash << "\n";
        out.close();
        std::error_code ec;
        std::filesystem::remove(dest_path, ec);
        return false;
      }
    }

    out.close();
    auto computed_hash = compute_file_hash(dest_path);
    if(!computed_hash || *computed_hash != primary.hash) {
      if(!quiet) std::cout << "File hash mismatch for staged download " << dest_path << "\n";
      std::error_code ec;
      std::filesystem::remove(dest_path, ec);
      return false;
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
    std::cout << "  sync <peer:/path|hash>            Sync file or directory\n";
    std::cout << "  watch [list|add|remove|interval]  Manage directory watches\n";
    std::cout << "  settings [list|get|set|save|load] Manage runtime settings\n";
    std::cout << "  ignore [list|add|remove]          Manage persistent ignore rules\n";
    std::cout << "  conflict [list|accept|ignore|stage|view|unstage]  Review or resolve download conflicts\n";
    std::cout << "  bell                              Play notification bell\n";
    std::cout << "  dirs                              Show important directory locations\n";
    std::cout << "  pwd|cwd                           Show current working directory\n";
  }

  void ensure_directories() {
    std::error_code ec;
    std::filesystem::create_directories(share_root(), ec);
    std::filesystem::create_directories(download_root(), ec);
    std::filesystem::create_directories(config_root(), ec);
    std::filesystem::create_directories(archive_root(), ec);
    std::filesystem::create_directories(conflict_stage_root(), ec);
  }

  std::filesystem::path share_root() const {
    return std::filesystem::current_path() / "share";
  }

  std::filesystem::path download_root() const {
    return share_root() / ".sync_tmp";
  }

  std::filesystem::path config_root() const {
    return share_root() / ".config";
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
    if(bytes <= 999999) {
      return std::to_string(bytes);
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

  std::vector<PeerManager::RemoteListingItem> blocking_list_peer(const std::string& peer,
                                                                 const std::string& path,
                                                                 const std::string& hash,
                                                                 const std::string& dir_guid) {
    auto promise = std::make_shared<std::promise<std::vector<PeerManager::RemoteListingItem>>>();
    auto future = promise->get_future();
    bool requested = pm_->request_peer_listing(peer, path, hash, dir_guid,
      [promise](const std::vector<PeerManager::RemoteListingItem>& items){
        promise->set_value(items);
      });

    if(!requested) {
      std::cout << "Peer " << peer << " is not connected.\n";
      return {};
    }

    auto status = future.wait_for(std::chrono::seconds(5));
    if(status != std::future_status::ready) {
      std::cout << "Timed out waiting for response from " << peer << ".\n";
      return {};
    }
    return future.get();
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

      std::cout << std::left << std::setw(static_cast<int>(id_width)) << id_str << std::right << "  "
                << std::setw(static_cast<int>(size_width)) << size_str << "  ";
      if(show_peer_prefix) {
        std::cout << item.peer_id << ":";
      }
      std::cout << display_path << "\n";
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

  bool is_known_peer(const std::string& peer) const {
    auto arr = pm_->make_peer_list_json();
    for(auto& p : arr) {
      if(p["peer_id"].get<std::string>() == peer) return true;
    }
    return false;
  }

  void index_share_root() {
    std::lock_guard<std::mutex> guard(share_index_mutex_);
    auto root = share_root();
    std::error_code ec;
    if(!std::filesystem::exists(root, ec)) return;
    if(std::filesystem::is_regular_file(root, ec)) {
      PeerManager::SharedFileEntry existing;
      auto rel_opt = relative_to_share(root);
      if(rel_opt && pm_->find_local_file_by_path(*rel_opt, existing)) {
        std::error_code size_ec;
        auto size = std::filesystem::file_size(root, size_ec);
        if(!size_ec && existing.size == size) {
          return;
        }
      }
      share_single_file(root, false);
      return;
    }
    for(auto it = std::filesystem::recursive_directory_iterator(root, ec);
        !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
      if(it->is_directory()) {
        auto rel = it->path().lexically_relative(root).generic_string();
        if(is_internal_listing_path(rel)) {
          it.disable_recursion_pending();
          continue;
        }
        pm_->register_directory(rel);
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
  }
};
