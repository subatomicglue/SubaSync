#include "connection.hpp"
#include "protocol.hpp"
#include "utils.hpp"
#include "log.hpp"
#include <fstream>
#include <filesystem>

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
        pm_->handle_message(shared_from_this(), j);
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
