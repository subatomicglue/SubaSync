#pragma once
#include <string>
#include <vector>

std::string hex_from_bytes(const std::vector<unsigned char>&);
std::vector<unsigned char> sha256_bytes(const std::string &data);
std::string sha256_hex(const std::string &data);
