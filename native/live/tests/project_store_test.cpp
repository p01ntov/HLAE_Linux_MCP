#include "../live_controller.h"
#include "../project_store.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <tuple>

using namespace cs2_live;
namespace fs=std::filesystem;

namespace {
// Nothing in this suite starts the game, opens a socket, or touches the real
// project list. The caller can keep every fixture on its designated test disk.
static fs::path ArtifactRoot() {
    const char* configured=std::getenv("CS2_CAMERA_TEST_ARTIFACTS");
    return configured && *configured?fs::path(configured):fs::temp_directory_path();
}

struct Environment {
    std::map<std::string,std::optional<std::string>> previous;
    void Set(const std::string& name,const std::string& value) {
        if(!previous.count(name)) {
            const char* current=std::getenv(name.c_str());
            previous[name]=current?std::optional<std::string>(current):std::nullopt;
        }
        assert(!setenv(name.c_str(),value.c_str(),1));
    }
    ~Environment(){for(const auto& [name,value]:previous) {
        if(value)setenv(name.c_str(),value->c_str(),1);else unsetenv(name.c_str());
    }}
};

static std::string ReadText(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);assert(input);
    return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
}
static Json ReadJson(const fs::path& path){return Json::parse(ReadText(path));}
static void WriteText(const fs::path& path,const std::string& text) {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);assert(output);
    output<<text;output.close();assert(output);
}
static void WriteJson(const fs::path& path,const Json& value){WriteText(path,value.dump(2)+"\n");}
template<class Function> static bool Throws(Function&& function) {
    try{function();return false;}catch(const std::exception&){return true;}
}

struct Fixture {
    Environment environment;
    fs::path root,library,legacy,demo,otherDemo;
    Fixture() {
        const auto artifacts=ArtifactRoot();fs::create_directories(artifacts);
        std::string pattern=(artifacts/"cs2-project-store-XXXXXX").string();
        std::vector<char> name(pattern.begin(),pattern.end());name.push_back('\0');
        const char* directory=mkdtemp(name.data());assert(directory);root=directory;
        library=root/"projects";legacy=root/"legacy-last-project.json";
        demo=root/"match-a.dem";otherDemo=root/"match-b.dem";
        WriteText(demo,"recording-a");WriteText(otherDemo,"recording-b");
        environment.Set("CS2_CAMERA_LIVE","1");
        environment.Set("CS2_CAMERA_PROJECTS_DIR",library.string());
        environment.Set("CS2_CAMERA_PROJECT",legacy.string());
        environment.Set("CS2_CAMERA_DEMO_PATH",demo.string());
    }
    ~Fixture(){std::error_code ignored;fs::remove_all(root,ignored);}
    Json Recording(char fingerprint='a')const {
        struct stat info{};assert(!stat(demo.c_str(),&info));
        return {{"path",demo.string()},{"fingerprint",{{"sha256",std::string(64,fingerprint)},
            {"bytes",info.st_size},{"mtime_ns",std::int64_t(info.st_mtim.tv_sec)*1000000000+info.st_mtim.tv_nsec}}}};
    }
};

static Json ProjectFixture(const std::string& name,double offset=0) {
    // Distinct rotations, lens values, timing, path interpolation and time
    // curves make a wrong-project reload observable beyond its display name.
    return ToJson(ReadProject({{"name",name},{"start_tick",12000+int(offset*64)},
        {"duration",7.25},{"interpolation","smooth"},
        {"render_range",{{"mode","region"},{"in",.375},{"out",6.625}}},
        {"keys",Json::array({
            {{"id","opening"},{"time",0},{"position",{125.25+offset,-900.5,64.125}},
             {"angles",{-12.5,177.25,4.5}},{"fov",77.5},{"easing",{.125,.25,.75,.875}}},
            {{"id","action"},{"time",2.375},{"position",{375.5+offset,-700.25,92.5}},
             {"angles",{-7.25,-178.5,-1.25}},{"fov",49.25},{"easing",{.2,.1,.6,.95}}},
            {{"id","ending"},{"time",7.25},{"position",{625.75+offset,-450.125,128.75}},
             {"angles",{8.5,-135.25,0.5}},{"fov",105.75},{"easing",{0,0,1,1}}}
        })},{"scene",{{"exposure",1.375},{"dof_override",true},{"dof_near_blurry",-100},
            {"dof_near_crisp",64},{"dof_far_crisp",1250},{"dof_far_blurry",20000},
            {"dof_tilt",.25},{"bloom_enabled",true},{"bloom_strength",-1}}}}));
}
static const ProjectStore::Validate ValidatePortable=[](const Json& value){ReadProject(value);};
static Json Entry(const Json& state,const std::string& id) {
    for(const auto& entry:state.at("entries"))if(entry.at("id")==id)return entry;
    return nullptr;
}

