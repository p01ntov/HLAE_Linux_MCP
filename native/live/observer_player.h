#pragma once
#include "team_palette.h"
#include <stdexcept>

namespace cs2_live {
// Current F1 command parser uses controller entity index N, then its generic
// m_hPawn. All selection/readback identity checks retain complete serials.
class ObserverPlayerProbe {
public:
    using Address=ClientEntityProbe::Address;using Read=ClientEntityProbe::Read;using Player=ClientEntityProbe::PlayerIdentity;
    struct Fields {unsigned controllerPawn=0,observerServices=0,observerTarget=0,pawnController=0;};
    static std::optional<ClientEntityProbe::Identity> FromInstance(const Read& read,Address system,Address instance) {
        Address identity=0;std::uint32_t handle=0;
        if(!instance || !ClientEntityProbe::Value(read,instance+0x10,identity) || !identity || !ClientEntityProbe::Value(read,identity+0x10,handle))return {};
        const auto current=ClientEntityProbe::Resolve(read,system,handle);return current && current->instance==instance?current:std::nullopt;
    }
    static std::optional<Player> Target(const Read& read,Address base,Address system,const ClientEntityProbe::Fields& actors,const Fields& fields,const ClientBuildProfile* layout=nullptr) {
        const auto at=[&](Address previous){return base+(layout?layout->Offset(previous):previous);};
        Address local=0,services=0,table=0,type=0,owner=0;std::uint32_t localPawn=0,target=0,controller=0;
        if(!ClientEntityProbe::Value(read,at(0x49083b8),local))return {};
        const auto localController=FromInstance(read,system,local);
        if(!localController || (localController->className!="CCSPlayerController" && localController->className!="C_CSPlayerController") ||
           !ClientEntityProbe::Value(read,local+fields.controllerPawn,localPawn))return {};
        const auto pawn=ClientEntityProbe::Resolve(read,system,localPawn);
        if(!pawn || (pawn->className!="C_CSObserverPawn" && pawn->className!="C_CSPlayerPawn") ||
           !ClientEntityProbe::Value(read,pawn->instance+fields.observerServices,services) || !services ||
           !ClientEntityProbe::Value(read,services,table) || table!=at(0x44c6f58) ||
           !ClientEntityProbe::Value(read,table-8,type) || type!=at(0x44c6368) ||
           !ClientEntityProbe::Value(read,services+0x38,owner) || owner!=pawn->instance ||
           !ClientEntityProbe::Value(read,services+fields.observerTarget,target))return {};
        const auto observed=ClientEntityProbe::Resolve(read,system,target);
        if(!observed || observed->className!="C_CSPlayerPawn" || !ClientEntityProbe::Value(read,observed->instance+fields.pawnController,controller))return {};
        const auto player=ClientEntityProbe::ResolvePlayer(read,system,actors,controller);
        Address afterLocal=0,afterServices=0;std::uint32_t afterTarget=0;
        if(!player || player->pawn.handle!=target || player->pawn.instance!=observed->instance ||
           !ClientEntityProbe::Value(read,at(0x49083b8),afterLocal) || afterLocal!=local ||
           !ClientEntityProbe::Value(read,pawn->instance+fields.observerServices,afterServices) || afterServices!=services ||
           !ClientEntityProbe::Value(read,services+fields.observerTarget,afterTarget) || afterTarget!=target ||
           !ClientEntityProbe::StillCurrent(read,*localController) || !ClientEntityProbe::StillCurrent(read,*pawn))return {};
        return player;
    }
    static Player Find(const Read& read,Address system,const ClientEntityProbe::Fields& actors,const Fields& fields,const std::string& steamId) {
        std::optional<Player> selected;
        const auto snapshot=ClientEntityProbe::Inspect(read,system,actors,128);
        for(const auto& entry:snapshot.at("entries"))if(entry.contains("steam_id") && entry["steam_id"]==steamId) {
            const auto player=ClientEntityProbe::ResolvePlayer(read,system,actors,entry.at("handle").get<std::uint32_t>());
            if(!player || std::to_string(player->steamId)!=steamId || selected)throw std::runtime_error("selected SteamID has no unique current controller/pawn; no observer commands sent");
            std::uint32_t genericPawn=0;const unsigned index=player->controller.handle&0x7fff;
            if(index<1 || index>64 || !ClientEntityProbe::Value(read,player->controller.instance+fields.controllerPawn,genericPawn) || genericPawn!=player->pawn.handle)
                throw std::runtime_error("selected controller does not map to its current player pawn; no observer commands sent");
            selected=player;
        }
        if(!selected)throw std::runtime_error("selected SteamID has no current demo player; no observer commands sent");
        return *selected;
    }
    static nlohmann::json Json(const Player& p){return {{"steam_id",std::to_string(p.steamId)},{"nickname",p.nickname},{"team",p.team},{"pawn_handle",p.pawn.handle},{"controller_handle",p.controller.handle}};}
};
} // namespace cs2_live
