#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

class SerialPort {
public:
    struct Config {
        std::string portName = "/dev/ttyUSB0";
        int baudRate = 38400;
        int dataBits = 8;
        char parity = 'N';
        int stopBits = 1;
        int readTimeout = 100; // ms
    };

    using DataCallback = std::function<void(const std::vector<uint8_t>&)>;

    static SerialPort& getInstance() {
        static SerialPort instance;
        return instance;
    }

    bool open(const Config& config);
    void close();
    bool isOpen() const;

    // 데이터 전송
    bool send(const std::vector<uint8_t>& data);
    bool send(const std::string& hexString);
    
    // 동기 읽기
    std::vector<uint8_t> read(size_t maxBytes, int timeoutMs = -1);
    
    // 비동기 읽기 콜백
    void setDataCallback(DataCallback callback) { dataCallback_ = callback; }

    // PTZ 특화 함수들
    bool sendPtzCommand(uint8_t command, const std::vector<uint8_t>& params = {});
    uint8_t calculateChecksum(const std::vector<uint8_t>& data);

private:
    SerialPort();
    ~SerialPort();

    void readThread();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    
    Config config_;
    std::atomic<bool> running_{false};
    std::thread readThread_;
    DataCallback dataCallback_;
    
    mutable std::mutex sendMutex_;
};