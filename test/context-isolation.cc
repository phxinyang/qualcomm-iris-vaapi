/*
 * Regression probe for stateful V4L2 sessions.
 *
 * The probe deliberately creates two VA contexts before submitting any
 * pictures. A stateful decoder must give each context an independent V4L2
 * queue; sharing the queue makes the second create fail with EBUSY.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_drm.h>
}

namespace {

int fail(const char* operation, VAStatus status)
{
    std::fprintf(stderr, "%s: %s\n", operation, vaErrorStr(status));
    return 1;
}

} // namespace

int main()
{
    const char* device_path = std::getenv("VA_DRM_DEVICE");
    if (!device_path)
        device_path = "/dev/dri/renderD128";

    const int drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        std::perror(device_path);
        return 1;
    }

    VADisplay display = vaGetDisplayDRM(drm_fd);
    if (!display) {
        std::fprintf(stderr, "vaGetDisplayDRM failed\n");
        close(drm_fd);
        return 1;
    }

    int major = 0;
    int minor = 0;
    VAStatus status = vaInitialize(display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        close(drm_fd);
        return fail("vaInitialize", status);
    }

    VAConfigID config = VA_INVALID_ID;
    status = vaCreateConfig(display, VAProfileH264High, VAEntrypointVLD, nullptr, 0, &config);
    if (status != VA_STATUS_SUCCESS) {
        vaTerminate(display);
        close(drm_fd);
        return fail("vaCreateConfig", status);
    }

    std::vector<VASurfaceID> surfaces(8, VA_INVALID_SURFACE);
    status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 640, 368, surfaces.data(), surfaces.size(), nullptr, 0);
    if (status != VA_STATUS_SUCCESS) {
        vaDestroyConfig(display, config);
        vaTerminate(display);
        close(drm_fd);
        return fail("vaCreateSurfaces", status);
    }

    VAContextID first = VA_INVALID_ID;
    VAContextID second = VA_INVALID_ID;
    status = vaCreateContext(display, config, 640, 368, VA_PROGRESSIVE, surfaces.data(), surfaces.size(), &first);
    if (status != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(display, surfaces.data(), surfaces.size());
        vaDestroyConfig(display, config);
        vaTerminate(display);
        close(drm_fd);
        return fail("vaCreateContext(first)", status);
    }

    status = vaCreateContext(display, config, 640, 368, VA_PROGRESSIVE, surfaces.data(), surfaces.size(), &second);
    if (status != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "second context failed while first context %u was alive\n", first);
        vaDestroyContext(display, first);
        vaDestroySurfaces(display, surfaces.data(), surfaces.size());
        vaDestroyConfig(display, config);
        vaTerminate(display);
        close(drm_fd);
        return fail("vaCreateContext(second)", status);
    }

    std::printf("created two contexts: %u and %u\n", first, second);
    vaDestroyContext(display, second);
    vaDestroyContext(display, first);
    vaDestroySurfaces(display, surfaces.data(), surfaces.size());
    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(drm_fd);
    return 0;
}
