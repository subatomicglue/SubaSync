cmake_minimum_required(VERSION 3.20)
project(sync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(asio REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(cpptrace REQUIRED)
find_package(Readline QUIET)

add_executable(sync
    src/main.cpp
    src/base64.cpp
    src/connection.cpp
    src/peer_manager.cpp
    src/server.cpp
    src/client.cpp
    src/file_index.cpp
    src/protocol.cpp
    src/utils.cpp
    src/log.cpp
    src/swarm_downloader.cpp
)

target_include_directories(sync PRIVATE src)
target_compile_definitions(sync PRIVATE ASIO_STANDALONE)

target_link_libraries(sync PRIVATE
    asio::asio
    OpenSSL::SSL
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    cpptrace::cpptrace
)

if(Readline_FOUND)
  target_link_libraries(sync PRIVATE Readline::Readline)
  target_compile_definitions(sync PRIVATE HAVE_READLINE)
endif()
