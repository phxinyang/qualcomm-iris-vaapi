// Test the actual stable-export layout generator without opening a decoder.
// Device-dependent entry points are removed by section GC, as in the HEVC
// parameter-set test.
#include "../src/surface.cc"

#include <iostream>
#include <stdexcept>

int main()
{
    try {
        const std::pair<unsigned, unsigned> sizes[] = {
            { 720, 1280 }, { 1080, 1920 }, { 480, 360 }, { 1440, 1080 },
            { 320, 180 }, { 640, 360 }, { 960, 720 }, { 1280, 720 },
            { 1920, 1080 }, { 96, 96 }, { 8192, 8192 },
        };
        for (const auto& [width, height] : sizes) {
            Surface surface {};
            surface.width = width;
            surface.height = height;
            const size_t size = prepare_compact_nv12_export_layout(surface);
            const auto& layout = surface.stable_export_layout;
            if (size == 0 || layout.size() != 2)
                throw std::runtime_error("missing NV12 export planes");
            const unsigned expected_pitch = (width + 63u) & ~63u;
            if (layout[0].pitch != expected_pitch || layout[1].pitch != expected_pitch) {
                std::cerr << "FAIL stable NV12 export width=" << width << " height=" << height
                          << ": expected GPU-importable pitch=" << expected_pitch
                          << ", got " << layout[0].pitch << '/' << layout[1].pitch << '\n';
                return 1;
            }
            if (surface.width != width || surface.height != height
                || layout[0].offset != 0 || layout[0].size != expected_pitch * height
                || layout[1].offset != layout[0].size
                || layout[1].size != expected_pitch * (height / 2)
                || size != static_cast<size_t>(layout[1].offset) + layout[1].size
                || layout[0].physical_plane_index != 0 || layout[1].physical_plane_index != 0)
                throw std::runtime_error("visible dimensions or plane extents changed");
            if (prepare_compact_nv12_export_layout(surface) != size)
                throw std::runtime_error("repeat export changed layout");
            // Once published, pitches/offsets must not change if the surface
            // is inspected during queue reconfiguration.
            surface.width += 128;
            if (prepare_compact_nv12_export_layout(surface) != 0
                || surface.stable_export_layout[0].pitch != expected_pitch)
                throw std::runtime_error("published layout was silently replaced");
        }
        std::cout << "PASS stable NV12 export: 11 geometries, GPU pitch and immutable layout\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
