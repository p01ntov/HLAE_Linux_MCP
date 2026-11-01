"""Builtin CS2 cosmetics metadata from the user's installed game resources.

This module reads definitions; it never changes inventory or applies a skin.
Item/paint relationships are included only when an installed schema section
explicitly names both. Catalog assets are metadata, not copied Valve models.
"""
from __future__ import annotations

from functools import lru_cache
from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct

from .appearance import VpkIndex, model_resource
from .discovery import find_game

ITEMS_RESOURCE = "scripts/items/items_game.txt"
ICON_PREFIX = "panorama/images/econ/default_generated/"
MAX_TEXT_BYTES = 16 * 1024**2
MAX_CATALOG_BYTES = 16 * 1024**2
MAX_PAIRS = 500000
MAX_QUERY_BYTES = 384 * 1024
CATALOG_SECTIONS = {"items","prefabs","paint_kits","item_sets","client_loot_lists","loot_lists","revolving_loot_lists","item_icons"}
_TOKEN = re.compile(r'\s+|//[^\r\n]*|/\*.*?\*/|"((?:\\.|[^"\\])*)"|([{}])|([^\s{}"]+)', re.DOTALL)


def _unescape(value):
    escapes = {"n":"\n", "r":"\r", "t":"\t", '"':'"', "\\":"\\"}
    return re.sub(r"\\(.)", lambda match: escapes.get(match[1], match[0]), value)


def parse_keyvalues(data, *, allow_duplicate_keys=False, catalog_schema=False):
    """Read bounded text KV1 (not KV3); no include files or execution."""
    if isinstance(data, bytes):
        if len(data) > MAX_TEXT_BYTES:
            raise ValueError("Cosmetic schema text exceeds 16 MiB")
        data = data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
    if len(data.encode("utf-8")) > MAX_TEXT_BYTES:
        raise ValueError("Cosmetic schema text exceeds 16 MiB")
    def tokens():
        cursor = 0
        for match in _TOKEN.finditer(data):
            if match.start() != cursor:
                raise ValueError("Invalid KeyValues token")
            cursor = match.end()
            quoted, brace, bare = match.groups()
            if quoted is not None:
                if len(quoted) > 65536:
                    raise ValueError("Oversized KeyValues string")
                yield (_unescape(quoted), False, True)
            elif brace:
                yield (brace, True, False)
            elif bare:
                yield (bare, False, False)
        if cursor != len(data):
            raise ValueError("Unterminated KeyValues string or comment")
    stream = iter(tokens())
    lookahead = None
    count = 0
    def take():
        nonlocal lookahead
        if lookahead is not None:
            token, lookahead = lookahead, None
            return token
        return next(stream, None)
    def body(depth=0, path=()):
        nonlocal lookahead, count
        if depth > 32:
            raise ValueError("KeyValues nesting exceeds 32 levels")
        result = {}
        while True:
            key = take()
            if key is None:
                if depth:
                    raise ValueError("Truncated KeyValues object")
                return result
            if key[:2] == ("}", True):
                if not depth:
                    raise ValueError("Unexpected KeyValues closing brace")
                return result
            if key[1] or key[0].casefold() in {"#include", "#base"}:
                raise ValueError("Unexpected KeyValues key or unsupported external include")
            value = take()
            if value is None or value[:2] == ("}", True):
                raise ValueError("KeyValues field has no value")
            if value[:2] == ("{", True):
                child = body(depth+1, (*path,key[0]))
            elif value[1]:
                raise ValueError("Unexpected KeyValues brace")
            else:
                child = value[0]
            count += 1
            if count > MAX_PAIRS:
                raise ValueError("KeyValues field count exceeds limit")
            if key[0] in result:
                split_section = catalog_schema and path==("items_game",) and key[0] in CATALOG_SECTIONS
                ignored_section = catalog_schema and len(path)>=2 and path[0]=="items_game" and path[1] not in CATALOG_SECTIONS
                ignored_field = catalog_schema and (path==("items_game","prefabs","campaign_prefab") and key[0]=="image_inventory"
                    or len(path)==3 and path[:2]==("items_game","client_loot_lists") and key[0]=="limit_description_to_number_rnd")
                if split_section and isinstance(result[key[0]],dict) and isinstance(child,dict):
                    overlap=set(result[key[0]]) & set(child)
                    if overlap:
                        raise ValueError("Conflicting repeated cosmetic schema definitions: "+"/".join((*path,key[0],sorted(overlap)[0])))
                    result[key[0]].update(child)
                elif not (allow_duplicate_keys or ignored_section or ignored_field or catalog_schema and path==("items_game",) and key[0] not in CATALOG_SECTIONS):
                    raise ValueError(f"Duplicate KeyValues schema key: {'/'.join((*path,key[0]))}")
                # Valve localization repeats some token names. Schema metadata
                # uses strict mode so conflicting item IDs/fields cannot vanish.
                elif isinstance(result[key[0]], dict) and isinstance(child, dict):
                    result[key[0]] = _merge(result[key[0]], child)
                else:
                    result[key[0]] = child
            else:
                result[key[0]] = child
            following = take()
            if following and not following[1] and not following[2] and following[0].startswith("["):
                # Platform-conditional catalog data needs an explicit adapter;
                # silently choosing a platform branch would be misleading.
                raise ValueError("Platform-conditional KeyValues metadata is unsupported")
            lookahead = following
    return body()


