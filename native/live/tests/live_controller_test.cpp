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

static void TestGuardedCameraMutation() {
    Controller controller;float origin[3]={100,200,300},angles[3]={10,20,0},fov=90;
    controller.View(origin,angles,&fov,true);
    controller.Frame(true,640,true,64,true,"",[](const std::string&){});
    const auto initial=State(controller);const auto revision=initial["revision"];
    Command(controller,"guarded_mutation",{{"expected_revision",revision},{"method","set_camera"},{"params",{{"position",{110,220,330}}}}});
    assert(State(controller)["camera"]["position"]==Json::array({110,220,330}));
    const auto accepted=State(controller);
    Command(controller,"guarded_mutation",{{"expected_revision",revision},{"method","set_camera"},{"params",{{"position",{999,999,999}}}}});
    assert(State(controller)["camera"]==accepted["camera"] && State(controller)["project"]==accepted["project"]);
    assert(State(controller)["revision"]==accepted["revision"]);
    assert(State(controller)["error"].get<std::string>().find("changed during geometry validation")!=std::string::npos);
    Command(controller,"guarded_mutation",{{"expected_revision",accepted["revision"]},{"method","guarded_mutation"},{"params",Json::object()}});
    assert(State(controller)["camera"]==accepted["camera"]);
    Command(controller,"guarded_mutation",{{"expected_revision",accepted["revision"]},{"expected_tick",641},{"expected_paused",true},{"method","set_camera"},{"params",{{"position",{999,999,999}}}}});
    assert(State(controller)["camera"]==accepted["camera"] && State(controller)["revision"]==accepted["revision"]);
    Command(controller,"guarded_mutation",{{"expected_revision",accepted["revision"]},{"expected_tick",640},{"expected_paused",false},{"method","set_camera"},{"params",{{"position",{999,999,999}}}}});
    assert(State(controller)["camera"]==accepted["camera"] && State(controller)["revision"]==accepted["revision"]);
    assert(State(controller)["guarded_mutation_abi"]==1);
    const Json preview={{"map","de_nuke"},{"status","partial"},{"reason","Static geometry; dynamic entities unknown"},{"bounds",{-1000,-2500,3000,1000}},
        {"layers",Json::array({{{"z",-350},{"lines",Json::array({Json::array({0,0,100,100})})}}})}};
    Command(controller,"set_map_preview",{{"preview",preview}});
    assert(State(controller)["map_preview"]["map"]=="de_nuke" && State(controller)["map_preview"]["layers"].size()==1);
    assert(!Json::parse(controller.UiStateJson()).contains("map_preview"));
    assert(Json::parse(controller.MapPreviewJson())==State(controller)["map_preview"]);
    auto invalid=preview;invalid["layers"][0]["lines"][0][0]="bad";
    Command(controller,"set_map_preview",{{"preview",invalid}});assert(State(controller)["map_preview"]["layers"]==preview["layers"]);
    assert(State(controller)["project"]==accepted["project"]); // Preview metadata never rewrites a camera document.
    const auto projectBeforeCheck=State(controller)["project"];
    Command(controller,"request_geometry_validation");
    const auto request=State(controller)["geometry_validation_request_id"];
    const auto checkRevision=State(controller)["revision"];
    assert(State(controller)["geometry_validation_current"] && State(controller)["geometry_validation"]["status"]=="pending");
    Command(controller,"set_geometry_validation",{{"request_id",request},{"expected_revision",checkRevision},
        {"result",{{"passed",false},{"status","blocked"},{"collision",{{"position",{100,200,300}}}}}}});
    assert(State(controller)["geometry_validation"]["status"]=="blocked");
    assert(State(controller)["project"]==projectBeforeCheck && State(controller)["revision"]==checkRevision);
    Command(controller,"set_camera",{{"fov",100}});
    assert(!State(controller)["geometry_validation_current"].get<bool>());
    Command(controller,"set_geometry_validation",{{"request_id",request},{"expected_revision",checkRevision},
        {"result",{{"passed",true},{"status","clear"}}}});
    assert(State(controller)["geometry_validation"]["status"]=="blocked");
    Command(controller,"request_geometry_validation");
    Command(controller,"set_geometry_validation",{{"request_id",request},{"expected_revision",State(controller)["revision"]},
        {"result",{{"passed",true},{"status","clear"}}}});
    assert(State(controller)["geometry_validation"]["status"]=="pending"); // Old request cannot certify a new one.
    Command(controller,"apply_preset",{{"name","hold"},{"duration",2}});
    Command(controller,"scrub",{{"time",1}});assert(State(controller)["cursor"]==1);
    const auto beforeReset=State(controller);
    Command(controller,"guarded_mutation",{{"expected_revision",beforeReset["revision"]},{"expected_tick",640},{"expected_paused",true},
        {"reset_cursor",true},{"method","set_project"},{"params",{{"name","guarded preset"}}}});
    assert(State(controller)["cursor"]==0 && State(controller)["project"]["name"]=="guarded preset");
    Command(controller,"scrub",{{"time",1}});const auto beforeInvalidReset=State(controller);
    Command(controller,"guarded_mutation",{{"expected_revision",beforeInvalidReset["revision"]},{"reset_cursor",true},
        {"method","set_camera"},{"params",{{"position",{999,999,999}}}}});
    assert(State(controller)["cursor"]==1 && State(controller)["camera"]==beforeInvalidReset["camera"]);
    const auto staleFullKeys=State(controller);
    const auto otherKey=staleFullKeys["project"]["keys"][1]["id"];
    Command(controller,"update_key",{{"id",otherKey},{"fov",100}});
    const auto newerProject=State(controller)["project"];
    Command(controller,"guarded_mutation",{{"expected_revision",staleFullKeys["revision"]},{"method","set_project"},
        {"params",{{"keys",staleFullKeys["project"]["keys"]}}}});
    assert(State(controller)["project"]==newerProject);
    assert(State(controller)["error"].get<std::string>().find("changed during geometry validation")!=std::string::npos);
}

