#include "../live_controller.h"
#include <cassert>
#include <iostream>
using namespace cs2_live;
using namespace std::chrono;

struct TelemetryEngine {
    unsigned players=0;
    void Bind(Controller& controller) {
        PlayerCosmetics::Callbacks callback;callback.actors=[&]{++players;return std::vector<PlayerCosmetics::Actor>{};};controller.SetCosmeticsBackend(std::move(callback));
    }

};
static void Frame(Controller& controller,int tick=640){controller.Frame(true,tick,true,64,true,"",[](const std::string&){});}
static Json State(Controller& controller){return Json::parse(controller.StateJson());}
static void IdleRowsAndPublicationAreBounded() {
    auto now=Clock::time_point{}+hours(1);Controller controller([&]{return now;});TelemetryEngine engine;engine.Bind(controller);
    unsigned published=0;std::string previous;
    for(int i=0;i<1000;++i){
        float position[3]={float(i),0,0},angles[3]={0,0,0},fov=90;controller.View(position,angles,&fov,true);
        Frame(controller);
        auto state=controller.StateJson();if(state!=previous){++published;previous=std::move(state);}now+=milliseconds(1);
    }
    assert(engine.players==10);
    assert(published>=28 && published<=31);
    const auto state=State(controller);
    assert(state["weapon_cosmetics"]["weapons"].empty() && state["glove_cosmetics"]["gloves"].empty());
    assert(state["cosmetics"]["capabilities"]["weapons"]["available"]==false && state["cosmetics"]["capabilities"]["gloves"]["available"]==false);
}
static void CommandsSeekAndLifetimeChangesAreImmediate() {
    auto now=Clock::time_point{}+hours(1);Controller controller([&]{return now;});TelemetryEngine engine;engine.Bind(controller);Frame(controller);
    now+=milliseconds(1);controller.EnqueueText(R"({"method":"release_camera","params":{}})");Frame(controller);
    assert(engine.players==2 && State(controller)["revision"]==1);
    now+=milliseconds(1);controller.EnqueueText(R"({"method":"demo_control","params":{"action":"seek","tick":1000}})");Frame(controller);
    assert(engine.players>=4);
    const auto before=engine.players;now+=milliseconds(1);Frame(controller,10);assert(engine.players==before+1);
    now+=milliseconds(34);Frame(controller,11);
    assert(State(controller)["tick"]==11 && State(controller)["cosmetics"]["sample_tick"]==10);
}
static void CaptureBypassesAllCadence() {
    setenv("CS2_CAMERA_CAPTURE_COSMETICS","1",1);
    auto now=Clock::time_point{};Controller controller([&]{return now;});TelemetryEngine engine;engine.Bind(controller);
    for(int i=0;i<100;++i){Frame(controller,640+i);}
    assert(engine.players==100);
    assert(State(controller)["tick"]==739 && State(controller)["cosmetics"]["sample_tick"]==739);
    unsetenv("CS2_CAMERA_CAPTURE_COSMETICS");
}
static void DisabledPaletteIsIdle() {
    assert(!PlayerCosmeticsRequested({{"team_palette",{{"enabled",false}}}}));
    assert(PlayerCosmeticsRequested({{"team_palette",{{"enabled",true}}}}));
    assert(PlayerCosmeticsRequested({{"players",Json::array({{{"steam_id","76561198012345678"},{"agent_item_def",1}}})}}));
    for(bool capture:{false,true}) {
        if(capture)setenv("CS2_CAMERA_CAPTURE_COSMETICS","1",1);
        auto now=Clock::time_point{};Controller controller([&]{return now;});TelemetryEngine engine;engine.Bind(controller);
        controller.EnqueueText(R"({"method":"set_team_palette","params":{"enabled":false,"t":[255,0,0],"ct":[0,0,255]}})");Frame(controller);
        assert(!State(controller).contains("error") && State(controller)["project"]["cosmetics"]["team_palette"]["enabled"]==false);
        engine.players=0;
        for(int i=0;i<1000;++i){now+=milliseconds(1);Frame(controller);}
        assert(engine.players==(capture?1000u:10u));
        unsetenv("CS2_CAMERA_CAPTURE_COSMETICS");
    }
}
int main(){
    setenv("CS2_CAMERA_LIVE","1",1);unsetenv("CS2_CAMERA_CAPTURE_COSMETICS");
    IdleRowsAndPublicationAreBounded();CommandsSeekAndLifetimeChangesAreImmediate();CaptureBypassesAllCadence();DisabledPaletteIsIdle();
    std::cout<<"Live telemetry: bounded idle reads/publication, immediate commands/lifetimes, removed skin runtimes and unthrottled capture OK\n";
}
