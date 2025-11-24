#include "swarm_downloader.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "utils.hpp"

namespace {

struct PeerSlot {
  std::string peer_id;
  bool busy = false;
};

}

bool run_swarm_download(const std::string& hash,
                        uint64_t file_size,
                        const SwarmConfig& config,
                        const std::vector<PeerManager::RemoteListingItem>& providers,
                        std::vector<uint8_t>& chunk_states,
                        const SwarmChunkFetcher& fetch_chunk,
                        const SwarmChunkWriter& write_chunk,
                        const SwarmMeterCallback& meter_callback,
                        std::string& failure_reason,
                        const SwarmPeerProvider& refresh_providers,
                        SwarmPeerStatsMap* peer_stats,
                        const std::shared_ptr<std::atomic<std::size_t>>& active_peer_count) {
  if(file_size == 0 || providers.empty()) return false;
  if(!fetch_chunk || !write_chunk) return false;

  const std::size_t total_chunks = config.total_chunks;
  if(total_chunks == 0 || chunk_states.size() != total_chunks) return false;

  std::vector<std::string> swarm_peers;
  swarm_peers.reserve(providers.size());
  std::unordered_set<std::string> seen;
  for(const auto& provider : providers) {
    if(provider.peer_id.empty()) continue;
    if(seen.insert(provider.peer_id).second) {
      swarm_peers.push_back(provider.peer_id);
    }
  }
  if(swarm_peers.empty()) return false;

  std::vector<std::size_t> chunk_order(total_chunks);
  std::iota(chunk_order.begin(), chunk_order.end(), 0);
  {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(chunk_order.begin(), chunk_order.end(), rng);
  }

  struct PeerState {
    std::string peer_id;
    std::size_t outstanding = 0;
  };

  using PeerStatePtr = std::shared_ptr<PeerState>;

  std::vector<PeerStatePtr> peer_states;
  peer_states.reserve(swarm_peers.size());
  std::unordered_set<std::string> known_peers(swarm_peers.begin(), swarm_peers.end());
  const std::size_t per_peer_limit = std::max<std::size_t>(1, config.chunk_buffers);
  if(peer_stats) {
    for(const auto& peer : swarm_peers) {
      (*peer_stats)[peer];
    }
  }
  for(const auto& peer : swarm_peers) {
    if(peer.empty()) continue;
    peer_states.push_back(std::make_shared<PeerState>(PeerState{peer, 0}));
  }
  if(peer_states.empty()) return false;

  std::size_t worker_count = config.max_parallel == 0
    ? peer_states.size()
    : std::max<std::size_t>(1, config.max_parallel);

  struct JobState {
    std::size_t attempts = 0;
  };
  std::vector<JobState> job_states(total_chunks);

  std::mutex job_mutex;
  std::condition_variable job_cv;
  std::deque<std::size_t> job_queue(chunk_order.begin(), chunk_order.end());
  std::size_t active_jobs = 0;
  std::size_t completed_jobs = 0;
  bool shutting_down = false;
  bool failure = false;

  std::mutex peer_capacity_mutex;
  std::condition_variable peer_capacity_cv;
  std::atomic<std::size_t> next_peer_index{0};

  auto add_peer_state = [&](const std::string& peer_id) {
    if(peer_id.empty()) return;
    {
      std::lock_guard<std::mutex> lock(peer_capacity_mutex);
      auto exists = std::any_of(peer_states.begin(), peer_states.end(),
        [&](const PeerStatePtr& state){ return state && state->peer_id == peer_id; });
      if(exists) return;
      peer_states.push_back(std::make_shared<PeerState>(PeerState{peer_id, 0}));
      if(peer_stats) (*peer_stats)[peer_id];
    }
    peer_capacity_cv.notify_all();
  };

  auto acquire_peer_state = [&]() -> PeerStatePtr {
    std::unique_lock<std::mutex> lock(peer_capacity_mutex);
    peer_capacity_cv.wait(lock, [&]{
      if(shutting_down || failure) return true;
      for(const auto& state : peer_states) {
        if(state && state->outstanding < per_peer_limit) return true;
      }
      return false;
    });
    if(shutting_down || failure) return nullptr;
    std::size_t start = next_peer_index.load(std::memory_order_relaxed);
    for(std::size_t i = 0; i < peer_states.size(); ++i) {
      std::size_t idx = (start + i) % peer_states.size();
      auto& state = peer_states[idx];
      if(state && state->outstanding < per_peer_limit) {
        state->outstanding++;
        next_peer_index.store((idx + 1) % peer_states.size(), std::memory_order_relaxed);
        return state;
      }
    }
    return nullptr;
  };

  auto release_peer_state = [&](const PeerStatePtr& state){
    if(!state) return;
    {
      std::lock_guard<std::mutex> lock(peer_capacity_mutex);
      if(state->outstanding > 0) --state->outstanding;
    }
    peer_capacity_cv.notify_all();
  };

  std::mutex progress_mutex;
  uint64_t downloaded_bytes = 0;
  std::mutex stats_mutex;
  auto meter_interval = config.progress_interval.count() > 0
    ? config.progress_interval
    : std::chrono::milliseconds(200);
  std::atomic<bool> meter_stop{false};
  auto emit_meter = [&](bool force = false){
    if(!config.enable_meter || !meter_callback) return;
    std::lock_guard<std::mutex> lock(progress_mutex);
    meter_callback(chunk_states, downloaded_bytes, force);
  };

  std::thread meter_thread;
  if(config.enable_meter && meter_callback) {
    meter_thread = std::thread([&](){
      while(!meter_stop.load()) {
        std::this_thread::sleep_for(meter_interval);
        emit_meter(true);
      }
    });
  }

  std::atomic<bool> refresh_stop{false};
  std::thread refresh_thread;
  if(refresh_providers && config.enable_dynamic_providers && config.provider_refresh_interval.count() > 0) {
    auto refresh_interval = config.provider_refresh_interval;
    refresh_thread = std::thread([&, refresh_interval](){
      while(!refresh_stop.load()) {
        auto latest = refresh_providers();
        for(const auto& peer : latest) {
          if(peer.empty()) continue;
          if(!known_peers.insert(peer).second) continue;
          add_peer_state(peer);
        }
        auto sleep_interval = refresh_interval.count() > 0
          ? refresh_interval
          : std::chrono::milliseconds(1500);
        auto deadline = std::chrono::steady_clock::now() + sleep_interval;
        while(!refresh_stop.load() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      }
    });
  }

  auto take_job = [&]() -> std::optional<std::size_t> {
    std::unique_lock<std::mutex> lock(job_mutex);
    job_cv.wait(lock, [&]{
      return shutting_down || failure || !job_queue.empty();
    });
    if(shutting_down || failure) return std::nullopt;
    if(job_queue.empty()) return std::nullopt;
    std::size_t job = job_queue.front();
    job_queue.pop_front();
    ++active_jobs;
    return job;
  };

  auto requeue_job = [&](std::size_t job_index, const std::string& reason, bool fatal){
    {
      std::lock_guard<std::mutex> lock(job_mutex);
      if(fatal) {
        failure = true;
        if(failure_reason.empty()) failure_reason = reason;
        shutting_down = true;
      } else {
        job_queue.push_back(job_index);
      }
      if(active_jobs > 0) --active_jobs;
      if(shutting_down || failure) job_queue.clear();
    }
    job_cv.notify_all();
    peer_capacity_cv.notify_all();
  };

  auto worker_fn = [&](std::size_t /*worker_id*/){
    while(true) {
      if(failure) break;
      auto job_opt = take_job();
      if(!job_opt) break;
      std::size_t job_index = *job_opt;
      auto peer_state = acquire_peer_state();
      if(!peer_state) {
        requeue_job(job_index, "Swarm shutdown", true);
        break;
      }
      std::string peer_id = peer_state->peer_id;
      uint64_t offset = static_cast<uint64_t>(job_index) * config.chunk_size;
      auto length = static_cast<std::size_t>(std::min<uint64_t>(config.chunk_size, file_size - offset));
      auto resp = fetch_chunk(peer_id, hash, offset, length);
      release_peer_state(peer_state);
      if(!resp.success || resp.data.empty()) {
        job_states[job_index].attempts++;
        bool fatal = job_states[job_index].attempts >= std::max<std::size_t>(3, peer_states.size() * 2);
        requeue_job(job_index, "Failed to download chunk", fatal);
        continue;
      }
      std::string local_sha = sha256_hex(std::string(resp.data.data(), resp.data.size()));
      if(!resp.chunk_sha.empty() && resp.chunk_sha != local_sha) {
        job_states[job_index].attempts++;
        bool fatal = job_states[job_index].attempts >= std::max<std::size_t>(3, peer_states.size() * 2);
        requeue_job(job_index, "Chunk checksum mismatch", fatal);
        continue;
      }
      if(!write_chunk(offset, resp.data)) {
        requeue_job(job_index, "Failed writing chunk to disk", true);
        break;
      }
      {
        std::lock_guard<std::mutex> lock(progress_mutex);
        if(job_index < chunk_states.size()) chunk_states[job_index] = 1;
        downloaded_bytes += resp.data.size();
        bool first_chunk_from_peer = false;
        if(peer_stats) {
          std::lock_guard<std::mutex> stats_lock(stats_mutex);
          auto& stat = (*peer_stats)[peer_id];
          if(stat.bytes == 0) first_chunk_from_peer = true;
          stat.chunks += 1;
          stat.bytes += resp.data.size();
        }
        if(first_chunk_from_peer && active_peer_count) {
          active_peer_count->fetch_add(1, std::memory_order_relaxed);
        }
      }
      emit_meter(false);

      bool finished = false;
      {
        std::lock_guard<std::mutex> lock(job_mutex);
        if(completed_jobs < total_chunks) ++completed_jobs;
        if(active_jobs > 0) --active_jobs;
        if(completed_jobs >= total_chunks && !shutting_down) {
          shutting_down = true;
          finished = true;
        }
      }
      if(finished) {
        job_cv.notify_all();
        peer_capacity_cv.notify_all();
      }
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for(std::size_t i = 0; i < worker_count; ++i) {
    workers.emplace_back(worker_fn, i);
  }
  for(auto& thread : workers) {
    if(thread.joinable()) thread.join();
  }
  meter_stop = true;
  if(meter_thread.joinable()) meter_thread.join();
  refresh_stop = true;
  if(refresh_thread.joinable()) refresh_thread.join();

  if(failure) {
    if(failure_reason.empty()) {
      failure_reason = "Swarm download failed.";
    }
    return false;
  }
  emit_meter(true);
  return completed_jobs == total_chunks;
}
