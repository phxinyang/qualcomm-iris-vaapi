/* Qualcomm Iris stateful AV1 VA-API context. */

#pragma once

#include <cstdint>
#include <set>
#include <vector>

extern "C" {
#include "linux/videodev2.h"
#include <va/va.h>
}

// Older sanitized kernel headers predate the stateful AV1 fourcc even though
// the target kernel and firmware expose it. Keep the userspace build portable.
#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '0', '1')
#endif

#include "av1_obu.h"
#include "context.h"

struct Buffer;
struct DriverData;
class V4L2M2MDevice;

class AV1Context : public Context {
public:
    static std::set<VAProfile> supported_profiles(const V4L2M2MDevice& device);

    AV1Context(DriverData* driver_data, V4L2M2MDevice& device, int picture_width, int picture_height,
        std::span<VASurfaceID> surface_ids)
        : Context(driver_data, device, V4L2_PIX_FMT_AV1, picture_width, picture_height, surface_ids)
    {
        if (!surface_ids.empty())
            initialize(surface_ids);
    }

    VAStatus store_buffer(const Buffer& buffer) const override;
    int set_controls() override { return VA_STATUS_SUCCESS; }
    bool uses_request_api() const override { return false; }
    bool uses_stateful_streaming() const override { return true; }
    unsigned stateful_frame_type(VASurfaceID surface_id) const override;
    bool stateful_sequence_start(VASurfaceID surface_id) override;

    // An OBU frame header must state which reference slots the frame writes
    // into, and VA-API does not carry refresh_frame_flags. The only way to
    // recover it is to compare the reference map of the following frame
    // against this frame's surface, so a frame is held back until its
    // successor arrives.
    VAStatus stateful_submit_picture(VASurfaceID surface_id) override;
    VAStatus stateful_flush_deferred() override;

private:
    void observe_picture(const VADecPictureParameterBufferAV1& picture) const;
    VAStatus release_deferred(uint8_t refresh_frame_flags);

    mutable AV1SequenceState sequence_;
    mutable AV1ReferenceState references_;
    mutable VASurfaceID deferred_surface_ = VA_INVALID_SURFACE;
    mutable VADecPictureParameterBufferAV1 deferred_picture_ {};
    mutable std::vector<AV1TileEntry> deferred_tiles_;
    mutable std::vector<AV1TileEntry> tiles_;
    mutable bool deferred_emits_sequence_header_ = false;
    mutable bool emit_sequence_header_ = false;
    mutable bool seen_sequence_ = false;
    mutable uint8_t pending_refresh_ = 0;
    mutable bool pending_refresh_known_ = false;
};
