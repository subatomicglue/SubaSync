#include "utils.hpp"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>

std::string hex_from_bytes(const std::vector<unsigned char>& b){
    std::ostringstream oss;
    for(auto c: b) oss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return oss.str();
}

std::vector<unsigned char> sha256_bytes(const std::string &data){
    std::vector<unsigned char> out(SHA256_DIGEST_LENGTH);
    SHA256((const unsigned char*)data.data(), data.size(), out.data());
    return out;
}

std::string sha256_hex(const std::string &data){
    return hex_from_bytes(sha256_bytes(data));
}
