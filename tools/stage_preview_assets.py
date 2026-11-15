"""Stage a named, local Sekiro specimen from the retail PC data archives.

This host tool reads files from the installation and writes extracted files only
to the caller's staging directory. It never opens the executable.
"""
import argparse
from pathlib import Path, PurePosixPath
import struct
import subprocess
import tempfile
import zlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.serialization import load_pem_public_key

MAX_MEMBER_BYTES = 64 * 1024 * 1024


def read_table(path, key_path):
    wrapped = path.read_bytes()
    if wrapped[:4] == b"BHD5":
        return wrapped
    key = load_pem_public_key(key_path.read_bytes()).public_numbers()
    width = (key.n.bit_length() + 7) // 8
    if len(wrapped) % width:
        raise ValueError(f"{path.name} has an incomplete index block")
    table = b"".join(
        pow(int.from_bytes(wrapped[pos:pos + width], "big"), key.e, key.n).to_bytes(width - 1, "big")
        for pos in range(0, len(wrapped), width)
    )
    if table[:5] != b"BHD5\xff":
        raise ValueError(f"{path.name} is not a supported Sekiro index")
    return table


def path_hash(name):
    value = 0
    for letter in "/" + name.lower().lstrip("/"):
        value = (value * 37 + ord(letter)) & 0xFFFFFFFF
    return value


def read_archive_member(game, keys, archive, name):
    bhd = read_table(game / f"{archive}.bhd", keys / f"{archive}.pem")
    if bhd[:5] != b"BHD5\xff":
        raise ValueError("Expected little-endian BHD5")
    buckets, offset = struct.unpack_from("<II", bhd, 16)
    if not 0 < buckets <= (len(bhd) - offset) // 8:
        raise ValueError("BHD5 bucket table exceeds index bounds")
    wanted = path_hash(name)
    entries, start = struct.unpack_from("<II", bhd, offset + (wanted % buckets) * 8)
    record = struct.Struct("<IIQqqq")
    if entries > (len(bhd) - start) // record.size:
        raise ValueError("BHD5 bucket exceeds index bounds")
    found = []
    for pos in range(start, start + entries * record.size, record.size):
        hashed, padded, location, _, aes, size = record.unpack_from(bhd, pos)
        if hashed == wanted:
            found.append((padded, location, aes, size))
    if len(found) != 1:
        raise ValueError(f"Expected one entry for {name}, found {len(found)}")
    padded, location, aes, size = found[0]
    size = size or padded
    if not 0 < size <= padded <= MAX_MEMBER_BYTES:
        raise ValueError("BHD5 entry size is outside the preview staging limit")
    with (game / f"{archive}.bdt").open("rb") as stream:
        stream.seek(location)
        packed = bytearray(stream.read(padded))
    if len(packed) != padded:
        raise ValueError("Truncated archive entry")
    if aes:
        if not 0 < aes <= len(bhd) - 20:
            raise ValueError("Invalid BHD5 range table offset")
        count = struct.unpack_from("<I", bhd, aes + 16)[0]
        if count > (len(bhd) - aes - 20) // 16:
            raise ValueError("BHD5 ranges exceed the index")
        decoder = Cipher(algorithms.AES(bhd[aes:aes + 16]), modes.ECB()).decryptor()
        for i in range(count):
            begin, end = struct.unpack_from("<qq", bhd, aes + 20 + 16 * i)
            if begin == end or begin == -1 or end == -1:
                continue
            if not 0 <= begin < end <= padded or (end - begin) % 16:
                raise ValueError("BHD5 entry range is out of bounds")
            packed[begin:end] = decoder.update(bytes(packed[begin:end]))
        decoder.finalize()
    return bytes(packed[:size])


