#pragma once
#include <asio.hpp>
#include <string>
#include <functional>

class Client {
public:
    Client(asio::io_context& io);
    void connect_and_send(const std::string& host, unsigned short port, const std::string& message);
private:
    asio::io_context& io_;
};
