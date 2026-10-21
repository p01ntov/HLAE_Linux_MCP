#include "../capture_environment.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using cs2_mvp::CaptureEnvironment;
using Json = CaptureEnvironment::Json;
namespace fs = std::filesystem;

static std::string Hex(const std::string& bytes) {
    static const char* alphabet = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) { result += alphabet[byte >> 4]; result += alphabet[byte & 15]; }
    return result;
}
static std::string Action(const std::string& kind, const Json& params) {
    return "cs2_mvp_" + kind + " " + Hex(params.dump());
}
template<class F> static std::string Reject(F operation) {
    try { operation(); }
    catch (const std::exception& error) { return error.what(); }
    assert(false && "expected operation rejection"); return {};
}
static Json Logged(const std::string& line, const std::string& marker) {
    const auto at = line.find(marker); assert(at != std::string::npos);
    return Json::parse(line.substr(at + marker.size()));
}

struct FakeScene {
    std::map<std::string, cs2_live::SceneCvar> values;
    std::vector<std::pair<std::string, std::string>> writes;
    std::string ignoreName, ignoreValue, rejectName, rejectValue, quantizeName;
    FakeScene() {
        for (const auto& control : cs2_live::SceneControls()) {
            cs2_live::SceneCvar value;
            value.available = true;
            value.nativeType = control.boolean ? "bool" : "float32";
            value.value = control.boolean ? Json(true) : Json(1.0);
            if (std::string(control.id) == "depth_of_field" || std::string(control.id) == "dof_override") value.value = false;
            if (std::string(control.id) == "dof_near_blurry") value.value = -100.0;
            if (std::string(control.id) == "dof_near_crisp") value.value = 0.0;
            if (std::string(control.id) == "dof_far_crisp") value.value = 180.0;
            if (std::string(control.id) == "dof_far_blurry") value.value = 2000.0;
            value.raw = value.value.is_boolean() ? (value.value.get<bool>() ? "1" : "0") : cs2_live::SceneNumberText(value.value.get<double>());
            values[control.cvar] = value;
        }
        auto& fullbright = values.at("mat_fullbright");
        fullbright.integer = true; fullbright.nativeType = "int32"; fullbright.raw = "2"; fullbright.value = 2;
    }
    bool Write(const std::string& name, const std::string& raw) {
        writes.push_back({name, raw});
        if (name == rejectName && raw == rejectValue) return false;
        if (name == ignoreName && raw == ignoreValue) return true;
        auto& entry = values.at(name);
        entry.raw = raw;
        if (entry.nativeType == "bool") entry.value = raw == "1" || raw == "true";
        else if (entry.integer) entry.value = std::stoi(raw);
        else {
            double number = std::stod(raw);
            if (name == quantizeName) { number = std::round(number * 1000000) / 1000000; entry.raw = cs2_live::SceneNumberText(number); }
            entry.value = number;
        }
        return true;
    }
    void Bind(CaptureEnvironment& environment) {
        environment.BindScene([this](const std::string& name) { return values.at(name); },
            [this](const std::string& name, const std::string& raw) { return Write(name, raw); });
    }
};

