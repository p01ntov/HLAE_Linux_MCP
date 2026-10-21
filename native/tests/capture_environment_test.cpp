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

struct FakeAgents {
    using Players=cs2_live::PlayerCosmetics;Players::Actor actor;
    unsigned reads=0,models=0,colors=0;bool hidden=false,reject=false;
    FakeAgents(){actor.steamId="76561198012345678";actor.nickname="Agent";actor.team=2;actor.pawnHandle=32775;actor.controllerHandle=32771;actor.instance=0x1000;actor.generation=1;actor.model="characters/map_t.vmdl";actor.color={12,34,56,255};actor.modelReady=actor.colorReady=true;}
    void Bind(CaptureEnvironment& environment){
        Players::Callbacks cb;cb.agents=cb.palette=true;
        cb.catalog=cs2_live::CosmeticsCatalog::Parse({{"format","cs2-camera-cosmetics-catalog"},{"version",1},{"agents",Json::array({{{"kind","agent"},{"item_def",1},{"name","agent"},{"display_name","Agent"},{"team","T"},{"allowed_teams",{2}},{"model","characters/agent.vmdl"}}})}});
        cb.actors=[&]{++reads;return hidden?std::vector<Players::Actor>{}:std::vector<Players::Actor>{actor};};
        cb.current=[&](const auto& a){return !hidden && a.instance==actor.instance && a.pawnHandle==actor.pawnHandle && a.team==actor.team;};
        cb.model=[&](const auto&,const auto& model){++models;if(reject)return false;actor.model=model;return true;};
        cb.color=[&](const auto&,const auto& color){++colors;actor.color=color;return true;};environment.BindCosmetics(std::move(cb));
    }
    Json Desired()const{return {{"players",Json::array({{{"steam_id",actor.steamId},{"agent_item_def",1}}})},{"team_palette",{{"enabled",true},{"t",{255,0,0}},{"ct",{0,0,255}}}}};}
};
static void TestAgentPaletteReadbackAndRestore(){
    CaptureEnvironment env;FakeAgents agents;agents.Bind(env);std::vector<std::string> logs;const auto log=[&](const auto& line){logs.push_back(line);};
    env.Frame(1,false,log);assert(agents.reads==0); // Empty capture never scans actors.
    for(const char* key:{"weapon_paints","gloves"}){
        auto bad=agents.Desired();bad[key]=Json::array({{{"steam_id",agents.actor.steamId},{"item_def",7},{"paint_kit",180}}});
        Reject([&]{env.Handle(Action("cosmetics",bad),800,log);});assert(agents.reads==0 && agents.models==0 && agents.colors==0 && logs.empty());
    }
    Reject([&]{env.Handle(Action("appearance",{{"path","/tmp/pack"},{"models",{"characters/agent.vmdl"}}}),800,log);});assert(logs.empty());
    env.Handle(Action("cosmetics",agents.Desired()),900,log);env.Frame(901,false,log);env.BeforeMovie(964,log);
    assert(agents.models==1 && agents.colors==1 && logs.size()==1);const auto proof=Logged(logs[0]," proof=");
    assert(proof["players"]["players"][0]["agent_applied"]==true && proof["players"]["players"][0]["palette_applied"]==true);
    assert(!proof.contains("weapon_cosmetics") && !proof.contains("glove_cosmetics"));
    agents.actor.model="characters/respawn_t.vmdl";env.Frame(1000,true,log);assert(agents.actor.model=="characters/agent.vmdl");
    env.Restore(1036,"quit_after_flush",log);assert(agents.actor.model=="characters/respawn_t.vmdl" && agents.actor.color==FakeAgents::Players::Color({12,34,56,255}));
    assert(logs[logs.size()-2]=="CS2_MVP_COSMETICS_RESTORED tick=1036 players_owned=0 weapons_owned=0 gloves_owned=0");
}
static void TestAgentReadinessAndDeferredRestore(){
    CaptureEnvironment env;FakeAgents agents;agents.Bind(env);std::vector<std::string> logs;const auto log=[&](const auto& line){logs.push_back(line);};
    agents.hidden=true;env.Handle(Action("cosmetics",agents.Desired()),900,log);env.Frame(901,false,log);assert(logs.empty() && agents.models==0);
    Reject([&]{env.BeforeMovie(902,log);});agents.hidden=false;env.Frame(910,false,log);assert(logs.size()==1 && agents.models==1);
    agents.hidden=true;Reject([&]{env.Frame(911,true,log);});Reject([&]{env.Restore(912,"missing_actor",log);});
    agents.hidden=false;env.Restore(913,"actor_available",log);assert(agents.actor.model=="characters/map_t.vmdl");
    agents.reject=true;logs.clear();env.Handle(Action("cosmetics",agents.Desired()),1000,log);Reject([&]{env.BeforeMovie(1064,log);});assert(logs.empty());
    env.Restore(1065,"before_movie_error",log);
}
int main(){TestDecode();TestSceneReadbackAndRestore();TestRejectedSceneHasNoPartialEffects();TestIncompleteSceneRollbackRetainsRecoveryJournal();TestAgentPaletteReadbackAndRestore();TestAgentReadinessAndDeferredRestore();std::cout<<"Capture environment: scene rollback, agents/palette proof and removed-runtime rejection passed\n";}
