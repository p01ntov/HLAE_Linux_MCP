#include "../live_controller.h"
#include <cassert>
#include <iostream>
using Json=nlohmann::json;
using Gloves=cs2_live::GloveCosmetics;
static Json Attribute(unsigned id,float value) {
    std::uint32_t bits=0;std::memcpy(&bits,&value,4);
    return {{"definition",id},{"value_bits",bits}};
}
static Json Item(unsigned definition) {
    return {{"m_iItemDefinitionIndex",definition},{"m_bInitialized",1},{"m_iItemID",0},{"m_iItemIDHigh",0},{"m_iItemIDLow",0},{"attributes",Json::array()},{"custom_name","baseline"}};
}
int main() {
    setenv("CS2_CAMERA_LIVE","1",1);cs2_live::Controller controller;
    std::vector<Gloves::Glove> actors(2);
    for(unsigned i=0;i<actors.size();++i) {
        auto& g=actors[i];g.actor.steamId=std::to_string(76561198012345678ull+i);g.actor.nickname="same nickname";
        g.actor.instance=0x1000+i*0x1000;g.actor.pawnHandle=32775+i;g.actor.controllerHandle=65540+i;g.actor.generation=1;g.actor.team=2;
        g.item=Item(5030);g.itemDef=5030;g.itemReadable=g.ready=g.worldReady=g.hudReady=g.worldPoseReady=g.hudPoseReady=g.worldRenderReady=g.hudRenderReady=true;g.hudParentRenderEnabled=true;g.worldMaterialJobs=g.hudMaterialJobs=0;
    }
    const auto baseline=actors;Gloves::Callbacks cb;cb.available=true;cb.reason="test adapter";
    cb.gloves=[&]{return actors;};
    cb.current=[&](const auto& g){return std::any_of(actors.begin(),actors.end(),[&](const auto& a){return a.actor.steamId==g.actor.steamId && a.actor.instance==g.actor.instance && a.actor.pawnHandle==g.actor.pawnHandle;});};
    cb.resolve=[](const Json& value){if(value["item_def"]!=5035 || value["paint_kit"]!=10006)throw std::invalid_argument("unsupported glove pair");return Gloves::Paint{5035,10006,value.value("seed",0u),value.value("wear",0.01f)};};
    cb.snapshot=[](const auto& g){auto snapshot=std::make_shared<Gloves::Snapshot>();snapshot->item=g.item;snapshot->native=std::make_shared<int>(1);return snapshot;};
    cb.apply=[&](const auto& g,const auto& p){for(auto& a:actors)if(a.actor.steamId==g.actor.steamId){a.item=Item(p.itemDef);a.item["attributes"]=Json::array({Attribute(6,float(p.kit)),Attribute(7,float(p.seed)),Attribute(8,p.wear)});a.itemDef=p.itemDef;}return true;};
    cb.restore=[&](const auto& g,const auto& snapshot){for(auto& a:actors)if(a.actor.steamId==g.actor.steamId){a.item=snapshot.item;a.itemDef=a.item["m_iItemDefinitionIndex"].template get<unsigned>();}return true;};
    controller.SetGloveCosmeticsBackend(std::move(cb));
    const auto frame=[&](bool demo=true){controller.Frame(demo,640,true,64,true,"",[](const std::string&){});};
    const auto state=[&]{return Json::parse(controller.StateJson());};
    const auto command=[&](const std::string& name,const Json& params=Json::object(),bool demo=true){const auto request=Json{{"method",name},{"params",params}}.dump();controller.EnqueueText(request.c_str());frame(demo);};
    const Json request={{"steam_id",actors[0].actor.steamId},{"nickname","same nickname"},{"item_def",5035},{"paint_kit",10006},{"seed",42},{"wear",0.1}};
    frame();command("set_player_gloves",request);
    assert(state()["project"]["cosmetics"]["gloves"]==Json::array({request}));
    assert(state()["cosmetics"]["capabilities"]["gloves"]["available"]==true);
    assert(state()["glove_cosmetics"]["gloves"][0]["applied"]==true && actors[1].item==baseline[1].item);
    const auto revision=state()["revision"];auto invalid=request;invalid["paint_kit"]=0;command("set_player_gloves",invalid);
    assert(state()["revision"]==revision && state().contains("error"));
    command("undo");assert(actors[0].item==baseline[0].item && state()["project"]["cosmetics"].empty());
    command("redo");assert(state()["glove_cosmetics"]["gloves"][0]["applied"]==true);
    const auto project=cs2_live::ReadProject(state()["project"]);assert(cs2_live::ToJson(project)["cosmetics"]["gloves"]==Json::array({request}));
    command("reset_player_gloves",{{"steam_id",actors[0].actor.steamId}});assert(state()["project"]["cosmetics"].empty() && actors[0].item==baseline[0].item);
    command("set_player_gloves",request);frame(false);const auto inactiveRevision=state()["revision"];
    command("undo",Json::object(),false);assert(state()["revision"]==inactiveRevision && state().contains("error"));
    assert(actors[0].item==baseline[0].item && actors[1].item==baseline[1].item);
    Gloves scoped;auto scopedCallbacks=Gloves::Callbacks{};auto originalSnapshot=std::make_shared<Gloves::Snapshot>();
    originalSnapshot->item=actors[0].item;originalSnapshot->native=std::make_shared<int>(1);originalSnapshot->teamMask=1u<<2;
    scopedCallbacks.available=true;scopedCallbacks.gloves=[&]{return std::vector<Gloves::Glove>{actors[0]};};
    scopedCallbacks.current=[](const auto&){return true;};scopedCallbacks.resolve=[](const Json&){return Gloves::Paint{5035,10006,42,0.1f};};
    scopedCallbacks.snapshot=[&](const auto&){return originalSnapshot;};unsigned restoreCalls=0;
    scopedCallbacks.apply=[&](const auto&,const auto& p){actors[0].item=Item(p.itemDef);actors[0].item["attributes"]=Json::array({Attribute(6,float(p.kit)),Attribute(7,float(p.seed)),Attribute(8,p.wear)});return true;};
    scopedCallbacks.restore=[&](const auto&,const auto&){++restoreCalls;return true;};
    scoped.Bind(std::move(scopedCallbacks));scoped.Apply(Json::array({request}));actors[0].actor.team=3;scoped.Refresh(true);scoped.Apply(Json::array());
    assert(scoped.State()["needs_demo_refresh"]==true && scoped.State()["owned_overrides"]==1 && restoreCalls==0);
    actors[0].item=Item(5030);scoped.Refresh(false);assert(scoped.State()["owned_overrides"]==0 && scoped.State()["needs_demo_refresh"]==false);
    std::cout<<"Glove controller: SteamID scope, undo/reset, project persistence and demo gating passed\n";
}
