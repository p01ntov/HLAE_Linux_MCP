import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cs2_mvp import demo_analysis as analysis
from cs2_mvp.live_mcp import ProtocolError, Server, call_tool


class Frame:
    def __init__(self, rows):
        self.rows = rows

    def __len__(self):
        return len(self.rows)

    def to_dict(self, orient):
        return self.rows


def death(tick, attacker="11", victim="22", weapon="awp", round=0, **extra):
    return {"tick": tick, "attacker_name": "Alice", "attacker_steamid": attacker,
            "user_name": "Bob", "user_steamid": victim, "weapon": weapon,
            "attacker_team_name": "CT", "user_team_name": "TERRORIST",
            "attacker_X": 10, "attacker_Y": 20, "attacker_Z": 30,
            "user_X": 100, "user_Y": 200, "user_Z": 300,
            "total_rounds_played": round, "is_warmup_period": False,
            "headshot": True, **extra}


class Parser:
    def __init__(self):
        self.deaths = [death(640), death(704), death(768, weapon="taser"),
                       death(832, is_warmup_period=True), death(896, victim="11"),
                       death(1024, round=1), death(1088, round=1), death(1152, attacker="22", victim="11")]
        self.players = [{"steamid": 11, "name": "Alice", "team_number": 3}, {"steamid": 22, "name": "Bob", "team_number": 2}]
        self.tick_queries = []

    def parse_header(self):
        return {"map_name": "de_test"}

    def parse_player_info(self):
        return Frame(self.players)

    def parse_event(self, event, **kwargs):
        if event == "player_death":
            assert "total_rounds_played" in kwargs["other"]
            assert "team_name" in kwargs["player"]
            return Frame(self.deaths)
        return Frame([{"tick": 1, "total_rounds_played": 0, "is_warmup_period": False},
                      {"tick": 1000, "total_rounds_played": 1, "is_warmup_period": False}])

    def parse_ticks(self, properties, **kwargs):
        self.tick_queries.append((properties, kwargs))
        return Frame([{"tick": 640, "steamid": 11, "X": 1, "Y": 2, "Z": 3, "pitch": -5,
                       "yaw": 75, "health": 100, "is_alive": True, "team_name": "CT"}])


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "match.dem"
        self.path.write_bytes(b"PBDEMS2\0test")
        self.demo = Parser()
        analysis._index.cache_clear()
        self.mock = patch.object(analysis, "parser", return_value=self.demo).start()
        self.addCleanup(patch.stopall)
        self.addCleanup(analysis._index.cache_clear)

    def test_inspect_reports_actual_round_starts_and_string_ids(self):
        result = analysis.inspect(self.path)
        self.assertEqual(result["players"][0]["steamid"], "11")
        self.assertEqual(result["rounds"][1], {"round": 2, "start_tick": 1000, "start_seconds": 15.625, "warmup": False})
        self.assertEqual(result["weapons"], ["awp", "taser"])
        self.assertEqual(result["observed_event_ticks"]["last"], 1152)

    def test_multikills_filter_before_counting_and_do_not_cross_rounds(self):
        result = analysis.find_moments(self.path, player="alice", weapons=["weapon_AWP"], min_kills=2, window_seconds=10)
        self.assertEqual([moment["kill_count"] for moment in result["moments"]], [2, 2])
        self.assertEqual([moment["round"] for moment in result["moments"]], [1, 2])
        self.assertEqual(result["moments"][0]["start_tick"], 640)
        self.assertEqual(result["moments"][0]["end_seconds"], 11)
        self.assertEqual(result["moments"][0]["kills"][0]["attacker"]["position"], [10, 20, 30])

    def test_tick_and_second_intervals_are_exact_and_end_exclusive(self):
        first = analysis.find_moments(self.path, start_tick=640, end_tick=768)
        second = analysis.find_moments(self.path, start_seconds=10, end_seconds=12)
        self.assertEqual(first["moments"], second["moments"])
        self.assertEqual([m["start_tick"] for m in first["moments"]], [640, 704])
        narrow = analysis.find_moments(self.path, start_seconds=10.001, end_seconds=11.001)
        self.assertEqual([m["start_tick"] for m in narrow["moments"]], [704])

    def test_victim_round_and_headshot_filters_use_recorded_values(self):
        self.demo.deaths[-1]["headshot"] = False
        self.demo.deaths[-1]["total_rounds_played"] = 1
        result = analysis.find_moments(self.path, player="11", role="victim", round=2, headshot=False)
        self.assertEqual(result["matched_kills"], 1)
        self.assertEqual(result["moments"][0]["start_tick"], 1152)

    def test_restarted_round_numbers_do_not_merge_separate_round_starts(self):
        self.demo.deaths = [death(640), death(1024)]
        original = self.demo.parse_event
        def events(name, **kwargs):
            if name == "round_start":
                return Frame([{"tick": 1, "total_rounds_played": 0, "is_warmup_period": False},
                              {"tick": 1000, "total_rounds_played": 0, "is_warmup_period": False}])
            return original(name, **kwargs)
        self.demo.parse_event = events
        self.assertEqual(analysis.find_moments(self.path, min_kills=2, window_seconds=20)["matched_moments"], 0)

    def test_invalid_query_fails_before_parser_and_ambiguous_player_never_guessed(self):
        for query in ({"start_tick": 640}, {"start_tick": 700, "end_tick": 640},
                      {"start_tick": 1, "end_tick": 2, "start_seconds": 1, "end_seconds": 2},
                      {"role": "victim", "min_kills": 2}):
            with self.subTest(query=query), self.assertRaises(ValueError):
                analysis.find_moments(self.path, **query)
        self.mock.assert_not_called()
        self.demo.players[1]["name"] = "Alice"
        with self.assertRaisesRegex(ValueError, "exactly one"):
            analysis.find_moments(self.path, player="Alice")
        self.assertEqual(analysis.find_moments(self.path, player="11")["player"]["steamid"], "11")

    def test_unknown_warmup_and_non_enemy_deaths_are_not_claimed_as_match_kills(self):
        self.demo.deaths = [death(640, is_warmup_period=None), death(704, user_team_name="CT"), death(768)]
        self.assertEqual(analysis.find_moments(self.path)["matched_kills"], 1)
        self.assertEqual(analysis.find_moments(self.path, include_warmup=True, include_non_enemy=True)["matched_kills"], 3)

    def test_read_only_pagination_and_fingerprint_cache_do_not_mutate_results(self):
        result = analysis.find_moments(self.path, limit=1)
        self.assertEqual(result["next_offset"], 1)
        next_page = analysis.find_moments(self.path, limit=1, offset=1)
        self.assertEqual(next_page["moments"][0]["start_tick"], 704)
        self.assertEqual(self.mock.call_count, 1)
        self.path.write_bytes(self.path.read_bytes() + b"changed")
        analysis.inspect(self.path)
        self.assertEqual(self.mock.call_count, 2)

    def test_position_samples_request_only_exact_player_ticks_and_report_missing(self):
        result = analysis.player_positions(self.path, "11", [704, 640, 640])
        self.assertEqual(self.demo.tick_queries[0][1], {"players": [11], "ticks": [640, 704]})
        self.assertEqual(result["missing_ticks"], [704])
        self.assertEqual(result["samples"][0]["angles"], [-5, 75])

    def test_nonfinite_parser_positions_become_null_json_not_invented_coordinates(self):
        self.demo.deaths[0]["attacker_X"] = float("nan")
        result = analysis.find_moments(self.path, limit=1)
        self.assertIsNone(result["moments"][0]["kills"][0]["attacker"]["position"])
        json.dumps(result, allow_nan=False)

    def test_mcp_offline_works_without_native_requests_and_defaults_to_session_demo(self):
        class Client:
            def request(self, *args):
                raise AssertionError("Offline analysis must not send engine requests")
        with patch("cs2_mvp.live_mcp.session_descriptor", return_value={"demo": str(self.path)}):
            result = call_tool(Client(), "cs2_demo_find_moments", {"weapons": ["taser"]})
        self.assertEqual(result["moments"][0]["start_tick"], 768)
        with self.assertRaises(ProtocolError):
            call_tool(Client(), "cs2_demo_player_positions", {"demo": str(self.path), "player": "11", "ticks": list(range(33))})
        with self.assertRaises(ProtocolError):
            call_tool(Client(), "cs2_demo_find_moments", {"demo": str(self.path), "limit": 100})

    def test_parser_failure_is_a_tool_error_and_stdio_server_can_continue(self):
        server = Server(None)
        server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize"})
        self.mock.side_effect = Exception("unsupported demo build")
        response = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                  "params": {"name": "cs2_demo_inspect", "arguments": {"demo": str(self.path)}}})
        self.assertTrue(response["result"]["isError"])
        self.assertIn("unsupported demo build", response["result"]["content"][0]["text"])
        self.assertEqual(server.handle({"jsonrpc": "2.0", "id": 3, "method": "ping"})["result"], {})


if __name__ == "__main__":
    unittest.main()
