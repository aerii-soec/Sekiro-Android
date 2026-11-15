#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sekiro::assets {

struct BinderMember {
    uint32_t id;
    std::u16string path;
    uint32_t offset;
    uint64_t storedBytes;
    uint64_t decodedBytes;
    bool compressed;
};

// Reads the little-endian Unicode BND4 layout observed in the Sekiro sample.
// Throws std::runtime_error for other layouts or invalid byte ranges.
std::vector<BinderMember> readBnd4(std::span<const uint8_t> bytes);

}