def _merge(base, override):
    result = dict(base)
    for key, value in override.items():
        if key != "used_by_classes" and isinstance(result.get(key), dict) and isinstance(value, dict):
            result[key] = _merge(result[key], value)
        else:
            result[key] = value
    return result


def resolve_definition(value, prefabs, *, chain=()):
    names = value.get("prefab", "").split()
    result = {}
    for name in names:
        if name in chain or len(chain) >= 32:
            raise ValueError("Cosmetic prefab inheritance contains a cycle or exceeds 32 levels")
        prefab = prefabs.get(name)
        if not isinstance(prefab, dict):
            raise ValueError(f"Unknown cosmetic prefab: {name}")
        result = _merge(result, resolve_definition(prefab, prefabs, chain=(*chain,name)))
    return _merge(result, value)


def _number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def _teams(item):
    classes = item.get("used_by_classes", {})
    return [number for name, number in (("terrorists",2),("counter-terrorists",3)) if classes.get(name) == "1"] if isinstance(classes,dict) else []


def _model(value):
    if not isinstance(value,str) or not value.endswith(".vmdl"):
        return None
    try:
        model_resource(value+"_c")
    except ValueError:
        return None
    return value


@lru_cache(maxsize=4)
def _elf_build_id(path, size, modified):
    """Read bounded GNU ELF64 notes without invoking a host binutils command."""
    with Path(path).open("rb") as handle:
        header=handle.read(64)
        if len(header)!=64 or header[:6]!=b"\x7fELF\x02\x01":
            raise ValueError("Installed client is not a little-endian ELF64 library")
        offset=struct.unpack_from("<Q",header,32)[0]
        stride,count=struct.unpack_from("<HH",header,54)
        if not 56<=stride<=4096 or not 1<=count<=256 or offset+stride*count>size:
            raise ValueError("Installed client ELF program headers exceed supported bounds")
        for index in range(count):
            handle.seek(offset+index*stride)
            entry=handle.read(56)
            if len(entry)!=56:
                raise ValueError("Installed client ELF program header is truncated")
            if struct.unpack_from("<I",entry)[0]!=4:
                continue
            note_offset,note_size=struct.unpack_from("<Q",entry,8)[0],struct.unpack_from("<Q",entry,32)[0]
            if note_size>1024*1024 or note_offset+note_size>size:
                raise ValueError("Installed client ELF notes exceed supported bounds")
            handle.seek(note_offset)
            notes=handle.read(note_size)
            cursor=0
            while cursor+12<=len(notes):
                name_size,value_size,kind=struct.unpack_from("<III",notes,cursor)
                start=cursor+12
                value_start=start+((name_size+3)&~3)
                end=value_start+((value_size+3)&~3)
                if end>len(notes):
                    raise ValueError("Installed client ELF note is truncated")
                if kind==3 and notes[start:start+name_size]==b"GNU\0" and 1<=value_size<=128:
                    return notes[value_start:value_start+value_size].hex()
                cursor=end
    raise ValueError("Installed client has no GNU build ID")


