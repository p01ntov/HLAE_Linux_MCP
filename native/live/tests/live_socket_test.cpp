#include "../live_controller.h"
#include <cassert>
#include <iostream>

using namespace cs2_live;

static Json Request(const std::string& path,const Json& request) {
    int descriptor=socket(AF_UNIX,SOCK_STREAM,0); assert(descriptor>=0);
    sockaddr_un address{}; address.sun_family=AF_UNIX; std::strncpy(address.sun_path,path.c_str(),sizeof(address.sun_path)-1);
    assert(connect(descriptor,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    std::string payload=request.dump()+"\n";
    assert(send(descriptor,payload.data(),payload.size(),MSG_NOSIGNAL)==ssize_t(payload.size()));
    std::string response; char data[8192];
    while(response.find('\n')==std::string::npos) {auto count=recv(descriptor,data,sizeof(data),0);assert(count>0);response.append(data,std::size_t(count));}
    close(descriptor); return Json::parse(response);
}

int main() {
    char temporary[]="/tmp/cs2-live-socket-XXXXXX";
    char* directory=mkdtemp(temporary); assert(directory);
    const std::string path=std::string(directory)+"/control.sock";
    setenv("CS2_CAMERA_LIVE","1",1); setenv("CS2_CAMERA_SOCKET",path.c_str(),1);
    Controller controller; controller.Start();
    struct stat info{}; assert(stat(path.c_str(),&info)==0 && (info.st_mode&0777)==0600);
    auto initial=Request(path,{{"id",1},{"method","get_state"}});
    assert(initial["id"]==1 && initial["result"]["demo"]==false);
    float origin[3]={1,2,3},angles[3]={0,0,0},fov=90;
    controller.View(origin,angles,&fov,true);
    auto pending=std::async(std::launch::async,[&]{return Request(path,{{"id","test"},{"method","add_key"},{"params",{{"time",0}}}});});
    for(int i=0;i<100 && pending.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready;++i) {
        controller.Frame(true,100,true,64,true,"",[](const std::string&){});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready);
    auto reply=pending.get(); assert(reply["id"]=="test" && reply["result"]["id"].is_string());
    auto invalid=std::async(std::launch::async,[&]{return Request(path,{{"id",2},{"method","demo_control"},{"params",{{"action","seek"},{"tick",-1}}}});});
    for(int i=0;i<100 && invalid.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready;++i) {
        controller.Frame(true,100,true,64,true,"",[](const std::string&){});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(invalid.get()["error"]["message"].is_string());
    // A timeout cannot leave a pending command to execute after its caller
    // has already been told it failed.
    auto timeout=Request(path,{{"id",3},{"method","add_key"},{"params",{{"time",2}}}});
    assert(timeout["error"]["message"].get<std::string>().find("2 seconds")!=std::string::npos);
    controller.Frame(true,100,true,64,true,"",[](const std::string&){});
    assert(Json::parse(controller.StateJson())["project"]["keys"].size()==1);
    controller.Stop(); assert(lstat(path.c_str(),&info)!=0); assert(rmdir(directory)==0);
    std::cout<<"Live Unix socket: permissions, framing, replies, validation, timeout cancellation OK\n";
}
