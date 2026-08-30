/*
 * Hardware-independent tests for the stateful decoder ordering model.
 */

#include "stateful_session.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Session = iris::StatefulSession;

[[noreturn]] void fail(const char* expression, const char* test, int line)
{
    std::fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__, line, test, expression);
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression) \
    do { \
        if (!(expression)) \
            fail(#expression, __func__, __LINE__); \
    } while (false)

void normal_one_in_one_out()
{
    Session session;
    CHECK(session.on_output_queued(1));
    CHECK(session.on_capture_completed(100));
    CHECK(session.on_output_dequeued(1));
    CHECK(session.state() == Session::State::Running);
    CHECK(session.queued_output_count() == 0);
    CHECK(session.completed_capture_count() == 1);
}

void b_frame_capture_reordering()
{
    Session session;
    CHECK(session.on_output_queued(1));
    CHECK(session.on_output_queued(2));
    CHECK(session.on_output_queued(3));
    CHECK(session.on_capture_completed(300));
    CHECK(session.on_capture_completed(100));
    CHECK(session.on_capture_completed(200));
    CHECK(session.on_output_dequeued(2));
    CHECK(session.on_output_dequeued(1));
    CHECK(session.on_output_dequeued(3));
    CHECK(session.completed_capture_count() == 3);
    CHECK(session.queued_output_count() == 0);
}

void capture_can_precede_output_recycle()
{
    Session session;
    CHECK(session.on_output_queued(7));
    CHECK(session.on_capture_completed(700));
    CHECK(session.queued_output_count() == 1);
    CHECK(session.on_output_dequeued(7));
    CHECK(session.queued_output_count() == 0);
}

void one_output_can_produce_multiple_captures()
{
    Session session;
    CHECK(session.on_output_queued(4));
    CHECK(session.on_capture_completed(401));
    CHECK(session.on_capture_completed(402));
    CHECK(session.on_capture_completed(403));
    CHECK(session.on_output_dequeued(4));
    CHECK(session.completed_capture_count() == 3);
}

void last_can_precede_trailing_output()
{
    Session session;
    CHECK(session.on_output_queued(1));
    CHECK(session.on_output_queued(2));
    CHECK(session.request_drain());
    CHECK(session.on_output_dequeued(1));
    CHECK(session.on_capture_last());
    CHECK(session.state() == Session::State::Draining);
    CHECK(!session.restart());
    CHECK(session.on_output_dequeued(2));
    CHECK(session.state() == Session::State::RestartPending);
    CHECK(session.restart());
    CHECK(session.state() == Session::State::Running);
}

void timestamp_zero_error_is_not_progress()
{
    Session session;
    CHECK(session.on_output_queued(1));
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::RecoveryRequested);
    CHECK(session.consecutive_timeout_count() == 1);
    CHECK(session.on_capture_error(0));
    CHECK(session.error_capture_count() == 1);
    CHECK(session.zero_timestamp_error_count() == 1);
    CHECK(session.completed_capture_count() == 0);
    CHECK(session.consecutive_timeout_count() == 1);
}

void resolution_change_during_drain()
{
    Session session;
    CHECK(session.on_output_queued(8));
    CHECK(session.request_drain());
    CHECK(session.request_resolution_change());
    CHECK(session.state() == Session::State::Reconfiguring);
    CHECK(session.on_capture_last());
    CHECK(session.state() == Session::State::Reconfiguring);
    CHECK(!session.restart());
    CHECK(session.on_output_dequeued(8));
    CHECK(session.state() == Session::State::RestartPending);
    CHECK(session.restart());
}

void resolution_change_after_completed_drain_needs_no_restart()
{
    Session session;
    CHECK(session.on_output_queued(11));
    CHECK(session.request_drain());
    CHECK(session.on_output_dequeued(11));
    CHECK(session.on_capture_last());
    CHECK(session.state() == Session::State::RestartPending);
    CHECK(session.request_resolution_change());
    CHECK(session.state() == Session::State::RestartPending);
}

