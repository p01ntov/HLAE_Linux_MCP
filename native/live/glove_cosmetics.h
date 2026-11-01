#pragma once
#include "player_cosmetics.h"
#include "team_palette.h"
#include "glove_pose.h"
#include <memory>
#include <optional>
#include <thread>

namespace cs2_live {
// A journal owns an engine copy of the complete original item, never a memcpy
// of its managed vectors. All callbacks and journal disposal run on game thread.
class GloveCosmetics {
public:
    using Json=nlohmann::json;using Actor=PlayerCosmetics::Actor;
    struct Paint {unsigned itemDef=0,kit=0,seed=0;float wear=0.01f;};
    struct Glove {
        Actor actor;Json item=Json::object(),finalization=Json::object();unsigned itemDef=0;unsigned char networkVersion=0;
        std::uint32_t worldHandle=0xffffffff,hudHandle=0xffffffff;std::uintptr_t worldInstance=0,hudInstance=0;
        bool itemReadable=false,ready=false,worldReady=false,hudReady=false,needsRefresh=false;
        bool worldPoseReady=false,hudPoseReady=false;unsigned worldBones=0,hudBones=0,worldRequestedBones=0,hudRequestedBones=0;
        bool worldRenderReady=false,hudRenderReady=false,hudRenderRequired=true;std::optional<bool> hudParentRenderEnabled;unsigned worldRenderObjects=0,hudRenderObjects=0,worldHiddenObjects=0,hudHiddenObjects=0;
        int worldMaterialJobs=-1,hudMaterialJobs=-1;std::string reason;
    };
    struct Snapshot {Json item;std::shared_ptr<void> native;unsigned teamMask=(1u<<2)|(1u<<3);};
    struct Callbacks {
        std::function<std::vector<Glove>()> gloves,observe;
        std::function<void(std::function<void()>)> observeAfterRender;
        std::function<void(bool,int,bool)> context;
        std::function<bool(const Glove&)> current;
        std::function<Paint(const Json&)> resolve;
        // snapshot must construct, copy and verify a second scratch item before
        // returning. Returning null cannot have changed the live item.
        std::function<std::shared_ptr<Snapshot>(const Glove&)> snapshot;
        std::function<bool(const Glove&,const Paint&)> apply;
        std::function<bool(const Glove&,const Snapshot&)> restore;
        bool available=false;std::string reason="current client glove adapter has not passed preflight";
    };
    ~GloveCosmetics(){observationLife_.reset();}
    void Bind(Callbacks value){
        if(!owned_.empty())throw std::logic_error("restore gloves before rebinding");
        // Registration may install the native phase callback. Keep the prior
        // backend and observer alive until that registration has succeeded.
        auto nextLife=std::make_shared<int>(0);
        if(value.observeAfterRender){
            std::weak_ptr<int> life=nextLife;
            value.observeAfterRender([this,life]{const auto current=life.lock();if(current && current==observationLife_)RefreshObserved();});
        }
        callbacks_=std::move(value);observationLife_=std::move(nextLife);
    }
    void Context(bool demo,int tick,bool paused){if(callbacks_.context)callbacks_.context(demo,tick,paused);}
    void ValidateRequest(const Json& value,bool requireCurrent=false)const {
        ValidateGloves(value);if(value.empty())return;
        if(!callbacks_.available || !callbacks_.resolve || !callbacks_.snapshot || !callbacks_.apply || !callbacks_.restore)throw std::runtime_error(callbacks_.reason);
        const auto live=requireCurrent && callbacks_.gloves?callbacks_.gloves():std::vector<Glove>{};
        for(const auto& entry:value){callbacks_.resolve(entry);if(!requireCurrent)continue;unsigned count=0;for(const auto& g:live)if(g.actor.steamId==entry["steam_id"]){++count;if(!g.ready || !Current(g))throw std::runtime_error(g.reason.empty()?"selected glove item is not current or cannot be copied safely":g.reason);}if(count!=1)throw std::runtime_error("selected SteamID has no unique current demo pawn");}
    }
    void Apply(const Json& value){ValidateRequest(value);if(desired_!=value)++revision_;desired_=value;Refresh(true);}
    void Refresh(bool active){
        active_=active;error_.clear();errors_.clear();bool dirty=false;std::vector<Glove> live;
        try{if(callbacks_.gloves)live=callbacks_.gloves();}catch(const std::exception& e){error_=e.what();stale_=true;return;}
        stale_=live.empty();
        for(auto it=attempts_.begin();it!=attempts_.end();)if(!owned_.count(it->first) && std::none_of(live.begin(),live.end(),[&](const Glove& g){return g.actor.steamId==it->first;}))it=attempts_.erase(it);else ++it;
        // An absent object may be a transient snapshot. A new generation or a
        // replacement for that SteamID proves the former lifetime has ended.
        for(auto it=owned_.begin();it!=owned_.end();){
            const auto found=std::find_if(live.begin(),live.end(),[&](const Glove& g){return Same(g.actor,it->second.actor);});
            if(found==live.end()){
                const bool replaced=std::any_of(live.begin(),live.end(),[&](const Glove& g){return g.actor.generation!=it->second.actor.generation || g.actor.steamId==it->second.actor.steamId;});
                if(replaced)it=owned_.erase(it);else ++it;continue;
            }
            auto& owned=it->second;const auto& g=*found;
            if(!Current(g) || !g.itemReadable){++it;continue;}
            // An immediate post-write read failure cannot establish what we
            // now own. Keep the original; never adopt an unknown later item.
            if(owned.readbackUnresolved){
                if(Semantic(g.item)!=Semantic(owned.last)){errors_[g.actor.steamId]="glove post-write readback is unresolved; original snapshot retained until demo refresh";++it;continue;}
                owned.readbackUnresolved=false;
            }
            // A network/third-party baseline supersedes our exact last output.
            // Discard its former backup without writing any part of it back.
            if(Semantic(g.item)!=Semantic(owned.last)){it=owned_.erase(it);continue;}
            owned.networkVersion=g.networkVersion;
            owned.actor=g.actor; // Team is presentation state within this same pawn lifetime.
            if(!(owned.original->teamMask&(1u<<g.actor.team))) {
                errors_[g.actor.steamId]="original glove belongs to the previous team; canonical demo refresh is required";++it;continue;
            }
            if(active && Override(g)){++it;continue;}
            if(!g.ready){++it;continue;}
            const auto attempt=Attempt(g,Json::object());
            if(owned.restoreAttempt==attempt){errors_[g.actor.steamId]="glove restoration was rejected; refresh the demo before capture";++it;continue;}
            owned.restoreAttempt=attempt;dirty=true;
            bool restored=false;try{restored=callbacks_.restore && callbacks_.restore(g,*owned.original);}catch(const std::exception& e){errors_[g.actor.steamId]=e.what();}
            if(restored){it=owned_.erase(it);continue;}
            errors_[g.actor.steamId]="client did not confirm exact original glove restoration";++it;
        }
        if(dirty && !Reload(live))return;
        for(const auto& g:live){
            const auto* wanted=active?Override(g):nullptr;if(!wanted || !g.itemReadable || !g.ready || !Current(g))continue;
            if(!KnownItemId(g.item)){errors_[g.actor.steamId]="glove raw item ID is neither zero nor its verified high/low value; no item write was attempted";continue;}
            const auto paint=callbacks_.resolve(*wanted);auto owned=owned_.find(g.actor.steamId);
            if(owned!=owned_.end() && owned->second.readbackUnresolved){errors_[g.actor.steamId]="glove post-write readback is unresolved; original snapshot retained until demo refresh";continue;}
            if(owned!=owned_.end() && owned->second.confirmed && Matches(g.item,paint)){owned->second.paint=paint;continue;}
            const auto attempt=Attempt(g,*wanted);
            if(attempts_[g.actor.steamId]==attempt){errors_[g.actor.steamId]="glove preflight/application was rejected; change the selection or refresh the demo";continue;}
            attempts_[g.actor.steamId]=attempt;
            if(owned==owned_.end()){
                std::shared_ptr<Snapshot> baseline;try{baseline=callbacks_.snapshot(g);}catch(const std::exception& e){errors_[g.actor.steamId]=e.what();}
                if(!baseline || CopyCanonical(baseline->item)!=CopyCanonical(g.item) || !Current(g)){const auto detail=errors_.count(g.actor.steamId)?": "+errors_[g.actor.steamId]:std::string();errors_[g.actor.steamId]="engine glove snapshot round-trip was rejected; no live item write was attempted"+detail;continue;}
                Owned journal;journal.actor=g.actor;journal.original=std::move(baseline);journal.last=g.item;journal.networkVersion=g.networkVersion;
                owned=owned_.emplace(g.actor.steamId,std::move(journal)).first;
            }
            bool accepted=false;try{accepted=callbacks_.apply(g,paint);}catch(const std::exception& e){errors_[g.actor.steamId]=e.what();}
            dirty=true;owned->second.paint=paint;owned->second.confirmed=accepted;
            // Capture actual readback even after a partially rejected call so
            // reset can restore only that exact output on this same lifetime.
            owned->second.readbackUnresolved=true;
            std::vector<Glove> after;if(!Reload(after)){owned->second.confirmed=false;return;}
            const auto actual=std::find_if(after.begin(),after.end(),[&](const Glove& a){return Same(g.actor,a.actor);});
            if(actual!=after.end() && Current(*actual) && actual->itemReadable && actual->networkVersion==g.networkVersion){owned->second.last=actual->item;owned->second.networkVersion=actual->networkVersion;owned->second.readbackUnresolved=false;owned->second.confirmed=accepted && Matches(actual->item,paint);}
            else owned->second.confirmed=false;
            if(actual!=after.end() && actual->networkVersion!=g.networkVersion){owned->second.confirmed=false;errors_[g.actor.steamId]="network glove version changed during application; original snapshot retained";continue;}
            if(!owned->second.confirmed)errors_[g.actor.steamId]=owned->second.readbackUnresolved?"glove post-write readback is unresolved; original snapshot retained until demo refresh":"client did not confirm glove item and native material refresh";
        }
        if(dirty && !Reload(live))return;
        BuildRows(live,active);
    }

