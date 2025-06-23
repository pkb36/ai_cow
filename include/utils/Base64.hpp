#pragma once

#include <string>
#include <vector>
#include <glib.h>

class Base64 {
public:
    static std::string encode(const std::vector<uint8_t>& data) {
        if (data.empty()) return "";
        
        gchar* encoded = g_base64_encode(data.data(), data.size());
        std::string result(encoded);
        g_free(encoded);
        
        return result;
    }
    
    static std::string encode(const std::string& data) {
        return encode(std::vector<uint8_t>(data.begin(), data.end()));
    }
    
    static std::vector<uint8_t> decode(const std::string& encoded) {
        if (encoded.empty()) return {};
        
        gsize len;
        guchar* decoded = g_base64_decode(encoded.c_str(), &len);
        
        std::vector<uint8_t> result(decoded, decoded + len);
        g_free(decoded);
        
        return result;
    }
    
    static std::string decodeToString(const std::string& encoded) {
        auto data = decode(encoded);
        return std::string(data.begin(), data.end());
    }
};