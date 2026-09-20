// SPDX-License-Identifier: MIT

#pragma once

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
    void operator()(const char* format, ...) const __attribute__((format(printf, 2, 3)))
    {
        if (!enabled_)
            return;
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
        std::fputc('\n', stderr);
    }

private:
    bool enabled_;
};

} // namespace iris
