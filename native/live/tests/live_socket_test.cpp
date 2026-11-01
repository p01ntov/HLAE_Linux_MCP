#include "../live_controller.h"
#include <cassert>
#include <iostream>

using namespace cs2_live;

static Json Request(const std::string& path,const Json& request) {
    int descriptor=socket(AF_UNIX,SOCK_STREAM,0); assert(descriptor>=0);
    timeval timeout{5,0};assert(setsockopt(descriptor,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    sockaddr_un address{}; address.sun_family=AF_UNIX; std::strncpy(address.sun_path,path.c_str(),sizeof(address.sun_path)-1);
    assert(connect(descriptor,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    std::string payload=request.dump()+"\n";
    assert(send(descriptor,payload.data(),payload.size(),MSG_NOSIGNAL)==ssize_t(payload.size()));
    std::string response; char data[8192];
    while(response.find('\n')==std::string::npos) {auto count=recv(descriptor,data,sizeof(data),0);assert(count>0);response.append(data,std::size_t(count));}
    close(descriptor); return Json::parse(response);
}

int main() {
    const char* artifacts=std::getenv("CS2_CAMERA_TEST_ARTIFACTS");
    std::string temporary=((artifacts&&*artifacts?std::filesystem::path(artifacts):std::filesystem::temp_directory_path())/"cs2-live-socket-XXXXXX").string();
    char* directory=mkdtemp(temporary.data()); assert(directory);
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

    // Appearance commands formerly stayed queued for 30s even though the
    // default host caller gives up after 5s. The execution-start deadline must
    // cover those methods too, independently of their operation duration.
    const auto before=Json::parse(controller.StateJson())["revision"];
    const auto began=Clock::now();
    auto appearance=Request(path,{{"id",4},{"method","reset_team_palette"}});
    assert(appearance["error"]["message"].get<std::string>().find("cancelled before execution")!=std::string::npos);
    assert(Clock::now()-began<std::chrono::seconds(4));
    controller.Frame(true,100,true,64,true,"",[](const std::string&){});
    assert(Json::parse(controller.StateJson())["revision"]==before);

    // Disconnecting before FRAME_START retires the command. A following ping
    // is a deterministic barrier: this single socket worker accepted/finished
    // the abandoned request before servicing the ping.
    int abandoned=socket(AF_UNIX,SOCK_STREAM,0);assert(abandoned>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;std::strncpy(address.sun_path,path.c_str(),sizeof(address.sun_path)-1);
    assert(connect(abandoned,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    const auto text=Json{{"id",5},{"method","reset_team_palette"}}.dump()+"\n";
    assert(send(abandoned,text.data(),text.size(),MSG_NOSIGNAL)==ssize_t(text.size()));close(abandoned);
    assert(Request(path,{{"id",6},{"method","ping"}})["result"]["pong"]==true);
    controller.Frame(true,100,true,64,true,"",[](const std::string&){});
    assert(Json::parse(controller.StateJson())["revision"]==before);

    // A caller may close only its write half after its one framed request and
    // still wait for the response. That is not an abandoned operation.
    int half=socket(AF_UNIX,SOCK_STREAM,0);assert(half>=0);
    assert(connect(half,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    assert(send(half,text.data(),text.size(),MSG_NOSIGNAL)==ssize_t(text.size()));
    assert(shutdown(half,SHUT_WR)==0);
    pollfd readable{half,POLLIN,0};
    for(int i=0;i<100 && poll(&readable,1,0)==0;++i) {
        controller.Frame(true,100,true,64,true,"",[](const std::string&){});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    char bytes[32768];const auto received=recv(half,bytes,sizeof(bytes),0);assert(received>0);close(half);
    assert(Json::parse(std::string(bytes,std::size_t(received))).contains("result"));

    // FRAME_START enforces expiration itself, not only a socket waiter. This
    // also covers overlay work and avoids an execution race when both threads
    // resume after the deadline.
    controller.EnqueueText("{\"method\":\"reset_team_palette\"}");
    const auto beforeExpiration=Json::parse(controller.StateJson())["revision"];
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    controller.Frame(true,100,true,64,true,"",[](const std::string&){});
    assert(Json::parse(controller.StateJson())["revision"]==beforeExpiration);

    // A command which really started must be reported as uncertain, not
    // cancelled, when its engine callback exceeds the response deadline.
    auto started=std::async(std::launch::async,[&]{return Request(path,{{"id",7},{"method","set_camera"},{"params",{{"fov",80}}}});});
    bool callback=false;
    for(int i=0;i<100 && started.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready;++i) {
        controller.Frame(true,100,true,64,true,"",[&](const std::string&){callback=true;std::this_thread::sleep_for(std::chrono::milliseconds(2200));});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(callback);
    assert(started.get()["error"]["message"].get<std::string>().find("outcome is unknown")!=std::string::npos);
    assert(Json::parse(controller.StateJson())["camera"]["fov"]==80);
    controller.Stop(); assert(lstat(path.c_str(),&info)!=0); assert(rmdir(directory)==0);
    std::cout<<"Live Unix socket: framing, permissions, deadlines, disconnect cancellation and started-operation uncertainty OK\n";
}