struct SceneBackend {
    std::map<std::string,SceneCvar> values;
    SceneBackend() {
        for(const auto& control:SceneControls()) {
            SceneCvar value;value.available=true;value.nativeType=control.boolean?"bool":"float32";
            value.integer=control.integer;value.value=control.boolean?Json(false):Json(1.0);
            if(std::string(control.id)=="dof_near_blurry")value.value=-100;
            if(std::string(control.id)=="dof_near_crisp")value.value=0;
            if(std::string(control.id)=="dof_far_crisp")value.value=180;
            if(std::string(control.id)=="dof_far_blurry")value.value=2000;
            if(std::string(control.id)=="dof_tilt")value.value=.5;
            value.raw=control.boolean?"0":SceneNumberText(value.value.get<double>());
            values[control.cvar]=value;
        }
        auto& lighting=values.at("mat_fullbright");lighting.integer=true;lighting.nativeType="int32";lighting.value=0;lighting.raw="0";
    }
    void Bind(Controller& controller) {
        controller.SetSceneBackend([this](const std::string& name){return values.at(name);},
            [this](const std::string& name,const std::string& text){
                auto& value=values.at(name);value.raw=text;
                value.value=value.nativeType=="bool"?Json(text=="1" || text=="true"):Json(std::stod(text));return true;
            });
    }
};
static Json State(Controller& controller){return Json::parse(controller.StateJson());}
static Json Send(Controller& controller,const std::string& method,Json params=Json::object(),bool success=true) {
    const auto request=Json{{"method",method},{"params",params}}.dump();
    controller.EnqueueText(request.c_str());
    controller.Frame(true,12480,true,64,true,"",[](const std::string&){});
    auto state=State(controller);
    if(success && state.contains("error"))std::cerr<<method<<": "<<state["error"]<<'\n';
    assert(state.contains("error")!=success);return state;
}
static void Ready(Controller& controller,SceneBackend& backend) {
    backend.Bind(controller);
    float position[3]={100,200,300},angles[3]={0,90,0},fov=90;
    controller.View(position,angles,&fov,true);
    controller.Frame(true,12480,true,64,true,"",[](const std::string&){});
    assert(State(controller)["camera_valid"]==true);
}
static Json CameraHistoryState(const Json& state) {
    Json result=Json::object();
    for(const char* key:{"project","project_document_id","project_storage","revision","undo_available","redo_available","camera","cursor","mode","preview_playing"})
        result[key]=state.at(key);
    return result;
}

static void StoreRoundTripAndRecency() {
    Fixture fixture;ProjectStore store;const auto demo=fixture.Recording();
    const auto a=ProjectFixture("A: wide entrance"),b=ProjectFixture("B: close elimination",80);
    const auto saveA=store.Save(a,"",demo),saveB=store.Save(b,"",demo);
    const auto idA=saveA.at("id").get<std::string>(),idB=saveB.at("id").get<std::string>();
    assert(idA!=idB && saveA["path"]!=saveB["path"]);
    assert(ReadJson(saveA.at("path").get<std::string>())==a);
    assert(ReadJson(saveB.at("path").get<std::string>())==b);
    assert(!a.contains("documentId") && !a.contains("document_id"));
    store.Refresh(demo,ValidatePortable);
    assert(store.State()["entries"].size()==2 && store.State()["error"]=="");
    assert(store.Load(idA,demo,ValidatePortable)==a && store.Load(idB,demo,ValidatePortable)==b);
    // Explicit timestamps avoid wall-clock sleeps and exercise ordering ties.
    for(const auto& [id,saved,opened]:std::vector<std::tuple<std::string,int,int>>{{idA,100,0},{idB,200,0}}) {
        const auto path=fixture.library/(id+".meta");auto metadata=ReadJson(path);
        metadata["saved_at_ns"]=saved;metadata["opened_at_ns"]=opened;WriteJson(path,metadata);
    }
    store.Refresh(demo,ValidatePortable);assert(store.State()["entries"][0]["id"]==idB);
    const auto beforeOpenA=ReadText(saveA.at("path").get<std::string>());
    store.Opened(idA);store.Refresh(demo,ValidatePortable);
    assert(store.State()["entries"][0]["id"]==idA);
    assert(ReadText(saveA.at("path").get<std::string>())==beforeOpenA);
    assert(Entry(store.State(),idA)["key_count"]==3 && Entry(store.State(),idA)["compatible"]==true);
    auto editedA=a;editedA["keys"][1]["fov"]=32.75;
    const auto update=store.Save(editedA,idA,demo);
    assert(update["id"]==idA && update["path"]==saveA["path"]);
    assert(ReadJson(saveA.at("path").get<std::string>())==editedA && ReadJson(saveB.at("path").get<std::string>())==b);
}

