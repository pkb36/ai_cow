#include "core/Application.hpp"
#include "core/Logger.hpp"
#include <iostream>
#include <csignal>
#include <execinfo.h>
#include <cxxabi.h>

namespace {
    // 시그널 핸들러
    void signalHandler(int signal) {
        LOG_INFO("Received signal: {}", signal);
        
        if (signal == SIGSEGV) {
            // 스택 트레이스 출력
            void* array[20];
            size_t size = backtrace(array, 20);
            
            LOG_CRITICAL("Signal {} (SIGSEGV) received. Stack trace:", signal);
            
            char** symbols = backtrace_symbols(array, size);
            if (symbols) {
                for (size_t i = 0; i < size; i++) {
                    // C++ 심볼 demangle
                    std::string symbol(symbols[i]);
                    size_t start = symbol.find('(');
                    size_t end = symbol.find('+');
                    
                    if (start != std::string::npos && end != std::string::npos) {
                        std::string mangledName = symbol.substr(start + 1, end - start - 1);
                        
                        int status;
                        char* demangled = abi::__cxa_demangle(
                            mangledName.c_str(), nullptr, nullptr, &status);
                        
                        if (status == 0 && demangled) {
                            symbol.replace(start + 1, end - start - 1, demangled);
                            free(demangled);
                        }
                    }
                    
                    LOG_CRITICAL("  {}: {}", i, symbol);
                }
                free(symbols);
            }
        }
        
        Application::getInstance().shutdown();
        
        if (signal == SIGSEGV || signal == SIGABRT) {
            std::abort();
        }
    }

    void setupSignalHandlers() {
        // 시그널 핸들러 설정
        std::signal(SIGINT, signalHandler);   // Ctrl+C
        std::signal(SIGTERM, signalHandler);  // Termination
        std::signal(SIGSEGV, signalHandler);  // Segmentation fault
        std::signal(SIGABRT, signalHandler);  // Abort
        std::signal(SIGPIPE, SIG_IGN);        // Broken pipe (ignore)
        
        // 코어 덤프 활성화
        struct rlimit core_limit;
        core_limit.rlim_cur = RLIM_INFINITY;
        core_limit.rlim_max = RLIM_INFINITY;
        setrlimit(RLIMIT_CORE, &core_limit);
    }

    void printBanner() {
        std::cout << R"(
╔══════════════════════════════════════════════════════════════╗
║             WebRTC Camera System v2.0 (C++17)                ║
║                   Powered by GStreamer                       ║
╚══════════════════════════════════════════════════════════════╝
)" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    try {
        // 배너 출력
        printBanner();
        
        // 로거 초기화
        Logger::getInstance().setLogFile("webrtc_camera.log");
        Logger::getInstance().setLogLevel(LogLevel::DEBUG);
        
        LOG_INFO("========================================");
        LOG_INFO("WebRTC Camera System Starting...");
       LOG_INFO("Version: 2.0.0");
       LOG_INFO("Build: {} {}", __DATE__, __TIME__);
       LOG_INFO("========================================");

       // 시그널 핸들러 설정
       setupSignalHandlers();

       // 애플리케이션 초기화 및 실행
       auto& app = Application::getInstance();
       
       if (!app.initialize(argc, argv)) {
           LOG_ERROR("Failed to initialize application");
           return EXIT_FAILURE;
       }

       // 메인 루프 실행
       app.run();
       
       LOG_INFO("Application terminated normally");
       return EXIT_SUCCESS;
       
   } catch (const std::exception& e) {
       LOG_CRITICAL("Unhandled exception: {}", e.what());
       std::cerr << "Fatal error: " << e.what() << std::endl;
       return EXIT_FAILURE;
   } catch (...) {
       LOG_CRITICAL("Unknown exception caught");
       std::cerr << "Fatal error: Unknown exception" << std::endl;
       return EXIT_FAILURE;
   }
}