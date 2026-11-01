"""One portable executable for the editor, MCP and offline capture commands."""
import sys


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "index-maps":
        from cs2_mvp.map_geometry import main as index_main
        return index_main(sys.argv[2:])
    if len(sys.argv) > 1 and sys.argv[1] in {"render", "finish", "finalize", "doctor", "inspect", "compose", "bootstrap"}:
        from cs2_mvp.cli import main as capture_main
        return capture_main()
    from cs2_mvp.live import main as live_main
    return live_main()


if __name__ == "__main__":
    raise SystemExit(main())
