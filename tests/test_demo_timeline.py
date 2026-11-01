import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp import demo_timeline as timeline
from cs2_mvp import live
from cs2_mvp.live_mcp import TOOL_MAP


def varint(value):
    data = bytearray()
    while value >= 128:
        data.append((value & 127) | 128)
        value >>= 7
    data.append(value)
    return bytes(data)


def demo_bytes(ticks=10000, seconds=None, *, compressed=False, payload=None):
    if payload is None:
        payload = b"\x0d" + struct.pack("<f", ticks / 64 if seconds is None else seconds) + b"\x10" + varint(ticks)
    if compressed:
        if len(payload) >= 60:
            raise AssertionError("Fixture literal must be short")
        payload = varint(len(payload)) + bytes([(len(payload) - 1) << 2]) + payload
    return b"PBDEMS2\0" + struct.pack("<II", 16, 0) + varint(66 if compressed else 2) + varint(ticks) + varint(len(payload)) + payload


class Frame:
    def __init__(self, rows):
        self.rows = rows

    def __len__(self):
        return len(self.rows)

    def to_dict(self, orient):
        return copy.deepcopy(self.rows)


class Parser:
    def __init__(self):
        self.events_calls = 0
        self.roster_calls = 0
        self.players = [{"steamid": 76561198000000001, "name": "Same name"},
                        {"steamid": 76561198000000002, "name": "Same name"},
                        {"steamid": 76561198000000003, "name": "No events"}]
        self.deaths = [{"tick": 64, "attacker_steamid": "76561198000000001", "user_steamid": "76561198000000002"},
                       {"tick": 64, "attacker_steamid": "76561198000000001", "user_steamid": "76561198000000002"},
                       {"tick": 128, "attacker_steamid": "76561198000000001", "user_steamid": "76561198000000001"},
                       {"tick": 256, "attacker_steamid": None, "user_steamid": "76561198000000002"}]
        self.rounds = [{"tick": 32, "round": 1}, {"tick": 512, "round": 1}]

    def parse_events(self, names, *, other):
        self.events_calls += 1
        assert names == ["player_death", "round_start"]
        assert other == ["total_rounds_played", "is_warmup_period"]
        return [("player_death", Frame(self.deaths)), ("round_start", Frame(self.rounds))]

    def parse_player_info(self):
        self.roster_calls += 1
        return Frame(self.players)


class TimelineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "match.dem"
        self.path.write_bytes(demo_bytes())
        self.parser = Parser()
        timeline._index.cache_clear()
        self.addCleanup(timeline._index.cache_clear)
        self.factory = patch.object(timeline, "parser", return_value=self.parser).start()
        self.addCleanup(patch.stopall)

    def test_authoritative_end_is_independent_of_last_recorded_event_and_camera_keys(self):
        result = timeline.build_timeline(self.path)
        self.assertEqual(result["demo"]["playback_ticks"], 10000)
        self.assertEqual(result["demo"]["duration_seconds"], 156.25)
        self.assertEqual(result["demo"]["authoritative_duration_source"], "DEM_FileInfo")
        self.assertEqual(result["rounds"], [{"tick": 32, "round": 1}, {"tick": 512, "round": 1}])
        self.assertNotIn("keys", result)
        rows = {row["steam_id"]: row for row in result["players"]}
        self.assertEqual(rows["76561198000000001"]["kills"], [64, 64])
        self.assertEqual(rows["76561198000000001"]["deaths"], [128])
        self.assertEqual(rows["76561198000000002"]["deaths"], [64, 64, 256])
        self.assertEqual(rows["76561198000000003"]["kills"], [])
        self.assertEqual(result["demo"]["fingerprint"], {"sha256": hashlib.sha256(self.path.read_bytes()).hexdigest(),
            "bytes": self.path.stat().st_size, "mtime_ns": self.path.stat().st_mtime_ns})
        self.assertEqual((self.parser.events_calls, self.parser.roster_calls), (1, 1))

    def test_cached_index_is_not_reparsed_or_mutable_by_callers_and_changed_file_invalidates_it(self):
        first = timeline.build_timeline(self.path)
        first["players"].clear()
        second = timeline.build_timeline(self.path)
        self.assertEqual(len(second["players"]), 3)
        self.assertEqual(self.factory.call_count, 1)
        self.path.write_bytes(demo_bytes(20000))
        third = timeline.build_timeline(self.path)
        self.assertEqual(third["demo"]["playback_ticks"], 20000)
        self.assertNotEqual(second["demo"]["fingerprint"]["sha256"], third["demo"]["fingerprint"]["sha256"])
        self.assertEqual(self.factory.call_count, 2)

    def test_incomplete_or_contradictory_duration_never_falls_back_to_events(self):
        for data in (b"PBDEMS2\0", demo_bytes(seconds=1), demo_bytes(seconds=float("nan")),
                     demo_bytes(payload=b"\x10" + varint(10000)), demo_bytes(payload=b"\x0d" + struct.pack("<f", 10)),
                     demo_bytes()[:-1], demo_bytes(payload=b"\x0d" + struct.pack("<f", 156.25) + b"\x10" + varint(10000) + b"\x10\x01")):
            with self.subTest(data=data):
                self.path.write_bytes(data)
                with self.assertRaises(ValueError):
                    timeline.build_timeline(self.path)
        self.factory.assert_not_called()

    def test_compressed_file_info_and_bounded_snappy_validation(self):
        self.path.write_bytes(demo_bytes(compressed=True))
        self.assertEqual(timeline.file_info(self.path)["playback_ticks"], 10000)
        # A copy whose offset exceeds decoded bytes must not touch arbitrary data.
        for data in (varint(timeline.MAX_FILE_INFO + 1), b"\x04\x01\x01", b"\x01\x00", b"\x01\x00Aextra"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                timeline._snappy(data)

    def test_invalid_event_bounds_or_float_steam_ids_are_rejected(self):
        for field, value in (("tick", -1), ("tick", 10001), ("tick", 64.0), ("attacker_steamid", 76561198000000001.0)):
            timeline._index.cache_clear()
            saved = self.parser.deaths[0][field]
            self.parser.deaths[0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                timeline.build_timeline(self.path)
            self.parser.deaths[0][field] = saved

    def test_event_roster_and_json_limits_fail_explicitly_without_truncation(self):
        self.parser.rounds *= 513
        with self.assertRaisesRegex(ValueError, "limit"):
            timeline.build_timeline(self.path)
        self.parser.rounds = []
        self.parser.players = [{"steamid": 1 + i, "name": "Player"} for i in range(129)]
        with self.assertRaisesRegex(ValueError, "limit"):
            timeline.build_timeline(self.path)
        with self.assertRaisesRegex(ValueError, "512 KiB"):
            timeline.encode({"oversized": "x" * timeline.MAX_BYTES})

    def test_source_replacement_during_parse_is_rejected(self):
        original = self.parser.parse_player_info
        def replaced():
            self.path.write_bytes(demo_bytes(20000))
            return original()
        self.parser.parse_player_info = replaced
        with self.assertRaisesRegex(ValueError, "changed|replaced"):
            timeline.build_timeline(self.path)

    def test_staged_metadata_is_private_regular_and_never_overwrites(self):
        destination = Path(self.directory.name) / "timeline.json"
        self.assertEqual(timeline.stage_timeline(self.path, destination), destination)
        self.assertFalse(destination.is_symlink())
        self.assertEqual(destination.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads(destination.read_text())["demo"]["path"], str(self.path))
        with self.assertRaises(FileExistsError):
            timeline.stage_timeline(self.path, destination)
        alias = Path(self.directory.name) / "alias.json"
        alias.symlink_to(destination)
        with self.assertRaises(FileExistsError):
            timeline.stage_timeline(self.path, alias)

    def test_host_startup_failure_keeps_camera_usable_and_refresh_is_internal(self):
        notices = []
        with patch.object(timeline, "stage_timeline", side_effect=ValueError("No authoritative footer")):
            result = live.staged_demo_timeline_environment(self.path, Path(self.directory.name), notices.append)
        self.assertIn("No authoritative footer", result["CS2_CAMERA_DEMO_TIMELINE_ERROR"])
        self.assertTrue(notices)
        class Client:
            def __init__(self): self.calls = []
            def request(self, method, params): self.calls.append((method, params)); return {"accepted": True}
        client = Client()
        with patch.object(live, "session_descriptor", return_value={"demo": str(self.path)}):
            self.assertEqual(live.refresh_demo_timeline(client), {"accepted": True})
        self.assertEqual(client.calls[0][0], "set_demo_timeline")
        self.assertEqual(client.calls[0][1]["timeline"]["demo"]["playback_ticks"], 10000)
        self.assertIn("set_demo_timeline", live.INTERNAL_LIVE_METHODS)
        self.assertNotIn("set_demo_timeline", live.LIVE_METHODS)
        self.assertNotIn("cs2_camera_set_demo_timeline", TOOL_MAP)


if __name__ == "__main__":
    unittest.main()
