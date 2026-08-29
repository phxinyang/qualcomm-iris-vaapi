/* Qualcomm Iris stateful AV1 VA-API context. */

#pragma once

#include <set>

extern "C" {
#include "linux/videodev2.h"
#include <va/va.h>
}

// Older sanitized kernel headers predate the stateful AV1 fourcc even though
// the target kernel and firmware expose it. Keep the userspace build portable.
#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '0', '1')
#endif

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
};