static void TestValueCurves() {
    Project p;p.duration=4.7;p.interpolation="bezier";p.rotationInterpolation="bezier";p.fovInterpolation="bezier";
    for(int i=0;i<3;++i) {Key k;k.id=std::to_string(i);k.time=std::array<double,3>{0,1.3,4.7}[i];
        k.pose.position=std::array<std::array<double,3>,3>{{{0,0,0},{13,6,3},{90,25,12}}}[i];
        k.pose.angles=std::array<std::array<double,3>,3>{{{0,170,5},{10,-170,15},{-5,-135,30}}}[i];
        k.pose.fov=std::array<double,3>{60,80,110}[i];p.keys.push_back(k);}
    Normalize(p);
    const double h=.0001,t=p.keys[1].time;const auto left=Sample(p,t-h),middle=Sample(p,t),right=Sample(p,t+h);
    for(int axis=0;axis<3;++axis) {
        assert(std::abs((middle.position[axis]-left.position[axis])/h-(right.position[axis]-middle.position[axis])/h)<.02);
        assert(std::abs((middle.angles[axis]-left.angles[axis])/h-(right.angles[axis]-middle.angles[axis])/h)<.02);
    }
    assert(std::abs((middle.fov-left.fov)/h-(right.fov-middle.fov)/h)<.02);
    assert(std::abs(Sample(p,p.duration).angles[1]-225)<1e-8);
    auto first=PositionControls(p,0),second=PositionControls(p,1);
    for(int axis=0;axis<3;++axis)assert(std::abs(3*(first[3][axis]-first[2][axis])/1.3-3*(second[1][axis]-second[0][axis])/3.4)<1e-10);
    p.duration=2;p.renderRange.reset();p.keys.resize(2);p.keys[1].time=2;p.keys[0].pose.position={0,0,0};p.keys[1].pose.position={40,0,0};
    p.keys[0].positionOut=std::array<double,3>{0,40,0};p.keys[1].positionIn=std::array<double,3>{0,40,0};
    p.keys[0].pose.angles={0,0,0};p.keys[1].pose.angles={0,90,0};
    p.keys[0].rotationOut=std::array<double,3>{0,120,0};p.keys[1].rotationIn=std::array<double,3>{0,0,0};
    p.keys[0].pose.fov=60;p.keys[1].pose.fov=100;p.keys[0].fovOut=40;p.keys[1].fovIn=40;Normalize(p);
    const auto custom=Sample(p,1);assert(std::abs(custom.position[0]-20)<1e-8 && std::abs(custom.position[1]-30)<1e-8);
    assert(std::abs(custom.angles[1]-90)<1e-8 && std::abs(custom.fov-110)<1e-8);
    assert(ToJson(ReadProject(Json::parse(ToJson(p).dump())))==ToJson(p));
    p.keys[0].rotationOut=std::array<double,3>{1000,0,0};p.keys[1].rotationIn=std::array<double,3>{1000,0,0};
    p.keys[0].fovOut=1000;p.keys[1].fovIn=1000;Normalize(p);
    assert(Sample(p,1).angles[0]==90 && Sample(p,1).fov==179);
    auto key=ReadKey({{"id","channels"},{"position_out",{10,20,30}},{"fov_in",5},{"rotation_easing",{0,0,1,1}}});
    const auto stored=ToJson(key);assert(stored.contains("position_out") && stored.contains("fov_in"));
    key=ReadKey({{"position_out",nullptr},{"fov_in",nullptr},{"rotation_easing",nullptr}},key);
    assert(!ToJson(key).contains("position_out") && !ToJson(key).contains("fov_in") && !ToJson(key).contains("rotation_easing"));
    Controller controller;float origin[3]={0,0,0},angles[3]={0,0,0},fov=90;controller.View(origin,angles,&fov,true);
    Command(controller,"import_project",{{"project",ToJson(p)}});
    const auto original=State(controller)["project"];
    Command(controller,"update_key",{{"id","0"},{"position_out",nullptr},{"rotation_easing",{.9,0,1,1}}});
    assert(!State(controller)["project"]["keys"][0].contains("position_out"));
    Command(controller,"undo");assert(State(controller)["project"]==original);
    Command(controller,"redo");assert(!State(controller)["project"]["keys"][0].contains("position_out"));
    const auto beforeInvalid=State(controller)["project"];
    Command(controller,"update_key",{{"id","0"},{"position_out",{0,0,1000001}}});assert(State(controller)["project"]==beforeInvalid);
}

