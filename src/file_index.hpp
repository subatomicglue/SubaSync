#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

struct FileEntry {
    std::string path;
    std::string file_hash; // hex
    uint64_t size;
    uint32_t chunk_size;
    std::vector<std::string> chunk_hashes; // hex strings
};

class FileIndex {
public:
    void add_or_update(const FileEntry&);
    std::vector<FileEntry> list();
    bool has(const std::string& file_hash);
    FileEntry get(const std::string& file_hash);
private:
    std::mutex m_;
    std::unordered_map<std::string, FileEntry> map_; // file_hash -> entry
};
