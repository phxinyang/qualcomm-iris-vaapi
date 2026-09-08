/* Qualcomm Iris stateful HEVC VA-API context. */

#pragma once

#include <set>
#include <optional>

extern "C" {
#include "linux/videodev2.h"
#include <va/va.h>
#include <va/va_dec_hevc.h>
}

#include "context.h"

struct Buffer;
struct DriverData;
struct Surface;
class V4L2M2MDevice;

class HEVCContext : public Context {
public:
    static std::set<VAProfile> supported_profiles(const V4L2M2MDevice& device);

    HEVCContext(DriverData* driver_data, V4L2M2MDevice device, VAProfile profile, int picture_width,
        int picture_height, std::span<VASurfaceID> surface_ids)
        : Context(driver_data, std::move(device), V4L2_PIX_FMT_HEVC, picture_width, picture_height, surface_ids)
        , profile(profile)
    {
        if (!surface_ids.empty())
            initialize(surface_ids);
    }

    VAStatus store_buffer(const Buffer& buffer) const override;
    int set_controls() override { return VA_STATUS_SUCCESS; }
    bool uses_request_api() const override { return false; }
    bool uses_stateful_streaming() const override { return true; }
    bool stateful_timeout_drain() const override { return true; }

private:
    bool prepend_parameter_sets(Surface& surface) const;
    // Own the effective tables: VA buffers may be destroyed after submission.
    mutable std::optional<VAIQMatrixBufferHEVC> scaling_matrix_;
    VAProfile profile;
};
