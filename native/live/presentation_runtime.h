#pragma once
#include "presentation.h"
#include "hud_presentation.h"
#include "team_palette.h"
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
    }catch(const std::exception& error){callbacks.viewReason=error.what();}
    return callbacks;
}
} // namespace cs2_live
#endif
