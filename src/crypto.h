#pragma once
#include <string>

// sha-1 and base64, used by websocket handshakes and basic auth

// raw 20-byte sha-1 digest
std::string sha1_raw(const std::string& data);
std::string sha1_hex(const std::string& data);
std::string base64_encode(const std::string& data);
// false when the input is not valid base64
bool base64_decode(const std::string& in, std::string& out);
