/* MSB-first bitstream writer shared by the generated parameter sets. */

#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

// H.264 and HEVC both synthesise parameter sets from VA metadata because the
// long-format API does not carry the original SPS/PPS/VPS, and AV1 has to
// rebuild whole OBUs for the same reason. All three need the same MSB-first
// writer, so it lives here instead of being copied per codec.
class BitWriter {
public:
    void bit(unsigned value)
    {
        if (bit_offset_ == 0)
            data_.push_back(0);
        data_.back() |= (value & 1u) << (7u - bit_offset_);
        bit_offset_ = (bit_offset_ + 1u) & 7u;
    }

    void bits(uint64_t value, unsigned count)
    {
        for (unsigned i = count; i > 0; --i)
            bit(static_cast<unsigned>(value >> (i - 1)));
    }

    // Exp-Golomb. H.264/HEVC call this ue(v); AV1 calls the identical coding
    // uvlc(). Both spellings are provided so each codec reads like its spec.
    void ue(uint32_t value)
    {
        const uint32_t code_num = value + 1;
        unsigned width = 0;
        for (uint32_t n = code_num; n > 0; n >>= 1)
            ++width;
        for (unsigned i = 1; i < width; ++i)
            bit(0);
        bits(code_num, width);
    }

    void se(int32_t value)
    {
        ue(value > 0 ? static_cast<uint32_t>(value * 2 - 1) : static_cast<uint32_t>(-value * 2));
    }

    void uvlc(uint32_t value) { ue(value); }

    // AV1 su(n): n-bit two's complement.
    void su(int32_t value, unsigned count)
    {
        bits(static_cast<uint64_t>(static_cast<uint32_t>(value)) & ((1ull << count) - 1), count);
    }

    // AV1 ns(n): non-symmetric unsigned, spec 4.10.7. Values below the
    // threshold m use one fewer bit than the rest.
    void ns(uint32_t value, uint32_t n)
    {
        if (n <= 1)
            return;
        unsigned w = 0;
        for (uint32_t v = n; v > 0; v >>= 1)
            ++w;
        const uint32_t m = (1u << w) - n;
        if (value < m) {
            bits(value, w - 1);
            return;
        }
        bits(value + m, w);
    }

    // AV1 le(n): n little-endian bytes. Only valid at a byte boundary.
    void le(uint64_t value, unsigned bytes)
    {
        for (unsigned i = 0; i < bytes; ++i)
            bits((value >> (i * 8)) & 0xffu, 8);
    }

    // AV1 leb128(): 7 payload bits per byte, high bit continues.
    void leb128(uint64_t value)
    {
        do {
            uint8_t byte = value & 0x7fu;
            value >>= 7;
            if (value != 0)
                byte |= 0x80u;
            bits(byte, 8);
        } while (value != 0);
    }

    // Trailing one bit followed by zero padding to the next byte.
    void trailing_bits()
    {
        bit(1);
        while (bit_offset_ != 0)
            bit(0);
    }

    // AV1 byte_alignment(): zero padding only, no trailing one.
    void byte_align()
    {
        while (bit_offset_ != 0)
            bit(0);
    }

    void bytes(const uint8_t* source, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            bits(source[i], 8);
    }

    bool byte_aligned() const { return bit_offset_ == 0; }

    const std::vector<uint8_t>& data() const { return data_; }

private:
    std::vector<uint8_t> data_;
    unsigned bit_offset_ = 0;
};

// Emit an Annex-B start code, the codec's NAL header bytes, and the payload
// with emulation prevention applied.
inline void append_escaped_nal(
    std::vector<uint8_t>& stream, std::initializer_list<uint8_t> nal_header, const BitWriter& writer)
{
    stream.insert(stream.end(), { 0, 0, 0, 1 });
    stream.insert(stream.end(), nal_header);
    unsigned zero_count = 0;
    for (const auto byte : writer.data()) {
        if (zero_count >= 2 && byte <= 3) {
            stream.push_back(3);
            zero_count = 0;
        }
        stream.push_back(byte);
        zero_count = byte == 0 ? zero_count + 1 : 0;
    }
}
