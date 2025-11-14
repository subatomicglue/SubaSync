#include "file_index.hpp"
#include <stdexcept>

void FileIndex::add_or_update(const FileEntry& e){
    std::lock_guard lg(m_);
    map_[e.file_hash]=e;
}

std::vector<FileEntry> FileIndex::list(){
    std::lock_guard lg(m_);
    std::vector<FileEntry> out;
    out.reserve(map_.size());
    for(auto &p: map_) out.push_back(p.second);
    return out;
}

bool FileIndex::has(const std::string& file_hash){
    std::lock_guard lg(m_);
    return map_.count(file_hash)>0;
}

FileEntry FileIndex::get(const std::string& file_hash){
    std::lock_guard lg(m_);
    if(!map_.count(file_hash)) throw std::runtime_error("no such file");
    return map_.at(file_hash);
}
