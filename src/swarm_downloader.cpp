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
                        const SwarmPeerProvider& refresh_providers) {
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

  std::vector<PeerSlot> peer_slots;
  peer_slots.reserve(swarm_peers.size());
  const std::size_t buffers_per_peer = std::max<std::size_t>(1, config.chunk_buffers);
  const std::size_t max_parallel = (config.max_parallel == 0)
    ? std::numeric_limits<std::size_t>::max()
    : config.max_parallel;
  std::unordered_set<std::string> known_peers(swarm_peers.begin(), swarm_peers.end());

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

  std::mutex peer_mutex;
  std::condition_variable peer_cv;

  auto add_peer_slots_locked = [&](const std::string& peer_id) -> bool {
    if(peer_id.empty()) return false;
    if(peer_slots.size() >= max_parallel) return false;
    std::size_t existing = 0;
    for(const auto& slot : peer_slots) {
      if(slot.peer_id == peer_id) ++existing;
    }
    bool added = false;
    while(existing < buffers_per_peer && peer_slots.size() < max_parallel) {
      peer_slots.push_back(PeerSlot{peer_id, false});
      ++existing;
      added = true;
    }
    return added;
  };

  {
    std::lock_guard<std::mutex> lock(peer_mutex);
    for(const auto& peer : swarm_peers) {
      add_peer_slots_locked(peer);
      if(peer_slots.size() >= max_parallel) break;
    }
  }
  if(peer_slots.empty()) return false;

  std::mutex progress_mutex;
  uint64_t downloaded_bytes = 0;
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
        bool notify = false;
        {
          std::lock_guard<std::mutex> lock(peer_mutex);
          for(const auto& peer : latest) {
            if(peer.empty()) continue;
            if(!known_peers.insert(peer).second) continue;
            if(add_peer_slots_locked(peer)) {
              notify = true;
            }
          }
        }
        if(notify) {
          peer_cv.notify_all();
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
    peer_cv.notify_all();
  };

  auto acquire_peer = [&]() -> std::optional<std::size_t> {
    std::unique_lock<std::mutex> lock(peer_mutex);
    peer_cv.wait(lock, [&]{
      if(failure || shutting_down) return true;
      for(const auto& slot : peer_slots) {
        if(!slot.busy) return true;
      }
      return false;
    });
    if(failure || shutting_down) return std::nullopt;
    for(std::size_t i = 0; i < peer_slots.size(); ++i) {
      if(!peer_slots[i].busy) {
        peer_slots[i].busy = true;
        return i;
      }
    }
    return std::nullopt;
  };

  auto release_peer = [&](std::size_t index){
    {
      std::lock_guard<std::mutex> lock(peer_mutex);
      peer_slots[index].busy = false;
    }
    peer_cv.notify_one();
  };

  auto worker_fn = [&](std::size_t /*worker_id*/){
    while(true) {
      if(failure) break;
      auto job_opt = take_job();
      if(!job_opt) break;
      std::size_t job_index = *job_opt;
      auto peer_slot = acquire_peer();
      if(!peer_slot) {
        requeue_job(job_index, "Swarm shutdown", true);
        break;
      }
      std::size_t peer_index = *peer_slot;
      std::string peer_id = peer_slots[peer_index].peer_id;
      uint64_t offset = static_cast<uint64_t>(job_index) * config.chunk_size;
      auto length = static_cast<std::size_t>(std::min<uint64_t>(config.chunk_size, file_size - offset));
      auto resp = fetch_chunk(peer_id, hash, offset, length);
      release_peer(peer_index);
      if(!resp.success || resp.data.empty()) {
        job_states[job_index].attempts++;
        bool fatal = job_states[job_index].attempts >= std::max<std::size_t>(3, peer_slots.size() * 2);
        requeue_job(job_index, "Failed to download chunk", fatal);
        continue;
      }
      std::string local_sha = sha256_hex(std::string(resp.data.data(), resp.data.size()));
      if(!resp.chunk_sha.empty() && resp.chunk_sha != local_sha) {
        job_states[job_index].attempts++;
        bool fatal = job_states[job_index].attempts >= std::max<std::size_t>(3, peer_slots.size() * 2);
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
        peer_cv.notify_all();
      }
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(peer_slots.size());
  for(std::size_t i = 0; i < peer_slots.size(); ++i) {
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