def _game_provenance(game):
    result={"game_directory":str(game),"client_build_id":None,"steam_build_id":None,"patch_version":None}
    try:
        client=game/"csgo/bin/linuxsteamrt64/libclient.so"
        info=client.stat()
        result.update(client_build_id=_elf_build_id(str(client),info.st_size,info.st_mtime_ns),
                      client_size=info.st_size,client_mtime_ns=info.st_mtime_ns)
    except (OSError,ValueError) as error:
        result["client_provenance_error"]=str(error)[:512]
    version=game/"csgo/steam.inf"
    if version.is_file() and version.stat().st_size<=65536:
        values=dict(line.split("=",1) for line in version.read_text().splitlines() if "=" in line)
        result["patch_version"]=values.get("PatchVersion")
    if len(game.parents)>2:
        manifest=game.parents[2]/"appmanifest_730.acf"
        if manifest.is_file() and manifest.stat().st_size<=1024*1024:
            match=re.search(r'"buildid"\s*"(\d+)"',manifest.read_text())
            if match:
                result["steam_build_id"]=match[1]
    return result


def build_catalog(schema, tokens, *, language="english", source=None, resource_names=()):
    root = schema.get("items_game",schema)
    if not isinstance(root,dict) or not isinstance(root.get("items"),dict):
        raise ValueError("Installed cosmetic schema has no items object")
    prefabs = root.get("prefabs",{})
    if not isinstance(prefabs,dict):
        raise ValueError("Installed cosmetic prefabs must be an object")
    translations = {key.casefold():value for key,value in tokens.items() if isinstance(value,str)}
    def localize(value, fallback):
        return translations.get(str(value).lstrip("#").casefold(), fallback) if value else fallback
    resources = set(resource_names)
    groups = {"agents":[],"weapons":[],"gloves":[]}
    by_name = {}
    failures = []
    for raw_id, definition in root["items"].items():
        if not raw_id.isdecimal() or not isinstance(definition,dict):
            continue
        name = definition.get("name","")
        if not isinstance(name,str) or not (name.startswith(("customplayer_","weapon_")) or "glove" in name):
            continue
        try:
            item = resolve_definition(definition,prefabs)
        except ValueError as error:
            failures.append({"item_def":int(raw_id),"name":name,"reason":str(error)})
            continue
        item_class = item.get("item_class","")
        slot = item.get("item_slot","")
        if name.startswith("customplayer_"):
            group,kind = "agents","agent"
        elif "glove" in name or item_class in {"wearable_item","wearable"} and slot == "clothing_hands":
            group,kind = "gloves","glove"
        elif item_class.startswith("weapon_") or name.startswith("weapon_"):
            group = "weapons"
            kind = "knife" if item_class == "weapon_knife" or name.startswith("weapon_knife") else "weapon"
        else:
            continue
        # Exclude grenades/C4 and schema-only containers from the skin selector.
        model_player,model_world = _model(item.get("model_player")),_model(item.get("model_world"))
        if group != "agents" and (not model_player or item.get("flexible_loadout_slot") in {"grenade0","grenade1","grenade2","grenade3","grenade4","c4"}
                                  or item_class in {"weapon_c4","weapon_hegrenade","weapon_flashbang","weapon_smokegrenade","weapon_molotov","weapon_incgrenade","weapon_decoy"}):
            continue
        allowed = _teams(item)
        if group == "agents" and (not model_player or len(allowed) != 1):
            failures.append({"item_def":int(raw_id),"name":name,"reason":"Agent requires an explicit single allowed team and a model_player resource"})
            continue
        record = {"item_def":int(raw_id),"name":name,"display_name":localize(item.get("item_name"),name),
                  "kind":kind,"allowed_teams":allowed,"model":model_player,"model_player":model_player,"model_world":model_world,
                  "arms_model":_model(item.get("model_arms") or item.get("arms_model")),
                  "compiled_model":model_player+"_c" if model_player else None,
                  "compiled_model_present":model_player+"_c" in resources if resources and model_player else None,
                  "prefabs":item.get("prefab","").split(),"paint_kits":[]}
        if group == "agents":
            record["team"] = "T" if allowed == [2] else "CT"
            record["map_based"] = name in {"customplayer_ct_map_based","customplayer_t_map_based"}
        by_name[name] = record
        groups[group].append(record)
    paints = {}
    for raw_id,definition in root.get("paint_kits",{}).items():
        if not raw_id.isdecimal() or not isinstance(definition,dict):
            continue
        name = definition.get("name")
        if not isinstance(name,str):
            continue
        paints[name] = {"id":int(raw_id),"name":name,"display_name":localize(definition.get("description_tag"),name),
                        "wear_min":_number(definition.get("wear_remap_min")),"wear_max":_number(definition.get("wear_remap_max")),
                        "material":definition.get("vmt_path"),
                        "category":"glove" if "/paints_gloves/" in str(definition.get("vmt_path","")) else "weapon",
                        "items":[],"association_sources":[],"item_icons":{}}
    def associate(paint,item,source_name,icon=None):
        if item["kind"] == "agent":
            return
        if item["item_def"] not in paint["items"]:
            paint["items"].append(item["item_def"])
        if paint["id"] not in item["paint_kits"]:
            item["paint_kits"].append(paint["id"])
        if source_name not in paint["association_sources"]:
            paint["association_sources"].append(source_name)
        if icon:
            paint["item_icons"][str(item["item_def"])] = icon
    def associations(value, source_name, depth=0):
        if depth > 32 or not isinstance(value,dict):
            return
        for key,child in value.items():
            match = re.fullmatch(r"\[([^\]]+)\](.+)",key)
            if match and match[1] in paints and match[2] in by_name:
                paint,item = paints[match[1]],by_name[match[2]]
                associate(paint,item,source_name)
            associations(child,source_name,depth+1)
    # Read associations only from installed schema sections, never infer glove
    # compatibility from paint-kit number ranges or similarly named assets.
    for name in ("item_sets","client_loot_lists","loot_lists","revolving_loot_lists","item_icons"):
        associations(root.get(name),name)
    icons = {resource for resource in resources if resource.startswith(ICON_PREFIX)}
    for item in by_name.values():
        if item["kind"] == "agent":
            continue
        for paint in paints.values():
            # The exact installed generated inventory icon names both schema
            # identifiers. No paint ID range, approximate prefix, or weapon
            # family convention is used to infer a relationship.
            for wear in ("light","medium","heavy"):
                resource = ICON_PREFIX + item["name"] + "_" + paint["name"] + "_" + wear + "_png.vtex_c"
                if resource in icons:
                    associate(paint,item,"vpk_generated_inventory_icon",resource)
                    break
    for records in groups.values():
        records.sort(key=lambda item:(item["display_name"].casefold(),item["item_def"]))
        for item in records:
            item["paint_kits"].sort()
    paint_records = sorted(paints.values(),key=lambda paint:paint["id"])
    for paint in paint_records:
        paint["items"].sort()
    return {"format":"cs2-camera-cosmetics-catalog","version":1,"language":language,"source":source or {},
            **groups,"paint_kits":paint_records,"unresolved":failures[:128],"unresolved_total":len(failures),
            "notes":["Metadata only; no cosmetic has been applied and no Steam inventory is modified.",
                     "Agent teams come from used_by_classes; runtime application must recheck the target pawn's current team.",
                     "Paint-kit item associations are explicit schema evidence and may be incomplete; absent associations are not guessed.",
                     "Map-based agents may be overridden by map settings; model is the item schema's exact fallback, not a confirmed live entity model."]}


