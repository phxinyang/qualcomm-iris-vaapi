// SPDX-License-Identifier: MIT

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <va/va_drmcommon.h>
}

namespace va {

VAStatus createBuffer(VADriverContextP ctx, VAContextID, VABufferType type, unsigned size, unsigned count,
    void* initial, VABufferID* id)
{
    switch (type) {
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VASliceParameterBufferType:
    case VASliceDataBufferType:
    case VAImageBufferType:
        break;
    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    auto& d = data(ctx);
    auto b = std::make_shared<Buffer>();
    b->type = type;
    b->size = size;
    b->count = count;
    b->data.resize(static_cast<size_t>(size) * count);
    if (initial)
        std::memcpy(b->data.data(), initial, b->data.size());
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    *id = d.next_id++;
    d.buffers[*id] = b;
    return VA_STATUS_SUCCESS;
}

VAStatus destroyBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (it->second->acquired_fd >= 0)
        close(it->second->acquired_fd);
    d.buffers.erase(it);
    return VA_STATUS_SUCCESS;
}

VAStatus mapBuffer(VADriverContextP ctx, VABufferID id, void** mapped)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    *mapped = it->second->data.data();
    return VA_STATUS_SUCCESS;
}

VAStatus unmapBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    return d.buffers.count(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus bufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    it->second->count = count;
    it->second->data.resize(static_cast<size_t>(it->second->size) * count);
    return VA_STATUS_SUCCESS;
}

VAStatus bufferInfo(VADriverContextP ctx, VABufferID id, VABufferType* type, unsigned* size, unsigned* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    *type = it->second->type;
    *size = it->second->size;
    *count = it->second->count;
    return VA_STATUS_SUCCESS;
}

// Handles for derived images: the DMA-BUF behind the surface. Acquiring a
// handle before the surface's first decode marks it persistent, like an
// export does.
VAStatus acquireBufferHandle(VADriverContextP ctx, VABufferID id, VABufferInfo* info)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    auto& b = *it->second;
    if (b.type != VAImageBufferType || b.derived_surface == VA_INVALID_ID)
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    if (info->mem_type && !(info->mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    auto sit = d.surfaces.find(b.derived_surface);
    if (sit == d.surfaces.end())
        return VA_STATUS_ERROR_INVALID_SURFACE;
    auto& s = *sit->second;
    int fd = -1;
    unsigned size = 0;
    if (s.target.frame) {
        fd = s.target.frame->fd();
        size = s.target.frame->length();
    } else {
        auto& stable = ensure_stable(d, s);
        fd = stable.fd();
        size = stable.size();
        if (!s.target.completed)
            s.persistent_export = true;
    }
    b.acquired_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (b.acquired_fd < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    ++s.acquired_handles;
    *info = {};
    info->handle = static_cast<uintptr_t>(b.acquired_fd);
    info->type = b.type;
    info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    info->mem_size = size;
    return VA_STATUS_SUCCESS;
}

VAStatus releaseBufferHandle(VADriverContextP ctx, VABufferID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    auto& b = *it->second;
    if (b.acquired_fd >= 0) {
        close(b.acquired_fd);
        b.acquired_fd = -1;
        auto sit = d.surfaces.find(b.derived_surface);
        if (sit != d.surfaces.end() && sit->second->acquired_handles > 0)
            --sit->second->acquired_handles;
    }
    return VA_STATUS_SUCCESS;
}

} // namespace va
