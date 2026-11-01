#include "../live_controller.h"
#include <cassert>
#include <iostream>
using namespace cs2_live;
template<class F>void Reject(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}assert(failed);}
int main() {
    unsigned observer=2,requested=0;bool gameplay=false,ready=false;
    Presentation presentation;Presentation::Callbacks callbacks;callbacks.observer=[&]{return std::optional<unsigned>(observer);};
    const auto expectedPreset=GameplayHudScenePreset();
    Json presetCapabilities=Json::object();for(const auto& entry:expectedPreset.items())presetCapabilities[entry.key()]={{"available",true},{"value",false}};
    presentation.SetSceneCapabilities(presetCapabilities);
    callbacks.gameplay=[&](bool value){gameplay=value;};callbacks.capabilities=[&]{return Json{{"installed",true},{"available",ready},{"gameplay_active",gameplay && ready}};};
    callbacks.requestObserver=[&](unsigned value){requested=value;return true;};callbacks.viewRequestsVerified=true;presentation.Bind(callbacks);
    presentation.Frame(true,true);assert(presentation.State()["view"]=="player");
    Reject([&]{presentation.Apply({{"view","free"},{"hud","gameplay"}});});assert(!requested && !gameplay);
    presentation.Apply({{"view","free"},{"hud","demo"}});assert(requested==4 && presentation.State()["view"]=="player" && presentation.State()["pending"]==true);
    observer=4;presentation.Frame(true,true);assert(presentation.State()["view"]=="free" && presentation.State()["pending"]==false);
    presentation.Apply({{"view","player"},{"hud","gameplay"}});assert(requested==2 && !gameplay);
    observer=2;presentation.Frame(true,true);assert(gameplay && presentation.State()["hud"]=="demo" && presentation.State()["pending"]==true);
    ready=true;presentation.Frame(true,true);assert(presentation.State()["hud"]=="gameplay" && presentation.State()["pending"]==false);
    presentation.Frame(true,false);assert(presentation.State()["hud"]=="hidden");
    presentation.Frame(false,false);assert(!gameplay && presentation.State()["stale"]==true);
    for(const Json& invalid:{Json{{"hud","invented"}},Json{{"view",4}},Json{{"raw_console","quit"}}})Reject([&]{ValidatePresentation(invalid);});
    Project project;project.presentation={{"view","player"},{"hud","hidden"}};const auto copy=ReadProject(ToJson(project));
    assert(copy.presentation==project.presentation && EffectiveSceneSettings(copy)["hud_visible"]==false);
    assert(EffectiveSceneSettings(Project{}).empty());
    Project gameplayProject;gameplayProject.presentation={{"view","player"},{"hud","gameplay"}};
    const auto effectiveGameplay=EffectiveSceneSettings(gameplayProject);for(const auto& entry:expectedPreset.items())assert(effectiveGameplay.at(entry.key())==false);
    setenv("CS2_CAMERA_LIVE","1",1);Controller controller;
    bool visible=true;controller.SetSceneBackend([&](const std::string& name){SceneCvar value;if(name=="cl_drawhud"){value.available=true;value.nativeType="bool";value.raw=visible?"1":"0";value.value=visible;}return value;},
        [&](const std::string& name,const std::string& raw){if(name!="cl_drawhud")return false;visible=raw=="1";return true;});
    observer=2;ready=true;controller.SetPresentationBackend(callbacks);
    const auto frame=[&]{controller.Frame(true,100,true,64,true,"",[](const std::string&){});};
    const auto state=[&]{return Json::parse(controller.StateJson());};
    const auto command=[&](const std::string& method,Json params=Json::object()){const auto text=Json{{"method",method},{"params",params}}.dump();controller.EnqueueText(text.c_str());frame();};
    frame();command("set_presentation",{{"hud","hidden"}});frame();assert(!visible && state()["presentation"]["hud"]=="hidden");
    assert(state()["project"]["presentation"]["hud"]=="hidden");command("undo");assert(visible && state()["project"]["presentation"].empty());
    command("redo");assert(!visible);command("set_presentation",{{"hud","demo"}});assert(visible);
    const auto beforeUnavailable=state()["revision"];command("set_presentation",{{"hud","gameplay"}});
    assert(state()["revision"]==beforeUnavailable && state()["presentation"]["capabilities"]["gameplay"]["requestable"]==false);
    std::map<std::string,bool> cvars={{"cl_drawhud",true},{"cl_spec_stats",true},{"cl_spec_show_bindings",true},{"cl_radar_square_when_spectating",true},{"cl_radar_show_all_players_when_spectating",true}};
    controller.SetSceneBackend([&](const std::string& name){SceneCvar value;if(cvars.count(name)){value.available=true;value.nativeType="bool";value.value=cvars.at(name);value.raw=cvars.at(name)?"1":"0";}return value;},
        [&](const std::string& name,const std::string& raw){if(!cvars.count(name))return false;cvars[name]=raw=="1";return true;});
    frame();command("set_presentation",{{"view","player"},{"hud","gameplay"}});frame();
    assert(state()["presentation"]["hud"]=="gameplay" && state()["presentation"]["pending"]==false);
    assert(state()["presentation"]["adapter"]["gameplay_preset"]["confirmed"]==true);
    for(const auto& item:cvars)if(item.first!="cl_drawhud")assert(!item.second);
    command("set_presentation",{{"hud","demo"}});frame();for(const auto& item:cvars)assert(item.second);
    std::cout<<"Presentation: typed view requests, actual observer/HUD readback, invalid combinations, visibility undo and project persistence OK\n";
}