@lru_cache(maxsize=2)
def _installed(path, size, modified, language):
    locale = f"resource/csgo_{language}.txt"
    index = VpkIndex(path,extra_resources=[ITEMS_RESOURCE,locale,"resource/csgo_english.txt"],resource_prefixes=[ICON_PREFIX])
    schema_bytes = index.read_resource(ITEMS_RESOURCE)
    english = parse_keyvalues(index.read_resource("resource/csgo_english.txt"),allow_duplicate_keys=True).get("lang",{}).get("Tokens",{})
    selected = english
    used_language = "english"
    if language != "english" and locale in index.entries:
        selected = _merge(english,parse_keyvalues(index.read_resource(locale),allow_duplicate_keys=True).get("lang",{}).get("Tokens",{}))
        used_language = language
    return build_catalog(parse_keyvalues(schema_bytes,catalog_schema=True), selected, language=used_language, resource_names=index.entries,
                         source={"vpk":str(path),"tree_sha256":index.tree_sha256,
                                 "vpk_size":size,"vpk_mtime_ns":modified,
                                 "items_resource":ITEMS_RESOURCE,"items_crc32":index.entries[ITEMS_RESOURCE].crc32,
                                 "items_game_sha256":hashlib.sha256(schema_bytes).hexdigest()})


