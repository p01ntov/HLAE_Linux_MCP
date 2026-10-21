#include "../live_controller.h"
#include <cassert>
#include <iostream>

using namespace cs2_live;

static Json State(Controller& controller) { return Json::parse(controller.StateJson()); }
static void Command(Controller& controller,const std::string& method,Json params=Json::object(),bool demo=true) {
    Json request={{"method",method},{"params",params}};
    const std::string text=request.dump();
    controller.EnqueueText(text.c_str());
    controller.Frame(demo,640,true,64,true,"",[](const std::string&){});
}

struct FakeSceneCvars {
    std::map<std::string,SceneCvar> entries;
    std::vector<std::pair<std::string,std::string>> writes;
    std::string rejectName,rejectValue,ignoreName,ignoreValue;
    FakeSceneCvars() {
        for(const auto& control:SceneControls()) {
            SceneCvar value;value.available=true;value.nativeType=control.boolean?"bool":"float32";
            value.value=control.boolean?Json(false):Json(1.0);
            if(std::string(control.id)=="dof_near_blurry")value.value=-100;
            if(std::string(control.id)=="dof_near_crisp")value.value=0;
            if(std::string(control.id)=="dof_far_crisp")value.value=180;
            if(std::string(control.id)=="dof_far_blurry")value.value=2000;
            if(std::string(control.id)=="dof_tilt")value.value=.5;
            value.raw=value.value.is_boolean()?"0":SceneNumberText(value.value.get<double>());
            entries[control.cvar]=value;
        }
        auto& fullbright=entries.at("mat_fullbright");fullbright.integer=true;fullbright.nativeType="int32";fullbright.value=2;fullbright.raw="2";
    }
    SceneCvar Read(const std::string& name) {return entries.at(name);}
    bool Write(const std::string& name,const std::string& raw) {
        writes.push_back({name,raw});
        if(name==rejectName && raw==rejectValue)return false;
        if(name==ignoreName && raw==ignoreValue)return true;
        auto& entry=entries.at(name);entry.raw=raw;
        entry.value=entry.nativeType=="bool"?Json(raw=="1" || raw=="true"):Json(std::stod(raw));
        return true;
    }
    void Bind(Controller& controller) {
        controller.SetSceneBackend([this](const std::string& name){return Read(name);},
            [this](const std::string& name,const std::string& raw){return Write(name,raw);});
    }
};

