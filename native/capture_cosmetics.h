#pragma once
#include "cosmetics_runtime.h"
#include "weapon_cosmetics.h"
#include "glove_cosmetics.h"

namespace cs2_mvp {
// Uses the same lifetime-checked owners as the live editor. Recording is
// permitted only after observed models, colors and texture attributes agree.
class CaptureCosmetics {
public:
    using Json=nlohmann::json;
    using Log=std::function<void(const std::string&)>;
    void Bind(cs2_live::PlayerCosmetics::Callbacks players,cs2_live::WeaponCosmetics::Callbacks weapons,cs2_live::GloveCosmetics::Callbacks gloves={}) {
        players_.Bind(std::move(players));weapons_.Bind(std::move(weapons));gloves_.Bind(std::move(gloves));
    }
    void Apply(const Json& settings) {
        cs2_live::ValidateCosmetics(settings);
        const auto paints=settings.value("weapon_paints",Json::array());
        // Validate every desired/catalog value before the first mutation.
        // Preroll may precede actor/HUD creation; guarded owners wait for
        // their current endpoints, and BeforeMovie requires full readback.
        const auto gloves=settings.value("gloves",Json::array());
        players_.ValidateRequest(settings);weapons_.ValidateRequest(paints);gloves_.ValidateRequest(gloves);
        touched_=true;requested_=settings;confirmed_=false;gloves_.Context(true,0,false);
        players_.Apply(settings);weapons_.Apply(paints);gloves_.Apply(gloves);
    }
    void Frame(int tick,bool recording,const Log& log) {
        if(!touched_)return;
        gloves_.Context(true,tick,false);
        if(BodyRequested() || players_.State().value("owned_overrides",std::size_t{0}))players_.Refresh(true);
        if(!requested_.value("weapon_paints",Json::array()).empty() || weapons_.State().value("owned_overrides",std::size_t{0}))weapons_.Refresh(true);
        if(!requested_.value("gloves",Json::array()).empty() || gloves_.State().value("owned_overrides",std::size_t{0}))gloves_.Refresh(true);
        if(recording)RequireReady();
        else if(!confirmed_) {
            // During warmup a freshly recreated HUD or material job may still
            // be pending. No success marker exists until every target agrees.
            try{RequireReady();}catch(const std::exception&){return;}
            Confirm(tick,log);
        }
    }
    void BeforeMovie(int tick,const Log& log) {
        if(!touched_)return;
        Frame(tick,false,log);RequireReady();
        if(!confirmed_)Confirm(tick,log);
    }
    void Confirm(int tick,const Log& log) {
        Json value={{"settings",requested_},{"players",players_.State()},{"weapon_cosmetics",weapons_.State()},{"glove_cosmetics",gloves_.State()}};
        const auto encoded=value.dump();
        if(encoded.size()>1024*1024)throw std::runtime_error("capture cosmetics readback exceeds the bounded proof payload");
        if(log && !requested_.value("gloves",Json::array()).empty())log("CS2_MVP_GLOVES_POSE_ABI 1");
        if(log && !requested_.value("gloves",Json::array()).empty())log("CS2_MVP_GLOVES_RENDER_ABI 1");
        if(log)log("CS2_MVP_COSMETICS_APPLIED tick="+std::to_string(tick)+" proof="+encoded);
        confirmed_=true;
    }
    void Restore(int tick,const Log& log) {
        if(!touched_)return;
        std::string error;
        try{gloves_.Apply(Json::array());}catch(const std::exception& e){error=e.what();}
        try{players_.Apply(Json::object());}catch(const std::exception& e){if(!error.empty())error+="; ";error+=e.what();}
        try{weapons_.Apply(Json::array());}catch(const std::exception& e){if(!error.empty())error+="; ";error+=e.what();}
        const auto players=players_.State(),weapons=weapons_.State(),gloves=gloves_.State();
        if(players.value("owned_overrides",std::size_t{0}) || weapons.value("owned_overrides",std::size_t{0}) || gloves.value("owned_overrides",std::size_t{0})) {
            if(!error.empty())error+="; ";
            error+="per-player restoration is incomplete; original model/attribute journal retained";
        }
        if(!error.empty())throw std::runtime_error(error);
        if(log)log("CS2_MVP_COSMETICS_RESTORED tick="+std::to_string(tick)+" players_owned=0 weapons_owned=0 gloves_owned=0");
        touched_=false;confirmed_=false;requested_=Json::object();
    }
    bool RecordingReady()const{return !touched_ || confirmed_;}
    bool Active()const{return !requested_.value("players",Json::array()).empty() || !requested_.value("weapon_paints",Json::array()).empty() || !requested_.value("gloves",Json::array()).empty() ||
        (requested_.contains("team_palette") && requested_["team_palette"].value("enabled",false));}
private:
    cs2_live::PlayerCosmetics players_;
    cs2_live::WeaponCosmetics weapons_;
    cs2_live::GloveCosmetics gloves_;
    Json requested_=Json::object();bool touched_=false,confirmed_=false;
    bool BodyRequested()const{return !requested_.value("players",Json::array()).empty() || (requested_.contains("team_palette") && requested_["team_palette"].value("enabled",false));}
    void RequireReady() const {
        const auto players=players_.State(),weapons=weapons_.State();
        const auto gloves=gloves_.State();
        const bool bodyActive=BodyRequested() || players.value("owned_overrides",std::size_t{0});
        const bool weaponActive=!requested_.value("weapon_paints",Json::array()).empty() || weapons.value("owned_overrides",std::size_t{0});
        const bool gloveActive=!requested_.value("gloves",Json::array()).empty() || gloves.value("owned_overrides",std::size_t{0});
        if((bodyActive && (players.value("needs_demo_refresh",false) || players.value("restoration_pending",false))) ||
           (gloveActive && (gloves.value("restoration_pending",false) || gloves.value("restoration_unresolved",false) || gloves.value("needs_demo_refresh",false))) ||
           (weaponActive && !weapons.value("restore_pending",Json::array()).empty()))throw std::runtime_error("capture still has an unresolved original cosmetics restoration");
        if((bodyActive && players.contains("error")) || (weaponActive && weapons.contains("error")) || (gloveActive && gloves.contains("error")))throw std::runtime_error("capture cosmetics snapshot reports an error");
        const auto actorRows=players.at("players"),weaponRows=weapons.at("weapons");
        const std::array<Json,3> allRows={actorRows,weaponRows,gloves.at("gloves")};const std::array<bool,3> active={bodyActive,weaponActive,gloveActive};
        for(unsigned index=0;index<allRows.size();++index)if(active[index])for(const auto& row:allRows[index])if(row.contains("error"))throw std::runtime_error("capture cosmetics actor reports an unconfirmed application or restoration");
        if(requested_.contains("players"))for(const auto& wanted:requested_["players"]) {
            const auto found=std::find_if(actorRows.begin(),actorRows.end(),[&](const Json& row){return row["steam_id"]==wanted["steam_id"];});
            if(players.value("stale",true) || found==actorRows.end() || !found->value("agent_applied",false) || found->contains("error"))
                throw std::runtime_error("capture agent readback is missing or mismatched for SteamID "+wanted["steam_id"].get<std::string>());
        }
        if(requested_.contains("team_palette") && requested_["team_palette"].value("enabled",false)) {
            if(players.value("stale",true) || actorRows.empty())throw std::runtime_error("capture palette has no current player snapshot");
            for(const auto& row:actorRows)if(!row.value("palette_applied",false) || row.contains("error"))
                throw std::runtime_error("capture palette readback is missing or mismatched for SteamID "+row["steam_id"].get<std::string>());
        }
        if(requested_.contains("weapon_paints"))for(const auto& wanted:requested_["weapon_paints"]) {
            bool found=false;
            for(const auto& row:weaponRows)if(row["steam_id"]==wanted["steam_id"] && row["item_def"]==wanted["item_def"]) {
                found=true;
                if(!row.value("ready",false) || !row.value("applied",false) || row.value("materials_pending",true) || row.contains("error"))
                    throw std::runtime_error("capture owned weapon paint readback is missing or mismatched for SteamID "+wanted["steam_id"].get<std::string>());
            }
            if(!found || weapons.value("stale",true))throw std::runtime_error("capture selected player no longer owns the requested painted item");
        }
        if(requested_.contains("gloves")) {
            const auto state=gloves_.State();const auto& rows=state.at("gloves");
            for(const auto& wanted:requested_["gloves"]) {
                const auto found=std::find_if(rows.begin(),rows.end(),[&](const Json& row){return row["steam_id"]==wanted["steam_id"];});
                if(state.value("stale",true) || found==rows.end() || !found->value("ready",false) || !found->value("applied",false) || !found->value("world_pose_ready",false) || !found->value("hud_pose_ready",false) || !found->value("world_render_ready",false) || !found->contains("hud_render_required") || !(*found)["hud_render_required"].is_boolean() || !found->contains("hud_parent_render_enabled") || !(*found)["hud_parent_render_enabled"].is_boolean() || (*found)["hud_render_required"]!=(*found)["hud_parent_render_enabled"] || (found->value("hud_render_required",true) && !found->value("hud_render_ready",false)) || found->value("materials_pending",true) || found->contains("error"))
                    throw std::runtime_error("capture glove item/material/pose readback is missing or mismatched for SteamID "+wanted["steam_id"].get<std::string>());
            }
        }
    }
};
} // namespace cs2_mvp
