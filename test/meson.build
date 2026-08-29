context_isolation = executable(
	'v4l2-context-isolation',
	'context-isolation.cc',
	dependencies: [libva_dep, libva_drm_dep, libdrm_dep])

# Hardware-independent regression guard for the browser-facing queue and
# stable-export invariants. The integration probe above remains opt-in because
# hosts without /dev/video nodes cannot execute it.
test('iris-source-invariants', find_program('iris-source-invariants.sh'))
test('iris-stateful-order-check', find_program('iris-stateful-order-check.sh'))

# This is an integration probe for a V4L2-capable target, so it is built but
# not registered as a default Meson test on hosts without /dev/video nodes.