static void RecordingIdentityGates() {
    Fixture fixture;ProjectStore store;const auto demo=fixture.Recording();
    const auto save=store.Save(ProjectFixture("Fingerprint-owned cameras"),"",demo);
    const auto id=save.at("id").get<std::string>();
    auto wrong=demo;wrong["fingerprint"]["sha256"]=std::string(64,'b');
    store.Refresh(wrong,ValidatePortable);auto row=Entry(store.State(),id);
    assert(row["compatible"]==false && !row["reason"].get<std::string>().empty());
    const auto before=ReadText(save.at("path").get<std::string>());
    assert(Throws([&]{store.Load(id,wrong,ValidatePortable);}));
    assert(Throws([&]{store.Load(id,nullptr,ValidatePortable);}));
    assert(Throws([&]{store.Load(id,Json{{"path",fixture.demo.string()}},ValidatePortable);}));
    auto moved=demo;moved["path"]=fixture.otherDemo.string();
    store.Refresh(moved,ValidatePortable);
    assert(Entry(store.State(),id)["compatible"]==true); // The recording content, not its path, is authoritative.
    assert(store.Load(id,moved,ValidatePortable)==ReadJson(save.at("path").get<std::string>()));
    auto wrongSize=demo;wrongSize["fingerprint"]["bytes"]=999;
    assert(Throws([&]{store.Load(id,wrongSize,ValidatePortable);}));
    assert(ReadText(save.at("path").get<std::string>())==before);
    const auto pathOnly=store.Save(ProjectFixture("Path-owned cameras"),"",{{"path",fixture.demo.string()}});
    const auto pathId=pathOnly.at("id").get<std::string>();
    store.Refresh(Json{{"path",fixture.otherDemo.string()}},ValidatePortable);
    assert(Entry(store.State(),pathId)["compatible"]==false);
    assert(Throws([&]{store.Load(pathId,Json{{"path",fixture.otherDemo.string()}},ValidatePortable);}));
    store.Refresh(Json{{"path",fixture.demo.string()}},ValidatePortable);
    assert(Entry(store.State(),pathId)["compatible"]==true);
}