def installed_catalog(game_dir=None, language="english"):
    if language not in {"english","russian"}:
        raise ValueError("Catalog language must be english or russian")
    path = find_game(str(game_dir) if game_dir is not None else None)/"csgo/pak01_dir.vpk"
    info = path.stat()
    result = deepcopy(_installed(str(path),info.st_size,info.st_mtime_ns,language))
    result["source"].update(_game_provenance(path.parent.parent))
    return result


def query_catalog(game_dir=None, *, kind="agents", team=None, weapon=None, search="", limit=50, offset=0, language="english"):
    if kind not in {"agents","weapons","knives","gloves"} or team not in {None,"CT","T"}:
        raise ValueError("Cosmetic kind/team must be agents|weapons|knives|gloves and CT|T")
    if (not isinstance(search,str) or len(search)>256 or isinstance(limit,bool) or not isinstance(limit,int)
            or not 1<=limit<=100 or isinstance(offset,bool) or not isinstance(offset,int) or not 0<=offset<=100000):
        raise ValueError("Cosmetic catalog pagination/search exceeds its bounds")
    if weapon is not None and (not isinstance(weapon,str) or not 1<=len(weapon)<=256):
        raise ValueError("Cosmetic item selector must be a bounded exact definition name or item_def string")
    catalog = installed_catalog(game_dir,language)
    rows = catalog["weapons" if kind == "knives" else kind]
    if kind == "knives":
        rows = [row for row in rows if row["kind"] == "knife"]
    if team:
        number = 2 if team == "T" else 3
        rows = [row for row in rows if number in row["allowed_teams"]]
    if weapon:
        identity = str(weapon).casefold()
        rows = [row for row in rows if str(row["item_def"])==identity or row["name"].casefold()==identity]
        if not rows:
            raise ValueError("Weapon/glove must match an exact installed definition name or item_def")
    needle = search.casefold()
    by_paint = {paint["id"]:paint for paint in catalog["paint_kits"]}
    records = []
    for row in rows:
        paints = [by_paint[identity] for identity in row["paint_kits"]]
        base_matches = needle in row["name"].casefold() or needle in row["display_name"].casefold() or needle == str(row["item_def"])
        matching = paints if base_matches else [paint for paint in paints if needle in paint["name"].casefold() or needle in paint["display_name"].casefold() or needle==str(paint["id"])]
        if base_matches or matching:
            # A queried item's icon evidence belongs to that item. Returning all
            # other items' icon maps in every row duplicates megabytes of data.
            selected = [{**paint,"item_icons":{str(row["item_def"]):paint["item_icons"][str(row["item_def"])]}
                        if str(row["item_def"]) in paint["item_icons"] else {}} for paint in matching[:100]]
            records.append({**row,"paint_kits":selected,"paint_kits_total":len(matching),"paint_kits_truncated":len(matching)>100})
    page,used = [],0
    candidates = records[offset:offset+limit]
    for row in candidates:
        size = len(json.dumps(row,ensure_ascii=False,separators=(",",":"),allow_nan=False).encode())
        if size>MAX_QUERY_BYTES:
            raise ValueError("Cosmetic item metadata exceeds the query bound; use a more specific search")
        if page and used+size>MAX_QUERY_BYTES:
            break
        page.append(row)
        used += size
    category = "glove" if kind == "gloves" else "weapon"
    unassigned = [] if kind == "agents" else [paint for paint in catalog["paint_kits"] if not paint["items"] and paint["category"] == category
                  and (not needle or needle in paint["name"].casefold() or needle in paint["display_name"].casefold() or needle == str(paint["id"]))]
    return {"kind":kind,"language":catalog["language"],"source":catalog["source"],"total":len(records),"offset":offset,
            "next_offset":offset+len(page) if offset+len(page)<len(records) else None,"items":page,
            "page_limited_by_bytes":len(page)<len(candidates),
            "unassigned_paint_kits":unassigned[offset:offset+limit],"unassigned_paint_kits_total":len(unassigned),
            "unassigned_next_offset":offset+limit if offset+limit<len(unassigned) else None,
            "notes":catalog["notes"]+["Unassigned paint kits are real definitions with no evidenced item pairing here; do not treat them as compatible with the selected weapon/glove."]}


