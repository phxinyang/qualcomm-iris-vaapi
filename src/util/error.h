// SPDX-License-Identifier: MIT

#pragma once

#include <stdexcept>
#include <string>

extern "C" {
#include <va/va.h>
}

namespace iris {

// The core throws this; the VA glue turns it into a status code. Nothing
// under src/iris returns VAStatus directly, so a session can be driven by a
// unit test without libva's error vocabulary leaking into the state machine.
struct Error : std::runtime_error {
    VAStatus status;
    Error(VAStatus s, const std::string& message)
        : std::runtime_error(message)
        , status(s)
    {
    }
};

inline void require(bool condition, const char* message, VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED)
{
    if (!condition)
        throw Error(status, message);
}

} // namespace iris
