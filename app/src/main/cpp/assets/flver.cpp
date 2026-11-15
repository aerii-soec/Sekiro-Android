#include "assets/flver.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sekiro::assets {
namespace {
constexpr size_t headerSize = 0x80;

struct Reader {
    std::span<const uint8_t> bytes;

    void range(size_t offset, size_t length) const {
        if (offset > bytes.size() || length > bytes.size() - offset) {
            throw std::runtime_error("FLVER field exceeds its section");
        }
    }

    uint32_t integer(size_t offset, size_t width = 4) const {
        range(offset, width);
        uint32_t value = 0;
        for (size_t i = 0; i < width; ++i) {
            value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
        }
        return value;
    }

    int16_t i16(size_t offset) const { return std::bit_cast<int16_t>(static_cast<uint16_t>(integer(offset, 2))); }

    float real(size_t offset) const {
        const auto value = std::bit_cast<float>(integer(offset));
        if (!std::isfinite(value) || std::abs(value) > 1e8f) {
            throw std::runtime_error("FLVER float outside supported range");
        }
        return value;
    }

    std::array<float, 3> vec3(size_t offset) const {
        return {real(offset), real(offset + 4), real(offset + 8)};
    }

    std::u16string name(size_t offset) const {
        if (offset < headerSize || offset % 2 != 0) {
            throw std::runtime_error("Invalid FLVER name offset");
        }
        std::u16string value;
        for (size_t i = 0; i < 1024; ++i) {
            auto unit = static_cast<char16_t>(integer(offset + i * 2, 2));
            if (unit == 0) {
                return value;
            }
            value.push_back(unit);
        }
        throw std::runtime_error("Unterminated FLVER name");
    }
};

struct Table {
    size_t start;
    uint32_t count;
    size_t stride;

