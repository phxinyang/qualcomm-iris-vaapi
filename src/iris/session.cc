// SPDX-License-Identifier: MIT

#include "session.h"

#include "../util/error.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <linux/v4l2-controls.h>
#include <poll.h>
}

namespace iris {

namespace {

constexpr unsigned kMinCapturePool = 32;
constexpr unsigned kMaxCapturePool = 64;
constexpr int kLastWaitMs = 2000;
constexpr int kOutputDrainMs = 500;
constexpr int kSourceChangeWaitMs = 5000;
// Bound on the client buffer's implicit fence before it is handed to the
// firmware: the compositor may still be sampling the previous picture.
constexpr int kImportFenceMs = 2000;

// Compressed AU capacity when the caller has no better estimate: Chromium
// and GStreamer both size the OUTPUT plane from the coded area with a floor.
unsigned default_au_capacity(unsigned width, unsigned height)
{
    const uint64_t area = static_cast<uint64_t>(width) * height;
    // 4K IDR frames at high bit rates exceed 1 MiB; 8K stays under 8 MiB.
    if (area > 1920u * 1088u)
        return static_cast<unsigned>(std::min<uint64_t>(8u << 20, std::max<uint64_t>(2u << 20, area)));
    return 2u << 20;
}

} // namespace

Session::Session(std::unique_ptr<Device> device, const SessionConfig& config, const Options& options,
    CopyEngine* copier, const Trace& trace)
    : device_(std::move(device))
    , config_(config)
    , options_(options)
    , copier_(copier)
    , trace_(trace)
    , pool_(*device_)
{
    require(config_.width && config_.height && config_.codec_pixelformat, "session geometry",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    visible_ = Selection { 0, 0, config_.width, config_.height };
    if (config_.au_capacity == 0)
        config_.au_capacity = default_au_capacity(config_.width, config_.height);
    if (!options_.dump_dir.empty()) {
        std::string path = options_.dump_dir + "/iris-session-" + std::to_string(reinterpret_cast<uintptr_t>(this))
            + ".bin";
        dump_ = std::fopen(path.c_str(), "wb");
    }
    try {
        configure_output();
    } catch (...) {
        if (dump_)
            std::fclose(static_cast<FILE*>(dump_));
        throw;
    }
}

Session::~Session()
{
    // Callers are expected to finish() first; a destructor cannot wait on
    // hardware. Stop streaming so the kernel returns every buffer, then let
    // the pool close fds. Frames still held by targets outlive the session
    // (they hold their own fd) but must not be requeued: they never are,
    // because the pool is gone.
    try {
        if (state_ != SessionState::Failed) {
            device_->stream(Queue::Capture, false);
            device_->stream(Queue::Output, false);
        }
    } catch (...) {
    }
    for (auto& [token, target] : pending_) {
        target->pending = false;
        target->error = true;
    }
    pending_.clear();
    if (dump_)
        std::fclose(static_cast<FILE*>(dump_));
}

int Session::sync_timeout_ms() const
{
    if (cold_ && !options_.sync_timeout_overridden)
        return options_.cold_start_timeout_ms;
    return options_.sync_timeout_ms;
}

bool Session::expired(std::chrono::steady_clock::time_point deadline) const
{
    return std::chrono::steady_clock::now() >= deadline;
}

bool Session::supports_import() const
{
    if (mode_ != SessionMode::DecodeOrder || import_rejected_)
        return false;
    return pool_.capture_count() == 0 || pool_.capture_memory() == CaptureMemory::Import;
}

bool Session::import_compatible(const Layout& client, const Layout& capture)
{
    // The firmware writes with the CAPTURE pitch and puts chroma after
    // storage_height luma rows; the client's exported offsets are fixed, so
    // both must agree exactly and the buffer must hold the whole plane set.
    return client.fourcc == capture.fourcc && client.stride == capture.stride
        && client.chroma_offset() == capture.chroma_offset() && client.size >= capture.size;
}

// ---------------------------------------------------------------------------
// Bring-up

void Session::configure_output()
{
    device_->subscribe(V4L2_EVENT_SOURCE_CHANGE);
    FormatInfo request;
    request.pixelformat = config_.codec_pixelformat;
    request.width = config_.width;
    request.height = config_.height;
    request.sizeimage = config_.au_capacity;
    const FormatInfo granted = device_->set_format(Queue::Output, request);
    require(granted.pixelformat == config_.codec_pixelformat, "decoder rejected the codec",
        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    config_.au_capacity = granted.sizeimage ? granted.sizeimage : config_.au_capacity;

    // Decode-order output if the kernel offers it (docs/architecture.md §2).
    // Both controls must be set before OUTPUT streams; the driver rejects
    // changes afterwards.
    if (device_->set_control(V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY, 0)
        && device_->set_control(V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, 1))
        mode_ = SessionMode::DecodeOrder;
    else
        mode_ = SessionMode::DisplayOrder;

    pool_.allocate_output(config_.output_slots);
    device_->stream(Queue::Output, true);
    state_ = SessionState::Configured;
    trace_("session open node=%s mode=%s codec=%c%c%c%c %ux%u au=%u output=%u", device_->path().c_str(),
        mode_ == SessionMode::DecodeOrder ? "decode-order" : "display-order", config_.codec_pixelformat & 0xff,
        (config_.codec_pixelformat >> 8) & 0xff, (config_.codec_pixelformat >> 16) & 0xff,
        (config_.codec_pixelformat >> 24) & 0xff, config_.width, config_.height, config_.au_capacity,
        pool_.output_count());
}

void Session::configure_capture()
{
    // Only meaningful after SOURCE_CHANGE: the decoder has parsed the
    // stream and knows the coded geometry.
    const uint32_t pixelformat = config_.capture_fourcc == VA_FOURCC_P010 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
    FormatInfo current = device_->get_format(Queue::Capture);
    current.pixelformat = pixelformat;
    const FormatInfo granted = device_->set_format(Queue::Capture, current);
    if (granted.pixelformat != pixelformat || granted.num_planes != 1) {
        trace_("capture format rejected wanted=%c%c%c%c granted=%c%c%c%c planes=%u %ux%u", pixelformat & 0xff,
            (pixelformat >> 8) & 0xff, (pixelformat >> 16) & 0xff, (pixelformat >> 24) & 0xff,
            granted.pixelformat & 0xff, (granted.pixelformat >> 8) & 0xff, (granted.pixelformat >> 16) & 0xff,
            (granted.pixelformat >> 24) & 0xff, granted.num_planes, granted.width, granted.height);
        throw Error(VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "decoder CAPTURE layout unsupported");
    }

    // The visible rectangle. The kernel reports the firmware's crop through
    // every selection target; when it is empty (this Iris driver returns a
    // zero rectangle before the first frame) fall back to what the client
    // declared, bounded by the coded size. The client's geometry is the SPS
    // crop as far as it knows, so it is the right default.
    if (auto compose = device_->get_compose(Queue::Capture); compose && compose->width && compose->height
        && compose->width <= granted.width && compose->height <= granted.height)
        visible_ = *compose;
    else
        visible_ = Selection { 0, 0, std::min(config_.width, granted.width), std::min(config_.height, granted.height) };
    config_.width = visible_.width;
    config_.height = visible_.height;

    Layout layout;
    layout.fourcc = config_.capture_fourcc;
    layout.width = granted.width;
    layout.height = granted.height;
    layout.stride = granted.bytesperline;
    // iris_buffer.c: luma scanlines aligned to 32, chroma directly after.
    layout.storage_height = (granted.height + 31) & ~31u;
    layout.data_offset = 0;
    layout.size = granted.sizeimage;

    // Import memory when the first picture asks for it and the contract
    // holds (docs/architecture.md §3.3): decode-order output, and the
    // client's buffer laid out exactly as the firmware will write it.
    CaptureMemory memory = CaptureMemory::Mmap;
    if (!pending_.empty() && pending_.begin()->second->publish == Publish::Import) {
        const Target& first = *pending_.begin()->second;
        const char* why = nullptr;
        if (mode_ != SessionMode::DecodeOrder)
            why = "display-order output";
        else if (!first.destination)
            why = "no client buffer";
        else if (!import_compatible(first.destination->layout(), layout))
            why = "client layout differs from the CAPTURE layout";
        if (why) {
            import_rejected_ = true;
            trace_("import rejected: %s (client stride=%u chroma=%u size=%u; capture stride=%u chroma=%u size=%u)", why,
                first.destination ? first.destination->layout().stride : 0,
                first.destination ? first.destination->layout().chroma_offset() : 0,
                first.destination ? first.destination->size() : 0, layout.stride, layout.chroma_offset(),
                layout.size);
        } else {
            memory = CaptureMemory::Import;
        }
    }

    // Pool sizing: the client's surface count plus in-flight headroom, never
    // below the firmware's minimum, never below 32 (the old vb2 ceiling that
    // every hardware pass ran with), capped by the kernel's maximum.
    const auto minimum = device_->get_control(V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);
    unsigned wanted = std::clamp(config_.surface_count + 4, kMinCapturePool, kMaxCapturePool);
    if (minimum && *minimum > 0)
        wanted = std::max(wanted, static_cast<unsigned>(*minimum));
    pool_.allocate_capture(wanted, layout, memory);
    trace_("capture configure %ux%u stride=%u size=%u visible=%ux%u pool=%u memory=%s firmware-min=%d",
        granted.width, granted.height, granted.bytesperline, granted.sizeimage, visible_.width, visible_.height,
        pool_.capture_count(), memory == CaptureMemory::Import ? "import" : "mmap", minimum ? *minimum : -1);
    enter_streaming();
}

void Session::enter_streaming()
{
    if (pool_.capture_memory() == CaptureMemory::Import) {
        // The pictures already submitted (the first AU, or the one that
        // announced a resolution change) get their buffers now, in token
        // order, so the firmware fills them in the order it decodes.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kImportFenceMs);
        for (auto it = pending_.begin(); it != pending_.end();) {
            Target& target = *it->second;
            const uint64_t token = it->first;
            ++it;
            try {
                queue_import(target, token, deadline);
            } catch (const Error& error) {
                trace_("import token=%llu failed: %s", static_cast<unsigned long long>(token), error.what());
                pending_.erase(token);
                target.pending = false;
                target.error = true;
                ++stats_.errors;
            }
        }
    } else {
        pool_.requeue_capture();
    }
    device_->stream(Queue::Capture, true);
    source_change_ = false;
    saw_last_ = false;
    state_ = SessionState::Streaming;
}

void Session::queue_import(Target& target, uint64_t token, std::chrono::steady_clock::time_point deadline)
{
    require(target.publish != Publish::Direct, "an Import session needs an exported surface for every picture",
        VA_STATUS_ERROR_INVALID_SURFACE);
    require(target.destination != nullptr, "Import target without a client buffer", VA_STATUS_ERROR_INVALID_SURFACE);
    require(import_compatible(target.destination->layout(), pool_.capture_layout()),
        "client buffer layout does not match the CAPTURE layout", VA_STATUS_ERROR_INVALID_SURFACE);
    // The compositor may still be sampling the previous picture from this
    // buffer; the firmware must not write under it.
    wait_dma_buf(target.destination->fd(), true, kImportFenceMs);
    std::optional<unsigned> slot;
    while (!(slot = pool_.queue_capture_import(target.destination->fd(), target.destination->size(), token))) {
        require(!expired(deadline), "every CAPTURE slot is in flight", VA_STATUS_ERROR_TIMEDOUT);
        pump(20);
    }
    trace_("import token=%llu index=%u fd=%d", static_cast<unsigned long long>(token), *slot,
        target.destination->fd());
}

// ---------------------------------------------------------------------------
// Submission

void Session::submit(const uint8_t* data, size_t size, Target& target)
{
    std::lock_guard<std::mutex> lock(mutex_);
    require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
    require(state_ != SessionState::Finished, "decoder session finished", VA_STATUS_ERROR_OPERATION_FAILED);
    require(size > 0 && size <= config_.au_capacity, "access unit does not fit the OUTPUT slot",
        VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
    // A target that still owns a token from an earlier picture is being
    // reused without release(); treat it as released.
    if (target.pending)
        release_locked(target);

    pump(0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync_timeout_ms());
    // A resolution change is waiting for the client to release frames it
    // still holds from the old geometry. Nothing can be accepted until the
    // CAPTURE pool is rebuilt; the client must release (or sync and drop)
    // before it submits more.
    while (state_ == SessionState::Reconfiguring) {
        require(!expired(deadline), "resolution change blocked by held CAPTURE frames; release them first",
            VA_STATUS_ERROR_SURFACE_BUSY);
        pump(20);
    }
    // In display-order mode the firmware may hold every OUTPUT slot behind a
    // reorder window; the drain in wait() is what frees them. Here we can
    // only wait for the normal return of a slot.
    OutputSlot* slot = nullptr;
    while (!(slot = pool_.free_output())) {
        require(state_ == SessionState::Streaming || state_ == SessionState::Configured,
            "decoder not accepting input while draining", VA_STATUS_ERROR_HW_BUSY);
        require(!expired(deadline), "OUTPUT queue full", VA_STATUS_ERROR_TIMEDOUT);
        pump(20);
    }

    if (state_ == SessionState::Configured && target.fourcc != 0)
        config_.capture_fourcc = target.fourcc;
    std::memcpy(slot->mapping, data, size);
    if (dump_) {
        std::fwrite(data, 1, size, static_cast<FILE*>(dump_));
        std::fflush(static_cast<FILE*>(dump_));
    }
    const uint64_t token = next_token_++;
    if (pool_.capture_memory() == CaptureMemory::Import) {
        // The client's buffer goes in before the bitstream so it is the next
        // one the firmware picks up when it decodes this picture. A buffer
        // of another geometry cannot be written by the current CAPTURE
        // format: it is the first picture of a resolution change and gets
        // queued when the pool is rebuilt (enter_streaming). If no change
        // follows, the completion lands elsewhere and is reported misplaced.
        if (target.destination && import_compatible(target.destination->layout(), pool_.capture_layout()))
            queue_import(target, token, deadline);
        else
            trace_("import token=%llu deferred: geometry differs from the CAPTURE format",
                static_cast<unsigned long long>(token));
    }
    device_->queue_buffer(Queue::Output, slot->index, static_cast<unsigned>(size), token, 0);
    slot->queued = true;
    slot->token = token;
    target.token = token;
    target.pending = true;
    target.completed = false;
    target.error = false;
    target.frame.reset();
    pending_[token] = &target;
    ++stats_.submitted;
    trace_("submit token=%llu bytes=%zu output=%u", static_cast<unsigned long long>(token), size, slot->index);

    if (state_ == SessionState::Configured) {
        // The first AU raises SOURCE_CHANGE; CAPTURE cannot be configured
        // before that. Bring-up may take firmware load time.
        const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSourceChangeWaitMs);
        while (state_ == SessionState::Configured) {
            require(!expired(start_deadline), "no SOURCE_CHANGE after the first access unit",
                VA_STATUS_ERROR_TIMEDOUT);
            pump(20);
        }
    }
}

// ---------------------------------------------------------------------------
// Servicing

void Session::service()
{
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || state_ == SessionState::Failed)
        return;
    try {
        pump(0);
    } catch (const Error&) {
        // fail() already recorded it; the next submit/wait reports.
    }
}

void Session::pump(int timeout_ms)
{
    if (state_ == SessionState::Failed)
        return;
    pool_.requeue_capture();
    if (timeout_ms > 0) {
        // Some Iris kernels do not wake POLLIN for a completed CAPTURE
        // buffer while OUTPUT has writable space; the wait is a bounded
        // sleep and the non-blocking DQBUF below is authoritative.
        device_->poll(POLLIN | POLLPRI | POLLOUT, std::min(timeout_ms, 5));
    }
    try {
        handle_events();
        if (source_change_ && state_ == SessionState::Configured)
            configure_capture();
        handle_output_returns();
        if (state_ == SessionState::Streaming || state_ == SessionState::Draining)
            handle_capture_returns();
        if (source_change_ && state_ == SessionState::Streaming)
            reconfigure();
        else if (state_ == SessionState::Reconfiguring)
            continue_reconfigure();
        if (state_ != SessionState::Reconfiguring)
            pool_.requeue_capture();
    } catch (const Error& error) {
        fail(error.what());
        throw;
    }
}

void Session::handle_events()
{
    for (;;) {
        const Event event = device_->dequeue_event();
        if (event == Event::None)
            return;
        if (event == Event::SourceChange) {
            source_change_ = true;
            trace_("event source-change state=%d", static_cast<int>(state_));
        }
    }
}

void Session::handle_output_returns()
{
    while (auto returned = device_->dequeue_buffer(Queue::Output)) {
        OutputSlot& slot = pool_.output(returned->index);
        slot.queued = false;
        if (returned->error()) {
            // The firmware rejected the bitstream in this slot. The
            // corresponding CAPTURE may never come; fail the target now.
            auto it = pending_.find(slot.token);
            if (it != pending_.end()) {
                Target& target = *it->second;
                target.pending = false;
                target.error = true;
                pending_.erase(it);
                ++stats_.errors;
                trace_("output error token=%llu output=%u", static_cast<unsigned long long>(slot.token),
                    slot.index);
            }
        }
        slot.token = 0;
    }
}

bool Session::handle_capture_returns()
{
    bool last = false;
    while (auto returned = device_->dequeue_buffer(Queue::Capture)) {
        FrameRef frame = pool_.take_capture(returned->index);
        const uint64_t slot_token = pool_.take_import_token(returned->index);
        const bool payload = returned->bytesused > returned->data_offset;
        if (returned->last()) {
            last = true;
            saw_last_ = true;
        }
        if (returned->error() && returned->timestamp_us == 0) {
            // Firmware error marker: no token, no pixels. Requeued by the
            // next requeue pass (use count returns to 1 when frame drops).
            ++stats_.errors;
            trace_("capture error index=%u flags=%#x", returned->index, returned->flags);
            continue;
        }
        if (!payload) {
            trace_("capture empty index=%u flags=%#x last=%d", returned->index, returned->flags, last ? 1 : 0);
            if (last)
                trace_("capture last");
            continue;
        }
        auto it = pending_.find(returned->timestamp_us);
        if (it == pending_.end()) {
            // Released target, or a completion after a drain; never publish
            // pixels the caller did not ask for.
            ++stats_.drops;
            trace_("capture token=%llu index=%u flags=%#x publish=drop",
                static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags);
            continue;
        }
        Target& target = *it->second;
        pending_.erase(it);
        if (returned->error()) {
            target.pending = false;
            target.error = true;
            ++stats_.errors;
            trace_("capture token=%llu index=%u flags=%#x publish=error",
                static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags);
            continue;
        }
        if (pool_.capture_memory() == CaptureMemory::Import && slot_token != returned->timestamp_us) {
            // The firmware wrote this picture into a buffer queued for a
            // different token: the client's surface for this token holds
            // someone else's pixels. Never report it as decoded.
            target.pending = false;
            target.error = true;
            ++stats_.errors;
            ++stats_.misplaced;
            trace_("capture token=%llu index=%u flags=%#x publish=misplaced slot_token=%llu",
                static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags,
                static_cast<unsigned long long>(slot_token));
            continue;
        }
        frame->layout().data_offset = returned->data_offset;
        publish(target, frame, *returned);
        cold_ = false;
        ++stats_.completed;
        ++progress_;
        if (last)
            trace_("capture last");
    }
    return last;
}

void Session::publish(Target& target, FrameRef frame, const Dequeued& completion)
{
    const char* how = "direct";
    if (pool_.capture_memory() == CaptureMemory::Import) {
        // The firmware wrote straight into the client's buffer; the slot
        // carried nothing of ours and is free once the ref drops.
        how = "import";
        target.publish = Publish::Import;
        target.frame.reset();
    } else if (target.publish == Publish::Import) {
        // Asked for Import, got an Mmap pool: the client buffer exists, so
        // fill it the Copy way. supports_import() tells the caller.
        target.publish = Publish::Copy;
    }
    if (target.publish == Publish::Copy) {
        require(target.destination != nullptr && copier_ != nullptr, "copy publish without a destination");
        try {
            copier_->copy(*target.destination, *frame);
        } catch (const Error&) {
            target.pending = false;
            target.error = true;
            ++stats_.errors;
            throw;
        }
        how = copier_->name();
        // The slot goes back to the firmware as soon as `frame` drops.
        target.frame.reset();
    } else if (target.publish == Publish::Direct) {
        target.frame = frame;
    }
    target.pending = false;
    target.completed = true;
    target.error = false;
    trace_("capture token=%llu index=%u flags=%#x publish=%s%s",
        static_cast<unsigned long long>(completion.timestamp_us), completion.index, completion.flags,
        target.publish == Publish::Copy ? "copy-" : "", how);
}

// ---------------------------------------------------------------------------
// Waiting

void Session::wait(Target& target)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!target.pending) {
        require(!target.error, "target decode failed", VA_STATUS_ERROR_DECODING_ERROR);
        return;
    }
    require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
    pump(0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync_timeout_ms());
    uint64_t seen_progress = progress_;
    bool drained = false;
    while (target.pending) {
        require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
        if (progress_ != seen_progress) {
            seen_progress = progress_;
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync_timeout_ms());
        }
        if (expired(deadline)) {
            ++stats_.timeouts;
            if (mode_ == SessionMode::DisplayOrder && !drained && state_ == SessionState::Streaming) {
                // Display-order firmware may be holding this picture behind
                // later input the client has not submitted yet. One drain
                // flushes the reorder window; after it, a still-pending
                // target is a real failure.
                trace_("sync timeout token=%llu pending=%zu queued=%u held=%u action=drain",
                    static_cast<unsigned long long>(target.token), pending_.size(), pool_.capture_queued(),
                    pool_.capture_held());
                drain("timeout", true);
                restart();
                drained = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.sync_timeout_ms);
                continue;
            }
            trace_("sync timeout token=%llu pending=%zu queued=%u held=%u action=fail",
                static_cast<unsigned long long>(target.token), pending_.size(), pool_.capture_queued(),
                pool_.capture_held());
            // Invalidate the token so a late completion is dropped rather
            // than written into a surface the client has moved on from.
            pending_.erase(target.token);
            target.pending = false;
            target.error = true;
            throw Error(VA_STATUS_ERROR_TIMEDOUT, "surface sync timeout");
        }
        lock.unlock();
        device_->poll(POLLIN | POLLPRI, 10);
        lock.lock();
        pump(0);
    }
    require(!target.error, "target decode failed", VA_STATUS_ERROR_DECODING_ERROR);
}

