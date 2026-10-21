#pragma once
#include <cstring>
#include "scene_environment.h"
#include "appearance_runtime.h"
#include "capture_cosmetics.h"

namespace cs2_mvp {
class CaptureEnvironment {
public:
    using Json=nlohmann::json;
    using Log=std::function<void(const std::string&)>;
    static constexpr std::size_t MaxPayload=1024*1024;
    void BindScene(cs2_live::SceneEnvironment::Read read,cs2_live::SceneEnvironment::Write write) {
        scene_.Bind(std::move(read),std::move(write));
    }
    void BindAppearance(cs2_live::AppearancePack::Callbacks callbacks) {appearance_.Bind(std::move(callbacks));}
    void BindCosmetics(cs2_live::PlayerCosmetics::Callbacks players,cs2_live::WeaponCosmetics::Callbacks weapons,cs2_live::GloveCosmetics::Callbacks gloves={}) {cosmetics_.Bind(std::move(players),std::move(weapons),std::move(gloves));}
    void Frame(int tick,bool recording,const Log& log) {cosmetics_.Frame(tick,recording,log);}
    void BeforeMovie(int tick,const Log& log) {cosmetics_.BeforeMovie(tick,log);}
    static bool IsAction(const std::string& command) {
        return command.rfind("cs2_mvp_scene",0)==0 || command.rfind("cs2_mvp_appearance",0)==0 || command.rfind("cs2_mvp_cosmetics",0)==0;
    }
    static Json Decode(const std::string& hex) {
        if(hex.empty() || hex.size()>MaxPayload*2 || hex.size()%2)throw std::invalid_argument("environment action requires bounded even-length hex JSON");
        auto digit=[](char value)->unsigned {
            if(value>='0' && value<='9')return unsigned(value-'0');
            if(value>='a' && value<='f')return unsigned(value-'a'+10);
            if(value>='A' && value<='F')return unsigned(value-'A'+10);
            throw std::invalid_argument("environment action contains non-hex input");
        };
        std::string text;text.reserve(hex.size()/2);
        for(std::size_t i=0;i<hex.size();i+=2)text.push_back(char((digit(hex[i])<<4)|digit(hex[i+1])));
        auto value=Json::parse(text);
        if(!value.is_object())throw std::invalid_argument("environment action JSON must be an object");
        return value;
    }
    void Handle(const std::string& command,int tick,const Log& log) {
        constexpr const char* scene="cs2_mvp_scene ";constexpr const char* appearance="cs2_mvp_appearance ";constexpr const char* cosmetics="cs2_mvp_cosmetics ";
        if(command.rfind(scene,0)==0) {
            const auto settings=Decode(command.substr(std::strlen(scene)));
            scene_.Apply(settings);
            Json actual=Json::object();const auto capabilities=scene_.Capabilities();
            for(auto entry=settings.begin();entry!=settings.end();++entry)actual[entry.key()]=capabilities.at(entry.key()).at("value");
            if(log)log("CS2_MVP_SCENE_APPLIED tick="+std::to_string(tick)+" settings="+actual.dump());
        } else if(command.rfind(appearance,0)==0) {
            if(cosmetics_.Active())throw std::invalid_argument("a global appearance pack cannot be combined with per-player cosmetics");
            appearance_.Apply(Decode(command.substr(std::strlen(appearance))));
            if(log)log("CS2_MVP_APPEARANCE_APPLIED tick="+std::to_string(tick)+" state="+appearance_.State().dump());
        } else if(command.rfind(cosmetics,0)==0) {
            if(appearance_.State().value("active",false))throw std::invalid_argument("per-player cosmetics cannot be combined with a global appearance pack");
            cosmetics_.Apply(Decode(command.substr(std::strlen(cosmetics))));
        } else throw std::invalid_argument("unknown internal environment action");
    }
    void Restore(int tick,const std::string& reason,const Log& log) {
        std::string error;
        try {scene_.Apply(Json::object());}catch(const std::exception& exception){error=exception.what();}
        try {appearance_.Reset();}catch(const std::exception& exception){if(!error.empty())error+="; ";error+=exception.what();}
        try {cosmetics_.Restore(tick,log);}catch(const std::exception& exception){if(!error.empty())error+="; ";error+=exception.what();}
        if(!error.empty())throw std::runtime_error(error);
        if(log)log("CS2_MVP_ENVIRONMENT_RESTORED tick="+std::to_string(tick)+" reason="+reason);
    }
private:
    cs2_live::SceneEnvironment scene_;
    cs2_live::AppearancePack appearance_;
    CaptureCosmetics cosmetics_;
};
} // namespace cs2_mvp
