#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

class StringUtils {
public:
    // 문자열 분할
    static std::vector<std::string> split(const std::string& str, char delimiter) {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;
        
        while (std::getline(ss, token, delimiter)) {
            if (!token.empty()) {
                tokens.push_back(token);
            }
        }
        
        return tokens;
    }
    
    // 문자열 트림
    static std::string trim(const std::string& str) {
        auto start = std::find_if(str.begin(), str.end(), 
            [](unsigned char ch) { return !std::isspace(ch); });
        
        auto end = std::find_if(str.rbegin(), str.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base();
        
        return (start < end) ? std::string(start, end) : std::string();
    }
    
    // 대소문자 변환
    static std::string toLower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return result;
    }
    
    static std::string toUpper(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::toupper(c); });
        return result;
    }
    
    // 문자열 치환
    static std::string replace(const std::string& str, 
                              const std::string& from, 
                              const std::string& to) {
        std::string result = str;
        size_t pos = 0;
        
        while ((pos = result.find(from, pos)) != std::string::npos) {
            result.replace(pos, from.length(), to);
            pos += to.length();
        }
        
        return result;
    }
    
    // 헥스 문자열 변환
    static std::string toHex(const std::vector<uint8_t>& data) {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        
        for (auto byte : data) {
            ss << std::setw(2) << static_cast<int>(byte) << " ";
        }
        
        return ss.str();
    }
    
    static std::vector<uint8_t> fromHex(const std::string& hex) {
        std::vector<uint8_t> result;
        std::istringstream iss(hex);
        std::string byte;
        
        while (iss >> byte) {
            if (byte.length() == 2) {
                result.push_back(static_cast<uint8_t>(
                    std::stoi(byte, nullptr, 16)));
            }
        }
        
        return result;
    }
};