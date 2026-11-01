"""Offline audit must reject malformed ELF metadata without reading past bounds."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("linux_client_revalidation", ROOT / "tools/revalidate_linux_client.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def fixture():
    data = bytearray(0x400)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, 62, 1, 0, 64, 0x100, 0, 64, 56, 2, 64, 2, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, len(data), len(data), 0x1000)
    struct.pack_into("<IIQQQQQQ", data, 120, 4, 4, 0x200, 0x200, 0x200, 36, 36, 4)
    struct.pack_into("<IIQQQQIIQQ", data, 0x140, 0, 4, 0, 0, 0x280, 24, 0, 0, 8, 24)
    struct.pack_into("<III", data, 0x200, 4, 20, 3)
    data[0x20c:0x210] = b"GNU\0"
    data[0x210:0x224] = bytes.fromhex(audit.BUILD_ID)
    struct.pack_into("<QQq", data, 0x280, 0x300, 8, 0x320)
    struct.pack_into("<Q", data, 0x300, 0x320)
    data[0x320:0x325] = b"test\0"
    return data


class LinuxClientAuditTests(unittest.TestCase):
    def test_valid_elf_pointer_has_matching_relocation(self):
        elf = audit.Elf64(bytes(fixture()))
        self.assertEqual(elf.build_id(), audit.BUILD_ID)
        elf.pointer(0x300, 0x320)
        elf.text(0x320, "test")
        self.assertEqual(elf.read(0x300, 8, True), struct.pack("<Q", 0x320))

    def test_truncated_header_and_tables_fail_closed(self):
        for size in [0, 7, 63, 100, 175, 0x180, 0x220, 0x290]:
            with self.subTest(size=size), self.assertRaises(audit.ValidationError):
                audit.Elf64(bytes(fixture()[:size]))
        for offset, fmt, value in [(32, "<Q", 0xffffffffff), (54, "<H", 1),
                                   (40, "<Q", 0xffffffffff), (58, "<H", 1),
                                   (64 + 32, "<Q", 0xffffffff), (0x140 + 56, "<Q", 0)]:
            data = fixture();struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(audit.ValidationError):
                audit.Elf64(bytes(data))

    def test_bad_note_and_unmapped_or_nonexecuting_ranges_fail(self):
        data = fixture();struct.pack_into("<I", data, 0x204, 0xffffffff)
        with self.assertRaises(audit.ValidationError):
            audit.Elf64(bytes(data)).build_id()
        data = fixture();struct.pack_into("<I", data, 68, 4)
        elf = audit.Elf64(bytes(data))
        for address, size, executable in [(0x300, 8, True), (-1, 1, False),
                                          (0x3ff, 2, False), (0x500, 0, False)]:
            with self.subTest(address=address), self.assertRaises(audit.ValidationError):
                elf.read(address, size, executable)

    def test_pointer_bytes_alone_do_not_authorize_relocation(self):
        for offset, value in [(0x288, 1), (0x290, 0x321)]:
            data = fixture();struct.pack_into("<Q", data, offset, value)
            with self.assertRaises(audit.ValidationError):
                audit.Elf64(bytes(data)).pointer(0x300, 0x320)

    def test_exact_source_profile_has_all_independently_mapped_sites(self):
        instructions, mapping = audit.source_profile(ROOT)
        self.assertEqual(len(instructions), 17)
        self.assertEqual(len(mapping), 35)
        self.assertEqual(mapping[0x44f83b8], 0x44fc600)
        self.assertEqual(mapping[0x4b95610], 0x4b99c90)
        with self.assertRaises(audit.ValidationError):
            audit.table("", "LatestInstructions")


if __name__ == "__main__":
    unittest.main()