// ---------------------------------------------------------------------------
// Release, drain, restart, reconfigure, finish

void Session::release_locked(Target& target)
{
    if (target.pending) {
        pending_.erase(target.token);
        target.pending = false;
    }
    target.frame.reset();
    target.completed = false;
    target.error = false;
    target.token = 0;
}

void Session::release(Target& target)
{
    std::lock_guard<std::mutex> lock(mutex_);
    release_locked(target);
    if (state_ == SessionState::Reconfiguring) {
        try {
            continue_reconfigure();
        } catch (const Error&) {
            // fail() ran; the next submit reports it.
        }
    }
}

void Session::drain(const char* reason, bool wait_output)
{
    require(state_ == SessionState::Streaming, "drain outside Streaming");
    ++stats_.drains;
    trace_("drain begin reason=%s pending=%zu", reason, pending_.size());
    state_ = SessionState::Draining;
    saw_last_ = false;
    // LAST arrives on a buffer of its own; an Import pool has none queued
    // beyond the pictures in flight, so lend the firmware a scratch one.
    if (pool_.capture_memory() == CaptureMemory::Import)
        pool_.queue_capture_scratch();
    require(device_->decoder_command(V4L2_DEC_CMD_STOP), "DECODER_CMD_STOP busy", VA_STATUS_ERROR_HW_BUSY);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLastWaitMs);
    while (!saw_last_) {
        if (expired(deadline)) {
            fail("no LAST after STOP");
            throw Error(VA_STATUS_ERROR_DECODING_ERROR, "decoder did not drain");
        }
        pool_.requeue_capture();
        device_->poll(POLLIN | POLLPRI, 5);
        handle_events();
        handle_output_returns();
        handle_capture_returns();
    }
    // Iris recycles the final OUTPUT a few ticks after LAST; START with a
    // pre-STOP OUTPUT still queued returns EBUSY.
    if (wait_output) {
        const auto output_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kOutputDrainMs);
        while (pool_.output_queued() > 0 && !expired(output_deadline)) {
            device_->poll(POLLOUT, 5);
            handle_output_returns();
        }
    }
    fail_unrecoverable_tokens();
    state_ = SessionState::RestartPending;
    ++progress_;
    trace_("drain complete last=1 output_queued=%u", pool_.output_queued());
}

