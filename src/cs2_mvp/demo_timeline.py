"""One-time host index for the full recording, independent of camera key time.

Duration comes exclusively from the recording's DEM_FileInfo protobuf. Event
ticks never stand in for the end of the demo. No work in this module runs from
the game's frame loop.
"""
from __future__ import annotations

import copy
from functools import lru_cache
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct

from .demo import parser

MAX_BYTES = 512 * 1024
MAX_FILE_INFO = 1024 * 1024
MAX_PLAYERS = 128
MAX_ROUNDS = 1024
MAX_EVENT_TICKS = 32768
MAX_TICK = 2000000000
TICK_RATE = 64


def _varint(read):
    value = 0
    for shift in range(0, 70, 7):
        data = read(1)
        if len(data) != 1:
            raise ValueError("Truncated demo metadata varint")
        byte = data[0]
        value |= (byte & 127) << shift
        if not byte & 128:
            if value > 0xffffffffffffffff:
                break
            return value
    raise ValueError("Invalid demo metadata varint")


class _Bytes:
    def __init__(self, data):
        self.data, self.position = data, 0

    def read(self, count):
        end = self.position + count
        if end > len(self.data):
            raise ValueError("Truncated demo metadata payload")
        value = self.data[self.position:end]
        self.position = end
        return value


