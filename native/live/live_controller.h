#pragma once

// The controller has one owner (FRAME_START). Socket and overlay threads only
// enqueue commands or read immutable JSON; the late view callback exchanges
// plain camera samples under a short mutex, never calling the engine itself.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace cs2_live {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static constexpr int Abi = 1;
static constexpr std::size_t MaxLine = 1024 * 1024;

struct Pose {
    std::array<double, 3> position{0,0,0}, angles{0,0,0};
    double fov = 90;
};

inline void Finite(double value, double low, double high, const char* label) {
    if (!std::isfinite(value) || value < low || value > high)
        throw std::invalid_argument(std::string(label) + " is outside its finite range");
}
inline std::array<double,3> Triple(const Json& value, const char* name) {
    if (!value.is_array() || value.size()!=3) throw std::invalid_argument(std::string(name)+" requires 3 values");
    return {value[0].get<double>(), value[1].get<double>(), value[2].get<double>()};
}
inline void Validate(const Pose& pose) {
    for (double value:pose.position) Finite(value,-1000000,1000000,"position");
    for (double value:pose.angles) Finite(value,-1000000,1000000,"angles");
    Finite(pose.angles[0],-90,90,"pitch");
    Finite(pose.fov,1,179,"FOV");
}
inline Json ToJson(const Pose& pose) {
    return {{"position",pose.position},{"angles",pose.angles},{"fov",pose.fov}};
}
inline Pose ReadPose(const Json& value, Pose pose = {}) {
    if (!value.is_object()) throw std::invalid_argument("camera must be an object");
    if (value.contains("position")) pose.position=Triple(value.at("position"),"position");
    if (value.contains("angles")) pose.angles=Triple(value.at("angles"),"angles");
    if (value.contains("fov")) pose.fov=value.at("fov").get<double>();
    Validate(pose); return pose;
}

