// SPDX-License-Identifier: MIT

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>

namespace va {

namespace {

VAStatus guarded(VADriverContextP ctx, const char* where, auto&& body)
{
    try {
        return body();
    } catch (const std::exception& error) {
        return report(ctx, where, error);
    }
}

} // namespace

VAStatus queryConfigProfiles(VADriverContextP ctx, VAProfile* profiles, int* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    int n = 0;
    for (VAProfile profile : d.capabilities.profiles)
        if (n < static_cast<int>(kMaxProfiles))
            profiles[n++] = profile;
    *count = n;
    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoints, int* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    if (d.capabilities.profiles.count(profile)) {
        entrypoints[0] = VAEntrypointVLD;
        *count = 1;
    } else {
        *count = 0;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus getConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
    VAConfigAttrib* attributes, int count)
{
    auto& d = data(ctx);
    const bool supported = d.capabilities.profiles.count(profile) && entrypoint == VAEntrypointVLD;
    for (int i = 0; i < count; ++i) {
        switch (attributes[i].type) {
        case VAConfigAttribRTFormat:
            attributes[i].value = supported ? VA_RT_FORMAT_YUV420 : VA_ATTRIB_NOT_SUPPORTED;
            if (supported && (profile == VAProfileHEVCMain10 || profile == VAProfileVP9Profile2)
                && d.capabilities.p010)
                attributes[i].value |= VA_RT_FORMAT_YUV420_10;
            break;
        case VAConfigAttribDecSliceMode:
            attributes[i].value = VA_DEC_SLICE_MODE_NORMAL;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[i].value = d.capabilities.max_width;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[i].value = d.capabilities.max_height;
            break;
        default:
            attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attributes,
    int count, VAConfigID* id)
{
    return guarded(ctx, "createConfig", [&] {
        auto& d = data(ctx);
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        iris::require(d.capabilities.profiles.count(profile) > 0, "profile not advertised",
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        iris::require(entrypoint == VAEntrypointVLD, "only VLD is supported", VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        uint32_t rt_format = VA_RT_FORMAT_YUV420;
        if (d.capabilities.p010 && (profile == VAProfileHEVCMain10 || profile == VAProfileVP9Profile2))
            rt_format |= VA_RT_FORMAT_YUV420_10;
        for (int i = 0; i < count; ++i) {
            if (attributes[i].type == VAConfigAttribRTFormat && attributes[i].value != VA_ATTRIB_NOT_SUPPORTED)
                rt_format = attributes[i].value;
        }
        iris::require(rt_format & (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10), "unsupported RT format",
            VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        *id = d.next_id++;
        d.configs[*id] = Config { profile, entrypoint, rt_format };
        return VA_STATUS_SUCCESS;
    });
}

VAStatus destroyConfig(VADriverContextP ctx, VAConfigID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    return d.configs.erase(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus queryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile* profile, VAEntrypoint* entrypoint,
    VAConfigAttrib* attributes, int* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.configs.find(id);
    if (it == d.configs.end())
        return VA_STATUS_ERROR_INVALID_CONFIG;
    if (profile)
        *profile = it->second.profile;
    if (entrypoint)
        *entrypoint = it->second.entrypoint;
    if (count)
        *count = 1;
    if (attributes) {
        attributes[0].type = VAConfigAttribRTFormat;
        attributes[0].value = it->second.rt_format;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus queryDisplayAttributes(VADriverContextP, VADisplayAttribute*, int* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}
VAStatus getDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

} // namespace va
