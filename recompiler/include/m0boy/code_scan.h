#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace m0boy {

/* Speculative code/data sweep shared by the source consoles.  The driver
 * below walks raw bytes; the CPU knowledge (instruction lengths, unconditional
 * terminators, accesses to known hardware) stays with each system through the
 * ScanAdapter.  A byte is code when a well-formed course of instructions runs
 * from it to an unconditional terminator without an illegal opcode or an
 * access to a nonexistent area; anything else piles up into data zones
 * (padding, repetitions, pointer tables, or undecodable bytes).
 *
 * The classification is conservative by design: a byte wrongly left as data
 * only loses compiled speed (the runtimes interpret uncovered code), while a
 * byte wrongly taken as code wastes flash and misleads the analyzer. */

enum class ScanClass : uint8_t {
    Unknown,    ///< not yet explained
    Code,       ///< inside a validated course of instructions
    Padding,    ///< 8+ identical 00/FF bytes
    Repetitive, ///< a period of 1..16 bytes repeated 3+ times (24+ bytes)
    Pointer,    ///< 8+ consecutive plausible 16-bit pointers, clustered
    Illogical,  ///< decodes badly or never reaches a terminator
};

struct ScanZone {
    uint32_t start = 0;          ///< index of the first byte
    uint32_t end = 0;            ///< index past the last byte
    ScanClass dominant = ScanClass::Illogical;
};

/* One decoded instruction, from the adapter's point of view. */
struct ScanStep {
    size_t length = 0;       ///< 0: invalid instruction at pc
    bool terminator = false; ///< unconditional RET/JP/...
    int access = 0;          ///< >0: known hardware; <0: nonexistent area
};

struct ScanAdapter {
    void* ctx = nullptr;
    /* Decode the instruction at pc; false leaves the course rejected.  The
     * instruction must fit before end (no straddling the region). */
    bool (*step)(const void* ctx, size_t pc, size_t end, ScanStep* out);
};

struct ScanOptions {
    size_t max_instructions = 400; ///< a course with no terminator by then is data
    size_t min_instructions = 1;   ///< 2 rejects single-instruction courses (lone RET in data)
    /* Plausible pointer targets of the console (16-bit words of data space) */
    uint16_t pointer_lo = 0x0000;
    uint16_t pointer_hi = 0xc000;
    uint16_t pointer_mask = 0xc000; ///< clustering window (16 KiB)
};

struct ScanResult {
    std::vector<ScanClass> klass; ///< per byte of the scanned region
    size_t code = 0;              ///< bytes of each class, after scan_region
    size_t padding = 0;
    size_t repetitive = 0;
    size_t pointer = 0;
    size_t illogical = 0;
    size_t hardware_hits = 0;     ///< known-hardware accesses inside code courses
};

/* One course from pc: 0 = rejected, else the accepted length.  *consumed (if
 * non-null) receives the bytes walked before a rejection, so callers can skip
 * a long illegal stretch at once. */
size_t scan_course(const uint8_t* data, size_t size, const ScanAdapter& cpu,
                   size_t pc, const ScanOptions& opt, size_t* consumed = nullptr);

/* Classify a whole region: padding, repetitions and pointer tables first,
 * then the speculative sweep of what is left. */
void scan_region(const uint8_t* data, size_t size, const ScanAdapter& cpu,
                 const ScanOptions& opt, ScanResult& out);

/* Merge everything not Code into zones of at least min_size bytes, each with
 * its dominant class.  Zones are returned by start address. */
std::vector<ScanZone> scan_data_zones(const ScanResult& result, size_t min_size);

} // namespace m0boy