struct FakeAppearance {
    fs::path temporary;
    std::string directory, model = "characters/test/capture.vmdl";
    std::vector<std::string> mounted, reloads, calls;
    unsigned failReloads = 0;
    FakeAppearance() {
        char pattern[] = "/tmp/cs2-capture-environment-test-XXXXXX";
        const auto root = ::mkdtemp(pattern); assert(root); temporary = root;
        directory = (temporary / "appearance").string();
        const auto file = fs::path(directory) / (model + "_c");
        fs::create_directories(file.parent_path());
        std::string content(36, '\0');
        const auto write = [&](std::size_t offset, std::uint32_t value) {
            for (int i = 0; i != 4; ++i) content[offset + i] = static_cast<char>((value >> (i * 8)) & 255);
        };
        write(0, content.size()); content[4] = 12; content[6] = 1;
        write(8, 8); write(12, 1); write(16, 0x41544144); write(20, 8); write(24, 8);
        content.replace(28, 8, "testdata");
        std::ofstream output(file, std::ios::binary); output.write(content.data(), content.size()); output.close();
        assert(output.good());
    }
    ~FakeAppearance() { std::error_code ignored; fs::remove_all(temporary, ignored); }
    Json Params() const { return {{"path", directory}, {"models", {model}}}; }
    cs2_live::AppearancePack::Callbacks Callbacks() {
        return {
            [this](const std::string& path) {
                calls.push_back("mount:" + path); mounted.insert(mounted.begin(), path); return true;
            },
            [this](const std::string& path) {
                calls.push_back("unmount:" + path);
                mounted.erase(std::remove(mounted.begin(), mounted.end(), path), mounted.end()); return true;
            },
            [this](const std::string& resource) {
                calls.push_back("read:" + resource);
                for (const auto& root : mounted) {
                    const auto path = fs::path(root) / resource;
                    if (!fs::is_regular_file(path)) continue;
                    std::ifstream stream(path, std::ios::binary);
                    return cs2_live::AppearancePack::Bytes(std::istreambuf_iterator<char>(stream), {});
                }
                return cs2_live::AppearancePack::Bytes();
            },
            [this](const std::string& resource) {
                calls.push_back("reload:" + resource); reloads.push_back(resource);
                if (failReloads) { --failReloads; return false; }
                return true;
            }
        };
    }
};

static void TestDecode() {
    const Json utf8 = {{"name", "Камера — пролётка 🎬"}, {"exposure", 1.25}};
    assert(CaptureEnvironment::Decode(Hex(utf8.dump())) == utf8);
    assert(CaptureEnvironment::Decode("7B7D") == Json::object());
    for (const auto& invalid : {std::string(), std::string("0"), std::string("zz"), std::string("7x"), Hex("not JSON"),
            Hex("null"), Hex("[]"), Hex("1"), Hex("true"), Hex("\"text\""), Hex(std::string("{\"x\":\"\xc0\"}", 9))})
        Reject([&] { CaptureEnvironment::Decode(invalid); });
    std::string atLimit = "{}"; atLimit.resize(CaptureEnvironment::MaxPayload, ' ');
    assert(CaptureEnvironment::Decode(Hex(atLimit)) == Json::object());
    atLimit += ' ';
    Reject([&] { CaptureEnvironment::Decode(Hex(atLimit)); });
    assert(CaptureEnvironment::IsAction(Action("scene", Json::object())));
    assert(CaptureEnvironment::IsAction(Action("appearance", Json::object())));
    assert(!CaptureEnvironment::IsAction("camera_begin"));
}

static void TestSceneReadbackAndRestore() {
    CaptureEnvironment environment; FakeScene engine; engine.Bind(environment);
    const auto original = engine.values;
    std::vector<std::string> logs;
    const auto log = [&](const std::string& line) { logs.push_back(line); };
    engine.quantizeName = "mat_tonemap_force_scale";
    const Json first = {{"exposure", 1.2345678}, {"fullbright", false}, {"hud_visible", false}};
    environment.Handle(Action("scene", first), 4321, log);
    assert(logs.size() == 1 && logs.back().find("CS2_MVP_SCENE_APPLIED tick=4321 settings=") == 0);
    const auto actual = Logged(logs.back(), " settings=");
    assert(actual["exposure"] == engine.values.at("mat_tonemap_force_scale").value);
    assert(actual["exposure"] != first["exposure"]); // Log engine readback, not requested JSON.
    assert(actual["fullbright"] == false && actual["hud_visible"] == false);
    assert(engine.values.at("mat_fullbright").value == 0);
    engine.quantizeName.clear();
    environment.Handle(Action("scene", {{"exposure", 3}, {"bloom_strength", 2}}), 4322, log);
    assert(engine.values.at("mat_fullbright").value == 2);
    assert(engine.values.at("cl_drawhud").value == true);
    environment.Handle(Action("scene", {{"depth_of_field", true}, {"dof_radius", 4}}), 4323, log);
    assert(engine.values.at("mat_tonemap_force_scale").value == original.at("mat_tonemap_force_scale").value);
    environment.Restore(4330, "camera_end", log);
    assert(logs.back() == "CS2_MVP_ENVIRONMENT_RESTORED tick=4330 reason=camera_end");
    for (const auto& entry : original) {
        assert(engine.values.at(entry.first).value == entry.second.value);
        assert(engine.values.at(entry.first).raw == entry.second.raw);
    }
    environment.Restore(4331, "shutdown", log);
    assert(logs.back() == "CS2_MVP_ENVIRONMENT_RESTORED tick=4331 reason=shutdown");
}