void Session::restart()
{
    require(state_ == SessionState::RestartPending, "restart outside RestartPending");
    pool_.requeue_capture();
    // EBUSY here means an OUTPUT is still owned by the firmware; give it a
    // short grace instead of declaring the instance dead.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kOutputDrainMs);
    while (!device_->decoder_command(V4L2_DEC_CMD_START)) {
        if (expired(deadline)) {
            fail("DECODER_CMD_START stayed busy");
            throw Error(VA_STATUS_ERROR_HW_BUSY, "decoder restart busy");
        }
        device_->poll(POLLOUT, 5);
        handle_output_returns();
    }
    saw_last_ = false;
    state_ = SessionState::Streaming;
    ++progress_;
    trace_("restart");
}

void Session::reconfigure()
{
    // Mid-stream resolution change. The kernel contract: STOP, wait LAST,
    // STREAMOFF CAPTURE, REQBUFS(0), then re-read the format and rebuild.
    // OUTPUT keeps streaming. Held frames from the old geometry stay valid
    // for their holders (they own the fd); the pool must be empty of holds
    // before REQBUFS(0), so callers are expected to have released targets
    // they no longer display. If one is still held, the change waits.
    ++stats_.reconfigures;
    trace_("session reconfigure %ux%u ->", config_.width, config_.height);
    drain("drc", false);
    state_ = SessionState::Reconfiguring;
    device_->stream(Queue::Capture, false);
    pool_.mark_all_capture_returned();
    // The copy engine may cache imports of the old slots; those imports
    // hold DMA-BUF references that would block REQBUFS(0).
    if (copier_)
        for (unsigned i = 0; i < pool_.capture_count(); ++i)
            copier_->forget(pool_.take_capture(i)->id());
    continue_reconfigure();
}

