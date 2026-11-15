#include "assets/bnd4.h"

#include <algorithm>
#include <stdexcept>

namespace sekiro::assets {
namespace {
constexpr size_t headerSize = 0x40;
constexpr size_t memberSize = 0x24;

uint64_t littleEndian(std::span<const uint8_t> bytes, size_t position, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        value |= static_cast<uint64_t>(bytes[position + i]) << (i * 8);
    }
    return value;
}

std::u16string readPath(std::span<const uint8_t> bytes, size_t position, size_t limit) {
    std::u16string path;
    while (position <= limit && limit - position >= 2) {
        auto unit = static_cast<char16_t>(littleEndian(bytes, position, 2));
        if (unit == u'\0' && !path.empty()) {
            return path;
        }
        if (unit == u'\0') {
            break;
        }
        path.push_back(unit);
        position += 2;
    }
    throw std::runtime_error("Invalid BND4 member path");
}
}

std::vector<BinderMember> readBnd4(std::span<const uint8_t> bytes) {
    if (bytes.size() < headerSize || !std::equal(bytes.begin(), bytes.begin() + 4, "BND4")) {
        throw std::runtime_error("Missing BND4 header");
    }
    if (bytes[9] != 0 || bytes[10] != 1 || bytes[0x30] != 1 || bytes[0x31] != 0x74 ||
        bytes[0x32] != 4 || littleEndian(bytes, 0x10, 8) != headerSize ||
        littleEndian(bytes, 0x20, 8) != memberSize) {
        throw std::runtime_error("Unsupported BND4 header layout");
    }

    const auto count = littleEndian(bytes, 0x0C, 4);
    const auto dataStart = littleEndian(bytes, 0x28, 8);
    const auto hashTable = littleEndian(bytes, 0x38, 8);
    if (count > (bytes.size() - headerSize) / memberSize) {
        throw std::runtime_error("BND4 entry table extends past end of file");
    }
    const auto namesStart = headerSize + static_cast<size_t>(count) * memberSize;
    if (hashTable < namesStart || hashTable >= dataStart || dataStart > bytes.size()) {
        throw std::runtime_error("Invalid BND4 header ranges");
    }

    std::vector<BinderMember> members;
    members.reserve(static_cast<size_t>(count));
    for (size_t index = 0; index < count; ++index) {
        const size_t record = headerSize + index * memberSize;
        const auto flags = littleEndian(bytes, record, 4);
        if ((flags != 0x40 && flags != 0xC0) || littleEndian(bytes, record + 4, 4) != UINT32_MAX) {
            throw std::runtime_error("Unsupported BND4 member flags");
        }
        const bool compressed = (flags & 0x80) != 0;
        const auto stored = littleEndian(bytes, record + 8, 8);
        const auto decoded = littleEndian(bytes, record + 0x10, 8);
        const auto offset = static_cast<uint32_t>(littleEndian(bytes, record + 0x18, 4));
        const auto id = static_cast<uint32_t>(littleEndian(bytes, record + 0x1C, 4));
        const auto filename = static_cast<uint32_t>(littleEndian(bytes, record + 0x20, 4));
        if (offset < dataStart || offset > bytes.size() || stored > bytes.size() - offset ||
            (!compressed && stored != decoded)) {
            throw std::runtime_error("Invalid BND4 member data range");
        }
        if (filename < namesStart || filename >= hashTable || filename % 2 != 0) {
            throw std::runtime_error("Invalid BND4 member name offset");
        }
        members.push_back({id, readPath(bytes, filename, static_cast<size_t>(hashTable)),
                           offset, stored, decoded, compressed});
    }
    return members;
}
}
