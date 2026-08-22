#include "trunkmonkey/Logger.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace trunkmonkey {
Logger::Logger(std::string path)
{
    if (!path.empty()) {
        setPath(path);
    }
}

void Logger::setPath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    file_.close();
    file_.clear();

    const std::filesystem::path filePath(path);
    if (filePath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(filePath.parent_path(), ec);
        if (ec && !std::filesystem::is_directory(filePath.parent_path())) {
            std::cerr << "WaffleHouse-Client warning: unable to create log directory "
                      << filePath.parent_path() << ": " << ec.message() << '\n';
            return;
        }
    }

    file_.open(path, std::ios::app);
    if (!file_) {
        std::cerr << "WaffleHouse-Client warning: unable to open log file " << path << '\n';
        return;
    }
#ifndef _WIN32
    (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

void Logger::setConsoleEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    consoleEnabled_ = enabled;
}

void Logger::info(const std::string& message) { write("INFO", message); }
void Logger::warn(const std::string& message) { write("WARN", message); }
void Logger::error(const std::string& message) { write("ERROR", message); }

void Logger::write(const char* level, const std::string& message)
{
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream line;
    line << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << level << "] " << message;

    std::lock_guard<std::mutex> lock(mutex_);
    if (consoleEnabled_) {
        std::cout << line.str() << std::endl;
    }
    if (file_) {
        file_ << line.str() << '\n';
        file_.flush();
    }
}
} // namespace trunkmonkey
