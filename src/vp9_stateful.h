/*
 * Stateful VP9 support for Qualcomm Iris V4L2 M2M decoders.
 */

#pragma once

#include <set>

extern "C" {
#include "linux/videodev2.h"

#include <va/va.h>
}

#include "context.h"

struct Buffer;
struct DriverData;
class V4L2M2MDevice;

class VP9StatefulContext : public Context {
public:
    static std::set<VAProfile> supported_profiles(const V4L2M2MDevice& device);

    VP9StatefulContext(DriverData* driver_data, V4L2M2MDevice device, int picture_width, int picture_height,
        std::span<VASurfaceID> surface_ids)
        : Context(driver_data, std::move(device), V4L2_PIX_FMT_VP9, picture_width, picture_height, surface_ids)
    {
        if (!surface_ids.empty())
            initialize(surface_ids);
    }

    VAStatus store_buffer(const Buffer& buffer) const override;
    int set_controls() override { return VA_STATUS_SUCCESS; }
    bool uses_request_api() const override { return false; }
    bool uses_stateful_streaming() const override { return true; }
};
