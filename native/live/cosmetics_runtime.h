#pragma once
#include "player_cosmetics.h"
#include "team_palette.h"

#ifdef CS2_CAMERA_SOURCE2_CVARS
#include <chrono>
#include <memory>
namespace cs2_live {
class Source2PlayerCosmetics {
public:
    using Address=ClientEntityProbe::Address;using Actor=PlayerCosmetics::Actor;using Color=PlayerCosmetics::Color;
    static PlayerCosmetics::Callbacks Bind(CreateInterfaceFn factory,void* client) {
        auto runtime=std::make_shared<Source2PlayerCosmetics>();PlayerCosmetics::Callbacks result;
        try {
            if(!OfflineLaunch())throw std::runtime_error("player cosmetics requires the isolated -insecure editor or cosmetics capture process");
            runtime->factory_=factory;runtime->client_=client;runtime->profile_=Source2ClientEntityProbe::ResolveProfile(factory,client);
            if(!runtime->profile_.fields.steamId || !runtime->profile_.fields.playerName || !runtime->profile_.fields.pawnHandle)
                throw std::runtime_error("runtime controller SteamID/name/full-pawn-handle schema is unavailable");
            runtime->CheckSchema("C_BaseEntity","m_pGameSceneNode",8,0x4a0);
            runtime->CheckSchema("C_BaseEntity","m_pRenderComponent",8,0x4a8);
            runtime->CheckSchema("CSkeletonInstance","m_modelState",-1,0x140);
            runtime->CheckSchema("CModelState","m_hModel",8,0xa0);
            runtime->CheckSchema("C_CSPlayerPawn","m_bNeedToReApplyGloves",1,0x16f5);
            runtime->Fingerprint(0x161f690,{0x55,0x48,0x89,0xe5,0x53,0x48,0x89,0xfb,0x48,0x83,0xec,0x08,0x48,0x8d,0x05,0x0d,0xa1,0x31,0x03,0x48,0x8b,0x38,0x48,0x8b,0x07,0xff,0x50,0x68,0x48,0x89,0xdf,0x48,0x8b,0x5d,0xf8,0xc9,0x48,0x89,0xc6,0xe9,0xc4,0xfc,0xff,0xff});
            runtime->Fingerprint(0x161f4f2,{0x49,0x8b,0x04,0x24,0x4c,0x89,0xe7,0x48,0x8b,0x80,0xb0,0x07,0x00,0x00});
            runtime->Fingerprint(0x1a18390,{0x55,0x48,0x89,0xe5,0x53,0x48,0x89,0xfb,0x48,0x83,0xec,0x08,0xe8,0xcf,0x2b,0xc2,0xff,0xc6,0x83,0xf5,0x16,0x00,0x00,0x01,0x48,0x89,0xdf,0x48,0x8b,0x5d,0xf8,0xc9,0xe9,0x6b,0x55,0xb7,0xff});
            runtime->Fingerprint(0x252f8c0,{0x48,0x8b,0x47,0x08,0x48,0x8d,0x15,0x73,0x11,0x66,0xfe,0x48,0x85,0xc0,0x48,0x0f,0x44,0xc2,0xc3});
            dl_iterate_phdr(&ExecutableSegments,runtime.get());
            runtime->CheckService();
            result.actors=[runtime]{return runtime->Actors();};result.current=[runtime](const Actor& actor){return runtime->Current(actor);};
            result.model=[runtime](const Actor& actor,const std::string& path){return runtime->SetModel(actor,path);};
            const char* path=std::getenv("CS2_CAMERA_COSMETICS_CATALOG");
            try {
                if(!path || !*path) {
                    const char* reason=std::getenv("CS2_CAMERA_COSMETICS_CATALOG_ERROR");
                    throw std::runtime_error(reason && *reason?reason:"builtin cosmetics catalogue was not staged for this session");
                }
                const auto catalog=CosmeticsCatalog::ReadJson(path);
                if(catalog.contains("source") && catalog["source"].contains("client_build_id") && !catalog["source"]["client_build_id"].is_null() &&
                   catalog["source"]["client_build_id"]!=runtime->profile_.buildId)throw std::runtime_error("builtin catalogue client build-ID differs from the current validated client");
                result.catalog=CosmeticsCatalog::Parse(catalog);result.agents=result.catalog.Size()>0;
            }catch(const std::exception& error){result.agentReason=error.what();}
            try {
                runtime->CheckSchema("C_BaseModelEntity","m_pClientAlphaProperty",8,0xfc8);
                runtime->Fingerprint(0x1909290,{0x40,0x88,0x77,0x17,0xc3});
                runtime->Fingerprint(0x1622c80,{0x55,0x48,0x89,0xe5,0x41,0x54,0x53,0x48,0x89,0xfb,0x48,0x83,0xec,0x20,0x48,0x8b,0xbf,0xc8,0x0f,0x00,0x00,0x0f,0xb6,0xb3,0x23,0x0c,0x00,0x00,0x48,0x8b,0x07,0xff,0x50,0x20});
                result.palette=true;result.color=[runtime](const Actor& actor,const Color& color){return runtime->SetColor(actor,color);};
            }catch(const std::exception& error){result.paletteReason=error.what();}
        }catch(const std::exception& error){result.agentReason=error.what();result.paletteReason=error.what();}
        return result;
    }
private:
    Source2ClientEntityProbe::Profile profile_;
    CreateInterfaceFn factory_=nullptr;void* client_=nullptr;std::uint64_t generation_=1;
    Address service_=0,serviceVtable_=0,serviceFunction_=0;
    std::vector<std::pair<Address,Address>> executable_;
    std::vector<std::uint32_t> controllers_;
    std::set<std::string> modelStrings_;
    std::chrono::steady_clock::time_point nextScan_{};
    static bool OfflineLaunch() {
        const char* live=std::getenv("CS2_CAMERA_LIVE");const char* capture=std::getenv("CS2_CAMERA_CAPTURE_COSMETICS");
        if((!live || std::strcmp(live,"1")) && (!capture || std::strcmp(capture,"1")))return false;
        const int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);if(fd<0)return false;
        std::array<char,65536> bytes{};const auto count=read(fd,bytes.data(),bytes.size());close(fd);
        if(count<=0 || count==ssize_t(bytes.size()))return false;
        for(std::size_t position=0;position<std::size_t(count);) {const auto size=strnlen(bytes.data()+position,std::size_t(count)-position);if(size==std::size_t(count)-position)return false;if(std::string(bytes.data()+position,size)=="-insecure")return true;position+=size+1;}return false;
    }
    template<class T>bool Read(Address address,T& value)const{return ClientEntityProbe::Value(profile_.read,address,value);}
    bool Exec(Address address)const{for(const auto& range:executable_)if(address>=range.first && address<range.second)return true;return false;}
    static int ExecutableSegments(dl_phdr_info* module,std::size_t,void* data) {
        auto& runtime=*static_cast<Source2PlayerCosmetics*>(data);
        for(unsigned i=0;i<module->dlpi_phnum;++i){const auto& segment=module->dlpi_phdr[i];if(segment.p_type==PT_LOAD && (segment.p_flags&PF_X) && !(segment.p_flags&PF_W))runtime.executable_.emplace_back(module->dlpi_addr+segment.p_vaddr,module->dlpi_addr+segment.p_vaddr+segment.p_memsz);}return 0;
    }
    void Fingerprint(Address offset,std::initializer_list<unsigned char> expected)const {
        profile_.Fingerprint(offset,expected);
    }
    void CheckSchema(const char* cls,const char* name,int size,unsigned expected)const {
        const auto offset=Source2ClientEntityProbe::SchemaField(profile_.scope,cls,name,size);
        if(!offset || *offset!=expected)throw std::runtime_error(std::string("runtime cosmetic schema differs: ")+cls+"."+name);
    }
    void CheckService() {
        if(!Read(profile_.At(0x49397b0),service_) || !service_ || !Read(service_,serviceVtable_) || !serviceVtable_ ||
           !Read(serviceVtable_+0x68,serviceFunction_) || !Exec(serviceFunction_))throw std::runtime_error("current client model service or its lookup function is unavailable");
    }
    bool ServiceCurrent()const{Address service=0,table=0,function=0;return Read(profile_.At(0x49397b0),service) && service==service_ && Read(service,table) && table==serviceVtable_ && Read(table+0x68,function) && function==serviceFunction_ && Exec(function);}
    bool SystemCurrent()const{Address current=0;return Read(profile_.At(0x4b95610),current) && current==profile_.system;}
    bool PawnVtable(Address pawn)const{Address table=0,callback=0;return Read(pawn,table) && table==profile_.At(0x450bd30) && Read(table+0x7b0,callback) && callback==profile_.At(0x1a18390) && Exec(callback);}
    bool Current(const Actor& actor)const {
        if(!SystemCurrent() || actor.generation!=generation_ || actor.steamId.empty() || (actor.team!=2 && actor.team!=3))return false;
        const auto player=ClientEntityProbe::ResolvePlayer(profile_.read,profile_.system,profile_.fields,actor.controllerHandle);
        return player && player->pawn.instance==actor.instance && player->pawn.handle==actor.pawnHandle && player->team==actor.team &&
            std::to_string(player->steamId)==actor.steamId && PawnVtable(player->pawn.instance) && SystemCurrent();
    }
    static bool ModelName(const std::string& path) {
        if(path.empty() || path.size()>512 || path.front()=='/' || path.size()<6 || path.substr(path.size()-5)!=".vmdl" || path.find("..")!=std::string::npos || path.find("//")!=std::string::npos)return false;
        if(path.rfind("characters/",0) && path.rfind("models/",0) && path.rfind("agents/",0))return false;
        for(const unsigned char c:path)if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='/' || c=='_' || c=='-' || c=='.'))return false;
        return true;
    }
    std::string CurrentModel(Address pawn)const {
        Address node=0,binding=0,data=0,name=0;
        if(!Read(pawn+0x4a0,node) || !node || !SkeletonNode(node) || !Read(node+0x1e0,binding) || !binding || !Read(binding,data) || !data || !Read(data+8,name))return {};
        const auto result=ClientEntityProbe::Text(profile_.read,name,513);return ModelName(result)?result:std::string();
    }
    bool SkeletonNode(Address node)const {
        Address table=0,type=0,parent=0,accessor=0;if(!Read(node,table))return false;
        if(table==profile_.At(0x44e8560))return true;
        // Actual network pawns embed this exact single-inheritance wrapper.
        // Its RTTI names CSkeletonInstance as the base at offset zero; its
        // model state uses the schema-validated base layout above.
        return table==profile_.At(0x43ea830) && Read(table-8,type) && type==profile_.At(0x43ea268) &&
            Read(type+16,parent) && parent==profile_.At(0x44e3cc8) && Read(table+8*8,accessor) && accessor==profile_.At(0x17d4520) && Exec(accessor);
    }
    bool ColorReady(Address pawn)const {
        Address alpha=0,table=0,fn=0,renderer=0,renderTable=0,renderFn=0,entityTable=0,colorFn=0;
        return Read(pawn+0xfc8,alpha) && alpha && Read(alpha,table) && table==profile_.At(0x44308a8) && Read(table+0x20,fn) && fn==profile_.At(0x1909290) && Exec(fn) &&
            Read(pawn+0x4a8,renderer) && renderer && Read(renderer,renderTable) && Read(renderTable,renderFn) && Exec(renderFn) &&
            Read(pawn,entityTable) && Read(entityTable+0x750,colorFn) && Exec(colorFn);
    }
    std::vector<Actor> Actors() {
        if(!SystemCurrent()) {
            controllers_.clear();nextScan_={};
            const auto replacement=Source2ClientEntityProbe::ResolveProfile(factory_,client_);
            if(replacement.base!=profile_.base)throw std::runtime_error("client module changed during the cosmetics session");
            profile_=replacement;++generation_;CheckService();
        }
        const auto now=std::chrono::steady_clock::now();
        if(now>=nextScan_) {
            controllers_.clear();const auto probe=ClientEntityProbe::Inspect(profile_.read,profile_.system,profile_.fields,128);
            for(const auto& entry:probe["entries"])if(entry.contains("steam_id") && entry["steam_id"]!="0" && entry.contains("pawn_handle"))controllers_.push_back(entry["handle"].get<std::uint32_t>());
            nextScan_=now+std::chrono::seconds(1);
        }
        std::vector<Actor> actors;
        for(const auto handle:controllers_) {
            const auto player=ClientEntityProbe::ResolvePlayer(profile_.read,profile_.system,profile_.fields,handle);if(!player)continue;
            Actor actor;actor.steamId=std::to_string(player->steamId);actor.nickname=player->nickname;
            actor.controllerHandle=handle;actor.pawnHandle=player->pawn.handle;actor.instance=player->pawn.instance;actor.generation=generation_;actor.team=player->team;
            if(!Current(actor))continue;
            actor.model=CurrentModel(actor.instance);actor.modelReady=!actor.model.empty();
            actor.colorReady=ColorReady(actor.instance) && profile_.read(actor.instance+profile_.fields.renderColor,actor.color.data(),4);
            actors.push_back(std::move(actor));
        }
        std::map<std::string,unsigned> counts;for(const auto& actor:actors)++counts[actor.steamId];
        actors.erase(std::remove_if(actors.begin(),actors.end(),[&](const Actor& actor){return counts[actor.steamId]!=1;}),actors.end());
        return actors;
    }
    bool SetModel(const Actor& actor,const std::string& model) {
        // SetModel updates the skeleton, bounds and pawn model-change virtual;
        // the validated pawn callback itself requests glove reapplication.
        if(!Current(actor) || !ServiceCurrent() || !ModelName(model) || CurrentModel(actor.instance).empty())return false;
        const auto stable=modelStrings_.insert(model).first;
        reinterpret_cast<void(*)(void*,const char*)>(profile_.At(0x161f690))(reinterpret_cast<void*>(actor.instance),stable->c_str());
        return Current(actor) && CurrentModel(actor.instance)==model;
    }
    bool SetColor(const Actor& actor,const Color& color)const {
        if(!Current(actor) || !ColorReady(actor.instance))return false;
        Color original{};if(!profile_.read(actor.instance+profile_.fields.renderColor,original.data(),4))return false;
        iovec local{const_cast<unsigned char*>(color.data()),4},remote{reinterpret_cast<void*>(actor.instance+profile_.fields.renderColor),4};
        if(process_vm_writev(getpid(),&local,1,&remote,1,0)!=4)return false;
        reinterpret_cast<void(*)(void*)>(profile_.At(0x1622c80))(reinterpret_cast<void*>(actor.instance));
        Color actual{};return Current(actor) && profile_.read(actor.instance+profile_.fields.renderColor,actual.data(),4) && actual==color;
    }
};
} // namespace cs2_live
#endif