void Session::continue_reconfigure()
{
    require(state_ == SessionState::Reconfiguring, "continue_reconfigure outside Reconfiguring");
    // vb2 refuses REQBUFS(0) while an exported buffer is still open. A Direct
    // client may legitimately hold frames of the old geometry for display;
    // the change waits for release() and submit() reports SURFACE_BUSY in
    // the meantime. Copy clients never hold, so Chrome never waits here.
    if (pool_.capture_held() > 0) {
        trace_("session reconfigure deferred held=%u", pool_.capture_held());
        return;
    }
    pool_.release_capture();
    // Per the V4L2 stateful decoder contract, CAPTURE STREAMON after the
    // drain resumes decoding at the new geometry; no START is issued and
    // OUTPUT keeps streaming with the new-resolution access unit queued.
    source_change_ = true;
    state_ = SessionState::Configured;
    configure_capture();
    ++progress_;
    trace_("session reconfigure -> %ux%u", config_.width, config_.height);
}

void Session::fail_unrecoverable_tokens()
{
    // After a drain the firmware has returned every picture it decoded. A
    // token whose OUTPUT slot is still queued was never consumed (the
    // new-resolution access unit, or input behind a STOP) and decodes after
    // the restart; every other pending token is lost.
    for (auto it = pending_.begin(); it != pending_.end();) {
        bool still_queued = false;
        for (const auto& slot : pool_.outputs())
            if (slot.queued && slot.token == it->first) {
                still_queued = true;
                break;
            }
        if (still_queued) {
            ++it;
            continue;
        }
        it->second->pending = false;
        it->second->error = true;
        ++stats_.errors;
        trace_("drain lost token=%llu", static_cast<unsigned long long>(it->first));
        it = pending_.erase(it);
    }
}

