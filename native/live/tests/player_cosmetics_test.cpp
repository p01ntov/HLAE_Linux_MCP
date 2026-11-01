#include "../live_controller.h"
#include <cassert>
#include <iostream>
using Cosmetics=cs2_live::PlayerCosmetics;
using Json=nlohmann::json;

static Json Catalog() {
    Json agents=Json::array();
    for(const auto def:{1,2,3})agents.push_back({{"kind","agent"},{"item_def",def},{"name","agent"+std::to_string(def)},
        {"display_name","Agent"},{"allowed_teams",{def==2?3:2}},{"team",def==2?"CT":"T"},
        {"model","characters/agent"+std::to_string(def)+".vmdl"},{"map_based",def==3}});
    return {{"format","cs2-camera-cosmetics-catalog"},{"version",1},{"agents",agents}};
}
struct Engine {
    std::vector<Cosmetics::Actor> actors;
    std::vector<std::pair<std::uint32_t,std::string>> models;
    std::vector<std::pair<std::uint32_t,Cosmetics::Color>> colors;
    bool expire=false,reject=false,hideSnapshot=false,failSnapshot=false;
    Engine(){Cosmetics::Actor actor;actor.steamId="76561198012345678";actor.nickname="same nickname";actor.model="characters/map_t.vmdl";actor.team=2;actor.pawnHandle=(4<<15)|7;actor.controllerHandle=(2<<15)|3;actor.instance=0x1000;actor.modelReady=actor.colorReady=true;actor.color={12,34,56,255};actors.push_back(actor);actor.steamId="76561198012345679";actor.instance=0x2000;actor.pawnHandle=(5<<15)|8;actors.push_back(actor);}
    bool Current(const Cosmetics::Actor& value){if(expire)return false;for(const auto& actor:actors)if(actor.instance==value.instance && actor.pawnHandle==value.pawnHandle && actor.controllerHandle==value.controllerHandle && actor.steamId==value.steamId && actor.team==value.team)return true;return false;}
    Cosmetics::Callbacks Callbacks(){Cosmetics::Callbacks out;out.catalog=cs2_live::CosmeticsCatalog::Parse(Catalog());out.agents=out.palette=true;out.actors=[&]{if(failSnapshot)throw std::runtime_error("snapshot transition");return hideSnapshot?std::vector<Cosmetics::Actor>{}:actors;};out.current=[&](const auto& a){return Current(a);};out.model=[&](const auto& a,const auto& model){assert(Current(a));models.emplace_back(a.pawnHandle,model);if(reject)return false;for(auto& live:actors)if(live.pawnHandle==a.pawnHandle)live.model=model;return true;};out.color=[&](const auto& a,const auto& color){assert(Current(a));colors.emplace_back(a.pawnHandle,color);if(reject)return false;for(auto& live:actors)if(live.pawnHandle==a.pawnHandle)live.color=color;return true;};return out;}
    Json Desired(unsigned def=1){return {{"players",Json::array({{{"steam_id",actors[0].steamId},{"nickname",actors[0].nickname},{"agent_item_def",def}}})}};}
};
template<class F>void Reject(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}assert(failed);}
static void TestPerSteamIdentityAndRestore() {
    Cosmetics runtime;Engine engine;runtime.Bind(engine.Callbacks());
    runtime.ValidateRequest(engine.Desired(),true);runtime.Apply(engine.Desired());
    assert(engine.models.size()==1 && engine.actors[0].model=="characters/agent1.vmdl" && engine.actors[1].model=="characters/map_t.vmdl");
    runtime.Refresh(true);assert(engine.models.size()==1); // Event-driven; do not rebuild every frame.
    runtime.Apply(Json::object());assert(engine.models.size()==2 && engine.actors[0].model=="characters/map_t.vmdl");
    assert(runtime.State()["capabilities"]["agents"]["render_verified_1p"]==false);
    runtime.Apply(engine.Desired());runtime.Apply(engine.Desired(3)); // Map default is the captured actual model.
    assert(engine.actors[0].model=="characters/map_t.vmdl");
    const auto count=engine.models.size();Reject([&]{runtime.ValidateRequest(engine.Desired(2),true);});assert(engine.models.size()==count);
    engine.expire=true;runtime.Refresh(true);assert(engine.models.size()==count);
}
static void TestSeekRespawnAndSideSwitch() {
    Cosmetics runtime;Engine engine;runtime.Bind(engine.Callbacks());runtime.Apply(engine.Desired());
    // Same pointer/serial after external seek must retain the original baseline,
    // rather than capture our own still-bound override as the original.
    runtime.Refresh(true);runtime.Apply(Json::object());assert(engine.actors[0].model=="characters/map_t.vmdl");
    runtime.Apply(engine.Desired());const auto oldHandle=engine.actors[0].pawnHandle;
    engine.actors[0].pawnHandle+=1<<15;engine.actors[0].model="characters/respawn_t.vmdl";
    runtime.Refresh(true);assert(engine.models.back().first!=oldHandle);runtime.Apply(Json::object());assert(engine.actors[0].model=="characters/respawn_t.vmdl");
    runtime.Apply(engine.Desired());engine.actors[0].team=3;const auto before=engine.models.size();
    runtime.Refresh(true);assert(engine.models.size()==before && runtime.State()["needs_demo_refresh"]==true);
    runtime.Refresh(false);assert(engine.models.size()==before); // Never install T baseline onto CT.
    engine.actors[0].model="characters/map_ct.vmdl";runtime.Refresh(true);
    assert(engine.models.size()==before && runtime.State()["needs_demo_refresh"]==false);
    assert(runtime.State()["players"][0].contains("error"));
}
static void TestPaletteAndStaleSafety() {
    Cosmetics runtime;Engine engine;runtime.Bind(engine.Callbacks());
    const Json palette={{"team_palette",{{"enabled",true},{"t",{255,0,0}},{"ct",{0,0,255}}}}};
    runtime.Apply(palette);assert(engine.colors.size()==2 && engine.actors[0].color==Cosmetics::Color({255,0,0,255}));
    auto disabled=palette;disabled["team_palette"]["enabled"]=false;runtime.Apply(disabled);
    assert(engine.actors[0].color==Cosmetics::Color({12,34,56,255}));assert(runtime.State()["team_palette"]["t"]==Json::array({255,0,0}));
    engine.actors[0].team=3;runtime.Apply(palette);assert(engine.actors[0].color==Cosmetics::Color({0,0,255,255}));
    const auto before=engine.colors.size();engine.actors.clear();runtime.Refresh(false);assert(engine.colors.size()==before);
}
static void TestStrictValidationAndCapability() {
    Cosmetics runtime;Engine engine;const auto count=engine.models.size();Reject([&]{runtime.Apply(engine.Desired());});assert(engine.models.size()==count);
    for(const Json& value:{Json{{"players",Json::array({{{"steam_id",true},{"agent_item_def",1}}})}},Json{{"players",Json::array({{{"steam_id","76561198012345678"},{"agent_item_def",1.0}}})}},
        Json{{"team_palette",{{"enabled",true},{"t",{1,2,3,4}},{"ct",{1,2,3}}}}},Json{{"team_palette",{{"enabled",true},{"t",{true,2,3}},{"ct",{1,2,3}}}}},Json{{"raw_address",1}}})Reject([&]{cs2_live::ValidateCosmetics(value);});
    auto missing=Catalog();missing["agents"][0]["compiled_model_present"]=false;auto callbacks=engine.Callbacks();callbacks.catalog=cs2_live::CosmeticsCatalog::Parse(missing);runtime.Bind(std::move(callbacks));Reject([&]{runtime.Apply(engine.Desired());});
}
static void TestRejectedSetterBoundedAndObservedState() {
    Cosmetics runtime;Engine engine;engine.reject=true;runtime.Bind(engine.Callbacks());runtime.Apply(engine.Desired());
    runtime.Refresh(true);runtime.Refresh(true);assert(engine.models.size()==1);
    assert(runtime.State()["players"][0]["applied"]==false && runtime.State()["players"][0]["agent_applied"]==false);
    assert(runtime.State()["players"][0].contains("error"));
    runtime.Apply(Json::object());engine.reject=false;runtime.Apply(engine.Desired());runtime.Refresh(true);
    assert(runtime.State()["players"][0]["agent_applied"]==true);
    engine.reject=true;runtime.Apply(Json::object());const auto count=engine.models.size();runtime.Refresh(true);runtime.Refresh(true);assert(engine.models.size()==count);
    assert(runtime.State()["players"][0].contains("error"));
}
static void TestTransientSnapshotPreservesBaseline() {
    Cosmetics runtime;Engine engine;runtime.Bind(engine.Callbacks());runtime.Apply(engine.Desired());
    engine.failSnapshot=true;runtime.Refresh(true);assert(runtime.State()["stale"]==true);engine.failSnapshot=false;
    engine.hideSnapshot=true;runtime.Refresh(true);assert(runtime.State()["stale"]==true);engine.hideSnapshot=false;
    runtime.Refresh(true);runtime.Apply(Json::object());assert(engine.actors[0].model=="characters/map_t.vmdl");
}
static void TestPaletteHotTeamChangePreservesOriginal() {
    Cosmetics runtime;Engine engine;runtime.Bind(engine.Callbacks());
    runtime.Apply({{"team_palette",{{"enabled",true},{"t",{255,0,0}},{"ct",{0,0,255}}}}});
    engine.actors[0].team=3;runtime.Refresh(true);assert(engine.actors[0].color==Cosmetics::Color({0,0,255,255}));
    runtime.Apply(Json::object());assert(engine.actors[0].color==Cosmetics::Color({12,34,56,255}));
}
static void TestUnreadableOwnedFieldNeverLosesBaseline() {
    Cosmetics runtime;Engine engine;runtime.Bind(engine.Callbacks());runtime.Apply(engine.Desired());
    const auto appliedModel=engine.actors[0].model;engine.actors[0].modelReady=false;engine.actors[0].model.clear();
    runtime.Apply(Json::object());assert(runtime.State()["owned_overrides"]==1 && runtime.State()["players"][0].contains("error"));
    engine.actors[0].modelReady=true;engine.actors[0].model=appliedModel;runtime.Refresh(false);
    assert(runtime.State()["owned_overrides"]==0 && engine.actors[0].model=="characters/map_t.vmdl");
    runtime.Apply({{"team_palette",{{"enabled",true},{"t",{255,0,0}},{"ct",{0,0,255}}}}});
    const auto appliedColor=engine.actors[0].color;engine.actors[0].colorReady=false;engine.actors[0].color={};
    runtime.Apply(Json::object());assert(runtime.State()["owned_overrides"]==1);
    engine.actors[0].colorReady=true;engine.actors[0].color=appliedColor;runtime.Refresh(false);
    assert(runtime.State()["owned_overrides"]==0 && engine.actors[0].color==Cosmetics::Color({12,34,56,255}));
}
static void TestControllerProjectCommandsAndUndo() {
    setenv("CS2_CAMERA_LIVE","1",1);cs2_live::Controller controller;Engine engine;controller.SetCosmeticsBackend(engine.Callbacks());
    const auto frame=[&]{controller.Frame(true,640,true,64,true,"",[](const std::string&){});};
    const auto state=[&]{return Json::parse(controller.StateJson());};
    const auto command=[&](const std::string& method,Json params=Json::object()){const auto text=Json{{"method",method},{"params",params}}.dump();controller.EnqueueText(text.c_str());frame();};
    frame();command("set_player_cosmetics",engine.Desired()["players"][0]);frame();
    assert(state()["project"]["cosmetics"]["players"].size()==1 && state()["cosmetics"]["players"][0]["agent_applied"]==true);
    const auto before=state()["revision"];command("set_player_cosmetics",engine.Desired(2)["players"][0]);
    assert(state()["revision"]==before && state().contains("error"));
    command("undo");assert(engine.actors[0].model=="characters/map_t.vmdl" && state()["project"]["cosmetics"].empty());
    command("redo");assert(engine.actors[0].model=="characters/agent1.vmdl");
    command("reset_player_cosmetics",{{"steam_id",engine.actors[0].steamId}});assert(engine.actors[0].model=="characters/map_t.vmdl" && state()["project"]["cosmetics"].empty());
    command("set_team_palette",{{"enabled",true},{"t",{255,0,0}}});command("set_team_palette",{{"enabled",false}});
    assert(state()["project"]["cosmetics"]["team_palette"]["t"]==Json::array({255,0,0}));
    const auto roundtrip=cs2_live::ReadProject(state()["project"]);assert(cs2_live::ToJson(roundtrip)["cosmetics"]==state()["project"]["cosmetics"]);
    command("reset_team_palette");assert(state()["project"]["cosmetics"].empty());
}
int main(){TestPerSteamIdentityAndRestore();TestSeekRespawnAndSideSwitch();TestPaletteAndStaleSafety();TestStrictValidationAndCapability();TestRejectedSetterBoundedAndObservedState();TestTransientSnapshotPreservesBaseline();TestPaletteHotTeamChangePreservesOriginal();TestUnreadableOwnedFieldNeverLosesBaseline();TestControllerProjectCommandsAndUndo();std::cout<<"Per-player cosmetics: identities, baseline restore, seek/respawn, team switch, palette, bounded failures, project undo and strict gates OK\n";}
