// SPDX-License-Identifier: MIT
// Kept in C: libdrm's msm_drm.h names a field "or", a C++ operator keyword.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <xf86drm.h>
#include <libdrm/msm_drm.h>

#ifndef MSM_BO_CACHED_COHERENT
#define MSM_BO_CACHED_COHERENT 0x080000
#endif

int iris_msm_allocate(int render_fd, unsigned size, int* coherent)
{
    *coherent = 0;
    struct drm_msm_gem_new request = { .size = size, .flags = MSM_BO_CACHED_COHERENT };
    if (drmIoctl(render_fd, DRM_IOCTL_MSM_GEM_NEW, &request) < 0) {
        // Unsupported cache modes return EINVAL; anything else is a real
        // failure and must not be masked by a retry.
        if (errno != EINVAL && errno != EOPNOTSUPP)
            return -1;
        request.flags = MSM_BO_WC;
        if (drmIoctl(render_fd, DRM_IOCTL_MSM_GEM_NEW, &request) < 0)
            return -1;
    }
    int prime_fd = -1;
    const int exported = drmPrimeHandleToFD(render_fd, request.handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd);
    const int saved_errno = errno;
    struct drm_gem_close close_request = { .handle = request.handle };
    drmIoctl(render_fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    if (exported < 0) {
        if (prime_fd >= 0)
            close(prime_fd);
        errno = saved_errno;
        return -1;
    }
    *coherent = request.flags == MSM_BO_CACHED_COHERENT;
    return prime_fd;
}