def open_dcx(data, ooz, scratch, use_local_oodle=False):
    if not data.startswith(b"DCX\0"):
        return data
    if len(data) < 0x4C or data[0x18:0x1C] != b"DCS\0" or data[0x24:0x28] != b"DCP\0":
        raise ValueError("Unsupported DCX header")
    size, compressed = struct.unpack_from(">II", data, 0x1C)
    if not 0 < size <= MAX_MEMBER_BYTES or not 0 < compressed <= len(data) - 0x4C:
        raise ValueError("DCX size is outside the preview staging limit")
    body = data[0x4C:0x4C + compressed]
    codec = data[0x28:0x2C]
    if codec == b"DFLT":
        stream = zlib.decompressobj()
        result = stream.decompress(body, size + 1)
        if not stream.eof:
            raise ValueError("Incomplete DCX/DFLT data")
    elif codec == b"KRAK":
        with tempfile.TemporaryDirectory(dir=scratch, prefix="dcx-") as temp:
            source = Path(temp) / "input.ooz"
            output = Path(temp) / "output.bin"
            source.write_bytes(struct.pack("<Q", size) + body)
            decoded = subprocess.run([str(ooz), "-d", str(source), str(output)], cwd=temp,
                                     capture_output=True, timeout=30)
            if decoded.returncode == 0:
                result = output.read_bytes()
            elif use_local_oodle:
                from soulstruct.dcx import decompress

                result, _ = decompress(data)
            else:
                raise ValueError("ooz could not decode this DCX/KRAK block")
    else:
        raise ValueError(f"Unsupported DCX codec {codec!r}")
    if len(result) != size:
        raise ValueError("DCX decoder returned a different size")
    return result


def binder_members(data):
    if (len(data) < 64 or data[:4] != b"BND4" or data[9:11] != b"\x00\x01"
            or data[0x30:0x32] != b"\x01\x74"):
        raise ValueError("Expected little-endian Sekiro Unicode BND4")
    count = struct.unpack_from("<I", data, 12)[0]
    stride = struct.unpack_from("<Q", data, 32)[0]
    if stride != 36 or count > (len(data) - 64) // stride:
        raise ValueError("Unsupported BND4 table")
    for start in range(64, 64 + count * stride, stride):
        flags, _, stored, decoded, offset, _, name = struct.unpack_from("<IIQQIII", data, start)
        if name >= offset or offset > len(data) or stored > len(data) - offset:
            raise ValueError("BND4 member range exceeds binder")
        terminator = name
        while terminator + 2 <= offset and data[terminator:terminator + 2] != b"\x00\x00":
            terminator += 2
        if terminator + 2 > offset:
            raise ValueError("Unterminated BND4 filename")
        path = PurePosixPath(data[name:terminator].decode("utf-16-le").replace("\\", "/"))
        payload = data[offset:offset + stored]
        if flags & 0x80 and not payload.startswith(b"DCX\0"):
            stream = zlib.decompressobj()
            if not 0 < decoded <= MAX_MEMBER_BYTES:
                raise ValueError("Compressed BND4 member exceeds preview staging limit")
            payload = stream.decompress(payload, decoded + 1)
            if not stream.eof or len(payload) != decoded:
                raise ValueError("Invalid compressed BND4 member")
        yield path.name, payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("--keys", type=Path, required=True, help="Sekiro PC archive key directory")
    parser.add_argument("--ooz", type=Path, required=True, help="Host ooz executable")
    parser.add_argument("--use-local-oodle", action="store_true",
                        help="Use the installed Soulstruct Oodle decoder if ooz cannot read an entry")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--entry", action="append", required=True, metavar="ARCHIVE:PATH")
    args = parser.parse_args()
    game, output = args.game.resolve(strict=True), args.output.resolve()
    if output == game or output.is_relative_to(game):
        parser.error("The output must be outside the read-only game installation")
    ooz = args.ooz.resolve(strict=True)
    output.mkdir(parents=True, exist_ok=True)
    for entry in args.entry:
        archive, name = entry.split(":", 1)
        if archive not in {f"Data{i}" for i in range(1, 6)}:
            parser.error("Archive must be Data1 through Data5")
        target = (output / PurePosixPath(name).name.removesuffix(".dcx")).resolve()
        if target == output or not target.is_relative_to(output) or target.is_relative_to(game):
            parser.error("The staging directory must be within the requested output directory")
        target.mkdir(exist_ok=True)
        data = open_dcx(read_archive_member(game, args.keys, archive, name), ooz, target, args.use_local_oodle)
        for filename, payload in binder_members(data):
            filename = filename.removesuffix(".dcx")
            if filename.lower().endswith((".flver", ".tpf")):
                if ":" in filename or Path(filename).name != filename:
                    raise ValueError("Unsafe binder member name")
                raw = open_dcx(payload, ooz, target, args.use_local_oodle)
                destination = target / filename
                if not destination.resolve().is_relative_to(target):
                    raise ValueError("Binder member resolves outside the staging directory")
                destination.write_bytes(raw)
                print(f"{archive}:{name} -> {destination} ({len(raw)} bytes)")


if __name__ == "__main__":
    main()