static void TestSceneEnvironment() {
    FakeSceneCvars backend;
    SceneEnvironment scene;
    scene.Bind([&](const std::string& name){return backend.Read(name);},
               [&](const std::string& name,const std::string& raw){return backend.Write(name,raw);});
    scene.Refresh();
    assert(scene.Capabilities()["fullbright"]["value"]==true);
    assert(scene.Capabilities()["dof_radius"]["group"]=="Lens");
    scene.Apply({{"fullbright",false},{"exposure",2.5}});
    assert(backend.entries["mat_fullbright"].value==0);
    assert(backend.entries["mat_tonemap_force_scale"].value==2.5);
    scene.Apply(Json::object());
    assert(backend.entries["mat_fullbright"].value==2); // Preserve baseline mode 2, not just boolean true.
    assert(backend.entries["mat_tonemap_force_scale"].raw=="1");
    assert(scene.Capabilities()["shading_mode"]["value"]==2);
    assert(scene.Capabilities()["shading_mode"]["integer"]==true);
    backend.writes.clear();
    scene.Apply({{"shading_mode",0}});
    assert(backend.entries["mat_fullbright"].value==0 && backend.writes.size()==1);
    backend.writes.clear();
    scene.Apply({{"fullbright",true}});
    assert(backend.entries["mat_fullbright"].value==1 && backend.writes.size()==1);
    backend.writes.clear();
    scene.Apply({{"shading_mode",2}});
    assert(backend.entries["mat_fullbright"].value==2 && backend.writes.size()==1);
    scene.Apply({{"shading_mode",1},{"fullbright",true}});
    assert(backend.entries["mat_fullbright"].value==1);
    scene.Apply(Json::object());assert(backend.entries["mat_fullbright"].value==2);
    backend.writes.clear();
    bool aliasRejected=false;
    try{scene.Apply({{"shading_mode",2},{"fullbright",true},{"exposure",3}});}catch(const std::exception&){aliasRejected=true;}
    assert(aliasRejected && backend.writes.empty());
    for(const Json& invalid:std::vector<Json>{{{"shading_mode",.5}},{{"shading_mode",3}},{{"shading_mode",true}}}) {
        aliasRejected=false;try{scene.Apply(invalid);}catch(const std::exception&){aliasRejected=true;}
        assert(aliasRejected && backend.writes.empty());
    }
    backend.entries["cl_showfps"].integer=true;backend.entries["cl_showfps"].nativeType="int32";
    backend.entries["cl_showfps"].value=4;backend.entries["cl_showfps"].raw="4";
    SceneEnvironment diagnostics;
    diagnostics.Bind([&](const std::string& name){return backend.Read(name);},
                     [&](const std::string& name,const std::string& raw){return backend.Write(name,raw);});
    diagnostics.Apply({{"fps_counter_visible",false},{"trueview_status_visible",false}});
    assert(backend.entries["cl_showfps"].value==0);
    diagnostics.Apply(Json::object());assert(backend.entries["cl_showfps"].value==4);
    backend.writes.clear();
    bool rejected=false;
    try{scene.Apply({{"exposure",2},{"dof_near_crisp",3000}});}catch(const std::exception&){rejected=true;}
    assert(rejected && backend.writes.empty());
    backend.entries["r_dof2_radiusscale"].available=false;scene.Refresh();
    assert(scene.Capabilities()["dof_radius"]["available"]==false);
    rejected=false;try{scene.Apply({{"exposure",2},{"dof_radius",1}});}catch(const std::exception&){rejected=true;}
    assert(rejected && backend.writes.empty());
    backend.entries["r_dof2_radiusscale"].available=true;
    backend.rejectName="mat_tonemap_force_scale";backend.rejectValue="3";
    rejected=false;try{scene.Apply({{"bloom_strength",2},{"exposure",3}});}catch(const std::exception& error){rejected=true;assert(std::string(error.what()).find("previous scene values restored")!=std::string::npos);}
    assert(rejected && backend.entries["r_csgo_render_post_bloom_strength"].value==1);
    assert(backend.entries["mat_tonemap_force_scale"].value==1);
    backend.rejectName.clear();backend.ignoreName="mat_tonemap_force_scale";backend.ignoreValue="4";
    rejected=false;try{scene.Apply({{"bloom_strength",2},{"exposure",4}});}catch(const std::exception&){rejected=true;}
    assert(rejected && backend.entries["r_csgo_render_post_bloom_strength"].value==1);
    backend.ignoreName.clear();
    backend.entries["r_dof2_maxblursize"].integer=true;
    backend.entries["r_dof2_maxblursize"].nativeType="int32";
    backend.entries["r_dof2_maxblursize"].maximum=8;
    for(const Json& invalid:std::vector<Json>{{{"dof_max_blur",8.1}},{{"dof_max_blur",.5}},{{"exposure",true}},{{"fullbright",1}},{{"raw_console","quit"}},{{"exposure",std::numeric_limits<double>::infinity()}}}) {
        backend.writes.clear();rejected=false;
        try{scene.Apply(invalid);}catch(const std::exception&){rejected=true;}
        assert(rejected && backend.writes.empty());
    }

    Controller controller;FakeSceneCvars engine;engine.Bind(controller);
    float origin[3]={0,0,40},angles[3]={0,0,0},fov=90;
    controller.View(origin,angles,&fov,true);
    controller.Frame(true,640,true,64,true,"",[](const std::string&){});
    Command(controller,"set_scene",{{"settings",{{"exposure",2},{"fullbright",false}}}});
    assert(State(controller)["project"]["scene"]["exposure"]==2);
    assert(State(controller)["scene_capabilities"]["exposure"]["value"]==2);
    Command(controller,"set_scene",{{"settings",{{"bloom_strength",3}}}});
    assert(State(controller)["project"]["scene"].size()==3);
    Command(controller,"undo");assert(!State(controller)["project"]["scene"].contains("bloom_strength"));
    assert(engine.entries["r_csgo_render_post_bloom_strength"].value==1);
    Command(controller,"redo");assert(engine.entries["r_csgo_render_post_bloom_strength"].value==3);
    Command(controller,"reset_scene");assert(State(controller)["project"]["scene"].empty());
    assert(engine.entries["mat_fullbright"].value==2 && engine.entries["mat_tonemap_force_scale"].value==1);
    Command(controller,"set_scene",{{"settings",{{"fullbright",true}}}});
    Command(controller,"set_scene",{{"settings",{{"shading_mode",2}}}});
    assert(!State(controller)["project"]["scene"].contains("fullbright"));
    assert(State(controller)["project"]["scene"]["shading_mode"]==2 && engine.entries["mat_fullbright"].value==2);
    Command(controller,"undo");assert(State(controller)["project"]["scene"]["fullbright"]==true && engine.entries["mat_fullbright"].value==1);
    Command(controller,"redo");assert(engine.entries["mat_fullbright"].value==2);
    Command(controller,"set_scene",{{"settings",{{"fullbright",false}}}});
    assert(!State(controller)["project"]["scene"].contains("shading_mode"));
    assert(engine.entries["mat_fullbright"].value==0);
    Command(controller,"reset_scene");assert(engine.entries["mat_fullbright"].value==2);
    // Resume the earlier history branch used by the save/load and demo tests.
    Command(controller,"set_scene",{{"settings",{{"exposure",2},{"fullbright",false},{"bloom_strength",3}}}});
    Command(controller,"reset_scene");
    Command(controller,"undo");assert(engine.entries["mat_tonemap_force_scale"].value==2);
    controller.Frame(false,640,true,64,true,"",[](const std::string&){});
    assert(engine.entries["mat_tonemap_force_scale"].value==1);
    assert(State(controller)["project"]["scene"]["exposure"]==2);
    const auto before=State(controller)["project"];
    Command(controller,"set_scene",{{"settings",{{"exposure",3}}}},false);
    assert(State(controller)["project"]==before);
    controller.Frame(true,640,true,64,true,"",[](const std::string&){});
    assert(engine.entries["mat_tonemap_force_scale"].value==2);
    const auto good=State(controller)["project"];
    auto bad=good;bad["scene"]={{"unknown_effect",1}};
    Command(controller,"import_project",{{"project",bad}});
    assert(State(controller)["project"]==good);
    engine.entries["r_dof2_radiusscale"].available=false;
    bad=good;bad["scene"]={{"dof_radius",2}};
    Command(controller,"import_project",{{"project",bad}});
    assert(State(controller)["project"]==good);
    char temporary[]="/tmp/cs2-scene-project-XXXXXX";char* directory=mkdtemp(temporary);assert(directory);
    const std::string filename=std::string(directory)+"/scene.json";setenv("CS2_CAMERA_PROJECT",filename.c_str(),1);
    Command(controller,"save_project");Command(controller,"reset_scene");Command(controller,"load_project");
    assert(State(controller)["project"]["scene"]==good["scene"]);
    assert(engine.entries["mat_tonemap_force_scale"].value==2);
    controller.RestoreScene();assert(engine.entries["mat_tonemap_force_scale"].value==1);
    assert(unlink(filename.c_str())==0 && rmdir(directory)==0);
    unsigned inspectedLimit=0;
    controller.SetEntityInspector([&](unsigned limit){inspectedLimit=limit;return Json{{"read_only",true},{"identity_count",42},{"entries",Json::array()}};});
    const auto beforeInspect=State(controller);
    Command(controller,"inspect_entities",{{"limit",20}});
    assert(inspectedLimit==20 && State(controller)["entity_probe"]["identity_count"]==42);
    assert(State(controller)["revision"]==beforeInspect["revision"] && State(controller)["project"]==beforeInspect["project"]);
    inspectedLimit=0;Command(controller,"inspect_entities",{{"limit",129}});assert(!inspectedLimit);
    Command(controller,"inspect_entities",{{"limit",1.5}});assert(!inspectedLimit);
    Command(controller,"inspect_entities",{{"arbitrary_address",123}});assert(!inspectedLimit);
    Command(controller,"inspect_entities",Json::object(),false);assert(!inspectedLimit);
}

