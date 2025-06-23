#pragma once

#include <memory>
#include <mutex>

template<typename T>
class Singleton {
protected:
    Singleton() = default;
    virtual ~Singleton() = default;

public:
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton(Singleton&&) = delete;
    Singleton& operator=(Singleton&&) = delete;

    static T& getInstance() {
        std::call_once(initFlag_, []() {
            instance_.reset(new T());
        });
        return *instance_;
    }

private:
    static std::unique_ptr<T> instance_;
    static std::once_flag initFlag_;
};

template<typename T>
std::unique_ptr<T> Singleton<T>::instance_;

template<typename T>
std::once_flag Singleton<T>::initFlag_;