def validate_agent_team(item_def, team, catalog):
    """Validate team membership only; a map-based choice means restore/reset."""
    if type(item_def) is not int or not 1<=item_def<=65535:
        raise ValueError("Agent item_def must be an integer in 1..65535 from the installed catalog")
    number = {"T":2,"CT":3}.get(team) if isinstance(team,str) else team if type(team) is int and team in (2,3) else None
    if number is None:
        raise ValueError("Target player must have a confirmed current CT or T team")
    match = next((item for item in catalog["agents"] if item["item_def"]==item_def),None)
    if match is None or number not in match["allowed_teams"]:
        raise ValueError("Selected agent is not allowed for the target player's current team")
    return match


def validate_paint_association(item_def, paint_kit, catalog):
    if type(item_def) is not int or not 1<=item_def<=65535:
        raise ValueError("Item definition must be an integer in 1..65535")
    if type(paint_kit) is not int or not 0<=paint_kit<=2147483647:
        raise ValueError("Paint kit must be an integer in 0..2147483647")
    item = next((item for group in ("weapons","gloves") for item in catalog[group] if item["item_def"]==item_def),None)
    if item is None or paint_kit not in item["paint_kits"]:
        raise ValueError("This item/paint-kit relationship is not evidenced by the installed cosmetic schema")
    return item


def stage_catalog(game_dir, path, *, language="english"):
    catalog = installed_catalog(game_dir,language)
    data = json.dumps(catalog,ensure_ascii=False,separators=(",",":"),allow_nan=False).encode()
    if len(data)>MAX_CATALOG_BYTES:
        raise ValueError("Cosmetic catalog exceeds the native 16 MiB bound")
    path = Path(path)
    with os.fdopen(os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY|os.O_CLOEXEC,0o600),"wb") as handle:
        os.fchmod(handle.fileno(),0o600)
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    return path
