#pragma once
#include <asio.hpp>
#include <memory>
#include <string>
#include <deque>
#include <nlohmann/json.hpp>
#include "peer_manager.hpp"

class Connection : public std::enable_shared_from_this<Connection> {
public:
    // For incoming connection (accepted socket)
    static std::shared_ptr<Connection> create_incoming(asio::io_context& io,
                                                       asio::ip::tcp::socket sock,
                                                       std::shared_ptr<PeerManager> pm);

    // For outgoing connection (we will resolve/connect)
    static void connect_outgoing(asio::io_context& io,
                                 const std::string& host,
                                 unsigned short port,
                                 std::shared_ptr<PeerManager> pm);

    ~Connection();

    void start(); // start read loop (for incoming)
    void async_send_json(const nlohmann::json& j);

    std::string peer_id() const { return peer_id_; }
    void set_peer_id(const std::string& id) { peer_id_ = id; }

private:
    Connection(asio::io_context& io, asio::ip::tcp::socket sock, std::shared_ptr<PeerManager> pm);
    void do_read();
    void handle_line(std::string line);
    void do_write();
    void close();

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    std::shared_ptr<PeerManager> pm_;
    asio::streambuf read_buf_;
    std::deque<std::string> write_queue_;
    std::string peer_id_;
    bool writing_ = false;
};