static void TestEditorPreferences() {
    const auto defaults=EditorPreferenceDefaults();assert(defaults["frame_rate"]==60 && defaults["timeline_height"]==240);
    assert(defaults["workspace_source"]=="camera" && defaults["workspace_tool"]=="timeline" && defaults["workspace_open"]==true);
    assert(defaults["inspector_section"]=="camera" && defaults["demo_navigation_pinned"]==false);
    assert(ValidateEditorPreferences({{"ui_scale",1.25}},{{"ui_scale",1}})["ui_scale"]==1.25);
    for(const auto& patch:std::vector<Json>{{{"frame_rate",true}},{{"frame_rate",59.5}},{{"panel_opacity",.2}},
        {{"ui_scale",1.51}},{{"snap_mode","ticks"}},{{"timeline_height",421}},{{"compact_transport",1}},{{"unknown",true}},
        {{"workspace_source","shot"}},{{"workspace_tool","scene"}},{{"inspector_section","map"}},
        {{"workspace_open",1}},{{"demo_navigation_pinned",nullptr}}}) {
        bool rejected=false;try{ValidateEditorPreferences(patch);}catch(const std::exception&){rejected=true;}assert(rejected);
    }
    const char* base=std::getenv("CS2_PREF_TEST_ROOT");
    const std::string pattern=std::string(base?base:"/tmp")+"/cs2-editor-prefs-XXXXXX";
    std::vector<char> writable(pattern.begin(),pattern.end());writable.push_back(0);char* folder=mkdtemp(writable.data());assert(folder);
    const std::string path=std::string(folder)+"/editor.json",socket=std::string(folder)+"/live.sock";
    auto write=[&](const std::string& contents){int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);
        assert(::write(fd,contents.data(),contents.size())==ssize_t(contents.size()));close(fd);};
    auto previous=[](const char* name)->std::optional<std::string>{const char* value=std::getenv(name);return value?std::optional<std::string>(value):std::nullopt;};
    const auto oldPath=previous("CS2_CAMERA_EDITOR_PREFS"),oldSocket=previous("CS2_CAMERA_SOCKET");
    setenv("CS2_CAMERA_EDITOR_PREFS",path.c_str(),1);setenv("CS2_CAMERA_SOCKET",socket.c_str(),1);
    write(Json({{"version",1},{"preferences",{{"time_unit","frames"},{"frame_rate",120}}}}).dump());
    {
        Controller controller;controller.Start();const auto initial=State(controller);
        assert(initial["editor_preferences"]["time_unit"]=="frames" && initial["editor_preferences"]["frame_rate"]==120);
        assert(initial["editor_preferences"]["workspace_source"]=="camera" && initial["editor_preferences"]["workspace_tool"]=="timeline");
        assert(initial["editor_preferences_revision"]==0 && initial["editor_preferences_error"]=="");
        Command(controller,"set_editor_preferences",{{"frame_rate",24},{"ui_scale",1.25},{"workspace_source","demo"},
            {"workspace_tool","curves"},{"inspector_section","scene"},{"workspace_open",false},{"demo_navigation_pinned",true}},false);
        const auto edited=State(controller);assert(edited["editor_preferences_revision"]==1);
        assert(edited["project"]==initial["project"] && edited["revision"]==initial["revision"]);
        assert(edited["undo_available"]==initial["undo_available"] && edited["redo_available"]==initial["redo_available"]);
        Command(controller,"set_editor_preferences",{{"frame_rate",60},{"unknown",true}},false);
        assert(State(controller)["editor_preferences"]==edited["editor_preferences"] && State(controller)["editor_preferences_revision"]==1);
        Command(controller,"set_editor_preferences",{{"frame_rate",24}},false);assert(State(controller)["editor_preferences_revision"]==1);
        controller.Stop();
    }
    const std::string broken="{\"version\":99,\"preferences\":{\"frame_rate\":24}}";write(broken);
    {
        Controller controller;controller.Start();const auto initial=State(controller);
        assert(initial["editor_preferences"]==defaults && initial["editor_preferences_revision"]==0);
        assert(!initial["editor_preferences_error"].get<std::string>().empty());
        Command(controller,"set_editor_preferences",defaults,false);
        assert(State(controller)["editor_preferences_revision"]==1 && State(controller)["editor_preferences_error"]=="");
        assert(State(controller)["project"]==initial["project"] && State(controller)["revision"]==initial["revision"]);
        controller.Stop();
    }
    auto restore=[](const char* name,const std::optional<std::string>& value){if(value)setenv(name,value->c_str(),1);else unsetenv(name);};
    restore("CS2_CAMERA_EDITOR_PREFS",oldPath);restore("CS2_CAMERA_SOCKET",oldSocket);
    assert(unlink(path.c_str())==0 && rmdir(folder)==0);
}

