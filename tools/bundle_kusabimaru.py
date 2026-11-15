"""Build a local APK asset directory for the original Kusabimaru preview."""
import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path, PureWindowsPath
import struct

from PIL import Image
from soulstruct.base.models.mtd import MTD
from soulstruct.containers import Binder

from stage_preview_assets import binder_members, open_dcx, read_archive_member


def texture_entries(tpf):
    if len(tpf) < 16 or tpf[:4] != b"TPF\0" or tpf[12] != 0 or tpf[14] != 1:
        raise ValueError("Expected a Unicode PC TPF")
    count = struct.unpack_from("<I", tpf, 8)[0]
    end_table = 16 + count * 20
    if end_table > len(tpf):
        raise ValueError("TPF entries exceed file bounds")
    entries = {}
    for i in range(count):
        start, length, _, name, _ = struct.unpack_from("<5I", tpf, 16 + i * 20)
        if not end_table <= name < start <= len(tpf) or length > len(tpf) - start:
            raise ValueError("TPF data or name exceeds file bounds")
        end = next((p for p in range(name, start, 2) if tpf[p:p + 2] == b"\0\0"), None)
        if end is None:
            raise ValueError("Unterminated TPF name")
        stem = tpf[name:end].decode("utf-16-le").lower()
        entries[stem] = tpf[start:start + length]
    return entries


def color_data(dds):
    if len(dds) < 148 or dds[:4] != b"DDS " or dds[84:88] != b"DX10":
        raise ValueError("Expected a DX10 DDS color map")
    fmt, dimension, flags, count = struct.unpack_from("<4I", dds, 128)
    if dimension != 3 or flags or count != 1 or fmt not in (72, 99):
        raise ValueError("Expected one BC1 sRGB or BC7 sRGB image")
    if fmt == 72:
        # Pillow accepts the unchanged BC1 blocks under the older DXT1 header.
        dds = dds[:84] + b"DXT1" + dds[88:128] + dds[148:]
    with Image.open(BytesIO(dds)) as image:
        if not 0 < image.width <= 4096 or not 0 < image.height <= 4096:
            raise ValueError("Base-color dimensions exceed preview limits")
        rgba = image.convert("RGBA")
        return rgba.width, rgba.height, rgba.tobytes()


def stage(game, keys, ooz, fallback, target):
    target.mkdir(parents=True, exist_ok=True)
    packed = read_archive_member(game, keys, "Data2", "/parts/wp_a_0300.partsbnd.dcx")
    sword = dict(binder_members(open_dcx(packed, ooz, target, True)))
    model = sword["WP_A_0300.flver"]
    if not model.startswith(b"FLVER\0L\0"):
        raise ValueError("The original sword FLVER was not found")
    entries = texture_entries(sword["WP_A_0300.tpf"])

    material_packed = read_archive_member(game, keys, "Data1", "/mtd/allmaterialbnd.mtdbnd.dcx")
    materials = Binder.from_bytes(open_dcx(material_packed, ooz, target, True))
    names = {"p_wp_a_0300_decal", "p_wp_a_0300_tuba", "p_wp_a_0300_blade_decal"}
    bindings = {}
    for entry in materials.entries:
        name = PureWindowsPath(entry.path).stem.lower()
        if name not in names:
            continue
        mtd = MTD.from_bytes(entry.get_uncompressed_data())
        sampler = next(item for item in mtd.samplers if "AlbedoMap" in item.sampler_type and item.sampler_path)
        if sampler.uv_index != 1:
            raise ValueError("The expected source material's primary UV channel changed")
        bindings[name] = PureWindowsPath(sampler.sampler_path).stem.lower()

    if bindings.keys() != names:
        raise ValueError("Could not resolve all source sword materials")
    stems = sorted(set(bindings.values()))
    pack = bytearray(struct.pack("<4sIII", b"SKMP", 1, len(stems), len(bindings)))
    for stem in stems:
        width, height, pixels = color_data(entries[stem])
        name = stem.encode("ascii")
        pack.extend(struct.pack("<III", len(name), width, height))
        pack.extend(name)
        pack.extend(pixels)
    for mtd, stem in sorted(bindings.items()):
        name = mtd.encode("ascii")
        pack.extend(struct.pack("<II", len(name), stems.index(stem)))
        pack.extend(name)

    backdrop = fallback.read_bytes()
    if not backdrop.startswith(b"TPF\0"):
        raise ValueError("Expected the verified Data5 texture as a fallback")
    for name, content in (("model.flver", model), ("materials.skmp", pack), ("preview.tpf", backdrop)):
        destination = target / name
        if destination.resolve().is_relative_to(game):
            raise ValueError("Staged asset would resolve into the game installation")
        destination.write_bytes(content)
        print(f"{destination.name} {len(content)} bytes sha256={hashlib.sha256(content).hexdigest()}")
    print(json.dumps(bindings, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("--keys", type=Path, required=True)
    parser.add_argument("--ooz", type=Path, required=True)
    parser.add_argument("--fallback", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Gradle asset root")
    args = parser.parse_args()
    game = args.game.resolve(strict=True)
    target = (args.output / "sekiro").resolve()
    if target.is_relative_to(game):
        parser.error("The output must be outside the read-only game installation")
    stage(game, args.keys.resolve(strict=True), args.ooz.resolve(strict=True), args.fallback, target)


if __name__ == "__main__":
    main()
