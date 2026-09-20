// SPDX-License-Identifier: MIT
//
// Every environment variable the driver reads, read once. Nothing under
// src/ may call getenv() except options.cc; test/iris-env-doc-check.sh
// keeps README.md and this file in agreement by name.

#pragma once

#include <optional>
#include <string>

namespace iris {

enum class CopyMode { Gpu, Cpu };
enum class PublishOverride { Auto, Copy, Direct };

struct Options {
    // Decoder node override; when empty the node is found by QUERYCAP name.
    std::string video_path;
    bool trace = false;
    // Bounded vaSyncSurface wait. The first frame of a cold session may take
    // firmware bring-up time and gets cold_start_timeout_ms unless the user
    // pinned the bound explicitly.
    int sync_timeout_ms = 2000;
    bool sync_timeout_overridden = false;
    int cold_start_timeout_ms = 30000;
    CopyMode copy_mode = CopyMode::Gpu;
    PublishOverride publish = PublishOverride::Auto;
    // Directory to write each submitted access unit into; empty disables.
    std::string dump_dir;

    static Options from_environment();
};

} // namespace iris
