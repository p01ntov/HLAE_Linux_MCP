#!/usr/bin/env python3
"""Read-only offline revalidation of the Steam 26334290 Linux client adapter.

No game execution, memory access, downloads, runtime scanning or source changes.
The checked-in C++ tables supply the exact runtime bytes; independent ELF/RTTI,
relocation and call-chain assertions prove the meaning of their mapped sites.
Run from any directory: python tools/revalidate_linux_client.py /path/libclient.so
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

BUILD_ID = "c6c5098eab52964d9d088279ca5cc991c76d1a7d"
SHA256 = "8b319dfec7d63e2de18962685ddcdbf45f943f0a7e7fcbb4e3fe86814fb73ad2"


class ValidationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


class Elf64:
    """The small ELF64 LE subset needed here, with bounded file accesses."""

    def __init__(self, data: bytes):
        self.blob = data
        require(len(data) >= 64 and data[:7] == b"\x7fELF\x02\x01\x01", "not ELF64 little endian")
        require(self.unpack("<HH", 16) == (3, 62), "expected x86-64 shared ELF")
        phoff, shoff = self.unpack("<QQ", 32)
        phsize, phnum, shsize, shnum = self.unpack("<HHHH", 54)
        require(phsize == 56 and 0 < phnum < 4096, "invalid program headers")
        require(shsize == 64 and 0 < shnum < 65535, "invalid section headers")
        self.segments = [self.unpack("<IIQQQQQQ", phoff + i * phsize) for i in range(phnum)]
        self.sections = [self.unpack("<IIQQQQIIQQ", shoff + i * shsize) for i in range(shnum)]
        for kind, _, offset, _, _, size, memory, _ in self.segments:
            if kind in (1, 4):
                self.slice(offset, size)
                require(size <= memory, "segment file size exceeds memory size")
        self.relocations = {}
        for section in self.sections:
            if section[1] != 4:  # SHT_RELA
                continue
            require(section[9] == 24 and section[5] % 24 == 0, "invalid RELA section")
            self.slice(section[4], section[5])
            for position in range(section[4], section[4] + section[5], 24):
                address, info, addend = self.unpack("<QQq", position)
                require(address not in self.relocations, "duplicate relocation address")
                self.relocations[address] = (info & 0xffffffff, addend)

    def slice(self, offset: int, size: int) -> bytes:
        require(0 <= offset <= len(self.blob) and 0 <= size <= len(self.blob) - offset,
                f"ELF file range out of bounds: {offset:#x}+{size:#x}")
        return self.blob[offset:offset + size]

    def unpack(self, fmt: str, offset: int) -> tuple:
        return struct.unpack(fmt, self.slice(offset, struct.calcsize(fmt)))

    def read(self, address: int, size: int, executable: bool = False) -> bytes:
        ranges = [(off + address - va, flags) for kind, flags, off, va, _, length, _, _ in self.segments
                  if kind == 1 and va <= address and 0 <= size <= length - (address - va)]
        require(len(ranges) == 1, f"unmapped/ambiguous ELF address {address:#x}")
        offset, flags = ranges[0]
        require(not executable or flags & 1, f"instruction outside executable segment: {address:#x}")
        return self.slice(offset, size)

    def u64(self, address: int) -> int:
        return struct.unpack("<Q", self.read(address, 8))[0]

    def relative(self, address: int, displacement: int = 3, size: int = 7) -> int:
        return address + size + struct.unpack("<i", self.read(address + displacement, 4, True))[0]

    def pointer(self, address: int, target: int) -> None:
        require(self.u64(address) == target and self.relocations.get(address) == (8, target),
                f"R_X86_64_RELATIVE pointer differs: {address:#x} -> {target:#x}")

    def text(self, address: int, expected: str) -> None:
        value = expected.encode() + b"\0"
        require(self.read(address, len(value)) == value, f"string differs at {address:#x}: {expected}")

    def build_id(self) -> str:
        results = []
        align = lambda size: (size + 3) & ~3
        for kind, _, offset, _, _, size, _, _ in self.segments:
            if kind != 4:
                continue
            end = offset + size
            while offset < end:
                require(end - offset >= 12, "truncated ELF note header")
                namesz, descsz, typ = self.unpack("<III", offset)
                total = 12 + align(namesz) + align(descsz)
                require(total <= end - offset, "truncated ELF note data")
                if typ == 3 and self.slice(offset + 12, namesz) == b"GNU\0":
                    results.append(self.slice(offset + 12 + align(namesz), descsz).hex())
                offset += total
        require(len(results) == 1, "expected one GNU build ID")
        return results[0]


def table(source: str, name: str) -> str:
    found = re.search(r"static const std::vector<(?:Instruction|Mapping)>& " + name
                      + r"\(\) \{\s*static const std::vector<(?:Instruction|Mapping)> values=\{(.*?)\};return values;", source, re.S)
    require(found is not None, f"missing C++ profile table: {name}")
    return found[1]


def source_profile(root: Path) -> tuple[list[tuple[int, int, bytes]], dict[int, int]]:
    source = (root / "native/live/client_build_profile.h").read_text()
    require(f'LatestBuildId="{BUILD_ID}"' in source, "runtime build ID differs")
    instructions = [(int(old, 16), int(new, 16), bytes(int(x, 16) for x in values.split(',')))
                    for old, new, values in re.findall(r"\{(0x[0-9a-f]+),(0x[0-9a-f]+),\{([^}]+)\}\}", table(source, "LatestInstructions"))]
    pairs = [(int(old, 16), int(new, 16)) for old, new in
             re.findall(r"\{(0x[0-9a-f]+),(0x[0-9a-f]+)\}", table(source, "LatestMappings"))]
    mapping = dict(pairs)
    require(len(instructions) == 17 and len(pairs) == len(mapping) == 18, "unexpected profile table size")
    require(len({old for old, _, _ in instructions} | mapping.keys()) == 35, "duplicate logical profile site")
    mapping.update({old: new for old, new, _ in instructions})
    return instructions, mapping


def validate(path: Path, root: Path) -> dict:
    blob = path.read_bytes()
    elf = Elf64(blob)
    require(elf.build_id() == BUILD_ID, "unvalidated client GNU build ID")
    digest = hashlib.sha256(blob).hexdigest()
    require(digest == SHA256, "client SHA-256 differs from the independently audited ELF")
    instructions, mapped = source_profile(root)
    checked = []
    for old, new, expected in instructions:
        require(expected and elf.read(new, len(expected), True) == expected,
                f"runtime instruction differs: {old:#x} -> {new:#x}")
        checked.append({"logical": hex(old), "current": hex(new), "length": len(expected)})

    view = (root / "native/linux_view_patch.h").read_text()
    require(f'LatestBuildIdText = "{BUILD_ID}"' in view, "view build ID differs")
    view_id = re.search(r"LatestBuildId = \{([^}]+)\}", view)
    require(view_id is not None, "missing latest view GNU build ID bytes")
    require(bytes(int(x.strip(), 16) for x in view_id[1].split(',')).hex() == BUILD_ID,
            "view GNU build ID bytes differ from the selected client")
    require('LatestSite = 0x1939920;' in view, "view patch site differs")
    row = re.search(r"\{LatestBuildIdText, LatestBuildId, (0x[0-9a-f]+), LatestSite,\s*"
                    r"(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+),\s*LatestOriginal, true\}", view)
    require(row is not None, "missing complete latest view ABI profile")
    require(tuple(int(x, 16) for x in row.groups()) ==
            (0x1939300, 0x1939950, 0x193996b, 0x19396d6, 0x19399f8, 0x1939a01), "view ABI addresses differ")
    original = re.search(r"LatestOriginal = \{([^}]+)\}", view)
    require(original is not None, "missing latest patch bytes")
    original_bytes = bytes(int(x.strip(), 16) for x in original[1].split(','))
    guards = {
        0x1939300: bytes.fromhex("55 48 89 e5 41 57 41 56 49 89 f6 41 55 41 54 53 48 83 ec 78"),
        0x1939920: original_bytes,
        0x1939933: bytes.fromhex("84 c0 74 4f"),
        0x1939950: bytes.fromhex("49 8b 86 a0 04 00 00"),
        0x193996b: bytes.fromhex("49 8b 86 b8 04 00 00"),
        0x19396d6: bytes.fromhex("f3 41 0f 11 86 98 04 00 00"),
        0x19399f8: bytes.fromhex("f3 41 0f 5c 8e 98 04 00 00"),
    }
    require(len(original_bytes) == 19, "view patch instruction length differs")
    for address, expected in guards.items():
        require(elf.read(address, len(expected), True) == expected, f"view ABI bytes differ at {address:#x}")

    # Each data mapping has its own anchor; there is deliberately no delta.
    pointers = [
        (mapped[0x44f83b8] + 174 * 8, mapped[0x18eee90]),
        (mapped[0x44f83b8] + 81 * 8, mapped[0x18ee880]),
        (mapped[0x44f83b8] - 8, 0x44f9c98),
        (mapped[0x44c6f58] - 8, mapped[0x44c6368]),
        (mapped[0x450bd30] - 8, 0x450ae00),
        (mapped[0x450bd30] + 0x7b0, mapped[0x1a18390]),
        (mapped[0x44e8560] - 8, mapped[0x44e3cc8]),
        (mapped[0x43ea830] - 8, mapped[0x43ea268]),
        (mapped[0x43ea268] + 16, mapped[0x44e3cc8]),
        (mapped[0x43ea830] + 8 * 8, mapped[0x17d4520]),
        (mapped[0x44308a8] - 8, 0x442c0d8),
        (mapped[0x44308a8] + 0x20, mapped[0x1909290]),
    ]
    for address, target in pointers:
        elf.pointer(address, target)
    rtti = {
        0x44f9c98: "14CSource2Client", 0x44ca5a8: "28CCSObserver_ObserverServices",
        0x450ae00: "14C_CSPlayerPawn", 0x44e7f08: "17CSkeletonInstance",
        0x43ee428: "N30CBodyComponentSkeletonInstance29NetworkVar_m_skeletonInstanceE",
        0x442c0d8: "20CClientAlphaProperty",
    }
    for address, name in rtti.items():
        text_address = elf.u64(address + 8)
        elf.pointer(address + 8, text_address)
        elf.text(text_address, name)
    relative_links = [
        (mapped[0x18eee9f], mapped[0x4b95610]),
        (mapped[0x18ee880] + 13, mapped[0x4b95610]),
        (mapped[0x17b7bee], mapped[0xb0e236]),
        (mapped[0x17b7de0], mapped[0xb6d0b1]),
        (mapped[0x17f7fd0], mapped[0x4687d38]),
        (mapped[0x17f7fd0] + 24, mapped[0x491ca70]),
        (mapped[0x161f690] + 12, mapped[0x49397b0]),
        (0x1626e1c, mapped[0x49083b8]),
    ]
    for address, target in relative_links:
        require(elf.relative(address) == target, f"RIP-relative address differs at {address:#x}")
    elf.text(mapped[0xb0e236], "spec_mode")
    elf.text(mapped[0xb6d0b1], "spec_player")
    # The observer readback calls the local-controller resolution function.
    require(elf.relative(mapped[0x1907110] + 6, 1, 5) == 0x1626e00, "observer resolver differs")
    # Distinguish the otherwise duplicate getter implementations using a caller
    # that reads skeleton m_modelState.m_hModel at 0x140 + 0xa0.
    model_binding = bytes.fromhex("48 8b 86 e0 01 00 00 48 89 f3 48 85 c0 74 10 48 8b 38 48 85 ff 74 08")
    require(elf.read(0xf8c265, len(model_binding), True) == model_binding, "model-name call chain differs")
    require(elf.relative(0xf8c27c, 1, 5) == mapped[0x252f8c0], "model-name getter target differs")
    return {
        "ok": True, "steam_build": 26334290, "version": "1.41.9.4", "gnu_build_id": BUILD_ID,
        "sha256": digest, "read_only": True, "instruction_sites": checked,
        "view_abi_sites": [hex(address) for address in guards],
        "relative_links": [{"site": hex(a), "target": hex(t)} for a, t in relative_links],
        "relocated_vtable_links": len(pointers), "rtti_types": {hex(a): name for a, name in rtti.items()},
        "runtime_requirements": ["registered SchemaSystem fields", "full-handle current entity ownership",
                                 "isolated -insecure demo session", "typed request/readback verification"],
        "limits": ["offline ELF checks do not prove live game/render behavior", "no gameplay HUD or removed cosmetics guard widened"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("client", type=Path, help="read-only installed Linux libclient.so")
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, help="optional JSON evidence path")
    arguments = parser.parse_args()
    try:
        report = validate(arguments.client, arguments.source_root)
    except (OSError, ValueError, struct.error) as error:
        report = {"ok": False, "read_only": True, "error": str(error)}
    result = json.dumps(report, indent=2) + "\n"
    if arguments.output:
        arguments.output.write_text(result)
    print(result, end="")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
