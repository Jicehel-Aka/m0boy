#include "m0boy/system.h"
#include <fstream>
namespace m0boy::vectrex {
std::optional<CartridgeInfo> inspect(const std::filesystem::path &path,
                                     std::string &error) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    error = "Cannot read Vectrex cartridge: " + path.string();
    return std::nullopt;
  }
  const auto size = in.tellg();
  if (size <= 0 || size > 32768) {
    error = "Vectrex cartridge must be 1 to 32768 bytes";
    return std::nullopt;
  }
  return CartridgeInfo{SystemId::vectrex, path.stem().string(), size_t(size), 0,
                       "flat ROM"};
}
} // namespace m0boy::vectrex