def _snappy(data):
    """Decode a bounded raw Snappy metadata block, without another dependency."""
    source = _Bytes(data)
    expected = _varint(source.read)
    if not 1 <= expected <= MAX_FILE_INFO:
        raise ValueError("Compressed demo metadata exceeds its supported bound")
    result = bytearray()
    while len(result) < expected:
        tag = source.read(1)[0]
        kind = tag & 3
        if kind == 0:
            length = tag >> 2
            if length < 60:
                length += 1
            else:
                length = int.from_bytes(source.read(length - 59), "little") + 1
            if len(result) + length > expected:
                raise ValueError("Invalid compressed demo metadata literal")
            result.extend(source.read(length))
            continue
        if kind == 1:
            length = 4 + ((tag >> 2) & 7)
            offset = ((tag & 0xe0) << 3) | source.read(1)[0]
        else:
            length = 1 + (tag >> 2)
            offset = int.from_bytes(source.read(2 if kind == 2 else 4), "little")
        if not 1 <= offset <= len(result) or len(result) + length > expected:
            raise ValueError("Invalid compressed demo metadata copy")
        pattern = result[-offset:]
        result.extend((pattern * ((length + offset - 1) // offset))[:length])
    if source.position != len(data):
        raise ValueError("Trailing bytes in compressed demo metadata")
    return bytes(result)


def _protobuf(data):
    source, values = _Bytes(data), {}
    while source.position < len(data):
        key = _varint(source.read)
        field, wire = key >> 3, key & 7
        if not field:
            raise ValueError("Invalid demo metadata protobuf field")
        if wire == 0:
            value = _varint(source.read)
        elif wire in (1, 5):
            value = source.read(8 if wire == 1 else 4)
        elif wire == 2:
            length = _varint(source.read)
            if length > MAX_FILE_INFO:
                raise ValueError("Demo metadata protobuf field exceeds its bound")
            value = source.read(length)
        else:
            raise ValueError("Unsupported demo metadata protobuf wire type")
        if field in (1, 2):
            if field in values:
                raise ValueError("Ambiguous duplicate demo duration metadata")
            values[field] = (wire, value)
    return values


def file_info(path):
    """Read authoritative playback_ticks/playback_time at the PBDEMS2 footer."""
    path = Path(path)
    with path.open("rb") as handle:
        size = os.fstat(handle.fileno()).st_size
        header = handle.read(16)
        if len(header) != 16 or header[:8] != b"PBDEMS2\0":
            raise ValueError("Full demo timeline requires a complete CS2 PBDEMS2 recording")
        offset = struct.unpack_from("<I", header, 8)[0]
        if not 16 <= offset < size:
            raise ValueError("Authoritative DEM_FileInfo is missing; the demo may still be downloading")
        handle.seek(offset)
        command = _varint(handle.read)
        frame_tick = _varint(handle.read)
        count = _varint(handle.read)
        if command & ~64 != 2 or not 1 <= count <= MAX_FILE_INFO or handle.tell() + count > size:
            raise ValueError("Invalid authoritative DEM_FileInfo frame")
        payload = handle.read(count)
        if command & 64:
            payload = _snappy(payload)
    values = _protobuf(payload)
    if values.get(1, (None,))[0] != 5 or values.get(2, (None,))[0] != 0:
        raise ValueError("DEM_FileInfo has no authoritative playback_time and playback_ticks")
    duration = struct.unpack("<f", values[1][1])[0]
    ticks = values[2][1]
    if (not 1 <= ticks <= MAX_TICK or not math.isfinite(duration) or duration <= 0
            or frame_tick > MAX_TICK or abs(duration - ticks / TICK_RATE) > 1 / TICK_RATE):
        raise ValueError("DEM_FileInfo playback duration is invalid or disagrees with its tick count")
    return {"playback_ticks": ticks, "tick_rate": TICK_RATE, "duration_seconds": duration,
            "authoritative_duration_source": "DEM_FileInfo"}


def _identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def _steam_id(value):
    if value is None or value in ("", "0", 0) or isinstance(value, float) and math.isnan(value):
        return None  # World damage and bots have no exact SteamID64 target.
    if hasattr(value, "item"):
        value = value.item()
    text = str(value)
    if isinstance(value, (bool, float)) or not re.fullmatch(r"[1-9][0-9]{0,19}", text) or int(text) > 0xffffffffffffffff:
        raise ValueError("Demo timeline contains an inexact or invalid SteamID64; refusing nickname/float targeting")
    return text


def _name(value):
    if not isinstance(value, str):
        return ""
    return value.replace("\0", "").encode("utf-8", errors="replace")[:128].decode("utf-8", errors="ignore")


def _tick(value, end):
    if hasattr(value, "item"):
        value = value.item()
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= end:
        raise ValueError("Recorded demo event tick is invalid or outside authoritative playback bounds")
    return value


def _rows(frame, limit):
    if len(frame) > limit:
        raise ValueError("Full demo timeline exceeds its bounded event/roster limit")
    return frame.to_dict(orient="records")


@lru_cache(maxsize=4)
def _index(path, identity):
    source = Path(path)
    duration = file_info(source)
    try:
        demo = parser(source)
        # Both event types are requested in one parser pass, once per recording.
        tables = dict(demo.parse_events(["player_death", "round_start"],
                                       other=["total_rounds_played", "is_warmup_period"]))
        if set(tables) != {"player_death", "round_start"}:
            raise ValueError("Demo parser did not return both requested timeline event tables")
        roster = _rows(demo.parse_player_info(), MAX_PLAYERS)
        deaths = _rows(tables["player_death"], MAX_EVENT_TICKS)
        starts = _rows(tables["round_start"], MAX_ROUNDS)
    except (OSError, ValueError):
        raise
    except Exception as error:
        raise ValueError(f"Full demo timeline parser could not read this recording: {error}") from error
    players = {}

    def player(identity, name):
        if identity is None:
            return None
        if identity not in players:
            if len(players) >= MAX_PLAYERS:
                raise ValueError("Full demo timeline exceeds 128 exact SteamID64 players")
            players[identity] = {"steam_id": identity, "name": _name(name), "kills": [], "deaths": []}
        return players[identity]

    for row in roster:
        player(_steam_id(row.get("steamid")), row.get("name"))
    event_count = 0
    for row in deaths:
        tick = _tick(row.get("tick"), duration["playback_ticks"])
        attacker_id, victim_id = _steam_id(row.get("attacker_steamid")), _steam_id(row.get("user_steamid"))
        attacker = player(attacker_id, row.get("attacker_name"))
        victim = player(victim_id, row.get("user_name"))
        if attacker and attacker_id != victim_id:
            attacker["kills"].append(tick)
            event_count += 1
        if victim:
            victim["deaths"].append(tick)
            event_count += 1
        if event_count > MAX_EVENT_TICKS:
            raise ValueError("Full demo timeline exceeds 32768 recorded kill/death markers")
    rounds = []
    for row in starts:
        item = {"tick": _tick(row.get("tick"), duration["playback_ticks"])}
        number = row.get("round")
        if hasattr(number, "item"):
            number = number.item()
        if type(number) is int and 1 <= number <= MAX_ROUNDS:
            item["round"] = number
        rounds.append(item)
    rounds.sort(key=lambda row: row["tick"])
    for row in players.values():
        row["kills"].sort()
        row["deaths"].sort()
    digest = hashlib.sha256()
    with source.open("rb") as handle:
        if _identity(os.fstat(handle.fileno())) != identity:
            raise ValueError("Demo changed while its full timeline was being indexed")
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
        if _identity(os.fstat(handle.fileno())) != identity:
            raise ValueError("Demo changed while its full fingerprint was being read")
    if _identity(source.stat()) != identity:
        raise ValueError("Demo was replaced while its full timeline was being indexed")
    result = {"version": 1, "demo": {"path": path, "fingerprint": {"sha256": digest.hexdigest(),
              "bytes": identity[2], "mtime_ns": identity[3]}, **duration},
              "rounds": rounds, "players": sorted(players.values(), key=lambda row: int(row["steam_id"]))}
    encode(result)  # Refuse an oversized complete index instead of truncating it.
    return result


def encode(timeline):
    data = json.dumps(timeline, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(data) > MAX_BYTES:
        raise ValueError("Full demo timeline exceeds its 512 KiB metadata limit")
    return data


def build_timeline(path):
    source = Path(path).expanduser().resolve(strict=True)
    if source.suffix.lower() != ".dem" or not source.is_file():
        raise ValueError("Full demo timeline requires an existing local .dem file")
    return copy.deepcopy(_index(str(source), _identity(source.stat())))


def stage_timeline(demo, destination):
    """Write a private regular metadata file before launching the game."""
    timeline = build_timeline(demo)
    destination = Path(destination).absolute()
    descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(encode(timeline))
            handle.flush()
            os.fsync(handle.fileno())
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    return destination