static void UntrustedLibraryFilesAreBoundedAndIsolated() {
    Fixture fixture;ProjectStore store;const auto project=ProjectFixture("Valid project");
    const auto save=store.Save(project,"",fixture.Recording());const auto id=save.at("id").get<std::string>();
    const auto outside=fixture.root/"outside.json";WriteJson(outside,project);const auto untouched=ReadText(outside);
    assert(!symlink(outside.c_str(),(fixture.library/"linked.json").c_str()));
    assert(!symlink(outside.c_str(),(fixture.library/"linked-meta.meta").c_str()));
    WriteJson(fixture.library/"linked-meta.json",project);
    fs::create_directory(fixture.library/"directory.json");
    assert(!mkfifo((fixture.library/"pipe.json").c_str(),0600));
    WriteText(fixture.library/"broken.json","{not JSON");
    WriteJson(fixture.library/"invalid.json",{{"duration",-1},{"keys",Json::array()}});
    WriteJson(fixture.library/"bad-meta.json",project);WriteText(fixture.library/"bad-meta.meta","{broken");
    WriteJson(fixture.library/"wrong-meta.json",project);WriteJson(fixture.library/"wrong-meta.meta",{{"version",1},{"id","different"}});
    WriteJson(fixture.library/"last-project.json",project); // Legacy quick save is never a second library row.
    WriteJson(fixture.library/"ignored.JSON",project);
    WriteJson(fixture.library/"..escape.json",project);
    WriteText(fixture.library/"oversized.json",std::string(ProjectStore::MaxFile+1,' '));
    // Metadata-free portable projects remain available, with a visible warning.
    WriteJson(fixture.library/"portable.json",project);
    store.Refresh(fixture.Recording(),ValidatePortable);const auto state=store.State();
    assert(state["error"]=="" && state["entries"].size()==2 && state["skipped"].get<int>()>=8);
    assert(!Entry(state,id).is_null() && !Entry(state,"portable")["warning"].get<std::string>().empty());
    assert(store.Load("portable",fixture.Recording(),ValidatePortable)==project);
    for(const char* invalid:{"../outside","/outside","..","linked","linked-meta","pipe","directory","broken","invalid","bad-meta","wrong-meta","last-project"}) {
        assert(Throws([&]{store.Load(invalid,fixture.Recording(),ValidatePortable);}));
        assert(Throws([&]{store.Opened(invalid);}));
    }
    for(const char* invalid:{"../outside","/outside","..","last-project"})
        assert(Throws([&]{store.Save(project,invalid,fixture.Recording());}));
    assert(Throws([&]{store.Save(project,"linked",fixture.Recording());}));
    assert(Throws([&]{store.Save(project,"linked-meta",fixture.Recording());}));
    assert(ReadText(outside)==untouched);
    // A file substituted after a refresh is checked again at the actual load.
    fs::remove(fixture.library/(id+".json"));assert(!symlink(outside.c_str(),(fixture.library/(id+".json")).c_str()));
    assert(Throws([&]{store.Load(id,fixture.Recording(),ValidatePortable);}));
    assert(ReadText(outside)==untouched);
    const auto alternate=fixture.root/"linked-library";assert(!symlink(fixture.library.c_str(),alternate.c_str()));
    fixture.environment.Set("CS2_CAMERA_PROJECTS_DIR",alternate.string());store.Refresh(fixture.Recording(),ValidatePortable);
    assert(!store.State()["error"].get<std::string>().empty() && store.State()["entries"].empty());
    fixture.environment.Set("CS2_CAMERA_PROJECTS_DIR","relative-projects");
    assert(Throws([]{ProjectStore::Directory();}));
}

static void PortableUnicodeAndSpaceNamesRemainSafe() {
    Fixture fixture;ProjectStore store;fs::create_directories(fixture.library);
    const std::string id="Мой красивый клип 1";
    assert(ProjectStore::ValidId(id));
    const auto project=ProjectFixture("Клип от L201742");WriteJson(fixture.library/(id+".json"),project);
    const std::vector<std::string> invalidUtf8={std::string("broken-")+char(0xff),
        std::string("truncated-")+char(0xd0),std::string("overlong-")+char(0xc0)+char(0xaf)};
    std::size_t createdInvalid=0;
    for(const auto& invalid:invalidUtf8) {
        assert(!ProjectStore::ValidId(invalid));
        const auto path=fixture.library/(invalid+".json");
        const int descriptor=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(descriptor>=0) {
            const auto contents=project.dump();
            assert(write(descriptor,contents.data(),contents.size())==ssize_t(contents.size()));
            assert(!close(descriptor));++createdInvalid;
        }else {
            // NTFS with iocharset=utf8 can reject these names before the
            // application sees them. The ID validator is still checked above.
            assert(errno==EILSEQ || errno==EINVAL);
        }
    }
    for(const std::string& invalid:std::vector<std::string>{"line\nbreak","a\\b","a/b",std::string("embedded\0nul",12)})
        assert(!ProjectStore::ValidId(invalid));
    store.Refresh(fixture.Recording(),ValidatePortable);
    auto state=store.State();assert(state["entries"].size()==1 && state["error"]=="");
    assert(state["skipped"].get<std::size_t>()>=createdInvalid);
    assert(!state.dump().empty()); // Invalid path bytes cannot break state publication.
    assert(Entry(state,id)["title"]==project["name"] && store.Load(id,fixture.Recording(),ValidatePortable)==project);
    store.Opened(id);auto changed=project;changed["keys"][0]["fov"]=88.25;
    const auto saved=store.Save(changed,id,fixture.Recording());
    assert(saved["path"]==(fixture.library/(id+".json")).string());
    assert(ReadJson(fixture.library/(id+".json"))==changed && ReadJson(fixture.library/(id+".meta"))["id"]==id);
    store.Refresh(fixture.Recording(),ValidatePortable);assert(!store.State().dump().empty());
}

