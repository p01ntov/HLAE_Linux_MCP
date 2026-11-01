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
#include <unordered_set>
#include <vector>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "scene_environment.h"
#include "demo_refresh.h"
#include "player_cosmetics.h"
#include "presentation.h"
#include "live_telemetry.h"
#include "demo_timeline.h"
#include "demo_playback_range.h"
#include "project_store.h"
#include "map_preview.h"
#include "editor_preferences.h"
#include "lens_project.h"

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
    // Cubic value controls are offsets from this key, in world units/degrees.
    // An absent handle resolves to the shared time-weighted automatic tangent.
    std::optional<std::array<double,3>> positionIn,positionOut,rotationIn,rotationOut;
    std::optional<double> fovIn,fovOut;
    std::optional<std::array<double,4>> positionEasing,rotationEasing,fovEasing;
};
struct RenderRange {
    std::string mode="composition";
    double in=0,out=5;
};
struct Project {
    // Session identity follows ordinary edits and undo/redo, but is never
    // serialized into portable projects. Import/load starts a new document.
    std::uint64_t documentId=0;
    std::string name="Untitled cinematic";
    int startTick=0;
    double duration=5;
    std::string interpolation="smooth";
    std::string rotationInterpolation="slerp",fovInterpolation="linear";
    std::vector<Key> keys;
    // Transient authoring cache, rebuilt by Normalize after every edit.
    mutable std::vector<std::array<double,3>> unwrappedRotations;
    std::optional<RenderRange> renderRange;
    Json scene=Json::object();
    Json cosmetics=Json::object();
    Json presentation=Json::object();
    Json lens=Json::object();
};
inline RenderRange StoredRenderRange(const Project& project) {
    return project.renderRange.value_or(RenderRange{"composition",0,project.duration});
}
inline RenderRange PlaybackRange(const Project& project) {
    auto range=StoredRenderRange(project);
    if(range.mode=="composition") {range.in=0;range.out=project.duration;}
    return range;
}
inline void ValidateRenderRange(const RenderRange& range,double duration) {
    if(range.mode!="composition" && range.mode!="region") throw std::invalid_argument("render range mode must be composition or region");
    Finite(range.in,0,duration,"render range In");
    Finite(range.out,0,duration,"render range Out");
    if(range.in>=range.out) throw std::invalid_argument("render range requires In < Out");
}
inline RenderRange ReadRenderRange(const Json& value,double duration) {
    if(!value.is_object()) throw std::invalid_argument("render_range must be an object");
    RenderRange range{"composition",0,duration};
    range.mode=value.value("mode",range.mode);
    range.in=value.value("in",range.in);
    range.out=value.value("out",range.out);
    ValidateRenderRange(range,duration); return range;
}
inline void ResizeRenderRange(Project& project,double previousDuration) {
    auto range=project.renderRange.value_or(RenderRange{"composition",0,previousDuration});
    if(std::abs(range.out-previousDuration)<1e-9) range.out=project.duration;
    const double span=std::min(.01,project.duration);
    range.in=std::clamp(range.in,0.0,project.duration-span);
    range.out=std::clamp(range.out,range.in+span,project.duration);
    project.renderRange=range;
}
inline Json ToJson(const Key& key) {
    Json result=ToJson(key.pose);
    result["id"]=key.id; result["time"]=key.time; result["easing"]=key.easing;
    if(key.positionIn)result["position_in"]=*key.positionIn;
    if(key.positionOut)result["position_out"]=*key.positionOut;
    if(key.rotationIn)result["rotation_in"]=*key.rotationIn;
    if(key.rotationOut)result["rotation_out"]=*key.rotationOut;
    if(key.fovIn)result["fov_in"]=*key.fovIn;
    if(key.fovOut)result["fov_out"]=*key.fovOut;
    if(key.positionEasing)result["position_easing"]=*key.positionEasing;
    if(key.rotationEasing)result["rotation_easing"]=*key.rotationEasing;
    if(key.fovEasing)result["fov_easing"]=*key.fovEasing;
    return result;
}
inline Json ToJson(const Project& project) {
    Json keys=Json::array();
    for (const auto& key:project.keys) keys.push_back(ToJson(key));
    const auto range=StoredRenderRange(project);
    Json result={{"name",project.name},{"start_tick",project.startTick},{"duration",project.duration},
            {"interpolation",project.interpolation},{"keys",keys},
            {"render_range",{{"mode",range.mode},{"in",range.in},{"out",range.out}}},{"scene",project.scene},{"cosmetics",project.cosmetics},{"presentation",project.presentation}};
    if(!project.lens.empty())result["lens"]=project.lens;
    if(project.rotationInterpolation!="slerp")result["rotation_interpolation"]=project.rotationInterpolation;
    if(project.fovInterpolation!="linear")result["fov_interpolation"]=project.fovInterpolation;
    return result;
}
inline void ValidateKeyCurves(const Key& key) {
    for(const auto* value:{&key.positionIn,&key.positionOut,&key.rotationIn,&key.rotationOut})
        if(*value)for(const auto number:**value)Finite(number,-1000000,1000000,"relative camera curve handle");
    for(const auto* value:{&key.fovIn,&key.fovOut})if(*value)Finite(**value,-1000000,1000000,"relative FOV curve handle");
    for(const auto* value:{&key.positionEasing,&key.rotationEasing,&key.fovEasing})
        if(*value)for(const auto number:**value)Finite(number,0,1,"channel easing coordinate");
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
    auto triple=[&](const char* name,std::optional<std::array<double,3>>& target) {
        if(!value.contains(name))return;
        if(value[name].is_null())target.reset();else target=Triple(value[name],name);
    };
    auto scalar=[&](const char* name,std::optional<double>& target) {
        if(!value.contains(name))return;
        if(value[name].is_null())target.reset();else target=value[name].get<double>();
    };
    auto easing=[&](const char* name,std::optional<std::array<double,4>>& target) {
        if(!value.contains(name))return;
        if(value[name].is_null()){target.reset();return;}
        if(!value[name].is_array() || value[name].size()!=4)throw std::invalid_argument(std::string(name)+" requires [x1,y1,x2,y2]");
        std::array<double,4> numbers{};for(int i=0;i<4;++i)numbers[i]=value[name][i].get<double>();target=numbers;
    };
    triple("position_in",key.positionIn);triple("position_out",key.positionOut);
    triple("rotation_in",key.rotationIn);triple("rotation_out",key.rotationOut);
    scalar("fov_in",key.fovIn);scalar("fov_out",key.fovOut);
    easing("position_easing",key.positionEasing);easing("rotation_easing",key.rotationEasing);easing("fov_easing",key.fovEasing);
    ValidateKeyCurves(key);
    return key;
}
inline void Normalize(Project& project) {
    if (project.name.size()>256) throw std::invalid_argument("project name is too long");
    Finite(project.duration,0.01,86400,"duration");
    if(!project.renderRange) project.renderRange=RenderRange{"composition",0,project.duration};
    ValidateRenderRange(*project.renderRange,project.duration);
    ValidateSceneSettings(project.scene);
    ValidateSupportedCosmetics(project.cosmetics);
    ValidatePresentation(project.presentation);
    {const auto validated=NormalizeLens(project.lens);if(!project.lens.empty())project.lens=validated;}
    if (project.startTick<0 || project.startTick>2000000000) throw std::invalid_argument("invalid start_tick");
    if (project.interpolation!="linear" && project.interpolation!="smooth" && project.interpolation!="bezier") throw std::invalid_argument("interpolation must be linear, smooth or bezier");
    if(project.rotationInterpolation!="slerp" && project.rotationInterpolation!="bezier")throw std::invalid_argument("rotation_interpolation must be slerp or bezier");
    if(project.fovInterpolation!="linear" && project.fovInterpolation!="bezier")throw std::invalid_argument("fov_interpolation must be linear or bezier");
    if (project.keys.size()>4096) throw std::invalid_argument("project supports at most 4096 keys");
    std::sort(project.keys.begin(),project.keys.end(),[](const Key& a,const Key& b){return a.time<b.time;});
    std::unordered_set<std::string> ids;ids.reserve(project.keys.size());
    for (std::size_t i=0;i<project.keys.size();++i) {
        Validate(project.keys[i].pose);
        ValidateKeyCurves(project.keys[i]);
        if (project.keys[i].time>project.duration) throw std::invalid_argument("key exceeds project duration");
        if (i && project.keys[i].time-project.keys[i-1].time<0.000001) throw std::invalid_argument("keys need distinct times");
        if(!ids.insert(project.keys[i].id).second) throw std::invalid_argument("duplicate key id");
    }
    project.unwrappedRotations.clear();project.unwrappedRotations.reserve(project.keys.size());
    for(const auto& key:project.keys) {
        auto angles=key.pose.angles;
        if(!project.unwrappedRotations.empty())for(int axis=0;axis<3;++axis)
            angles[axis]=project.unwrappedRotations.back()[axis]+std::remainder(angles[axis]-project.unwrappedRotations.back()[axis],360.0);
        project.unwrappedRotations.push_back(angles);
    }
    if (ToJson(project).dump().size()>MaxLine-16384) throw std::invalid_argument("project JSON exceeds the local transport limit");
}
inline Project ReadProject(const Json& value) {
    for(const char* key:{"appearance","appearance_pack"})if(value.contains(key))
        throw std::invalid_argument("global appearance packs are disabled in this build");
    if (!value.is_object()) throw std::invalid_argument("project must be an object");
    Project project;
    project.name=value.value("name",project.name);
    project.startTick=value.value("start_tick",0);
    project.duration=value.value("duration",5.0);
    project.interpolation=value.value("interpolation",std::string("smooth"));
    project.rotationInterpolation=value.value("rotation_interpolation",std::string("slerp"));
    project.fovInterpolation=value.value("fov_interpolation",std::string("linear"));
    if(value.contains("render_range")) project.renderRange=ReadRenderRange(value.at("render_range"),project.duration);
    if(value.contains("scene")) project.scene=value.at("scene");
    if(value.contains("cosmetics")) project.cosmetics=value.at("cosmetics");
    if(value.contains("presentation")) project.presentation=value.at("presentation");
    if(value.contains("lens"))project.lens=value.at("lens");
    if (value.contains("keys")) {
        if (!value.at("keys").is_array()) throw std::invalid_argument("keys must be an array");
        for (const auto& key:value.at("keys")) project.keys.push_back(ReadKey(key));
    }
    Normalize(project); return project;
}
inline Json EffectiveSceneSettings(const Project& project) {
    auto scene=project.scene;
    if(project.presentation.contains("hud"))scene["hud_visible"]=project.presentation["hud"]!="hidden";
    if(project.presentation.value("hud",std::string())=="gameplay")scene.update(GameplayHudScenePreset());
    return scene;
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
inline double CubicValue(double a,double b,double c,double d,double u) {
    const double v=1-u;return v*v*v*a+3*v*v*u*b+3*v*u*u*c+u*u*u*d;
}
template<std::size_t N,class Value>
inline std::array<std::array<double,N>,4> AutomaticControls(const Project& project,std::size_t left,Value value,bool linear=false) {
    if(left+1>=project.keys.size())throw std::invalid_argument("curve controls require an existing outgoing segment");
    const auto right=left+1,previous=left?left-1:left,after=std::min(right+1,project.keys.size()-1);
    const auto a=value(left),b=value(right),p=value(previous),n=value(after);
    const double span=project.keys[right].time-project.keys[left].time;
    if(span<=0)throw std::invalid_argument("curve keys require increasing times");
    std::array<std::array<double,N>,4> result{};result[0]=a;result[3]=b;
    for(std::size_t axis=0;axis<N;++axis) {
        const double tangentA=linear?b[axis]-a[axis]:(b[axis]-p[axis])/(project.keys[right].time-project.keys[previous].time)*span;
        const double tangentB=linear?b[axis]-a[axis]:(n[axis]-a[axis])/(project.keys[after].time-project.keys[left].time)*span;
        result[1][axis]=a[axis]+tangentA/3;result[2][axis]=b[axis]-tangentB/3;
    }
    return result;
}
inline std::array<std::array<double,3>,4> PositionControls(const Project& project,std::size_t left) {
    const bool linear=project.interpolation=="linear" || (project.interpolation=="smooth" && project.keys.size()<3);
    auto result=AutomaticControls<3>(project,left,[&](std::size_t i){return project.keys[i].pose.position;},linear);
    if(project.interpolation=="bezier")for(int axis=0;axis<3;++axis) {
        if(project.keys[left].positionOut)result[1][axis]=result[0][axis]+(*project.keys[left].positionOut)[axis];
        if(project.keys[left+1].positionIn)result[2][axis]=result[3][axis]+(*project.keys[left+1].positionIn)[axis];
    }
    return result;
}
inline const std::vector<std::array<double,3>>& UnwrappedRotations(const Project& project) {
    if(project.unwrappedRotations.size()==project.keys.size())return project.unwrappedRotations;
    auto& values=project.unwrappedRotations;values.clear();values.reserve(project.keys.size());
    for(const auto& key:project.keys) {
        auto value=key.pose.angles;
        if(!values.empty())for(int axis=0;axis<3;++axis)value[axis]=values.back()[axis]+AngleDelta(values.back()[axis],value[axis]);
        values.push_back(value);
    }
    return values;
}
inline std::array<std::array<double,3>,4> RotationControls(const Project& project,std::size_t left) {
    // In legacy SLERP mode these are editable Euler anchors, not an exact
    // cubic representation of a quaternion geodesic. Sample remains SLERP.
    const auto& values=UnwrappedRotations(project);
    auto result=AutomaticControls<3>(project,left,[&](std::size_t i){return values[i];},project.rotationInterpolation!="bezier");
    if(project.rotationInterpolation=="bezier")for(int axis=0;axis<3;++axis) {
        if(project.keys[left].rotationOut)result[1][axis]=result[0][axis]+(*project.keys[left].rotationOut)[axis];
        if(project.keys[left+1].rotationIn)result[2][axis]=result[3][axis]+(*project.keys[left+1].rotationIn)[axis];
    }
    return result;
}
inline std::array<double,4> FovControls(const Project& project,std::size_t left) {
    auto values=AutomaticControls<1>(project,left,[&](std::size_t i){return std::array<double,1>{project.keys[i].pose.fov};},project.fovInterpolation!="bezier");
    std::array<double,4> result{values[0][0],values[1][0],values[2][0],values[3][0]};
    if(project.fovInterpolation=="bezier") {
        if(project.keys[left].fovOut)result[1]=result[0]+*project.keys[left].fovOut;
        if(project.keys[left+1].fovIn)result[2]=result[3]+*project.keys[left+1].fovIn;
    }
    return result;
}
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
    if (time>=keys.back().time) {
        auto pose=keys.back().pose;
        if(project.rotationInterpolation=="bezier")pose.angles=UnwrappedRotations(project).back();
        return pose;
    }
    auto next=std::upper_bound(keys.begin(),keys.end(),time,[](double t,const Key& key){return t<key.time;});
    const std::size_t right=std::size_t(next-keys.begin()), left=right-1;
    const auto& a=keys[left]; const auto& b=keys[right];
    const double span=b.time-a.time,progress=(time-a.time)/span;
    const double u=Ease(progress,a.positionEasing.value_or(a.easing));
    Pose result;
    std::array<std::array<double,3>,4> positionControls{};
    if(project.interpolation=="bezier")positionControls=PositionControls(project,left);
    for (int axis=0;axis<3;++axis) {
        if(project.interpolation=="bezier") {
            result.position[axis]=CubicValue(positionControls[0][axis],positionControls[1][axis],positionControls[2][axis],positionControls[3][axis],u);
        } else if (project.interpolation=="linear" || keys.size()<3) result.position[axis]=a.pose.position[axis]+u*(b.pose.position[axis]-a.pose.position[axis]);
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
    const double fovU=Ease(progress,a.fovEasing.value_or(a.easing)),rotationU=Ease(progress,a.rotationEasing.value_or(a.easing));
    if(project.fovInterpolation=="bezier") {
        const auto controls=FovControls(project,left);
        result.fov=std::clamp(CubicValue(controls[0],controls[1],controls[2],controls[3],fovU),1.0,179.0);
    } else result.fov=a.pose.fov+fovU*(b.pose.fov-a.pose.fov);
    if(project.rotationInterpolation=="bezier") {
        const auto controls=RotationControls(project,left);
        for(int axis=0;axis<3;++axis)result.angles[axis]=CubicValue(controls[0][axis],controls[1][axis],controls[2][axis],controls[3][axis],rotationU);
        result.angles[0]=std::clamp(result.angles[0],-90.0,90.0);
        return result;
    }
    result.angles=Euler(Slerp(Rotation(a.pose.angles),Rotation(b.pose.angles),rotationU));
    // Equivalent yaw/roll representations should remain close to the authored
    // key values, including crossings of +/-180 degrees.
    for(int axis=1;axis<3;++axis) {
        const double reference=a.pose.angles[axis]+rotationU*AngleDelta(a.pose.angles[axis],b.pose.angles[axis]);
        result.angles[axis]=reference+AngleDelta(reference,result.angles[axis]);
    }
    return result;
}

class Controller {
public:
    using Execute=std::function<void(const std::string&)>;
    explicit Controller(LiveTelemetryCadence::Now now={})
        :cosmeticsCadence_(std::chrono::milliseconds(100),LiveTelemetryCadence::LiveOnly(),now),
         publicationCadence_(std::chrono::milliseconds(34),LiveTelemetryCadence::LiveOnly(),now),
         demoSeekNow_(now),presentation_(std::move(now)) {
        const char* enabled=std::getenv("CS2_CAMERA_LIVE"); enabled_=enabled && std::strcmp(enabled,"1")==0; Publish();
    }
    ~Controller() { Stop(); }
    bool Enabled() const { return enabled_; }
    void Start() {
        if (!enabled_ || running_.exchange(true)) return;
        try {editorPreferences_=LoadEditorPreferencesFile(std::getenv("CS2_CAMERA_EDITOR_PREFS"));editorPreferencesError_.clear();}
        catch(const std::exception& error){editorPreferences_=EditorPreferenceDefaults();editorPreferencesError_=error.what();}
        demoTimeline_.LoadFile(std::getenv("CS2_CAMERA_DEMO_TIMELINE"),std::getenv("CS2_CAMERA_DEMO_PATH"),std::getenv("CS2_CAMERA_DEMO_TIMELINE_ERROR"));
        demoPlaybackRange_.Bind(demoTimeline_.PlaybackTicks(),demoTimeline_.RecordingRevision());
        Publish();
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
    std::string UiStateJson() const { std::lock_guard<std::mutex> lock(stateMutex_); return uiStateJson_; }
    std::string MapPreviewJson() const { std::lock_guard<std::mutex> lock(stateMutex_); return mapPreviewJson_; }
    void SetSceneBackend(SceneEnvironment::Read read,SceneEnvironment::Write write) {scene_.Bind(std::move(read),std::move(write));}
    void SetSceneRegistry(Json registry) {sceneRegistry_=std::move(registry);}
    void SetEntityInspector(std::function<Json(unsigned)> inspect) {entityInspector_=std::move(inspect);}
    void SetCosmeticsBackend(PlayerCosmetics::Callbacks callbacks) {cosmetics_.Bind(std::move(callbacks));}
    void SetPresentationBackend(Presentation::Callbacks callbacks) {presentation_.Bind(std::move(callbacks));}
    void RestorePresentation() {presentation_.Suspend();}
    void RestoreCosmetics() {cosmetics_.Refresh(false);}
    void RestoreScene() {scene_.Apply(Json::object());}
    bool CanRefreshAppearance() const {
        const char* path=std::getenv("CS2_CAMERA_DEMO_PATH");if(!path)return false;
        try{DemoRefresh::CheckPath(path);return true;}catch(...){return false;}
    }
    bool RequestAppearanceRefresh() {
        if(demoRefresh_.Active() || !currentExecute_ || !CanRefreshAppearance())return false;
        if(!appearanceTransition_) {
            appearanceSavedCamera_=camera_;appearanceSavedCursor_=cursor_;
            appearanceSavedTick_=tick_;appearanceSavedPaused_=paused_;
        }
        mode_="hold";playing_=false;appearanceTransition_=true;
        try {
            const auto result=demoRefresh_.Start(std::getenv("CS2_CAMERA_DEMO_PATH"),appearanceSavedTick_,appearanceSavedPaused_,currentExecute_);
            if(result==DemoRefresh::Result::Failed){commandError_=demoRefresh_.Error();appearanceTransition_=false;return false;}
            return true;
        }catch(const std::exception& error){commandError_=error.what();appearanceTransition_=false;return false;}
    }
    void EnqueueText(const char* text) {
        if (!enabled_ || !text) return;
        if (strnlen(text,MaxLine+1)>MaxLine) return;
        try { Queue(Json::parse(text)); } catch (const std::exception&) {}
    }
    // Render-thread capability publication is independent of game-thread state.
    // No game pointers or project edits cross this ABI callback.
    void LensStatusText(const char* text) {
        if(!text || strnlen(text,2049)>2048)return;
        try {
            const auto value=Json::parse(text);
            if(!value.is_object() || !value.contains("available") || !value["available"].is_boolean() ||
               value.value("backend",std::string())!="vulkan-color" || !value.contains("depth_available") ||
               !value["depth_available"].is_boolean() || value["depth_available"].get<bool>() ||
               !value.contains("reason") || !value["reason"].is_string() || value["reason"].get<std::string>().size()>1024)return;
            const Json normalized={{"available",value["available"]},{"backend","vulkan-color"},
                                   {"depth_available",false},{"reason",value["reason"]}};
            std::lock_guard<std::mutex> lock(lensMutex_);lensRuntime_=normalized;
        }catch(const std::exception&){}
    }
    // Called once per FRAME_START, before all playback-related early returns.
    void Frame(bool demo,int tick,bool paused,double tickRate,bool hookInstalled,const std::string& hookError,const Execute& execute) {
        if (!enabled_) return;
        const bool previewWasPlaying=playing_;
        const bool cameraWasValid=cameraValid_;
        if(paused!=paused_ || hookInstalled!=hookInstalled_)publicationCadence_.Reset();
        const bool lifetimeChanged=demo!=demo_ || (demo && (tick<tick_ || tick-tick_>int(std::max(128.0,tickRate*2))));
        if(lifetimeChanged){cosmeticsCadence_.Reset();publicationCadence_.Reset();presentation_.InvalidateTarget();}
        if(demo!=demo_ || (demo && (tick<tick_ || tick-tick_>int(std::max(128.0,tickRate*2))))) {
            ++entityEpoch_;if(entityProbe_.is_object())entityProbe_["stale"]=true;
        }
        demo_=demo; tick_=tick; paused_=paused;if(pauseIntent_ && *pauseIntent_==paused_)pauseIntent_.reset(); tickRate_=tickRate>0?tickRate:64;
        hookInstalled_=hookInstalled;
        bool seekChanged=false;
        try {seekChanged=AdvanceDemoSeek(execute);}
        catch(const std::exception& exception) {CancelDemoSeek();commandError_=exception.what();seekChanged=true;}
        currentExecute_=execute;
        if(demoRefresh_.Active()) {
            bool viewReady=false;
            {std::lock_guard<std::mutex> lock(viewMutex_);
             if(!demo_ && std::strcmp(demoRefresh_.Phase(),"waiting_unload")==0)observedValid_=false;
             viewReady=observedValid_;}
            const auto result=demoRefresh_.Advance(demo_,tick_,viewReady,execute);
            if(result==DemoRefresh::Result::Complete) {
                camera_=appearanceSavedCamera_;cursor_=appearanceSavedCursor_;
                mode_="hold";playing_=false;cameraValid_=true;waitingForSeek_=false;
                appearanceTransition_=false;commandError_.clear();
            } else if(result==DemoRefresh::Result::Failed){commandError_="appearance refresh: "+demoRefresh_.Error();appearanceTransition_=false;}
        }
        scene_.Refresh();
        presentation_.SetSceneCapabilities(scene_.Capabilities());
        const std::string priorityError=!startupError_.empty()?startupError_:hookError;
        error_=priorityError.empty()?commandError_:priorityError;
        { std::lock_guard<std::mutex> lock(viewMutex_); if (observedValid_) { cameraValid_=true; if (mode_=="free") camera_=observed_; } }
        if (!demo_ && !appearanceTransition_) {
            try {scene_.Apply(Json::object());}catch(const std::exception& exception){commandError_=exception.what();}
            playing_=false; mode_="free"; cameraValid_=false;
            std::lock_guard<std::mutex> lock(viewMutex_); observedValid_=false;
        }
        if(demo_) {
            try {scene_.Apply(EffectiveSceneSettings(project_));}catch(const std::exception& exception){commandError_=exception.what();}
        }
        {const auto capabilities=scene_.Capabilities();bool visible=true;
         presentation_.SetSceneCapabilities(capabilities);
         if(capabilities.contains("hud_visible") && capabilities["hud_visible"].contains("value") && capabilities["hud_visible"]["value"].is_boolean())visible=capabilities["hud_visible"]["value"].get<bool>();
         presentation_.Frame(demo_,visible);}
        bool cosmeticsRefreshed=false;
        const auto refreshCosmetics=[&]{
        cosmeticsRefreshed=true;
        cosmeticsSampleTick_=tick_;
        try {
            cosmetics_.Refresh(demo_);
            const auto generation=cosmetics_.State().value("runtime_generation",std::uint64_t(0));
            if(generation!=cosmeticsGeneration_){cosmeticsGeneration_=generation;++entityEpoch_;if(entityProbe_.is_object())entityProbe_["stale"]=true;}
            if(demo_ && cosmetics_.State().value("needs_demo_refresh",false) && !appearanceTransition_ && !demoRefresh_.Active() && cosmeticsRefreshRevision_!=revision_) {
                cosmeticsRefreshRevision_=revision_;cosmetics_.Refresh(false);
                if(!RequestAppearanceRefresh())commandError_="player changed team with an owned model; canonical demo refresh failed or is unavailable";
            }
        }catch(const std::exception& exception){commandError_=exception.what();}
        };
        if(cosmeticsCadence_.Due(cosmeticsBusy_ || PlayerCosmeticsRequested(project_.cosmetics)))refreshCosmetics();
        error_=priorityError.empty()?commandError_:priorityError;
        // Capturing additional keys while freely navigating should use the
        // visible demo time. A later capture can extend the initial duration.
        if (demo_ && mode_=="free" && !paused_ && !project_.keys.empty())
            cursor_=std::clamp((tick_-project_.startTick)/tickRate_,0.0,86400.0);
        std::deque<std::shared_ptr<Request>> ready;
        bool commandProcessed=false;
        { std::lock_guard<std::mutex> lock(queueMutex_);
          for (int i=0;i<64 && !queue_.empty();++i) { ready.push_back(queue_.front()); queue_.pop_front(); } }
        for (const auto& request:ready) {
            const auto cancellation=QueuedCancellation(*request);
            if(!cancellation.empty()){CancelQueued(request,cancellation);continue;}
            unsigned queued=0;if(!request->phase.compare_exchange_strong(queued,1))continue;
            // Recheck after claiming the command, in case this thread was
            // descheduled between the deadline test and the CAS.
            const auto claimedCancellation=QueuedCancellation(*request);
            if(!claimedCancellation.empty()){Resolve(request,Error(request->input,claimedCancellation));continue;}
            commandProcessed=true;
            try {
                if(!cosmeticsRefreshed && request->input.at("method")!="get_state") {
                    cosmeticsCadence_.Due(true);refreshCosmetics();
                }
                const bool wasSyncPlaying=playing_ && syncDemo_;
                Json result=Command(request->input,execute);
                RecordImportReceipt(request->input,true,"");
                const auto method=request->input.at("method").get<std::string>();
                if (wasSyncPlaying && !playing_ && method!="preview" && method!="release_camera" && method!="demo_control" && method!="request_safe_exit") {
                    SetDemoSeekPauseIntent(true);execute("demo_pause");
                }
                if(method!="get_state") commandError_.clear();
                error_=priorityError.empty()?commandError_:priorityError;
                Publish();
                if (result.is_null()) result=Snapshot();
                Resolve(request,{{"id",request->input.value("id",Json(nullptr))},{"result",std::move(result)}});
            } catch (const std::exception& exception) {
                RecordImportReceipt(request->input,false,exception.what());
                commandError_=exception.what();
                error_=priorityError.empty()?commandError_:priorityError;
                Publish();
                Resolve(request,Error(request->input,exception.what()));
            }
        }
        if (playing_ && demo_) {
            // A seek is asynchronous. Keep the requested pose until the demo
            // approaches its target instead of sampling an unrelated old tick.
            if (syncDemo_ && waitingForSeek_ && std::abs(tick_-previewSeekTick_)<=8) waitingForSeek_=false;
            if (syncDemo_ && waitingForSeek_ && Clock::now()-previewStart_>std::chrono::seconds(10)) {
                waitingForSeek_=false; playing_=false; mode_="hold";SetDemoSeekPauseIntent(true);execute("demo_pause");
                commandError_="demo seek did not reach the preview start";
                error_=priorityError.empty()?commandError_:priorityError;
            }
            const auto range=PlaybackRange(project_);
            if (syncDemo_ && !waitingForSeek_ && playing_) cursor_=std::clamp((tick_-project_.startTick)/tickRate_,range.in,range.out);
            else if (!syncDemo_) cursor_=std::clamp(previewStartCursor_+std::chrono::duration<double>(Clock::now()-previewStart_).count(),range.in,range.out);
            camera_=Sample(project_,cursor_);
            if (playing_ && cursor_>=range.out) { playing_=false; mode_="hold"; if (syncDemo_) {SetDemoSeekPauseIntent(true);execute("demo_pause");} }
        }
        // Full-demo limits do not constrain camera preview or paused scrubbing.
        // Seek fast-forward reports transient ticks outside the chosen range.
        if(demo_ && demoPlaybackRange_.Enabled() && !playing_ && !paused_ &&
           !demoSeekIssued_ && !demoRefresh_.Active() && !appearanceTransition_ &&
           !demoPlaybackRange_.Contains(tick_) && !pauseIntent_.value_or(false)) {
            SetDemoSeekPauseIntent(true);execute("demo_pause");
        }
        { std::lock_guard<std::mutex> lock(viewMutex_); output_=camera_; outputEnabled_=(demo_ || appearanceTransition_) && mode_!="free" && cameraValid_; }
        if(commandProcessed || seekChanged || playing_ || previewWasPlaying || cameraWasValid!=cameraValid_ || presentationObservation_!=presentation_.ObservationRevision() || publicationCadence_.Due())Publish();
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
    struct Request {
        Json input;std::promise<Json> reply;std::atomic<unsigned> phase{0}; // queued/running/completed/cancelled
        Clock::time_point startDeadline=Clock::now()+std::chrono::seconds(2);
        // Keep a separate descriptor until the queued request retires. The
        // socket worker may close/reuse its descriptor while FRAME_START checks
        // whether this caller is still connected.
        int peer=-1;
        ~Request(){if(peer>=0)close(peer);}
    };
    static Json Error(const Json& input,const std::string& message) {
        return {{"id",input.is_object()?input.value("id",Json(nullptr)):Json(nullptr)},{"error",{{"message",message}}}};
    }
    static void Resolve(const std::shared_ptr<Request>& request,Json value) { try {request->reply.set_value(std::move(value));request->phase=2;} catch (const std::future_error&) {} }
    static std::string QueuedCancellation(const Request& request) {
        if(Clock::now()>=request.startDeadline)
            return "game thread did not service command within 2 seconds; cancelled before execution";
        if(request.peer>=0) {
            pollfd peer{request.peer,POLLIN,0};
            // POLLHUP observes a fully disconnected peer. EOF/POLLRDHUP alone
            // also occurs for a valid caller that only closed its write half.
            if(poll(&peer,1,0)>0 && (peer.revents&(POLLHUP|POLLERR|POLLNVAL)))
                return "caller disconnected; cancelled before execution";
        }
        return {};
    }
    static bool CancelQueued(const std::shared_ptr<Request>& request,const std::string& reason) {
        unsigned queued=0;if(!request->phase.compare_exchange_strong(queued,3))return false;
        Resolve(request,Error(request->input,reason));return true;
    }
    std::shared_ptr<Request> Queue(Json input,int peer=-1) {
        if (!input.is_object() || !input.contains("method") || !input.at("method").is_string()) throw std::invalid_argument("request requires method");
        auto request=std::make_shared<Request>(); request->input=std::move(input);
        if(peer>=0) {
            request->peer=fcntl(peer,F_DUPFD_CLOEXEC,0);
            if(request->peer<0)throw std::runtime_error("cannot retain command connection");
        }
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queue_.size()>=256) throw std::runtime_error("live command queue is full");
        queue_.push_back(request); return request;
    }
    Json Snapshot() const { std::lock_guard<std::mutex> lock(stateMutex_); return Json::parse(stateJson_); }
    void Publish() {
        auto cosmetics=cosmetics_.State();cosmetics["epoch"]=entityEpoch_;cosmetics["sample_tick"]=cosmeticsSampleTick_;cosmetics["stale"]=!demo_ || cosmetics.value("stale",false);
        const Json removed={{"available",false},{"reason","weapon paint and gloves are disabled in this build"}};
        cosmetics["capabilities"]["weapons"]=removed;cosmetics["capabilities"]["gloves"]=removed;
        const Json weapons={{"weapons",Json::array()},{"capability",removed},{"owned_overrides",0},{"stale",true}};
        const Json gloves={{"gloves",Json::array()},{"capability",removed},{"owned_overrides",0},{"stale",true}};
        cosmeticsBusy_=cosmetics.value("owned_overrides",std::size_t(0))!=0;
        const auto presentation=presentation_.State();
        Json lensRuntime;{std::lock_guard<std::mutex> lock(lensMutex_);lensRuntime=lensRuntime_;}
        auto liveLens=NormalizeLens(project_.lens);if(!demo_)liveLens["enabled"]=false;
        Json state={{"abi",Abi},{"connected",enabled_},{"demo",demo_},{"tick",tick_},{"tick_rate",tickRate_},
                    {"paused",paused_},{"mode",mode_},{"camera",ToJson(camera_)},{"camera_valid",cameraValid_},
                    {"project",ToJson(project_)},{"project_document_id",project_.documentId},{"project_import_receipt",projectImportReceipt_},
                    {"geometry_validation_request_id",geometryValidationRequest_},{"geometry_validation",geometryValidation_},
                    {"geometry_validation_current",geometryValidationRevision_==revision_},
                    {"project_library",projectStore_.State()},{"project_storage",ProjectStorage()},
                    {"cursor",cursor_},{"preview_playing",playing_},{"sync_demo",syncDemo_},{"demo_seek",DemoSeekState()},
                    {"revision",revision_},{"view_hook_installed",hookInstalled_},{"view_writes",viewWrites_.load()},
                    {"guarded_mutation_abi",1},
                    {"editor_preferences",editorPreferences_},{"editor_preferences_revision",editorPreferencesRevision_},
                    {"editor_preferences_error",editorPreferencesError_},
                    {"undo_available",!undo_.empty()},{"redo_available",!redo_.empty()},
                    {"scene_capabilities",scene_.Capabilities()},{"scene_registry",sceneRegistry_},
                    {"lens",liveLens},{"lens_runtime",lensRuntime},
                    {"appearance",{{"active",false}}},{"appearance_capabilities",{{"available",false},{"reason","global appearance packs are disabled in this build"}}},
                    {"appearance_refresh",demoRefresh_.State()},
                    {"entity_inspection_available",bool(entityInspector_)},{"entity_probe",entityProbe_},
                    {"cosmetics",std::move(cosmetics)},
                    {"presentation",presentation},
                    {"demo_timeline",demoTimeline_.State(demo_,tick_,paused_,presentation.value("selected_player",Json(nullptr)))},
                    {"demo_playback_range",demoPlaybackRange_.State()},
                    {"weapon_cosmetics",std::move(weapons)},
                    {"glove_cosmetics",std::move(gloves)},
                    {"export_request_id",exportRequestId_},{"safe_exit_request_id",safeExitRequestId_},{"safe_exit",safeExitStatus_}};
        if (!error_.empty()) state["error"]=error_;
        // Static map data is validated/serialized once. Avoid copying thousands
        // of JSON arrays and formatting their numbers on every preview frame.
        auto compact=state.dump();
        std::lock_guard<std::mutex> lock(stateMutex_);
        uiStateJson_=compact;
        compact.pop_back();compact+=",\"map_preview\":";compact+=mapPreviewJson_;compact+='}';
        stateJson_=std::move(compact);publicationCadence_.Advance();presentationObservation_=presentation_.ObservationRevision();
    }
    void NeedDemo() const {
        if (!demo_) throw std::runtime_error("camera control requires active local demo playback");
        if (!hookInstalled_) throw std::runtime_error("validated camera hook unavailable: "+error_);
    }
    static bool ValidImportToken(const Json& token) {
        if(!token.is_string())return false;
        const auto text=token.get<std::string>();
        return !text.empty() && text.size()<=128 && text.find('\0')==std::string::npos;
    }
    void RecordImportReceipt(const Json& input,bool applied,const std::string& error) {
        if(!input.is_object() || !input.contains("method") || input["method"]!="import_project" ||
           !input.contains("params") || !input["params"].is_object())return;
        const auto& params=input["params"];
        if(!params.contains("import_token") || !ValidImportToken(params["import_token"]))return;
        projectImportReceipt_={{"token",params["import_token"]},{"applied",applied},
            {"project_document_id",project_.documentId},{"revision",revision_},{"error",error.substr(0,512)}};
    }
    void NeedCamera() const { if (!cameraValid_) throw std::runtime_error("waiting for the first valid game camera sample"); }
    struct DemoSeekTarget { int tick=0; bool paused=true; bool atOrAfter=false; };
    Json DemoSeekState() const {
        const auto& target=demoSeekQueued_?demoSeekQueued_:demoSeekIssued_;
        return {{"active",bool(demoSeekIssued_)},{"queued",bool(demoSeekQueued_)},
                {"target_tick",target?Json(target->tick):Json(nullptr)},
                {"dispatched_tick",demoSeekIssued_?Json(demoSeekIssued_->tick):Json(nullptr)}};
    }
    void CancelDemoSeek() { demoSeekIssued_.reset();demoSeekQueued_.reset(); }
    void SetDemoSeekPauseIntent(bool paused) {
        pauseIntent_=paused;
        if(demoSeekIssued_)demoSeekIssued_->paused=paused;
        if(demoSeekQueued_)demoSeekQueued_->paused=paused;
    }
    Clock::time_point DemoSeekNow() const {return demoSeekNow_?demoSeekNow_():Clock::now();}
    void DispatchDemoSeek(DemoSeekTarget target,const Execute& execute) {
        try {
            PrepareCosmeticsSeek();
            demoSeekIssued_=target;demoSeekStarted_=DemoSeekNow();pauseIntent_=target.paused;
            if(target.paused)execute("demo_pause");
            execute("demo_gototick "+std::to_string(target.tick)+(target.paused?" pause":""));
            if(!target.paused)execute("demo_resume");
        }catch(...) {CancelDemoSeek();throw;}
    }
    void RequestDemoSeek(int target,bool paused,const Execute& execute,bool atOrAfter=false) {
        const DemoSeekTarget next{target,paused,atOrAfter};
        if(demoSeekIssued_) {
            // Native demo seeks are asynchronous and expensive. Keep only the
            // newest destination while one is running, rather than feeding a
            // chain of obsolete seeks to the engine in the same FRAME_START.
            if(next.tick==demoSeekIssued_->tick && next.paused==demoSeekIssued_->paused && next.atOrAfter==demoSeekIssued_->atOrAfter)demoSeekQueued_.reset();
            else demoSeekQueued_=next;
        }else DispatchDemoSeek(next,execute);
    }
    bool AdvanceDemoSeek(const Execute& execute) {
        if(!demo_) {const bool active=bool(demoSeekIssued_);CancelDemoSeek();return active;}
        if(!demoSeekIssued_)return false;
        if(std::abs(tick_-demoSeekIssued_->tick)<=8 && (!demoSeekIssued_->atOrAfter || demoSeekIssued_->paused || tick_>=demoSeekIssued_->tick)) {
            const bool desiredPause=demoSeekIssued_->paused;
            demoSeekIssued_.reset();
            if(demoSeekQueued_) {const auto next=*demoSeekQueued_;demoSeekQueued_.reset();DispatchDemoSeek(next,execute);}
            else if(desiredPause!=paused_) {pauseIntent_=desiredPause;execute(desiredPause?"demo_pause":"demo_resume");}
            return true;
        }
        if(DemoSeekNow()-demoSeekStarted_>std::chrono::seconds(30)) {
            CancelDemoSeek();commandError_="demo seek did not reach its target within 30 seconds; no automatic retry was sent";
            return true;
        }
        return false;
    }
    void PrepareCosmeticsSeek() {
        cosmetics_.Refresh(false);
        ++entityEpoch_;if(entityProbe_.is_object())entityProbe_["stale"]=true;
    }
    void Commit(Project value) {
        Normalize(value);
        if(!value.scene.empty() || !value.cosmetics.empty() || !value.presentation.empty())NeedDemo();
        ValidateSupportedCosmetics(value.cosmetics);cosmetics_.ValidateRequest(value.cosmetics);
        presentation_.ValidateRequest(value.presentation);
        scene_.Apply(EffectiveSceneSettings(value));
        presentation_.SetSceneCapabilities(scene_.Capabilities());
        try{presentation_.Apply(value.presentation);cosmetics_.Apply(value.cosmetics);}
        catch(...){scene_.Apply(EffectiveSceneSettings(project_));presentation_.SetSceneCapabilities(scene_.Capabilities());presentation_.Apply(project_.presentation);cosmetics_.Apply(project_.cosmetics);throw;}
        undo_.push_back(project_); if (undo_.size()>64) undo_.pop_front();
        // The native seek already issued cannot be cancelled. A deferred
        // destination belongs to the old document and must not move a newly
        // imported/loaded project after the first seek eventually completes.
        if(value.documentId!=project_.documentId)demoSeekQueued_.reset();
        project_=std::move(value); redo_.clear(); playing_=false; if(mode_=="preview") mode_="hold";
        cursor_=std::min(cursor_,project_.duration); ++revision_;
    }
    std::string ProjectPath() const {
        const char* path=std::getenv("CS2_CAMERA_PROJECT");
        if(!path || !*path || path[0]!='/') throw std::runtime_error("CS2_CAMERA_PROJECT must specify an absolute project filename");
        return path;
    }
    Json SaveQuickProject() {
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
    Json CurrentProjectDemo()const {
        const auto timeline=demoTimeline_.State(demo_,tick_,paused_,nullptr);
        if(timeline.contains("demo"))return {{"path",timeline["demo"]["path"]},{"fingerprint",timeline["demo"]["fingerprint"]}};
        const char* path=std::getenv("CS2_CAMERA_DEMO_PATH");
        if(path && *path && path[0]=='/')return {{"path",path}};
        return nullptr;
    }
    static void ValidateStoredProject(const Json& value){ReadProject(value);}
    Json ProjectStorage()const {
        const auto found=storedDocuments_.find(project_.documentId);
        Json state={{"id",nullptr},{"path",nullptr},{"warning",projectStorageWarning_}};
        if(found!=storedDocuments_.end()) {
            state["id"]=found->second;
            try{state["path"]=ProjectStore::Directory()+"/"+found->second+".json";}catch(const std::exception&){}
        }
        return state;
    }
    Json SaveProject(bool copy=false) {
        projectStorageWarning_.clear();const auto found=storedDocuments_.find(project_.documentId);
        const auto result=projectStore_.Save(ToJson(project_),!copy && found!=storedDocuments_.end()?found->second:std::string(),CurrentProjectDemo());
        storedDocuments_[project_.documentId]=result.at("id").get<std::string>();
        while(storedDocuments_.size()>512)storedDocuments_.erase(storedDocuments_.begin());
        // Retain the old recovery/quick-save entry without making it the sole
        // copy of every authored composition.
        const char* quick=std::getenv("CS2_CAMERA_PROJECT");
        if(quick && *quick)try{SaveQuickProject();}catch(const std::exception& error){projectStorageWarning_="Project saved; quick-save copy failed: "+std::string(error.what());}
        projectStore_.Refresh(CurrentProjectDemo(),ValidateStoredProject);return result;
    }
    void LoadSavedProject(const std::string& id) {
        const auto value=projectStore_.Load(id,CurrentProjectDemo(),ValidateStoredProject);
        Project next=ReadProject(value);next.documentId=++projectId_;Commit(std::move(next));
        storedDocuments_[project_.documentId]=id;projectStorageWarning_.clear();
        try{projectStore_.Opened(id);}catch(const std::exception& error){projectStorageWarning_="Project opened; recency could not be saved: "+std::string(error.what());}
        projectStore_.Refresh(CurrentProjectDemo(),ValidateStoredProject);
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
            next.documentId=++projectId_;
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
        if(method=="set_player_weapon_paint" || method=="reset_player_weapon_paint" || method=="set_player_gloves" || method=="reset_player_gloves")
            throw std::runtime_error("weapon paint and gloves are disabled in this build");
        if(method=="apply_appearance_pack" || method=="reset_appearance" || method=="probe_appearance_pack")
            throw std::runtime_error("global appearance packs are disabled in this build");
        if (!params.is_object()) throw std::invalid_argument("params must be an object");
        if(method=="set_safe_exit_status") {
            if(!params.contains("phase") || !params["phase"].is_string() || params.size()>2 ||
               (params.size()==2 && !params.contains("error")))throw std::invalid_argument("safe exit status requires phase and optional error");
            const auto phase=params["phase"].get<std::string>();
            if(phase!="idle" && phase!="requested" && phase!="saving" && phase!="closing" && phase!="error")
                throw std::invalid_argument("invalid safe exit phase");
            if(params.contains("error") && (!params["error"].is_string() || params["error"].get<std::string>().size()>4096 || params["error"].get<std::string>().find('\0')!=std::string::npos))
                throw std::invalid_argument("safe exit error must be text up to 4096 bytes");
            const auto current=safeExitStatus_.value("phase",std::string("idle"));
            if(phase=="idle" && (current=="requested" || current=="saving" || current=="closing"))
                throw std::runtime_error("safe exit is in progress; Keep editing is only available after a save error");
            safeExitStatus_={{"phase",phase},{"error",params.value("error",std::string())}};return safeExitStatus_;
        }
        if(method=="request_safe_exit") {
            if(!params.empty())throw std::invalid_argument("request_safe_exit takes no arguments");
            const auto phase=safeExitStatus_.value("phase",std::string("idle"));
            if(phase=="requested" || phase=="saving" || phase=="closing")return {{"request_id",safeExitRequestId_}};
            ++safeExitRequestId_;safeExitStatus_={{"phase","requested"},{"error",""}};
            playing_=false;waitingForSeek_=false;if(mode_=="preview")mode_="hold";
            if(demo_){SetDemoSeekPauseIntent(true);execute("demo_pause");}
            return {{"request_id",safeExitRequestId_}};
        }
        const auto exitPhase=safeExitStatus_.value("phase",std::string("idle"));
        if(exitPhase=="requested" || exitPhase=="saving" || exitPhase=="closing") {
            static const std::vector<std::string> allowed={"get_state","ping","export_project","save_project","save_project_copy",
                "refresh_project_library","set_editor_preferences","inspect_entities","set_map_preview","request_geometry_validation",
                "set_geometry_validation","set_demo_timeline"};
            if(std::find(allowed.begin(),allowed.end(),method)==allowed.end())
                throw std::runtime_error("safe exit is saving or closing; editing is temporarily locked");
        }
        if(method=="set_editor_preferences") {
            const auto next=ValidateEditorPreferences(params,editorPreferences_);
            if(next!=editorPreferences_ || !editorPreferencesError_.empty()){editorPreferences_=next;++editorPreferencesRevision_;editorPreferencesError_.clear();}
            return {{"editor_preferences",editorPreferences_},{"editor_preferences_revision",editorPreferencesRevision_}};
        }
        if(method=="guarded_mutation") {
            if(params.size()<3 || params.size()>6 || !params.contains("expected_revision") || !params["expected_revision"].is_number_integer() ||
               !params.contains("method") || !params["method"].is_string() || !params.contains("params") || !params["params"].is_object())
                throw std::invalid_argument("guarded mutation requires expected_revision, method and params");
            for(auto it=params.begin();it!=params.end();++it)
                if(it.key()!="expected_revision" && it.key()!="expected_tick" && it.key()!="expected_paused" && it.key()!="reset_cursor" && it.key()!="method" && it.key()!="params")throw std::invalid_argument("unknown guarded mutation field");
            const auto expected=params["expected_revision"].get<long long>();
            if(expected<0 || static_cast<std::uint64_t>(expected)!=revision_)throw std::runtime_error("camera changed during geometry validation; retry from current state");
            if(params.contains("expected_tick") && (!params["expected_tick"].is_number_integer() || params["expected_tick"].get<long long>()!=tick_ || DemoSeekState().value("active",false)))
                throw std::runtime_error("demo tick changed during geometry validation; pause and retry");
            if(params.contains("expected_paused") && (!params["expected_paused"].is_boolean() || params["expected_paused"].get<bool>()!=paused_))
                throw std::runtime_error("demo pause state changed during geometry validation; pause and retry");
            const std::string nested=params["method"].get<std::string>();
            const std::vector<std::string> allowed={"set_camera","add_key","update_key","remove_key","set_project","import_project","apply_preset","scrub","preview","load_project","load_saved_project","request_export"};
            if(std::find(allowed.begin(),allowed.end(),nested)==allowed.end())throw std::invalid_argument("method is not a guarded camera mutation");
            if(params.contains("reset_cursor") && (!params["reset_cursor"].is_boolean() || nested!="set_project"))
                throw std::invalid_argument("reset_cursor requires a guarded set_project and boolean value");
            const auto result=Command({{"method",nested},{"params",params["params"]}},execute);
            if(params.value("reset_cursor",false))cursor_=0;
            return result;
        }
        if(method=="set_map_preview") {
            if(params.size()!=1 || !params.contains("preview"))throw std::invalid_argument("set_map_preview requires only preview");
            const auto preview=ValidateMapPreview(params["preview"]);
            {std::lock_guard<std::mutex> lock(stateMutex_);mapPreviewJson_=preview.dump();}
            return nullptr;
        }
        if(method=="request_geometry_validation") {
            if(!params.empty())throw std::invalid_argument("request_geometry_validation takes no arguments");
            ++geometryValidationRequest_;geometryValidationRevision_=revision_;
            geometryValidation_={{"status","pending"},{"passed",false}};return nullptr;
        }
        if(method=="set_geometry_validation") {
            if(params.size()!=3 || !params.contains("request_id") || !params["request_id"].is_number_integer() ||
               !params.contains("expected_revision") || !params["expected_revision"].is_number_integer() ||
               !params.contains("result") || !params["result"].is_object() || params["result"].dump().size()>16384)
                throw std::invalid_argument("invalid geometry validation result");
            if(params["request_id"].get<std::uint64_t>()!=geometryValidationRequest_ || params["expected_revision"].get<std::uint64_t>()!=revision_)
                return {{"accepted",false}};
            if(!params["result"].contains("passed") || !params["result"]["passed"].is_boolean() || !params["result"].contains("status") || !params["result"]["status"].is_string())
                throw std::invalid_argument("geometry validation result requires status and passed");
            geometryValidation_=params["result"];geometryValidationRevision_=revision_;return {{"accepted",true}};
        }
        if (method=="ping") return {{"pong",true},{"abi",Abi}};
        if (method=="get_state") return nullptr;
        if (method=="set_demo_timeline") {
            if(params.size()!=1 || !params.contains("timeline"))throw std::invalid_argument("set_demo_timeline requires only timeline metadata");
            demoTimeline_.Load(params.at("timeline"),std::getenv("CS2_CAMERA_DEMO_PATH"));
            demoPlaybackRange_.Bind(demoTimeline_.PlaybackTicks(),demoTimeline_.RecordingRevision());return nullptr;
        }
        if(method=="set_demo_playback_range") {
            NeedDemo();demoPlaybackRange_.Update(params);return nullptr;
        }
        if (method=="select_timeline_player") {
            if(!demo_)throw std::runtime_error("timeline marker selection requires active local demo playback");
            if(params.size()!=1 || !params.contains("steam_id"))throw std::invalid_argument("select_timeline_player requires only steam_id");
            demoTimeline_.SelectPlayer(params.at("steam_id"));return nullptr;
        }
        if (method=="export_project") return ToJson(project_);
        if (method=="request_export") {
            if(!params.empty())throw std::invalid_argument("request_export takes no arguments");
            ++exportRequestId_;return nullptr;
        }
        if (method=="refresh_project_library") {
            if(!params.empty())throw std::invalid_argument("refresh_project_library takes no arguments");
            projectStore_.Refresh(CurrentProjectDemo(),ValidateStoredProject);return nullptr;
        }
        if (method=="load_saved_project") {
            if(params.size()!=1 || !params.contains("id") || !params["id"].is_string())throw std::invalid_argument("load_saved_project requires only a saved project id");
            LoadSavedProject(params["id"].get<std::string>());return nullptr;
        }
        if (method=="save_project" || method=="save_project_copy") {
            if(!params.empty())throw std::invalid_argument("native project save takes no arguments");
            return SaveProject(method=="save_project_copy");
        }
        if(demoRefresh_.Active())throw std::runtime_error("wait for the appearance demo reload to finish before editing");
        if (method=="load_project") { LoadProject(); return nullptr; }
        if(method=="inspect_entities") {
            NeedDemo();
            for(auto entry=params.begin();entry!=params.end();++entry)
                if(entry.key()!="limit")throw std::invalid_argument("inspect_entities only accepts limit");
            const auto value=params.value("limit",Json(64));
            if(!value.is_number_integer() || value.get<long long>()<1 || value.get<long long>()>128)
                throw std::invalid_argument("inspect_entities limit must be an integer in 1..128");
            if(!entityInspector_)throw std::runtime_error("client entity inspection backend is not bound");
            entityProbe_=entityInspector_(value.get<unsigned>());entityProbe_["epoch"]=entityEpoch_;entityProbe_["sample_tick"]=tick_;entityProbe_["snapshot_id"]=++entitySnapshot_;entityProbe_["stale"]=false;return entityProbe_;
        }
        if(method=="set_player_cosmetics" || method=="reset_player_cosmetics" || method=="set_team_palette" || method=="reset_team_palette") {
            NeedDemo();
            Project next=project_;
            if(method=="set_player_cosmetics") {
                CosmeticsKeys(params,{"steam_id","nickname","agent_item_def"},"set_player_cosmetics");
                const Json request={{"players",Json::array({params})}};cosmetics_.ValidateRequest(request,true);
                auto players=next.cosmetics.value("players",Json::array());
                players.erase(std::remove_if(players.begin(),players.end(),[&](const Json& item){return item["steam_id"]==params.at("steam_id");}),players.end());
                if(!cosmetics_.IsMapDefault(params.at("agent_item_def").get<unsigned>()))players.push_back(params);
                if(players.empty())next.cosmetics.erase("players");else next.cosmetics["players"]=std::move(players);
            }else if(method=="reset_player_cosmetics") {
                CosmeticsKeys(params,{"steam_id"},"reset_player_cosmetics");
                if(params.contains("steam_id"))ValidateSteamId(params["steam_id"]);
                if(next.cosmetics.contains("players")) {
                    auto& players=next.cosmetics["players"];
                    if(params.empty())next.cosmetics.erase("players");
                    else {players.erase(std::remove_if(players.begin(),players.end(),[&](const Json& item){return item["steam_id"]==params["steam_id"]; }),players.end());if(players.empty())next.cosmetics.erase("players");}
                }
            }else if(method=="set_team_palette") {
                CosmeticsKeys(params,{"enabled","t","ct"},"set_team_palette");
                auto palette=next.cosmetics.value("team_palette",Json{{"enabled",false},{"t",{255,64,64}},{"ct",{64,128,255}}});
                palette.update(params);next.cosmetics["team_palette"]=std::move(palette);
            }else {if(!params.empty())throw std::invalid_argument("reset_team_palette takes no arguments");next.cosmetics.erase("team_palette");}
            Commit(std::move(next));return nullptr;
        }
        if(method=="select_player") {
            NeedDemo();CosmeticsKeys(params,{"steam_id"},"select_player");ValidateSteamId(params.at("steam_id"));
            const auto viewer=presentation_.Merge({{"view","player"}});
            // Viewer selection is transient: it never rewrites the authored
            // shot, HUD choice, keys, cosmetics or undo history.
            presentation_.SelectPlayer(params.at("steam_id").get<std::string>(),viewer);
            playing_=false;waitingForSeek_=false;mode_="free";return nullptr;
        }
        if(method=="set_presentation") {
            NeedDemo();Project next=project_;next.presentation=presentation_.Merge(params);Commit(std::move(next));
            if(params.contains("view")){playing_=false;mode_="free";}
            return nullptr;
        }
        if(method=="set_lens") {
            NeedDemo();if(params.size()!=1 || !params.contains("settings"))throw std::invalid_argument("set_lens requires settings");
            NormalizeLens(params["settings"]);
            Project next=project_;next.lens=NormalizeLens(next.lens);next.lens.update(params["settings"]);next.lens=NormalizeLens(next.lens);
            if(next.lens["enabled"].get<bool>()) {
                std::lock_guard<std::mutex> lock(lensMutex_);
                if(!lensRuntime_.value("available",false))throw std::runtime_error("Vulkan lens preview is unavailable: "+lensRuntime_.value("reason",std::string("renderer not connected")));
            }
            Commit(std::move(next));return nullptr;
        }
        if(method=="reset_lens") {NeedDemo();Project next=project_;next.lens=Json::object();Commit(std::move(next));return nullptr;}
        if(method=="set_scene") {
            NeedDemo();
            const auto& settings=params.at("settings");ValidateSceneSettings(settings);
            Project next=project_;
            if(settings.contains("shading_mode") && !settings.contains("fullbright"))next.scene.erase("fullbright");
            if(settings.contains("fullbright") && !settings.contains("shading_mode"))next.scene.erase("shading_mode");
            next.scene.update(settings);Commit(std::move(next));
        } else if(method=="reset_scene") {
            NeedDemo();Project next=project_;next.scene=Json::object();Commit(std::move(next));
        } else if (method=="set_camera") {
            NeedDemo(); NeedCamera(); camera_=ReadPose(params.contains("camera")?params.at("camera"):params,camera_);
            mode_="hold"; playing_=false; execute("spec_autodirector 0"); ++revision_;
        } else if (method=="release_camera") {
            playing_=false; mode_="free"; ++revision_;
        } else if (method=="add_key") {
            NeedDemo(); NeedCamera(); Project next=project_;
            Key key; key.id=NewKeyId(); key.time=params.value("time",cursor_); key.pose=camera_;
            if(next.keys.empty()) next.startTick=std::max(0,tick_-int(std::llround(key.time*tickRate_)));
            Json value=params; value["id"]=key.id;
            key=ReadKey(value,key); next.duration=std::max(next.duration,key.time);
            if(next.duration!=project_.duration) ResizeRenderRange(next,project_.duration);
            next.keys.push_back(key); Commit(next);
            return ToJson(key);
        } else if (method=="update_key" || method=="remove_key") {
            Project next=project_; const std::string id=params.at("id").get<std::string>();
            auto key=std::find_if(next.keys.begin(),next.keys.end(),[&](const Key& candidate){return candidate.id==id;});
            if (key==next.keys.end()) throw std::invalid_argument("unknown camera key id");
            if (method=="remove_key") next.keys.erase(key); else *key=ReadKey(params,*key);
            Commit(next);
        } else if (method=="set_project" || method=="import_project") {
            if(method=="import_project" && params.contains("import_token") && !ValidImportToken(params["import_token"]))
                throw std::invalid_argument("import_token must contain 1..128 bytes without NUL");
            Json value=params.contains("project")?params.at("project"):params;
            if (method=="set_project") {
                if(!value.is_object()) throw std::invalid_argument("project must be an object");
                Project base=project_;
                if(value.contains("duration")) {
                    base.duration=value.at("duration").get<double>();
                    Finite(base.duration,.01,86400,"duration");
                    if(base.duration!=project_.duration) ResizeRenderRange(base,project_.duration);
                }
                Json merged=ToJson(base);
                if(value.contains("render_range")) {
                    if(!value.at("render_range").is_object()) throw std::invalid_argument("render_range must be an object");
                    Json range=merged.at("render_range"); range.update(value.at("render_range"));
                    merged.update(value); merged["render_range"]=std::move(range);
                } else merged.update(value);
                value=std::move(merged);
            }
            auto next=ReadProject(value);
            next.documentId=method=="import_project"?++projectId_:project_.documentId;
            Commit(std::move(next));
        } else if (method=="scrub") {
            NeedDemo(); NeedCamera(); double time=params.at("time").get<double>(); Finite(time,0,project_.duration,"cursor");
            Pose next=Sample(project_,time); camera_=next; cursor_=time; playing_=false; mode_="hold";
            waitingForSeek_=false;
            if (params.value("seek_demo",false)) {
                pauseIntent_=true;
                RequestDemoSeek(project_.startTick+int(std::llround(time*tickRate_)),true,execute);
            }
            ++revision_;
        } else if (method=="preview") {
            NeedDemo(); NeedCamera(); bool playing=params.value("playing",true);
            const bool wasPlaying=playing_,oldSync=syncDemo_;
            const bool sync=params.value("sync_demo",syncDemo_);
            if (playing && project_.keys.empty()) throw std::invalid_argument("add a camera key before preview");
            syncDemo_=sync;
            if (playing) {
                const auto range=PlaybackRange(project_);
                if(cursor_<range.in || cursor_>=range.out) cursor_=range.in;
                previewStartCursor_=cursor_; previewStart_=Clock::now(); playing_=true; mode_="preview";
                camera_=Sample(project_,cursor_); execute("spec_autodirector 0");
                waitingForSeek_=syncDemo_;
                if (syncDemo_) {
                    pauseIntent_=false;previewSeekTick_=project_.startTick+int(std::llround(cursor_*tickRate_));
                    RequestDemoSeek(previewSeekTick_,false,execute);
                }
                else {SetDemoSeekPauseIntent(true);execute("demo_pause");}
            } else {
                playing_=false;
                if(wasPlaying) mode_="hold";
                if(wasPlaying && oldSync) {SetDemoSeekPauseIntent(true);execute("demo_pause");}
            }
            ++revision_;
        } else if (method=="demo_control") {
            NeedDemo(); const std::string action=params.at("action").get<std::string>();
            if(action=="pause" || action=="resume") {
                if(action=="resume") {
                    // Demo Play returns to the observed game camera. It must
                    // not leave a held camera frozen over a moving recording.
                    playing_=false;waitingForSeek_=false;mode_="free";
                    const int destination=demoSeekQueued_?demoSeekQueued_->tick:demoSeekIssued_?demoSeekIssued_->tick:tick_;
                    if(demoPlaybackRange_.Enabled() && !demoPlaybackRange_.Contains(destination))
                        RequestDemoSeek(demoPlaybackRange_.In(),false,execute,true);
                    else {
                        if(demoPlaybackRange_.Enabled()) {
                            if(demoSeekIssued_)demoSeekIssued_->atOrAfter=true;
                            if(demoSeekQueued_)demoSeekQueued_->atOrAfter=true;
                        }
                        SetDemoSeekPauseIntent(false);execute("demo_resume");
                    }
                } else {
                    if(playing_){playing_=false;waitingForSeek_=false;mode_="hold";}
                    SetDemoSeekPauseIntent(true);execute("demo_pause");
                }
            }
            else if (action=="seek") {
                const auto& value=params.at("tick");
                if(!value.is_number_integer() || value.get<long long>()<0 || value.get<long long>()>2000000000)
                    throw std::invalid_argument("invalid demo tick");
                const int target=value.get<int>();
                const bool preservePause=pauseIntent_.value_or(demoSeekQueued_?demoSeekQueued_->paused:
                    demoSeekIssued_?demoSeekIssued_->paused:paused_);
                pauseIntent_=preservePause;
                // Full-demo navigation exits the held preset camera. Otherwise
                // ticks move behind a frozen pose, or synchronized preview
                // immediately pauses the seek again at the shot's Out point.
                playing_=false;waitingForSeek_=false;mode_="free";
                RequestDemoSeek(target,preservePause,execute);
            }
            else throw std::invalid_argument("demo action must be pause, resume, or seek");
        } else if (method=="apply_preset") {
            NeedDemo(); NeedCamera(); Project next=project_; next.keys.clear(); next.startTick=std::max(0,tick_);
            const auto name=params.at("name").get<std::string>(); next.duration=params.value("duration",5.0); Finite(next.duration,.01,86400,"duration");
            if(next.duration!=project_.duration) ResizeRenderRange(next,project_.duration);
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
            if(!source.back().scene.empty() || !source.back().cosmetics.empty() || !source.back().presentation.empty() || !project_.cosmetics.empty() || !project_.presentation.empty())NeedDemo();
            ValidateSupportedCosmetics(source.back().cosmetics);cosmetics_.ValidateRequest(source.back().cosmetics);
            presentation_.ValidateRequest(source.back().presentation);
            scene_.Apply(EffectiveSceneSettings(source.back()));
            presentation_.SetSceneCapabilities(scene_.Capabilities());
            try{presentation_.Apply(source.back().presentation);cosmetics_.Apply(source.back().cosmetics);}
            catch(...){scene_.Apply(EffectiveSceneSettings(project_));presentation_.SetSceneCapabilities(scene_.Capabilities());presentation_.Apply(project_.presentation);cosmetics_.Apply(project_.cosmetics);throw;}
            target.push_back(project_);
            if(source.back().documentId!=project_.documentId)demoSeekQueued_.reset();
            project_=source.back(); source.pop_back(); playing_=false; if(mode_=="preview") mode_="hold"; cursor_=std::min(cursor_,project_.duration); ++revision_;
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
                    auto request=Queue(input,client); auto future=request->reply.get_future();
                    const auto method=input.at("method").get<std::string>();
                    const bool appearance=method=="apply_appearance_pack" || method=="reset_appearance" || method=="probe_appearance_pack" || method=="set_player_cosmetics" || method=="reset_player_cosmetics" || method=="set_team_palette" || method=="reset_team_palette" || method=="set_player_weapon_paint" || method=="reset_player_weapon_paint" || method=="set_player_gloves" || method=="reset_player_gloves";
                    const auto expires=Clock::now()+std::chrono::seconds(appearance?30:2);
                    while(running_ && Clock::now()<expires && future.wait_for(std::chrono::milliseconds(30))!=std::future_status::ready) {
                        // Long appearance operations may run for 30 seconds,
                        // but no command may begin after its caller timed out.
                        if(request->phase.load()==0) {
                            const auto reason=QueuedCancellation(*request);
                            if(!reason.empty())CancelQueued(request,reason);
                        }
                    }
                    if(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
                        if(!CancelQueued(request,running_?"game thread did not service command within 2 seconds; cancelled before execution":"live session stopped; cancelled before execution"))
                            throw std::runtime_error("engine operation already started; outcome is unknown until get_state confirms it; do not retry blindly");
                    }
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
    LiveTelemetryCadence cosmeticsCadence_,publicationCadence_;
    DemoTimeline demoTimeline_;
    DemoPlaybackRange demoPlaybackRange_;
    ProjectStore projectStore_;
    std::map<std::uint64_t,std::string> storedDocuments_;
    std::string projectStorageWarning_;
    bool cosmeticsBusy_=false;
    int cosmeticsSampleTick_=0;
    bool enabled_=false,ownsSocket_=false;
    std::atomic<bool> running_{false}; int listener_=-1; std::thread worker_;
    std::string socketPath_,startupError_;
    mutable std::mutex stateMutex_; std::string stateJson_,uiStateJson_,mapPreviewJson_="{}";
    std::mutex lensMutex_;Json lensRuntime_={{"available",false},{"backend","vulkan-color"},{"depth_available",false},{"reason","renderer not connected"}};
    std::mutex queueMutex_; std::deque<std::shared_ptr<Request>> queue_;
    std::mutex viewMutex_; Pose observed_,output_; bool observedValid_=false,outputEnabled_=false;
    std::atomic<std::uint64_t> viewWrites_{0};
    std::optional<bool> pauseIntent_;
    std::optional<DemoSeekTarget> demoSeekIssued_,demoSeekQueued_;
    LiveTelemetryCadence::Now demoSeekNow_;
    Clock::time_point demoSeekStarted_{};
    bool demo_=false,paused_=false,hookInstalled_=false,cameraValid_=false,playing_=false,syncDemo_=true,waitingForSeek_=false;
    int tick_=0,previewSeekTick_=0; double tickRate_=64,cursor_=0,previewStartCursor_=0;
    Clock::time_point previewStart_=Clock::now();
    Pose camera_; Project project_; std::string mode_="free",error_,commandError_;
    SceneEnvironment scene_;
    DemoRefresh demoRefresh_;
    Execute currentExecute_;
    Pose appearanceSavedCamera_;
    double appearanceSavedCursor_=0;
    int appearanceSavedTick_=0;
    bool appearanceSavedPaused_=true,appearanceTransition_=false;
    Json sceneRegistry_=Json::array();
    Json editorPreferences_=EditorPreferenceDefaults();
    std::uint64_t editorPreferencesRevision_=0;
    std::string editorPreferencesError_;
    Json geometryValidation_=Json::object();
    std::uint64_t geometryValidationRequest_=0,geometryValidationRevision_=std::numeric_limits<std::uint64_t>::max();
    std::function<Json(unsigned)> entityInspector_;
    PlayerCosmetics cosmetics_;
    Presentation presentation_;
    std::uint64_t entityEpoch_=0,entitySnapshot_=0,presentationObservation_=0;
    std::uint64_t cosmeticsRefreshRevision_=std::numeric_limits<std::uint64_t>::max();
    std::uint64_t cosmeticsGeneration_=0;
    Json entityProbe_=Json::object();
    std::uint64_t revision_=0,keyId_=0,fileId_=0;
    std::uint64_t projectId_=0;
    Json projectImportReceipt_=Json::object();
    std::uint64_t exportRequestId_=0;
    std::uint64_t safeExitRequestId_=0;
    Json safeExitStatus_={{"phase","idle"},{"error",""}};
    std::deque<Project> undo_,redo_;
};
} // namespace cs2_live
