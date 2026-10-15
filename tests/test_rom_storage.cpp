#include "m0boy/rom_storage.h"
#include "m0_rom_codec.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <zlib.h>

static void check(bool ok) {
    if (!ok) { std::fprintf(stderr, "ROM codec test failed\n"); std::abort(); }
}

static size_t greedy_bytes(const uint8_t* data, size_t size) {
    size_t bits = 0;
    for (size_t i = 0; i < size;) {
        size_t best = 0;
        for (size_t distance = 1; distance <= std::min<size_t>(i, 256); ++distance) {
            size_t n = 0;
            while (n < 17 && i + n < size && data[i - distance + n] == data[i + n]) ++n;
            best = std::max(best, n);
        }
        if (best >= 2) { bits += 13; i += best; }
        else { bits += 9; ++i; }
    }
    return (bits + 7) / 8;
}

int main() {
    std::mt19937 random(12345);
    m0_inflate_workspace work;
    static_assert(sizeof(work) == 1032);
    std::array<uint8_t, 512> input;
    std::array<uint8_t, 514> output;
    check(m0boy::rom::fits_raw(240, 40, 100, 100));
    check(!m0boy::rom::fits_raw(240, 40, 101, 100));
    check(m0boy::rom::density(1, 50000, m0boy::rom::Codec::deflate, 300) == 0);
    auto units = m0boy::rom::select_units({100, 512, 100}, {100, 1, 0}, 1, 0, 512, 512, 50000);
    check(units.raw == std::vector<size_t>{0} && units.packed.empty());
    units = m0boy::rom::select_units({100, 512}, {100, 1}, 1, 0, 612, 512, 50000);
    check(units.raw == std::vector<size_t>{1} && units.packed == std::vector<size_t>{0});
    for (unsigned trial = 0; trial < 2000; ++trial) {
        size_t size = trial < 512 ? trial + 1 : 512;
        for (size_t i = 0; i < size; ++i) {
            const unsigned r = random();
            input[i] = trial % 4 == 0 ? r : trial % 4 == 1 ? r % 4 :
                       trial % 4 == 2 ? i / 8 + r % 3 : 0;
        }
        auto lz = m0boy::rom::lzss_encode(input.data(), size);
        check(lz.size() <= greedy_bytes(input.data(), size));
        output.fill(0xa5);
        check(m0_rom_lzss(lz.data(), lz.size(), output.data() + 1, size));
        check(std::equal(input.begin(), input.begin() + size, output.begin() + 1));
        check(output[0] == 0xa5 && output[size + 1] == 0xa5);
        if (size == 512) {
            auto padded = lz;
            padded.push_back(0); padded.push_back(0);
            m0_rom_unpack512_c(padded.data(), output.data() + 1);
            check(std::equal(input.begin(), input.end(), output.begin() + 1));
            check(output[0] == 0xa5 && output.back() == 0xa5);
        }
        for (int mode = 0; mode < 3; ++mode) {
            z_stream z{};
            check(deflateInit2(&z, mode == 0 ? 0 : 9, Z_DEFLATED, -9, 8,
                               mode == 1 ? Z_FIXED : Z_DEFAULT_STRATEGY) == Z_OK);
            std::array<uint8_t, 1024> packed;
            z.next_in = input.data(); z.avail_in = size;
            z.next_out = packed.data(); z.avail_out = packed.size();
            check(deflate(&z, Z_FINISH) == Z_STREAM_END);
            size_t n = z.total_out;
            deflateEnd(&z);
            output.fill(0xa5);
            check(m0_rom_inflate(packed.data(), n, output.data() + 1, size, &work));
            check(std::equal(input.begin(), input.begin() + size, output.begin() + 1));
            check(output[0] == 0xa5 && output[size + 1] == 0xa5);
            check(!m0_rom_inflate(packed.data(), n - 1, output.data() + 1, size, &work));
        }
        auto z = m0boy::rom::deflate_encode(input.data(), size);
        check(m0_rom_inflate(z.data(), z.size(), output.data() + 1, size, &work));
        if (size == 512 && trial % 10 == 0) {
            z_stream stream{};
            check(deflateInit2(&stream, 9, Z_DEFLATED, -9, 8, Z_DEFAULT_STRATEGY) == Z_OK);
            std::array<uint8_t, 2048> blocks;
            stream.next_out = blocks.data(); stream.avail_out = blocks.size();
            stream.next_in = input.data(); stream.avail_in = 256;
            check(deflate(&stream, Z_SYNC_FLUSH) == Z_OK);
            stream.next_in = input.data() + 256; stream.avail_in = 256;
            check(deflate(&stream, Z_FINISH) == Z_STREAM_END);
            size_t packed = stream.total_out;
            deflateEnd(&stream);
            check(m0_rom_inflate(blocks.data(), packed, output.data() + 1, size, &work));
            check(std::equal(input.begin(), input.end(), output.begin() + 1));
        }
    }
    /* Bounded decoding of arbitrary/truncated input under sanitizers. */
    for (unsigned i = 0; i < 10000; ++i) {
        for (auto& b : input) b = random();
        const size_t n = random() % 513;
        output.fill(0xa5);
        m0_rom_inflate(input.data(), n, output.data() + 1, 512, &work);
        m0_rom_lzss(input.data(), n, output.data() + 1, 512);
        check(output.front() == 0xa5 && output.back() == 0xa5);
    }
    std::puts("ROM codecs: LZSS, stored/fixed/dynamic DEFLATE, truncation and bounds passed");
}
