#ifndef RECOMPILER_CODEGEN_REGISTER_IR_H
#define RECOMPILER_CODEGEN_REGISTER_IR_H

#include <array>
#include <cstdint>
#include <string>

namespace gbrecomp {

/* Symbolic byte values: either an entry register or a constant. Copies share
 * a value; replacing a destination removes dead writes without a pattern table.
 * No memory, flags, timing or control effects are admitted into this IR. */
class RegisterIR {
public:
    struct Value {
        int source;
        uint8_t constant = 0;
        bool operator==(const Value&) const = default;
    };
    RegisterIR();
    bool append(uint8_t op, uint8_t n8 = 0, uint16_t n16 = 0);
    const std::array<Value, 8>& values() const { return values_; }
    /* Thumb-1 on ARMv6-M, equivalent snapshot-based C on host. */
    std::string emit() const;
private:
    std::array<Value, 8> values_;
};

} // namespace gbrecomp
#endif
