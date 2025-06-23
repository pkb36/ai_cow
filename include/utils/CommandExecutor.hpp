#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>
#include <regex>  // 추가
#include <unordered_map>

class CommandExecutor {
public:
    struct CommandResult {
        int exitCode;
        std::string output;
        std::string error;
        std::chrono::milliseconds executionTime;
    };

    struct CommandConfig {
        std::chrono::seconds timeout{30};  // {} 초기화 구문으로 변경
        size_t maxOutputSize = 8192;
        bool captureStderr = true;
        std::optional<std::string> workingDirectory;
        std::unordered_map<std::string, std::string> environment;
    };

    // 싱글톤
    static CommandExecutor& getInstance() {
        static CommandExecutor instance;
        return instance;
    }

    // 허용된 명령 등록
    void registerAllowedCommand(const std::string& name, const std::string& command);
    void registerAllowedPattern(const std::string& pattern);
    
    // 명령 실행
    std::optional<CommandResult> execute(const std::string& commandName, 
                                       const std::vector<std::string>& args = {},
                                       const CommandConfig& config = CommandConfig{});
    
    // 비동기 실행
    using CompletionCallback = std::function<void(const CommandResult&)>;
    void executeAsync(const std::string& commandName,
                     const std::vector<std::string>& args,
                     CompletionCallback callback,
                     const CommandConfig& config = CommandConfig{});

private:
    CommandExecutor();
    
    bool isCommandAllowed(const std::string& command) const;
    std::string sanitizeArgument(const std::string& arg) const;
    
    std::unordered_map<std::string, std::string> allowedCommands_;
    std::vector<std::regex> allowedPatterns_;
    mutable std::mutex mutex_;
};