"""Inspect a local Sekiro Havok 2016 TAG0 animation with Soulstruct's type-table reader."""
import argparse
import json
from pathlib import Path
import struct

from soulstruct.havok.tagfile.unpacker import TagFileUnpacker
from soulstruct.utilities.binary import BinaryReader


def chunks(data: memoryview):
    cursor = 0
    result = {}
    while cursor < len(data):
        if cursor + 8 > len(data):
            raise ValueError("Truncated TAG0 section header")
        size, name = struct.unpack_from(">I4s", data, cursor)
        size &= 0x3FFFFFFF
        if size < 8 or size > len(data) - cursor or name in result:
            raise ValueError("Invalid TAG0 section size or duplicate tag")
        result[name] = data[cursor + 8:cursor + size]
        cursor += size
    return result


def inspect(data: bytes):
    root = chunks(memoryview(data))[b"TAG0"]
    sections = chunks(root)
    tag_data = sections[b"DATA"]
    unpacker = TagFileUnpacker()
    unpacker.unpack(BinaryReader(data), types_only=True)
    if unpacker.hk_version != "20160200":
        raise ValueError(f"Unsupported Sekiro SDK version {unpacker.hk_version}")
    types = unpacker.hk_type_infos
    items = list(struct.iter_unpack("<III", chunks(sections[b"INDX"])[b"ITEM"]))
    by_name = {item.name: item for item in types[1:]}

    def fields(name):
        return {field.name: field.offset for field in by_name[name].members}

    def item(index, expected, stride):
        if not 0 < index < len(items):
            raise ValueError("Invalid TAG0 item reference")
        type_index, offset, count = items[index]
        type_index &= 0xFFFFFF
        if (not 0 < type_index < len(types) or types[type_index].name != expected
                or offset > len(tag_data) or count > (len(tag_data) - offset) // stride):
            raise ValueError(f"Invalid {expected} TAG0 item")
        return tag_data[offset:offset + stride * count], count

    def ref(record, offset):
        return struct.unpack_from("<I", record, offset)[0]

    def text(record, offset):
        value, _ = item(ref(record, offset), "char", 1)
        return bytes(value).split(b"\0", 1)[0].decode("utf-8")

    skeletons = []
    clips = []
    bindings = []
    for i, (flags, _, _) in enumerate(items):
        if flags >> 24 != 0x10:
            continue
        type_index = flags & 0xFFFFFF
        if not 0 < type_index < len(types):
            raise ValueError("Invalid TAG0 object type")
        typ = types[type_index].name
        if typ == "hkaSkeleton":
            f = fields(typ)
            record, _ = item(i, typ, types[type_index].byte_size)
            parent_data, count = item(ref(record, f["parentIndices"]), "hkInt16", 2)
            parents = [value for value, in struct.iter_unpack("<h", parent_data)]
            if any(parent < -1 or parent >= count for parent in parents):
                raise ValueError("Havok skeleton parent out of range")
            skeletons.append({"name": text(record, f["name"]), "parents": parents})
        elif typ == "hkaSplineCompressedAnimation":
            f = fields("hkaAnimation") | fields(typ)
            record, _ = item(i, typ, types[type_index].byte_size)
            _, size = item(ref(record, f["data"]), "hkUint8", 1)
            clips.append({
                "item": i,
                "seconds": struct.unpack_from("<f", record, f["duration"])[0],
                "frames": ref(record, f["numFrames"]),
                "blocks": ref(record, f["numBlocks"]),
                "transform_tracks": ref(record, f["numberOfTransformTracks"]),
                "float_tracks": ref(record, f["numberOfFloatTracks"]),
                "spline_bytes": size,
            })
        elif typ == "hkaAnimationBinding":
            f = fields(typ)
            record, _ = item(i, typ, types[type_index].byte_size)
            tracks, _ = item(ref(record, f["transformTrackToBoneIndices"]), "hkInt16", 2)
            bindings.append({
                "animation_item": ref(record, f["animation"]),
                "skeleton": text(record, f["originalSkeletonName"]),
                "track_to_bone": [value for value, in struct.iter_unpack("<h", tracks)],
            })
    return {"sdk": unpacker.hk_version, "skeletons": skeletons, "spline_clips": clips, "bindings": bindings}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--offset", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--length", type=lambda value: int(value, 0))
    args = parser.parse_args()
    maximum = 64 * 1024 * 1024
    size = args.path.stat().st_size
    length = args.length if args.length is not None else size - args.offset
    if args.offset < 0 or length <= 0 or length > maximum or args.offset + length > size:
        parser.error("Provide a TAG0 byte range within the input file, up to 64 MiB")
    with args.path.open("rb") as source:
        source.seek(args.offset)
        data = source.read(length)
    print(json.dumps(inspect(data), indent=2))


if __name__ == "__main__":
    main()
