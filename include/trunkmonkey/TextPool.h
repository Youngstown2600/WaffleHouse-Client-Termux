#pragma once
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace trunkmonkey {
class TextPool {
public:
    void load(const std::string& path);
    void set(std::vector<std::string> v);
    std::vector<std::string> values() const;
    std::string next();
    bool empty() const;
    std::size_t size() const;
    void reset();
private:
    mutable std::mutex mutex_;
    std::vector<std::string> values_;
    std::size_t cursor_{0};
};
} // namespace trunkmonkey