struct Key {
    std::string id;
    double time=0;
    Pose pose;
    // A straight time curve preserves the Hermite path's velocity at interior
    // keys. Ease-in/out belongs on an intentional segment, not every key.
    std::array<double,4> easing{0,0,1,1};
};
struct Project {
    std::string name="Untitled cinematic";
    int startTick=0;
    double duration=5;
    std::string interpolation="smooth";
    std::vector<Key> keys;
};
inline Json ToJson(const Key& key) {
    Json result=ToJson(key.pose);
    result["id"]=key.id; result["time"]=key.time; result["easing"]=key.easing;
    return result;
}
inline Json ToJson(const Project& project) {
    Json keys=Json::array();
    for (const auto& key:project.keys) keys.push_back(ToJson(key));
    return {{"name",project.name},{"start_tick",project.startTick},{"duration",project.duration},
            {"interpolation",project.interpolation},{"keys",keys}};
}
inline Key ReadKey(const Json& value, Key key={}) {
    if (value.contains("id")) key.id=value.at("id").get<std::string>();
    if (key.id.empty() || key.id.size()>128) throw std::invalid_argument("key id must contain 1..128 characters");
    if (value.contains("time")) key.time=value.at("time").get<double>();
    Finite(key.time,0,86400,"key time");
    key.pose=ReadPose(value,key.pose);
    if (value.contains("easing")) {
        const auto& easing=value.at("easing");
        if (!easing.is_array() || easing.size()!=4) throw std::invalid_argument("easing requires [x1,y1,x2,y2]");
        for (int i=0;i<4;++i) key.easing[i]=easing[i].get<double>();
    }
    // Monotone x allows unique inversion; bounded y prevents lens/position overshoot.
    for (double v:key.easing) Finite(v,0,1,"easing coordinate");
    return key;
}
inline void Normalize(Project& project) {
    if (project.name.size()>256) throw std::invalid_argument("project name is too long");
    Finite(project.duration,0.01,86400,"duration");
    if (project.startTick<0 || project.startTick>2000000000) throw std::invalid_argument("invalid start_tick");
    if (project.interpolation!="linear" && project.interpolation!="smooth") throw std::invalid_argument("interpolation must be linear or smooth");
    if (project.keys.size()>4096) throw std::invalid_argument("project supports at most 4096 keys");
    std::sort(project.keys.begin(),project.keys.end(),[](const Key& a,const Key& b){return a.time<b.time;});
    for (std::size_t i=0;i<project.keys.size();++i) {
        Validate(project.keys[i].pose);
        if (project.keys[i].time>project.duration) throw std::invalid_argument("key exceeds project duration");
        if (i && project.keys[i].time-project.keys[i-1].time<0.000001) throw std::invalid_argument("keys need distinct times");
        for (std::size_t j=0;j<i;++j) if (project.keys[i].id==project.keys[j].id) throw std::invalid_argument("duplicate key id");
    }
    if (ToJson(project).dump().size()>MaxLine-16384) throw std::invalid_argument("project JSON exceeds the local transport limit");
}
inline Project ReadProject(const Json& value) {
    if (!value.is_object()) throw std::invalid_argument("project must be an object");
    Project project;
    project.name=value.value("name",project.name);
    project.startTick=value.value("start_tick",0);
    project.duration=value.value("duration",5.0);
    project.interpolation=value.value("interpolation",std::string("smooth"));
    if (value.contains("keys")) {
        if (!value.at("keys").is_array()) throw std::invalid_argument("keys must be an array");
        for (const auto& key:value.at("keys")) project.keys.push_back(ReadKey(key));
    }
    Normalize(project); return project;
}
inline double BezierCoordinate(double t,double a,double b) {
    const double v=1-t; return 3*v*v*t*a+3*v*t*t*b+t*t*t;
}
inline double Ease(double progress,const std::array<double,4>& curve) {
    if (progress<=0) return 0;
    if (progress>=1) return 1;
    double low=0,high=1;
    for (int i=0;i<36;++i) {
        const double middle=(low+high)*0.5;
        if (BezierCoordinate(middle,curve[0],curve[2])<progress) low=middle; else high=middle;
    }
    return BezierCoordinate((low+high)*0.5,curve[1],curve[3]);
}
inline double AngleDelta(double a,double b) { return std::remainder(b-a,360.0); }
struct Quaternion { double w=1,x=0,y=0,z=0; };
inline Quaternion Rotation(const std::array<double,3>& angles) {
    // Source camera rotation: Rz(yaw) * Ry(pitch) * Rx(roll), with positive
    // pitch looking down. Use half angles to construct the unit quaternion.
    constexpr double radians=3.141592653589793/360.0;
    const double cp=std::cos(angles[0]*radians),sp=std::sin(angles[0]*radians);
    const double cy=std::cos(angles[1]*radians),sy=std::sin(angles[1]*radians);
    const double cr=std::cos(angles[2]*radians),sr=std::sin(angles[2]*radians);
    return {cr*cp*cy+sr*sp*sy,sr*cp*cy-cr*sp*sy,cr*sp*cy+sr*cp*sy,cr*cp*sy-sr*sp*cy};
}
inline double Dot(const Quaternion& a,const Quaternion& b) { return a.w*b.w+a.x*b.x+a.y*b.y+a.z*b.z; }
inline Quaternion Slerp(Quaternion a,Quaternion b,double t) {
    double dot=Dot(a,b);
    if(dot<0) { b={-b.w,-b.x,-b.y,-b.z}; dot=-dot; }
    double left=1-t,right=t;
    if(dot<.9995) {
        const double angle=std::acos(std::clamp(dot,-1.0,1.0)),denominator=std::sin(angle);
        left=std::sin((1-t)*angle)/denominator; right=std::sin(t*angle)/denominator;
    }
    Quaternion result{left*a.w+right*b.w,left*a.x+right*b.x,left*a.y+right*b.y,left*a.z+right*b.z};
    const double norm=std::sqrt(Dot(result,result));
    result.w/=norm; result.x/=norm; result.y/=norm; result.z/=norm;
    return result;
}
inline std::array<double,3> Euler(const Quaternion& q) {
    constexpr double degrees=180.0/3.141592653589793;
    const double sinePitch=std::clamp(2*(q.w*q.y-q.z*q.x),-1.0,1.0);
    if(std::abs(sinePitch)>1-1e-12) {
        // At a pole yaw and roll describe the same degree of freedom. Choose
        // roll=0 using stable matrix entries instead of atan2(0,0).
        return {std::copysign(90.0,sinePitch),std::atan2(2*(q.w*q.z-q.x*q.y),1-2*(q.x*q.x+q.z*q.z))*degrees,0};
    }
    return {std::asin(sinePitch)*degrees,
            std::atan2(2*(q.w*q.z+q.x*q.y),1-2*(q.y*q.y+q.z*q.z))*degrees,
            std::atan2(2*(q.w*q.x+q.y*q.z),1-2*(q.x*q.x+q.y*q.y))*degrees};
}
inline Pose Sample(const Project& project,double time) {
    if (project.keys.empty()) throw std::invalid_argument("add a camera key before preview");
    const auto& keys=project.keys;
    if (time<=keys.front().time) return keys.front().pose;
    if (time>=keys.back().time) return keys.back().pose;
    auto next=std::upper_bound(keys.begin(),keys.end(),time,[](double t,const Key& key){return t<key.time;});
    const std::size_t right=std::size_t(next-keys.begin()), left=right-1;
    const auto& a=keys[left]; const auto& b=keys[right];
    const double span=b.time-a.time, u=Ease((time-a.time)/span,a.easing);
    Pose result;
    for (int axis=0;axis<3;++axis) {
        if (project.interpolation=="linear" || keys.size()<3) result.position[axis]=a.pose.position[axis]+u*(b.pose.position[axis]-a.pose.position[axis]);
        else {
            const auto& previous=keys[left?left-1:left];
            const auto& after=keys[right+1<keys.size()?right+1:right];
            const double tangentA=(b.pose.position[axis]-previous.pose.position[axis])/(b.time-previous.time)*span;
            const double tangentB=(after.pose.position[axis]-a.pose.position[axis])/(after.time-a.time)*span;
            const double u2=u*u,u3=u2*u;
            result.position[axis]=(2*u3-3*u2+1)*a.pose.position[axis]+(u3-2*u2+u)*tangentA
                                 +(-2*u3+3*u2)*b.pose.position[axis]+(u3-u2)*tangentB;
        }
    }
    result.fov=a.pose.fov+u*(b.pose.fov-a.pose.fov);
    result.angles=Euler(Slerp(Rotation(a.pose.angles),Rotation(b.pose.angles),u));
    // Equivalent yaw/roll representations should remain close to the authored
    // key values, including crossings of +/-180 degrees.
    for(int axis=1;axis<3;++axis) {
        const double reference=a.pose.angles[axis]+u*AngleDelta(a.pose.angles[axis],b.pose.angles[axis]);
        result.angles[axis]=reference+AngleDelta(reference,result.angles[axis]);
    }
    return result;
}