static void TestAppearanceDemoRefreshPreservesEditor() {
    char pattern[]="/tmp/cs2-controller-refresh-XXXXXX";
    const char* folder=mkdtemp(pattern);assert(folder);
    const std::string path=std::string(folder)+"/input.dem";
    int file=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);assert(file>=0);
    assert(write(file,"demo",4)==4);close(file);
    setenv("CS2_CAMERA_DEMO_PATH",path.c_str(),1);
    Controller controller;FakeSceneCvars scene;scene.Bind(controller);
    float origin[3]={10,20,30},angles[3]={4,50,6},fov=79.25f;
    controller.View(origin,angles,&fov,true);
    Command(controller,"set_camera",{{"fov",79.25}});
    Command(controller,"add_key",{{"time",.25}});
    Command(controller,"set_scene",{{"settings",{{"fullbright",false}}}});
    const auto before=State(controller);std::vector<std::string> commands;
    const auto execute=[&](const std::string& command){commands.push_back(command);};
    controller.Frame(true,640,true,64,true,"",execute);
    assert(controller.CanRefreshAppearance() && controller.RequestAppearanceRefresh());
    assert((commands==std::vector<std::string>{"demo_pause","disconnect"}));
    commands.clear();controller.Frame(false,0,false,64,true,"",execute);
    auto during=State(controller);
    assert(during["project"]==before["project"] && during["cursor"]==before["cursor"]);
    assert(during["mode"]=="hold" && during["camera"]==before["camera"]);
    assert(during["appearance_refresh"]["phase"]=="waiting_load");
    assert(commands==std::vector<std::string>{"playdemo \""+path+"\""});
    assert(scene.entries["mat_fullbright"].value==0); // Do not clear project FX during intentional unload.
    commands.clear();origin[0]=999;fov=60;
    assert(controller.View(origin,angles,&fov,true));
    assert(origin[0]==10 && fov==79.25f); // First reloaded view already holds the editor camera.
    controller.Frame(true,96,false,64,true,"",execute);
    assert((commands==std::vector<std::string>{"demo_pause","demo_gototick 640 pause"}));
    const auto text=Json({{"method","set_camera"},{"params",{{"fov",100}}}}).dump();
    controller.EnqueueText(text.c_str());controller.Frame(true,100,true,64,true,"",execute);
    assert(State(controller)["camera"]==before["camera"]); // No stale edit can be lost during refresh.
    commands.clear();controller.Frame(true,640,true,64,true,"",execute);
    const auto after=State(controller);
    assert(after["appearance_refresh"]["phase"]=="complete");
    assert(after["project"]==before["project"] && after["camera"]==before["camera"]);
    assert(after["cursor"]==before["cursor"] && after["revision"]==before["revision"]);
    assert(commands==std::vector<std::string>{"demo_pause"});
    unsetenv("CS2_CAMERA_DEMO_PATH");unlink(path.c_str());rmdir(folder);
}

