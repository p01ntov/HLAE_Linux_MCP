#pragma once

// A builtin catalogue is an allowlist of resources, not a cosmetic adapter.
// Applying an agent additionally requires a verified live pawn and its current
// team. Custom assets use a separate catalogue/import flow.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cs2_live {
class CosmeticsCatalog {
public:
    using Json=nlohmann::json;
    struct Agent {
        unsigned itemDef=0,team=0;
        std::string name,displayName,model;
        bool mapBased=false;
        bool compiledModelPresent=true;
    };
    static constexpr std::size_t MaxBytes=16*1024*1024,MaxAgents=4096;
    static CosmeticsCatalog Parse(const Json& input) {
        if(!input.is_object() || input.value("format",std::string())!="cs2-camera-cosmetics-catalog" ||
           !input.contains("version") || !input["version"].is_number_integer() || input["version"]!=1 ||
           !input.contains("agents") || !input["agents"].is_array() || input["agents"].size()>MaxAgents)
            throw std::invalid_argument("unsupported builtin cosmetics catalogue format/version/agent count");
        CosmeticsCatalog result;
        for(const auto& entry:input["agents"]) {
            if(!entry.is_object() || !entry.contains("item_def") || !entry["item_def"].is_number_integer() ||
               entry["item_def"].get<long long>()<1 || entry["item_def"].get<long long>()>65535 ||
               entry.value("kind",std::string())!="agent")
                throw std::invalid_argument("catalogue agent requires a builtin item definition in 1..65535");
            Agent agent;agent.itemDef=entry["item_def"].get<unsigned>();
            if(result.agents_.count(agent.itemDef))throw std::invalid_argument("duplicate builtin agent item definition");
            agent.name=Label(entry,"name");agent.displayName=Label(entry,"display_name");
            const auto& teams=entry.at("allowed_teams");
            if(!teams.is_array() || teams.size()!=1 || !teams[0].is_number_integer() ||
               (teams[0]!=2 && teams[0]!=3))
                throw std::invalid_argument("builtin agent must belong to exactly one T or CT team");
            agent.team=teams[0].get<unsigned>();
            if(entry.value("team",std::string())!=(agent.team==2?"T":"CT"))
                throw std::invalid_argument("builtin agent team label differs from its allowed runtime team");
            if(entry.contains("map_based")) {
                if(!entry["map_based"].is_boolean())throw std::invalid_argument("agent map_based must be a boolean");
                agent.mapBased=entry["map_based"].get<bool>();
            }
            if(entry.contains("compiled_model_present") && !entry["compiled_model_present"].is_null()) {
                if(!entry["compiled_model_present"].is_boolean())throw std::invalid_argument("compiled_model_present must be boolean or null");
                agent.compiledModelPresent=entry["compiled_model_present"].get<bool>();
            }
            if(entry.contains("model") && !entry["model"].is_null()) {
                if(!entry["model"].is_string())throw std::invalid_argument("builtin agent model must be a resource name");
                agent.model=entry["model"].get<std::string>();ValidateModel(agent.model);
            }
            result.agents_.emplace(agent.itemDef,std::move(agent));
        }
        return result;
    }
    static Json ReadJson(const std::string& path) {
        if(path.empty() || path.front()!='/')throw std::invalid_argument("cosmetics catalogue requires an absolute private filename");
        const auto slash=path.find_last_of('/');struct stat parent{};
        if(slash==0 || ::lstat(path.substr(0,slash).c_str(),&parent) || !S_ISDIR(parent.st_mode) ||
           parent.st_uid!=getuid() || (parent.st_mode&0777)!=0700)
            throw std::invalid_argument("cosmetics catalogue parent must be a private directory owned by this user");
        int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)throw std::runtime_error("cannot open private cosmetics catalogue");
        try {
            struct stat info{};
            if(::fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=getuid() ||
               info.st_size<=0 || std::uint64_t(info.st_size)>MaxBytes)
                throw std::invalid_argument("cosmetics catalogue must be an owned regular JSON file up to 16 MiB");
            std::string bytes;bytes.reserve(std::size_t(info.st_size));char buffer[8192];
            for(;;) {
                const auto count=::read(fd,buffer,sizeof(buffer));
                if(count<0 && errno==EINTR)continue;
                if(count<0)throw std::runtime_error("cannot read private cosmetics catalogue");
                if(!count)break;
                bytes.append(buffer,std::size_t(count));if(bytes.size()>MaxBytes)throw std::invalid_argument("cosmetics catalogue exceeds 16 MiB");
            }
            ::close(fd);fd=-1;return Json::parse(bytes);
        }catch(...){if(fd>=0)::close(fd);throw;}
    }
    static CosmeticsCatalog Read(const std::string& path) {return Parse(ReadJson(path));}
    const Agent& ForTeam(unsigned itemDef,unsigned runtimeTeam) const {
        if(runtimeTeam!=2 && runtimeTeam!=3)throw std::runtime_error("agent override requires a live T or CT player");
        const auto found=agents_.find(itemDef);
        if(found==agents_.end())throw std::invalid_argument("agent item definition is not in the builtin catalogue");
        if(found->second.team!=runtimeTeam)throw std::invalid_argument("agent belongs to the opposite team");
        if(found->second.mapBased)throw std::invalid_argument("map-based default agent requires restoring the current map's original model");
        if(found->second.model.empty())throw std::runtime_error("builtin agent has no compiled model resource in the catalogue");
        return found->second;
    }
    bool MatchesTeam(unsigned itemDef,unsigned runtimeTeam) const {
        const auto found=agents_.find(itemDef);return found!=agents_.end() && found->second.team==runtimeTeam && (runtimeTeam==2 || runtimeTeam==3);
    }
    const Agent& GetAgent(unsigned itemDef) const {
        const auto found=agents_.find(itemDef);
        if(found==agents_.end())throw std::invalid_argument("agent item definition is not in the builtin catalogue");
        return found->second;
    }
    std::size_t Size() const {return agents_.size();}
    std::size_t AgentCount() const {return agents_.size();}
private:
    std::map<unsigned,Agent> agents_;
    static std::string Label(const Json& entry,const char* field) {
        if(!entry.contains(field) || !entry[field].is_string())throw std::invalid_argument("builtin agent label is missing");
        const auto result=entry[field].get<std::string>();
        if(result.empty() || result.size()>1024 || result.find('\0')!=std::string::npos)
            throw std::invalid_argument("builtin agent label is invalid or too long");
        return result;
    }
    static void ValidateModel(const std::string& path) {
        if(path.empty() || path.size()>512 || path.front()=='/' || path.size()<6 || path.substr(path.size()-5)!=".vmdl")
            throw std::invalid_argument("builtin agent model must be a relative .vmdl resource name");
        const auto first=path.substr(0,path.find('/'));
        if(first!="agents" && first!="characters" && first!="models")throw std::invalid_argument("builtin agent uses an unsupported resource root");
        std::size_t begin=0;
        while(begin<path.size()) {
            const auto end=path.find('/',begin);const auto part=path.substr(begin,end==std::string::npos?end:end-begin);
            if(part.empty() || part=="." || part=="..")throw std::invalid_argument("builtin model contains a path alias");
            for(const unsigned char c:part)
                if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.'))
                    throw std::invalid_argument("builtin model contains an unsupported resource character");
            if(end==std::string::npos)break;
            begin=end+1;
        }
    }
};
} // namespace cs2_live