    Json State()const {
        const bool unresolved=std::any_of(owned_.begin(),owned_.end(),[](const auto& entry){return entry.second.readbackUnresolved;});
        const bool pending=std::any_of(owned_.begin(),owned_.end(),[&](const auto& owned){return std::none_of(desired_.begin(),desired_.end(),[&](const Json& entry){return entry["steam_id"]==owned.first;});});
        const bool teamRefresh=std::any_of(owned_.begin(),owned_.end(),[](const auto& entry){return !(entry.second.original->teamMask&(1u<<entry.second.actor.team));});
        Json result={{"gloves",rows_},{"capability",Capabilities()},{"stale",stale_},{"owned_overrides",owned_.size()},{"restoration_unresolved",unresolved},{"restoration_pending",pending},{"needs_demo_refresh",teamRefresh}};
        if(!error_.empty())result["error"]=error_;
        return result;
    }
    Json Capabilities()const{return {{"available",callbacks_.available},{"reason",callbacks_.reason},{"scope","one builtin glove item on the selected SteamID's current pawn"},{"snapshot_roundtrip_required",true},{"render_verified_1p",false},{"render_verified_3p",false}};}
    static bool KnownItemId(const Json& item){if(!item.contains("m_iItemID") || !item.contains("m_iItemIDHigh") || !item.contains("m_iItemIDLow"))return false;const auto raw=item.at("m_iItemID").get<std::uint64_t>();const auto packed=(std::uint64_t(item.at("m_iItemIDHigh").get<std::uint32_t>())<<32)|item.at("m_iItemIDLow").get<std::uint32_t>();return raw==0 || raw==packed;}
    // The current virtual ID getter and engine assignment both derive this
    // redundant cache from the unchanged network high/low halves.
    static Json CopyCanonical(const Json& item){Json value=item;if(item.contains("m_iItemIDHigh") && item.contains("m_iItemIDLow"))value["m_iItemID"]=(std::uint64_t(item.at("m_iItemIDHigh").get<std::uint32_t>())<<32)|item.at("m_iItemIDLow").get<std::uint32_t>();return value;}
    static Json Semantic(const Json& item){Json value=CopyCanonical(item);for(const char* key:{"m_bInventoryImageRgbaRequested","m_bInventoryImageTriedCache","m_nInventoryImageRgbaWidth","m_nInventoryImageRgbaHeight","m_szCurrentLoadCachedFileName","m_bInitializedTags","m_bRestoreCustomMaterialAfterPrecache"})value.erase(key);return value;}
    static bool Matches(const Json& item,const Paint& p){
        if(!item.is_object() || item.value("m_iItemDefinitionIndex",0u)!=p.itemDef || item.value("m_bInitialized",0u)!=1 || !item.contains("attributes"))return false;
        std::array<std::optional<float>,3> values{};for(const auto& a:item["attributes"]){const auto id=a.at("definition").get<unsigned>();if(id>=6 && id<=8){if(values[id-6])return false;std::uint32_t bits=a.at("value_bits").get<std::uint32_t>();float f=0;std::memcpy(&f,&bits,4);values[id-6]=f;}}
        return values==std::array<std::optional<float>,3>{float(p.kit),float(p.seed),p.wear};
    }
private:
    void RefreshObserved(){try{const auto live=callbacks_.observe?callbacks_.observe():std::vector<Glove>{};stale_=live.empty();BuildRows(live,active_);}catch(const std::exception& e){error_=e.what();stale_=true;}}
    void BuildRows(const std::vector<Glove>& live,bool active){
        rows_=Json::array();for(const auto& g:live){
            const auto owned=owned_.find(g.actor.steamId);const auto* wanted=active?Override(g):nullptr;bool applied=false;
            if(wanted && owned!=owned_.end())applied=Same(owned->second.actor,g.actor) && Current(g) && g.itemReadable && !owned->second.readbackUnresolved && owned->second.confirmed && Semantic(g.item)==Semantic(owned->second.last) && g.networkVersion==owned->second.networkVersion && Matches(g.item,callbacks_.resolve(*wanted));
            Json row={{"steam_id",g.actor.steamId},{"nickname",g.actor.nickname},{"team",g.actor.team},{"pawn_handle",g.actor.pawnHandle},{"item_def",g.itemDef},{"item_readable",g.itemReadable},{"ready",g.ready},{"applied",applied},{"restoration_unresolved",owned!=owned_.end() && owned->second.readbackUnresolved},{"world_ready",g.worldReady},{"hud_ready",g.hudReady},{"world_handle",g.worldHandle},{"hud_handle",g.hudHandle},{"needs_refresh",g.needsRefresh},{"actual_item",g.item},{"material_jobs",{{"world",g.worldMaterialJobs},{"hud",g.hudMaterialJobs}}},{"materials_pending",g.needsRefresh || !g.worldReady || !g.hudReady || g.worldMaterialJobs!=0 || g.hudMaterialJobs!=0}};
            row["world_pose_ready"]=g.worldPoseReady;row["hud_pose_ready"]=g.hudPoseReady;row["pose_bones"]={{"world",g.worldBones},{"hud",g.hudBones}};
            row["pose_request_bones"]={{"world",g.worldRequestedBones},{"hud",g.hudRequestedBones}};
            row["world_render_ready"]=g.worldRenderReady;row["hud_render_ready"]=g.hudRenderReady;row["hud_render_required"]=g.hudRenderRequired;row["hud_parent_render_enabled"]=g.hudParentRenderEnabled?Json(*g.hudParentRenderEnabled):Json(nullptr);
            row["render_objects"]={{"world",g.worldRenderObjects},{"hud",g.hudRenderObjects}};row["hidden_render_objects"]={{"world",g.worldHiddenObjects},{"hud",g.hudHiddenObjects}};
            row["materials_pending"]=row["materials_pending"].get<bool>() || !g.worldPoseReady || !g.hudPoseReady || !g.worldRenderReady || (g.hudRenderRequired && !g.hudRenderReady);
            if(!g.finalization.empty())row["finalization"]=g.finalization;
            if(g.item.contains("m_iItemID")){row["raw_item_id"]=g.item["m_iItemID"];row["engine_copy_item_id"]=CopyCanonical(g.item)["m_iItemID"];}
            if(wanted){const auto p=callbacks_.resolve(*wanted);row["requested_item_def"]=p.itemDef;row["paint_kit"]=p.kit;row["seed"]=p.seed;row["wear"]=p.wear;}
            if(!g.reason.empty())row["reason"]=g.reason;
            if(errors_.count(g.actor.steamId))row["error"]=errors_[g.actor.steamId];
            rows_.push_back(std::move(row));
        }
    }
    struct Owned{Actor actor;std::shared_ptr<Snapshot> original;Json last;Paint paint;unsigned char networkVersion=0;bool confirmed=false,readbackUnresolved=false;std::string restoreAttempt;};
    Callbacks callbacks_;std::map<std::string,Owned> owned_;std::map<std::string,std::string> attempts_,errors_;Json desired_=Json::array(),rows_=Json::array();std::uint64_t revision_=0;bool active_=false,stale_=true;std::shared_ptr<int> observationLife_=std::make_shared<int>(0);std::string error_;
    static bool Same(const Actor& a,const Actor& b){return a.steamId==b.steamId && a.instance==b.instance && a.pawnHandle==b.pawnHandle && a.controllerHandle==b.controllerHandle && a.generation==b.generation;}
    bool Current(const Glove& g)const{return callbacks_.current && callbacks_.current(g);}
    const Json* Override(const Glove& g)const{for(const auto& entry:desired_)if(entry["steam_id"]==g.actor.steamId)return &entry;return nullptr;}
    std::string Attempt(const Glove& g,const Json& target)const{return std::to_string(revision_)+":"+std::to_string(g.actor.generation)+":"+std::to_string(g.actor.pawnHandle)+":"+std::to_string(g.actor.instance)+":"+std::to_string(g.networkVersion)+":"+Semantic(g.item).dump()+":"+target.dump();}
    bool Reload(std::vector<Glove>& live){try{live=callbacks_.gloves?callbacks_.gloves():std::vector<Glove>{};stale_=live.empty();return true;}catch(const std::exception& e){error_=e.what();stale_=true;return false;}}
};
} // namespace cs2_live