static void TestRejectedSceneHasNoPartialEffects() {
    CaptureEnvironment environment; FakeScene engine; engine.Bind(environment);
    std::vector<std::string> logs;
    const auto log = [&](const std::string& line) { logs.push_back(line); };
    for (const auto& settings : std::vector<Json>{
        {{"exposure", 2}, {"unknown", 3}}, {{"exposure", 2}, {"fullbright", 1}},
        {{"bloom_strength", 2}, {"exposure", "3"}}, {{"dof_near_crisp", 3000}}
    }) {
        Reject([&] { environment.Handle(Action("scene", settings), 500, log); });
        assert(engine.writes.empty() && logs.empty());
    }
    engine.values.at("r_dof2_radiusscale").available = false;
    Reject([&] { environment.Handle(Action("scene", {{"exposure", 2}, {"dof_radius", 3}}), 501, log); });
    assert(engine.writes.empty() && logs.empty());
    engine.values.at("r_dof2_radiusscale").available = true;
    engine.ignoreName = "mat_tonemap_force_scale"; engine.ignoreValue = "4";
    const auto error = Reject([&] { environment.Handle(Action("scene", {{"bloom_strength", 2}, {"exposure", 4}}), 502, log); });
    assert(error.find("readback") != std::string::npos && error.find("previous scene values restored") != std::string::npos);
    assert(engine.values.at("r_csgo_render_post_bloom_strength").value == 1);
    assert(engine.values.at("mat_tonemap_force_scale").value == 1);
    assert(engine.writes.size() == 4 && logs.empty());
    engine.writes.clear();
    Reject([&] { environment.Handle("cs2_mvp_scene ff", 503, log); });
    Reject([&] { environment.Handle("cs2_mvp_scene", 504, log); });
    Reject([&] { environment.Handle("quit", 505, log); });
    assert(engine.writes.empty() && logs.empty());
}

static void TestAppearanceLoggingAndFailureRollback() {
    CaptureEnvironment environment; FakeAppearance engine;
    environment.BindAppearance(engine.Callbacks());
    std::vector<std::string> logs;
    const auto log = [&](const std::string& line) { logs.push_back(line); };
    environment.Handle(Action("appearance", engine.Params()), 89624, log);
    assert(logs.size() == 1 && logs.back().find("CS2_MVP_APPEARANCE_APPLIED tick=89624 state=") == 0);
    const auto state = Logged(logs.back(), " state=");
    assert(state["active"] == true && state["supported"] == true);
    assert(state["path"] == engine.directory && state["models"] == Json::array({engine.model}));
    assert(state["scope"] == "global resource replacement");
    assert(state["mounted_verified"] == true && state["reload_requested"] == true);
    assert(state["render_verified"] == false && state["compiled_resource_verified"] == false);
    assert(engine.calls == (std::vector<std::string>{"mount:" + engine.directory, "read:" + engine.model + "_c", "reload:" + engine.model}));
    environment.Restore(89696, "capture_end", log);
    assert(engine.mounted.empty() && engine.reloads.size() == 2);
    assert(logs.back() == "CS2_MVP_ENVIRONMENT_RESTORED tick=89696 reason=capture_end");
    logs.clear(); engine.calls.clear(); engine.reloads.clear(); engine.failReloads = 1;
    const auto error = Reject([&] { environment.Handle(Action("appearance", engine.Params()), 89700, log); });
    assert(error.find("reload_model request failed") != std::string::npos);
    assert(engine.mounted.empty() && engine.reloads.size() == 2 && logs.empty());
    assert(engine.calls[3] == "unmount:" + engine.directory);
    environment.Restore(89701, "failed_capture", log);
    assert(logs.back() == "CS2_MVP_ENVIRONMENT_RESTORED tick=89701 reason=failed_capture");
}
static void TestIncompleteSceneRollbackRetainsRecoveryJournal() {
    CaptureEnvironment environment;FakeScene engine;engine.Bind(environment);std::vector<std::string> logs;
    const auto log=[&](const auto& line){logs.push_back(line);};
    engine.ignoreName="mat_tonemap_force_scale";engine.ignoreValue="4";
    engine.rejectName="r_csgo_render_post_bloom_strength";engine.rejectValue="1";
    const auto error=Reject([&]{environment.Handle(Action("scene",{{"bloom_strength",2},{"exposure",4}}),500,log);});
    assert(error.find("refused complete rollback")!=std::string::npos);
    assert(engine.values.at("r_csgo_render_post_bloom_strength").value==2 && logs.empty());
    Reject([&]{environment.Restore(501,"capture_error",log);});assert(logs.empty());
    engine.rejectName.clear();environment.Restore(502,"restore_retry",log);
    assert(engine.values.at("r_csgo_render_post_bloom_strength").value==1);
    assert(logs.back()=="CS2_MVP_ENVIRONMENT_RESTORED tick=502 reason=restore_retry");
}

