#pragma once

// Whitelisted engine postprocessing controls. The core owns no game pointers;
// its callbacks run only from the controller's game-thread Frame/Shutdown.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef CS2_CAMERA_SOURCE2_CVARS
#include "icvar.h"
#include "tier1/convar.h"
#endif

namespace cs2_live {
using SceneJson=nlohmann::json;
struct SceneControl {
    const char *id,*cvar,*label,*group;
    bool boolean;
    double minimum=0,maximum=1;
    bool integer=false;
};
inline const std::vector<SceneControl>& SceneControls() {
    static const std::vector<SceneControl> controls={
        {"depth_of_field","r_depth_of_field","Depth of field renderer","Lens",true},
        {"dof_override","r_dof_override","Depth of field","Lens",true},
        {"dof_near_blurry","r_dof_override_near_blurry","Near blur distance","Lens",false,-10000,100000},
        {"dof_near_crisp","r_dof_override_near_crisp","Near focus distance","Lens",false,0,100000},
        {"dof_far_crisp","r_dof_override_far_crisp","Far focus distance","Lens",false,0,100000},
        {"dof_far_blurry","r_dof_override_far_blurry","Far blur distance","Lens",false,0,100000},
        {"dof_tilt","r_dof_override_tilt_to_ground","Ground tilt","Lens",false,0,1},
        {"dof_radius","r_dof2_radiusscale","Blur radius scale","Lens",false,0,10},
        {"dof_max_blur","r_dof2_maxblursize","Maximum blur size","Lens",false,0,64},
        {"bloom_strength","r_csgo_render_post_bloom_strength","Bloom strength","Post processing",false,0,10},
        {"exposure","mat_tonemap_force_scale","Exposure scale override","Lighting",false,0,16},
        {"color_correction","r_csgo_render_post_colorcorrection","Color correction","Post processing",true},
        {"tonemapping","r_csgo_enable_tonemapping","Tonemapping","Post processing",true},
        {"fullbright","mat_fullbright","Fullbright lighting","Lighting",true},
        {"shading_mode","mat_fullbright","Shading: 0 normal, 1 fullbright, 2 diffuse lighting","Lighting",false,0,2,true},
        {"fog_gradient","r_csgo_enable_gradient_fog","Gradient fog","Environment",true},
        {"fog_cubemap","r_csgo_enable_cubemap_fog","Cubemap fog","Environment",true},
        {"fog_volume","r_csgo_enable_volume_fog","Volume fog","Environment",true},
        {"fog","fog_enable","Fog rendering","Environment",true},
        {"hud_visible","cl_drawhud","Game HUD","Environment",true},
        {"viewmodel_visible","r_drawviewmodel","First-person weapon","Environment",true},
        {"fps_counter_visible","cl_showfps","FPS counter","Environment",true},
        {"trueview_status_visible","cl_trueview_show_status","TrueView status","Environment",true},
        {"telemetry_frametime_visible","cl_hud_telemetry_frametime_show","Frame-time telemetry","Environment",true},
        {"telemetry_network_visible","cl_hud_telemetry_net_misdelivery_show","Network telemetry","Environment",true},
        {"telemetry_ping_visible","cl_hud_telemetry_ping_show","Ping telemetry","Environment",true},
        {"telemetry_margin_visible","cl_hud_telemetry_serverrecvmargin_graph_show","Server receive margin","Environment",true},
        {"build_info_visible","r_show_build_info","Build information","Environment",true},
        {"spectator_stats_visible","cl_spec_stats","Spectator stats","Presentation",true},
        {"spectator_bindings_visible","cl_spec_show_bindings","Spectator bindings","Presentation",true},
        {"spectator_square_radar","cl_radar_square_when_spectating","Square spectator radar","Presentation",true},
        {"spectator_all_players_radar","cl_radar_show_all_players_when_spectating","All players on spectator radar","Presentation",true},
    };
    return controls;
}
inline const SceneControl& SceneControlById(const std::string& id) {
    for(const auto& control:SceneControls()) if(id==control.id) return control;
    throw std::invalid_argument("unknown scene setting: "+id);
}
inline SceneJson GameplayHudScenePreset() {
    return {{"spectator_stats_visible",false},{"spectator_bindings_visible",false},{"spectator_square_radar",false},{"spectator_all_players_radar",false}};
}
inline SceneJson GameplayHudPresetCapabilities(const SceneJson& capabilities) {
    bool available=true,confirmed=true;SceneJson values=SceneJson::object();std::string reason;
    const auto preset=GameplayHudScenePreset();
    for(auto entry=preset.begin();entry!=preset.end();++entry) {
        if(!capabilities.contains(entry.key()) || !capabilities[entry.key()].value("available",false) ||
           !capabilities[entry.key()].contains("value") || !capabilities[entry.key()]["value"].is_boolean()) {
            available=false;confirmed=false;values[entry.key()]=nullptr;if(reason.empty())reason=entry.key()+" is unavailable or is not a boolean control";
        }else {values[entry.key()]=capabilities[entry.key()]["value"];confirmed=confirmed && values[entry.key()]==false;}
    }
    return {{"available",available},{"confirmed",available && confirmed},{"values",values},{"reason",reason.empty()?"four registered spectator cvars with actual primitive readback":reason}};
}
inline void ValidateSceneSettings(const SceneJson& settings) {
    if(!settings.is_object()) throw std::invalid_argument("scene settings must be an object");
    for(auto entry=settings.begin();entry!=settings.end();++entry) {
        const auto& control=SceneControlById(entry.key());
        if(control.boolean) {
            if(!entry->is_boolean()) throw std::invalid_argument(entry.key()+" requires a boolean");
        } else {
            if(!entry->is_number()) throw std::invalid_argument(entry.key()+" requires a finite number");
            const double value=entry->get<double>();
            if(!std::isfinite(value) || value<control.minimum || value>control.maximum ||
               (control.integer && value!=std::trunc(value)))
                throw std::invalid_argument(entry.key()+" is outside its supported finite range");
        }
    }
    if(settings.contains("shading_mode") && settings.contains("fullbright") &&
       settings.at("shading_mode").get<double>()!=(settings.at("fullbright").get<bool>()?1:0))
        throw std::invalid_argument("shading_mode conflicts with the legacy fullbright override");
}
inline std::string SceneNumberText(double value) {
    std::ostringstream text; text.imbue(std::locale::classic());text<<std::setprecision(17)<<value;return text.str();
}
struct SceneCvar {
    bool available=false,integer=false;
    std::string nativeType,raw,reason,help;
    SceneJson value;
    std::optional<double> minimum,maximum;
};
class SceneEnvironment {
public:
    using Read=std::function<SceneCvar(const std::string&)>;
    using Write=std::function<bool(const std::string&,const std::string&)>;
    void Bind(Read read,Write write) {read_=std::move(read);write_=std::move(write);}
    void Refresh() {
        for(const auto& control:SceneControls()) {
            auto value=ReadOne(control);
            if(value.available && !baseline_.count(control.id)) baseline_[control.id]=value;
            current_[control.id]=std::move(value);
        }
    }
    SceneJson Capabilities() const {
        SceneJson capabilities=SceneJson::object();
        for(const auto& control:SceneControls()) {
            const auto found=current_.find(control.id);
            SceneCvar value=found==current_.end()?SceneCvar{}:found->second;
            SceneJson entry={{"available",value.available},{"type",control.boolean?"boolean":"number"},
                {"label",control.label},{"group",control.group},{"cvar",control.cvar},
                {"native_type",value.nativeType},{"value",nullptr}};
            entry["recovery_pending"]=recovery_.count(control.id)!=0;
            if(value.available) {
                entry["value"]=control.boolean?SceneJson(Number(value.value)!=0):value.value;
                entry["baseline"]=control.boolean?SceneJson(Number(baseline_.at(control.id).value)!=0):baseline_.at(control.id).value;
                if(!value.help.empty())entry["help"]=value.help;
                if(!control.boolean) {
                    entry["minimum"]=std::max(control.minimum,value.minimum.value_or(control.minimum));
                    entry["maximum"]=std::min(control.maximum,value.maximum.value_or(control.maximum));
                    entry["integer"]=value.integer || control.integer;
                }
            } else entry["reason"]=value.reason.empty()?"engine cvar adapter is not available":value.reason;
            capabilities[control.id]=std::move(entry);
        }
        return capabilities;
    }
    void ValidateAvailable(const SceneJson& settings) const {
        ValidateSceneSettings(settings);
        for(auto entry=settings.begin();entry!=settings.end();++entry) {
            const auto& control=SceneControlById(entry.key());
            const auto found=current_.find(entry.key());
            if(found==current_.end() || !found->second.available)
                throw std::runtime_error(entry.key()+" is unavailable on this game build: "+
                    (found==current_.end()?"cvar capabilities are not initialized":found->second.reason));
            const auto& value=found->second;
            if(!control.boolean) {
                const double number=entry->get<double>();
                if((value.integer && number!=std::trunc(number)) ||
                   (value.minimum && number<*value.minimum) || (value.maximum && number>*value.maximum))
                    throw std::invalid_argument(entry.key()+" exceeds the runtime cvar type or bounds");
            }
        }
    }
    void Apply(const SceneJson& settings) {
        Recover();
        Refresh();ValidateAvailable(settings);
        struct Change {std::string id,cvar,raw;SceneCvar before;SceneJson expected;bool restoring;};
        std::vector<Change> changes;
        // These two public controls are aliases of one engine integer. Choose
        // exactly one owner so switching either way never restores over it.
        const bool shadingAlias=settings.contains("shading_mode") ||
            (!settings.contains("fullbright") && owned_.count("shading_mode"));
        for(const auto& control:SceneControls()) {
            const bool lightingAlias=std::string(control.cvar)=="mat_fullbright";
            if(lightingAlias && std::string(control.id)!=(shadingAlias?"shading_mode":"fullbright"))continue;
            const bool requested=settings.contains(control.id),wasOwned=lightingAlias?
                (owned_.count("fullbright") || owned_.count("shading_mode")):owned_.count(control.id);
            if(!requested && !wasOwned)continue;
            const auto& current=current_.at(control.id);
            if(!current.available)throw std::runtime_error(std::string(control.id)+" cannot be restored: "+current.reason);
            SceneJson expected;std::string raw;
            if(requested) {
                expected=settings.at(control.id);
                raw=control.boolean?(expected.get<bool>()?"1":"0"):SceneNumberText(expected.get<double>());
                if(Equivalent(current.value,expected,control.boolean))continue;
            } else {
                const auto& baseline=baseline_.at(control.id);expected=baseline.value;raw=baseline.raw;
                if(current.value==expected)continue;
            }
            changes.push_back({control.id,control.cvar,raw,current,expected,!requested});
        }
        // Check all effective focus planes before writing any member of a batch.
        const auto focusPlane=[](const std::string& id){return id=="dof_near_blurry" || id=="dof_near_crisp" || id=="dof_far_crisp" || id=="dof_far_blurry";};
        if(settings.contains("dof_near_blurry") || settings.contains("dof_near_crisp") || settings.contains("dof_far_crisp") || settings.contains("dof_far_blurry") ||
           std::any_of(changes.begin(),changes.end(),[&](const Change& change){return focusPlane(change.id);})) {
            std::vector<double> planes;
            for(const char* id:{"dof_near_blurry","dof_near_crisp","dof_far_crisp","dof_far_blurry"}) {
                const auto& value=current_.at(id);
                if(!value.available)throw std::runtime_error(std::string(id)+" is required to validate the focus planes");
                planes.push_back(settings.contains(id)?settings.at(id).get<double>():
                    Number(owned_.count(id)?baseline_.at(id).value:value.value));
            }
            if(!std::is_sorted(planes.begin(),planes.end()))throw std::invalid_argument("DOF distances require near blur <= near focus <= far focus <= far blur");
        }
        std::size_t attempted=0;
        try {
            for(const auto& change:changes) {
                ++attempted;
                if(!write_ || !write_(change.cvar,change.raw))throw std::runtime_error(change.id+" was rejected by the engine");
                const auto after=ReadOne(SceneControlById(change.id));
                const bool confirmed=after.available && (change.restoring?after.value==change.expected:
                    Equivalent(after.value,change.expected,SceneControlById(change.id).boolean));
                if(!confirmed)throw std::runtime_error(change.id+" engine readback did not confirm the requested value");
            }
        } catch(const std::exception& error) {
            bool rollback=true;
            while(attempted) {
                const auto& change=changes[--attempted];
                bool restored=false;
                try {const bool accepted=write_ && write_(change.cvar,change.before.raw);
                    const auto after=ReadOne(SceneControlById(change.id));restored=accepted && after.available && after.value==change.before.value;
                }catch(...){restored=false;}
                if(!restored)recovery_[change.id]=change.before;
                rollback=restored && rollback;
            }
            Refresh();throw std::runtime_error(std::string(error.what())+(rollback?"; previous scene values restored":"; engine refused complete rollback"));
        }
        owned_.clear();
        for(auto entry=settings.begin();entry!=settings.end();++entry)owned_[entry.key()]=true;
        Refresh();
    }
private:
    void Recover() {
        for(auto it=recovery_.begin();it!=recovery_.end();) {
            bool restored=false;const auto& control=SceneControlById(it->first);
            try {
                const auto before=ReadOne(control);
                if(before.available && before.value==it->second.value)restored=true;
                else if(write_ && write_(control.cvar,it->second.raw)) {
                    const auto after=ReadOne(control);restored=after.available && after.value==it->second.value;
                }
            }catch(...){restored=false;}
            if(restored)it=recovery_.erase(it);else ++it;
        }
        if(!recovery_.empty()){Refresh();throw std::runtime_error("engine refused recovery of an incomplete scene rollback; recovery journal retained");}
    }
    static double Number(const SceneJson& value) {return value.is_boolean()?(value.get<bool>()?1:0):value.get<double>();}
    static bool Equivalent(const SceneJson& actual,const SceneJson& expected,bool boolean) {
        if(boolean)return Number(actual)==Number(expected);
        const double a=Number(actual),b=Number(expected);return std::abs(a-b)<=1e-5*std::max(1.0,std::abs(b));
    }
    SceneCvar ReadOne(const SceneControl& control) const {
        SceneCvar value;
        if(!read_){value.reason="engine cvar adapter is not bound";return value;}
        try{value=read_(control.cvar);}catch(const std::exception& error){value.reason=error.what();value.available=false;}
        if(value.available && (!value.value.is_boolean() && !value.value.is_number())) {
            value.available=false;value.reason="engine cvar is not a supported primitive";
        }
        if(value.available && !std::isfinite(Number(value.value))) {value.available=false;value.reason="engine cvar has a non-finite value";}
        if(value.available && control.boolean && value.nativeType!="bool" && !value.integer) {value.available=false;value.reason="engine cvar type does not support a boolean control";}
        if(value.available && !control.boolean && std::max(control.minimum,value.minimum.value_or(control.minimum))>
            std::min(control.maximum,value.maximum.value_or(control.maximum))) {value.available=false;value.reason="engine cvar bounds do not overlap the supported control range";}
        return value;
    }
    Read read_;Write write_;
    std::map<std::string,SceneCvar> baseline_,current_,recovery_;
    std::map<std::string,bool> owned_;
};

#ifdef CS2_CAMERA_SOURCE2_CVARS
inline SceneCvar ReadSource2SceneCvar(const std::string& name) {
    SceneCvar result;
    if(!g_pCVar){result.reason="VEngineCvar007 is not initialized";return result;}
    const auto found=g_pCVar->FindConVar(name.c_str(),true);
    if(!found.IsValidRef()){result.reason="cvar is not registered in this build";return result;}
    ConVarRefAbstract ref(found);
    if(!ref.IsConVarDataValid() || !ref.IsConVarDataAvailable()){result.reason="cvar data is not available";return result;}
    switch(ref.GetType()) {
        case EConVarType_Bool:result.nativeType="bool";result.value=ref.GetBool();break;
        case EConVarType_Int16:result.nativeType="int16";result.value=ref.GetAs<int16>();result.integer=true;break;
        case EConVarType_UInt16:result.nativeType="uint16";result.value=ref.GetAs<uint16>();result.integer=true;break;
        case EConVarType_Int32:result.nativeType="int32";result.value=ref.GetAs<int32>();result.integer=true;break;
        case EConVarType_UInt32:result.nativeType="uint32";result.value=ref.GetAs<uint32>();result.integer=true;break;
        case EConVarType_Int64:result.nativeType="int64";result.value=ref.GetAs<int64>();result.integer=true;break;
        case EConVarType_UInt64:result.nativeType="uint64";result.value=ref.GetAs<uint64>();result.integer=true;break;
        case EConVarType_Float32:result.nativeType="float32";result.value=ref.GetAs<float32>();break;
        case EConVarType_Float64:result.nativeType="float64";result.value=ref.GetAs<float64>();break;
        default:result.reason="engine cvar type is not numeric/boolean";return result;
    }
    result.raw=result.value.is_boolean()?(result.value.get<bool>()?"1":"0"):
        result.integer?result.value.dump():SceneNumberText(result.value.get<double>());
    if(ref.HasHelpText())result.help=ref.GetHelpText();
    auto readBound=[](CBufferString& text)->std::optional<double> {
        char* end=nullptr;const double number=std::strtod(text.Get(),&end);
        return end && end!=text.Get() && !*end && std::isfinite(number)?std::optional<double>(number):std::nullopt;
    };
    if(ref.HasMin()){CBufferString text;ref.GetMinAsString(text);result.minimum=readBound(text);}
    if(ref.HasMax()){CBufferString text;ref.GetMaxAsString(text);result.maximum=readBound(text);}
    result.available=true;return result;
}
inline bool WriteSource2SceneCvar(const std::string& name,const std::string& value) {
    if(!g_pCVar)return false;
    const auto found=g_pCVar->FindConVar(name.c_str(),true);
    if(!found.IsValidRef())return false;
    ConVarRefAbstract ref(found);
    return ref.IsConVarDataValid() && ref.IsConVarDataAvailable() && ref.SetString(CUtlString(value.c_str()));
}
inline SceneJson Source2SceneRegistry() {
    SceneJson result=SceneJson::array();if(!g_pCVar)return result;
    std::map<unsigned,bool> visited;
    auto next=g_pCVar->FindFirstConVar();
    for(unsigned count=0;count<65536 && next.IsValidRef() && result.size()<128;++count) {
        if(visited.count(next.GetAccessIndex()))break;
        visited[next.GetAccessIndex()]=true;
        ConVarRefAbstract ref(next);
        if(ref.IsConVarDataValid() && ref.IsConVarDataAvailable()) {
            const std::string name=ref.GetName();bool relevant=false;
            for(const char* pattern:{"fog","dof","bokeh","bloom","tonemap","fullbright","colorcorrection","showfps","trueview_show_status","hud_telemetry","show_build_info"})
                if(name.find(pattern)!=std::string::npos)relevant=true;
            if(relevant) {
                const auto value=ReadSource2SceneCvar(name);
                if(value.available)result.push_back({{"name",name},{"native_type",value.nativeType},{"value",value.value},{"help",value.help}});
            }
        }
        next=g_pCVar->FindNextConVar(next);
    }
    return result;
}
#endif
} // namespace cs2_live
