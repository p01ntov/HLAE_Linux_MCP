#pragma once
#include "cosmetics_runtime.h"

namespace cs2_mvp {
// Production capture supports builtin agent models and team color only.
class CaptureCosmetics {
public:
    using Json=nlohmann::json;using Log=std::function<void(const std::string&)>;
    void Bind(cs2_live::PlayerCosmetics::Callbacks players){players_.Bind(std::move(players));}
    void Apply(const Json& settings){
        cs2_live::ValidateSupportedCosmetics(settings);players_.ValidateRequest(settings);
        touched_=true;requested_=settings;confirmed_=false;players_.Apply(settings);
    }
    void Frame(int tick,bool recording,const Log& log){
        if(!touched_)return;
        if(Active() || players_.State().value("owned_overrides",std::size_t{0}))players_.Refresh(true);
        if(recording)RequireReady();
        else if(!confirmed_){try{RequireReady();}catch(const std::exception&){return;}Confirm(tick,log);}
    }
    void BeforeMovie(int tick,const Log& log){if(!touched_)return;Frame(tick,false,log);RequireReady();if(!confirmed_)Confirm(tick,log);}
    void Confirm(int tick,const Log& log){
        const auto encoded=Json{{"settings",requested_},{"players",players_.State()}}.dump();
        if(encoded.size()>1024*1024)throw std::runtime_error("capture cosmetics readback exceeds the bounded proof payload");
        if(log)log("CS2_MVP_COSMETICS_APPLIED tick="+std::to_string(tick)+" proof="+encoded);
        confirmed_=true;
    }
    void Restore(int tick,const Log& log){
        if(!touched_)return;
        players_.Apply(Json::object());
        if(players_.State().value("owned_overrides",std::size_t{0}))throw std::runtime_error("per-player restoration is incomplete; original model/color journal retained");
        // Retain the zero fields for older proof readers; no such managers exist.
        if(log)log("CS2_MVP_COSMETICS_RESTORED tick="+std::to_string(tick)+" players_owned=0 weapons_owned=0 gloves_owned=0");
        touched_=false;confirmed_=false;requested_=Json::object();
    }
    bool RecordingReady()const{return !touched_ || confirmed_;}
    bool Active()const{return !requested_.value("players",Json::array()).empty() || (requested_.contains("team_palette") && requested_["team_palette"].value("enabled",false));}
private:
    cs2_live::PlayerCosmetics players_;Json requested_=Json::object();bool touched_=false,confirmed_=false;
    void RequireReady()const{
        const auto state=players_.State();const bool active=Active() || state.value("owned_overrides",std::size_t{0});
        if(!active)return;
        if(state.value("needs_demo_refresh",false) || state.value("restoration_pending",false))throw std::runtime_error("capture still has an unresolved original agent/color restoration");
        if(state.contains("error"))throw std::runtime_error("capture agent/color snapshot reports an error");
        const auto& rows=state.at("players");
        for(const auto& row:rows)if(row.contains("error"))throw std::runtime_error("capture agent/color actor reports an unconfirmed application or restoration");
        if(requested_.contains("players"))for(const auto& wanted:requested_["players"]){
            const auto found=std::find_if(rows.begin(),rows.end(),[&](const Json& row){return row["steam_id"]==wanted["steam_id"];});
            if(state.value("stale",true) || found==rows.end() || !found->value("agent_applied",false) || found->contains("error"))
                throw std::runtime_error("capture agent readback is missing or mismatched for SteamID "+wanted["steam_id"].get<std::string>());
        }
        if(requested_.contains("team_palette") && requested_["team_palette"].value("enabled",false)){
            if(state.value("stale",true) || rows.empty())throw std::runtime_error("capture palette has no current player snapshot");
            for(const auto& row:rows)if(!row.value("palette_applied",false) || row.contains("error"))throw std::runtime_error("capture palette readback is missing or mismatched for SteamID "+row["steam_id"].get<std::string>());
        }
    }
};
}