#ifdef CS2_CAMERA_SOURCE2_CVARS
#include <chrono>
#include <mutex>
#include "glove_render_phase.h"
namespace cs2_live {
class Source2GloveCosmetics:public std::enable_shared_from_this<Source2GloveCosmetics> {
public:
    using Address=ClientEntityProbe::Address;using Json=nlohmann::json;using Glove=GloveCosmetics::Glove;using Paint=GloveCosmetics::Paint;using Snapshot=GloveCosmetics::Snapshot;using Actor=PlayerCosmetics::Actor;
    static GloveCosmetics::Callbacks Bind(CreateInterfaceFn factory,void* client){
        auto r=std::make_shared<Source2GloveCosmetics>();GloveCosmetics::Callbacks result;
        try{
            if(!OfflineLaunch())throw std::runtime_error("gloves require the isolated -insecure editor or cosmetics capture process");
            r->factory_=factory;r->client_=client;r->profile_=Source2ClientEntityProbe::ResolveProfile(factory,client);r->Preflight();
            const char* path=std::getenv("CS2_CAMERA_COSMETICS_CATALOG");if(!path || !*path)throw std::runtime_error("private builtin glove catalogue is unavailable");r->ReadCatalog(CosmeticsCatalog::ReadJson(path));
            result.gloves=[r]{return r->Gloves();};result.observe=[r]{return r->Gloves(false);};result.observeAfterRender=[r](std::function<void()> callback){std::weak_ptr<Source2GloveCosmetics> weak=r;GloveRenderPhase::Install(r->profile_.base,r->profile_.read,[weak]{if(const auto live=weak.lock();live && live->OnThread())live->LateRender();});r->afterRender_=std::move(callback);};result.context=[r](bool demo,int tick,bool paused){r->demo_=demo;r->tick_=tick;r->paused_=paused;};result.current=[r](const Glove& g){return r->Current(g);};result.resolve=[r](const Json& j){return r->Resolve(j);};
            result.snapshot=[r](const Glove& g){return r->Capture(g);};result.apply=[r](const Glove& g,const Paint& p){return r->Apply(g,p);};result.restore=[r](const Glove& g,const Snapshot& s){return r->Restore(g,s);};
            result.available=true;result.reason="current Linux glove ABI/catalogue passed; each mutation additionally requires an engine snapshot round-trip; render verification pending";
        }catch(const std::exception& e){result.reason=e.what();}return result;
    }
    static bool ShutdownRenderPhase(){return GloveRenderPhase::Remove([](Address a,void* out,std::size_t n){iovec local{out,n},remote{reinterpret_cast<void*>(a),n};return process_vm_readv(getpid(),&local,1,&remote,1,0)==ssize_t(n);});}
private:
    struct PaintRange{float minimum=0,maximum=1;};
    struct alignas(16) Storage{std::array<unsigned char,0x14b8> bytes{};};
    struct Deferred{std::shared_ptr<Source2GloveCosmetics> runtime;Storage* storage;};
    inline static std::mutex deferredMutex_;inline static std::vector<Deferred> deferred_;
    Source2ClientEntityProbe::Profile profile_;CreateInterfaceFn factory_=nullptr;void* client_=nullptr;std::uint64_t generation_=1;std::thread::id thread_;bool demo_=false,paused_=false;int tick_=0;bool servicing_=false;std::function<void()> afterRender_;
    std::vector<std::uint32_t> controllers_;std::chrono::steady_clock::time_point nextScan_{};std::map<unsigned,std::set<unsigned>> allowed_,teams_;std::map<unsigned,PaintRange> paints_;
    struct PoseWork:GlovePublicationScope{Actor actor;Json item;unsigned calls=0;std::chrono::steady_clock::time_point nextAttempt;bool hudRefreshed=false;std::string worldPublishReason="not_attempted",hudPublishReason="not_attempted";unsigned publications=0;};
    std::map<std::string,PoseWork> poseWork_;std::map<std::string,Json> poseDiagnostics_;
    template<class T>bool Read(Address a,T& v)const{return ClientEntityProbe::Value(profile_.read,a,v);}
    static bool OfflineLaunch(){const char* l=std::getenv("CS2_CAMERA_LIVE"),*c=std::getenv("CS2_CAMERA_CAPTURE_COSMETICS");if((!l || std::strcmp(l,"1")) && (!c || std::strcmp(c,"1")))return false;int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);if(fd<0)return false;std::array<char,65536> b{};const auto n=read(fd,b.data(),b.size());close(fd);if(n<=0 || n==ssize_t(b.size()))return false;for(std::size_t p=0;p<std::size_t(n);){const auto z=strnlen(b.data()+p,std::size_t(n)-p);if(z==std::size_t(n)-p)return false;if(std::string(b.data()+p,z)=="-insecure")return true;p+=z+1;}return false;}
    void Thread(bool drain=true){if(thread_==std::thread::id())thread_=std::this_thread::get_id();if(thread_!=std::this_thread::get_id())throw std::runtime_error("glove engine calls require the original game thread");if(!ModuleCurrent())throw std::runtime_error("validated glove client module/constructor/copy/destructor is no longer loaded");if(drain)Drain();}
    bool OnThread()const{return thread_!=std::thread::id() && thread_==std::this_thread::get_id();}
    bool ModuleCurrent()const{
        struct ModuleCheck{Address base;unsigned matches=0;bool build=false;};ModuleCheck check{profile_.base};
        dl_iterate_phdr([](dl_phdr_info* info,std::size_t,void* data){auto& c=*static_cast<ModuleCheck*>(data);if(info->dlpi_addr!=c.base || !info->dlpi_name || !std::strstr(info->dlpi_name,"/csgo/bin/linuxsteamrt64/libclient.so"))return 0;++c.matches;
            static constexpr unsigned char expected[]={0xf1,0xe0,0x4e,0x36,0x16,0x92,0x1a,0x3b,0xb2,0x59,0x40,0xee,0xa9,0xb4,0xb6,0xbf,0xd7,0xcc,0xad,0xa5};
            for(unsigned i=0;i<info->dlpi_phnum;++i){const auto& segment=info->dlpi_phdr[i];if(segment.p_type!=PT_NOTE)continue;const auto* bytes=reinterpret_cast<const unsigned char*>(info->dlpi_addr+segment.p_vaddr);for(std::size_t p=0;p+sizeof(ElfW(Nhdr))<=segment.p_memsz;){const auto* n=reinterpret_cast<const ElfW(Nhdr)*>(bytes+p);const auto align=[](std::size_t x){return (x+3)&~std::size_t(3);};if(n->n_namesz>segment.p_memsz || n->n_descsz>segment.p_memsz)break;const auto size=sizeof(*n)+align(n->n_namesz)+align(n->n_descsz);if(size>segment.p_memsz-p)break;const auto* name=bytes+p+sizeof(*n);const auto* value=name+align(n->n_namesz);if(n->n_type==NT_GNU_BUILD_ID && n->n_namesz==4 && !std::memcmp(name,"GNU",4) && n->n_descsz==sizeof(expected) && !std::memcmp(value,expected,sizeof(expected)))c.build=true;p+=size;}}
            return 0;},&check);
        if(check.matches!=1 || !check.build)return false;
        try{Instructions();}catch(...){return false;}return GloveRenderPhase::Current(profile_.base,profile_.read);
    }
    void Drain(){std::vector<Deferred> ready;{std::lock_guard<std::mutex> lock(deferredMutex_);for(auto it=deferred_.begin();it!=deferred_.end();)if(it->runtime->OnThread() && it->runtime->ModuleCurrent()){ready.push_back(std::move(*it));it=deferred_.erase(it);}else ++it;}for(auto& d:ready){reinterpret_cast<void(*)(void*)>(d.runtime->profile_.base+0x2082400)(d.storage);delete d.storage;}}
    std::shared_ptr<void> NewScratch(){Thread();auto* p=new Storage;reinterpret_cast<void(*)(void*)>(profile_.base+0x2081040)(p);auto self=shared_from_this();return {p,[self](void* v){auto* s=static_cast<Storage*>(v);if(self->OnThread() && self->ModuleCurrent()){reinterpret_cast<void(*)(void*)>(self->profile_.base+0x2082400)(s);delete s;}else{std::lock_guard<std::mutex> lock(deferredMutex_);deferred_.push_back({self,s});}}};}
    void Fingerprint(Address off,std::initializer_list<unsigned char> expected)const{std::vector<unsigned char>b(expected.size());if(!profile_.read(profile_.base+off,b.data(),b.size()) || !std::equal(b.begin(),b.end(),expected.begin()))throw std::runtime_error("client glove instruction fingerprint differs");}
    void Schema(const char* cls,const char* field,int size,unsigned expected)const{const auto v=Source2ClientEntityProbe::SchemaField(profile_.scope,cls,field,size);if(!v || *v!=expected)throw std::runtime_error(std::string("client glove schema differs: ")+cls+"."+field);}
    bool Executable(Address offset,std::size_t size)const {
        struct Check{Address base,offset;std::size_t size;bool executable=false;};Check check{profile_.base,offset,size};
        dl_iterate_phdr([](dl_phdr_info* info,std::size_t,void* data){auto& c=*static_cast<Check*>(data);if(info->dlpi_addr!=c.base)return 0;
            for(unsigned i=0;i<info->dlpi_phnum;++i){const auto& p=info->dlpi_phdr[i];if(p.p_type==PT_LOAD && (p.p_flags&PF_X) && c.offset>=p.p_vaddr && c.offset-p.p_vaddr<=p.p_memsz && c.size<=p.p_memsz-(c.offset-p.p_vaddr))c.executable=true;}return 0;},&check);
        return check.executable;
    }
    void Instructions()const{
        if(!Executable(GloveRenderPhase::Site-10,15) || !Executable(GloveRenderPhase::Target,16))throw std::runtime_error("native glove interpolation phase is not executable");
        Fingerprint(GloveRenderPhase::Site-10,{0xf3,0x0f,0x10,0x85,0x7c,0xff,0xff,0xff,0x89,0xdf});
        Fingerprint(GloveRenderPhase::Target,{0x4c,0x8d,0x54,0x24,0x08,0x48,0x83,0xe4,0x80,0x41,0xff,0x72,0xf8,0x55,0x48,0x8d});
        if(!Executable(0x1808e00,0xc30))throw std::runtime_error("native glove pose evaluator is not inside the current executable segment");
        if(!Executable(0xd32610,0x79) || !Executable(0xfc36e0,0xf9))throw std::runtime_error("native glove render publication is not inside the current executable segment");
        Fingerprint(0x1a15930,{0xc6,0x87,0xf5,0x16,0,0,1,0xc3});
        Fingerprint(0x2081040,{0x55,0x48,0x8d,0x05,0xc0,0xac,0x4d,0x02,0x31,0xf6,0x31,0xc9,0x48,0x89,0xe5});
        Fingerprint(0x2081c40,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x49,0x89,0xf4,0x53,0x48,0x89,0xfb});
        Fingerprint(0x2080ac0,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x89,0xcd,0x41,0x54,0x53,0x48,0x89,0xfb});
        Fingerprint(0x2082400,{0x55,0x48,0x8d,0x05,0x00,0x99,0x4d,0x02,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55});
        Fingerprint(0x2086240,{0x55,0x48,0x89,0xe5,0x53,0x48,0x89,0xfb,0x48,0x8d,0xbf,0x10,0x11,0,0});
        Fingerprint(0x1a50e90,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x03,0,0,0x4c,0x8b,0xa7,0xb0,0,0,0});
        Fingerprint(0x1a9d975,{0x48,0x8d,0xbb,0x80,0x15,0,0,0xe8,0x0f,0x35,0xfb,0xff});
        Fingerprint(0x1fb0a80,{0x8b,0x8f,0x98,0x06,0,0,0x83,0xf9,0xff,0x0f,0x84,0x81,0x05,0,0});
        Fingerprint(0xd8db40,{0x48,0x8b,0x87,0xa0,0x04,0,0,0x48,0x8b,0x78,0x38});
        Fingerprint(0x13d94c0,{0x8b,0x87,0xd8,0x10,0,0,0x8b,0x97,0xdc,0x10,0,0,0x48,0xc1,0xe0,0x20,0x48,0x09,0xd0,0xc3});
        Fingerprint(0x1808e00,{0x48,0x8b,0x87,0xe0,0x01,0,0,0x48,0x85,0xc0,0x0f,0x84,0x48,0x01,0,0,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x89,0xfb,0x48,0x81,0xec,0x08,0x42,0,0});
        Fingerprint(0x1809a3d,{0xbe,0x00,0xfc,0x03,0,0x41,0x54,0x53,0x48,0x89,0xfb});
        Fingerprint(0x1809a62,{0x48,0x89,0xdf,0xe8,0x96,0xf3,0xff,0xff,0x44,0x0f,0xb7,0xbb,0xd4,0x01,0,0,0x66,0x41,0x0f,0x6e,0x8d,0xc0,0,0,0});
        Fingerprint(0x18095f1,{0x48,0x8b,0x7b,0x38,0x48,0x8b,0x07,0xff,0x50,0x70,0xbe,0,0,0x04,0,0x49,0x89,0xc6,0x48,0x89,0xc7,0xe8,0xf5,0xf7,0xff,0xff});
        Fingerprint(0xd13470,{0x48,0x89,0xf8,0xc3});
        Fingerprint(0xd32610,{0x40,0x38,0xb7,0xb8,0,0,0,0x74,0x6f,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x49,0x89,0xfc,0x53,0x48,0x83,0xec,0x08});
        Fingerprint(0xd3262d,{0x80,0x7f,0x58,0,0x40,0x88,0xb7,0xb8,0,0,0,0x74,0x3b,0x4c,0x63,0x77,0x38});
        Fingerprint(0x1886226,{0x0f,0xb6,0x70,0x08,0x48,0x8b,0x38,0xe8,0xde,0xc3,0x4a,0xff});
        Fingerprint(0xfc36e0,{0x0f,0xb6,0x46,0x48,0x89,0xc1,0xd0,0xe9,0x83,0xe1,0x01,0x38,0xd1,0x0f,0x84,0xe5,0,0,0,0x83,0xe2,0x01});
        Fingerprint(0xf8d2d2,{0x0f,0xb6,0x50,0x48,0x48,0x8b,0x70,0x20,0xf6,0xc2,0x01,0x74,0xb1,0x48,0x85,0xf6,0x74,0xde});
        Fingerprint(0xf8d2ee,{0x48,0x8b,0x46,0x70,0x48,0x21,0xc1,0x83,0xe2,0x02,0x75,0xac,0x48,0x89,0xc1,0x48,0x0f,0xba,0xe9,0x3c,0xeb,0xa2});
        Fingerprint(0xd8c0f0,{0x8b,0x0f,0x83,0xf9,0xff,0x74,0x41,0x48,0x8b,0x35,0x02,0x6f,0x93,0x03,0x83,0xf9,0xfe,0x74,0x35,0x48,0x85,0xf6,0x74,0x30});
        Fingerprint(0xd8c11e,{0x81,0xe2,0xff,0x01,0,0,0x48,0x6b,0xd2,0x70,0x48,0x01,0xd0,0x3b,0x48,0x10,0x75,0x08,0xc3});
    }
    void Preflight()const{
        if(profile_.buildId!="f1e04e3616921a3bb25940eea9b4b6bfd7ccada5")throw std::runtime_error("gloves require the validated current Linux client build");
        if(!profile_.fields.steamId || !profile_.fields.playerName || !profile_.fields.pawnHandle)throw std::runtime_error("glove controller identity schema unavailable");
        Schema("C_CSPlayerPawn","m_EconGloves",-1,0x16f8);Schema("C_CSPlayerPawn","m_bNeedToReApplyGloves",1,0x16f5);Schema("C_CSPlayerPawn","m_nEconGlovesChanged",1,0x2bb0);Schema("C_CSPlayerPawn","m_hHudModelArms",4,0x2c34);
        Schema("C_BaseEntity","m_hOwnerEntity",4,0x698);Schema("C_BaseEntity","m_pGameSceneNode",8,0x4a0);Schema("CGameSceneNode","m_pOwner",8,0x30);Schema("CGameSceneNode","m_pParent",8,0x38);
        Schema("CSkeletonInstance","m_modelState",-1,0x140);Schema("CModelState","m_hModel",-1,0xa0);
        Schema("C_BaseEntity","m_pRenderComponent",8,0x4a8);Schema("CRenderComponent","m_bEnableRendering",1,0x58);Schema("CRenderComponent","m_bInterpolationReadyToDraw",1,0xb8);
        Schema("C_EconItemView","m_iItemDefinitionIndex",2,0x10c2);Schema("C_EconItemView","m_iEntityQuality",4,0x10c4);Schema("C_EconItemView","m_iEntityLevel",4,0x10c8);Schema("C_EconItemView","m_iItemID",8,0x10d0);Schema("C_EconItemView","m_iItemIDHigh",4,0x10d8);Schema("C_EconItemView","m_iItemIDLow",4,0x10dc);Schema("C_EconItemView","m_iAccountID",4,0x10e0);Schema("C_EconItemView","m_bInitialized",1,0x10f0);Schema("C_EconItemView","m_AttributeList",-1,0x1110);Schema("C_EconItemView","m_NetworkedDynamicAttributes",-1,0x1188);Schema("CAttributeList","m_Attributes",-1,8);Schema("CEconItemAttribute","m_iAttributeDefinitionIndex",2,0x30);Schema("CEconItemAttribute","m_flValue",4,0x34);Schema("CEconItemAttribute","m_flInitialValue",4,0x38);Schema("CEconItemAttribute","m_nRefundableCurrency",4,0x3c);Schema("CEconItemAttribute","m_bSetBonus",1,0x40);
        Instructions();
        Address renderService=0;if(!RenderService(renderService))throw std::runtime_error("current glove render service ABI is unavailable");
    }
    void ReadCatalog(const Json& j){
        if(!j.contains("source") || !j["source"].contains("client_build_id") || j["source"]["client_build_id"]!=profile_.buildId || !j.contains("gloves") || !j["gloves"].is_array() || !j.contains("paint_kits") || !j["paint_kits"].is_array())throw std::runtime_error("private glove catalogue does not match this client build");
        for(const auto& i:j["gloves"]){if(i.value("kind",std::string())!="glove")throw std::runtime_error("non-glove private catalogue entry");const auto id=i.at("item_def").get<unsigned>();if(!id || id>65535 || allowed_.count(id))throw std::runtime_error("invalid glove definition");auto& kits=allowed_[id];for(const auto& k:i.at("paint_kits")){const auto idk=k.get<unsigned>();if(!idk || idk>65535)throw std::runtime_error("invalid glove paint pair");kits.insert(idk);}if(!i.contains("allowed_teams") || !i["allowed_teams"].is_array() || i["allowed_teams"].empty())throw std::runtime_error("glove allowed teams are unavailable");auto& teams=teams_[id];for(const auto& team:i["allowed_teams"]){if(!team.is_number_integer() || (team!=2 && team!=3))throw std::runtime_error("invalid glove allowed team");teams.insert(team.get<unsigned>());}}
        for(const auto& p:j["paint_kits"]){if(p.value("category",std::string())!="glove")continue;const auto id=p.at("id").get<unsigned>();if(!id || id>65535 || paints_.count(id))throw std::runtime_error("invalid glove paint");PaintRange r;if(p.contains("wear_min") && !p["wear_min"].is_null())r.minimum=p["wear_min"].get<float>();if(p.contains("wear_max") && !p["wear_max"].is_null())r.maximum=p["wear_max"].get<float>();if(!std::isfinite(r.minimum) || !std::isfinite(r.maximum) || r.minimum<0 || r.maximum>1 || r.minimum>r.maximum)throw std::runtime_error("invalid glove wear range");paints_.emplace(id,r);}
        if(allowed_.empty() || paints_.empty())throw std::runtime_error("private glove catalogue is empty");
    }
    Paint Resolve(const Json& j)const{Paint p;p.itemDef=j.at("item_def").get<unsigned>();p.kit=j.at("paint_kit").get<unsigned>();p.seed=j.value("seed",0u);const auto a=allowed_.find(p.itemDef),b=allowed_.end();const auto w=paints_.find(p.kit);if(a==b || !a->second.count(p.kit) || w==paints_.end())throw std::invalid_argument("glove paint has no installed catalogue pairing evidence");p.wear=j.contains("wear")?j["wear"].get<float>():std::clamp(0.01f,w->second.minimum,w->second.maximum);if(p.wear<w->second.minimum || p.wear>w->second.maximum)throw std::invalid_argument("glove wear is outside the builtin paint range");return p;}
    bool SystemCurrent()const{Address s=0;return Read(profile_.base+0x4b95610,s) && s==profile_.system;}
    bool ActorCurrent(const Actor& a)const{if(!OnThread() || !SystemCurrent() || a.generation!=generation_)return false;const auto p=ClientEntityProbe::ResolvePlayer(profile_.read,profile_.system,profile_.fields,a.controllerHandle);Address table=0,back=0;return p && p->pawn.instance==a.instance && p->pawn.handle==a.pawnHandle && p->team==a.team && std::to_string(p->steamId)==a.steamId && Read(a.instance,table) && table==profile_.base+0x450bd30 && Read(a.instance+0x1630,back) && back==a.instance && SystemCurrent();}
    bool Current(const Glove& g)const{return ActorCurrent(g.actor);}
    bool VersionCurrent(const Glove& g)const{unsigned char v=0;return Current(g) && Read(g.actor.instance+0x2bb0,v) && v==g.networkVersion;}
    bool ModelReady(Address a)const{Address n=0,b=0,m=0;return Read(a+0x4a0,n) && n && Read(n+0x1e0,b) && b && Read(b,m) && m;}
    bool Skeleton(Address entity,Address& node)const {
        Address table=0,type=0,parentType=0,self=0,model=0,data=0;node=0;
        if(!Read(entity+0x4a0,node) || !node || !Read(node+0x30,self) || self!=entity || !Read(node,table) ||
           !Read(table-8,type) || !Read(table+0x70,self) || self!=profile_.base+0xd13470)return false;
        if(table==profile_.base+0x44e8560){if(type!=profile_.base+0x44e3cc8)return false;}
        else if(table!=profile_.base+0x43ea830 || type!=profile_.base+0x43ea268 || !Read(type+16,parentType) || parentType!=profile_.base+0x44e3cc8)return false;
        return Read(node+0x1e0,model) && model && Read(model,data) && data;
    }
    GlovePoseProbe::Result Pose(Address entity)const{Address node=0;return Skeleton(entity,node)?GlovePoseProbe::Inspect(profile_.read,node):GlovePoseProbe::Result{};}
    GloveRenderProbe::Result Render(Address entity)const{Address node=0;return Skeleton(entity,node)?GloveRenderProbe::Inspect(profile_.read,profile_.base,entity,node):GloveRenderProbe::Result{};}
    GloveRenderProbe::Result ParentRender(Address entity)const{Address node=0;return Skeleton(entity,node)?GloveRenderProbe::InspectParent(profile_.read,profile_.base,entity,node):GloveRenderProbe::Result{};}
    bool RenderService(Address& service)const{Address table=0,type=0;return Read(profile_.base+0x473fc78,service) && service && Read(service,table) && table==profile_.base+0x4417500 && Read(table-8,type) && type==profile_.base+0x44163b8;}
    const char* PublishRender(const Actor& a,bool hudCopy,unsigned& calls) {
        if(!ActorCurrent(a))return "actor_not_current";
        std::uint32_t wh=0,hh=0;Address world=0,hud=0,entity=0,parent=0,node=0,service=0;
        if(!World(a,wh,world))return "world_not_current";
        if(hudCopy){if(!Hud(a,hh,hud) || !HudCopy(a,hud,wh,entity))return "hud_copy_not_current";parent=hud;}else{entity=world;parent=a.instance;}
        const auto parentRender=ParentRender(parent),target=Render(entity);
        if(!parentRender.layoutValid)return "parent_component_unreadable";
        if(!parentRender.enabled)return "parent_render_disabled";
        if(!parentRender.interpolationReady)return "parent_interpolation_pending";
        if(!target.layoutValid)return "target_component_unreadable";
        if(!target.enabled)return "target_render_disabled";
        if(target.interpolationReady)return "interpolation_already_published";
        if(!Pose(entity).ready || !Skeleton(entity,node))return "target_pose_pending";
        if(!RenderService(service))return "render_service_changed";
        Address identity=0;std::uint32_t handle=0;
        if(!Read(entity+0x10,identity) || !identity || !Read(identity+0x10,handle))return "target_identity_unreadable";
        const auto before=ClientEntityProbe::Resolve(profile_.read,profile_.system,handle);
        if(!before || before->instance!=entity)return "target_identity_changed";
        ++calls;reinterpret_cast<void(*)(void*,bool)>(profile_.base+0xd32610)(reinterpret_cast<void*>(target.component),true);
        Address afterService=0;const auto after=ClientEntityProbe::Resolve(profile_.read,profile_.system,handle);
        if(!ActorCurrent(a) || !after || after->instance!=entity || after->entityClass!=before->entityClass || !RenderService(afterService) || afterService!=service)return "native_setter_lifetime_changed";
        std::uint32_t afterWorld=0,afterHud=0;Address afterW=0,afterH=0,afterEntity=0,afterNode=0;
        if(!World(a,afterWorld,afterW) || afterWorld!=wh || afterW!=world)return "native_setter_world_changed";
        if(hudCopy){if(!Hud(a,afterHud,afterH) || afterHud!=hh || afterH!=hud || !HudCopy(a,hud,wh,afterEntity) || afterEntity!=entity)return "native_setter_hud_changed";}else afterEntity=afterW;
        if(!Skeleton(afterEntity,afterNode) || afterNode!=node)return "native_setter_node_changed";
        return Render(afterEntity).ready?"render_ready":"native_setter_called_scene_pending"; // Scene consumer may publish on the following paused render frame.
    }
    // Use the exact native draw-pose evaluator, on the original game thread,
    // after re-resolving this glove's current full-handle and parent chain.
    // A busy worker or pending model remains pending; no cache/mask is forced.
    void FinishPose(const Actor& a,bool hudCopy,unsigned& calls) {
        if(!ActorCurrent(a))return;
        std::uint32_t wh=0,hh=0;Address world=0,hud=0,entity=0,node=0;
        if(!World(a,wh,world) || !ModelReady(world))return;
        if(hudCopy){if(!Hud(a,hh,hud) || !HudCopy(a,hud,wh,entity))return;}else entity=world;
        Address identity=0;std::uint32_t handle=0;int locked=0;
        if(!Read(entity+0x10,identity) || !identity || !Read(identity+0x10,handle))return;
        const auto before=ClientEntityProbe::Resolve(profile_.read,profile_.system,handle);
        if(!before || before->instance!=entity || !Skeleton(entity,node) || !Read(node+0x118,locked) || locked || !GlovePoseProbe::Inspect(profile_.read,node).layoutValid)return;
        ++calls;
        reinterpret_cast<void(*)(void*,std::uint32_t)>(profile_.base+0x1808e00)(reinterpret_cast<void*>(node),0x3fc00u);
        if(!ActorCurrent(a))return;
        const auto after=ClientEntityProbe::Resolve(profile_.read,profile_.system,handle);
        if(!after || after->instance!=entity || after->entityClass!=before->entityClass)return;
        std::uint32_t afterWorld=0,afterHud=0;Address worldInstance=0,hudInstance=0,afterEntity=0,afterNode=0;
        if(!World(a,afterWorld,worldInstance) || afterWorld!=wh || worldInstance!=world)return;
        if(hudCopy){if(!Hud(a,afterHud,hudInstance) || afterHud!=hh || hudInstance!=hud || !HudCopy(a,hud,wh,afterEntity) || afterEntity!=entity)return;}
        else afterEntity=worldInstance;
        if(!Skeleton(afterEntity,afterNode) || afterNode!=node)return;
        // Snapshot reporting independently reads the finished pose. This call
        // cannot turn a failed pose read into an item-ownership failure.
        (void)GlovePoseProbe::Inspect(profile_.read,afterNode);
    }
    void TracePose(const PoseWork& work,const char* reason,bool pending=true) {
        if(poseDiagnostics_.size()>=128 && !poseDiagnostics_.count(work.actor.steamId))poseDiagnostics_.erase(poseDiagnostics_.begin());
        poseDiagnostics_[work.actor.steamId]={{"pending",pending},{"reason",reason},{"calls",work.calls},{"publication_calls",work.publications},{"paused_scope",work.pausedScope},{"scope_tick",work.tick},{"pawn_handle",work.actor.pawnHandle},{"generation",work.actor.generation},{"world_handle",work.worldHandle},{"hud_refreshed",work.hudRefreshed},{"world_publish_reason",work.worldPublishReason},{"hud_publish_reason",work.hudPublishReason}};
    }
    void QueuePose(const Actor& a) {
        PoseWork work;work.actor=a;Json item;if(!ActorCurrent(a)){TracePose(work,"queue_actor_not_current",false);return;}if(!ItemAt(a.instance+0x16f8,false,item)){TracePose(work,"queue_item_unreadable",false);return;}
        const auto now=std::chrono::steady_clock::now();work.item=GloveCosmetics::Semantic(item);work.deadline=now+std::chrono::seconds(5);work.nextAttempt=now;work.tick=tick_;work.pausedScope=paused_;
        World(a,work.worldHandle,work.worldInstance);TracePose(work,"queued");poseWork_[a.steamId]=std::move(work);
    }
    void RecoverMissingWorld(PoseWork& work) {
        const auto& a=work.actor;std::uint32_t handle=0;Json item;
        if(work.calls>29 || !ActorCurrent(a) || !Read(a.instance+0x1580,handle) || !AbsentChild(handle) || !RelationsReady(a) || !ModelReady(a.instance) || !ItemAt(a.instance+0x16f8,false,item) || GloveCosmetics::Semantic(item)!=work.item || item.value("m_bInitialized",0u)!=1)return;
        ++work.calls;reinterpret_cast<void(*)(void*)>(profile_.base+0x1a15930)(reinterpret_cast<void*>(a.instance));
        if(!ActorCurrent(a) || !Read(a.instance+0x1580,handle) || !AbsentChild(handle) || !RelationsReady(a) || !ItemAt(a.instance+0x16f8,false,item) || GloveCosmetics::Semantic(item)!=work.item)return;
        ++work.calls;reinterpret_cast<void(*)(void*)>(profile_.base+0x1a50e90)(reinterpret_cast<void*>(a.instance+0x1580));
        work.worldHandle=0xffffffff;work.worldInstance=0;work.hudRefreshed=false;
    }
    bool FinishHudLink(const Actor& a,unsigned& calls) {
        std::uint32_t wh=0,hh=0;Address world=0,hud=0,node=0;
        if(!ActorCurrent(a) || !World(a,wh,world) || !Skeleton(world,node) || !Hud(a,hh,hud) || !Skeleton(hud,node) || !Parent(hud,a.instance))return false;
        ++calls;reinterpret_cast<void(*)(void*)>(profile_.base+0x1fb0a80)(reinterpret_cast<void*>(hud));
        if(!ActorCurrent(a))return false;
        std::uint32_t afterWorld=0,afterHud=0;Address afterW=0,afterH=0,copy=0;
        return World(a,afterWorld,afterW) && afterWorld==wh && afterW==world && Hud(a,afterHud,afterH) && afterHud==hh && afterH==hud && HudCopy(a,hud,wh,copy);
    }
    void LateRender(){Thread(false);if(demo_)DrainPoses(true);if(afterRender_)afterRender_();}
    void DrainPoses(bool late=false) {
        if(servicing_)return;struct ServiceScope{bool& active;explicit ServiceScope(bool& value):active(value){active=true;}~ServiceScope(){active=false;}} scope(servicing_);
        const auto now=std::chrono::steady_clock::now();
        for(auto it=poseWork_.begin();it!=poseWork_.end();){
            auto& work=it->second;Json item;
            if(work.pausedScope && !paused_){work.pausedScope=false;work.deadline=now+std::chrono::seconds(5);}
            if(!demo_ || !ActorCurrent(work.actor) || (work.pausedScope && tick_!=work.tick) || (!work.pausedScope && now>work.deadline)){TracePose(work,!demo_?"demo_ended":!ActorCurrent(work.actor)?"actor_lifetime_changed":work.pausedScope?"paused_tick_changed":"deadline_elapsed",false);it=poseWork_.erase(it);continue;}
            if(!ItemAt(work.actor.instance+0x16f8,false,item)){TracePose(work,"item_unreadable");++it;continue;}
            if(item.value("m_bInitialized",0u)!=1){TracePose(work,"original_default_hands",false);it=poseWork_.erase(it);continue;}
            if(GloveCosmetics::Semantic(item)!=work.item){TracePose(work,"semantic_item_changed",false);auto& fields=poseDiagnostics_[work.actor.steamId]["changed_fields"];fields=Json::array();const auto current=GloveCosmetics::Semantic(item);for(auto field=work.item.begin();field!=work.item.end() && fields.size()<32;++field)if(!current.contains(field.key()) || current[field.key()]!=field.value())fields.push_back(field.key());it=poseWork_.erase(it);continue;}
            if(!late && now<work.nextAttempt){++it;continue;}if(!late)work.nextAttempt=now+std::chrono::milliseconds(25);
            const bool prepare=!late && now<=work.deadline && work.calls<30;
            std::uint32_t wh=0,hh=0;Address world=0,hud=0,copy=0;
            if(prepare && !World(work.actor,wh,world))RecoverMissingWorld(work);
            if(World(work.actor,wh,world)) {
                if(work.worldInstance && (work.worldHandle!=wh || work.worldInstance!=world)){TracePose(work,"world_child_replaced",false);poseDiagnostics_[work.actor.steamId]["observed_world_handle"]=wh;it=poseWork_.erase(it);continue;}
                work.worldHandle=wh;work.worldInstance=world;
                if(!work.pausedScope && paused_){Json observedItem;const bool itemCurrent=ItemAt(work.actor.instance+0x16f8,false,observedItem) && GloveCosmetics::Semantic(observedItem)==work.item;work.PromotePaused(demo_,paused_,tick_,now,ActorCurrent(work.actor),itemCurrent,wh,world);}
            }
            bool worldReady=World(work.actor,wh,world) && Pose(world).ready;
            if(!worldReady && work.calls<32){FinishPose(work.actor,false,work.calls);worldReady=World(work.actor,wh,world) && Pose(world).ready;}
            const bool naturalWorld=worldReady && Render(world).ready;
            if(late && worldReady && !naturalWorld)work.worldPublishReason=PublishRender(work.actor,false,work.publications);
            if(prepare && !work.hudRefreshed && work.calls<32)work.hudRefreshed=FinishHudLink(work.actor,work.calls);
            bool hudReady=Hud(work.actor,hh,hud) && HudCopy(work.actor,hud,wh,copy) && Pose(copy).ready;
            if(!hudReady && work.calls<32){FinishPose(work.actor,true,work.calls);hudReady=Hud(work.actor,hh,hud) && HudCopy(work.actor,hud,wh,copy) && Pose(copy).ready;}
            if(late && hudReady && !Render(copy).ready)work.hudPublishReason=PublishRender(work.actor,true,work.publications);
            const auto hudParent=Hud(work.actor,hh,hud)?ParentRender(hud):GloveRenderProbe::Result{};
            const bool hudRenderDone=Render(copy).ready || (hudParent.layoutValid && !hudParent.enabled);
            if(late && !paused_ && tick_!=work.tick && naturalWorld && hudReady && hudRenderDone && work.hudRefreshed){TracePose(work,"native_interpolation_settled",false);it=poseWork_.erase(it);}
            else{TracePose(work,worldReady && Render(world).ready && hudReady && hudRenderDone?"post_interpolation_publication_active":!worldReady?"world_pose_pending":!hudReady?"hud_pose_pending":"render_publication_pending",!(worldReady && Render(world).ready && hudReady && hudRenderDone));++it;}
        }
    }
    bool StringAt(Address a,std::size_t max,std::string& out)const{out.clear();for(std::size_t i=0;i<max;++i){char c=0;if(!Read(a+i,c))return false;if(!c)return true;out.push_back(c);}return false;}
    bool AttrsAt(Address list,Address expected,Json& out)const{
        Address table=0,data=0;int count=0;if(!Read(list,table) || table!=profile_.base+expected || !Read(list+8,count) || count<0 || count>128 || !Read(list+16,data) || (count && !data))return false;out=Json::array();std::set<unsigned> unique;
        for(int i=0;i<count;++i){Address table=0;const auto a=data+std::size_t(i)*0x48;std::uint16_t id=0;std::uint32_t value=0,initial=0,refund=0;unsigned char bonus=0;if(!Read(a,table) || table!=profile_.base+0x455b828 || !Read(a+0x30,id) || !id || !unique.insert(id).second || !Read(a+0x34,value) || !Read(a+0x38,initial) || !Read(a+0x3c,refund) || !Read(a+0x40,bonus) || bonus>1)return false;out.push_back({{"definition",id},{"value_bits",value},{"initial_bits",initial},{"refundable",refund},{"set_bonus",bonus}});}
        int after=0;Address ptr=0;return Read(list+8,after) && after==count && Read(list+16,ptr) && ptr==data;
    }
    bool ItemAt(Address a,bool scratch,Json& out)const{
        Address table=0,owned=0,getter=0;if(!Read(a,table) || table!=profile_.base+(scratch?0x455bd08:0x4507da8) || !Read(table+0x98,getter) || getter!=profile_.base+0x13d94c0 || !Read(a+0x10e8,owned) || owned)return false;out=Json::object();
        struct F{const char* name;unsigned off,size;bool signedValue;};
        const F fields[]={{"m_bInventoryImageRgbaRequested",0x70,1,false},{"m_bInventoryImageTriedCache",0x71,1,false},{"m_nInventoryImageRgbaWidth",0x90,4,true},{"m_nInventoryImageRgbaHeight",0x94,4,true},{"m_bRestoreCustomMaterialAfterPrecache",0x10c0,1,false},{"m_iItemDefinitionIndex",0x10c2,2,false},{"m_iEntityQuality",0x10c4,4,true},{"m_iEntityLevel",0x10c8,4,false},{"m_iItemID",0x10d0,8,false},{"m_iItemIDHigh",0x10d8,4,false},{"m_iItemIDLow",0x10dc,4,false},{"m_iAccountID",0x10e0,4,false},{"m_iInventoryPosition",0x10e4,4,false},{"m_bInitialized",0x10f0,1,false},{"m_bDisallowSOC",0x10f1,1,false},{"m_bIsStoreItem",0x10f2,1,false},{"m_bIsTradeItem",0x10f3,1,false},{"m_iEntityQuantity",0x10f4,4,true},{"m_iRarityOverride",0x10f8,4,true},{"m_iQualityOverride",0x10fc,4,true},{"m_iOriginOverride",0x1100,4,true},{"m_ubStyleOverride",0x1104,1,false},{"m_unClientFlags",0x1105,1,false},{"m_bInitializedTags",0x14b0,1,false}};
        for(const auto& f:fields){std::uint64_t v=0;if(!profile_.read(a+f.off,&v,f.size))return false;if(f.signedValue)out[f.name]=std::int32_t(std::uint32_t(v));else out[f.name]=v;}
        for(const auto& pair:std::array<std::pair<const char*,unsigned>,5>{{{"m_szCurrentLoadCachedFileName",0x98},{"m_szCustomName",0x1200},{"m_szCustomNameOverride",0x12a1},{"m_szCustomNameOverride2",0x1342},{"m_szCustomNameOverride3",0x13e3}}}){std::string s;if(!StringAt(a+pair.second,pair.second==0x98?4096:161,s))return false;out[pair.first]=s;}
        return GloveCosmetics::KnownItemId(out) && AttrsAt(a+0x1110,0x455b860,out["attributes"]) && AttrsAt(a+0x1188,0x455b898,out["network_attributes"]);
    }
    bool ItemCurrent(const Glove& g)const{Json item;unsigned char version=0;return Current(g) && ItemAt(g.actor.instance+0x16f8,false,item) && GloveCosmetics::Semantic(item)==GloveCosmetics::Semantic(g.item) && Read(g.actor.instance+0x2bb0,version) && version==g.networkVersion && Current(g);}
    bool Parent(Address entity,Address expected)const{Address node=0,parent=0,owner=0,expectedNode=0;return Read(entity+0x4a0,node) && node && Read(node+0x38,parent) && parent && Read(parent+0x30,owner) && owner==expected && Skeleton(expected,expectedNode) && parent==expectedNode;}
    bool MaterialArray(Address a)const{int count=0;Address ptr=0;return Read(a,count) && count>=0 && count<=128 && Read(a+8,ptr) && (!count || ptr);}
    bool World(const Actor& a,std::uint32_t& h,Address& instance)const{h=0xffffffff;instance=0;if(!ActorCurrent(a) || !Read(a.instance+0x1580,h))return false;const auto x=ClientEntityProbe::Resolve(profile_.read,profile_.system,h);Address table=0;if(!x || x->className!="C_WorldModelGloves" || !Read(x->instance,table) || table!=profile_.base+0x450b378 || !Parent(x->instance,a.instance) || !MaterialArray(x->instance+0xa30) || !MaterialArray(x->instance+0xa48))return false;instance=x->instance;return true;}
    bool Hud(const Actor& a,std::uint32_t& h,Address& instance)const{h=0xffffffff;instance=0;if(!ActorCurrent(a) || !Read(a.instance+0x2c34,h))return false;const auto x=ClientEntityProbe::Resolve(profile_.read,profile_.system,h);Address table=0;std::uint32_t owner=0;if(!x || x->className!="C_CS2HudModelArms" || !Read(x->instance,table) || table!=profile_.base+0x4554588 || !Read(x->instance+0x698,owner) || owner!=a.pawnHandle)return false;unsigned capacity=0;int count=0;Address data=0;if(!Read(x->instance+0x12b4,capacity) || !Read(x->instance+0x12c0,count) || !Read(x->instance+0x12b8,data))return false;capacity&=0x7fffffff;if(capacity>256 || count<0 || unsigned(count)>capacity || (capacity && !data))return false;instance=x->instance;return true;}
    bool AbsentChild(std::uint32_t handle)const{return handle==0xffffffff || ClientEntityProbe::ProvenAbsent(profile_.read,profile_.system,handle);}
    bool RelationsReady(const Actor& a)const{std::uint32_t w=0,h=0;Address world=0,hud=0;if(!Read(a.instance+0x1580,w) || !Read(a.instance+0x2c34,h))return false;return (AbsentChild(w) || World(a,w,world)) && (AbsentChild(h) || Hud(a,h,hud));}
    int Jobs(Address a)const{int n=-1;Address ptr=0;if(!a || !Read(a+0xa48,n) || n<0 || n>128 || !Read(a+0xa50,ptr) || (n && !ptr))return -1;return n;}
    bool HudCopy(const Actor& a,Address hud,std::uint32_t world,Address& copy)const{
        unsigned capacity=0;int count=0;Address data=0;copy=0;if(!ActorCurrent(a) || !Read(hud+0x12b4,capacity) || !Read(hud+0x12c0,count) || !Read(hud+0x12b8,data))return false;capacity&=0x7fffffff;if(capacity>256 || count<0 || unsigned(count)>capacity || (capacity && !data))return false;
        unsigned matches=0;for(unsigned i=0;i<capacity;++i){unsigned hash=0,source=0,handle=0;const auto p=data+16*i;if(!Read(p+4,hash))return false;if(hash!=0x3b3411b3)continue;if(!Read(p+8,source) || !Read(p+12,handle))return false;if(source!=world)continue;const auto x=ClientEntityProbe::Resolve(profile_.read,profile_.system,handle);Address table=0;if(!x || x->className!="C_CS2HudModelAddon" || !Read(x->instance,table) || table!=profile_.base+0x45527b8 || !Parent(x->instance,hud) || !ModelReady(x->instance))return false;copy=x->instance;++matches;}return matches==1;
    }
    std::vector<Glove> Gloves(bool prepare=true){
        Thread(prepare);if(!SystemCurrent()){if(!prepare)throw std::runtime_error("glove observation entity system changed");const auto replacement=Source2ClientEntityProbe::ResolveProfile(factory_,client_);if(replacement.base!=profile_.base)throw std::runtime_error("client module changed during glove session");profile_=replacement;++generation_;controllers_.clear();poseWork_.clear();poseDiagnostics_.clear();nextScan_={};Preflight();}
        if(prepare)DrainPoses();
        const auto now=std::chrono::steady_clock::now();if(now>=nextScan_){controllers_.clear();const auto q=ClientEntityProbe::Inspect(profile_.read,profile_.system,profile_.fields,128);for(const auto& p:q["entries"])if(p.contains("steam_id") && p["steam_id"]!="0" && p.contains("pawn_handle"))controllers_.push_back(p["handle"].get<std::uint32_t>());nextScan_=now+std::chrono::seconds(1);}
        std::vector<Glove> result;for(const auto h:controllers_){const auto p=ClientEntityProbe::ResolvePlayer(profile_.read,profile_.system,profile_.fields,h);if(!p)continue;Glove g;g.actor.steamId=std::to_string(p->steamId);g.actor.nickname=p->nickname;g.actor.team=p->team;g.actor.controllerHandle=h;g.actor.pawnHandle=p->pawn.handle;g.actor.instance=p->pawn.instance;g.actor.generation=generation_;if(!Current(g))continue;unsigned char dirty=0;g.itemReadable=ItemAt(g.actor.instance+0x16f8,false,g.item) && Read(g.actor.instance+0x2bb0,g.networkVersion) && Current(g);g.ready=g.itemReadable && Read(g.actor.instance+0x16f5,dirty) && dirty<=1 && ModelReady(g.actor.instance) && RelationsReady(g.actor);g.itemDef=g.item.value("m_iItemDefinitionIndex",0u);g.needsRefresh=dirty!=0;g.worldReady=World(g.actor,g.worldHandle,g.worldInstance) && ModelReady(g.worldInstance);
            const auto worldPose=g.worldReady?Pose(g.worldInstance):GlovePoseProbe::Result{};g.worldPoseReady=worldPose.ready;g.worldBones=worldPose.bones;g.worldRequestedBones=worldPose.requestedBones;
            const auto worldRender=g.worldReady?Render(g.worldInstance):GloveRenderProbe::Result{};g.worldRenderReady=worldRender.ready;g.worldRenderObjects=worldRender.objects;g.worldHiddenObjects=worldRender.hiddenObjects;g.worldReady=g.worldReady && g.worldPoseReady && g.worldRenderReady;
            g.hudReady=Hud(g.actor,g.hudHandle,g.hudInstance) && ModelReady(g.hudInstance);const auto hudParent=g.hudReady?ParentRender(g.hudInstance):GloveRenderProbe::Result{};if(hudParent.layoutValid){g.hudParentRenderEnabled=hudParent.enabled;g.hudRenderRequired=hudParent.enabled;}g.worldMaterialJobs=Jobs(g.worldInstance);Address copy=0;
            g.hudReady=g.hudReady && HudCopy(g.actor,g.hudInstance,g.worldHandle,copy);const auto hudPose=g.hudReady?Pose(copy):GlovePoseProbe::Result{};g.hudPoseReady=hudPose.ready;g.hudBones=hudPose.bones;g.hudRequestedBones=hudPose.requestedBones;
            const auto hudRender=g.hudReady?Render(copy):GloveRenderProbe::Result{};g.hudRenderReady=hudRender.ready;g.hudRenderObjects=hudRender.objects;g.hudHiddenObjects=hudRender.hiddenObjects;g.hudReady=g.hudReady && g.hudPoseReady && (g.hudRenderReady || !g.hudRenderRequired);g.hudMaterialJobs=Jobs(copy);
            if(g.itemReadable && g.itemDef && g.worldHandle!=0xffffffff && ClientEntityProbe::ProvenAbsent(profile_.read,profile_.system,g.worldHandle))g.reason="glove world child expired; original item snapshot is retained and guarded native rebuild is pending";
            else if(!g.itemReadable)g.reason="embedded glove item is unreadable or its typed/SOC layout is unsupported; original snapshot is retained";
            else if(!g.ready)g.reason="current glove child identity, pawn model or parent relation could not be validated";
            else if(g.itemDef && (!g.worldPoseReady || !g.hudPoseReady))g.reason="native glove world/HUD pose is pending; capture waits for populated bones";
            else if(g.itemDef && (!g.worldRenderReady || (g.hudRenderRequired && !g.hudRenderReady)))g.reason="native glove interpolation/render publication is pending; capture waits for visible scene objects";
            const auto diagnostic=poseDiagnostics_.find(g.actor.steamId);if(diagnostic!=poseDiagnostics_.end() && diagnostic->second.value("pawn_handle",0u)==g.actor.pawnHandle && diagnostic->second.value("generation",std::uint64_t(0))==g.actor.generation)g.finalization=diagnostic->second;
            result.push_back(std::move(g));}
        std::map<std::string,unsigned> counts;for(const auto& g:result)++counts[g.actor.steamId];result.erase(std::remove_if(result.begin(),result.end(),[&](const Glove& g){return counts[g.actor.steamId]!=1;}),result.end());return result;
    }
    std::shared_ptr<Snapshot> Capture(const Glove& g){
        Thread();if(!g.ready || !ItemCurrent(g))return {};unsigned teamMask=(1u<<2)|(1u<<3);
        if(g.item.value("m_bInitialized",0u)){const auto found=teams_.find(g.itemDef);if(found==teams_.end() || !found->second.count(g.actor.team))return {};teamMask=0;for(unsigned team:found->second)teamMask|=1u<<team;}
        auto one=NewScratch(),two=NewScratch();Json a,b;
        reinterpret_cast<void*(*)(void*,const void*)>(profile_.base+0x2081c40)(one.get(),reinterpret_cast<void*>(g.actor.instance+0x16f8));
        if(!ItemAt(reinterpret_cast<Address>(one.get()),true,a) || a.at("m_iItemID")!=GloveCosmetics::CopyCanonical(a).at("m_iItemID") || GloveCosmetics::CopyCanonical(a)!=GloveCosmetics::CopyCanonical(g.item) || !ItemCurrent(g))throw std::runtime_error("first engine copy did not preserve all baseline typed fields/names/attributes");
        reinterpret_cast<void*(*)(void*,const void*)>(profile_.base+0x2081c40)(two.get(),one.get());
        if(!ItemAt(reinterpret_cast<Address>(two.get()),true,b) || b.at("m_iItemID")!=GloveCosmetics::CopyCanonical(b).at("m_iItemID") || GloveCosmetics::CopyCanonical(b)!=GloveCosmetics::CopyCanonical(g.item) || !ItemCurrent(g))throw std::runtime_error("second engine copy did not preserve all baseline typed fields/names/attributes");
        auto snapshot=std::make_shared<Snapshot>();snapshot->item=g.item;snapshot->native=std::move(one);snapshot->teamMask=teamMask;return snapshot;
    }
    bool RefreshModels(const Actor& a){
        if(!ActorCurrent(a) || !ModelReady(a.instance) || !RelationsReady(a))return false;
        reinterpret_cast<void(*)(void*)>(profile_.base+0x1a15930)(reinterpret_cast<void*>(a.instance));if(!ActorCurrent(a))return false;
        reinterpret_cast<void(*)(void*)>(profile_.base+0x1a50e90)(reinterpret_cast<void*>(a.instance+0x1580));if(!ActorCurrent(a))return false;
        QueuePose(a);DrainPoses();
        return ActorCurrent(a);
    }
    bool Apply(const Glove& g,const Paint& p){
        Thread();if(!g.ready || !ItemCurrent(g) || !RelationsReady(g.actor) || !teams_.count(p.itemDef) || !teams_.at(p.itemDef).count(g.actor.team))return false;const auto item=g.actor.instance+0x16f8;
        if(g.itemDef!=p.itemDef || g.item.value("m_bInitialized",0u)!=1){reinterpret_cast<void(*)(void*,int,int,int,std::uint32_t)>(profile_.base+0x2080ac0)(reinterpret_cast<void*>(item),int(p.itemDef),9999,9999,g.item.at("m_iAccountID").get<std::uint32_t>());if(!VersionCurrent(g))return false;}
        const char* names[]={"set item texture prefab","set item texture seed","set item texture wear"};const float values[]={float(p.kit),float(p.seed),p.wear};for(unsigned i=0;i<3;++i){Json before;if(!VersionCurrent(g) || !ItemAt(item,false,before))return false;reinterpret_cast<void(*)(void*,const char*,float)>(profile_.base+0x2086240)(reinterpret_cast<void*>(item),names[i],values[i]);}
        Json after;if(!VersionCurrent(g) || !ItemAt(item,false,after) || !GloveCosmetics::Matches(after,p))return false;if(!RefreshModels(g.actor))return false;Json final;return VersionCurrent(g) && ItemAt(item,false,final) && GloveCosmetics::Semantic(final)==GloveCosmetics::Semantic(after);
    }
    bool Restore(const Glove& g,const Snapshot& snapshot){
        Thread();Json backup;if(!snapshot.native || !g.ready || !ItemCurrent(g) || !RelationsReady(g.actor) || !ItemAt(reinterpret_cast<Address>(snapshot.native.get()),true,backup) || GloveCosmetics::CopyCanonical(backup)!=GloveCosmetics::CopyCanonical(snapshot.item))return false;
        const auto definition=backup.value("m_iItemDefinitionIndex",0u);if(backup.value("m_bInitialized",0u) && (!teams_.count(definition) || !teams_.at(definition).count(g.actor.team)))return false;
        reinterpret_cast<void*(*)(void*,const void*)>(profile_.base+0x2081c40)(reinterpret_cast<void*>(g.actor.instance+0x16f8),snapshot.native.get());Json actual;if(!VersionCurrent(g) || !ItemAt(g.actor.instance+0x16f8,false,actual) || GloveCosmetics::Semantic(actual)!=GloveCosmetics::Semantic(snapshot.item))return false;if(!RefreshModels(g.actor))return false;return VersionCurrent(g) && ItemAt(g.actor.instance+0x16f8,false,actual) && GloveCosmetics::Semantic(actual)==GloveCosmetics::Semantic(snapshot.item);
    }
};
} // namespace cs2_live
#endif