class Controller {
public:
    using Execute=std::function<void(const std::string&)>;
    Controller() { const char* enabled=std::getenv("CS2_CAMERA_LIVE"); enabled_=enabled && std::strcmp(enabled,"1")==0; Publish(); }
    ~Controller() { Stop(); }
    bool Enabled() const { return enabled_; }
    void Start() {
        if (!enabled_ || running_.exchange(true)) return;
        const char* path=std::getenv("CS2_CAMERA_SOCKET");
        if (!path || !*path) { startupError_="CS2_CAMERA_SOCKET is required"; running_=false; return; }
        socketPath_=path;
        if (socketPath_.size()>=sizeof(sockaddr_un::sun_path)) { startupError_="socket path is too long"; running_=false; return; }
        const auto separator=socketPath_.find_last_of('/');
        if (separator==std::string::npos || separator==0) { startupError_="socket needs a private parent directory"; running_=false; return; }
        struct stat info{};
        if (lstat(socketPath_.substr(0,separator).c_str(),&info) || !S_ISDIR(info.st_mode) || info.st_uid!=getuid() || (info.st_mode&0077)) {
            startupError_="socket directory must be owned by this user with mode 0700"; running_=false; return;
        }
        listener_=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        if (listener_<0) { startupError_="cannot create local control socket"; running_=false; return; }
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        std::memcpy(address.sun_path,socketPath_.c_str(),socketPath_.size()+1);
        if (bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {
            startupError_="cannot bind local control socket (refusing to replace existing paths)";
            close(listener_); listener_=-1; running_=false; return;
        }
        ownsSocket_=true;
        if (chmod(socketPath_.c_str(),0600) || listen(listener_,8)) {
            startupError_="cannot secure local control socket";
            close(listener_); listener_=-1; unlink(socketPath_.c_str()); ownsSocket_=false; running_=false; return;
        }
        worker_=std::thread([this]{ Serve(); });
    }
    void Stop() {
        running_=false;
        if (worker_.joinable()) worker_.join();
        if (listener_>=0) { close(listener_); listener_=-1; }
        if (ownsSocket_) { unlink(socketPath_.c_str()); ownsSocket_=false; }
        std::deque<std::shared_ptr<Request>> remaining;
        { std::lock_guard<std::mutex> lock(queueMutex_); remaining.swap(queue_); }
        for (auto& request:remaining) Resolve(request,Error(request->input,"live session stopped"));
    }
    std::string StateJson() const { std::lock_guard<std::mutex> lock(stateMutex_); return stateJson_; }
    void EnqueueText(const char* text) {
        if (!enabled_ || !text) return;
        if (strnlen(text,MaxLine+1)>MaxLine) return;
        try { Queue(Json::parse(text)); } catch (const std::exception&) {}
    }
    // Called once per FRAME_START, before all playback-related early returns.
    void Frame(bool demo,int tick,bool paused,double tickRate,bool hookInstalled,const std::string& hookError,const Execute& execute) {
        if (!enabled_) return;
        demo_=demo; tick_=tick; paused_=paused; tickRate_=tickRate>0?tickRate:64;
        hookInstalled_=hookInstalled;
        const std::string priorityError=!startupError_.empty()?startupError_:hookError;
        error_=priorityError.empty()?commandError_:priorityError;
        { std::lock_guard<std::mutex> lock(viewMutex_); if (observedValid_) { cameraValid_=true; if (mode_=="free") camera_=observed_; } }
        if (!demo_) {
            playing_=false; mode_="free"; cameraValid_=false;
            std::lock_guard<std::mutex> lock(viewMutex_); observedValid_=false;
        }
        // Capturing additional keys while freely navigating should use the
        // visible demo time. A later capture can extend the initial duration.
        if (demo_ && mode_=="free" && !paused_ && !project_.keys.empty())
            cursor_=std::clamp((tick_-project_.startTick)/tickRate_,0.0,86400.0);
        std::deque<std::shared_ptr<Request>> ready;
        { std::lock_guard<std::mutex> lock(queueMutex_);
          for (int i=0;i<64 && !queue_.empty();++i) { ready.push_back(queue_.front()); queue_.pop_front(); } }
        for (const auto& request:ready) {
            if (request->cancelled) continue;
            try {
                const bool wasSyncPlaying=playing_ && syncDemo_;
                Json result=Command(request->input,execute);
                const auto method=request->input.at("method").get<std::string>();
                if (wasSyncPlaying && !playing_ && method!="preview" && method!="release_camera" && method!="demo_control")
                    execute("demo_pause");
                if(method!="get_state") commandError_.clear();
                error_=priorityError.empty()?commandError_:priorityError;
                Publish();
                if (result.is_null()) result=Snapshot();
                Resolve(request,{{"id",request->input.value("id",Json(nullptr))},{"result",std::move(result)}});
            } catch (const std::exception& exception) {
                commandError_=exception.what();
                error_=priorityError.empty()?commandError_:priorityError;
                Resolve(request,Error(request->input,exception.what()));
            }
        }
        if (playing_ && demo_) {
            // A seek is asynchronous. Keep the requested pose until the demo
            // approaches its target instead of sampling an unrelated old tick.
            if (syncDemo_ && waitingForSeek_ && std::abs(tick_-previewSeekTick_)<=8) waitingForSeek_=false;
            if (syncDemo_ && waitingForSeek_ && Clock::now()-previewStart_>std::chrono::seconds(10)) {
                waitingForSeek_=false; playing_=false; mode_="hold"; commandError_="demo seek did not reach the preview start";
                error_=priorityError.empty()?commandError_:priorityError;
            }
            if (syncDemo_ && !waitingForSeek_ && playing_) cursor_=std::clamp((tick_-project_.startTick)/tickRate_,0.0,project_.duration);
            else if (!syncDemo_) cursor_=std::clamp(previewStartCursor_+std::chrono::duration<double>(Clock::now()-previewStart_).count(),0.0,project_.duration);
            camera_=Sample(project_,cursor_);
            if (cursor_>=project_.duration) { playing_=false; mode_="hold"; if (syncDemo_) execute("demo_pause"); }
        }
        { std::lock_guard<std::mutex> lock(viewMutex_); output_=camera_; outputEnabled_=demo_ && mode_!="free" && cameraValid_; }
        Publish();
    }
    // No engine calls here: view construction may occur on another thread.
    bool View(float* origin,float* angles,float* fov,bool demo) {
        if (!enabled_ || !demo) return false;
        Pose original;
        for (int i=0;i<3;++i) { original.position[i]=origin[i]; original.angles[i]=angles[i]; }
        original.fov=*fov;
        try { Validate(original); } catch (const std::exception&) { return false; }
        std::lock_guard<std::mutex> lock(viewMutex_);
        observed_=original; observedValid_=true;
        if (!outputEnabled_) return false;
        for (int i=0;i<3;++i) { origin[i]=float(output_.position[i]); angles[i]=float(output_.angles[i]); }
        *fov=float(output_.fov); ++viewWrites_; return true;
    }

private:
    struct Request { Json input; std::promise<Json> reply; std::atomic<bool> cancelled{false}; };
    static Json Error(const Json& input,const std::string& message) {
        return {{"id",input.is_object()?input.value("id",Json(nullptr)):Json(nullptr)},{"error",{{"message",message}}}};
    }
    static void Resolve(const std::shared_ptr<Request>& request,Json value) { try {request->reply.set_value(std::move(value));} catch (const std::future_error&) {} }
    std::shared_ptr<Request> Queue(Json input) {
        if (!input.is_object() || !input.contains("method") || !input.at("method").is_string()) throw std::invalid_argument("request requires method");
        auto request=std::make_shared<Request>(); request->input=std::move(input);
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queue_.size()>=256) throw std::runtime_error("live command queue is full");
        queue_.push_back(request); return request;
    }
    Json Snapshot() const { std::lock_guard<std::mutex> lock(stateMutex_); return Json::parse(stateJson_); }
    void Publish() {
        Json state={{"abi",Abi},{"connected",enabled_},{"demo",demo_},{"tick",tick_},{"tick_rate",tickRate_},
                    {"paused",paused_},{"mode",mode_},{"camera",ToJson(camera_)},{"camera_valid",cameraValid_},
                    {"project",ToJson(project_)},{"cursor",cursor_},{"preview_playing",playing_},{"sync_demo",syncDemo_},
                    {"revision",revision_},{"view_hook_installed",hookInstalled_},{"view_writes",viewWrites_.load()},
                    {"undo_available",!undo_.empty()},{"redo_available",!redo_.empty()}};
        if (!error_.empty()) state["error"]=error_;
        std::lock_guard<std::mutex> lock(stateMutex_); stateJson_=state.dump();
    }
    void NeedDemo() const {
        if (!demo_) throw std::runtime_error("camera control requires active local demo playback");
        if (!hookInstalled_) throw std::runtime_error("validated camera hook unavailable: "+error_);
    }
    void NeedCamera() const { if (!cameraValid_) throw std::runtime_error("waiting for the first valid game camera sample"); }
    void Commit(Project value) {
        Normalize(value); undo_.push_back(project_); if (undo_.size()>64) undo_.pop_front();
        project_=std::move(value); redo_.clear(); playing_=false; if(mode_=="preview") mode_="hold";
        cursor_=std::min(cursor_,project_.duration); ++revision_;
    }
    std::string ProjectPath() const {
        const char* path=std::getenv("CS2_CAMERA_PROJECT");
        if(!path || !*path || path[0]!='/') throw std::runtime_error("CS2_CAMERA_PROJECT must specify an absolute project filename");
        return path;
    }
    Json SaveProject() {
        const std::string path=ProjectPath();
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        const std::string temporary=path+".tmp-"+std::to_string(getpid())+"-"+std::to_string(++fileId_);
        const std::string contents=ToJson(project_).dump(2)+"\n";
        int descriptor=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(descriptor<0) throw std::runtime_error("cannot create project save file");
        try {
            std::size_t written=0;
            while(written<contents.size()) {
                const auto count=write(descriptor,contents.data()+written,contents.size()-written);
                if(count<0 && errno==EINTR) continue;
                if(count<=0) throw std::runtime_error("cannot write project save file");
                written+=std::size_t(count);
            }
            if(fsync(descriptor)) throw std::runtime_error("cannot flush project save file");
            close(descriptor); descriptor=-1;
            if(rename(temporary.c_str(),path.c_str())) throw std::runtime_error("cannot replace saved project");
        } catch(...) {if(descriptor>=0)close(descriptor);unlink(temporary.c_str());throw;}
        return {{"saved",true},{"path",path}};
    }
    void LoadProject() {
        const std::string path=ProjectPath();
        int descriptor=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(descriptor<0) throw std::runtime_error("cannot open saved project");
        std::string contents;
        try {
            struct stat info{};
            if(fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_size<0 || info.st_size>4*ssize_t(MaxLine))
                throw std::runtime_error("saved project must be a regular JSON file no larger than 4 MiB");
            char bytes[8192];
            for(;;) {
                const auto count=read(descriptor,bytes,sizeof(bytes));
                if(count<0 && errno==EINTR) continue;
                if(count<0) throw std::runtime_error("cannot read saved project");
                if(!count) break;
                contents.append(bytes,std::size_t(count));
                if(contents.size()>4*MaxLine) throw std::runtime_error("saved project exceeds 4 MiB");
            }
            close(descriptor); descriptor=-1;
            Project next=ReadProject(Json::parse(contents));
            Commit(std::move(next));
        } catch(...) {if(descriptor>=0)close(descriptor);throw;}
    }
    std::string NewKeyId() {
        for (;;) {
            const std::string id="key-"+std::to_string(++keyId_);
            if (std::none_of(project_.keys.begin(),project_.keys.end(),[&](const Key& key){return key.id==id;})) return id;
        }
    }
    Json Command(const Json& request,const Execute& execute) {
        const std::string method=request.at("method").get<std::string>();
        Json params=request.value("params",Json::object());
        if (!params.is_object()) throw std::invalid_argument("params must be an object");
        if (method=="ping") return {{"pong",true},{"abi",Abi}};
        if (method=="get_state") return nullptr;
        if (method=="export_project") return ToJson(project_);
        if (method=="save_project") return SaveProject();
        if (method=="load_project") { LoadProject(); return nullptr; }
        if (method=="set_camera") {
            NeedDemo(); NeedCamera(); camera_=ReadPose(params.contains("camera")?params.at("camera"):params,camera_);
            mode_="hold"; playing_=false; execute("spec_autodirector 0"); ++revision_;
        } else if (method=="release_camera") {
            playing_=false; mode_="free"; ++revision_;
        } else if (method=="add_key") {
            NeedDemo(); NeedCamera(); Project next=project_;
            Key key; key.id=NewKeyId(); key.time=params.value("time",cursor_); key.pose=camera_;
            if(next.keys.empty()) next.startTick=std::max(0,tick_-int(std::llround(key.time*tickRate_)));
            Json value=params; value["id"]=key.id;
            key=ReadKey(value,key); next.duration=std::max(next.duration,key.time); next.keys.push_back(key); Commit(next);
            return ToJson(key);
        } else if (method=="update_key" || method=="remove_key") {
            Project next=project_; const std::string id=params.at("id").get<std::string>();
            auto key=std::find_if(next.keys.begin(),next.keys.end(),[&](const Key& candidate){return candidate.id==id;});
            if (key==next.keys.end()) throw std::invalid_argument("unknown camera key id");
            if (method=="remove_key") next.keys.erase(key); else *key=ReadKey(params,*key);
            Commit(next);
        } else if (method=="set_project" || method=="import_project") {
            Json value=params.contains("project")?params.at("project"):params;
            if (method=="set_project") { Json merged=ToJson(project_); merged.update(value); value=merged; }
            Commit(ReadProject(value));
        } else if (method=="scrub") {
            NeedDemo(); NeedCamera(); double time=params.at("time").get<double>(); Finite(time,0,project_.duration,"cursor");
            Pose next=Sample(project_,time); camera_=next; cursor_=time; playing_=false; mode_="hold";
            if (params.value("seek_demo",false)) { execute("demo_pause"); execute("demo_gototick "+std::to_string(project_.startTick+int(std::llround(time*tickRate_)))); }
            ++revision_;
        } else if (method=="preview") {
            NeedDemo(); NeedCamera(); bool playing=params.value("playing",true);
            const bool wasPlaying=playing_,oldSync=syncDemo_;
            const bool sync=params.value("sync_demo",syncDemo_);
            if (playing && project_.keys.empty()) throw std::invalid_argument("add a camera key before preview");
            syncDemo_=sync;
            if (playing) {
                if (cursor_>=project_.duration) cursor_=0;
                previewStartCursor_=cursor_; previewStart_=Clock::now(); playing_=true; mode_="preview";
                camera_=Sample(project_,cursor_); execute("spec_autodirector 0");
                waitingForSeek_=syncDemo_;
                if (syncDemo_) { previewSeekTick_=project_.startTick+int(std::llround(cursor_*tickRate_)); execute("demo_gototick "+std::to_string(previewSeekTick_)); execute("demo_resume"); }
                else execute("demo_pause");
            } else {
                playing_=false;
                if(wasPlaying) mode_="hold";
                if(wasPlaying && oldSync) execute("demo_pause");
            }
            ++revision_;
        } else if (method=="demo_control") {
            NeedDemo(); const std::string action=params.at("action").get<std::string>();
            if(action=="pause" || action=="resume") execute(action=="pause"?"demo_pause":"demo_resume");
            else if (action=="seek") { int target=params.at("tick").get<int>(); if(target<0 || target>2000000000) throw std::invalid_argument("invalid demo tick"); execute("demo_gototick "+std::to_string(target)); }
            else throw std::invalid_argument("demo action must be pause, resume, or seek");
        } else if (method=="apply_preset") {
            NeedDemo(); NeedCamera(); Project next=project_; next.keys.clear(); next.startTick=std::max(0,tick_);
            const auto name=params.at("name").get<std::string>(); next.duration=params.value("duration",5.0); Finite(next.duration,.01,86400,"duration");
            const double distance=params.value("distance",200.0); Finite(distance,1,10000,"distance");
            auto add=[&](double time,const Pose& pose) { Key key; key.id=NewKeyId(); key.time=time; key.pose=pose; next.keys.push_back(key); };
            Pose end=camera_;
            if (name=="hold") { add(0,camera_); add(next.duration,camera_); }
            else if(name=="dolly") {
                const double pitch=camera_.angles[0]*3.141592653589793/180,yaw=camera_.angles[1]*3.141592653589793/180;
                end.position[0]+=distance*std::cos(pitch)*std::cos(yaw); end.position[1]+=distance*std::cos(pitch)*std::sin(yaw); end.position[2]-=distance*std::sin(pitch);
                add(0,camera_); add(next.duration,end);
            } else if(name=="orbit") {
                // A 90 degree orbit looking at a point in front of the current camera.
                const double yaw=camera_.angles[1]*3.141592653589793/180;
                const double centerX=camera_.position[0]+distance*std::cos(yaw),centerY=camera_.position[1]+distance*std::sin(yaw);
                for(int i=0;i<=4;++i) { Pose pose=camera_; double turn=i*3.141592653589793/8; pose.position[0]=centerX-distance*std::cos(yaw+turn); pose.position[1]=centerY-distance*std::sin(yaw+turn); pose.angles[1]+=i*22.5; add(next.duration*i/4,pose); }
            } else throw std::invalid_argument("preset must be hold, dolly, or orbit");
            next.name=name+" cinematic"; Commit(next); cursor_=0;
        } else if(method=="undo" || method=="redo") {
            auto& source=method=="undo"?undo_:redo_; auto& target=method=="undo"?redo_:undo_;
            if(source.empty()) throw std::invalid_argument(method+" history is empty");
            target.push_back(project_); project_=source.back(); source.pop_back(); playing_=false; if(mode_=="preview") mode_="hold"; cursor_=std::min(cursor_,project_.duration); ++revision_;
        } else throw std::invalid_argument("unknown live method: "+method);
        return nullptr;
    }
    void Serve() {
        while(running_) {
            pollfd listener{listener_,POLLIN,0}; if(poll(&listener,1,100)<=0) continue;
            int client=accept4(listener_,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK); if(client<0) continue;
            // Same-user access is also checked after accept, independent of file mode.
            ucred credentials{}; socklen_t length=sizeof(credentials);
            if(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&credentials,&length) || credentials.uid!=getuid()) {close(client);continue;}
            Client(client); close(client);
        }
    }
    void Client(int client) {
        std::string buffer; auto deadline=Clock::now()+std::chrono::seconds(3);
        while(running_ && Clock::now()<deadline) {
            pollfd descriptor{client,POLLIN,0}; if(poll(&descriptor,1,100)<=0) continue;
            char bytes[8192]; const auto count=recv(client,bytes,sizeof(bytes),0);
            if(count<=0) return;
            buffer.append(bytes,std::size_t(count));
            if(buffer.size()>MaxLine) { Send(client,Error(Json(),"request exceeds 1 MiB")); return; }
            auto newline=buffer.find('\n'); if(newline==std::string::npos) continue;
            Json input;
            try {
                input=Json::parse(buffer.substr(0,newline)); Json response;
                if(input.value("method",std::string())=="get_state") response={{"id",input.value("id",Json(nullptr))},{"result",Snapshot()}};
                else if(input.value("method",std::string())=="ping") response={{"id",input.value("id",Json(nullptr))},{"result",{{"pong",true},{"abi",Abi}}}};
                else {
                    auto request=Queue(input); auto future=request->reply.get_future();
                    const auto expires=Clock::now()+std::chrono::seconds(2);
                    while(running_ && Clock::now()<expires && future.wait_for(std::chrono::milliseconds(30))!=std::future_status::ready) {}
                    if(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) { request->cancelled=true; throw std::runtime_error("game thread did not service command within 2 seconds"); }
                    response=future.get();
                }
                Send(client,response);
            } catch(const std::exception& exception) { Send(client,Error(input,exception.what())); }
            return; // one request per connection keeps independent clients bounded
        }
    }
    static void Send(int client,const Json& value) {
        const std::string output=value.dump()+"\n"; std::size_t sent=0; auto deadline=Clock::now()+std::chrono::seconds(1);
        while(sent<output.size() && Clock::now()<deadline) {
            auto count=send(client,output.data()+sent,output.size()-sent,MSG_NOSIGNAL);
            if(count>0) sent+=std::size_t(count);
            else if(errno==EAGAIN || errno==EINTR) { pollfd ready{client,POLLOUT,0}; poll(&ready,1,20); }
            else break;
        }
    }
    bool enabled_=false,ownsSocket_=false;
    std::atomic<bool> running_{false}; int listener_=-1; std::thread worker_;
    std::string socketPath_,startupError_;
    mutable std::mutex stateMutex_; std::string stateJson_;
    std::mutex queueMutex_; std::deque<std::shared_ptr<Request>> queue_;
    std::mutex viewMutex_; Pose observed_,output_; bool observedValid_=false,outputEnabled_=false;
    std::atomic<std::uint64_t> viewWrites_{0};
    bool demo_=false,paused_=false,hookInstalled_=false,cameraValid_=false,playing_=false,syncDemo_=true,waitingForSeek_=false;
    int tick_=0,previewSeekTick_=0; double tickRate_=64,cursor_=0,previewStartCursor_=0;
    Clock::time_point previewStart_=Clock::now();
    Pose camera_; Project project_; std::string mode_="free",error_,commandError_;
    std::uint64_t revision_=0,keyId_=0,fileId_=0;
    std::deque<Project> undo_,redo_;
};
} // namespace cs2_live
