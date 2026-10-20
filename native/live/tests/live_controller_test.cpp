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
    Controller controller;
    assert(State(controller)["sync_demo"]==true);
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
    Command(controller,"save_project");
    struct stat info{}; assert(stat(filename.c_str(),&info)==0 && (info.st_mode&0777)==0600);
    Command(controller,"set_project",{{"name","changed"},{"keys",Json::array()}});
    Command(controller,"load_project");
    assert(State(controller)["project"]["keys"].size()==5);
    int broken=open(filename.c_str(),O_WRONLY|O_TRUNC); assert(broken>=0); assert(write(broken,"{broken",7)==7); close(broken);
    const auto beforeLoad=State(controller)["revision"];
    Command(controller,"load_project");
    assert(State(controller)["revision"]==beforeLoad && State(controller).contains("error"));
    assert(unlink(filename.c_str())==0 && rmdir(directory)==0);
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
    std::cout << "Live controller: easing, angles, keys, hold, scrub, undo, demo gating OK\n";
}
