#include "../live_controller.h"
#include <cassert>
#include <fstream>
#include <iostream>

using namespace cs2_live;
struct DemoFile {
    std::string path;
    DemoFile(){char name[]="/tmp/cs2-demo-timeline-XXXXXX";int fd=mkstemp(name);assert(fd>=0);path=name;assert(write(fd,"fixture",7)==7);close(fd);}
    ~DemoFile(){unlink(path.c_str());}
    Json Metadata()const {
        struct stat info{};assert(!stat(path.c_str(),&info));
        return {{"version",1},{"demo",{{"path",path},{"fingerprint",{{"sha256",std::string(64,'a')},{"bytes",info.st_size},{"mtime_ns",std::int64_t(info.st_mtim.tv_sec)*1000000000+info.st_mtim.tv_nsec}}},
            {"playback_ticks",105916},{"tick_rate",64},{"duration_seconds",1654.9375},{"authoritative_duration_source","DEM_FileInfo"}}},
            {"rounds",Json::array({{{"tick",0},{"round",nullptr},{"warmup",true}},{{"tick",6400},{"round",1},{"warmup",false}}})},
            {"players",Json::array({{{"steam_id","76561198000000001"},{"name","One"},{"kills",{100,100,7000}},{"deaths",{500}}},
                                   {{"steam_id","76561198000000002"},{"kills",{500}},{"deaths",{100,7000}}}})}};
    }
};
static bool Rejected(DemoTimeline& timeline,const Json& value,const char* expected){try{timeline.Load(value,expected);return false;}catch(const std::exception&){return true;}}
static void AuthoritativeFullMatchAndSelectedIdentity() {
    DemoFile file;DemoTimeline timeline;
    assert(!timeline.State(true,0,true,nullptr)["available"].get<bool>());
    timeline.Load(file.Metadata(),file.path.c_str());
    auto state=timeline.State(true,22872,true,{{"steam_id","76561198000000001"}});
    assert(state["available"]==true && state["demo"]["playback_ticks"]==105916 && state["demo"]["duration_seconds"]==1654.9375);
    assert(state["selected_player"]["kills"]==Json({100,100,7000}));
    assert(state["tick"]==22872 && state["paused"]==true && state["selection_source"]=="observer");
    assert(state["players"]==Json::array({{{"steam_id","76561198000000001"},{"name","One"}},{{"steam_id","76561198000000002"},{"name",""}}}));
    for(const auto& player:state["players"])assert(!player.contains("kills") && !player.contains("deaths"));
    assert(timeline.State(true,0,true,{{"steam_id","76561198000000002"}})["selected_player"]["deaths"]==Json({100,7000}));
    assert(timeline.State(true,0,true,{{"steam_id","76561198000000003"}})["selected_player"].is_null());
    assert(timeline.State(true,0,true,nullptr)["selected_player"].is_null());
    assert(timeline.State(false,0,true,nullptr)["available"]==false);
    unlink(file.path.c_str()); // Publication never reopens, stats, hashes or parses the demo.
    for(int i=0;i<1000;++i)assert(timeline.State(true,i,false,nullptr)["available"]==true);
}
static void RecordedMarkerFilterDoesNotNeedAnObserver() {
    DemoFile file;DemoTimeline timeline;timeline.Load(file.Metadata(),file.path.c_str());
    timeline.SelectPlayer("76561198000000001");
    auto state=timeline.State(true,123,true,nullptr);
    assert(state["marker_player"]=="76561198000000001" && state["selection_source"]=="timeline");
    assert(state["selected_player"]["kills"]==Json({100,100,7000}));
    assert(timeline.State(true,123,true,{{"steam_id","76561198000000002"}})["selected_player"]["name"]=="One");
    for(const Json& bad:{Json(true),Json(7),Json(""),Json("0"),Json("01"),Json("18446744073709551616"),Json("76561198000000003"),Json("1;quit")}) {
        bool rejected=false;try{timeline.SelectPlayer(bad);}catch(const std::exception&){rejected=true;}
        assert(rejected && timeline.State(true,0,true,nullptr)["marker_player"]=="76561198000000001");
    }
    auto refreshed=file.Metadata();refreshed["players"][0]["kills"]={101};timeline.Load(refreshed,file.path.c_str());
    assert(timeline.State(true,0,true,nullptr)["selected_player"]["kills"]==Json({101}));
    timeline.SelectPlayer(nullptr);
    assert(timeline.State(true,0,true,nullptr)["selected_player"].is_null());
    assert(timeline.State(true,0,true,{{"steam_id","76561198000000002"}})["selection_source"]=="observer");
    timeline.SelectPlayer("76561198000000001");refreshed["players"].erase(0);timeline.Load(refreshed,file.path.c_str());
    assert(timeline.State(true,0,true,nullptr)["marker_player"].is_null());
}
static void RejectInvalidMetadataTransactionally() {
    DemoFile file,other;DemoTimeline timeline;auto good=file.Metadata();timeline.Load(good,file.path.c_str());
    assert(timeline.State(true,0,true,nullptr)["metadata_revision"]==1);
    std::vector<Json> invalid;
    auto add=[&](auto mutate){auto value=good;mutate(value);invalid.push_back(std::move(value));};
    add([](Json& x){x["version"]=true;});add([](Json& x){x["version"]=2;});
    add([](Json& x){x["demo"]["playback_ticks"]=105916.0;});add([](Json& x){x["demo"]["playback_ticks"]=2000000001;});
    add([](Json& x){x["demo"]["tick_rate"]=true;});add([](Json& x){x["demo"]["duration_seconds"]=5;});
    add([](Json& x){x["demo"]["authoritative_duration_source"]="last_event";});
    add([](Json& x){x["demo"]["fingerprint"]["bytes"]=8;});add([](Json& x){x["demo"]["fingerprint"]["mtime_ns"]=0;});
    add([](Json& x){x["demo"]["fingerprint"]["sha256"]=std::string(64,'z');});
    add([](Json& x){x["rounds"][1]["tick"]=-1;});add([](Json& x){x["rounds"][0]["warmup"]=1;});
    add([](Json& x){x["players"][0]["kills"]={7000,100};});add([](Json& x){x["players"][0]["deaths"]={105917};});
    add([](Json& x){x["players"][0]["kills"]={true};});add([](Json& x){x["players"][0]["steam_id"]="076561198000000001";});
    add([](Json& x){x["players"][0]["steam_id"]="18446744073709551616";});
    add([](Json& x){x["players"].push_back(x["players"][0]);});
    add([](Json& x){x["players"][0]["name"]=std::string(129,'x');});
    add([](Json& x){x["players"][0]["kills"]=std::vector<int>(32769,0);});
    add([](Json& x){x["rounds"]=std::vector<Json>(1025,Json{{"tick",0}});});
    add([](Json& x){x["unexpected"]=true;});
    for(const auto& value:invalid){assert(Rejected(timeline,value,file.path.c_str()));const auto state=timeline.State(true,0,true,nullptr);assert(state["demo"]==good["demo"] && state["metadata_revision"]==1);}
    assert(Rejected(timeline,good,other.path.c_str()) && Rejected(timeline,good,nullptr));
    good["players"][0]["kills"]={101,102};timeline.Load(good,file.path.c_str());
    const auto refreshed=timeline.State(true,0,true,{{"steam_id","76561198000000001"}});
    assert(refreshed["metadata_revision"]==2 && refreshed["selected_player"]["kills"]==Json({101,102}));
}
static void StagedFileAndErrorAreBounded() {
    DemoFile file,staged;auto metadata=file.Metadata();
    {std::ofstream stream(staged.path);stream<<metadata;}
    DemoTimeline timeline;timeline.LoadFile(staged.path.c_str(),file.path.c_str());assert(timeline.State(true,0,true,nullptr)["available"]==true);
    DemoTimeline failure;failure.LoadFile(nullptr,file.path.c_str(),"Cannot parse authoritative demo metadata");
    assert(failure.State(true,0,true,nullptr)["reason"]=="Cannot parse authoritative demo metadata");
    DemoTimeline tooLarge;{std::ofstream stream(staged.path);stream<<std::string(DemoTimeline::MaxBytes+1,' ');}
    tooLarge.LoadFile(staged.path.c_str(),file.path.c_str());assert(tooLarge.State(true,0,true,nullptr)["available"]==false);
    const auto link=staged.path+"-link";assert(!symlink(file.path.c_str(),link.c_str()));
    DemoTimeline symlinked;symlinked.LoadFile(link.c_str(),file.path.c_str());assert(symlinked.State(true,0,true,nullptr)["available"]==false);unlink(link.c_str());
}
static void LoadAndDemoSeekDoNotEditCameraProject() {
    DemoFile file;setenv("CS2_CAMERA_DEMO_PATH",file.path.c_str(),1);Controller controller;
    Presentation::Callbacks observer;observer.selectedPlayer=[](){return std::optional<Json>{{{"steam_id","76561198000000001"}}};};controller.SetPresentationBackend(std::move(observer));
    float position[3]={10,20,30},angles[3]={0,90,0},fov=80;controller.View(position,angles,&fov,true);
    std::vector<std::string> commands;
    auto send=[&](const char* method,const Json& params){auto text=Json{{"method",method},{"params",params}}.dump();controller.EnqueueText(text.c_str());controller.Frame(true,22872,true,64,true,"",[&](const std::string& command){commands.push_back(command);});return Json::parse(controller.StateJson());};
    send("apply_preset",{{"name","hold"},{"duration",5}});send("scrub",{{"time",2},{"seek_demo",false}});
    auto before=Json::parse(controller.StateJson());commands.clear();
    auto after=send("set_demo_timeline",{{"timeline",file.Metadata()}});
    for(const char* key:{"project","cursor","revision","camera","mode"})assert(after[key]==before[key]);
    assert(commands.empty() && after["demo_timeline"]["available"]==true && after["demo_timeline"]["selected_player"]["name"]=="One");
    after=send("demo_control",{{"action","seek"},{"tick",60000}});
    for(const char* key:{"project","cursor","revision","camera"})assert(after[key]==before[key]);
    assert(after["mode"]=="free" && after["preview_playing"]==false);
    assert(commands.size()==2 && commands[0]=="demo_pause" && commands[1]=="demo_gototick 60000 pause");
    after=send("set_demo_timeline",{{"timeline",file.Metadata()},{"extra",1}});assert(after.contains("error"));
    unsetenv("CS2_CAMERA_DEMO_PATH");
}
static void UnsupportedEntitiesStillPermitRecordedMarkerSelection() {
    DemoFile file;setenv("CS2_CAMERA_DEMO_PATH",file.path.c_str(),1);Controller controller;
    std::vector<std::string> commands;
    auto send=[&](const char* method,const Json& params,bool active=true){auto text=Json{{"method",method},{"params",params}}.dump();controller.EnqueueText(text.c_str());controller.Frame(active,22872,true,64,false,"",[&](const std::string& command){commands.push_back(command);});return Json::parse(controller.StateJson());};
    const auto before=send("set_demo_timeline",{{"timeline",file.Metadata()}});
    assert(before["entity_inspection_available"]==false && before["view_hook_installed"]==false);
    assert(before["presentation"]["capabilities"]["select_player"]["requestable"]==false);
    auto after=send("select_timeline_player",{{"steam_id","76561198000000001"}});
    assert(!after.contains("error") && after["demo_timeline"]["selected_player"]["name"]=="One");
    for(const char* key:{"project","cursor","revision","camera","mode","preview_playing","presentation"})assert(after[key]==before[key]);
    assert(commands.empty());
    for(const Json& params:{Json::object(),Json{{"steam_id",true}},Json{{"steam_id","76561198000000003"}},Json{{"steam_id",nullptr},{"index",7}}}) {
        after=send("select_timeline_player",params);assert(after.contains("error"));
        assert(after["demo_timeline"]["marker_player"]=="76561198000000001");
    }
    after=send("select_timeline_player",{{"steam_id",nullptr}});
    assert(!after.contains("error") && after["demo_timeline"]["selected_player"].is_null());
    assert(send("select_timeline_player",{{"steam_id","76561198000000001"}},false).contains("error"));
    assert(commands.empty());unsetenv("CS2_CAMERA_DEMO_PATH");
}
int main(){setenv("CS2_CAMERA_LIVE","1",1);AuthoritativeFullMatchAndSelectedIdentity();RecordedMarkerFilterDoesNotNeedAnObserver();RejectInvalidMetadataTransactionally();StagedFileAndErrorAreBounded();LoadAndDemoSeekDoNotEditCameraProject();UnsupportedEntitiesStillPermitRecordedMarkerSelection();std::cout<<"Demo timeline: authoritative full duration, bounded metadata, independent marker selection without entity adapters and camera-track isolation OK\n";}
