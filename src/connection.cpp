#include "connection.hpp"
#include "protocol.hpp"
#include "utils.hpp"
#include "log.hpp"
#include <fstream>
#include <filesystem>
#include "base64.h"

using json = nlohmann::json;

std::shared_ptr<Connection> Connection::create_incoming(asio::io_context& io,
                                                        asio::ip::tcp::socket sock,
                                                        std::shared_ptr<PeerManager> pm)
{
    auto c = std::shared_ptr<Connection>(new Connection(io, std::move(sock), pm));
    c->start();
    return c;
}

Connection::Connection(asio::io_context& io, asio::ip::tcp::socket sock, std::shared_ptr<PeerManager> pm)
: io_(io), socket_(std::move(sock)), pm_(pm)
{
}

Connection::~Connection(){
    try { socket_.close(); } catch(...) {}
    if(!peer_id_.empty()){
        pm_->on_disconnected(peer_id_);
    }
}

void Connection::start(){
    // On start, send our peer_announce, and a peer_list request/offer
    auto announce = make_peer_announce(pm_->local_peer_id(),
                                   pm_->local_addr(),
                                   pm_->display_name(),
                                   pm_->external_addr());
    async_send_json(announce);

    // send local peer_list so newcomers can attach
    auto list = pm_->make_peer_list_json();
    auto msg = make_peer_list(list);
    async_send_json(msg);

    do_read();
}

void Connection::do_read(){
    auto self = shared_from_this();
    asio::async_read_until(socket_, read_buf_, "\n",
        [this, self](std::error_code ec, std::size_t bytes_transferred){
            if(ec){
                log_info("Connection read error: {}", ec.message());
                close();
                return;
            }
            std::istream is(&read_buf_);
            std::string line;
            std::getline(is, line);
            if(!line.empty()){
                handle_line(line);
            }
            do_read();
        });
}