static void ControllerTwoProjectsNeverOverwriteEachOther() {
    Fixture fixture;SceneBackend backend;Controller controller;Ready(controller,backend);
    const auto a=ProjectFixture("Controller A"),b=ProjectFixture("Controller B",250);
    const auto importA=Send(controller,"import_project",{{"project",a}});
    const auto documentA=importA.at("project_document_id");
    const auto savedA=Send(controller,"save_project");const auto storageA=savedA.at("project_storage");
    assert(!storageA.at("id").get<std::string>().empty());
    const auto importedB=Send(controller,"import_project",{{"project",b}});
    assert(importedB["project_document_id"]!=documentA);
    const auto savedB=Send(controller,"save_project");const auto storageB=savedB.at("project_storage");
    assert(storageB["id"]!=storageA["id"] && storageB["path"]!=storageA["path"]);
    assert(ReadJson(storageA.at("path").get<std::string>())==a && ReadJson(storageB.at("path").get<std::string>())==b);
    auto changedB=b;changedB["keys"][1]["fov"]=25.125;changedB["name"]="Controller B edited";
    Send(controller,"set_project",{{"project",changedB}});
    const auto resavedB=Send(controller,"save_project");assert(resavedB["project_storage"]==storageB);
    const auto bBytes=ReadText(storageB.at("path").get<std::string>());
    Send(controller,"refresh_project_library");
    const auto loadedA=Send(controller,"load_saved_project",{{"id",storageA["id"]}});
    assert(loadedA["project"]==a && loadedA["project_storage"]==storageA);
    assert(loadedA["project_document_id"]!=documentA);
    assert(backend.values.at("mat_tonemap_force_scale").value==1.375);
    assert(backend.values.at("r_dof_override_far_blurry").value==20000);
    Send(controller,"save_project");
    assert(ReadJson(storageA.at("path").get<std::string>())==a && ReadText(storageB.at("path").get<std::string>())==bBytes);
    assert(ReadJson(fixture.legacy)==a); // Existing quick-save consumers still receive portable JSON.
}

static void ControllerStorageFollowsDocumentThroughUndoAndRedo() {
    Fixture fixture;SceneBackend backend;Controller controller;Ready(controller,backend);
    const auto a=ProjectFixture("Undo A"),b=ProjectFixture("Undo B",500);
    Send(controller,"import_project",{{"project",a}});const auto savedA=Send(controller,"save_project");
    Send(controller,"import_project",{{"project",b}});const auto savedB=Send(controller,"save_project");
    const auto storageA=savedA["project_storage"],storageB=savedB["project_storage"];
    const auto undone=Send(controller,"undo");
    assert(undone["project"]==a && undone["project_document_id"]==savedA["project_document_id"] && undone["project_storage"]==storageA);
    const auto redone=Send(controller,"redo");
    assert(redone["project"]==b && redone["project_document_id"]==savedB["project_document_id"] && redone["project_storage"]==storageB);
    Send(controller,"undo");
    const auto bBytes=ReadText(storageB.at("path").get<std::string>());
    auto edited=a;edited["keys"][0]["fov"]=112.5;
    Send(controller,"update_key",{{"id","opening"},{"fov",112.5}});
    const auto resaved=Send(controller,"save_project");
    assert(resaved["project_storage"]==storageA && ReadJson(storageA.at("path").get<std::string>())==edited);
    assert(ReadText(storageB.at("path").get<std::string>())==bBytes);
    // Importing an earlier saved document's portable JSON is a new document;
    // it must not inherit the current (B or A) file association accidentally.
    Send(controller,"import_project",{{"project",b}});
    const auto newSave=Send(controller,"save_project");
    assert(newSave["project_storage"]["id"]!=storageA["id"] && newSave["project_storage"]["id"]!=storageB["id"]);
    assert(ReadText(storageB.at("path").get<std::string>())==bBytes);
}