static void TestCombatSceneEnvironment() {
    FakeSceneCvars backend;
    for(const char* name:{"spec_show_xray","violence_hblood","fx_drawmetalspark"}) {
        backend.entries[name].value=true;backend.entries[name].raw="1";
    }
    SceneEnvironment scene;
    scene.Bind([&](const std::string& name){return backend.Read(name);},
               [&](const std::string& name,const std::string& raw){return backend.Write(name,raw);});
    scene.Refresh();
    const Json disabled={{"xray_enabled",false},{"metal_sparks_enabled",false}};
    for(auto entry=disabled.begin();entry!=disabled.end();++entry) {
        assert(scene.Capabilities()[entry.key()]["available"]==true);
        assert(scene.Capabilities()[entry.key()]["group"]=="Combat");
    }
    scene.Apply(disabled);
    for(auto entry=disabled.begin();entry!=disabled.end();++entry)assert(scene.Capabilities()[entry.key()]["value"]==false);
    assert(backend.writes.size()==2);
    scene.Apply(Json::object());
    for(const char* name:{"spec_show_xray","violence_hblood","fx_drawmetalspark"})assert(backend.entries[name].value==true);
    backend.writes.clear();
    assert(scene.Capabilities()["blood_enabled"]["available"]==false);
    assert(scene.Capabilities()["blood_enabled"]["reason"].get<std::string>().find("does not suppress")!=std::string::npos);
    bool rejected=false;
    auto unsupported=disabled;unsupported["blood_enabled"]=false;
    try{scene.Apply(unsupported);}catch(const std::exception&){rejected=true;}
    assert(rejected && backend.writes.empty()); // A registered but ineffective gate cannot partially apply a batch.
    for(const Json& invalid:std::vector<Json>{{{"blood_enabled",1}},{{"blood_amount",2}},{{"spark_spread",3}}}) {
        rejected=false;try{ValidateSceneSettings(invalid);}catch(const std::exception&){rejected=true;}
        assert(rejected);
    }
}

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
    for(const Json& invalid:std::vector<Json>{{{"dof_max_blur",8.1}},{{"dof_max_blur",.5}},{{"bloom_strength",-1.01}},{{"exposure",true}},{{"fullbright",1}},{{"raw_console","quit"}},{{"exposure",std::numeric_limits<double>::infinity()}}}) {
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
    Command(controller,"save_project");
    const std::string libraryPath=State(controller)["project_storage"]["path"].get<std::string>();
    Command(controller,"reset_scene");Command(controller,"load_project");
    assert(State(controller)["project"]["scene"]==good["scene"]);
    assert(engine.entries["mat_tonemap_force_scale"].value==2);
    controller.RestoreScene();assert(engine.entries["mat_tonemap_force_scale"].value==1);
    const std::string libraryMeta=libraryPath.substr(0,libraryPath.size()-5)+".meta";
    assert(unlink(filename.c_str())==0 && unlink(libraryPath.c_str())==0 && unlink(libraryMeta.c_str())==0 && rmdir(directory)==0);
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

static void TestScenePostprocessGates() {
    FakeSceneCvars backend;
    auto& postprocess=backend.entries.at("r_csgo_postprocess_enable");
    postprocess.value=true;postprocess.raw="1";
    auto& bloom=backend.entries.at("r_csgo_render_post_bloom");
    bloom.nativeType="int32";bloom.integer=true;bloom.value=2;bloom.raw="2";
    SceneEnvironment scene;
    scene.Bind([&](const std::string& name){return backend.Read(name);},
               [&](const std::string& name,const std::string& raw){return backend.Write(name,raw);});
    scene.Refresh();
    assert(scene.Capabilities()["postprocess_enabled"]["value"]==true);
    assert(scene.Capabilities()["bloom_enabled"]["value"]==true);
    assert(scene.Capabilities()["bloom_enabled"]["native_type"]=="int32");
    assert(scene.Capabilities()["bloom_strength"]["minimum"]==-1);
    scene.Apply({{"bloom_strength",3},{"exposure",2}});
    scene.Apply({{"bloom_strength",-1},{"exposure",2}});
    assert(backend.entries.at("r_csgo_render_post_bloom_strength").value==-1);
    assert(backend.entries.at("mat_tonemap_force_scale").value==2);
    const auto beforeAutomatic=backend.writes.size();
    scene.Apply({{"bloom_strength",-1},{"exposure",2}});
    assert(backend.writes.size()==beforeAutomatic);
    scene.Apply(Json::object());
    auto& strength=backend.entries.at("r_csgo_render_post_bloom_strength");
    strength.minimum=0;
    scene.Refresh();
    bool automaticRejected=false;
    try{scene.Apply({{"bloom_strength",-1}});}catch(const std::invalid_argument&){automaticRejected=true;}
    assert(automaticRejected && scene.Capabilities()["bloom_strength"]["minimum"]==0);
    strength.minimum.reset();
    scene.Refresh();
    scene.Apply({{"postprocess_enabled",false},{"bloom_enabled",false},{"bloom_strength",3}});
    assert(postprocess.value==false && bloom.value==0);
    assert(backend.entries.at("r_csgo_render_post_bloom_strength").value==3);
    scene.Apply(Json::object());
    assert(postprocess.value==true && bloom.value==2 && bloom.raw=="2");

    // Both gates are optional runtime capabilities. An absent cvar or a
    // non-boolean/non-integer type rejects the complete batch before writing.
    for(const auto& gate:std::vector<std::pair<std::string,std::string>>{
            {"postprocess_enabled","r_csgo_postprocess_enable"},
            {"bloom_enabled","r_csgo_render_post_bloom"}}) {
        const auto original=backend.entries.at(gate.second);
        for(bool absent:{true,false}) {
            auto& value=backend.entries.at(gate.second);value=original;
            if(absent){value.available=false;value.reason="cvar is not registered in this build";}
            else {value.nativeType="float32";value.integer=false;value.value=.5;value.raw="0.5";}
            scene.Refresh();
            const auto capabilities=scene.Capabilities();
            assert(capabilities[gate.first]["available"]==false);
            backend.writes.clear();bool rejected=false;
            try{scene.Apply({{gate.first,true},{"exposure",2}});}catch(const std::exception&){rejected=true;}
            assert(rejected && backend.writes.empty());
        }
        backend.entries.at(gate.second)=original;
        scene.Refresh();
        for(const Json& value:std::vector<Json>{0,1,"true",nullptr}) {
            backend.writes.clear();bool rejected=false;
            try{scene.Apply({{gate.first,value}});}catch(const std::exception&){rejected=true;}
            assert(rejected && backend.writes.empty());
        }
    }

    // A successful parser return is insufficient: unchanged engine readback
    // must reject the gate and restore earlier writes in the same batch.
    backend.ignoreName="r_csgo_render_post_bloom";backend.ignoreValue="0";
    backend.writes.clear();bool rejected=false;
    try{scene.Apply({{"postprocess_enabled",false},{"bloom_enabled",false}});}
    catch(const std::exception& error){rejected=true;assert(std::string(error.what()).find("readback")!=std::string::npos);}
    assert(rejected && postprocess.value==true && bloom.value==2);
    backend.ignoreName.clear();

    // A later effect failing also restores both gate values, including an
    // integer engine baseline that cannot be represented by the public bool.
    backend.rejectName="mat_tonemap_force_scale";backend.rejectValue="3";
    rejected=false;
    try{scene.Apply({{"postprocess_enabled",false},{"bloom_enabled",false},{"exposure",3}});}
    catch(const std::exception&){rejected=true;}
    assert(rejected && postprocess.value==true && bloom.value==2 && bloom.raw=="2");

    Controller controller;FakeSceneCvars engine;engine.Bind(controller);
    Command(controller,"set_scene",{{"settings",{{"postprocess_enabled",true},{"bloom_enabled",true}}}});
    assert(State(controller)["project"]["scene"]["postprocess_enabled"]==true);
    assert(State(controller)["project"]["scene"]["bloom_enabled"]==true);
    Command(controller,"set_scene",{{"settings",{{"bloom_strength",3},{"exposure",2}}}});
    Command(controller,"set_scene",{{"settings",{{"bloom_strength",-1}}}});
    assert(State(controller)["project"]["scene"]["bloom_strength"]==-1);
    assert(State(controller)["scene_capabilities"]["bloom_strength"]["value"]==-1);
    assert(engine.entries.at("mat_tonemap_force_scale").value==2);
    Command(controller,"undo");
    assert(engine.entries.at("r_csgo_render_post_bloom_strength").value==3);
    Command(controller,"redo");
    assert(engine.entries.at("r_csgo_render_post_bloom_strength").value==-1);
    Command(controller,"reset_scene");
    assert(State(controller)["project"]["scene"].empty());
    assert(engine.entries.at("r_csgo_postprocess_enable").value==false);
    assert(engine.entries.at("r_csgo_render_post_bloom").value==false);
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
    const auto frame=[&](int tick,bool paused){c.Frame(true,tick,paused,64,true,"",execute);};
    queue("seek",100);frame(640,true);
    assert((commands==std::vector<std::string>{"demo_pause","demo_gototick 100 pause"}));
    assert(State(c)["demo_seek"]["active"]==true && State(c)["demo_seek"]["target_tick"]==100);
    // A seek temporarily reports paused=false. New requests replace only the
    // deferred destination; they do not start expensive native seeks in a loop.
    commands.clear();frame(640,true);assert(commands.empty()); // Old paused sample can arrive before native fast-forward starts.
    queue("seek",200);queue("seek",300);frame(640,false);
    assert(commands.empty() && State(c)["demo_seek"]["target_tick"]==300);
    assert(State(c)["demo_seek"]["dispatched_tick"]==100 && State(c)["demo_seek"]["queued"]==true);
    frame(100,true);
    assert((commands==std::vector<std::string>{"demo_pause","demo_gototick 300 pause"}));
    commands.clear();queue("seek",300);frame(100,false);assert(commands.empty());
    frame(300,true);assert(commands.empty() && State(c)["demo_seek"]["active"]==false);
    // Explicit resume is retained when the current stale state is paused.
    queue("resume");queue("seek",400);frame(300,true);
    assert((commands==std::vector<std::string>{"demo_resume","demo_gototick 400","demo_resume"}));
    commands.clear();queue("seek",500);frame(300,false);assert(commands.empty());
    frame(400,false);assert((commands==std::vector<std::string>{"demo_gototick 500","demo_resume"}));
    commands.clear();frame(500,false);assert(commands.empty() && State(c)["demo_seek"]["active"]==false);
}

static void TestFullDemoSeekReleasesShotAndStopsPreview() {
    Controller c;float position[3]={10,20,30},angles[3]={0,90,0},fov=80;
    c.View(position,angles,&fov,true);
    std::vector<std::string> commands;
    const auto frame=[&](int tick,bool paused){c.Frame(true,tick,paused,64,true,"",[&](const std::string& text){commands.push_back(text);});};
    const auto send=[&](const char* method,Json params,int tick=640,bool paused=true) {
        c.EnqueueText(Json{{"method",method},{"params",params}}.dump().c_str());frame(tick,paused);
    };
    frame(640,true);send("apply_preset",{{"name","dolly"},{"duration",5}});
    send("set_project",{{"render_range",{{"mode","region"},{"in",4.99},{"out",5}}}});
    const auto project=State(c)["project"],revision=State(c)["revision"];
    send("scrub",{{"time",2},{"seek_demo",false}});
    send("demo_control",{{"action","seek"},{"tick",10000}});
    assert(State(c)["project"]==project && State(c)["mode"]=="free");
    assert(State(c)["preview_playing"]==false && State(c)["demo_seek"]["active"]==true);
    frame(10000,true);
    position[0]=77;assert(!c.View(position,angles,&fov,true));send("get_state",Json::object(),10000,true);
    assert(State(c)["camera"]["position"][0]==77); // Native player camera is visible again.
    send("preview",{{"playing",true},{"sync_demo",true}},10000,true);
    assert(State(c)["preview_playing"]==true);
    const int previewTick=State(c)["demo_seek"]["dispatched_tick"].get<int>();
    commands.clear();send("demo_control",{{"action","seek"},{"tick",20000}},10000,false);
    assert(State(c)["mode"]=="free" && State(c)["preview_playing"]==false);
    assert(State(c)["demo_seek"]["target_tick"]==20000 && commands.empty());
    frame(previewTick,false);
    assert(std::find(commands.begin(),commands.end(),"demo_gototick 20000")!=commands.end());
    commands.clear();frame(20000,false);frame(20001,false);
    assert(State(c)["mode"]=="free" && State(c)["preview_playing"]==false);
    assert(std::find(commands.begin(),commands.end(),"demo_pause")==commands.end());
    assert(State(c)["project"]==project && State(c)["revision"].get<int>()>=revision.get<int>());
    // Saved-shot loading uses seek then an explicit local camera scrub. That
    // keeps the imported pose held without starting another native seek.
    send("demo_control",{{"action","seek"},{"tick",30000}},20001,false);
    commands.clear();send("scrub",{{"time",0},{"seek_demo",false}},20001,false);
    assert(State(c)["mode"]=="hold" && commands.empty());
    frame(30000,false);assert(State(c)["mode"]=="hold");
    assert(State(c)["project"]==project);
    // Malformed requests must not release a valid camera or write the engine.
    commands.clear();send("demo_control",{{"action","seek"},{"tick",1.5}},30000,true);
    assert(State(c)["mode"]=="hold" && commands.empty() && State(c).contains("error"));
}

static void TestTinyPreviewDoesNotResumeAfterOut() {
    Controller c;float position[3]={0,0,30},angles[3]={0,0,0},fov=90;c.View(position,angles,&fov,true);
    std::vector<std::string> commands;
    const auto frame=[&](int tick,bool paused){c.Frame(true,tick,paused,64,true,"",[&](const std::string& text){commands.push_back(text);});};
    const auto send=[&](const char* method,Json params,int tick=640,bool paused=true) {
        c.EnqueueText(Json{{"method",method},{"params",params}}.dump().c_str());frame(tick,paused);
    };
    frame(640,true);send("apply_preset",{{"name","hold"},{"duration",5}});
    send("set_project",{{"render_range",{{"mode","region"},{"in",0},{"out",.01}}}});
    send("preview",{{"playing",true},{"sync_demo",true}},641,false);
    assert(State(c)["preview_playing"]==false && State(c)["demo_seek"]["active"]==true);
    commands.clear();frame(640,true);frame(641,true);
    assert(commands.empty() && State(c)["preview_playing"]==false && State(c)["demo_seek"]["active"]==false);
}

static void TestSafeExitRequest() {
    Controller c;std::vector<std::string> executed;
    const auto command=[&](const char* method,Json params=Json::object()){
        c.EnqueueText(Json{{"method",method},{"params",params}}.dump().c_str());
        c.Frame(false,0,true,64,true,"",[&](const std::string& text){executed.push_back(text);});
    };
    command("get_state");const auto initial=State(c);
    command("request_safe_exit",{{"quit",true}});assert(State(c)["safe_exit_request_id"]==0);
    command("request_safe_exit");assert(State(c)["safe_exit_request_id"]==1&&State(c)["safe_exit"]["phase"]=="requested");
    command("request_safe_exit");assert(State(c)["safe_exit_request_id"]==1&&executed.empty());
    for(const char* method:{"add_key","remove_key","set_project","preview","request_export","undo","guarded_mutation"}) {
        command(method);assert(State(c).contains("error")&&State(c)["project"]==initial["project"]&&State(c)["revision"]==initial["revision"]);
    }
    command("set_safe_exit_status",{{"phase","saving"}});
    const auto status=State(c)["safe_exit"];
    for(const Json& invalid:std::vector<Json>{{{"phase","quit"}},{{"phase",1}},{{"phase","error"},{"error",false}},
        {{"phase","error"},{"error",std::string(4097,'x')}},{{"phase","idle"},{"extra",true}}}) {
        command("set_safe_exit_status",invalid);assert(State(c)["safe_exit"]==status);
    }
    command("export_project");assert(!State(c).contains("error"));
    command("save_project");assert(!State(c).contains("error")&&State(c)["project_storage"]["id"].is_string());
    assert(State(c)["project"]==initial["project"]&&State(c)["revision"]==initial["revision"]);
    command("set_safe_exit_status",{{"phase","error"},{"error","Fixture disk full"}});
    command("set_project",{{"name","Still editable"}});assert(State(c)["project"]["name"]=="Still editable");
    command("request_safe_exit");assert(State(c)["safe_exit_request_id"]==2&&State(c)["safe_exit"]["phase"]=="requested");
    command("set_safe_exit_status",{{"phase","closing"}});command("request_safe_exit");assert(State(c)["safe_exit_request_id"]==2);
    command("set_safe_exit_status",{{"phase","idle"}});assert(State(c)["safe_exit"]["phase"]=="closing"&&State(c).contains("error"));
    command("set_safe_exit_status",{{"phase","error"},{"error","Fixture retry failed"}});
    command("set_safe_exit_status",{{"phase","idle"}});assert(State(c)["safe_exit"]["error"]=="");
    command("set_safe_exit_status",{{"phase","idle"}});assert(State(c)["safe_exit"]["phase"]=="idle"&&!State(c).contains("error"));
}

static void TestDemoPlaybackLimits() {
    const char* temporary=std::getenv("CS2_PREF_TEST_ROOT");
    std::string pattern=std::string(temporary?temporary:"/tmp")+"/cs2-demo-limits-XXXXXX";
    std::vector<char> directory(pattern.begin(),pattern.end());directory.push_back(0);assert(mkdtemp(directory.data()));
    const std::string path=std::string(directory.data())+"/fixture.dem";
    const int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);assert(write(fd,"demo",4)==4);close(fd);
    struct stat info{};assert(stat(path.c_str(),&info)==0);
    const char* oldPath=std::getenv("CS2_CAMERA_DEMO_PATH");const auto previous=oldPath?std::optional<std::string>(oldPath):std::nullopt;
    setenv("CS2_CAMERA_DEMO_PATH",path.c_str(),1);
    const Json timeline={{"version",1},{"demo",{{"path",path},{"fingerprint",{{"sha256",std::string(64,'0')},{"bytes",4},
        {"mtime_ns",std::uint64_t(info.st_mtim.tv_sec)*1000000000+std::uint64_t(info.st_mtim.tv_nsec)}}},
        {"playback_ticks",1000},{"tick_rate",64},{"duration_seconds",1000./64},{"authoritative_duration_source","DEM_FileInfo"}}},
        {"rounds",Json::array()},{"players",Json::array()}};
    Controller c;std::vector<std::string> commands;
    const auto frame=[&](int tick,bool paused){c.Frame(true,tick,paused,64,true,"",[&](const std::string& value){commands.push_back(value);});};
    const auto send=[&](const char* method,const Json& args,int tick=10,bool paused=true){
        c.EnqueueText(Json{{"method",method},{"params",args}}.dump().c_str());frame(tick,paused);
    };
    frame(10,true);const auto initial=State(c);
    send("set_demo_playback_range",{{"enabled",true}});assert(State(c).contains("error") && !State(c)["demo_playback_range"]["available"].get<bool>());
    send("set_demo_timeline",{{"timeline",timeline}});assert(State(c)["demo_playback_range"]["out_tick"]==1000);
    send("set_demo_playback_range",{{"enabled",true},{"in_tick",100},{"out_tick",200}});
    const auto bounds=State(c)["demo_playback_range"];
    for(const Json& invalid:std::vector<Json>{Json::object(),{{"enabled",1}},{{"in_tick",true}},{{"in_tick",-1}},{{"in_tick",200}},
        {{"out_tick",100}},{{"out_tick",1001}},{{"out_tick",1.5}},{{"out_tick",UINT64_MAX}},{{"unknown",true}}}) {
        commands.clear();send("set_demo_playback_range",invalid);
        assert(State(c).contains("error") && State(c)["demo_playback_range"]==bounds && commands.empty());
    }
    assert(State(c)["project"]==initial["project"] && State(c)["revision"]==initial["revision"] && State(c)["undo_available"]==false);
    // Metadata refresh of the same recording retains limits, without seeking.
    commands.clear();send("set_demo_timeline",{{"timeline",timeline}});assert(State(c)["demo_playback_range"]==bounds && commands.empty());
    send("demo_control",{{"action","resume"}},10,true);
    assert((commands==std::vector<std::string>{"demo_gototick 100","demo_resume"}));
    commands.clear();frame(50,false);assert(commands.empty()); // Fast-forward is not playback outside In.
    frame(92,false);frame(99,false);assert(commands.empty() && State(c)["demo_seek"]["active"]==true); // Seek tolerance must not pause before In.
    frame(100,false);assert(commands.empty() && !State(c)["demo_seek"]["active"].get<bool>());
    frame(199,false);assert(commands.empty());frame(200,false);assert((commands==std::vector<std::string>{"demo_pause"}));
    commands.clear();frame(201,false);assert(commands.empty()); // Await native pause readback, never spam.
    frame(201,true);send("demo_control",{{"action","resume"}},201,true);
    assert((commands==std::vector<std::string>{"demo_gototick 100","demo_resume"}));
    commands.clear();frame(100,false);send("demo_control",{{"action","pause"}},150,false);frame(150,true);
    commands.clear();send("demo_control",{{"action","seek"},{"tick",900}},150,true);frame(900,true);
    assert(!State(c)["demo_seek"]["active"].get<bool>());commands.clear();frame(900,true);assert(commands.empty()); // Paused navigation remains unrestricted.
    send("set_demo_playback_range",{{"enabled",false}},900,true);commands.clear();send("demo_control",{{"action","resume"}},900,true);
    assert((commands==std::vector<std::string>{"demo_resume"}));commands.clear();frame(999,false);assert(commands.empty());
    assert(State(c)["project"]==initial["project"] && State(c)["revision"]==initial["revision"]);
    for(const bool outside:{true,false}) {
        Controller pending;std::vector<std::string> sent;
        const auto step=[&](int tick,bool paused){pending.Frame(true,tick,paused,64,true,"",[&](const std::string& value){sent.push_back(value);});};
        const auto request=[&](const char* method,Json args,int tick,bool paused){pending.EnqueueText(Json{{"method",method},{"params",args}}.dump().c_str());step(tick,paused);};
        request("set_demo_timeline",{{"timeline",timeline}},150,true);
        request("set_demo_playback_range",{{"enabled",true},{"in_tick",100},{"out_tick",200}},150,true);
        request("demo_control",{{"action","seek"},{"tick",outside?900:103}},outside?150:900,true);
        sent.clear();request("demo_control",{{"action","resume"}},outside?150:900,true);
        if(outside) {
            assert(sent.empty() && State(pending)["demo_seek"]["target_tick"]==100);
            step(900,true);assert((sent==std::vector<std::string>{"demo_gototick 100","demo_resume"}));
        } else {
            assert((sent==std::vector<std::string>{"demo_resume"}) && State(pending)["demo_seek"]["target_tick"]==103);
            sent.clear();step(95,false);step(99,false);assert(sent.empty() && State(pending)["demo_seek"]["active"]==true);
            step(103,false);assert(sent.empty() && State(pending)["demo_seek"]["active"]==false);
        }
    }
    {
        Controller preview;std::vector<std::string> sent;float position[3]={0,0,30},angles[3]={0,0,0},fov=90;preview.View(position,angles,&fov,true);
        const auto step=[&](int tick,bool paused){preview.Frame(true,tick,paused,64,true,"",[&](const std::string& value){sent.push_back(value);});};
        const auto request=[&](const char* method,Json args,int tick=900,bool paused=true){preview.EnqueueText(Json{{"method",method},{"params",args}}.dump().c_str());step(tick,paused);};
        request("set_demo_timeline",{{"timeline",timeline}});request("set_demo_playback_range",{{"enabled",true},{"in_tick",100},{"out_tick",200}});
        request("demo_control",{{"action","resume"}},10,true);sent.clear();request("demo_control",{{"action","pause"}},92,false);sent.clear();step(92,true);
        assert(!State(preview)["demo_seek"]["active"].get<bool>() && sent.empty());
        request("apply_preset",{{"name","hold"},{"duration",2}});request("preview",{{"playing",true},{"sync_demo",true}});
        sent.clear();step(900,false);step(930,false);assert(State(preview)["preview_playing"]==true && sent.empty());
        // Demo bounds never become a camera export/preview range.
        assert(State(preview)["project"]["render_range"]["out"]==2);
    }
    if(previous)setenv("CS2_CAMERA_DEMO_PATH",previous->c_str(),1);else unsetenv("CS2_CAMERA_DEMO_PATH");
    unlink(path.c_str());rmdir(directory.data());
}

