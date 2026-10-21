#pragma once
#include "presentation.h"
#include "hud_presentation.h"
#include "team_palette.h"
#include "observer_player.h"
#ifdef CS2_CAMERA_SOURCE2_CVARS
namespace cs2_live {
inline Presentation::Callbacks Source2PresentationBackend(CreateInterfaceFn factory,void* client,LinuxHudPresentation& hud,
                                                          std::function<void(const std::string&)> execute) {
    Presentation::Callbacks callbacks;
    callbacks.frame=[&hud](bool demo){hud.Frame(demo);};callbacks.gameplay=[&hud](bool value){hud.SetGameplay(value);};
    callbacks.capabilities=[&hud]{return hud.Capabilities();};
    // A failed Panorama preflight still allows the independently validated
    // observer request/readback path and ordinary demo/hidden HUD visibility.
    hud.Install();
    try {
        const auto profile=Source2ClientEntityProbe::ResolveProfile(factory,client);
        if(profile.buildId!="f1e04e3616921a3bb25940eea9b4b6bfd7ccada5")throw std::runtime_error("spectator request mapping requires the verified current client build");
        const auto bytes=[&](std::uintptr_t offset,std::initializer_list<unsigned char> expected) {
            std::vector<unsigned char> actual(expected.size());
            if(!profile.read(profile.base+offset,actual.data(),actual.size()) || !std::equal(actual.begin(),actual.end(),expected.begin()))throw std::runtime_error("spectator command/observer instructions differ from the validated client");
        };
        bytes(0x1907110,{0x55,0x31,0xff,0x48,0x89,0xe5,0xe8,0x65,0xd9,0xd1,0xff,0x48,0x85,0xc0,0x74,0x30,0x48,0x8b,0xb8,0x90,0x12,0x00,0x00});
        bytes(0x17b7bee,{0x48,0x8d,0x35,0x41,0x66,0x35,0xff,0x4c,0x89,0xef,0xe8,0x63,0x66,0x4c,0xff,0x85,0xc0,0x0f,0x85,0x9b,0x00,0x00,0x00});
        bytes(0x17b7d5f,{0x48,0x8b,0x78,0x08,0xe8,0xa8,0x4a,0x4c,0xff,0x48,0x8b,0x13,0x41,0x89,0xc4,0x8d,0x40,0xfe,0x83,0xf8,0x02,0xb8,0x02,0x00,0x00,0x00,0x44,0x0f,0x47,0xe0});
        bytes(0x17b7d93,{0x48,0x8b,0x82,0x38,0x01,0x00,0x00,0x44,0x89,0xe6,0x48,0x89,0xdf});
        if(ClientEntityProbe::Text(profile.read,profile.base+0xb0e236,16)!="spec_mode")throw std::runtime_error("spectator command name does not match the validated handler");
        callbacks.observer=[profile]()->std::optional<unsigned>{const auto mode=reinterpret_cast<unsigned(*)()>(profile.base+0x1907110)();return mode<=5?std::optional<unsigned>(mode):std::nullopt;};
        callbacks.requestObserver=[execute](unsigned mode){if(!execute || (mode!=2 && mode!=4))return false;execute("spec_mode "+std::to_string(mode));return true;};
        callbacks.viewRequestsVerified=true;callbacks.viewReason="exact spec_mode parser and observer readback preflight passed";
        try {
            // The numeric spec_player parser resolves N-1 as client slot, then
            // N as controller entity index and that controller's generic pawn.
            bytes(0x17b7de0,{0x48,0x8d,0x35,0xca,0x52,0x3b,0xff,0x4c,0x89,0xef,0xe8,0x71,0x64,0x4c,0xff,0x85,0xc0,0x0f,0x85,0xd9,0x00,0x00,0x00});
            bytes(0x17b7e2d,{0x49,0x8b,0x84,0x24,0x40,0x04,0x00,0x00,0x48,0x8b,0x78,0x08,0xe8,0xd2,0x49,0x4c,0xff,0x85,0xc0,0x0f,0x8e,0x65,0x02,0x00,0x00,0x8d,0x78,0xff,0xe8,0x82,0x01,0x04,0x00});
            bytes(0x17b7e51,{0x48,0x8b,0x03,0x4c,0x89,0xe6,0x48,0x89,0xdf,0xff,0x90,0x50,0x01,0x00,0x00,0x84,0xc0,0x0f,0x84,0xc0,0xfd,0xff,0xff,0x48,0x8b,0x03,0x4c,0x89,0xe6,0x48,0x89,0xdf,0xff,0x90,0x20,0x01,0x00,0x00});
            bytes(0x17f7fd0,{0x48,0x8d,0x05,0x61,0xfd,0xe8,0x02,0x48,0x8b,0x00,0x48,0x85,0xc0,0x74,0x39,0x85,0xff,0x78,0x35,0x39,0x78,0x10,0x7e,0x30,0x48,0x8d,0x05,0x81,0x4a,0x12,0x03,0x55,0x8d,0x77,0x01,0x48,0x89,0xe5,0x48,0x8b,0x00,0x48,0x89,0xc7,0xe8,0x3f,0x49,0xef,0xff,0x48,0x89,0xc7,0x48,0x85,0xc0,0x74,0x07,0x5d,0xe9,0x31,0xc3,0xe2,0xff});
            bytes(0x1624340,{0x8b,0x8f,0x3c,0x08,0x00,0x00,0x83,0xf9,0xff,0x74,0x45,0x48,0x8b,0x35,0xae,0xec,0x09,0x03,0x48,0x85,0xf6,0x74,0x39,0x83,0xf9,0xfe,0x74,0x34});
            if(ClientEntityProbe::Text(profile.read,profile.base+0xb6d0b1,16)!="spec_player")throw std::runtime_error("spec_player name differs from the verified parser");
            const auto field=[&](const char* cls,const char* name,int size,unsigned expected){const auto actual=Source2ClientEntityProbe::SchemaField(profile.scope,cls,name,size);if(!actual || *actual!=expected)throw std::runtime_error(std::string("observer target schema differs: ")+cls+"."+name);return *actual;};
            const ObserverPlayerProbe::Fields fields={field("CBasePlayerController","m_hPawn",4,0x83c),field("C_BasePlayerPawn","m_pObserverServices",8,0x1290),field("CPlayer_ObserverServices","m_hObserverTarget",4,0x4c),field("C_BasePlayerPawn","m_hController",4,0x1444)};
            if(!profile.fields.steamId || !profile.fields.pawnHandle || !profile.fields.playerName)throw std::runtime_error("player identity schema is incomplete");
            const auto current=[profile]{std::uintptr_t primary=0,command=0;return ClientEntityProbe::Value(profile.read,profile.base+0x4b95610,primary) && primary==profile.system && ClientEntityProbe::Value(profile.read,profile.base+0x491ca70,command) && command==primary;};
            callbacks.selectedPlayer=[profile,fields,current]()->std::optional<Presentation::Json>{if(!current())return {};const auto target=ObserverPlayerProbe::Target(profile.read,profile.base,profile.system,profile.fields,fields);if(!target || !current())return {};return ObserverPlayerProbe::Json(*target);};
            callbacks.selectPlayer=[profile,fields,current,execute](const std::string& steamId){
                if(!execute || !current())throw std::runtime_error("current observer entity system is unavailable; no observer commands sent");
                const auto player=ObserverPlayerProbe::Find(profile.read,profile.system,profile.fields,fields,steamId);
                const auto checked=ClientEntityProbe::ResolvePlayer(profile.read,profile.system,profile.fields,player.controller.handle);
                std::uintptr_t globals=0;int maxClients=0;const unsigned index=player.controller.handle&0x7fff;
                if(!checked || checked->controller.instance!=player.controller.instance || checked->steamId!=player.steamId || checked->pawn.handle!=player.pawn.handle || checked->pawn.instance!=player.pawn.instance || !current() ||
                   !ClientEntityProbe::Value(profile.read,profile.base+0x4687d38,globals) || !globals || !ClientEntityProbe::Value(profile.read,globals+0x10,maxClients) || maxClients<1 || maxClients>64 || index>unsigned(maxClients))throw std::runtime_error("selected player lifetime/client slot changed; no observer commands sent");
                execute("spec_mode 2");execute("spec_player "+std::to_string(index));
            };
            callbacks.selectionVerified=true;callbacks.selectionReason="verified numeric controller selection with current full-handle observer target readback";
        }catch(const std::exception& error){callbacks.selectionReason=error.what();}

    }catch(const std::exception& error){callbacks.viewReason=error.what();callbacks.selectionReason=error.what();}
    return callbacks;
}
} // namespace cs2_live
#endif
