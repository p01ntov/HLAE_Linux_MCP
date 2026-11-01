"""MCP discovery remains usable before optional demo/GUI modules are loaded."""
import json
from pathlib import Path
import subprocess
import sys
import textwrap
import unittest


class FollowMcpDependencyTests(unittest.TestCase):
    def test_tool_discovery_needs_only_the_standard_library(self):
        source = Path(__file__).resolve().parents[1] / "src"
        script = textwrap.dedent("""
            import importlib.abc
            import json
            import sys

            sys.path.insert(0, sys.argv[1])
            blocked = {"demoparser2", "numpy", "pandas", "PySide6", "websockets"}

            class RejectOptionalImports(importlib.abc.MetaPathFinder):
                def find_spec(self, fullname, path=None, target=None):
                    if fullname.partition(".")[0] in blocked:
                        raise AssertionError("MCP discovery eagerly imported " + fullname)

            sys.meta_path.insert(0, RejectOptionalImports())
            from cs2_mvp.live_mcp import Server

            class NoGameClient:
                def request(self, *args, **kwargs):
                    raise AssertionError("MCP discovery contacted the game")

            server = Server(NoGameClient())
            initialized = server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize"})
            tools = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
            resources = server.handle({"jsonrpc": "2.0", "id": 3, "method": "resources/list"})
            guide = server.handle({"jsonrpc": "2.0", "id": 4, "method": "resources/read",
                                   "params": {"uri": "cs2-camera://guides/agent"}})
            assert not blocked.intersection(sys.modules)
            print(json.dumps({"initialized": initialized, "tools": tools,
                              "resources": resources, "guide": guide}))
        """)
        # -S removes site-packages even when the test itself runs in the full
        # release venv; the finder also catches accidental optional imports.
        result = subprocess.run([sys.executable, "-S", "-c", script, str(source)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        responses = json.loads(result.stdout)
        self.assertIn("serverInfo", responses["initialized"]["result"])
        tools = {item["name"]: item for item in responses["tools"]["result"]["tools"]}
        follow = tools["cs2_demo_follow_draft"]
        self.assertTrue(follow["annotations"]["readOnlyHint"])
        options = follow["inputSchema"]["properties"]["options"]
        self.assertFalse(options["additionalProperties"])
        self.assertEqual(options["properties"]["mode"]["enum"], ["look_at", "attach", "lock_on"])
        self.assertIn("cs2_camera_set_lens", tools)
        self.assertIn("cs2_camera_import_project", tools)
        self.assertEqual(responses["resources"]["result"]["resources"][0]["uri"],
                         "cs2-camera://guides/agent")
        self.assertTrue(responses["guide"]["result"]["contents"][0]["text"])


if __name__ == "__main__":
    unittest.main()
