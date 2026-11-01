import copy
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp import follow_camera as follow
from cs2_mvp.live_track import LiveTrack
from cs2_mvp.pose_track import from_angles


STEAM_ID = "76561198836929612"


class Frame:
    def __init__(self, rows):
        self.rows = rows

    def __len__(self):
        return len(self.rows)

    def to_dict(self, orient):
        assert orient == "records"
        return copy.deepcopy(self.rows)


def row(tick, **values):
    return {"tick": tick, "steamid": int(STEAM_ID), "X": tick-640, "Y": 0, "Z": 0,
            "pitch": 0, "yaw": 0, "health": 100, "is_alive": True, **values}


class Parser:
    def __init__(self):
        self.players = [{"steamid": int(STEAM_ID), "name": "L201742"}]
        self.records = [row(tick) for tick in range(640, 644)]
        self.queries = []

    def parse_player_info(self):
        return Frame(self.players)

    def parse_ticks(self, properties, **query):
        self.queries.append((properties, query))
        return Frame(self.records)


class FollowCameraTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "recording.dem"
        self.path.write_bytes(b"PBDEMS2\0test")
        self.source = Parser()
        self.patch = patch.object(follow, "parser", return_value=self.source)
        self.parse = self.patch.start()
        self.addCleanup(self.patch.stop)

    def bake(self, **options):
        return follow.bake_demo_follow(self.path, STEAM_ID, 640, 644, options)

    def assert_vector(self, actual, expected, places=7):
        self.assertEqual(len(actual), len(expected))
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, places=places)

    def test_exact_tick_evidence_bakes_editable_keys_and_declares_draft(self):
        result = self.bake()
        track = LiveTrack(result["project"])
        self.assertEqual(track.start_tick, 640)
        self.assertEqual(track.duration, 4/64)
        self.assertEqual([key.time for key in track.keys], [i/64 for i in range(5)])
        self.assertEqual(self.source.queries[0][1], {"players": [int(STEAM_ID)], "ticks": [640, 641, 642, 643]})
        self.assertEqual(track.interpolation, "linear")
        self.assertEqual(track.keys[-1].position, track.keys[-2].position)
        self.assertEqual(result["evidence"]["last_recorded_tick"], 643)
        self.assertFalse(result["evidence"]["dynamic_entity_tracking"])
        self.assertFalse(result["evidence"]["geometry_checked"])
        self.assertFalse(result["evidence"]["visual_verified"])
        self.assertTrue(result["evidence"]["requires_geometry_guard_before_apply"])
        self.assertEqual(result["evidence"]["status"], "draft_unverified")
        self.assertEqual(result["evidence"]["source"]["file_identity"],
                         follow._fingerprint_evidence(follow._fingerprint(self.path)))
        json.dumps(result, allow_nan=False)

    def test_yaw_offset_and_look_at_pitch_use_source_axes(self):
        self.source.records = [row(tick, yaw=90) for tick in range(640, 644)]
        result = self.bake(offset=[-100, 10, 40], look_at_offset=[0, 0, 40])
        self.assert_vector(result["project"]["keys"][0]["position"], [-10, -100, 40])
        self.assert_vector(result["project"]["keys"][0]["angles"], [0, math.degrees(math.atan2(100, 10)), 0])
        world = self.bake(offset_space="world", offset=[-100, 10, 40], look_at_offset=[0, 0, 40])
        self.assert_vector(world["project"]["keys"][0]["position"], [-100, 10, 40])

    def test_view_offset_and_attach_rotation_are_rigid_local_transforms(self):
        self.source.records = [row(tick, pitch=30, yaw=90) for tick in range(640, 644)]
        key = self.bake(mode="attach", offset_space="view", offset=[100, 0, 0], angle_offset=[0, 90, 0])["project"]["keys"][0]
        self.assert_vector(key["position"], [0, 100*math.cos(math.radians(30)), -50])
        # A local yaw trim rotates around the pitched view's up, not world Z.
        q = from_angles(key["angles"])
        forward = follow._rotate((1., 0., 0.), q)
        self.assert_vector(forward, [-1, 0, 0])
        self.assertGreater(abs(key["angles"][2]), 20)

    def test_lock_on_keeps_position_but_tracks_recorded_target(self):
        result = self.bake(mode="lock_on", camera_position=[-100, -100, 40], look_at_offset=[0, 0, 40])
        keys = result["project"]["keys"]
        for key in keys:
            self.assertEqual(key["position"], [-100., -100., 40.])
        self.assertAlmostEqual(keys[0]["angles"][1], 45)
        self.assertLess(keys[3]["angles"][1], keys[0]["angles"][1])

    def test_position_filter_uses_simulation_time_and_random_seeks_are_identical(self):
        options = {"mode": "attach", "offset": [0, 0, 0], "position_half_life": 1/64}
        first, second = self.bake(**options), self.bake(**options)
        self.assertEqual(first, second)
        keys = first["project"]["keys"]
        self.assertEqual([key["position"][0] for key in keys], [0, .5, 1.25, 2.125, 2.125])
        track = LiveTrack(first["project"])
        forward = {time: track.sample(time) for time in [0, 1/128, 1/64, 2/64, 3/64]}
        for time in [3/64, 0, 2/64, 1/128, 1/64, 3/64, 0]:
            self.assertEqual(track.sample(time), forward[time])

    def test_rotation_filter_takes_short_path_across_yaw_wrap(self):
        self.source.records = [row(640, yaw=179), row(641, yaw=-179), row(642, yaw=-178), row(643, yaw=-177)]
        result = self.bake(mode="attach", rotation_half_life=1/64)
        keys = result["project"]["keys"]
        self.assertAlmostEqual(abs(keys[1]["angles"][1]), 180)
        self.assertLess(abs(math.remainder(keys[2]["angles"][1]-keys[1]["angles"][1], 360)), 2)

    def test_loss_rejects_by_default_and_explicit_truncate_never_reacquires(self):
        cases = [(None, "missing"), ({"is_alive": False}, "dead"), ({"is_alive": None}, "unknown_life_state"),
                 ({"health": 0}, "dead"), ({"X": float("nan")}, "invalid_pose"),
                 ({"pitch": None}, "invalid_pose"), ({"X": 200}, "discontinuity")]
        for updates, reason in cases:
            with self.subTest(reason=reason):
                self.source.records = [row(tick) for tick in range(640, 644)]
                if updates is None:
                    self.source.records.pop(2)
                else:
                    self.source.records[2].update(updates)
                with self.assertRaisesRegex(ValueError, f"{reason} at absolute tick 642"):
                    self.bake()
                result = self.bake(on_loss="truncate")
                self.assertEqual(result["evidence"]["loss"], {"tick": 642, "reason": reason})
                self.assertEqual(result["evidence"]["effective"]["end_tick"], 642)
                self.assertEqual(result["project"]["duration"], 2/64)
                self.assertEqual(len(result["project"]["keys"]), 3)
                self.assertEqual(result["project"]["keys"][-1]["position"], result["project"]["keys"][-2]["position"])

    def test_cannot_truncate_before_first_valid_sample(self):
        self.source.records = []
        with self.assertRaisesRegex(ValueError, "missing at absolute tick 640"):
            self.bake(on_loss="truncate")

    def test_name_or_reused_slot_never_selects_another_identity(self):
        self.source.records[1]["steamid"] = int(STEAM_ID)+1
        self.source.records[1]["name"] = "L201742"
        with self.assertRaisesRegex(ValueError, "outside the exact player"):
            self.bake(on_loss="truncate")
        with self.assertRaisesRegex(ValueError, "SteamID64"):
            follow.bake_demo_follow(self.path, "L201742", 640, 644)

    def test_invalid_parser_identity_tick_and_duplicates_are_never_repaired(self):
        mutations = [{"steamid": float(STEAM_ID)}, {"tick": 644}, {"tick": 640.0},
                     {"tick": True}, {"tick": 640}, {"steamid": "0"}]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.source.records = [row(tick) for tick in range(640, 644)]
                self.source.records[1].update(mutation)
                with self.assertRaises(ValueError):
                    self.bake(on_loss="truncate")

    def test_unknown_and_duplicated_roster_identity_fail(self):
        for roster in ([], self.source.players*2, [{"steamid": 0, "name": "L201742"}]):
            with self.subTest(roster=roster):
                self.source.players = roster
                with self.assertRaisesRegex(ValueError, "exactly one recorded player"):
                    self.bake()

    def test_changing_source_cannot_emit_evidence_from_mixed_demo(self):
        original = self.source.parse_ticks
        def changed(*args, **kwargs):
            self.path.write_bytes(self.path.read_bytes()+b"changed")
            return original(*args, **kwargs)
        self.source.parse_ticks = changed
        with self.assertRaisesRegex(ValueError, "Demo changed"):
            self.bake()

    def test_preflight_rejects_unbounded_inputs_before_demo_parse(self):
        intervals = [(False, 5), (1.0, 5), (-1, 5), (5, 5), (6, 5), (0, 1921), (2_000_000_000, 2_000_000_001)]
        for begin, end in intervals:
            with self.subTest(begin=begin, end=end), self.assertRaises(ValueError):
                follow.bake_demo_follow(self.path, STEAM_ID, begin, end)
        for identity in [False, float(STEAM_ID), "01234", "-12", "0", "18446744073709551616"]:
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                follow.bake_demo_follow(self.path, identity, 640, 644)
        bad_options = [{"unknown": True}, {"mode": "bones"}, {"position_half_life": float("nan")},
                       {"rotation_half_life": True}, {"fov": 179}, {"offset": [1, 2]},
                       {"fov": 10**1000}, {"offset": [10**1000, 0, 0]},
                       {"offset": [1, True, 3]}, {"name": "я"*129}, {"mode": "lock_on"},
                       {"camera_position": [0, 0, 0]}, {"mode": "attach", "look_at_offset": [0, 0, 0]},
                       {"angle_offset": [0, 0, 0]}, {"mode": "lock_on", "camera_position": [0, 0, 0], "offset": [0, 0, 0]},
                       {"mode": []}, {"on_loss": "hold"}, {"offset_space": "bones"}]
        for options in bad_options:
            with self.subTest(options=options), self.assertRaises(ValueError):
                follow.bake_demo_follow(self.path, STEAM_ID, 640, 644, options)
        self.parse.assert_not_called()

    def test_undefined_aim_and_out_of_range_solved_position_fail(self):
        for options in [{"offset": [0, 0, 0], "look_at_offset": [0, 0, 0]},
                        {"offset": [0, 0, 100]},
                        {"mode": "attach", "offset": [1_000_000, 0, 0], "offset_space": "world"}]:
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.bake(**options)

    def test_max_duration_stays_below_native_key_and_message_bounds(self):
        self.source.records = [row(tick, X=10) for tick in range(640, 2560)]
        result = follow.bake_demo_follow(self.path, STEAM_ID, 640, 2560)
        self.assertEqual(len(result["project"]["keys"]), 1921)
        self.assertLess(len(json.dumps(result).encode()), 1024*1024)
        LiveTrack(result["project"])


if __name__ == "__main__":
    unittest.main()
