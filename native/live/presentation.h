#pragma once
#include "scene_environment.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace cs2_live {
inline void ValidatePresentation(const nlohmann::json& value) {
    if(!value.is_object())throw std::invalid_argument("presentation must be an object");
    for(auto item=value.begin();item!=value.end();++item) {
        if(item.key()!="view" && item.key()!="hud")throw std::invalid_argument("presentation only accepts view and hud");
        if(!item.value().is_string())throw std::invalid_argument("presentation choices must be strings");
    }
    const auto view=value.value("view",std::string());const auto hud=value.value("hud",std::string());
    if(!view.empty() && view!="player" && view!="free")throw std::invalid_argument("presentation view must be player or free");
    if(!hud.empty() && hud!="gameplay" && hud!="demo" && hud!="hidden")throw std::invalid_argument("HUD must be gameplay, demo, or hidden");
    if(view=="free" && hud=="gameplay")throw std::invalid_argument("gameplay HUD requires player view; free view supports demo or hidden HUD");
}
class Presentation {
public:
    using Json=nlohmann::json;
    struct Callbacks {
        std::function<void(bool)> frame;
        std::function<std::optional<unsigned>()> observer;
        std::function<void(bool)> gameplay;
        std::function<Json()> capabilities;
        std::function<bool(unsigned)> requestObserver;
        bool viewRequestsVerified=false;
        std::string viewReason="spectator mode command mapping has not been verified for this client";
    };
    void Bind(Callbacks value){callbacks_=std::move(value);}
    void SetSceneCapabilities(const Json& value){gameplayPreset_=GameplayHudPresetCapabilities(value);}
    Json Merge(const Json& partial) const {
        ValidatePresentation(partial);auto value=desired_;
        if(!value.contains("view") && ActualView()!="unknown")value["view"]=ActualView();
        if(!value.contains("hud"))value["hud"]=hudVisible_?"demo":"hidden";
        value.update(partial);if(value.value("hud",std::string())=="gameplay" && !value.contains("view"))value["view"]="player";
        ValidatePresentation(value);return value;
    }
    void ValidateRequest(const Json& value) const {
        ValidatePresentation(value);if(value.empty())return;
        if(value.contains("view")) {
            const auto target=value["view"]=="player"?2u:4u;
            if((!observer_ || *observer_!=target) && (!callbacks_.viewRequestsVerified || !callbacks_.requestObserver))throw std::runtime_error(callbacks_.viewReason);
        }
        if(value.value("hud",std::string())=="gameplay") {
            if(!Adapter().value("installed",false))throw std::runtime_error(Adapter().value("reason",std::string("gameplay HUD native adapter is unavailable")));
            if(!gameplayPreset_.value("available",false))throw std::runtime_error(gameplayPreset_.value("reason",std::string("gameplay spectator cvar preset is unavailable")));
        }
    }
    void Apply(const Json& value) {
        ValidateRequest(value);
        if(value==desired_)return;
        if(desired_.empty() && !value.empty())baseline_=observer_;
        const auto previous=desired_;const auto previousTarget=pendingTarget_;
        try {
            if(callbacks_.gameplay)callbacks_.gameplay(false);
            desired_=value;error_.clear();pendingTarget_.reset();
            std::optional<unsigned> target;
            if(value.contains("view"))target=value["view"]=="player"?2u:4u;
            else if(value.empty() && baseline_ && (*baseline_==2 || *baseline_==4))target=baseline_;
            if(target && (!observer_ || observer_!=target)) {
                if(!callbacks_.viewRequestsVerified || !callbacks_.requestObserver || !callbacks_.requestObserver(*target))throw std::runtime_error(callbacks_.viewReason);
                pendingTarget_=target;requestedAt_=std::chrono::steady_clock::now();
            }
            UpdateGameplay();
        }catch(...){desired_=previous;pendingTarget_=previousTarget;UpdateGameplay();throw;}
    }
    void Frame(bool demo,bool hudVisible) {
        demo_=demo;hudVisible_=hudVisible;
        try {
            if(callbacks_.frame)callbacks_.frame(demo);
            observer_=callbacks_.observer?callbacks_.observer():std::optional<unsigned>{};
            if(!demo){if(callbacks_.gameplay)callbacks_.gameplay(false);return;}
            if(pendingTarget_ && observer_==pendingTarget_){confirmedPlayer_=confirmedPlayer_ || *pendingTarget_==2;confirmedFree_=confirmedFree_ || *pendingTarget_==4;pendingTarget_.reset();error_.clear();}
            if(pendingTarget_ && std::chrono::steady_clock::now()-requestedAt_>std::chrono::seconds(5)) {
                error_="spectator view request did not reach the requested observer mode";pendingTarget_.reset();
            }
            UpdateGameplay();
            ConfirmHud();
        }catch(const std::exception& error){error_=error.what();}
    }
    void Suspend(){if(callbacks_.gameplay)callbacks_.gameplay(false);}
    Json State() const {
        const auto adapter=Adapter();const bool gameplay=adapter.value("gameplay_active",false);
        const auto capability=[](bool requestable,bool available,const std::string& reason){return Json{{"requestable",requestable},{"available",available},{"reason",reason}};};
        const bool view=callbacks_.viewRequestsVerified && bool(callbacks_.requestObserver);
        const bool installed=adapter.value("installed",false),preset=gameplayPreset_.value("available",false);
        const std::string actualHud=!hudVisible_?"hidden":confirmedHud_;
        const bool pending=error_.empty() && (bool(pendingTarget_) || (desired_.contains("hud") && desired_["hud"]!=actualHud) || (!desired_.empty() && desired_.value("hud",std::string())=="gameplay" && !gameplay));
        Json state={{"view",ActualView()},{"hud",actualHud},
            {"requested_view",desired_.value("view",ActualView())},{"requested_hud",desired_.value("hud",!hudVisible_?std::string("hidden"):std::string("demo"))},
            {"observer_mode",observer_?Json(*observer_):Json(nullptr)},{"pending",pending},{"stale",!demo_},{"adapter",adapter},
            {"capabilities",{{"player",capability(view,view && (confirmedPlayer_ || observer_==2),view?"typed spectator request with actual observer-mode readback":callbacks_.viewReason)},
                {"free",capability(view,view && (confirmedFree_ || observer_==4),view?"typed spectator request with actual observer-mode readback":callbacks_.viewReason)},
                {"gameplay",capability(installed && preset,adapter.value("available",false) && preset,!preset?gameplayPreset_.value("reason",std::string("gameplay spectator cvar preset is unavailable")):
                    installed?"scoped native HUD classes and transactional spectator cvar preset; rendered layout requires verification":adapter.value("reason",std::string("gameplay HUD adapter is unavailable")))},
                {"demo",capability(true,true,"native demo HUD")},{"hidden",capability(true,true,"transactional cl_drawhud visibility")}}}};
        if(!error_.empty())state["error"]=error_;
        return state;
    }
private:
    Callbacks callbacks_;Json desired_=Json::object(),gameplayPreset_={{"available",false},{"confirmed",false},{"reason","gameplay spectator cvar capabilities are not initialized"}};std::optional<unsigned> observer_,baseline_,pendingTarget_;
    bool demo_=false,hudVisible_=true,confirmedPlayer_=false,confirmedFree_=false;std::string error_,confirmedHud_="demo";std::chrono::steady_clock::time_point requestedAt_{};
    Json Adapter()const {
        auto value=callbacks_.capabilities?callbacks_.capabilities():Json{{"installed",false},{"reason","gameplay HUD adapter is unavailable"}};
        value["panorama_available"]=value.value("available",false);value["gameplay_preset"]=gameplayPreset_;
        const bool requested=demo_ && desired_.value("hud",std::string())=="gameplay";
        value["available"]=value.value("available",false) && (!requested || (gameplayPreset_.value("available",false) && gameplayPreset_.value("confirmed",false)));
        value["gameplay_active"]=value.value("gameplay_active",false) && gameplayPreset_.value("confirmed",false);
        return value;
    }
    std::string ActualView() const {return observer_ && *observer_==2?"player":observer_ && *observer_==4?"free":"unknown";}
    void UpdateGameplay(){if(callbacks_.gameplay)callbacks_.gameplay(demo_ && !desired_.empty() && desired_.value("hud",std::string())=="gameplay" && observer_ && *observer_==2 && !pendingTarget_ && gameplayPreset_.value("confirmed",false));}
    void ConfirmHud(){const auto adapter=Adapter();if(adapter.value("available",false))confirmedHud_=adapter.value("gameplay_active",false)?"gameplay":"demo";else if(!adapter.value("installed",false))confirmedHud_="demo";}
};
} // namespace cs2_live
