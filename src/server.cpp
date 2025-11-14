#include "server.hpp"
#include "log.hpp"

Server::Server(asio::io_context& io, unsigned short port)
: io_(io),
  acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port))
{
}

void Server::start_accept(){ do_accept(); }

void Server::do_accept(){
    acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket sock){
        if(!ec){
            log_info("Accepted connection from {}", sock.remote_endpoint().address().to_string());
            // For skeleton: read a single JSON message, then close.
            auto buf = std::make_shared<asio::streambuf>();
            asio::async_read_until(sock, *buf, "\n",
                [sock = std::move(sock), buf](std::error_code ec, std::size_t bytes){
                    if(ec){ log_error("read failed: {}", ec.message()); return; }
                    std::istream is(buf.get());
                    std::string line;
                    std::getline(is, line);
                    log_info("Server received: {}", line);
                });
        } else {
            log_error("accept failed: {}", ec.message());
        }
        do_accept();
    });
}