void successful_decode_then_consecutive_timeouts_fail()
{
    Session session;
    CHECK(session.on_output_queued(9));
    CHECK(session.on_capture_completed(900));
    CHECK(session.on_sync_timeout(true) == Session::TimeoutAction::None);
    CHECK(session.consecutive_timeout_count() == 0);
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::RecoveryRequested);
    CHECK(session.timeout_recovery_used());
    CHECK(session.on_capture_last());
    CHECK(session.on_output_dequeued(9));
    CHECK(session.restart());
    CHECK(session.consecutive_timeout_count() == 1);
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::None);
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::Failed);
    CHECK(session.state() == Session::State::Failed);
    CHECK(!session.on_capture_completed(901));
}

void progress_resets_only_the_consecutive_budget()
{
    Session session;
    CHECK(session.on_output_queued(10));
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::RecoveryRequested);
    CHECK(session.on_capture_completed(1000));
    CHECK(session.consecutive_timeout_count() == 0);
    CHECK(session.timeout_recovery_used());
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::None);
    session.on_new_sequence();
    CHECK(session.consecutive_timeout_count() == 0);
    CHECK(!session.timeout_recovery_used());
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::RecoveryRequested);
}

void new_sequence_clears_old_drain_ownership()
{
    Session session;
    CHECK(session.on_output_queued(1));
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::RecoveryRequested);
    CHECK(session.state() == Session::State::Draining);
    CHECK(session.drain_output_count() == 1);
    session.on_new_sequence();
    CHECK(session.state() == Session::State::Running);
    CHECK(session.queued_output_count() == 0);
    CHECK(session.drain_output_count() == 0);
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::RecoveryRequested);
}

void startup_recovery_can_be_gated_without_spending_failure_budget()
{
    Session session;
    CHECK(session.on_output_queued(1));
    CHECK(session.on_sync_timeout(true, false) == Session::TimeoutAction::None);
    CHECK(session.consecutive_timeout_count() == 0);
    CHECK(!session.timeout_recovery_used());
    CHECK(session.on_sync_timeout(true, true) == Session::TimeoutAction::RecoveryRequested);
    CHECK(session.consecutive_timeout_count() == 0);
}

void new_sequence_recovers_a_failed_session()
{
    Session session;
    CHECK(session.on_output_queued(1));
    CHECK(session.on_capture_completed(100));
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::RecoveryRequested);
    CHECK(session.on_capture_last());
    CHECK(session.on_output_dequeued(1));
    CHECK(session.restart());
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::None);
    CHECK(session.on_sync_timeout(false) == Session::TimeoutAction::Failed);
    CHECK(session.failed());
    session.on_new_sequence();
    CHECK(session.state() == Session::State::Running);
    CHECK(!session.timeout_recovery_used());
    CHECK(session.consecutive_timeout_count() == 0);
    CHECK(session.on_output_queued(2));
}

void restart_requires_last_and_all_predrain_output()
{
    Session session;
    CHECK(!session.restart());
    CHECK(session.on_output_queued(1));
    CHECK(session.request_drain());
    CHECK(!session.on_output_queued(2));
    CHECK(!session.restart());
    CHECK(session.on_output_dequeued(1));
    CHECK(!session.restart());
    CHECK(session.on_capture_last());
    CHECK(session.state() == Session::State::RestartPending);
    CHECK(session.restart());
    CHECK(!session.on_capture_last());
}

} // namespace

int main()
{
    normal_one_in_one_out();
    b_frame_capture_reordering();
    capture_can_precede_output_recycle();
    one_output_can_produce_multiple_captures();
    last_can_precede_trailing_output();
    timestamp_zero_error_is_not_progress();
    resolution_change_during_drain();
    resolution_change_after_completed_drain_needs_no_restart();
    successful_decode_then_consecutive_timeouts_fail();
    progress_resets_only_the_consecutive_budget();
    new_sequence_clears_old_drain_ownership();
    startup_recovery_can_be_gated_without_spending_failure_budget();
    new_sequence_recovers_a_failed_session();
    restart_requires_last_and_all_predrain_output();
    std::puts("stateful session tests passed");
    return EXIT_SUCCESS;
}
