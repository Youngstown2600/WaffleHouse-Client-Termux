#pragma once
#include <fstream>
#include <mutex>
#include <string>
namespace trunkmonkey {
class Logger {
public:
    explicit Logger(std::string path={});
    void setPath(const std::string& path);
    void setConsoleEnabled(bool enabled);
    void info(const std::string& m);
    void warn(const std::string& m);
    void error(const std::string& m);
private:
    void write(const char* level,const std::string& m);
    std::mutex mutex_;
    std::ofstream file_;
    bool consoleEnabled_{true};
};
}