static void TestRestoreAttemptsBothBackends() {
    CaptureEnvironment environment; FakeScene scene; FakeAppearance appearance;
    scene.Bind(environment); environment.BindAppearance(appearance.Callbacks());
    std::vector<std::string> logs;
    const auto log = [&](const std::string& line) { logs.push_back(line); };
    environment.Handle(Action("scene", {{"exposure", 2}}), 800, log);
    environment.Handle(Action("appearance", appearance.Params()), 800, log);
    logs.clear();
    scene.rejectName = "mat_tonemap_force_scale"; scene.rejectValue = "1";
    Reject([&] { environment.Restore(810, "capture_error", log); });
    assert(appearance.mounted.empty() && appearance.reloads.size() == 2);
    assert(scene.values.at("mat_tonemap_force_scale").value == 2);
    assert(logs.empty()); // An incomplete scene restore must not log success.
    scene.rejectName.clear();
    environment.Restore(811, "restore_retry", log);
    assert(scene.values.at("mat_tonemap_force_scale").value == 1);
    assert(logs.back() == "CS2_MVP_ENVIRONMENT_RESTORED tick=811 reason=restore_retry");
}

struct FakeCosmetics {
    using Players=cs2_live::PlayerCosmetics;using Weapons=cs2_live::WeaponCosmetics;
    Players::Actor actor;Weapons::Weapon weapon;
    unsigned modelWrites=0,colorWrites=0,paintWrites=0;bool hidden=false,rejectModel=false,rejectPaint=false;
    FakeCosmetics() {
        actor.steamId="76561198012345678";actor.nickname="capture player";actor.team=2;
        actor.pawnHandle=32775;actor.controllerHandle=32771;actor.instance=0x1000;actor.generation=1;
        actor.model="characters/map_t.vmdl";actor.color={12,34,56,255};actor.modelReady=actor.colorReady=true;
        weapon.actor=actor;weapon.handle=98320;weapon.instance=0x2000;weapon.itemDef=7;weapon.ready=weapon.hudReady=true;
        weapon.worldMaterialJobs=weapon.hudMaterialJobs=0;
    }
    void Bind(CaptureEnvironment& environment,cs2_live::GloveCosmetics::Callbacks gloveCallbacks={}) {
        Players::Callbacks players;players.agents=players.palette=true;
        players.catalog=cs2_live::CosmeticsCatalog::Parse({{"format","cs2-camera-cosmetics-catalog"},{"version",1},{"agents",Json::array({
            {{"kind","agent"},{"item_def",1},{"name","agent"},{"display_name","Agent"},{"team","T"},{"allowed_teams",{2}},{"model","characters/agent.vmdl"}}})}});
        players.actors=[&]{return hidden?std::vector<Players::Actor>{}:std::vector<Players::Actor>{actor};};
        players.current=[&](const auto& a){return !hidden && a.instance==actor.instance && a.pawnHandle==actor.pawnHandle && a.team==actor.team;};
        players.model=[&](const auto&,const auto& model){++modelWrites;if(rejectModel)return false;actor.model=model;return true;};
        players.color=[&](const auto&,const auto& color){++colorWrites;actor.color=color;return true;};
        Weapons::Callbacks weapons;weapons.available=true;
        weapons.weapons=[&]{weapon.actor=actor;return hidden?std::vector<Weapons::Weapon>{}:std::vector<Weapons::Weapon>{weapon};};
        weapons.current=[&](const auto& w){return !hidden && w.handle==weapon.handle && w.instance==weapon.instance;};
        weapons.resolve=[](const Json& value){if(value["item_def"]!=7 || value["paint_kit"]!=180)throw std::invalid_argument("not an installed paint pair");return Weapons::Paint{180,value.value("seed",0u),value.value("wear",0.01f)};};
        weapons.attributes=[&](const auto&,const auto& attrs){++paintWrites;if(rejectPaint)return false;weapon.attributes=attrs;return true;};
        environment.BindCosmetics(std::move(players),std::move(weapons),std::move(gloveCallbacks));
    }
    Json Desired()const{return {{"players",Json::array({{{"steam_id",actor.steamId},{"agent_item_def",1}}})},
        {"weapon_paints",Json::array({{{"steam_id",actor.steamId},{"item_def",7},{"paint_kit",180},{"seed",42},{"wear",0.1}}})},
        {"team_palette",{{"enabled",true},{"t",{255,0,0}},{"ct",{0,0,255}}}}};}
};

