#pragma once
#include "cosmetics_catalog.h"
#include <array>
#include <cmath>
#include <functional>
#include <set>

namespace cs2_live {
inline void CosmeticsKeys(const nlohmann::json& value,std::initializer_list<const char*> allowed,const char* label) {
    if(!value.is_object())throw std::invalid_argument(std::string(label)+" must be an object");
    for(auto item=value.begin();item!=value.end();++item) {
        bool found=false;for(const char* name:allowed)if(item.key()==name)found=true;
        if(!found)throw std::invalid_argument(std::string(label)+" contains an unsupported field: "+item.key());
    }
}
inline void ValidateSteamId(const nlohmann::json& value) {
    if(!value.is_string())throw std::invalid_argument("steam_id must be a decimal string");
    const auto text=value.get<std::string>();
    if(text.empty() || text.size()>20 || text=="0" || text.front()=='0' ||
       std::any_of(text.begin(),text.end(),[](char c){return c<'0' || c>'9';}))
        throw std::invalid_argument("steam_id must be a nonzero decimal string without aliases");
    try{std::size_t end=0;std::stoull(text,&end);if(end!=text.size())throw std::invalid_argument("steam_id");}
    catch(...){throw std::invalid_argument("steam_id exceeds uint64");}
}
inline void ValidateWeaponPaints(const nlohmann::json& value) {
    if(!value.is_array() || value.size()>512)throw std::invalid_argument("weapon_paints must contain at most 512 overrides");
    std::set<std::pair<std::string,unsigned>> unique;
    for(const auto& entry:value) {
        CosmeticsKeys(entry,{"steam_id","nickname","item_def","paint_kit","wear","seed"},"weapon paint");
        ValidateSteamId(entry.at("steam_id"));
        if(entry.contains("nickname") && (!entry["nickname"].is_string() || entry["nickname"].get<std::string>().size()>128 || entry["nickname"].get<std::string>().find('\0')!=std::string::npos))
            throw std::invalid_argument("nickname is a display string up to 128 bytes");
        for(const char* name:{"item_def","paint_kit"})if(!entry.contains(name) || !entry[name].is_number_integer() || entry[name].get<long long>()<1 || entry[name].get<long long>()>65535)
            throw std::invalid_argument(std::string(name)+" must be an integer in 1..65535");
        if(entry.contains("seed") && (!entry["seed"].is_number_integer() || entry["seed"].get<long long>()<0 || entry["seed"].get<long long>()>1000))throw std::invalid_argument("seed must be an integer in 0..1000");
        if(entry.contains("wear") && (!entry["wear"].is_number() || !std::isfinite(entry["wear"].get<double>()) || entry["wear"].get<double>()<0 || entry["wear"].get<double>()>1))throw std::invalid_argument("wear must be finite and in 0..1");
        if(!unique.emplace(entry["steam_id"].get<std::string>(),entry["item_def"].get<unsigned>()).second)throw std::invalid_argument("duplicate player/item weapon paint");
    }
}
inline void ValidateGloves(const nlohmann::json& value) {
    try{ValidateWeaponPaints(value);}catch(const std::exception& error){throw std::invalid_argument(std::string("gloves: ")+error.what());}
    if(value.size()>128)throw std::invalid_argument("gloves must contain at most 128 per-player overrides");
    std::set<std::string> unique;
    for(const auto& entry:value)if(!unique.insert(entry["steam_id"].get<std::string>()).second)throw std::invalid_argument("duplicate glove override for SteamID");
}
inline void ValidateCosmetics(const nlohmann::json& value) {
    CosmeticsKeys(value,{"players","team_palette","weapon_paints","gloves"},"cosmetics");
    if(value.contains("weapon_paints"))ValidateWeaponPaints(value["weapon_paints"]);
    if(value.contains("gloves"))ValidateGloves(value["gloves"]);
    if(value.contains("players")) {
        if(!value["players"].is_array() || value["players"].size()>128)throw std::invalid_argument("cosmetics players must contain at most 128 overrides");
        std::set<std::string> unique;
        for(const auto& player:value["players"]) {
            CosmeticsKeys(player,{"steam_id","nickname","agent_item_def"},"player cosmetics");
            ValidateSteamId(player.at("steam_id"));
            if(!unique.insert(player["steam_id"].get<std::string>()).second)throw std::invalid_argument("duplicate player cosmetics SteamID");
            if(player.contains("nickname") && (!player["nickname"].is_string() || player["nickname"].get<std::string>().size()>128 || player["nickname"].get<std::string>().find('\0')!=std::string::npos))
                throw std::invalid_argument("nickname is a display string up to 128 bytes");
            const auto& item=player.at("agent_item_def");
            if(!item.is_number_integer() || item.get<long long>()<1 || item.get<long long>()>65535)throw std::invalid_argument("agent_item_def must be an integer in 1..65535");
        }
    }
    if(value.contains("team_palette")) {
        const auto& palette=value["team_palette"];CosmeticsKeys(palette,{"enabled","t","ct"},"team_palette");
        if(!palette.contains("enabled") || !palette["enabled"].is_boolean())throw std::invalid_argument("team_palette requires enabled boolean");
        for(const char* team:{"t","ct"}) {
            if(!palette.contains(team) || !palette[team].is_array() || palette[team].size()!=3)throw std::invalid_argument("team_palette requires T and CT RGB triples");
            for(const auto& channel:palette[team])if(!channel.is_number_integer() || channel.get<long long>()<0 || channel.get<long long>()>255)throw std::invalid_argument("palette RGB channels must be integers in 0..255");
        }
    }
}

class PlayerCosmetics {
public:
    using Json=nlohmann::json;
    using Color=std::array<unsigned char,4>;
    struct Actor {
        std::string steamId,nickname,model;
        unsigned team=0;std::uint32_t pawnHandle=0,controllerHandle=0;
        std::uintptr_t instance=0;
        std::uint64_t generation=0;
        Color color{};
        bool modelReady=false,colorReady=false;
    };
    struct Callbacks {
        std::function<std::vector<Actor>()> actors;
        std::function<bool(const Actor&)> current;
        std::function<bool(const Actor&,const std::string&)> model;
        std::function<bool(const Actor&,const Color&)> color;
        CosmeticsCatalog catalog;
        bool agents=false,palette=false;
        std::string agentReason="verified client model adapter is unavailable",paletteReason="verified client color adapter is unavailable";
    };
    void Bind(Callbacks value) {if(!owned_.empty())throw std::logic_error("restore player cosmetics before rebinding");callbacks_=std::move(value);}
    bool IsMapDefault(unsigned itemDef)const{return callbacks_.catalog.GetAgent(itemDef).mapBased;}
    void ValidateRequest(const Json& value,bool requireCurrent=false) {
        ValidateCosmetics(value);
        if(value.contains("players") && !value["players"].empty()) {
            if(!callbacks_.agents)throw std::runtime_error(callbacks_.agentReason);
            const auto actors=callbacks_.actors?callbacks_.actors():std::vector<Actor>{};
            for(const auto& entry:value["players"]) {
                const auto& selected=callbacks_.catalog.GetAgent(entry["agent_item_def"].get<unsigned>());
                const auto found=std::find_if(actors.begin(),actors.end(),[&](const Actor& actor){return actor.steamId==entry["steam_id"];});
                if(found==actors.end()) {if(requireCurrent)throw std::runtime_error("selected SteamID has no current demo pawn; no changes applied");continue;}
                if(requireCurrent && (found->team!=selected.team || !callbacks_.current || !callbacks_.current(*found)))
                    throw std::runtime_error("selected agent does not match the player's current T/CT team or pawn lifetime; no changes applied");
                if(requireCurrent && !found->modelReady)throw std::runtime_error("the current pawn has no validated original model binding; no changes applied");
                if(!selected.mapBased && selected.model.empty())throw std::runtime_error("selected builtin agent has no installed model resource");
                if(!selected.mapBased && !selected.compiledModelPresent)throw std::runtime_error("selected builtin agent compiled resource is absent from the installed game index");
            }
        }
        if(value.contains("team_palette") && value["team_palette"]["enabled"].get<bool>() && !callbacks_.palette)throw std::runtime_error(callbacks_.paletteReason);
    }
    void Apply(const Json& value) {
        ValidateRequest(value);
        if(desired_!=value)++desiredRevision_;
        desired_=value;
        Refresh(true);
    }
    void Refresh(bool active) {
        error_.clear();refreshRequired_=false;actorErrors_.clear();refreshMutated_=false;
        std::vector<Actor> actors;
        try{if(callbacks_.actors)actors=callbacks_.actors();}catch(const std::exception& e){error_=e.what();snapshotValid_=false;return;}
        snapshotValid_=!actors.empty();
        if(!actors.empty())generation_=actors.front().generation;
        std::map<std::uint32_t,Actor> live;
        for(const auto& actor:actors)if(actor.team==2 || actor.team==3)live.emplace(actor.pawnHandle,actor);
        // Restore only owned effects while the full handle, entity instance,
        // SteamID and controller still identify this actor. A recycled handle
        // is forgotten without dereferencing its former pointer.
        for(auto it=owned_.begin();it!=owned_.end();) {
            auto current=live.find(it->first);
            if(current==live.end()) {
                const auto replacement=std::find_if(actors.begin(),actors.end(),[&](const Actor& value){return value.steamId==it->second.actor.steamId || value.generation!=it->second.actor.generation;});
                if(replacement!=actors.end())it=owned_.erase(it);else ++it;
                continue; // A transiently missing snapshot is not proof of destruction.
            }
            if(!Same(it->second.actor,current->second)) {it=owned_.erase(it);continue;}
            if(!Current(current->second)){++it;continue;}
            auto& owned=it->second;const auto& actor=current->second;
            if(owned.model && owned.actor.team!=actor.team && actor.model==owned.lastModel) {
                refreshRequired_=true;actorErrors_[actor.steamId]="team changed while the previous agent is still bound; canonical demo refresh is required";++it;continue;
            }
            const auto* wanted=active?Override(actor.steamId):nullptr;
            const CosmeticsCatalog::Agent* agent=wanted?&callbacks_.catalog.GetAgent((*wanted)["agent_item_def"].get<unsigned>()):nullptr;
            const bool keepModel=agent && !agent->mapBased && agent->team==actor.team && owned.actor.team==actor.team;
            const bool keepColor=active && PaletteEnabled() && owned.actor.team==actor.team;
            if(owned.model && !keepModel) {
                if(!actor.modelReady || actor.model.empty()) {
                    actorErrors_[actor.steamId]="original agent restoration is pending a readable current model binding";++it;continue;
                }
                // A network update may already have installed the new team's
                // model. Restore only if our last model still owns this field.
                if(actor.model==owned.lastModel) {
                    const auto attempt=std::to_string(desiredRevision_)+":"+actor.model+":"+owned.originalModel;
                    if(owned.modelRestoreAttempt==attempt){actorErrors_[actor.steamId]="original agent restoration was rejected; reset/reselect is required";++it;continue;}
                    owned.modelRestoreAttempt=attempt;
                    if(!SetModel(actor,owned.originalModel)) {++it;continue;}
                }
                owned.model=false;
            }
            if(owned.color && !keepColor) {
                if(!actor.colorReady) {
                    actorErrors_[actor.steamId]="original palette restoration is pending a readable current color";++it;continue;
                }
                if(actor.color==owned.lastColor) {
                    const auto attempt=std::to_string(desiredRevision_)+":"+ColorKey(actor.color)+":"+ColorKey(owned.originalColor);
                    if(owned.colorRestoreAttempt==attempt){actorErrors_[actor.steamId]="original palette restoration was rejected; reset/reselect is required";++it;continue;}
                    owned.colorRestoreAttempt=attempt;
                    if(!SetColor(actor,owned.originalColor)) {++it;continue;}
                }
                owned.color=false;
            }
            if(!owned.model && !owned.color)it=owned_.erase(it);else ++it;
        }
        if(refreshMutated_) {
            try{if(callbacks_.actors)actors=callbacks_.actors();}catch(const std::exception& e){error_=e.what();snapshotValid_=false;return;}
            refreshMutated_=false;
        }
        for(auto actor:actors) {
            if(!active || (actor.team!=2 && actor.team!=3) || !Current(actor))continue;
            const auto pending=owned_.find(actor.pawnHandle);
            if(pending!=owned_.end() && pending->second.model && pending->second.actor.team!=actor.team && actor.model==pending->second.lastModel)continue;
            const auto* wanted=Override(actor.steamId);
            const CosmeticsCatalog::Agent* agent=wanted?&callbacks_.catalog.GetAgent((*wanted)["agent_item_def"].get<unsigned>()):nullptr;
            if(agent && agent->team!=actor.team) {actorErrors_[actor.steamId]="agent skipped because the player's current team differs";agent=nullptr;}
            if(agent && !agent->mapBased && actor.modelReady && actor.model!=agent->model) {
                auto& owned=Own(actor);
                if(!owned.model){owned.originalModel=actor.model;owned.model=true;}
                else if(actor.model!=owned.lastModel)owned.originalModel=actor.model; // Fresh network/map baseline after a seek.
                const auto attempt=std::to_string(desiredRevision_)+":"+actor.model+":"+agent->model;
                if(owned.modelAttempt!=attempt) {
                    owned.modelAttempt=attempt;owned.lastModel=agent->model;
                    owned.modelError=!SetModel(actor,agent->model);
                }else owned.modelError=true;
                if(owned.modelError)actorErrors_[actor.steamId]="client model setter did not confirm the selected binding; reset/reselect or wait for a new network model before retrying";
            }else if(agent && !agent->mapBased && actor.modelReady && actor.model==agent->model) {
                const auto found=owned_.find(actor.pawnHandle);if(found!=owned_.end())found->second.modelError=false;
            }
            if(PaletteEnabled() && actor.colorReady) {
                Color color{255,255,255,255};const auto& rgb=desired_["team_palette"][actor.team==2?"t":"ct"];
                for(unsigned i=0;i<3;++i)color[i]=rgb[i].get<unsigned>();
                if(actor.color!=color) {
                    auto& owned=Own(actor);if(!owned.color){owned.originalColor=actor.color;owned.color=true;}
                    else if(actor.color!=owned.lastColor)owned.originalColor=actor.color;
                    const auto attempt=std::to_string(desiredRevision_)+":"+ColorKey(actor.color)+":"+ColorKey(color);
                    if(owned.colorAttempt!=attempt) {owned.colorAttempt=attempt;owned.lastColor=color;owned.colorError=!SetColor(actor,color);}
                    if(owned.colorError)actorErrors_[actor.steamId]="client color setter did not confirm the palette; reset/reselect or wait for a new network color before retrying";
                }
            }
        }
        if(refreshMutated_) {
            try{if(callbacks_.actors)actors=callbacks_.actors();}catch(const std::exception& e){error_=e.what();snapshotValid_=false;return;}
        }
        players_=Json::array();
        for(const auto& actor:actors) {
            Json player={{"steam_id",actor.steamId},{"nickname",actor.nickname},{"team",actor.team},{"pawn_handle",actor.pawnHandle},
                {"current_model",actor.model},{"model_ready",actor.modelReady},{"color_ready",actor.colorReady},{"applied",false}};
            if(actor.colorReady)player["current_color"]=actor.color;
            if(const auto* wanted=Override(actor.steamId))player["agent_item_def"]=(*wanted)["agent_item_def"];
            bool agentApplied=false,paletteApplied=false;
            if(const auto* wanted=Override(actor.steamId)) {
                const auto& agent=callbacks_.catalog.GetAgent((*wanted)["agent_item_def"].get<unsigned>());
                const auto owned=owned_.find(actor.pawnHandle);
                agentApplied=actor.modelReady && agent.team==actor.team && (agent.mapBased?(owned==owned_.end() || !owned->second.model):actor.model==agent.model);
            }
            if(active && PaletteEnabled() && actor.colorReady) {
                const auto& rgb=desired_["team_palette"][actor.team==2?"t":"ct"];
                paletteApplied=actor.color[3]==255;for(unsigned i=0;i<3;++i)paletteApplied=paletteApplied && actor.color[i]==rgb[i].get<unsigned>();
            }
            player["agent_applied"]=active && agentApplied;player["palette_applied"]=paletteApplied;player["applied"]=(active && agentApplied) || paletteApplied;
            if(actorErrors_.count(actor.steamId))player["error"]=actorErrors_[actor.steamId];
            players_.push_back(std::move(player));
        }
    }
    Json State() const {Json result={{"players",players_},{"capabilities",Capabilities()},{"catalog_available",callbacks_.catalog.Size()>0},
        {"team_palette",desired_.value("team_palette",Json{{"enabled",false},{"t",{255,64,64}},{"ct",{64,128,255}}})},{"needs_demo_refresh",refreshRequired_},{"runtime_generation",generation_},{"owned_overrides",owned_.size()},{"restoration_pending",RestorationPending()},{"stale",!snapshotValid_}};if(!error_.empty())result["error"]=error_;return result;}
    Json Capabilities() const {
        const auto capability=[](bool available,const std::string& reason){return Json{{"available",available},{"reason",available?"strict client preflight passed; rendered result requires verification":reason},{"render_verified_1p",false},{"render_verified_3p",false}};};
        return {{"agents",capability(callbacks_.agents,callbacks_.agentReason)},{"team_palette",capability(callbacks_.palette,callbacks_.paletteReason)},
            {"weapons",capability(false,"current client weapon/paint adapter has not been verified")},{"knives",capability(false,"current client knife/animation adapter has not been verified")},
            {"gloves",capability(false,"current client glove adapter has not been verified")},{"custom_import",capability(false,"per-player compiled custom model import has not been verified")}};
    }
private:
    struct Owned {Actor actor;bool model=false,color=false,modelError=false,colorError=false;std::string originalModel,lastModel,modelAttempt,colorAttempt,modelRestoreAttempt,colorRestoreAttempt;Color originalColor{},lastColor{};};
    Callbacks callbacks_;Json desired_=Json::object(),players_=Json::array();std::map<std::uint32_t,Owned> owned_;std::map<std::string,std::string> actorErrors_;std::string error_;bool refreshRequired_=false,snapshotValid_=false,refreshMutated_=false;std::uint64_t generation_=0,desiredRevision_=0;
    static std::string ColorKey(const Color& color){std::string result;for(const auto channel:color)result+=std::to_string(channel)+",";return result;}
    static bool Same(const Actor& a,const Actor& b){return a.instance==b.instance && a.pawnHandle==b.pawnHandle && a.controllerHandle==b.controllerHandle && a.steamId==b.steamId && a.generation==b.generation;}
    bool Current(const Actor& actor)const{return callbacks_.current && callbacks_.current(actor);}
    bool PaletteEnabled()const{return desired_.contains("team_palette") && desired_["team_palette"].value("enabled",false);}
    bool RestorationPending()const {
        for(const auto& entry:owned_) {
            const auto& owned=entry.second;const auto* wanted=Override(owned.actor.steamId);
            if(owned.color && !PaletteEnabled())return true;
            if(owned.model && (!wanted || callbacks_.catalog.GetAgent((*wanted)["agent_item_def"].get<unsigned>()).mapBased ||
                callbacks_.catalog.GetAgent((*wanted)["agent_item_def"].get<unsigned>()).team!=owned.actor.team))return true;
        }
        return false;
    }
    const Json* Override(const std::string& id)const{if(desired_.contains("players"))for(const auto& item:desired_["players"])if(item["steam_id"]==id)return &item;return nullptr;}
    Owned& Own(const Actor& actor){auto& owned=owned_[actor.pawnHandle];if(!owned.actor.instance)owned.actor=actor;return owned;}
    bool SetModel(const Actor& actor,const std::string& model){if(Current(actor) && callbacks_.model){refreshMutated_=true;if(callbacks_.model(actor,model))return true;}actorErrors_[actor.steamId]="client model setter rejected the current actor";return false;}
    bool SetColor(const Actor& actor,const Color& color){if(Current(actor) && callbacks_.color){refreshMutated_=true;if(callbacks_.color(actor,color))return true;}actorErrors_[actor.steamId]="client color setter rejected the current actor";return false;}
};
} // namespace cs2_live
