#include "client.hpp"
#include "log.hpp"

Client::Client(asio::io_context& io) : io_(io) {}

void Client::connect_and_send(const std::string& host, unsigned short port, const std::string& message){
    auto resolver = std::make_shared<asio::ip::tcp::resolver>(io_);
    auto sock = std::make_shared<asio::ip::tcp::socket>(io_);
    resolver->async_resolve(host, std::to_string(port),
        [resolver, sock, message](std::error_code ec, asio::ip::tcp::resolver::results_type results){
            if(ec){ log_error(nullptr, "resolve failed: {}", ec.message()); return; }
            asio::async_connect(*sock, results,
                [sock, message](std::error_code ec, asio::ip::tcp::endpoint ep){
                    if(ec){ log_error(nullptr, "connect failed: {}", ec.message()); return; }
                    log_info(nullptr, "Connected to {}", ep.address().to_string());
                    std::string m = message + "\n";
                    asio::async_write(*sock, asio::buffer(m),
                        [sock](std::error_code ec, std::size_t){
                            if(ec) log_error(nullptr, "write failed: {}", ec.message());
                            else log_info(nullptr, "message sent");
                        });
                });
        });
}
