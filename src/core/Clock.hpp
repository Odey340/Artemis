#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

namespace artemis {

inline std::int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::int64_t nowMs() { return nowUs() / 1000; }

inline std::int64_t monoUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// UTC day index (days since epoch) for daily loss accounting.
inline std::int64_t utcDay(std::int64_t tsUs) { return tsUs / 86'400'000'000LL; }

// Parse an RFC3339 timestamp such as "2026-10-11T18:00:00Z" (fraction and
// numeric offsets supported). Returns 0 on failure.
std::int64_t parseRfc3339Us(const std::string& s);

}  // namespace artemis
