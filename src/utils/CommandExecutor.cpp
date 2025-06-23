#include "utils/CommandExecutor.hpp"
#include "core/Logger.hpp"
#include <regex>
#include <array>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>     // pipe, fork, dup2, read, close, usleep, _exit, chdir
#include <sys/types.h>  // pid_t
#include <thread>       // std::thread

CommandExecutor::CommandExecutor() {
    // 기본 허용 패턴 추가
    allowedPatterns_.push_back(std::regex("^echo\\s+.*"));
    allowedPatterns_.push_back(std::regex("^ls\\s+-[la]*\\s+.*"));
    allowedPatterns_.push_back(std::regex("^cat\\s+/proc/.*"));
}

void CommandExecutor::registerAllowedCommand(const std::string& name, const std::string& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    allowedCommands_[name] = command;
    LOG_DEBUG("Registered command: {} -> {}", name, command);
}

void CommandExecutor::registerAllowedPattern(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        allowedPatterns_.push_back(std::regex(pattern));
        LOG_DEBUG("Registered pattern: {}", pattern);
    } catch (const std::regex_error& e) {
        LOG_ERROR("Invalid regex pattern: {} - {}", pattern, e.what());
    }
}

std::optional<CommandResult> 
CommandExecutor::execute(const std::string& commandName, 
                        const std::vector<std::string>& args,
                        const CommandConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 명령어 찾기
    std::string command;
    auto it = allowedCommands_.find(commandName);
    if (it != allowedCommands_.end()) {
        command = it->second;
    } else {
        // 직접 명령어로 시도
        command = commandName;
        if (!isCommandAllowed(command)) {
            LOG_WARNING("Command not allowed: {}", command);
            return std::nullopt;
        }
    }
    
    // 인자 추가
    for (const auto& arg : args) {
        command += " " + sanitizeArgument(arg);
    }
    
    LOG_INFO("Executing command: {}", command);
    
    auto startTime = std::chrono::steady_clock::now();
    
    // 파이프 생성
    int stdoutPipe[2], stderrPipe[2];
    if (pipe(stdoutPipe) == -1 || pipe(stderrPipe) == -1) {
        LOG_ERROR("Failed to create pipes");
        return std::nullopt;
    }
    
    // 프로세스 생성
    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR("Failed to fork process");
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        close(stderrPipe[0]); close(stderrPipe[1]);
        return std::nullopt;
    }
    
    if (pid == 0) {
        // 자식 프로세스
        close(stdoutPipe[0]);
        close(stderrPipe[0]);
        
        // stdout/stderr 리다이렉션
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        
        close(stdoutPipe[1]);
        close(stderrPipe[1]);
        
        // 작업 디렉토리 변경
        if (config.workingDirectory) {
            if (chdir(config.workingDirectory->c_str()) != 0) {
                _exit(127);
            }
        }
        
        // 환경 변수 설정
        for (const auto& [key, value] : config.environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }
        
        // 명령 실행
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }
    
    // 부모 프로세스
    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    
    // Non-blocking 모드 설정
    fcntl(stdoutPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderrPipe[0], F_SETFL, O_NONBLOCK);
    
    CommandResult result;
    result.output.reserve(config.maxOutputSize);
    result.error.reserve(config.maxOutputSize);
    
    // 타임아웃 처리
    auto deadline = startTime + config.timeout;
    bool timedOut = false;
    
    while (true) {
        // 프로세스 상태 확인
        int status;
        pid_t ret = waitpid(pid, &status, WNOHANG);
        
        if (ret == pid) {
            // 프로세스 종료
            if (WIFEXITED(status)) {
                result.exitCode = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exitCode = -WTERMSIG(status);
            }
            break;
        } else if (ret == -1) {
            LOG_ERROR("waitpid failed");
            result.exitCode = -1;
            break;
        }
        
        // 타임아웃 확인
        if (std::chrono::steady_clock::now() > deadline) {
            LOG_WARNING("Command timed out after {}s", config.timeout.count());
            kill(pid, SIGTERM);
            usleep(100000); // 100ms
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timedOut = true;
            result.exitCode = -1;
            break;
        }
        
        // 출력 읽기
        char buffer[4096];
        ssize_t n;
        
        // stdout
        while ((n = read(stdoutPipe[0], buffer, sizeof(buffer))) > 0) {
            if (result.output.size() + n <= config.maxOutputSize) {
                result.output.append(buffer, n);
            }
        }
        
        // stderr
        if (config.captureStderr) {
            while ((n = read(stderrPipe[0], buffer, sizeof(buffer))) > 0) {
                if (result.error.size() + n <= config.maxOutputSize) {
                    result.error.append(buffer, n);
                }
            }
        }
        
        usleep(10000); // 10ms
    }
    
    // 남은 출력 읽기
    char buffer[4096];
    ssize_t n;
    while ((n = read(stdoutPipe[0], buffer, sizeof(buffer))) > 0) {
        if (result.output.size() + n <= config.maxOutputSize) {
            result.output.append(buffer, n);
        }
    }
    
    if (config.captureStderr) {
        while ((n = read(stderrPipe[0], buffer, sizeof(buffer))) > 0) {
            if (result.error.size() + n <= config.maxOutputSize) {
                result.error.append(buffer, n);
            }
        }
    }
    
    close(stdoutPipe[0]);
    close(stderrPipe[0]);
    
    auto endTime = std::chrono::steady_clock::now();
    result.executionTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    LOG_INFO("Command completed: exit={}, time={}ms, output_size={}, error_size={}", 
             result.exitCode, result.executionTime.count(), 
             result.output.size(), result.error.size());
    
    return result;
}

void CommandExecutor::executeAsync(const std::string& commandName,
                                  const std::vector<std::string>& args,
                                  CompletionCallback callback,
                                  const CommandConfig& config) {
    std::thread([this, commandName, args, callback, config]() {
        auto result = execute(commandName, args, config);
        if (result && callback) {
            callback(*result);
        }
    }).detach();
}

bool CommandExecutor::isCommandAllowed(const std::string& command) const {
    // 허용된 명령어 확인
    for (const auto& [name, cmd] : allowedCommands_) {
        if (cmd == command) {
            return true;
        }
    }
    
    // 패턴 매칭
    for (const auto& pattern : allowedPatterns_) {
        if (std::regex_match(command, pattern)) {
            return true;
        }
    }
    
    return false;
}

std::string CommandExecutor::sanitizeArgument(const std::string& arg) const {
    std::string result;
    result.reserve(arg.size() * 2);
    
    // 셸 특수 문자 이스케이프
    for (char c : arg) {
        switch (c) {
            case '\'':
            case '"':
            case '\\':
            case '$':
            case '`':
            case '!':
            case '*':
            case '?':
            case '[':
            case ']':
            case '(':
            case ')':
            case '{':
            case '}':
            case '|':
            case '&':
            case ';':
            case '<':
            case '>':
            case '\n':
            case '\r':
            case '\t':
                result += '\\';
                break;
        }
        result += c;
    }
    
    return result;
}