void Session::finish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == SessionState::Failed || state_ == SessionState::Finished)
        return;
    if (state_ == SessionState::Configured) {
        // Nothing ever completed; no CAPTURE queue to drain.
        state_ = SessionState::Finished;
        return;
    }
    try {
        if (state_ == SessionState::Streaming)
            drain("finish", false);
    } catch (const Error&) {
        // fail() already ran.
    }
    state_ = state_ == SessionState::Failed ? SessionState::Failed : SessionState::Finished;
    trace_("session finish submitted=%llu completed=%llu errors=%llu drops=%llu timeouts=%llu drains=%llu "
           "misplaced=%llu",
        static_cast<unsigned long long>(stats_.submitted), static_cast<unsigned long long>(stats_.completed),
        static_cast<unsigned long long>(stats_.errors), static_cast<unsigned long long>(stats_.drops),
        static_cast<unsigned long long>(stats_.timeouts), static_cast<unsigned long long>(stats_.drains),
        static_cast<unsigned long long>(stats_.misplaced));
}

void Session::fail(const char* what)
{
    if (state_ == SessionState::Failed)
        return;
    state_ = SessionState::Failed;
    trace_("session failed: %s", what);
    fail_all_pending();
}

void Session::fail_all_pending()
{
    for (auto& [token, target] : pending_) {
        target->pending = false;
        target->error = true;
    }
    pending_.clear();
}

} // namespace iris