void Connection::handle_line(std::string line){
    try{
        auto j = json::parse(line);
        auto t = j.value("type", "");
        if(t == "peer_announce"){
          std::string peer_id = j.value("peer_id","");
          std::string display_name = j.value("display_name","");
          std::string local_addr = j.value("local_addr","");
          std::string external_addr = j.value("external_addr","");
          int ttl = j.value("ttl",0);

          if(peer_id.empty() || local_addr.empty()) return;

          pm_->add_peer_discovered(peer_id, display_name, local_addr, external_addr);

          if(ttl > 1){
            json fwd = j;
            fwd["ttl"] = ttl-1;
            pm_->broadcast_json(fwd, peer_id); // use message's peer_id
          }

          if(peer_id_.empty()){
            peer_id_ = peer_id;
            pm_->on_connected(peer_id_, shared_from_this());
          } else if(peer_id_ == peer_id){
            // already connected peer reaffirming identity; no further action needed
          }
        } else if(t == "peer_list"){
          auto arr = j["peers"];
          if(arr.is_array()){
            for(auto &item : arr){
              std::string pid = item.value("peer_id","");
              std::string dname = item.value("display_name","");
              std::string local_addr = item.value("local_addr","");
              std::string external_addr = item.value("external_addr","");
              if(!pid.empty()){
                pm_->add_peer_discovered(pid,dname,local_addr,external_addr);
                json fwd = make_peer_announce(pid,local_addr,dname,external_addr,3);
                pm_->broadcast_json(fwd, pid);
              }
            }
            // attempt to connect to more peers if under limit
            pm_->attempt_connect_more();
          }
        } else if(t == "chat") {
          std::string from = j.value("from", "unknown");
          std::string text = j.value("text", "");
          pm_->dispatch_chat_message(from, text);
        } else if(t == "share"){
          std::string hash = j.value("hash","");
          std::string fname = j.value("filename","");
          print_out("[{}] shared file {} -> {}", peer_id_, fname, hash);
        } else if(t == "list_request"){
          std::string request_id = j.value("request_id","");
          std::string path = j.value("path","");
          std::string hash = j.value("hash","");
          std::string dir_guid = j.value("dir_guid", "");
          if(!request_id.empty()){
            pm_->handle_list_request(peer_id_, request_id, path, hash, dir_guid);
          }
        } else if(t == "list_response"){
          std::string request_id = j.value("request_id","");
          std::string from_peer = j.value("peer_id", peer_id_);
          std::vector<PeerManager::RemoteListingItem> items;
          if(j.contains("entries") && j["entries"].is_array()){
            for(const auto& entry : j["entries"]){
              PeerManager::RemoteListingItem item;
              item.peer_id = from_peer;
              item.hash = entry.value("hash", "");
              item.relative_path = entry.value("path", "");
              item.size = entry.value("size", 0ULL);
              item.is_directory = entry.value("is_dir", false);
              item.directory_guid = entry.value("dir_guid", "");
              items.push_back(std::move(item));
            }
          }
          if(!request_id.empty()){
            pm_->handle_list_response(from_peer, request_id, items);
          }
        } else if(t == "file_chunk_request"){
          std::string request_id = j.value("request_id", "");
          std::string hash = j.value("hash","");
          uint64_t offset = j.value("offset", 0ULL);
          std::size_t length = j.value("length", static_cast<std::size_t>(PeerManager::kMaxChunkSize));
          if(request_id.empty() || hash.empty()){
            return;
          }

          PeerManager::SharedFileEntry entry;
          if(!pm_->find_local_file_by_hash(hash, entry)){
            nlohmann::json err;
            err["type"] = "file_chunk_response";
            err["request_id"] = request_id;
            err["hash"] = hash;
            err["error"] = "Unknown file hash";
            async_send_json(err);
            return;
          }

          if(offset >= entry.size){
            nlohmann::json err;
            err["type"] = "file_chunk_response";
            err["request_id"] = request_id;
            err["hash"] = hash;
            err["error"] = "Offset beyond file size";
            async_send_json(err);
            return;
          }

          std::size_t clamped_length = std::min<std::size_t>(length ? length : PeerManager::kMaxChunkSize,
                                                             static_cast<std::size_t>(entry.size - offset));
          clamped_length = std::min<std::size_t>(clamped_length, PeerManager::kMaxChunkSize);

          std::ifstream file(entry.full_path, std::ios::binary);
          if(!file){
            nlohmann::json err;
            err["type"] = "file_chunk_response";
            err["request_id"] = request_id;
            err["hash"] = hash;
            err["error"] = "Cannot open file";
            async_send_json(err);
            return;
          }

          file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
          std::vector<char> buffer(clamped_length);
          file.read(buffer.data(), static_cast<std::streamsize>(clamped_length));
          std::streamsize read_bytes = file.gcount();
          if(read_bytes <= 0){
            nlohmann::json err;
            err["type"] = "file_chunk_response";
            err["request_id"] = request_id;
            err["hash"] = hash;
            err["error"] = "Failed to read file chunk";
            async_send_json(err);
            return;
          }
          buffer.resize(static_cast<std::size_t>(read_bytes));

          std::string encoded = base64_encode(reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
          std::string chunk_sha = sha256_hex(std::string(buffer.data(), buffer.size()));

          nlohmann::json resp;
          resp["type"] = "file_chunk_response";
          resp["request_id"] = request_id;
          resp["hash"] = hash;
          resp["offset"] = offset;
          resp["length"] = buffer.size();
          resp["chunk_sha"] = chunk_sha;
          resp["data"] = encoded;
          async_send_json(resp);
        } else if(t == "file_chunk_response"){
          std::string request_id = j.value("request_id", "");
          std::string hash = j.value("hash", "");
          uint64_t offset = j.value("offset", 0ULL);
          std::string error = j.value("error", "");
          PeerManager::ChunkResponse response;
          response.peer_id = peer_id_;
          response.hash = hash;
          response.offset = offset;
          if(!error.empty()){
            response.success = false;
            response.error = error;
          } else {
            std::string data_encoded = j.value("data", "");
            std::string decoded = base64_decode(data_encoded);
            response.data.assign(decoded.begin(), decoded.end());
            response.chunk_sha = j.value("chunk_sha", "");
            response.success = true;
          }
          pm_->handle_file_chunk_response(peer_id_, request_id, std::move(response));
        } else if(t == "file_request"){
          std::string hash = j.value("hash","");
          if(hash.empty()) return;

          // Check if we have the file locally
          PeerManager::SharedFileEntry entry;
          if(pm_->find_local_file_by_hash(hash, entry)){
            const auto& path = entry.full_path;
            std::ifstream file(path, std::ios::binary);
            if(file){
              std::vector<char> buf((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
              std::string b64 = base64_encode(reinterpret_cast<const unsigned char*>(buf.data()), buf.size());


              nlohmann::json resp;
              resp["type"] = "file_response";
              resp["hash"] = hash;
              resp["filename"] = path.filename().string();
              resp["data"] = b64;
              async_send_json(resp);
              print_out("[{}] sending file {}", peer_id_, path.string());
            } else {
              nlohmann::json err;
              err["type"] = "file_error";
              err["hash"] = hash;
              err["reason"] = "Cannot open file";
              async_send_json(err);
            }
          }
        } else if(t == "file_response"){
          std::string hash = j.value("hash","");
          std::string fname = j.value("filename","");
          std::string data = j.value("data","");
          if(hash.empty() || data.empty()) return;

          // Decode base64 and write to local file
          auto bytes = base64_decode(data);  // returns vector<char>
          
          std::filesystem::path outpath = std::filesystem::current_path() / "download" / fname;
          std::filesystem::create_directories(outpath.parent_path());

          std::ofstream out(outpath, std::ios::binary);
          out.write(bytes.data(), bytes.size());
          print_out("[{}] received file {} -> {}", peer_id_, fname, outpath.string());
        } else {
          log_info("Unknown message type: {}", t);
        }
    } catch(const std::exception& ex){
        log_warn("Failed to parse JSON: {}  raw: {}", ex.what(), line);
    }
}

void Connection::async_send_json(const nlohmann::json& j){
    auto s = j.dump() + "\n";
    bool start_write = false;
    {
        // simple guarding by write_queue_
        start_write = write_queue_.empty();
        write_queue_.push_back(std::move(s));
    }
    if(start_write){
        do_write();
    }
}

void Connection::do_write(){
    if(write_queue_.empty()) return;
    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
        [this, self](std::error_code ec, std::size_t){
        if(ec){
            log_info("Connection write error: {}", ec.message());
            close();
            return;
        }
            write_queue_.pop_front();
            if(!write_queue_.empty()){
                do_write();
            } else {
                writing_ = false;
            }
        });
}

void Connection::close(){
    try { socket_.close(); } catch(...) {}
    if(!peer_id_.empty()) pm_->on_disconnected(peer_id_);
}

void Connection::connect_outgoing(asio::io_context& io,
                                 const std::string& host,
                                 unsigned short port,
                                 std::shared_ptr<PeerManager> pm)
{
    auto resolver = std::make_shared<asio::ip::tcp::resolver>(io);
    resolver->async_resolve(host, std::to_string(port),
        [resolver, &io, pm, host, port](std::error_code ec, asio::ip::tcp::resolver::results_type results){
            if(ec){
                log_info("Resolve failed for {}:{}  {}", host, port, ec.message());
                return;
            }
            auto sock = std::make_shared<asio::ip::tcp::socket>(io);
            asio::async_connect(*sock, results,
                [sock, pm](std::error_code ec, asio::ip::tcp::endpoint ep){
                    if(ec){
                        log_info("Connect failed: {}", ec.message());
                        return;
                    }
                    log_info("Connected outgoing to {}:{}", ep.address().to_string(), ep.port());
                    auto conn = Connection::create_incoming(pm->io(), std::move(*sock), pm);
                    // We created a Connection with the connected socket and reused the create_incoming path
                    // It will exchange announces and then call pm->on_connected when peer_id is seen.
                });
        });
}
