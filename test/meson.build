# Integration probe for a V4L2-capable target: built when libva-drm is
# available, never registered as a default test because it needs a decoder
# node. Hosts without libva-drm (CI containers, cross builds) still get the
# static checks below.
if libva_drm_dep.found()
	context_isolation = executable(
		'v4l2-context-isolation',
		'context-isolation.cc',
		dependencies: [libva_dep, libva_drm_dep, libdrm_dep])
endif

# Hardware-independent regression guards for the browser-facing queue and
# stable-export invariants. The integration probe above remains opt-in because
# hosts without /dev/video nodes cannot execute it.
#
# run-static-checks.sh is the same entry point CI uses, so `meson test` and CI
# cannot drift apart. The individual checks stay registered as well to keep
# failures attributable without reading the aggregate output.
test('static-checks', find_program('run-static-checks.sh'),
     args: [meson.project_source_root()])
test('iris-source-invariants', find_program('iris-source-invariants.sh'),
     args: [meson.project_source_root()])
test('iris-stateful-order-check', find_program('iris-stateful-order-check.sh'),
     args: [meson.project_source_root()])
test('iris-env-doc-check', find_program('iris-env-doc-check.sh'),
     args: [meson.project_source_root()])
test('iris-surface-limits-check', find_program('iris-surface-limits-check.sh'),
     args: [meson.project_source_root()])
