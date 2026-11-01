from __future__ import annotations

from pathlib import Path

from demoparser2 import DemoParser


def parser(path: Path) -> DemoParser:
    if not path.is_file() or path.suffix.lower() != ".dem":
        raise ValueError(f"Not a demo file: {path}")
    with path.open("rb") as source:
        if source.read(8) != b"PBDEMS2\x00":
            raise ValueError("Expected a CS2 PBDEMS2 demo")
    return DemoParser(str(path))


def inspect_demo(path: Path, include_kills: bool = False) -> dict:
    demo = parser(path)
    players = demo.parse_player_info().to_dict(orient="records")
    for player in players:
        player["steamid"] = str(player["steamid"])
    result = {"path": str(path.resolve()), "header": demo.parse_header(), "players": players}
    if include_kills:
        columns = ["tick", "attacker_name", "attacker_steamid", "user_name", "weapon", "headshot"]
        kills = demo.parse_event("player_death")
        result["kills"] = kills[[column for column in columns if column in kills]].to_dict(orient="records")
    return result


def resolve_player(path: Path, identity: str, start: int, end: int) -> dict:
    demo = parser(path)
    players = demo.parse_player_info().to_dict(orient="records")
    matches = [p for p in players if str(p["steamid"]) == identity or p["name"].casefold() == identity.casefold()]
    if len(matches) != 1:
        raise ValueError("Player must match exactly one nickname or SteamID64; use inspect to list players")
    player = matches[0]
    steam_id = str(player["steamid"])
    # In SourceTV demos CSDM uses spec_player (user_id + 1), not pawn entity_id.
    sample_ticks = sorted({max(1, start - 64), start, max(start, end - 1)})
    # Check the complete interval so a reconnect/slot change between the
    # boundary samples cannot silently select a different player.
    rows = demo.parse_ticks(["user_id", "is_alive"], players=[int(steam_id)])
    rows = rows[(rows["tick"] >= sample_ticks[0]) & (rows["tick"] < end)]
    records = rows.to_dict(orient="records")
    present = {int(row["tick"]) for row in records}
    if set(sample_ticks) - present:
        raise ValueError(f"Player missing at requested verification ticks: {set(sample_ticks) - present}")
    slots = {int(row["user_id"]) + 1 for row in records if row.get("user_id") is not None}
    if len(slots) != 1 or not all(1 <= slot <= 64 for slot in slots):
        raise ValueError("Could not resolve a stable spectator slot; refuse to guess POV")
    for row in records:
        if int(row["tick"]) in {max(1, start - 64), start} and row.get("is_alive") is not True:
            raise ValueError("The target must be alive at setup and capture start to select its first-person POV")
    return {"steam_id": steam_id, "name": player["name"], "slot": slots.pop(),
            "samples": [row for row in records if int(row["tick"]) in sample_ticks],
            "checked_ticks": len(present), "alive_throughout": all(row.get("is_alive") is True for row in records)}
