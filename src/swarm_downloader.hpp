#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "peer_manager.hpp"

struct SwarmConfig {
  std::size_t chunk_size = PeerManager::kMaxChunkSize;
  std::size_t total_chunks = 0;
  std::size_t max_parallel = 0; // 0 = all
  std::size_t chunk_buffers = 1;
  std::chrono::milliseconds progress_interval{200};
  bool enable_meter = false;
  bool enable_dynamic_providers = false;
  std::chrono::milliseconds provider_refresh_interval{1500};
};

using SwarmChunkFetcher = std::function<PeerManager::ChunkResponse(
  const std::string& peer_id,
  const std::string& hash,
  uint64_t offset,
  std::size_t length)>;

using SwarmChunkWriter = std::function<bool(uint64_t offset, const std::vector<char>& data)>;

using SwarmMeterCallback = std::function<void(const std::vector<uint8_t>& chunk_states,
                                              uint64_t downloaded_bytes,
                                              bool force)>;

using SwarmPeerProvider = std::function<std::vector<std::string>()>;

bool run_swarm_download(const std::string& hash,
                        uint64_t file_size,
                        const SwarmConfig& config,
                        const std::vector<PeerManager::RemoteListingItem>& providers,
                        std::vector<uint8_t>& chunk_states,
                        const SwarmChunkFetcher& fetch_chunk,
                        const SwarmChunkWriter& write_chunk,
                        const SwarmMeterCallback& meter_callback,
                        std::string& failure_reason,
                        const SwarmPeerProvider& refresh_providers = {});