static void ControllerSafeExitCopyPreservesFailedOriginal() {
    Fixture fixture;SceneBackend backend;Controller controller;Ready(controller,backend);
    const auto original=ProjectFixture("Safe exit original");
    Send(controller,"import_project",{{"project",original}});
    const auto first=Send(controller,"save_project");const auto storage=first.at("project_storage");
    const auto path=fs::path(storage.at("path").get<std::string>());
    const auto originalBytes=ReadText(path);
    const auto metadata=path.parent_path()/(storage.at("id").get<std::string>()+".meta");
    // Damaged metadata is deterministic even when tests run as root: an
    // ordinary save must fail before it replaces the original camera file.
    WriteText(metadata,"{damaged metadata");
    auto edited=original;edited["keys"][0]["fov"]=112.5;
    Send(controller,"update_key",{{"id","opening"},{"fov",112.5}});
    const auto before=Send(controller,"set_safe_exit_status",{{"phase","saving"}});
    Send(controller,"save_project",Json::object(),false);
    assert(ReadText(path)==originalBytes && ReadText(metadata)=="{damaged metadata");
    const auto copied=Send(controller,"save_project_copy");const auto recovered=copied.at("project_storage");
    assert(recovered["id"]!=storage["id"] && recovered["path"]!=storage["path"]);
    assert(ReadJson(recovered.at("path").get<std::string>())==edited);
    assert(ReadText(path)==originalBytes && ReadText(metadata)=="{damaged metadata");
    assert(copied["project"]==before["project"] && copied["project_document_id"]==before["project_document_id"] && copied["revision"]==before["revision"]);
    assert(Send(controller,"save_project")["project_storage"]==recovered);
}

static void ControllerSafeExitLocksQueuedEdits() {
    Fixture fixture;SceneBackend backend;Controller controller;Ready(controller,backend);
    const auto original=ProjectFixture("Before queued exit");
    Send(controller,"import_project",{{"project",original}});
    const auto enqueue=[&](const char* method,Json params=Json::object()){
        controller.EnqueueText(Json{{"method",method},{"params",params}}.dump().c_str());
    };
    // A committed edit ahead of the barrier belongs to the saved document;
    // a valid edit behind the barrier must be rejected in the same frame.
    enqueue("set_project",{{"name","Accepted before exit"}});
    enqueue("request_safe_exit");
    enqueue("set_project",{{"name","Rejected after exit"}});
    controller.Frame(true,12480,true,64,true,"",[](const std::string&){});
    auto frozen=State(controller);assert(frozen["project"]["name"]=="Accepted before exit");
    assert(frozen["error"].get<std::string>().find("editing is temporarily locked")!=std::string::npos);
    const std::vector<std::pair<std::string,Json>> edits={
        {"add_key",{{"time",1.0}}},{"update_key",{{"id","opening"},{"fov",100}}},
        {"remove_key",{{"id","opening"}}},{"set_project",{{"name","Should not change"}}},
        {"import_project",{{"project",original}}},{"set_scene",{{"settings",{{"exposure",2.0}}}}},
        {"reset_scene",Json::object()},{"apply_preset",{{"name","hold"},{"duration",5}}},
        {"undo",Json::object()},
        {"guarded_mutation",{{"expected_revision",frozen["revision"]},{"method","set_project"},{"params",{{"name","Nested blocked edit"}}}}}
    };
    for(const char* phase:{"requested","saving","closing"}) {
        Send(controller,"set_safe_exit_status",{{"phase",phase}});
        // A stale Keep editing click from an earlier error must not unlock a
        // new exit requested through the launcher while its UI catches up.
        const auto stale=Send(controller,"set_safe_exit_status",{{"phase","idle"}},false);
        assert(stale["safe_exit"]["phase"]==phase);
        assert(stale["error"].get<std::string>().find("only available after a save error")!=std::string::npos);
        for(const auto& [method,params]:edits) {
            const auto rejected=Send(controller,method,params,false);
            assert(rejected["error"].get<std::string>().find("editing is temporarily locked")!=std::string::npos);
            assert(rejected["project"]==frozen["project"] && rejected["project_document_id"]==frozen["project_document_id"] && rejected["revision"]==frozen["revision"]);
        }
    }
    Send(controller,"set_safe_exit_status",{{"phase","error"},{"error","Fixture save failed"}});
    const auto idle=Send(controller,"set_safe_exit_status",{{"phase","idle"}});
    assert(idle["safe_exit"]["phase"]=="idle" && idle["safe_exit"]["error"]=="");
    assert(Send(controller,"set_project",{{"name","Editing after save failure"}})["project"]["name"]=="Editing after save failure");
}

