#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cs2_live {
// Host-parsed demo metadata is independent of the authored camera project.
// File validation and JSON ingestion happen only on load, never on a frame.
class DemoTimeline {
public:
    using Json=nlohmann::json;
    static constexpr std::size_t MaxBytes=512*1024;
    void Load(const Json& value,const char* expectedPath) {
        Keys(value,{"version","demo","rounds","players"},"demo timeline");
        if(value.dump().size()>MaxBytes)throw std::invalid_argument("demo timeline exceeds 512 KiB");
        if(Integer(value.at("version"),1,1,"timeline version")!=1)throw std::invalid_argument("unsupported demo timeline version");
        const auto& demo=value.at("demo");
        Keys(demo,{"path","fingerprint","playback_ticks","tick_rate","duration_seconds","authoritative_duration_source"},"timeline demo");
        Text(demo.at("path"),4096,"demo path");
        if(!expectedPath || !*expectedPath)throw std::invalid_argument("owned demo path is unavailable");
        const auto path=demo.at("path").get<std::string>();
        if(!std::filesystem::path(path).is_absolute() || std::filesystem::canonical(path)!=std::filesystem::canonical(expectedPath))
            throw std::invalid_argument("demo timeline does not match the owned demo path");
        const auto& fingerprint=demo.at("fingerprint");
        Keys(fingerprint,{"sha256","bytes","mtime_ns"},"demo fingerprint");
        Text(fingerprint.at("sha256"),64,"demo SHA256");
        const auto sha=fingerprint.at("sha256").get<std::string>();
        if(sha.size()!=64 || !std::all_of(sha.begin(),sha.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}))
            throw std::invalid_argument("demo SHA256 must contain 64 lowercase hex digits");
        const auto bytes=Integer(fingerprint.at("bytes"),1,INT64_MAX,"demo bytes");
        const auto mtime=Integer(fingerprint.at("mtime_ns"),0,INT64_MAX,"demo modification time");
        struct stat info{};
        if(stat(expectedPath,&info) || !S_ISREG(info.st_mode) || info.st_size!=bytes ||
           info.st_mtim.tv_sec<0 || info.st_mtim.tv_sec>INT64_MAX/1000000000 ||
           std::uint64_t(info.st_mtim.tv_sec)*UINT64_C(1000000000)+std::uint64_t(info.st_mtim.tv_nsec)!=std::uint64_t(mtime))
            throw std::invalid_argument("demo timeline fingerprint no longer matches the owned demo file");
        const auto end=Integer(demo.at("playback_ticks"),1,2000000000,"demo playback ticks");
        const double rate=Number(demo.at("tick_rate"),1,1000,"demo tick rate");
        const double duration=Number(demo.at("duration_seconds"),.001,2000000000,"demo duration");
        if(demo.at("authoritative_duration_source")!="DEM_FileInfo" || std::abs(duration-double(end)/rate)>1/rate)
            throw std::invalid_argument("demo duration requires consistent authoritative DEM_FileInfo metadata");
        const auto& rounds=value.at("rounds");
        if(!rounds.is_array() || rounds.size()>1024)throw std::invalid_argument("demo timeline has too many rounds");
        std::int64_t previous=-1;
        for(const auto& round:rounds) {
            Keys(round,{"tick","round","warmup"},"demo round");
            const auto tick=Integer(round.at("tick"),0,end,"round tick");
            if(tick<previous)throw std::invalid_argument("round ticks must be sorted");
            previous=tick;
            if(round.contains("round") && !round["round"].is_null())Integer(round["round"],0,10000,"round number");
            if(round.contains("warmup") && !round["warmup"].is_null() && !round["warmup"].is_boolean())throw std::invalid_argument("round warmup must be boolean or null");
        }
        const auto& players=value.at("players");
        if(!players.is_array() || players.size()>128)throw std::invalid_argument("demo timeline has too many players");
        std::map<std::string,Json> nextPlayers;std::size_t events=0;
        for(const auto& player:players) {
            Keys(player,{"steam_id","name","kills","deaths"},"timeline player");
            Text(player.at("steam_id"),20,"player SteamID");
            const auto id=player["steam_id"].get<std::string>();
            if(id[0]=='0' || !std::all_of(id.begin(),id.end(),[](char c){return c>='0' && c<='9';}))throw std::invalid_argument("player SteamID must be canonical decimal");
            try{std::stoull(id);}catch(...){throw std::invalid_argument("player SteamID exceeds uint64");}
            if(player.contains("name"))Text(player["name"],128,"player name",true);
            for(const char* name:{"kills","deaths"}) {
                const auto& ticks=player.at(name);
                if(!ticks.is_array() || ticks.size()>32768-events)throw std::invalid_argument("demo timeline has too many events");
                events+=ticks.size();previous=-1;
                for(const auto& item:ticks) {
                    const auto tick=Integer(item,0,end,"player event tick");
                    if(tick<previous)throw std::invalid_argument("player event ticks must be sorted");
                    previous=tick;
                }
            }
            if(!nextPlayers.emplace(id,player).second)throw std::invalid_argument("duplicate timeline player SteamID");
        }
        // Commit only after the complete candidate and its file identity pass.
        Json nextDemo=demo,nextRounds=rounds;
        const bool sameRecording=loaded_ && demo_.at("path")==demo.at("path") && demo_.at("fingerprint")==fingerprint;
        if(!sameRecording || !nextPlayers.count(markerPlayer_))markerPlayer_.clear();
        demo_.swap(nextDemo);rounds_.swap(nextRounds);players_.swap(nextPlayers);reason_.clear();loaded_=true;++revision_;
        if(!sameRecording)++recordingRevision_;
    }
    void LoadFile(const char* path,const char* expectedPath,const char* unavailableReason=nullptr) {
        if(!path || !*path) {
            if(unavailableReason && *unavailableReason)reason_=std::string(unavailableReason,strnlen(unavailableReason,512));
            return;
        }
        try {
            if(path[0]!='/')throw std::invalid_argument("demo timeline file path must be absolute");
            int descriptor=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
            if(descriptor<0)throw std::runtime_error("cannot open staged demo timeline");
            std::string contents;
            try {
                struct stat info{};
                if(fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=getuid() || info.st_size<=0 || info.st_size>ssize_t(MaxBytes))
                    throw std::runtime_error("staged demo timeline must be an owned regular file up to 512 KiB");
                char buffer[8192];ssize_t count;
                while((count=read(descriptor,buffer,sizeof(buffer)))>0) {
                    contents.append(buffer,std::size_t(count));
                    if(contents.size()>MaxBytes)throw std::runtime_error("staged demo timeline exceeds 512 KiB");
                }
                if(count<0)throw std::runtime_error("cannot read staged demo timeline");
                close(descriptor);descriptor=-1;
                Load(Json::parse(contents),expectedPath);
            }catch(...){if(descriptor>=0)close(descriptor);throw;}
        }catch(const std::exception& error){reason_=std::string(error.what(),strnlen(error.what(),512));}
    }
    int PlaybackTicks()const{return loaded_?demo_.at("playback_ticks").get<int>():0;}
    std::uint64_t RecordingRevision()const{return recordingRevision_;}
    void SelectPlayer(const Json& id) {
        if(!loaded_)throw std::runtime_error("full demo metadata has not been loaded");
        // This is a metadata-only marker filter, never an entity/observer request.
        // A null filter follows a confirmed observed player when one is available.
        if(id.is_null()){markerPlayer_.clear();return;}
        Text(id,20,"timeline player SteamID");
        const auto text=id.get<std::string>();
        if(text[0]=='0' || !std::all_of(text.begin(),text.end(),[](char c){return c>='0' && c<='9';}))
            throw std::invalid_argument("timeline player SteamID must be canonical decimal");
        try{std::stoull(text);}catch(...){throw std::invalid_argument("timeline player SteamID exceeds uint64");}
        if(!players_.count(text))throw std::invalid_argument("timeline player SteamID is not in this demo roster");
        markerPlayer_=text;
    }
    Json State(bool active,int tick,bool paused,const Json& selected) const {
        Json state={{"available",loaded_ && active},{"version",1},{"metadata_revision",revision_},{"tick",tick},{"paused",paused},
            {"selected_player",nullptr},{"marker_player",nullptr},{"selection_source","none"},{"players",Json::array()}};
        if(!loaded_ || !active) {
            state["reason"]=!active?"local demo playback is not active":reason_.empty()?"full demo metadata has not been loaded":reason_;
            return state;
        }
        state["demo"]=demo_;state["rounds"]=rounds_;
        for(const auto& [id,player]:players_)
            state["players"].push_back({{"steam_id",id},{"name",player.value("name",std::string())}});
        std::string id=markerPlayer_;
        if(!id.empty())state["marker_player"]=id;
        else if(selected.is_object() && selected.contains("steam_id") && selected["steam_id"].is_string())
            id=selected["steam_id"].get<std::string>();
        if(const auto player=players_.find(id);player!=players_.end()) {
            state["selected_player"]=player->second;
            state["selection_source"]=markerPlayer_.empty()?"observer":"timeline";
        }
        return state;
    }