    size_t at(uint32_t index) const {
        if (index >= count) {
            throw std::runtime_error("FLVER table index out of range");
        }
        return start + index * stride;
    }
};

Table table(const Reader& meta, size_t& cursor, size_t countField, size_t stride) {
    const auto count = meta.integer(countField);
    if (count > 500'000 || cursor > meta.bytes.size() || count > (meta.bytes.size() - cursor) / stride) {
        throw std::runtime_error("Invalid FLVER table bounds");
    }
    const Table result{cursor, count, stride};
    cursor += stride * count;
    return result;
}

void readMaterials(FlverModel& model, const Reader& meta, Table materials, Table textures) {
    for (uint32_t i = 0; i < materials.count; ++i) {
        const auto record = materials.at(i);
        auto& output = model.materials.emplace_back();
        output.name = meta.name(meta.integer(record));
        output.mtd = meta.name(meta.integer(record + 4));
        const auto count = meta.integer(record + 8);
        const auto first = meta.integer(record + 12);
        if (count == 0) {
            continue;
        }
        if (first >= textures.count || count > textures.count - first) {
            throw std::runtime_error("FLVER material texture range is out of bounds");
        }
        for (uint32_t t = first; t < first + count; ++t) {
            const auto item = textures.at(t);
            const auto type = assetStem(meta.name(meta.integer(item + 4)));
            if (type == u"g_diffusetexture" || type == u"g_diffusetexture_0" ||
                type == u"g_albedotexture" || type == u"g_albedotexture_0") {
                output.diffuse = meta.name(meta.integer(item));
                output.diffuseScale = {meta.real(item + 8), meta.real(item + 12)};
                break;
            }
        }
    }
}

void readBones(FlverModel& model, const Reader& meta, Table bones) {
    if (bones.count > 32'768) {
        throw std::runtime_error("Too many FLVER bones");
    }
    for (uint32_t b = 0; b < bones.count; ++b) {
        const auto row = bones.at(b);
        const auto parent = meta.i16(row + 28);
        if (parent < -1 || parent >= static_cast<int32_t>(bones.count)) {
            throw std::runtime_error("Invalid FLVER parent bone index");
        }
        model.bones.push_back({meta.name(meta.integer(row + 12)), meta.vec3(row), meta.vec3(row + 16),
                               meta.vec3(row + 32), parent});
    }
    // Validate the parent forest, including forward references and disconnected roots.
    std::vector<uint8_t> visit(bones.count);
    for (uint32_t b = 0; b < bones.count; ++b) {
        int32_t node = b;
        while (node != -1 && visit[node] == 0) {
            visit[node] = 1;
            node = model.bones[node].parent;
        }
        if (node != -1 && visit[node] == 1) {
            throw std::runtime_error("Cyclic FLVER bone hierarchy");
        }
        node = b;
        while (node != -1 && visit[node] == 1) {
            visit[node] = 2;
            node = model.bones[node].parent;
        }
    }
}

struct Attribute {
    uint32_t offset;
    uint32_t type;
    uint32_t semantic;
};

size_t attributeWidth(const Attribute& attribute) {
    switch (attribute.type) {
    case 1: return 8;
    case 2: return 12;
    case 3: return 16;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x15: case 0x2F: return 4;
    case 0x16: case 0x18: case 0x1A: case 0x2E: return 8;
    default: throw std::runtime_error("Unsupported FLVER vertex attribute type");
    }
}

void readAttribute(FlverVertex& vertex, const Reader& data, size_t offset, const Attribute& attr) {
    if (attr.semantic == 0) {
        if (attr.type != 2 && attr.type != 3) {
            throw std::runtime_error("Unsupported FLVER position format");
        }
        vertex.position = data.vec3(offset);
    } else if (attr.semantic == 3) {
        if (attr.type == 2 || attr.type == 3) {
            vertex.normal = data.vec3(offset);
        } else if (attr.type == 0x10 || attr.type == 0x11 || attr.type == 0x13 || attr.type == 0x2F) {
            for (size_t i = 0; i < 3; ++i) {
                vertex.normal[i] = (static_cast<float>(data.integer(offset + i, 1)) - 127.0f) / 127.0f;
            }
        } else if (attr.type == 0x1A) {
            for (size_t i = 0; i < 3; ++i) {
                vertex.normal[i] = data.i16(offset + i * 2) / 32767.0f;
            }
        } else if (attr.type == 0x2E) {
            for (size_t i = 0; i < 3; ++i) {
                vertex.normal[i] = (static_cast<float>(data.integer(offset + i * 2, 2)) - 32767.0f) / 32767.0f;
            }
        } else if (attr.type == 0x12) {
            for (size_t i = 0; i < 3; ++i) {
                const auto channel = static_cast<uint8_t>(data.integer(offset + 3 - i, 1));
                vertex.normal[i] = std::bit_cast<int8_t>(channel) / 127.0f;
            }
        } else {
            throw std::runtime_error("Unsupported FLVER normal format");
        }
    } else if (attr.semantic == 5) {
        if (attr.type >= 1 && attr.type <= 3) {
            vertex.uv = {data.real(offset), data.real(offset + 4)};
        } else if ((attr.type >= 0x10 && attr.type <= 0x16 && attr.type != 0x14) || attr.type == 0x2E) {
            vertex.uv = {data.i16(offset) / 2048.0f, data.i16(offset + 2) / 2048.0f};
        } else {
            throw std::runtime_error("Unsupported FLVER UV format");
        }
    }
}

void readFaces(FlverModel& model, const Reader& meta, const Reader& data, Table faceSets,
               size_t mesh, uint32_t vertexBase, uint32_t vertices) {
    const auto count = meta.integer(mesh + 0x20);
    const auto list = meta.integer(mesh + 0x24);
    meta.range(list, static_cast<size_t>(count) * 4);
    if (count > faceSets.count) {
        throw std::runtime_error("Invalid FLVER faceset count for mesh");
    }
    for (uint32_t set = 0; set < count; ++set) {
        const auto row = faceSets.at(meta.integer(list + set * 4));
        const auto flags = meta.integer(row);
        if (flags & 0x83000000u) {
            continue; // Lower LOD and motion-blur copies.
        }
        const bool strip = meta.integer(row + 4, 1) != 0;
        const auto total = meta.integer(row + 8);
        const auto offset = meta.integer(row + 12);
        auto indexBits = meta.integer(row + 24);
        if (indexBits == 0) {
            indexBits = meta.integer(0x48, 1);
        }
        if ((indexBits != 16 && indexBits != 32) || (flags & 0x40000000u) ||
            (!strip && total % 3 != 0) || total > 4'000'000 ||
            model.indices.size() + static_cast<size_t>(total) * 3 > 12'000'000) {
            throw std::runtime_error("Unsupported FLVER face index stream");
        }
        const auto width = indexBits / 8;
        data.range(offset, static_cast<size_t>(total) * width);
        const auto restart = indexBits == 16 ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t a = 0, b = 0, run = 0;
        for (uint32_t i = 0; i < total; ++i) {
            auto c = data.integer(offset + static_cast<size_t>(i) * width, width);
            if (strip && c == restart && restart >= vertices) {
                run = 0;
                continue;
            }
            if (c >= vertices) {
                throw std::runtime_error("FLVER face index points past its mesh vertices");
            }
            if (run >= 2 && a != b && a != c && b != c) {
                const bool flip = strip && (run & 1u);
                model.indices.insert(model.indices.end(), {vertexBase + (flip ? b : a),
                    vertexBase + (flip ? a : b), vertexBase + c});
            }
            a = b;
            b = c;
            run = strip ? run + 1 : (run + 1) % 3;
        }
    }
}

void readMesh(FlverModel& model, const Reader& meta, const Reader& data, Table streams,
              Table layouts, Table faceSets, size_t row) {
    const auto firstVertex = static_cast<uint32_t>(model.vertices.size());
    const auto firstIndex = static_cast<uint32_t>(model.indices.size());
    const auto material = meta.integer(row + 4);
    const auto count = meta.integer(row + 40);
    const auto list = meta.integer(row + 44);
    if (material >= model.materials.size() || count < 1 || count > 3) {
        throw std::runtime_error("Invalid FLVER mesh material or stream count");
    }
    meta.range(list, count * 4);
    uint32_t vertices = 0;
    bool foundPosition = false;
    bool foundNormal = false;
    bool foundUv = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto stream = streams.at(meta.integer(list + i * 4));
        const auto layout = layouts.at(meta.integer(stream + 4));
        const auto stride = meta.integer(stream + 8);
        const auto countInStream = meta.integer(stream + 12);
        const auto offset = meta.integer(stream + 28);
        if (stride == 0 || stride > 512 || countInStream == 0 ||
            firstVertex + static_cast<size_t>(countInStream) > 1'500'000) {
            throw std::runtime_error("Invalid FLVER vertex count or stride");
        }
        if (i == 0) {
            vertices = countInStream;
            model.vertices.resize(firstVertex + vertices);
        } else if (vertices != countInStream) {
            throw std::runtime_error("FLVER vertex streams disagree on vertex count");
        }
        data.range(offset, static_cast<size_t>(vertices) * stride);
        const auto members = meta.integer(layout);
        const auto memberList = meta.integer(layout + 12);
        if (members == 0 || members > 64) {
            throw std::runtime_error("Unsupported FLVER vertex layout size");
        }
        for (uint32_t m = 0; m < members; ++m) {
            const auto member = memberList + static_cast<size_t>(m) * 20;
            const Attribute attr{meta.integer(member + 4), meta.integer(member + 8),
                                 meta.integer(member + 12)};
            if (attr.offset >= stride || attributeWidth(attr) > stride - attr.offset) {
                throw std::runtime_error("FLVER vertex attribute exceeds stride");
            }
            const bool position = attr.semantic == 0 && !foundPosition;
            const bool normal = attr.semantic == 3 && !foundNormal;
            const bool uv = attr.semantic == 5 && meta.integer(member + 16) == 0 && !foundUv;
            if (position || normal || uv) {
                for (uint32_t v = 0; v < vertices; ++v) {
                    readAttribute(model.vertices[firstVertex + v], data, offset + v * stride + attr.offset, attr);
                }
                foundPosition |= position;
                foundNormal |= normal;
                foundUv |= uv;
            }
        }
    }
    if (!foundPosition || !foundNormal) {
        throw std::runtime_error("FLVER mesh lacks supported positions or normals");
    }
    readFaces(model, meta, data, faceSets, row, firstVertex, vertices);
    const auto outputCount = static_cast<uint32_t>(model.indices.size()) - firstIndex;
    if (outputCount) {
        model.surfaces.push_back({firstIndex, outputCount, material, foundUv});
    }
}
}

