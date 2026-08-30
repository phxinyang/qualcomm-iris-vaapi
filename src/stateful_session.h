/*
 * Copyright (C) 2026
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>

namespace iris {

// Hardware-independent model of the ordering contract shared by all
// stateful codecs. It deliberately owns no V4L2 buffers: callers feed it
// dequeue/control events and perform the returned action against the device.
class StatefulSession {
public:
    using OutputId = std::uint64_t;
    using Timestamp = std::uint64_t;

    enum class State {
        Running,
        Draining,
        RestartPending,
        Reconfiguring,
        Failed,
    };

    enum class TimeoutAction {
        None,
        RecoveryRequested,
        Failed,
    };

    static constexpr unsigned failure_timeout_count = 3;

    State state() const { return state_; }
    bool failed() const { return state_ == State::Failed; }

    // OUTPUT queue ownership. New OUTPUT is rejected after a drain starts;
    // this makes the drain cutoff exact and prevents a post-STOP AU from
    // being mistaken for trailing work from the old sequence.
    bool on_output_queued(OutputId output);
    bool on_output_dequeued(OutputId output);

    // CAPTURE data and control completions. Error buffers, including Iris'
    // timestamp-zero markers, do not count as successful decode progress.
    bool on_capture_completed(Timestamp timestamp);
    bool on_capture_error(Timestamp timestamp);
    bool on_capture_last();

    // A resolution change enters the same STOP/LAST/OUTPUT barrier as an
    // ordinary drain, while exposing Reconfiguring to its caller.
    bool request_drain();
    bool request_resolution_change();
    bool restart();

    // Startup waits are deliberately outside the failure budget. Once the
    // caller identifies an ordinary (non-startup) timeout, the model permits
    // one STOP-drain recovery and fails on the third consecutive timeout.
    TimeoutAction on_sync_timeout(bool startup);
    TimeoutAction on_sync_timeout(bool startup, bool recovery_allowed);
    void on_new_sequence();

    std::size_t queued_output_count() const { return queued_outputs_.size(); }
    std::size_t drain_output_count() const { return drain_outputs_.size(); }
    std::size_t completed_capture_count() const { return completed_captures_; }
    std::size_t error_capture_count() const { return error_captures_; }
    std::size_t zero_timestamp_error_count() const { return zero_timestamp_errors_; }
    unsigned consecutive_timeout_count() const { return consecutive_timeouts_; }
    bool timeout_recovery_used() const { return timeout_recovery_used_; }
    bool last_seen() const { return last_seen_; }
    bool output_since_restart() const { return output_since_restart_; }

private:
    bool begin_drain(State drain_state);
    void note_decode_progress();
    void update_restart_barrier();

    State state_ = State::Running;
    std::set<OutputId> queued_outputs_;
    std::set<OutputId> drain_outputs_;
    bool drain_active_ = false;
    bool last_seen_ = false;
    bool timeout_recovery_used_ = false;
    bool output_since_restart_ = false;
    unsigned consecutive_timeouts_ = 0;
    std::size_t completed_captures_ = 0;
    std::size_t error_captures_ = 0;
    std::size_t zero_timestamp_errors_ = 0;
};

} // namespace iris