static void TestRenderRange() {
    const auto legacy=ReadProject({{"duration",3},{"keys",Json::array()}});
    assert(ToJson(legacy)["render_range"]==Json({{"mode","composition"},{"in",0},{"out",3}}));
    auto serialized=ToJson(legacy);
    serialized["render_range"]={{"mode","region"},{"in",.5},{"out",2.25}};
    assert(ToJson(ReadProject(serialized))==serialized);
    assert(PlaybackRange(ReadProject(serialized)).in==.5);
    serialized["render_range"]["mode"]="composition";
    assert(PlaybackRange(ReadProject(serialized)).in==0 && PlaybackRange(ReadProject(serialized)).out==3);
    assert(ToJson(ReadProject(serialized))["render_range"]["in"]==.5);
    for(const auto& invalid:std::vector<Json>{
        Json(nullptr),Json::array(),{{"mode","crop"},{"in",0},{"out",1}},
        {{"in",-1},{"out",1}},{{"in",1},{"out",1}},{{"in",2},{"out",1}},
        {{"in",0},{"out",3.1}},{{"in",std::numeric_limits<double>::infinity()},{"out",2}},
        {{"in",0},{"out",std::numeric_limits<double>::quiet_NaN()}}}) {
        auto bad=serialized; bad["render_range"]=invalid;
        bool rejected=false;try{ReadProject(bad);}catch(const std::exception&){rejected=true;}assert(rejected);
    }
    // Explicit fine regions are valid; .01 is only an automatic resize floor.
    serialized["render_range"]={{"mode","region"},{"in",1},{"out",1.001}};
    assert(ReadProject(serialized).renderRange->out==1.001);

    Controller controller;
    float origin[3]={0,0,40},angles[3]={0,0,15},fov=90;
    controller.View(origin,angles,&fov,true);
    controller.Frame(true,1000,true,64,true,"",[](const std::string&){});
    std::vector<std::string> commands;
    auto frame=[&](int tick,bool paused=false) {
        controller.Frame(true,tick,paused,64,true,"",[&](const std::string& command){commands.push_back(command);});
    };
    auto send=[&](const std::string& method,Json params,int tick=1000) {
        const std::string text=Json({{"method",method},{"params",params}}).dump();
        controller.EnqueueText(text.c_str());frame(tick,true);
    };
    auto range=[&] {return State(controller)["project"]["render_range"];};
    Json project={{"name","range fixture"},{"start_tick",1000},{"duration",5},{"interpolation","linear"},
                  {"keys",Json::array({{{"id","a"},{"time",0},{"position",{0,0,40}},{"angles",{0,0,15}},{"fov",60}},
                                       {{"id","b"},{"time",5},{"position",{50,0,40}},{"angles",{0,0,25}},{"fov",110}}})}};
    send("import_project",{{"project",project}});
    assert(range()["out"]==5);
    send("set_project",{{"render_range",{{"in",1},{"out",2}}}});
    assert(range()==Json({{"mode","composition"},{"in",1},{"out",2}}));
    send("set_project",{{"render_range",{{"mode","region"}}}});
    assert(range()==Json({{"mode","region"},{"in",1},{"out",2}}));
    send("set_project",{{"render_range",{{"in",1.25}}}});
    assert(range()["out"]==2 && range()["mode"]=="region");
    send("undo",Json::object());assert(range()["in"]==1);
    send("redo",Json::object());assert(range()["in"]==1.25);
    const auto before=State(controller)["project"];
    send("set_project",{{"render_range",{{"out",1}}}});
    assert(State(controller)["project"]==before && State(controller).contains("error"));
    send("set_project",{{"render_range",{{"in",1}}}});
    send("scrub",{{"time",.5}});
    commands.clear();send("preview",{{"playing",true},{"sync_demo",true}},3000);
    assert(State(controller)["cursor"]==1 && State(controller)["preview_playing"]==true);
    assert(std::find(commands.begin(),commands.end(),"demo_gototick 1064")!=commands.end());
    assert(std::find(commands.begin(),commands.end(),"demo_resume")!=commands.end());
    frame(3000);assert(State(controller)["cursor"]==1 && State(controller)["preview_playing"]==true);
    frame(1064);assert(State(controller)["cursor"]==1 && State(controller)["preview_playing"]==true);
    frame(1100);assert(State(controller)["cursor"].get<double>()>1 && State(controller)["cursor"].get<double>()<2);
    commands.clear();frame(1130);
    assert(State(controller)["cursor"]==2 && State(controller)["preview_playing"]==false);
    assert(std::abs(State(controller)["camera"]["position"][0].get<double>()-20)<1e-7);
    assert(std::abs(State(controller)["camera"]["fov"].get<double>()-80)<1e-7);
    assert(std::find(commands.begin(),commands.end(),"demo_pause")!=commands.end());
    send("preview",{{"playing",true},{"sync_demo",true}},3000);
    assert(State(controller)["cursor"]==1); // Out restarts at In.
    commands.clear();send("set_project",{{"render_range",{{"mode","composition"}}}},3000);
    assert(range()["in"]==1 && range()["out"]==2);
    assert(!State(controller)["preview_playing"].get<bool>());
    assert(std::find(commands.begin(),commands.end(),"demo_pause")!=commands.end());
    send("scrub",{{"time",5}});
    send("preview",{{"playing",true},{"sync_demo",true}},3000);
    assert(State(controller)["cursor"]==0); // Composition ignores stored region bounds.
    frame(1320); // Wait until the actual asynchronous target has been acknowledged.
    assert(State(controller)["preview_playing"]==true);
    frame(1000);frame(1400);
    assert(State(controller)["cursor"]==5 && State(controller)["preview_playing"]==false);

    // FOV-only commands retain position/rotation; key FOV edits retain its pose.
    const auto poseBefore=State(controller)["camera"];
    send("set_camera",{{"fov",75}});
    assert(State(controller)["camera"]["position"]==poseBefore["position"]);
    assert(State(controller)["camera"]["angles"]==poseBefore["angles"]);
    assert(State(controller)["camera"]["fov"]==75);
    send("update_key",{{"id","b"},{"fov",100}});
    assert(State(controller)["project"]["keys"][1]["position"]==project["keys"][1]["position"]);
    assert(State(controller)["project"]["keys"][1]["angles"]==project["keys"][1]["angles"]);
    assert(State(controller)["project"]["keys"][1]["fov"]==100);
    // Duration reduction never silently removes a key outside the new end.
    const auto keysBefore=State(controller)["project"]["keys"];
    send("set_project",{{"duration",3}});
    assert(State(controller)["project"]["duration"]==5 && State(controller)["project"]["keys"]==keysBefore);

    project["keys"].erase(1);
    send("import_project",{{"project",project}});
    send("set_project",{{"duration",7}});assert(range()["out"]==7);
    send("set_project",{{"render_range",{{"mode","region"},{"in",5},{"out",6}}}});
    send("set_project",{{"duration",4}});
    assert(std::abs(range()["in"].get<double>()-3.99)<1e-9 && range()["out"]==4);
    send("set_project",{{"duration",8}});assert(range()["out"]==8 && range()["in"].get<double>()>3.98);
    send("set_project",{{"render_range",{{"in",1},{"out",2}}}});
    send("set_project",{{"duration",10}});assert(range()["in"]==1 && range()["out"]==2);
    send("set_project",{{"duration",.01}});assert(range()["in"]==0 && range()["out"]==.01);
    assert(State(controller)["project"]["keys"].size()==1);

    // Unsynchronized preview also ends exactly at the selected temporal Out.
    send("import_project",{{"project",project}});
    send("set_project",{{"render_range",{{"mode","region"},{"in",1},{"out",1.01}}}});
    send("scrub",{{"time",0}});
    send("preview",{{"playing",true},{"sync_demo",false}});
    usleep(20000);frame(1000,true);
    assert(State(controller)["cursor"]==1.01 && State(controller)["preview_playing"]==false);
}

