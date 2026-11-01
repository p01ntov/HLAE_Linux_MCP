#include "../live_controller.h"
#include <cassert>
#include <iostream>
using namespace cs2_live;
template<class F>void MustReject(F action){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}assert(rejected);}
int main(){
    setenv("CS2_CAMERA_LIVE","1",1);unsetenv("CS2_CAMERA_CAPTURE_COSMETICS");
    auto now=LiveTelemetryCadence::Clock::time_point{};unsigned mode=4,reads=0,requests=0;
    Json target={{"steam_id","76561199075450896"},{"nickname","same display name"},{"pawn_handle",32780},{"controller_handle",32769},{"team",3}};
    std::optional<Json> actual=target;
    Presentation::Callbacks backend;backend.observer=[&]{return std::optional<unsigned>{mode};};backend.requestObserver=[](unsigned){return true;};backend.viewRequestsVerified=true;
    backend.selectedPlayer=[&]{++reads;return actual;};backend.selectPlayer=[&](const std::string& id){if(id!="76561199075450896")throw std::runtime_error("SteamID is not a current unique player");++requests;};backend.selectionVerified=true;
    Presentation p([&]{return now;});p.Bind(backend);
    for(unsigned i=0;i<1000;++i){p.Frame(true,true);now+=std::chrono::milliseconds(1);}assert(reads==10);
    p.Apply({{"view","free"},{"hud","hidden"}});p.SelectPlayer("76561199075450896",p.Merge({{"view","player"}}));
    assert(requests==1 && p.State()["requested_player"]=="76561199075450896" && p.State()["selection_pending"]==true);
    auto revision=p.ObservationRevision();actual=Json{{"steam_id","76561199542433257"},{"pawn_handle",32781},{"controller_handle",32770},{"team",2},{"nickname","same display name"}};
    p.Frame(true,false);assert(p.State()["selection_pending"]==true && p.ObservationRevision()!=revision);
    const auto oldReads=reads;for(int i=0;i<20;++i)p.Frame(true,false);assert(reads-oldReads==20); // Pending requests never use idle cadence.
    mode=2;actual=target;p.Frame(true,false);assert(p.State()["selection_pending"]==false && p.State()["view"]=="player" && p.State()["hud"]=="hidden");
    MustReject([&]{p.SelectPlayer("42",p.Merge({{"view","player"}}));});assert(requests==1);
    mode=4;p.SelectPlayer("76561199075450896",p.Merge({{"view","player"}}));actual.reset();now+=std::chrono::seconds(6);p.Frame(true,false);
    assert(p.State()["selection_pending"]==false && p.State().contains("error") && p.State()["selected_player"].is_null());
    p.Frame(false,false);MustReject([&]{p.SelectPlayer("76561199075450896",p.Merge({{"view","player"}}));});
    Presentation unsupported;unsupported.Frame(true,true);MustReject([&]{unsupported.SelectPlayer("76561199075450896",{{"view","player"}});});

    Controller c([&]{return now;});c.SetPresentationBackend(backend);mode=4;actual=target;
    const auto frame=[&]{c.Frame(true,2102,true,64,true,"",[](const std::string&){});};
    const auto state=[&]{return Json::parse(c.StateJson());};
    const auto command=[&](const char* method,Json params){const auto text=Json{{"method",method},{"params",params}}.dump();c.EnqueueText(text.c_str());frame();};
    frame();float origin[3]={1,2,3},angles[3]={0,0,0},fov=75;c.View(origin,angles,&fov,true);frame();
    command("add_key",Json::object());command("set_presentation",{{"view","free"},{"hud","hidden"}});command("set_camera",{{"fov",50}});
    const auto before=state();command("select_player",{{"steam_id","76561199075450896"}});const auto selected=state();
    assert(selected["mode"]=="free" && selected["project"]==before["project"] && selected["revision"]==before["revision"] && selected["undo_available"]==before["undo_available"]);
    assert(selected["presentation"]["requested_player"]=="76561199075450896" && selected["presentation"]["selection_pending"]==true);
    mode=2;frame();assert(state()["presentation"]["selection_pending"]==false); // Observation revision bypasses 34 ms DTO cadence.
    const auto requestsBefore=requests;
    for(const Json& bad:{Json{{"nickname","same display name"}},Json{{"steam_id",42}},Json{{"steam_id","76561199075450896"},{"index",1}},Json{{"steam_id","1;quit"}}})command("select_player",bad);
    assert(requests==requestsBefore && state()["project"]==before["project"]);
    std::cout<<"Player selection: fresh typed request, observer readback, bounded pending, 10 Hz idle polling and transient camera release passed\n";
}
