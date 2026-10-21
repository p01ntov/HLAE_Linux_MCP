"""Read installed VPK models and prepare private global resource overrides.

No game assets are distributed by this module. Prepared packs stay in the
user's data directory. Preparation alone does not apply them to a running game,
change a player's inventory, or implement per-player skins/paint kits.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct
import uuid
import zlib

from .discovery import find_game

MAX_TREE_BYTES = 64 * 1024**2
MAX_TREE_ENTRIES = 500000
MAX_MODEL_BYTES = 64 * 1024**2
MAX_PACK_BYTES = 256 * 1024**2
MAX_PACK_MODELS = 32
SIGNATURE = 0x55AA1234
DIRECTORY_ARCHIVE = 0x7FFF


def _resource_path(value):
    if (not isinstance(value, str) or not value or len(value) > 512 or "\\" in value
            or ":" in value or "\x00" in value or value.startswith("/")
            or any(part in {"", ".", ".."} for part in value.split("/"))):
        raise ValueError("Resource must be a safe relative engine path")
    if any(ord(character) < 32 for character in value):
        raise ValueError("Model resource contains control characters")
    return value


def model_resource(value):
    """Validate a compiled engine-relative name before any filesystem access."""
    if not isinstance(value, str) or not value.endswith(".vmdl_c"):
        raise ValueError("Model resource must be a safe relative .vmdl_c path")
    return _resource_path(value)


@dataclass(frozen=True)
class Entry:
    path: str
    crc32: int
    archive: int
    offset: int
    length: int
    preload: bytes

    @property
    def size(self):
        return len(self.preload) + self.length


class VpkIndex:
    """Bounded Valve VPK v2 tree reader; only model metadata is retained."""
    def __init__(self, directory, *, extra_resources=(), resource_prefixes=()):
        self.extra_resources = {_resource_path(resource) for resource in extra_resources}
        if len(self.extra_resources) > 32:
            raise ValueError("At most 32 explicit extra VPK resources may be indexed")
        self.resource_prefixes = tuple(_resource_path(prefix.rstrip("/"))+"/" for prefix in resource_prefixes)
        if len(self.resource_prefixes)>8:
            raise ValueError("At most 8 explicit VPK resource prefixes may be indexed")
        self.path = Path(directory).expanduser().resolve()
        if not self.path.name.endswith("_dir.vpk"):
            raise ValueError("Expected a VPK directory index named *_dir.vpk")
        with self.path.open("rb") as handle:
            header = handle.read(28)
            if len(header) != 28:
                raise ValueError("Truncated VPK v2 header")
            magic, version, tree_size, data_size, archive_md5, other_md5, signature = struct.unpack("<7I", header)
            if magic != SIGNATURE or version != 2:
                raise ValueError("Only Valve VPK directory format version 2 is supported")
            if not 1 <= tree_size <= MAX_TREE_BYTES:
                raise ValueError("VPK directory tree exceeds the supported size")
            declared_size = 28 + tree_size + data_size + archive_md5 + other_md5 + signature
            trailing = os.fstat(handle.fileno()).st_size - declared_size
            # Current CS2 indexes append a variable-length signing footer beyond
            # the header's small signature descriptor. We never consume it.
            if not 0 <= trailing <= 65536:
                raise ValueError("VPK sections are truncated or have an oversized signing footer")
            tree = handle.read(tree_size)
        self.data_offset, self.data_size = 28 + tree_size, data_size
        self.tree_sha256 = hashlib.sha256(header + tree).hexdigest()
        self.entries = {}
        self.entry_count = 0
        cursor = 0

        def string():
            nonlocal cursor
            end = tree.find(b"\0", cursor)
            if end < 0 or end - cursor > 4096:
                raise ValueError("Unterminated or oversized VPK tree string")
            try:
                value = tree[cursor:end].decode("utf-8")
            except UnicodeError as error:
                raise ValueError("VPK tree names must be UTF-8") from error
            cursor = end + 1
            return value

        while True:
            extension = string()
            if not extension:
                break
            while True:
                directory = string()
                if not directory:
                    break
                while True:
                    name = string()
                    if not name:
                        break
                    if cursor + 18 > len(tree):
                        raise ValueError("Truncated VPK directory entry")
                    crc, preload_size, archive, offset, length, terminator = struct.unpack_from("<IHHIIH", tree, cursor)
                    cursor += 18
                    self.entry_count += 1
                    if self.entry_count > MAX_TREE_ENTRIES:
                        raise ValueError("VPK has too many directory entries")
                    if terminator != 0xFFFF or cursor + preload_size > len(tree):
                        raise ValueError("Invalid VPK entry terminator or truncated preload")
                    preload = tree[cursor:cursor + preload_size]
                    cursor += preload_size
                    raw_path = ("" if directory == " " else directory + "/") + name + "." + extension
                    if extension == "vmdl_c" or raw_path in self.extra_resources or raw_path.startswith(self.resource_prefixes):
                        path = _resource_path(raw_path)
                        if path in self.entries:
                            raise ValueError("VPK contains a duplicate model resource")
                        if archive == DIRECTORY_ARCHIVE and offset + length > data_size:
                            raise ValueError("VPK model extends beyond its inline data section")
                        self.entries[path] = Entry(path, crc, archive, offset, length, preload)
        if cursor != len(tree):
            raise ValueError("Unexpected bytes after VPK directory tree")

    def read_model(self, resource):
        resource = model_resource(resource)
        return self.read_resource(resource)

    def read_resource(self, resource):
        resource = _resource_path(resource)
        entry = self.entries.get(resource)
        if entry is None:
            raise ValueError(f"Model is not present in this VPK: {resource}")
        if entry.size > MAX_MODEL_BYTES:
            raise ValueError(f"Model exceeds the {MAX_MODEL_BYTES // 1024**2} MiB per-resource limit")
        payload = entry.preload
        if entry.length:
            source = (self.path if entry.archive == DIRECTORY_ARCHIVE else
                      self.path.with_name(self.path.name.removesuffix("_dir.vpk") + f"_{entry.archive:03d}.vpk"))
            info = source.lstat()
            if not stat.S_ISREG(info.st_mode):
                raise ValueError("VPK model archive must be a regular file, not a symlink")
            offset = entry.offset + (self.data_offset if entry.archive == DIRECTORY_ARCHIVE else 0)
            with source.open("rb") as handle:
                if offset + entry.length > os.fstat(handle.fileno()).st_size:
                    raise ValueError("VPK model extends beyond its archive file")
                handle.seek(offset)
                chunk = handle.read(entry.length)
                if len(chunk) != entry.length:
                    raise ValueError("VPK model archive was truncated while reading")
                payload += chunk
        if zlib.crc32(payload) & 0xFFFFFFFF != entry.crc32:
            raise ValueError(f"VPK model CRC32 mismatch: {resource}")
        return payload


def installed_index(game_dir=None):
    return VpkIndex(find_game(str(game_dir) if game_dir is not None else None) / "csgo/pak01_dir.vpk")


def _kind(path):
    lowered = path.casefold()
    if "glove" in lowered:
        return "glove"
    if lowered.startswith(("agents/", "characters/", "models/player/")):
        return "character"
    if lowered.startswith(("weapons/", "models/weapons/")):
        return "weapon"
    return "other"


def catalog(game_dir=None, *, kind="all", search="", limit=50, offset=0):
    if kind not in {"all", "character", "weapon", "glove", "other"}:
        raise ValueError("Catalog kind must be all, character, weapon, glove or other")
    if (not isinstance(search, str) or len(search) > 256 or isinstance(limit, bool)
            or not isinstance(limit, int) or not 1 <= limit <= 100 or isinstance(offset, bool)
            or not isinstance(offset, int) or not 0 <= offset <= MAX_TREE_ENTRIES):
        raise ValueError("Catalog search/pagination exceeds supported bounds")
    index = installed_index(game_dir)
    matches = sorted(path for path in index.entries if (kind == "all" or _kind(path) == kind) and search.casefold() in path.casefold())
    page = matches[offset:offset + limit]
    return {"source_vpk": str(index.path), "tree_sha256": index.tree_sha256, "total": len(matches),
            "offset": offset, "next_offset": offset + len(page) if offset + len(page) < len(matches) else None,
            "models": [{"resource": path, "engine_model": path.removesuffix("_c"), "kind": _kind(path),
                        "bytes": index.entries[path].size, "crc32": f"{index.entries[path].crc32:08x}"} for path in page],
            "notes": ["Catalog only; no model has been applied to the game.",
                      "Replacing a resource affects every entity using that resource, not an individual player's inventory."]}


def _private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError("Appearance pack directory must be an owned, non-symlink directory")
    path.chmod(0o700)
    return path


def prepare_override_pack(mapping, game_dir=None):
    """Publish a new private loose-file pack atomically; never edit base CS2."""
    if not isinstance(mapping, dict) or not 1 <= len(mapping) <= MAX_PACK_MODELS:
        raise ValueError(f"An appearance pack requires 1..{MAX_PACK_MODELS} original-to-replacement mappings")
    normalized = {model_resource(original): model_resource(replacement) for original, replacement in mapping.items()}
    if any(original.split("/", 1)[0] not in {"agents", "characters", "weapons", "models"} for original in normalized):
        raise ValueError("Original appearance resources must use agents/, characters/, weapons/ or models/ roots")
    index = installed_index(game_dir)
    for resource in set(normalized) | set(normalized.values()):
        if resource not in index.entries:
            raise ValueError(f"Model is not present in the installed VPK: {resource}")
    total = sum(index.entries[path].size for path in normalized.values())
    if total > MAX_PACK_BYTES:
        raise ValueError(f"Appearance pack exceeds the {MAX_PACK_BYTES // 1024**2} MiB total limit")
    if any(index.entries[path].size > MAX_MODEL_BYTES for path in normalized.values()):
        raise ValueError("Appearance pack contains an oversized model resource")
    data_root = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")).expanduser()
    if not data_root.is_absolute():
        raise ValueError("XDG_DATA_HOME must be an absolute path for private appearance packs")
    project_root = _private_directory(data_root / "cs2-camera")
    root = _private_directory(project_root / "appearance-packs")
    identifier = "pack-" + uuid.uuid4().hex
    stage, destination = root / ("." + identifier + ".tmp"), root / identifier
    stage.mkdir(mode=0o700)
    records = []
    try:
        for original, replacement in sorted(normalized.items()):
            payload = index.read_model(replacement)
            path = stage.joinpath(*PurePosixPath(original).parts)
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with path.open("xb") as handle:
                os.chmod(path, 0o600)
                handle.write(payload)
                handle.flush()
                os.fsync(handle.fileno())
            records.append({"original": original, "replacement": replacement, "bytes": len(payload),
                            "crc32": f"{zlib.crc32(payload) & 0xFFFFFFFF:08x}", "sha256": hashlib.sha256(payload).hexdigest()})
        manifest = {"format": "cs2-camera-appearance-pack", "version": 1,
                    "created": datetime.now(timezone.utc).isoformat(), "scope": "global_resource_override",
                    "source_vpk": str(index.path), "source_tree_sha256": index.tree_sha256, "mappings": records,
                    "bytes": total, "models": [resource.removesuffix("_c") for resource in sorted(normalized)],
                    "notes": ["Prepared locally from installed assets; not applied to the game.",
                              "Base-game replacement dependencies remain in the installed VPK; no raw FBX conversion or per-player paint-kit editing."]}
        with (stage / "appearance-pack.json").open("x") as handle:
            os.chmod(stage / "appearance-pack.json", 0o600)
            json.dump(manifest, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        if destination.exists():
            raise FileExistsError("Appearance pack destination already exists")
        stage.rename(destination)
        return {"path": str(destination), "manifest": str(destination / "appearance-pack.json"), **manifest}
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
