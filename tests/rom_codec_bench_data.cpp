/* Prepare independent sectors with the production encoders for the M0+
 * microbenchmark. Output is ROM-derived: only use an ignored output folder. */
#include "m0boy/rom_storage.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 1;
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    const size_t skip = std::stoull(argv[2]);
    if (skip >= bytes.size()) return 2;
    const size_t sectors = std::min<size_t>(std::stoull(argv[3]), (bytes.size() - skip) / 512);
    if (!sectors) return 2;
    const std::filesystem::path out = argv[4];
    std::filesystem::create_directories(out);
    std::ofstream raw(out / "raw.bin", std::ios::binary);
    raw.write(reinterpret_cast<const char*>(bytes.data() + skip), sectors * 512);
    for (unsigned mode = 0; mode < 2; ++mode) {
        std::vector<uint8_t> blob;
        std::vector<uint32_t> offsets;
        for (size_t s = 0; s < sectors; ++s) {
            const uint8_t* data = bytes.data() + skip + s * 512;
            auto packed = mode ? m0boy::rom::deflate_encode(data, 512) : m0boy::rom::lzss_encode(data, 512);
            offsets.push_back(blob.size());
            if (packed.size() < 512) blob.insert(blob.end(), packed.begin(), packed.end());
            else blob.insert(blob.end(), data, data + 512);
        }
        offsets.push_back(blob.size());
        const size_t payload = blob.size();
        blob.push_back(0); blob.push_back(0);
        std::ofstream h(out / (mode ? "deflate.h" : "lzss.h"));
        h << "#define SECTORS " << sectors << "\nstatic const uint32_t offsets[] = {";
        for (uint32_t offset : offsets) h << offset << ',';
        h << "};\nstatic const uint8_t blob[] = {";
        for (uint8_t byte : blob) h << unsigned(byte) << ',';
        h << "};\n";
        std::cout << (mode ? "deflate" : "lzss") << ": " << sectors * 512 << " -> " << payload
                  << " bytes, index " << offsets.size() * 4 << " bytes\n";
    }
}
