# Copyright (C) 2019 Bootlin
# Copyright (C) 2023 Max Schettler
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sub license, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice (including the
# next paragraph) shall be included in all copies or substantial portions
# of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
# IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
# ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

project('libva-v4l2', ['cpp', 'c'],
	# Derived from git when one is available. The fallback matters: the driver
	# is routinely built from a source tree synced to a target device without
	# its .git directory, and a hard failure here blocks the build entirely.
	version: run_command(['sh', '-c', 'git rev-parse --short HEAD 2>/dev/null || echo unknown'],
		check: true).stdout().strip(),
	meson_version: '>= 0.56.0')

cc = meson.get_compiler('cpp')

libva_dep = dependency('libva', version : '>= 1.1.0')
libdrm_dep = dependency('libdrm', version : '>= 2.4.52')
libgstcodecparsers_dep = dependency('gstreamer-codecparsers-1.0', version : '>= 1.12', required: false)
libgstcodecs_dep = cc.find_library('gstcodecs-1.0',  # Present but not advertised via pkgconfig.
	has_headers: ['gst/codecs/gstvp9statefulparser.h'],
	header_dependencies: libgstcodecparsers_dep,
	required: false,
)
libudev_dep = dependency('libudev', version : '>= 247')
# GPU copy engine for persistent exports (docs/architecture.md §3.3). Optional
# at build time so a host without Mesa dev packages still builds and tests
# the core; the driver falls back to the CPU engine when absent.
egl_dep = dependency('egl', required: false)
glesv2_dep = dependency('glesv2', required: false)
gbm_dep = dependency('gbm', required: false)
have_gpu_copy = egl_dep.found() and glesv2_dep.found() and gbm_dep.found()
libva_drm_dep = dependency('libva-drm', version : '>= 1.1.0', required: false)
kernel_dep = declare_dependency(include_directories : get_option('kernel_headers'))

va_api_version_array = libva_dep.version().split('.')
va_api_major_version = va_api_version_array[0]
va_api_minor_version = va_api_version_array[1]

va_driver_init_func = '__vaDriverInit_@0@_@1@'.format(va_api_major_version, va_api_minor_version)

subdir('src')
subdir('test')

# Packaging with --prefix=/usr installs the driver, process-scoped browser
# launcher and desktop entries together. For direct system deployment, the
# install-system.sh helper additionally reads libva's canonical driverdir so
# a /usr/local project prefix cannot hide the driver from libva.
install_data(
	'scripts/iris-vaapi-browser',
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)
install_data(
	'data/chromium-iris-v4l2.desktop',
	'data/google-chrome-iris-v4l2.desktop',
	'data/chromium-iris-vulkan-webgpu.desktop',
	'data/google-chrome-iris-vulkan-webgpu.desktop',
	install_dir: join_paths(get_option('datadir'), 'applications'),
)
