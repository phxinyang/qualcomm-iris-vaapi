/* Cached lookup for the driver's tracing switch. */

#pragma once

#include <cstdlib>

// The trace call sites sit on the per-frame submission and completion paths,
// and every one of them used to call getenv() before deciding whether to
// print. On a 30-second 720-frame clip that is tens of thousands of
// environment scans in the decode loop even when tracing is off.
//
// The switch cannot change during the lifetime of a process, so read it once.
// Any value enables tracing, matching the previous behaviour exactly.
inline bool trace_enabled()
{
    static const bool enabled = std::getenv("V4L2_VA_TRACE") != nullptr;
    return enabled;
}