static void ControllerFailedLoadsDoNotMutateCameraHistory() {
    Fixture fixture;SceneBackend backend;Controller controller;Ready(controller,backend);
    const auto a=ProjectFixture("Protected current camera");
    Send(controller,"import_project",{{"project",a}});const auto saved=Send(controller,"save_project");
    const auto storage=saved.at("project_storage");const auto id=storage.at("id").get<std::string>();
    const auto path=storage.at("path").get<std::string>();
    Send(controller,"refresh_project_library");
    auto before=CameraHistoryState(State(controller));
    auto failed=Send(controller,"load_saved_project",{{"id","../outside"}},false);
    assert(CameraHistoryState(failed)==before);
    failed=Send(controller,"load_saved_project",{{"id",id},{"path","/tmp/arbitrary.json"}},false);
    assert(CameraHistoryState(failed)==before);
    const auto validFile=ReadText(path);WriteText(path,"{broken");
    failed=Send(controller,"load_saved_project",{{"id",id}},false);assert(CameraHistoryState(failed)==before);
    WriteText(path,validFile);
    fixture.environment.Set("CS2_CAMERA_DEMO_PATH",fixture.otherDemo.string());
    Send(controller,"refresh_project_library");before=CameraHistoryState(State(controller));
    assert(Entry(State(controller)["project_library"],id)["compatible"]==false);
    failed=Send(controller,"load_saved_project",{{"id",id}},false);assert(CameraHistoryState(failed)==before);
    fixture.environment.Set("CS2_CAMERA_DEMO_PATH",fixture.demo.string());
    Send(controller,"refresh_project_library");
    auto unsupported=a;unsupported["scene"]["dof_radius"]=2;
    WriteJson(path,unsupported);backend.values.at("r_dof2_radiusscale").available=false;
    before=CameraHistoryState(State(controller));
    failed=Send(controller,"load_saved_project",{{"id",id}},false);assert(CameraHistoryState(failed)==before);
    assert(backend.values.at("mat_tonemap_force_scale").value==1.375);
    assert(backend.values.at("r_dof_override_far_blurry").value==20000);
}
static void DocumentReplacementDropsOnlyOldDeferredSeeks() {
    Fixture fixture;SceneBackend backend;Controller controller;Ready(controller,backend);
    const auto a=ProjectFixture("Pending A"),b=ProjectFixture("Pending B",250);
    Send(controller,"import_project",{{"project",a}});
    const auto savedA=Send(controller,"save_project");const auto id=savedA["project_storage"]["id"];
    Send(controller,"import_project",{{"project",b}});
    Send(controller,"demo_control",{{"action","seek"},{"tick",10000}});
    auto queued=Send(controller,"demo_control",{{"action","seek"},{"tick",20000}});
    assert(queued["demo_seek"]["dispatched_tick"]==10000 && queued["demo_seek"]["target_tick"]==20000);
    // A rejected load must preserve the current document and both seek intents.
    const auto rejected=Send(controller,"load_saved_project",{{"id","missing-project"}},false);
    assert(rejected["project_document_id"]==queued["project_document_id"] && rejected["demo_seek"]==queued["demo_seek"]);
    const auto loaded=Send(controller,"load_saved_project",{{"id",id}});
    assert(loaded["project"]==a && loaded["project_document_id"]!=queued["project_document_id"]);
    assert(loaded["demo_seek"]["active"]==true && loaded["demo_seek"]["queued"]==false && loaded["demo_seek"]["target_tick"]==10000);
    std::vector<std::string> commands;
    controller.Frame(true,10000,true,64,true,"",[&](const std::string& text){commands.push_back(text);});
    assert(commands.empty() && State(controller)["demo_seek"]["active"]==false);
    // New-document import receipts still confirm the exact imported document.
    Send(controller,"demo_control",{{"action","seek"},{"tick",30000}});
    Send(controller,"demo_control",{{"action","seek"},{"tick",40000}});
    auto imported=Send(controller,"import_project",{{"project",b},{"import_token","pending-doc-import"}});
    assert(imported["demo_seek"]["queued"]==false && imported["project_import_receipt"]["applied"]==true);
    assert(imported["project_import_receipt"]["token"]=="pending-doc-import" && imported["project_import_receipt"]["project_document_id"]==imported["project_document_id"]);
    auto invalid=b;invalid["keys"][1]["time"]=0;
    queued=Send(controller,"demo_control",{{"action","seek"},{"tick",50000}});
    const auto failedImport=Send(controller,"import_project",{{"project",invalid},{"import_token","bad-pending-import"}},false);
    assert(failedImport["project"]==b && failedImport["demo_seek"]==queued["demo_seek"]);
    assert(failedImport["project_import_receipt"]["applied"]==false);
    auto undone=Send(controller,"undo");assert(undone["project"]==a && undone["demo_seek"]["queued"]==false);
    Send(controller,"demo_control",{{"action","seek"},{"tick",60000}});
    auto redone=Send(controller,"redo");assert(redone["project"]==b && redone["demo_seek"]["queued"]==false);
    // The legacy recovery-copy loader also starts a fresh document.
    Send(controller,"demo_control",{{"action","seek"},{"tick",70000}});
    auto recovered=Send(controller,"load_project");assert(recovered["project"]==a && recovered["demo_seek"]["queued"]==false);
    assert(recovered["demo_seek"]["dispatched_tick"]==30000);
    // Loading while preview seeks must not let its delayed completion resume it.
    controller.Frame(true,30000,true,64,true,"",[](const std::string&){});
    Send(controller,"preview",{{"playing",true},{"sync_demo",true}});
    const int previewTick=State(controller)["demo_seek"]["dispatched_tick"].get<int>();
    auto stopped=Send(controller,"load_saved_project",{{"id",id}});
    assert(stopped["project"]==a && stopped["preview_playing"]==false && stopped["mode"]=="hold");
    commands.clear();controller.Frame(true,previewTick,true,64,true,"",[&](const std::string& text){commands.push_back(text);});
    assert(std::find(commands.begin(),commands.end(),"demo_resume")==commands.end());
}

