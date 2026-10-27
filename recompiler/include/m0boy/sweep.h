/* Speculative code seeding shared by the emitters (--pce-sweep,
 * --nes-sweep, --vectrex-sweep, --megadrive-sweep, --meta-sweep): the
 * cartridge image is swept for the CPU's call/jump opcodes and their
 * targets seed the static walk, so code reachability cannot prove
 * (jump-table handlers, callers behind a bank table) compiles like the
 * rest.  Nothing runs there until the game jumps: an entry is entered
 * only under its own context (the MPR bytes, the bank) and the compiled
 * semantics stay the interpreter's — but the generated file grows.  The
 * discipline, one shape in every emitter:
 *
 *   1. the played code and the vectors seed the walk, which expands
 *      without a cap (a coverage walk is never truncated by the sweep);
 *   2. then the sweep's targets seed a second queue, taking from a
 *      Sweep budget: what the code budget has left (sweep_room), or
 *      kSweepDefault instructions when there is none;
 *   3. that queue expands under the same budget, so the speculative
 *      instances stay bounded whatever the card hides in data;
 *   4. every kept instance ranks by visit order — the swept ones after
 *      the played code — and a budget cuts them first.
 *
 * The Z80 emitter has its own, older scan (code_scan.h) driven by the
 * data-zone inference; this header is the bounded walk-seeding kind. */
#ifndef M0BOY_SWEEP_H
#define M0BOY_SWEEP_H

#include <cstddef>

namespace m0 {

/* the speculative instances when no --meta-code-budget bounds them */
constexpr size_t kSweepDefault = 16384;

inline size_t sweep_room(size_t covered, size_t max_instructions) {
    if (!max_instructions) return kSweepDefault;
    return max_instructions > covered ? max_instructions - covered : 0;
}

/* the sweep's budget: one take() per speculative instance inserted */
class Sweep {
public:
    explicit Sweep(size_t room) : room_(room) {}
    bool take() {
        if (used_ >= room_) return false;
        ++used_;
        return true;
    }

private:
    size_t room_, used_ = 0;
};

} // namespace m0

#endif
