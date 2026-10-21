#include "m0boy/code_scan.h"

#include <cstring>

namespace m0boy {
namespace {

void mark(ScanResult& r, size_t from, size_t to, ScanClass c) {
    for (size_t i = from; i < to; i++) {
        r.klass[i] = c;
    }
}

} // namespace

size_t scan_course(const uint8_t* data, size_t size, const ScanAdapter& cpu,
                   size_t pc, const ScanOptions& opt, size_t* consumed) {
    if (consumed) *consumed = 0;
    if (pc >= size) return 0;
    size_t off = pc;
    size_t walked = 0;
    size_t same_byte_run = 0;
    uint8_t same_byte = 0;
    for (size_t n = 0; n < opt.max_instructions; n++) {
        ScanStep step;
        if (!cpu.step(cpu.ctx, off, size, &step)) {
            if (consumed) *consumed = walked;
            return 0;
        }
        if (step.access < 0) { /* access to a nonexistent area */
            if (consumed) *consumed = walked;
            return 0;
        }
        if (step.terminator) {
            if (n + 1 < opt.min_instructions) {
                if (consumed) *consumed = walked;
                return 0;
            }
            return off + step.length - pc;
        }
        /* four identical single-byte instructions: padding the sweep ran into */
        if (step.length == 1) {
            const uint8_t v = data[off];
            if (same_byte_run > 0 && v == same_byte && step.access <= 0) {
                same_byte_run++;
            } else {
                same_byte_run = 1;
                same_byte = v;
            }
            if (same_byte_run >= 4) {
                if (consumed) *consumed = walked;
                return 0;
            }
        } else {
            same_byte_run = 0;
        }
        off += step.length;
        walked = off - pc;
        if (off >= size) {
            if (consumed) *consumed = walked;
            return 0; /* left the region without a terminator */
        }
    }
    if (consumed) *consumed = walked;
    return 0; /* no terminator in sight: data */
}

void scan_region(const uint8_t* data, size_t size, const ScanAdapter& cpu,
                 const ScanOptions& opt, ScanResult& out) {
    ScanResult& r = out;
    r.klass.assign(size, ScanClass::Unknown);
    r.code = r.padding = r.repetitive = r.pointer = r.illogical = r.hardware_hits = 0;

    /* Padding: runs of at least 8 identical 00/FF bytes. */
    size_t i = 0;
    while (i < size) {
        const uint8_t v = data[i];
        if ((v == 0x00 || v == 0xff) && r.klass[i] == ScanClass::Unknown) {
            size_t j = i;
            while (j < size && data[j] == v) j++;
            if (j - i >= 8) {
                mark(r, i, j, ScanClass::Padding);
                i = j;
                continue;
            }
        }
        i++;
    }

    /* Repetitions: a period of 1..16 bytes repeated 3+ times (24+ bytes). */
    i = 0;
    while (i < size) {
        if (r.klass[i] != ScanClass::Unknown) { i++; continue; }
        bool marked = false;
        for (size_t p = 1; p <= 16; p++) {
            if (i + 3 * p > size) break;
            bool same = true;
            for (size_t k = 0; k < p; k++) {
                if (data[i + k] != data[i + p + k]) { same = false; break; }
            }
            if (!same) continue;
            size_t j = i + p, reps = 1;
            while (j + p <= size) {
                bool period_same = true;
                for (size_t k = 0; k < p; k++) {
                    if (data[j + k] != data[i + k]) { period_same = false; break; }
                }
                if (!period_same) break;
                reps++;
                j += p;
            }
            if (reps >= 3 && p * reps >= 24) {
                mark(r, i, j, ScanClass::Repetitive);
                i = j;
                marked = true;
                break;
            }
        }
        if (!marked) i++;
    }

    /* Pointer tables: 8+ consecutive plausible 16-bit words, three quarters
     * of them clustered in one 16 KiB window. */
    i = 0;
    while (i + 16 <= size) {
        if (r.klass[i] != ScanClass::Unknown) { i++; continue; }
        size_t j = i;
        size_t count = 0;
        uint16_t windows[64];
        size_t window_count = 0;
        while (j + 2 <= size) {
            const uint16_t v = static_cast<uint16_t>(data[j] | (data[j + 1] << 8));
            if (v < opt.pointer_lo || v >= opt.pointer_hi) break;
            const uint16_t w = v & opt.pointer_mask;
            size_t k = 0;
            while (k < window_count && windows[k] != w) k++;
            if (k == window_count && window_count < 64) windows[window_count++] = w;
            count++;
            j += 2;
        }
        bool clustered = false;
        if (count >= 8) {
            for (size_t k = 0; k < window_count; k++) {
                size_t in_window = 0;
                for (size_t t = 0; t < count; t++) {
                    const uint16_t v = static_cast<uint16_t>(data[i + 2 * t] | (data[i + 2 * t + 1] << 8));
                    if ((v & opt.pointer_mask) == windows[k]) in_window++;
                }
                if (in_window * 4 >= count * 3) { clustered = true; break; }
            }
        }
        if (clustered) {
            mark(r, i, j, ScanClass::Pointer);
            i = j;
        } else {
            i++;
        }
    }

    /* Speculative sweep: what is left must decode as a course of instructions
     * ending on a terminator, else it is data.  A course that fails early
     * retries at the next byte (misalignment); one that swallowed a long
     * stretch before failing marks it all at once. */
    i = 0;
    while (i < size) {
        if (r.klass[i] != ScanClass::Unknown) { i++; continue; }
        size_t j = i;
        while (j < size && r.klass[j] == ScanClass::Unknown) j++;
        size_t cur = i;
        while (cur < j) {
            size_t consumed = 0;
            const size_t len = scan_course(data, j, cpu, cur, opt, &consumed);
            if (len > 0) {
                mark(r, cur, cur + len, ScanClass::Code);
                cur += len;
            } else if (consumed > 8) {
                mark(r, cur, cur + consumed, ScanClass::Illogical);
                cur += consumed;
            } else {
                r.klass[cur] = ScanClass::Illogical;
                cur += 1;
            }
        }
        i = j;
    }

    for (size_t k = 0; k < size; k++) {
        switch (r.klass[k]) {
            case ScanClass::Code: r.code++; break;
            case ScanClass::Padding: r.padding++; break;
            case ScanClass::Repetitive: r.repetitive++; break;
            case ScanClass::Pointer: r.pointer++; break;
            case ScanClass::Illogical: r.illogical++; break;
            case ScanClass::Unknown: break;
        }
    }
}

std::vector<ScanZone> scan_data_zones(const ScanResult& result, size_t min_size) {
    std::vector<ScanZone> zones;
    const size_t size = result.klass.size();
    size_t i = 0;
    while (i < size) {
        if (result.klass[i] == ScanClass::Code) { i++; continue; }
        const size_t start = i;
        while (i < size && result.klass[i] != ScanClass::Code) i++;
        if (i - start < min_size) continue;
        size_t counts[5] = {0, 0, 0, 0, 0}; /* padding, repetitive, pointer, illogical, unknown */
        for (size_t k = start; k < i; k++) {
            switch (result.klass[k]) {
                case ScanClass::Padding: counts[0]++; break;
                case ScanClass::Repetitive: counts[1]++; break;
                case ScanClass::Pointer: counts[2]++; break;
                case ScanClass::Illogical: counts[3]++; break;
                case ScanClass::Unknown: counts[4]++; break;
                default: break;
            }
        }
        static const ScanClass order[5] = {
            ScanClass::Padding, ScanClass::Repetitive, ScanClass::Pointer,
            ScanClass::Illogical, ScanClass::Unknown,
        };
        size_t best = 0;
        for (size_t k = 1; k < 5; k++) {
            if (counts[k] > counts[best]) best = k;
        }
        zones.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(i), order[best]});
    }
    return zones;
}

} // namespace m0boy