static void TestCosmeticsPreflightReadbackAndRestore() {
    CaptureEnvironment environment;FakeCosmetics engine;engine.Bind(environment);std::vector<std::string> logs;
    const auto log=[&](const auto& line){logs.push_back(line);};
    auto desired=engine.Desired(),invalid=desired;invalid["weapon_paints"][0]["paint_kit"]=999;
    Reject([&]{environment.Handle(Action("cosmetics",invalid),900,log);});
    assert(engine.modelWrites==0 && engine.colorWrites==0 && engine.paintWrites==0 && logs.empty());
    environment.Handle(Action("cosmetics",desired),900,log);
    assert(engine.modelWrites==1 && engine.colorWrites==1 && engine.paintWrites==1 && logs.empty());
    environment.Frame(901,false,log);environment.BeforeMovie(964,log);
    assert(logs.size()==1 && logs[0].find("CS2_MVP_COSMETICS_APPLIED tick=901 proof=")==0);
    const auto proof=Logged(logs[0]," proof=");assert(proof["settings"]==desired);
    assert(proof["players"]["players"][0]["agent_applied"]==true && proof["players"]["players"][0]["palette_applied"]==true);
    assert(proof["weapon_cosmetics"]["weapons"][0]["applied"]==true);
    // Network installation on this exact item becomes the fresh reset baseline.
    engine.actor.model="characters/respawn_t.vmdl";engine.weapon.attributes={12.f,3.f,0.4f};
    environment.Frame(1000,true,log);assert(engine.actor.model=="characters/agent.vmdl" && engine.weapon.attributes[0]==180.f);
    environment.Restore(1036,"quit_after_flush",log);
    assert(engine.actor.model=="characters/respawn_t.vmdl" && engine.actor.color==FakeCosmetics::Players::Color({12,34,56,255}));
    assert(engine.weapon.attributes==FakeCosmetics::Weapons::Attributes({12.f,3.f,0.4f}));
    assert(logs[logs.size()-2]=="CS2_MVP_COSMETICS_RESTORED tick=1036 players_owned=0 weapons_owned=0 gloves_owned=0");
    assert(logs.back()=="CS2_MVP_ENVIRONMENT_RESTORED tick=1036 reason=quit_after_flush");
}
static void TestCosmeticsUnconfirmedNeverStartsRecording() {
    CaptureEnvironment environment;FakeCosmetics engine;engine.Bind(environment);std::vector<std::string> logs;
    const auto log=[&](const auto& line){logs.push_back(line);};
    engine.rejectModel=true;environment.Handle(Action("cosmetics",engine.Desired()),900,log);
    Reject([&]{environment.BeforeMovie(964,log);});assert(logs.empty() && engine.modelWrites==1);
    environment.Frame(965,false,log);assert(engine.modelWrites==1); // No per-frame failed load loop.
    Reject([&]{environment.Frame(965,true,log);});
    environment.Restore(965,"before_movie_error",log);
    assert(engine.actor.model=="characters/map_t.vmdl" && !engine.weapon.attributes[0]);
    logs.clear();engine.rejectModel=false;environment.Handle(Action("cosmetics",engine.Desired()),1000,log);
    engine.hidden=true;Reject([&]{environment.BeforeMovie(1064,log);});assert(logs.empty());
    Reject([&]{environment.Restore(1065,"missing_actor",log);});assert(logs.empty());
    engine.hidden=false;environment.Restore(1066,"actor_available",log);
    assert(engine.actor.model=="characters/map_t.vmdl");
}
static void TestCosmeticsWaitsForMaterialJobs() {
    CaptureEnvironment environment;FakeCosmetics engine;engine.Bind(environment);std::vector<std::string> logs;
    const auto log=[&](const auto& line){logs.push_back(line);};
    engine.weapon.worldMaterialJobs=2;environment.Handle(Action("cosmetics",engine.Desired()),900,log);
    environment.Frame(901,false,log);assert(logs.empty());
    Reject([&]{environment.BeforeMovie(964,log);});assert(logs.empty());
    engine.weapon.worldMaterialJobs=0;environment.Frame(965,false,log);assert(logs.size()==1);
    environment.BeforeMovie(966,log);assert(logs.size()==1);
    engine.weapon.hudMaterialJobs=1;Reject([&]{environment.Frame(967,true,log);});
    engine.weapon.hudMaterialJobs=0;environment.Restore(1036,"quit_after_flush",log);
}
static void TestPrerollWaitsForActorAndHudCreation() {
    CaptureEnvironment environment;FakeCosmetics engine;engine.Bind(environment);std::vector<std::string> logs;
    const auto log=[&](const auto& line){logs.push_back(line);};
    engine.hidden=true;environment.Handle(Action("cosmetics",engine.Desired()),900,log);
    environment.Frame(901,false,log);assert(logs.empty() && engine.modelWrites==0 && engine.paintWrites==0);
    Reject([&]{environment.BeforeMovie(902,log);});assert(logs.empty());
    engine.hidden=false;environment.Frame(910,false,log);assert(logs.size()==1 && engine.modelWrites==1 && engine.paintWrites==1);
    environment.BeforeMovie(964,log);assert(logs.size()==1);
    environment.Restore(1036,"quit_after_flush",log);
}
static void TestGlovesCaptureConfirmationAndExactRestore() {
    using Gloves=cs2_live::GloveCosmetics;
    CaptureEnvironment environment;FakeCosmetics engine;Gloves::Glove glove;glove.actor=engine.actor;
    glove.item={{"m_iItemDefinitionIndex",5030},{"m_bInitialized",1},{"m_iItemID",0},{"m_iItemIDHigh",0},{"m_iItemIDLow",0},{"attributes",Json::array()},{"original_name","saved baseline"}};
    glove.itemDef=5030;glove.itemReadable=glove.ready=glove.worldReady=glove.hudReady=glove.worldPoseReady=glove.hudPoseReady=glove.worldRenderReady=glove.hudRenderReady=true;glove.hudParentRenderEnabled=true;glove.worldMaterialJobs=glove.hudMaterialJobs=0;
    const auto original=glove.item;Gloves::Callbacks callbacks;callbacks.available=true;
    callbacks.gloves=[&]{return std::vector<Gloves::Glove>{glove};};callbacks.current=[&](const auto& g){return g.actor.instance==glove.actor.instance && g.actor.pawnHandle==glove.actor.pawnHandle;};
    callbacks.resolve=[](const Json& entry){if(entry["item_def"]!=5035 || entry["paint_kit"]!=10006)throw std::invalid_argument("not an installed glove pair");return Gloves::Paint{5035,10006,entry.value("seed",0u),entry.value("wear",0.01f)};};
    callbacks.snapshot=[](const auto& g){auto s=std::make_shared<Gloves::Snapshot>();s->item=g.item;s->native=std::make_shared<int>(1);return s;};
    callbacks.apply=[&](const auto&,const auto& paint){glove.item["m_iItemDefinitionIndex"]=paint.itemDef;glove.item["attributes"]=Json::array();unsigned id=6;for(float value:{float(paint.kit),float(paint.seed),paint.wear}){std::uint32_t bits=0;std::memcpy(&bits,&value,4);glove.item["attributes"].push_back({{"definition",id++},{"value_bits",bits}});}glove.itemDef=paint.itemDef;return true;};
    callbacks.restore=[&](const auto&,const auto& baseline){glove.item=baseline.item;glove.itemDef=glove.item["m_iItemDefinitionIndex"].get<unsigned>();return true;};
    engine.Bind(environment,std::move(callbacks));auto desired=engine.Desired();
    desired["gloves"]=Json::array({{{"steam_id",engine.actor.steamId},{"item_def",5035},{"paint_kit",10006},{"seed",42},{"wear",0.1}}});
    std::vector<std::string> logs;const auto log=[&](const auto& line){logs.push_back(line);};
    environment.Handle(Action("cosmetics",desired),900,log);glove.hudMaterialJobs=1;
    environment.Frame(901,false,log);assert(logs.empty());Reject([&]{environment.BeforeMovie(964,log);});
    glove.hudMaterialJobs=0;glove.worldPoseReady=false;environment.Frame(965,false,log);assert(logs.empty());Reject([&]{environment.BeforeMovie(966,log);});
    glove.worldPoseReady=true;glove.hudPoseReady=false;environment.Frame(967,false,log);assert(logs.empty());Reject([&]{environment.BeforeMovie(968,log);});
    glove.hudPoseReady=true;glove.worldRenderReady=false;environment.Frame(969,false,log);assert(logs.empty());Reject([&]{environment.BeforeMovie(970,log);});
    glove.worldRenderReady=true;glove.hudRenderReady=false;environment.Frame(971,false,log);assert(logs.empty());Reject([&]{environment.BeforeMovie(972,log);});
    glove.hudRenderReady=true;glove.hudParentRenderEnabled.reset();environment.Frame(973,false,log);assert(logs.empty());Reject([&]{environment.BeforeMovie(974,log);});
    glove.hudParentRenderEnabled=false;environment.Frame(975,false,log);assert(logs.empty());Reject([&]{environment.BeforeMovie(976,log);});
    glove.hudRenderRequired=false;glove.hudRenderReady=false;environment.Frame(977,false,log);assert(logs.size()==3 && logs[0]=="CS2_MVP_GLOVES_POSE_ABI 1" && logs[1]=="CS2_MVP_GLOVES_RENDER_ABI 1");
    const auto proof=Logged(logs[2]," proof=");assert(proof["glove_cosmetics"]["gloves"][0]["applied"]==true);
    assert(proof["glove_cosmetics"]["gloves"][0]["material_jobs"]["hud"]==0);
    assert(proof["glove_cosmetics"]["gloves"][0]["world_pose_ready"]==true && proof["glove_cosmetics"]["gloves"][0]["hud_pose_ready"]==true);
    assert(proof["glove_cosmetics"]["gloves"][0]["hud_render_required"]==false && proof["glove_cosmetics"]["gloves"][0]["hud_parent_render_enabled"]==false && proof["glove_cosmetics"]["gloves"][0]["hud_render_ready"]==false);
    environment.BeforeMovie(978,log);assert(logs.size()==3);
    glove.hudRenderRequired=true;glove.hudParentRenderEnabled=true;Reject([&]{environment.BeforeMovie(979,log);});glove.hudRenderReady=true;environment.BeforeMovie(980,log);
    environment.Restore(1036,"quit_after_flush",log);assert(glove.item==original);
    assert(logs[logs.size()-2]=="CS2_MVP_COSMETICS_RESTORED tick=1036 players_owned=0 weapons_owned=0 gloves_owned=0");
}

int main() {
    TestDecode();
    TestSceneReadbackAndRestore();
    TestRejectedSceneHasNoPartialEffects();
    TestAppearanceLoggingAndFailureRollback();
    TestIncompleteSceneRollbackRetainsRecoveryJournal();
    TestRestoreAttemptsBothBackends();
    TestCosmeticsPreflightReadbackAndRestore();
    TestCosmeticsUnconfirmedNeverStartsRecording();
    TestCosmeticsWaitsForMaterialJobs();
    TestPrerollWaitsForActorAndHudCreation();
    TestGlovesCaptureConfirmationAndExactRestore();
    std::cout << "Capture environment tests passed\n";
}
