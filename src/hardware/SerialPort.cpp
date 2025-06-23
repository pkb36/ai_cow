#include "hardware/SerialPort.hpp"
#include "core/Logger.hpp"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <sstream>
#include <iomanip>

struct SerialPort::Impl {
    int fd = -1;
    struct termios oldTermios;
};

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const Config& config) {
    if (isOpen()) {
        LOG_WARNING("Serial port already open");
        return true;
    }

    impl_ = std::make_unique<Impl>();
    config_ = config;

    // 포트 열기
    impl_->fd = ::open(config.portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (impl_->fd < 0) {
        LOG_ERROR("Failed to open serial port {}: {}", config.portName, strerror(errno));
        return false;
    }

    // 현재 설정 저장
    if (tcgetattr(impl_->fd, &impl_->oldTermios) < 0) {
        LOG_ERROR("Failed to get terminal attributes: {}", strerror(errno));
        ::close(impl_->fd);
        impl_->fd = -1;
        return false;
    }

    // 새 설정 구성
    struct termios newTermios;
    memset(&newTermios, 0, sizeof(newTermios));

    // Baud rate 설정
    speed_t baudRate;
    switch (config.baudRate) {
        case 9600: baudRate = B9600; break;
        case 19200: baudRate = B19200; break;
        case 38400: baudRate = B38400; break;
        case 57600: baudRate = B57600; break;
        case 115200: baudRate = B115200; break;
        default:
            LOG_ERROR("Unsupported baud rate: {}", config.baudRate);
            ::close(impl_->fd);
            impl_->fd = -1;
            return false;
    }

    cfsetispeed(&newTermios, baudRate);
    cfsetospeed(&newTermios, baudRate);

    // 8N1 설정
    newTermios.c_cflag = CLOCAL | CREAD;
    newTermios.c_cflag |= CS8; // 8 data bits
    newTermios.c_iflag = IGNPAR;
    newTermios.c_oflag = 0;
    newTermios.c_lflag = 0;

    // 타임아웃 설정
    newTermios.c_cc[VTIME] = config.readTimeout / 100;
    newTermios.c_cc[VMIN] = 0;

    // 설정 적용
    tcflush(impl_->fd, TCIFLUSH);
    if (tcsetattr(impl_->fd, TCSANOW, &newTermios) < 0) {
        LOG_ERROR("Failed to set terminal attributes: {}", strerror(errno));
        ::close(impl_->fd);
        impl_->fd = -1;
        return false;
    }

    // 읽기 스레드 시작
    running_ = true;
    readThread_ = std::thread(&SerialPort::readThread, this);

    LOG_INFO("Serial port {} opened successfully", config.portName);
    return true;
}

void SerialPort::close() {
    if (!isOpen()) return;

    running_ = false;
    if (readThread_.joinable()) {
        readThread_.join();
    }

    if (impl_ && impl_->fd >= 0) {
        tcsetattr(impl_->fd, TCSANOW, &impl_->oldTermios);
        ::close(impl_->fd);
        impl_->fd = -1;
    }

    LOG_INFO("Serial port closed");
}

bool SerialPort::send(const std::vector<uint8_t>& data) {
    if (!isOpen()) {
        LOG_ERROR("Serial port not open");
        return false;
    }

    std::lock_guard<std::mutex> lock(sendMutex_);

    ssize_t written = ::write(impl_->fd, data.data(), data.size());
    if (written != static_cast<ssize_t>(data.size())) {
        LOG_ERROR("Failed to write all data: {} of {} bytes", written, data.size());
        return false;
    }

    // 디버그 출력
    if (LOG_LEVEL_DEBUG) {
        std::stringstream ss;
        ss << "TX: ";
        for (auto byte : data) {
            ss << std::hex << std::setw(2) << std::setfill('0') 
               << static_cast<int>(byte) << " ";
        }
        LOG_DEBUG("{}", ss.str());
    }

    return true;
}

bool SerialPort::send(const std::string& hexString) {
    std::vector<uint8_t> data;
    std::istringstream iss(hexString);
    std::string byte;

    while (std::getline(iss, byte, ',')) {
        try {
            data.push_back(static_cast<uint8_t>(std::stoi(byte, nullptr, 16)));
        } catch (const std::exception& e) {
            LOG_ERROR("Invalid hex string: {}", e.what());
            return false;
        }
    }

    return send(data);
}

void SerialPort::readThread() {
    std::vector<uint8_t> buffer(256);

    while (running_) {
        int bytesAvailable;
        if (ioctl(impl_->fd, FIONREAD, &bytesAvailable) < 0) {
            LOG_ERROR("ioctl failed: {}", strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (bytesAvailable > 0) {
            int toRead = std::min(bytesAvailable, static_cast<int>(buffer.size()));
            ssize_t bytesRead = ::read(impl_->fd, buffer.data(), toRead);

            if (bytesRead > 0) {
                std::vector<uint8_t> data(buffer.begin(), buffer.begin() + bytesRead);
                
                // 디버그 출력
                if (LOG_LEVEL_DEBUG) {
                    std::stringstream ss;
                    ss << "RX: ";
                    for (auto byte : data) {
                        ss << std::hex << std::setw(2) << std::setfill('0') 
                           << static_cast<int>(byte) << " ";
                    }
                    LOG_DEBUG("{}", ss.str());
                }

                if (dataCallback_) {
                    dataCallback_(data);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

uint8_t SerialPort::calculateChecksum(const std::vector<uint8_t>& data) {
    uint8_t checksum = 0;
    for (auto byte : data) {
        checksum ^= byte;
    }
    return checksum;
}