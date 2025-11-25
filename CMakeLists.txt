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

set(SYNC_CORE_SOURCES
    src/base64.cpp
    src/connection.cpp
    src/command_line_parser.cpp
    src/peer_manager.cpp
    src/server.cpp
    src/client.cpp
    src/file_index.cpp
    src/protocol.cpp
    src/utils.cpp
    src/log.cpp
    src/mesh_engine.cpp
    src/swarm_downloader.cpp
)

add_library(sync_core STATIC ${SYNC_CORE_SOURCES})
target_include_directories(sync_core PRIVATE src)
target_compile_definitions(sync_core PUBLIC ASIO_STANDALONE)
target_link_libraries(sync_core
    PUBLIC
    asio::asio
    OpenSSL::SSL
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    cpptrace::cpptrace
)

add_executable(sync src/main.cpp)
target_link_libraries(sync PRIVATE sync_core)

add_executable(mesh_engine_sample tests/mesh_engine_sample.cpp)
target_link_libraries(mesh_engine_sample PRIVATE sync_core)
target_include_directories(mesh_engine_sample PRIVATE tests src)

add_executable(mesh_test_runner
    tests/mesh_test_runner.cpp
)
target_link_libraries(mesh_test_runner PRIVATE sync_core)
target_include_directories(mesh_test_runner PRIVATE tests src)

add_custom_target(build_all_exes ALL
  DEPENDS sync mesh_engine_sample mesh_test_runner
)

if(Readline_FOUND)
  target_link_libraries(sync_core PUBLIC Readline::Readline)
  target_compile_definitions(sync_core PUBLIC HAVE_READLINE)
endif()
