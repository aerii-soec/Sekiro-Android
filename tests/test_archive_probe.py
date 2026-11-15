import struct
import unittest

from tools.archive_probe import KrakEntry, krak_entries


def header(compressed=12, decoded=32):
    data = bytearray(0x4C)
    struct.pack_into(">4s5I", data, 0, b"DCX\0", 0x11000, 0x18, 0x24, 0x44, 0x4C)
    struct.pack_into(">4sII", data, 0x18, b"DCS\0", decoded, compressed)
    struct.pack_into(">8sIB", data, 0x24, b"DCP\0KRAK", 0x20, 6)
    struct.pack_into(">4sI", data, 0x44, b"DCA\0", 8)
    return bytes(data)


class KrakHeaderTests(unittest.TestCase):
    def test_finds_multiple_headers_at_distinct_offsets(self):
        first = header() + bytes(12)
        sample = first + header(20, 48) + bytes(20)
        self.assertEqual(
            list(krak_entries(sample, len(sample))),
            [KrakEntry(0, 12, 32, 6), KrakEntry(len(first), 20, 48, 6)],
        )

    def test_accepts_prefix_when_payload_is_later_in_file(self):
        self.assertEqual(list(krak_entries(header(), 0x4C + 12)), [KrakEntry(0, 12, 32, 6)])

    def test_rejects_short_headers_and_out_of_range_payloads(self):
        self.assertEqual(list(krak_entries(header()[:-1], 1000)), [])
        self.assertEqual(list(krak_entries(header(), 0x4C + 11)), [])

    def test_rejects_other_codecs_and_bad_chunk_layout(self):
        self.assertEqual(list(krak_entries(header().replace(b"KRAK", b"DFLT"), 1000)), [])
        self.assertEqual(list(krak_entries(header().replace(b"DCA\0", b"bad!"), 1000)), [])


if __name__ == "__main__":
    unittest.main()