static void TestDemoSeekDeadlineAndQueueBound() {
    auto now=Clock::time_point{}+std::chrono::hours(1);
    Controller c([&]{return now;});std::vector<std::string> commands;
    const auto frame=[&](bool demo=true){c.Frame(demo,640,true,64,true,"",[&](const std::string& text){commands.push_back(text);});};
    const auto seek=[&](int target){c.EnqueueText(Json{{"method","demo_control"},{"params",{{"action","seek"},{"tick",target}}}}.dump().c_str());};
    for(int i=0;i<64;++i)seek(10000+i*100);
    frame();
    assert(commands==std::vector<std::string>({"demo_pause","demo_gototick 10000 pause"}));
    assert(State(c)["demo_seek"]["target_tick"]==16300 && State(c)["demo_seek"]["queued"]==true);
    commands.clear();now+=std::chrono::seconds(31);frame();
    assert(commands.empty() && State(c)["demo_seek"]["active"]==false);
    assert(State(c)["error"].get<std::string>().find("no automatic retry")!=std::string::npos);
    for(int i=0;i<100;++i){now+=std::chrono::seconds(1);frame();}
    assert(commands.empty());
    seek(20000);frame();assert(State(c)["demo_seek"]["active"]==true);
    commands.clear();frame(false);assert(State(c)["demo_seek"]["active"]==false && commands.empty());
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
    TestFullDemoSeekReleasesShotAndStopsPreview();
    TestDemoSeekDeadlineAndQueueBound();
    TestDemoPlaybackLimits();
    TestSafeExitRequest();
    TestTinyPreviewDoesNotResumeAfterOut();
    TestGuardedCameraMutation();
    TestValueCurves();
    TestEditorPreferences();
    TestCombatSceneEnvironment();
    TestSceneEnvironment();
    TestScenePostprocessGates();
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
    Command(controller,"set_project",{{"interpolation","bezier"},{"rotation_interpolation","bezier"},{"fov_interpolation","bezier"}});
    const auto curveKey=State(controller)["project"]["keys"][0]["id"];
    Command(controller,"update_key",{{"id",curveKey},{"position_out",{10,20,30}},{"rotation_out",{1,2,3}},
        {"fov_out",5},{"position_easing",{0,0,1,1}},{"rotation_easing",{.2,0,.8,1}},{"fov_easing",{.4,0,.6,1}}});
    const auto savedCurveKey=State(controller)["project"]["keys"][0];
    Command(controller,"set_project",{{"render_range",{{"mode","region"},{"in",1},{"out",3}}}});
    const auto savedRange=State(controller)["project"]["render_range"];
    Command(controller,"save_project");
    const std::string savedLibraryPath=State(controller)["project_storage"]["path"].get<std::string>();
    struct stat info{}; assert(stat(filename.c_str(),&info)==0 && (info.st_mode&0777)==0600);
    Command(controller,"set_project",{{"name","changed"},{"keys",Json::array()}});
    Command(controller,"load_project");
    assert(State(controller)["project"]["keys"].size()==5);
    assert(State(controller)["project"]["keys"][0]==savedCurveKey);
    assert(State(controller)["project"]["rotation_interpolation"]=="bezier" && State(controller)["project"]["fov_interpolation"]=="bezier");
    assert(State(controller)["project"]["render_range"]==savedRange);
    int broken=open(filename.c_str(),O_WRONLY|O_TRUNC); assert(broken>=0); assert(write(broken,"{broken",7)==7); close(broken);
    const auto beforeLoad=State(controller)["revision"];
    Command(controller,"load_project");
    assert(State(controller)["revision"]==beforeLoad && State(controller).contains("error"));
    const auto savedLibraryMeta=savedLibraryPath.substr(0,savedLibraryPath.size()-5)+".meta";
    assert(unlink(filename.c_str())==0 && unlink(savedLibraryPath.c_str())==0 && unlink(savedLibraryMeta.c_str())==0 && rmdir(directory)==0);
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
