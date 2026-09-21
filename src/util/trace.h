// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace iris {

// Trace lines are an API consumed by test/iris-*-check.sh; see
// docs/architecture.md §8 before renaming a record.
class Trace {
public:
    explicit Trace(bool enabled)
        : enabled_(enabled)
    {
    }
    bool enabled() const { return enabled_; }
    // Every record ends with ` t=<ms>` since the first record of the
    // process: the checkers match substrings, so the suffix is additive.
    void operator()(const char* format, ...) const __attribute__((format(printf, 2, 3)))
    {
        if (!enabled_)
            return;
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
        std::fprintf(stderr, " t=%lld\n", static_cast<long long>(now_ms()));
    }

private:
    static int64_t now_ms()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    }
    bool enabled_;
};

} // namespace iris