static void TestPausedSeekUsesNativePauseFlag(){
    Controller c;std::vector<std::string> commands;const auto execute=[&](const std::string& text){commands.push_back(text);};
    const auto queue=[&](const std::string& action,int tick=0){Json params={{"action",action}};if(action=="seek")params["tick"]=tick;const auto text=Json{{"method","demo_control"},{"params",params}}.dump();c.EnqueueText(text.c_str());};
    queue("seek",100);c.Frame(true,640,true,64,true,"",execute);assert((commands==std::vector<std::string>{"demo_pause","demo_gototick 100 pause"}));
    // A pending native seek reports paused=false until completion. Retain the
    // user's paused intent across a second bare seek during that interval.
    commands.clear();queue("seek",200);c.Frame(true,640,false,64,true,"",execute);assert((commands==std::vector<std::string>{"demo_pause","demo_gototick 200 pause"}));
    commands.clear();queue("pause");queue("seek",300);c.Frame(true,640,false,64,true,"",execute);assert((commands==std::vector<std::string>{"demo_pause","demo_pause","demo_gototick 300 pause"}));
    commands.clear();queue("resume");queue("seek",400);c.Frame(true,640,true,64,true,"",execute);assert((commands==std::vector<std::string>{"demo_resume","demo_gototick 400"}));
    commands.clear();queue("seek",500);c.Frame(true,640,false,64,true,"",execute);assert((commands==std::vector<std::string>{"demo_gototick 500"}));
}
int main() {
    assert(std::abs(Ease(.5,{0,0,1,1})-.5)<1e-8);
    // Invert x(t), rather than applying y directly to time.
    assert(Ease(.5,{.9,0,1,1})<.2);
    Project path;
    path.duration=2;
    Key a,b; a.id="a"; b.id="b"; b.time=2;
    a.pose.angles[1]=179; b.pose.angles[1]=-179;
    a.easing={0,0,1,1}; b.pose.position={10,20,30}; b.pose.fov=120;
    path.keys={a,b}; Normalize(path);
    const Pose middle=Sample(path,1);
    assert(std::abs(middle.position[0]-5)<1e-7);
    assert(std::abs(middle.angles[1]-180)<1e-7);
    assert(std::abs(middle.fov-105)<1e-7);
    for (const auto& angles:std::vector<std::array<double,3>>{{90,30,20},{-90,-170,80},{89.999,179,25},{-70,-179,120}}) {
        const auto rotation=Rotation(angles),roundTrip=Rotation(Euler(rotation));
        assert(std::abs(Dot(rotation,roundTrip))>1-1e-9);
    }
    const auto poleA=Rotation({89,0,10}),poleB=Rotation({89,180,-10});
    const auto pole=Slerp(poleA,poleB,.5);
    assert(std::abs(Dot(pole,Rotation(Euler(pole))))>1-1e-9);
    auto bad=ToJson(path); bad["keys"][1]["time"]=0;
    bool rejected=false; try {ReadProject(bad);} catch(const std::invalid_argument&) {rejected=true;} assert(rejected);

    setenv("CS2_CAMERA_LIVE","1",1);
    TestPausedSeekUsesNativePauseFlag();
    TestSceneEnvironment();
    TestAppearanceDemoRefreshPreservesEditor();
    TestRenderRange();
    Controller controller;
    assert(State(controller)["sync_demo"]==true);
    assert(State(controller)["export_request_id"]==0);
    const auto exportRevision=State(controller)["revision"];
    Command(controller,"request_export");Command(controller,"request_export");
    assert(State(controller)["export_request_id"]==2 && State(controller)["revision"]==exportRevision);
    assert(State(controller)["undo_available"]==false);
    Command(controller,"request_export",{{"unexpected",true}});
    assert(State(controller)["export_request_id"]==2);
    float origin[3]={100,200,300},angles[3]={10,20,0},fov=90;
    assert(!controller.View(origin,angles,&fov,true));
    controller.Frame(true,640,true,64,true,"",[](const std::string&){});
    assert(State(controller)["camera_valid"]==true);
    Command(controller,"add_key",{{"time",0}});
    assert(State(controller)["project"]["start_tick"]==640);
    Command(controller,"set_camera",{{"position",{110,220,330}},{"fov",110}});
    assert(controller.View(origin,angles,&fov,true));
    assert(origin[0]==110 && fov==110);
    Command(controller,"add_key",{{"time",2}});
    auto project=State(controller)["project"];
    assert(project["keys"].size()==2);
    Command(controller,"scrub",{{"time",1}});
    auto position=State(controller)["camera"]["position"];
    assert(position[0].get<double>()>100 && position[0].get<double>()<110);
    const auto revision=State(controller)["revision"];
    Command(controller,"set_camera",{{"fov",999}});
    assert(State(controller)["revision"]==revision);
    assert(State(controller).contains("error"));
    controller.Frame(true,640,true,64,true,"",[](const std::string&){});
    assert(State(controller).contains("error"));
    Command(controller,"get_state");
    assert(State(controller).contains("error"));
    Command(controller,"apply_preset",{{"name","orbit"},{"duration",4}});
    assert(!State(controller).contains("error"));
    assert(State(controller)["project"]["keys"].size()==5);
    Command(controller,"undo"); assert(State(controller)["project"]["keys"].size()==2);
    Command(controller,"redo"); assert(State(controller)["project"]["keys"].size()==5);
    char temporary[]="/tmp/cs2-live-project-XXXXXX";
    char* directory=mkdtemp(temporary); assert(directory);
    const std::string filename=std::string(directory)+"/scene.json";
    setenv("CS2_CAMERA_PROJECT",filename.c_str(),1);
    Command(controller,"set_project",{{"render_range",{{"mode","region"},{"in",1},{"out",3}}}});
    const auto savedRange=State(controller)["project"]["render_range"];
    Command(controller,"save_project");
    struct stat info{}; assert(stat(filename.c_str(),&info)==0 && (info.st_mode&0777)==0600);
    Command(controller,"set_project",{{"name","changed"},{"keys",Json::array()}});
    Command(controller,"load_project");
    assert(State(controller)["project"]["keys"].size()==5);
    assert(State(controller)["project"]["render_range"]==savedRange);
    int broken=open(filename.c_str(),O_WRONLY|O_TRUNC); assert(broken>=0); assert(write(broken,"{broken",7)==7); close(broken);
    const auto beforeLoad=State(controller)["revision"];
    Command(controller,"load_project");
    assert(State(controller)["revision"]==beforeLoad && State(controller).contains("error"));
    assert(unlink(filename.c_str())==0 && rmdir(directory)==0);
    Command(controller,"set_project",{{"render_range",{{"mode","composition"}}}});
    Command(controller,"preview",{{"playing",true}});
    assert(State(controller)["preview_playing"]==true);
    Command(controller,"preview",{{"playing",false}});
    Command(controller,"set_project",{{"start_tick",200}});
    Command(controller,"scrub",{{"time",0}});
    Command(controller,"preview",{{"playing",true},{"sync_demo",true}});
    // Current demo tick 640 belongs to the old scene until async seek completes.
    assert(State(controller)["preview_playing"]==true);
    assert(State(controller)["cursor"]==0);
    controller.Frame(true,200,false,64,true,"",[](const std::string&){});
    assert(State(controller)["preview_playing"]==true);
    controller.Frame(true,456,false,64,true,"",[](const std::string&){});
    assert(State(controller)["preview_playing"]==false);
    Command(controller,"release_camera");
    assert(!controller.View(origin,angles,&fov,true));
    Command(controller,"set_camera",{{"fov",100}},false);
    assert(State(controller)["mode"]=="free");
    assert(State(controller).contains("error"));
    assert(!controller.View(origin,angles,&fov,false));
    Controller freeCamera;
    origin[0]=10; angles[0]=0; fov=90;
    freeCamera.View(origin,angles,&fov,true);
    Command(freeCamera,"add_key");
    freeCamera.Frame(true,1088,false,64,true,"",[](const std::string&){});
    assert(State(freeCamera)["cursor"]==7);
    Command(freeCamera,"add_key");
    assert(State(freeCamera)["project"]["duration"]==7);
    assert(State(freeCamera)["project"]["keys"][1]["time"]==7);
    std::vector<std::string> executed;
    auto sendLogged=[&](const std::string& method,Json params) {
        Json request={{"method",method},{"params",params}};
        const auto text=request.dump(); freeCamera.EnqueueText(text.c_str());
        freeCamera.Frame(true,640,true,64,true,"",[&](const std::string& command){executed.push_back(command);});
    };
    sendLogged("scrub",{{"time",0}});
    sendLogged("preview",{{"playing",true},{"sync_demo",true}});
    executed.clear();
    sendLogged("set_project",{{"name","edited during synchronized preview"}});
    assert(!State(freeCamera)["preview_playing"].get<bool>());
    assert(std::find(executed.begin(),executed.end(),"demo_pause")!=executed.end());
    sendLogged("preview",{{"playing",true},{"sync_demo",true}});
    executed.clear();
    sendLogged("preview",{{"playing",false},{"sync_demo",false}});
    assert(State(freeCamera)["sync_demo"]==false);
    assert(std::find(executed.begin(),executed.end(),"demo_pause")!=executed.end());
    executed.clear();
    sendLogged("release_camera",Json::object());
    sendLogged("preview",{{"playing",false},{"sync_demo",true}});
    assert(State(freeCamera)["mode"]=="free" && State(freeCamera)["sync_demo"]==true);
    assert(executed.empty());
    std::cout << "Live controller: easing, angles, keys, render ranges, hold, scrub, undo, demo gating OK\n";
}
