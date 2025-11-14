#pragma once
#include <asio.hpp>
#include <memory>
#include "file_index.hpp"

class Server {
public:
    Server(asio::io_context& io, unsigned short port);
    void start_accept();
private:
    void do_accept();
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    FileIndex file_index_; // local index
};
