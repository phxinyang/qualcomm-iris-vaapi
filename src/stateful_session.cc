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

#include "stateful_session.h"

namespace iris {

bool StatefulSession::on_output_queued(OutputId output)
{
    if (state_ != State::Running || queued_outputs_.contains(output))
        return false;

    queued_outputs_.insert(output);
    output_since_restart_ = true;
    return true;
}

bool StatefulSession::on_output_dequeued(OutputId output)
{
    if (failed() || queued_outputs_.erase(output) == 0)
        return false;

    drain_outputs_.erase(output);
    update_restart_barrier();
    return true;
}

bool StatefulSession::on_capture_completed(Timestamp)
{
    if (failed())
        return false;

    completed_captures_++;
    note_decode_progress();
    return true;
}

bool StatefulSession::on_capture_error(Timestamp timestamp)
{
    if (failed())
        return false;

    error_captures_++;
    if (timestamp == 0)
        zero_timestamp_errors_++;
    return true;
}

bool StatefulSession::on_capture_last()
{
    if (failed() || !drain_active_ || last_seen_)
        return false;

    last_seen_ = true;
    update_restart_barrier();
    return true;
}

bool StatefulSession::request_drain()
{
    if (state_ == State::Draining || state_ == State::Reconfiguring)
        return true;
    if (state_ == State::Running && !output_since_restart_)
        return false;
    return begin_drain(State::Draining);
}

bool StatefulSession::request_resolution_change()
{
    if (failed())
        return false;
    // LAST plus all pre-drain OUTPUT already makes the old sequence safe to
    // tear down. A geometry rebuild does not need to START that sequence just
    // to STOP it again.
    if (state_ == State::RestartPending)
        return true;

    if (!drain_active_ && !begin_drain(State::Reconfiguring))
        return false;

    state_ = State::Reconfiguring;
    update_restart_barrier();
    return true;
}

bool StatefulSession::restart()
{
    // Stateful START is legal only after both sides of the STOP contract:
    // terminal CAPTURE LAST and every OUTPUT that existed at STOP dequeued.
    if (state_ != State::RestartPending || !last_seen_ || !drain_outputs_.empty())
        return false;

    state_ = State::Running;
    drain_active_ = false;
    last_seen_ = false;
    drain_outputs_.clear();
    output_since_restart_ = false;
    return true;
}

StatefulSession::TimeoutAction StatefulSession::on_sync_timeout(bool startup)
{
    return on_sync_timeout(startup, !startup);
}

StatefulSession::TimeoutAction StatefulSession::on_sync_timeout(bool startup, bool recovery_allowed)
{
    if (failed())
        return TimeoutAction::Failed;

    if (!startup) {
        consecutive_timeouts_++;
        if (consecutive_timeouts_ >= failure_timeout_count) {
            state_ = State::Failed;
            drain_active_ = false;
            drain_outputs_.clear();
            return TimeoutAction::Failed;
        }
    }

    if (recovery_allowed && !timeout_recovery_used_ && !drain_active_) {
        timeout_recovery_used_ = true;
        begin_drain(State::Draining);
        return TimeoutAction::RecoveryRequested;
    }

    return TimeoutAction::None;
}

void StatefulSession::on_new_sequence()
{
    // This event is a hard sequence boundary, not merely decode progress.
    // Callers must only announce it after old queues have been rebuilt or
    // otherwise proven abandoned, because all old ownership is discarded.
    state_ = State::Running;
    queued_outputs_.clear();
    drain_outputs_.clear();
    drain_active_ = false;
    last_seen_ = false;
    consecutive_timeouts_ = 0;
    timeout_recovery_used_ = false;
    output_since_restart_ = false;
}

bool StatefulSession::begin_drain(State drain_state)
{
    if (failed() || state_ == State::RestartPending || drain_active_)
        return false;

    drain_active_ = true;
    last_seen_ = false;
    drain_outputs_ = queued_outputs_;
    state_ = drain_state;
    return true;
}

void StatefulSession::note_decode_progress()
{
    consecutive_timeouts_ = 0;
}

void StatefulSession::update_restart_barrier()
{
    if (drain_active_ && last_seen_ && drain_outputs_.empty())
        state_ = State::RestartPending;
}

} // namespace iris
