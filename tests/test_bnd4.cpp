#include "assets/bnd4.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void write(std::vector<uint8_t>& bytes, size_t position, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        bytes.at(position + i) = static_cast<uint8_t>(value >> (8 * i));
    }
}

std::vector<uint8_t> fixture() {
    std::vector<uint8_t> bytes(0x83);
    std::copy_n("BND4", 4, bytes.begin());
    bytes[0x0A] = 1;
    write(bytes, 0x0C, 1, 4);
    write(bytes, 0x10, 0x40, 8);
    write(bytes, 0x20, 0x24, 8);
    write(bytes, 0x28, 0x80, 8);
    bytes[0x30] = 1;
    bytes[0x31] = 0x74;
    bytes[0x32] = 4;
    write(bytes, 0x38, 0x78, 8);
    bytes[0x40] = 0x40;
    write(bytes, 0x44, UINT32_MAX, 4);
    write(bytes, 0x48, 3, 8);
    write(bytes, 0x50, 3, 8);
    write(bytes, 0x58, 0x80, 4);
    write(bytes, 0x5C, 7, 4);
    write(bytes, 0x60, 0x64, 4);
    write(bytes, 0x64, 'A', 2);
    return bytes;
}

void reject(const std::vector<uint8_t>& bytes) {
    try {
        (void)sekiro::assets::readBnd4(bytes);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("Malformed binder was accepted");
}

void runTests() {
    const auto bytes = fixture();
    const auto members = sekiro::assets::readBnd4(bytes);
    assert(members.size() == 1);
    const auto& member = members.front();
    assert(member.id == 7 && member.path == u"A" && member.offset == 0x80);
    assert(member.storedBytes == 3 && member.decodedBytes == 3 && !member.compressed);

    auto truncated = bytes;
    truncated.pop_back();
    reject(truncated);
    auto invalidName = bytes;
    write(invalidName, 0x60, 0x78, 4);
    reject(invalidName);
    auto invalidCount = bytes;
    write(invalidCount, 0x0C, UINT32_MAX, 4);
    reject(invalidCount);
    auto unterminated = bytes;
    std::fill(unterminated.begin() + 0x64, unterminated.begin() + 0x78, 0xFF);
    reject(unterminated);
}

void inspect(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open binder");
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    for (const auto& member : sekiro::assets::readBnd4(bytes)) {
        const auto position = member.path.find_last_of(u"\\/");
        const auto name = member.path.substr(position == std::u16string::npos ? 0 : position + 1);
        const auto payload = std::span(bytes).subspan(member.offset, member.storedBytes);
        std::cout << member.id << ' ' << std::string(name.begin(), name.end()) << ' '
                  << payload.size() << " bytes\n";
    }
}
}

int main(int argc, char** argv) {
    runTests();
    std::cout << "BND4 fixture and four malformed cases passed\n";
    if (argc == 2) {
        inspect(argv[1]);
    }
}
