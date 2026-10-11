#pragma once

#include <cstdint>

namespace m0boy {

// A physical page identifier, independent of a console's mapping rules.
using BankId = uint16_t;
constexpr BankId UNKNOWN_BANK = UINT16_MAX;

// Decoders read bytes through the source system, never through its header or
// mapper implementation. The caller owns the reader and keeps it alive.
class ByteReader {
public:
    virtual ~ByteReader() = default;
    virtual uint8_t read_banked(BankId bank, uint16_t address) const = 0;
};

} // namespace m0boy