private:
    static void Keys(const Json& value,std::initializer_list<const char*> allowed,const char* name) {
        if(!value.is_object())throw std::invalid_argument(std::string(name)+" must be an object");
        for(auto item=value.begin();item!=value.end();++item)
            if(std::none_of(allowed.begin(),allowed.end(),[&](const char* key){return item.key()==key;}))throw std::invalid_argument(std::string(name)+" contains unsupported field "+item.key());
    }
    static std::int64_t Integer(const Json& value,std::int64_t low,std::int64_t high,const char* name) {
        if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>std::uint64_t(INT64_MAX)))throw std::invalid_argument(std::string(name)+" must be an integer");
        const auto number=value.get<std::int64_t>();
        if(number<low || number>high)throw std::invalid_argument(std::string(name)+" is outside its bounds");
        return number;
    }
    static double Number(const Json& value,double low,double high,const char* name) {
        if(!value.is_number())throw std::invalid_argument(std::string(name)+" must be numeric");
        const auto number=value.get<double>();
        if(!std::isfinite(number) || number<low || number>high)throw std::invalid_argument(std::string(name)+" is outside its finite bounds");
        return number;
    }
    static void Text(const Json& value,std::size_t limit,const char* name,bool empty=false) {
        if(!value.is_string())throw std::invalid_argument(std::string(name)+" must be a string");
        const auto text=value.get<std::string>();
        if((!empty && text.empty()) || text.size()>limit || text.find('\0')!=std::string::npos)throw std::invalid_argument(std::string(name)+" is outside its text bounds");
    }
    bool loaded_=false;std::uint64_t revision_=0,recordingRevision_=0;Json demo_=Json::object(),rounds_=Json::array();std::map<std::string,Json> players_;std::string reason_,markerPlayer_;
};
} // namespace cs2_live
