#include "m0boy/rom_storage.h"

#include <algorithm>
#include <stdexcept>
#include <zlib.h>

namespace m0boy::rom {

std::vector<uint8_t> lzss_encode(const uint8_t* data, size_t size) {
    if (size > 512) throw std::invalid_argument("ROM compression unit exceeds 512 bytes");
    struct Match { unsigned length = 0, offset = 0; };
    std::vector<Match> matches(size);
    for (size_t i = 0; i < size; ++i) {
        const size_t lo = i > 256 ? i - 256 : 0;
        for (size_t p = i; p-- > lo;) {
            unsigned length = 0;
            while (length < 17 && i + length < size && data[p + length] == data[i + length]) ++length;
            if (length > matches[i].length) matches[i] = {length, static_cast<unsigned>(i - p)};
            if (length == 17) break;
        }
    }
    /* Every prefix of a longest match is also valid. A shorter match (or a
     * literal) can expose a much better match at the next position. */
    std::vector<unsigned> cost(size + 1), length(size);
    for (size_t i = size; i-- > 0;) {
        cost[i] = 9 + cost[i + 1];
        for (unsigned n = 2; n <= matches[i].length; ++n) {
            const unsigned c = 13 + cost[i + n];
            if (c < cost[i]) { cost[i] = c; length[i] = n; }
        }
    }
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    unsigned bits = 0;
    auto put = [&](unsigned value, unsigned count) {
        while (count) {
            acc = (acc << 1) | ((value >> --count) & 1u);
            if (++bits == 8) { out.push_back(static_cast<uint8_t>(acc)); bits = 0; acc = 0; }
        }
    };
    for (size_t i = 0; i < size;) {
        if (length[i]) {
            put(0, 1); put(matches[i].offset - 1, 8); put(length[i] - 2, 4);
            i += length[i];
        } else { put(1, 1); put(data[i++], 8); }
    }
    if (bits) out.push_back(static_cast<uint8_t>(acc << (8 - bits)));
    return out;
}

std::vector<uint8_t> deflate_encode(const uint8_t* data, size_t size) {
    if (size > 512) throw std::invalid_argument("ROM compression unit exceeds 512 bytes");
    z_stream stream{};
    if (deflateInit2(&stream, 9, Z_DEFLATED, -9, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("cannot initialise ROM DEFLATE encoder");
    std::vector<uint8_t> out(compressBound(size));
    stream.next_in = const_cast<Bytef*>(data);
    stream.avail_in = static_cast<uInt>(size);
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const size_t used = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) throw std::runtime_error("cannot compress ROM sector");
    out.resize(used);
    return out;
}

Block encode(const uint8_t* data, size_t size, bool deflate) {
    Block out{Codec::raw, {data, data + size}};
    auto lz = lzss_encode(data, size);
    if (lz.size() < out.bytes.size()) out = {Codec::lzss, std::move(lz)};
    if (deflate) {
        auto z = deflate_encode(data, size);
        if (z.size() < out.bytes.size()) out = {Codec::deflate, std::move(z)};
    }
    return out;
}

double density(double reads, double sd_cycles, Codec codec, size_t bytes) {
    const double decode = codec == Codec::lzss ? lzss_cycles : codec == Codec::deflate ? deflate_cycles : 0;
    return bytes && sd_cycles > decode ? reads * (sd_cycles - decode) / bytes : 0;
}

bool fits_raw(size_t flash, size_t runtime, size_t rom_bytes, size_t code_bytes) {
    return runtime <= flash && rom_bytes <= flash - runtime && code_bytes <= flash - runtime - rom_bytes;
}

std::vector<size_t> raw_units(const std::vector<size_t>& packed_sizes,
                              const std::vector<double>& weights, size_t sectors_per_unit,
                              size_t first_unit, size_t raw_budget, size_t extra_budget) {
    std::vector<size_t> order, chosen;
    if (!sectors_per_unit) return chosen;
    for (size_t unit = first_unit; unit < weights.size(); ++unit)
        if (weights[unit] > 0 && (unit + 1) * sectors_per_unit <= packed_sizes.size()) order.push_back(unit);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return weights[a] > weights[b]; });
    const size_t unit_bytes = sectors_per_unit * 512;
    for (size_t unit : order) {
        size_t packed = 0;
        for (size_t s = unit * sectors_per_unit; s < (unit + 1) * sectors_per_unit; ++s) packed += packed_sizes[s];
        const size_t extra = unit_bytes > packed ? unit_bytes - packed : 0;
        if (unit_bytes <= raw_budget && extra <= extra_budget) {
            chosen.push_back(unit); raw_budget -= unit_bytes; extra_budget -= extra;
        }
    }
    return chosen;
}

Units select_units(const std::vector<size_t>& sizes, const std::vector<double>& weights,
                   size_t sectors_per_unit, size_t first_unit, size_t budget, size_t raw_budget,
                   double sd_cycles) {
    struct Candidate { double score; size_t unit, bytes; bool raw; };
    std::vector<Candidate> candidates;
    std::vector<unsigned> selected(weights.size(), 0);
    const size_t unit_bytes = sectors_per_unit * 512;
    for (size_t unit = first_unit; unit < weights.size(); ++unit) {
        if (!weights[unit] || (unit + 1) * sectors_per_unit > sizes.size()) continue;
        size_t packed = 0;
        for (size_t s = unit * sectors_per_unit; s < (unit + 1) * sectors_per_unit; ++s) packed += sizes[s];
        candidates.push_back({density(weights[unit], sd_cycles, Codec::raw, unit_bytes), unit, unit_bytes, true});
        if (packed < unit_bytes)
            candidates.push_back({density(weights[unit], sd_cycles, Codec::lzss, packed), unit, packed, false});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    for (const auto& c : candidates) {
        if (c.score <= 0 || selected[c.unit] || c.bytes > budget || (c.raw && c.bytes > raw_budget)) continue;
        selected[c.unit] = c.raw ? 1 : 2;
        budget -= c.bytes;
        if (c.raw) raw_budget -= c.bytes;
    }
    std::vector<double> promote(weights.size());
    for (size_t i = first_unit; i < weights.size(); ++i) if (selected[i] == 2) promote[i] = weights[i];
    for (size_t unit : raw_units(sizes, promote, sectors_per_unit, first_unit, raw_budget, budget)) selected[unit] = 1;
    Units result;
    for (size_t i = first_unit; i < weights.size(); ++i) {
        if (selected[i] == 1) result.raw.push_back(i);
        if (selected[i] == 2) result.packed.push_back(i);
    }
    return result;
}

} // namespace m0boy::rom
