# Project Architecture

This is a C++ 17 command line application with:

- this app sets up a mesh network with your peers/friends
  - useful for digital libraries to sync/replicate their archives content to other peers
- GUI is a command line interface, defined by MeshCLI
- We're using Conan version 2.19.1 (conanfile.txt) for package dependencies
- we're using cmake version 4.0.3 (CMakeLists.txt) for build system
- We're using Apple clang version 16.0.0 (clang-1600.0.26.6) for C++17 compiler
- we kick off the build using "make" (see the Makefile "all" default target) which builds both release an debug, we only use minimal simple Makefile to invoke cmake/conan/lldb however.
- we can debug using lldb, using the Makefile's lldb target (make lldb)
- built executable artifact goes into build/Release/sync and build/Debug/sync
- all sourcecode is in src/ and we keep it flat and simple until we find a need to complexify
- I use run.sh open 3 instances of "sync" running in 3 terminals, use kill.sh to close those.

## Coding Standards

- Use 2 spaces for indent
- Follow the existing naming conventions
- curlybrace goes on same line as if/for/while/function

# External Dependencies

We're using Conan version 2.19.1 (conanfile.txt) for package dependencies
- asio/1.28.0 - [non boost asio documentation](https://think-async.com/Asio/asio-1.36.0/doc/)
- nlohmann_json/3.11.3 - https://github.com/nlohmann/json
- openssl/1.1.1t - https://docs.openssl.org/1.1.1/
- spdlog/1.12.0 - https://github.com/gabime/spdlog
- cpptrace/1.0.4 - https://github.com/jeremy-rifkin/cpptrace
- zlib/1.3.1 - https://zlib.net/manual.html

When implementing features, reference these patterns.

## General Description

I want to create a cpp17 command line app, with asio networking, using conan and cmake.

Peer to peer.     We can share our app's external ip address and open a port.   Perhaps later we can use mdns discovery for local networks but probably not important.

Peers are given an address and credentials for your peer.    Once connected, peers are propagated to every node in the peer group (mesh).   

I can share files with my peer.   Other peers can see my listing as well as other peers listings.    I can choose to sync directories or files from another peer, to my machine, either within my shared files (which will automatically show up as shared) or to a hidden area on my own filesystem.    

Every file shared has a hash.   Unique to that file.   

When I select a peer's file or directory for sync, I choose a destination, and it begins to copy.  The way it copies is round robin from all peers who have that hash.    Occasionally getting the same segment from multiple peers to compare a spot check to validate one of the peers isn't spoofing...

Deleted or removed files could be auto deleted on synced peers, or, ignored.  To prevent cascading deletes wiping out libraries.   

This system could provide a resilient digital library that is geographically redundant.  No more book burners 

For security, each peer's listing of files shared, could be password protected, or by request...   if you have the credentials, you get access to sync them, if you request access, then that peer would have to accept.   

In contrast, only the file listings are protected.  To simply copy a file by hash, can come from any peer in your mesh...  no protections, this is like BitTorrent...

