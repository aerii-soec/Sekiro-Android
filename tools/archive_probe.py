"""Inventory a Sekiro directory and identify bounded, plaintext DCX/KRAK samples."""
import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

SCAN_BYTES = 8 * 1024 * 1024
MAX_SAMPLE_BYTES = 64 * 1024 * 1024
DCX_HEADER_BYTES = 0x4C
ARCHIVE_NAMES = tuple(f"Data{index}" for index in range(1, 6))


@dataclass(frozen=True)
class KrakEntry:
    offset: int
    compressed_bytes: int
    decoded_bytes: int
    level: int


def krak_entries(prefix: bytes, file_size: int):
    """Yield candidate headers whose size fields fit the underlying file."""
    offset = prefix.find(b"DCX\0")
    while offset != -1:
        if len(prefix) - offset >= DCX_HEADER_BYTES:
            version, dcs, dcp, dca, payload = struct.unpack_from(">5I", prefix, offset + 4)
            if (version, dcs, dcp, dca, payload) == (0x11000, 0x18, 0x24, 0x44, 0x4C):
                header = prefix[offset:offset + DCX_HEADER_BYTES]
                if (header[dcs:dcs+4] == b"DCS\0"
                        and header[dcp:dcp+8] == b"DCP\0KRAK"
                        and header[0x2C:0x30] == b"\0\0\0\x20"
                        and header[dca:dca+8] == b"DCA\0\0\0\0\x08"
                        and header[0x30] in (6, 9)):
                    decoded, compressed = struct.unpack_from(">II", header, dcs + 4)
                    if decoded and 0 < compressed <= file_size - offset - payload:
                        yield KrakEntry(offset, compressed, decoded, header[0x30])
        offset = prefix.find(b"DCX\0", offset + 4)


def fingerprint(path: Path, *, hash_contents: bool):
    with path.open("rb") as stream:
        prefix = stream.read(16)
        record = {"bytes": path.stat().st_size, "prefix_hex": prefix.hex()}
        if hash_contents:
            stream.seek(0)
            record["sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        return record


def audit(game: Path):
    archives = {}
    for name in ARCHIVE_NAMES:
        bhd = fingerprint(game / f"{name}.bhd", hash_contents=True)
        bhd["plaintext_bhd5"] = bhd["prefix_hex"].startswith(b"BHD5".hex())
        bdt = game / f"{name}.bdt"
        size = bdt.stat().st_size
        with bdt.open("rb") as stream:
            prefix = stream.read(SCAN_BYTES)
        entries = list(krak_entries(prefix, size))
        archives[name] = {
            "bhd": bhd,
            "bdt_bytes": size,
            "bdt_prefix_hex": prefix[:16].hex(),
            "scan_bytes": len(prefix),
            "krak_header_count": len(entries),
            "first_krak_headers": [asdict(entry) for entry in entries[:5]],
        }

    binaries = {
        path.name: fingerprint(path, hash_contents=True)
        for path in sorted(game.iterdir())
        if path.is_file() and path.suffix.lower() in {".exe", ".dll", ".org", ".rne"}
    }
    middleware_assets = {
        folder: {
            extension: len(list((game / folder).glob(f"*.{extension}")))
            for extension in extensions
        }
        for folder, extensions in {"sound": ("fev", "fsb"), "movie": ("bk2",)}.items()
    }
    return {"archives": archives, "binaries": binaries, "middleware_assets": middleware_assets}


def verify_sample(game: Path, decoder: Path, scratch: Path, archive: str, offset: int,
                  expected_magic: bytes, output: Path | None):
    """Verify a readable DCX/KRAK entry using the independently supplied ooz CLI."""
    source = game / archive
    with source.open("rb") as stream:
        stream.seek(offset)
        prefix = stream.read(DCX_HEADER_BYTES)
        entry = next(krak_entries(prefix, source.stat().st_size - offset), None)
        if (entry is None or entry.compressed_bytes > MAX_SAMPLE_BYTES
                or entry.decoded_bytes > MAX_SAMPLE_BYTES):
            raise ValueError(f"No supported, bounded DCX/KRAK sample at {archive} offset {offset:#x}")
        payload = stream.read(entry.compressed_bytes)
    if len(payload) != entry.compressed_bytes:
        raise ValueError("Truncated compressed sample")

    with tempfile.TemporaryDirectory(prefix="ooz-", dir=scratch) as work:
        workspace = Path(work)
        packed, unpacked = workspace / "sample.ooz", workspace / "sample.bin"
        packed.write_bytes(struct.pack("<Q", entry.decoded_bytes) + payload)
        subprocess.run(
            [str(decoder.resolve(strict=True)), "-d", str(packed), str(unpacked)],
            cwd=workspace, capture_output=True, check=True, timeout=30,
        )
        result = fingerprint(unpacked, hash_contents=True)
        if result["bytes"] != entry.decoded_bytes or not result["prefix_hex"].startswith(expected_magic.hex()):
            raise ValueError("Decoded sample differs from the expected format or size")
        if output is not None:
            unpacked.replace(output)
    return {"archive": archive, **asdict(entry), "offset": offset, "decoded": result}



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path)
    parser.add_argument("--ooz", type=Path, help="Independently verify the known BND4 and TPF samples")
    parser.add_argument("--stage-samples", action="store_true",
                        help="Keep the two decoded samples in the local generated reports directory")
    args = parser.parse_args()
    if args.stage_samples and not args.ooz:
        parser.error("--stage-samples requires --ooz")

    game = args.game.resolve(strict=True)
    report = audit(game)
    if args.ooz:
        scratch = Path(__file__).resolve().parents[1] / "reverse-engineering/reports/generated"
        scratch.mkdir(parents=True, exist_ok=True)
        samples = (
            ("Data1.bdt", 0, b"BND4", "inspection.bnd"),
            ("Data5.bdt", 0x3EF830, b"TPF\0", "inspection.tpf"),
        )
        report["decoded_samples"] = [
            verify_sample(game, args.ooz, scratch, archive, offset, magic,
                          scratch / name if args.stage_samples else None)
            for archive, offset, magic, name in samples
        ]
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