std::u16string assetStem(std::u16string_view name) {
    const auto end = name.find_last_of(u"/\\");
    if (end != std::u16string_view::npos) {
        name.remove_prefix(end + 1);
    }
    name = name.substr(0, name.find_last_of(u'.'));
    std::u16string stem(name);
    for (auto& ch : stem) {
        if (ch >= u'A' && ch <= u'Z') {
            ch = static_cast<char16_t>(ch + (u'a' - u'A'));
        }
    }
    return stem;
}

FlverModel readFlver2(std::span<const uint8_t> bytes) {
    constexpr uint8_t magic[]{'F', 'L', 'V', 'E', 'R', 0, 'L', 0};
    if (bytes.size() < headerSize || !std::equal(std::begin(magic), std::end(magic), bytes.begin())) {
        throw std::runtime_error("Expected a little-endian FLVER2 file");
    }
    const Reader input{bytes};
    const auto version = input.integer(8);
    if ((version != 0x2001A && version != 0x20016) || input.integer(0x49, 1) != 1) {
        throw std::runtime_error("Unsupported FLVER2 version or string encoding");
    }
    const auto dataOffset = input.integer(12);
    const auto dataSize = input.integer(16);
    input.range(dataOffset, dataSize);
    const Reader meta{bytes.first(dataOffset)};
    const Reader data{bytes.subspan(dataOffset, dataSize)};
    size_t cursor = headerSize;
    (void)table(meta, cursor, 0x14, 0x40); // Dummies.
    const auto materials = table(meta, cursor, 0x18, 0x20);
    const auto bones = table(meta, cursor, 0x1C, 0x80);
    const auto meshes = table(meta, cursor, 0x20, 0x30);
    const auto faceSets = table(meta, cursor, 0x50, 0x20);
    const auto vertexStreams = table(meta, cursor, 0x24, 0x20);
    const auto layouts = table(meta, cursor, 0x54, 0x10);
    const auto textures = table(meta, cursor, 0x58, 0x20);
    FlverModel model;
    readMaterials(model, meta, materials, textures);
    readBones(model, meta, bones);
    for (uint32_t i = 0; i < meshes.count; ++i) {
        readMesh(model, meta, data, vertexStreams, layouts, faceSets, meshes.at(i));
    }
    if (model.indices.empty()) {
        throw std::runtime_error("FLVER contains no renderable base-LOD triangles");
    }
    model.minimum.fill(std::numeric_limits<float>::infinity());
    model.maximum.fill(-std::numeric_limits<float>::infinity());
    for (uint32_t index : model.indices) {
        for (size_t axis = 0; axis < 3; ++axis) {
            const auto coordinate = model.vertices[index].position[axis];
            model.minimum[axis] = std::min(model.minimum[axis], coordinate);
            model.maximum[axis] = std::max(model.maximum[axis], coordinate);
        }
    }
    return model;
}
}
