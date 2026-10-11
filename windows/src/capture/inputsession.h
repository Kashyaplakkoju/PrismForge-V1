#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace PrismForge::Input {
enum class Kind { None, Heading, Mobile, System };
struct Choice {
    Kind kind = Kind::None;
    std::string key;
    std::string label;
};

// One source-independent processing/output owner. Switching waits for the old
// synchronous delivery, then rejects any old source's delayed frames/statistics.
// Advance is an external owner operation, never called from inside Deliver.
class SessionGate {
    std::mutex processing;
    std::atomic<uint64_t> generation{1};
public:
    uint64_t Advance() {
        std::lock_guard<std::mutex> lock(processing);
        return ++generation;
    }
    uint64_t Current() const { return generation.load(); }
    bool IsCurrent(uint64_t token) const { return token == generation.load(); }
    template<class Callback> bool Deliver(uint64_t token, Callback callback) {
        std::lock_guard<std::mutex> lock(processing);
        if (!IsCurrent(token)) return false;
        callback();
        return true;
    }
};

inline std::string MobileKey(const std::string& name, const std::string& url) {
    return "mobile:" + url + "\n" + name;
}
inline int FindChoice(const std::vector<Choice>& choices, const std::string& key) {
    if (key.empty()) return 0;
    for (size_t i=0; i<choices.size(); ++i)
        if (choices[i].kind != Kind::Heading && choices[i].key == key) return static_cast<int>(i);
    return 0;
}
}