static void PublishedLibraryHasABoundedByteBudget() {
    Fixture fixture;ProjectStore store;const auto project=ProjectFixture("Long recording path");
    auto demo=fixture.Recording();demo["path"]="/"+std::string(3900,'a');
    for(int index=0;index<40;++index) {
        const auto saved=store.Save(project,"",demo);const auto id=saved["id"].get<std::string>();
        const auto path=fixture.library/(id+".meta");auto metadata=ReadJson(path);
        metadata["demo"]["fingerprint"]["discardedPayload"]=std::string(4000,'x');WriteJson(path,metadata);
    }
    store.Refresh(fixture.Recording(),ValidatePortable);const auto state=store.State();
    assert(state["error"]=="" && state["truncated"]==true && !state["entries"].empty() && state["entries"].size()<40);
    assert(state["entries"].dump().size()<=ProjectStore::MaxStateBytes+ProjectStore::MaxProjects*2);
    for(const auto& row:state["entries"])assert(!row["demo"]["fingerprint"].contains("discardedPayload"));
}
} // namespace

int main() {
    StoreRoundTripAndRecency();RecordingIdentityGates();UntrustedLibraryFilesAreBoundedAndIsolated();
    PortableUnicodeAndSpaceNamesRemainSafe();
    PublishedLibraryHasABoundedByteBudget();
    ControllerTwoProjectsNeverOverwriteEachOther();ControllerStorageFollowsDocumentThroughUndoAndRedo();
    ControllerSafeExitCopyPreservesFailedOriginal();ControllerSafeExitLocksQueuedEdits();
    ControllerFailedLoadsDoNotMutateCameraHistory();
    DocumentReplacementDropsOnlyOldDeferredSeeks();
    std::cout<<"Project store: complete camera round trips, separate document saves, undo associations, bounded recents, recording gates and rejected-load isolation OK\n";
}
