#pragma once
#include "player_cosmetics.h"
#include "team_palette.h"
#include <cmath>
#include <optional>

namespace cs2_live {
// The state owner is independent of engine memory. Only three local texture
// attributes are owned; the original item definition, inventory IDs, network
// attributes, fallback fields and other players' loadouts are never replaced.
class WeaponCosmetics {
public:
    using Json=nlohmann::json;
    using Attributes=std::array<std::optional<float>,3>; // defs 6, 7 and 8
    enum class RestoreResult {Restored,Expired,Deferred,Rejected};
    struct Paint {unsigned kit=0,seed=0;float wear=0.01f;};
    struct Weapon {
        PlayerCosmetics::Actor actor;
        std::uint32_t handle=0,hudHandle=0;std::uintptr_t instance=0,hudInstance=0;
        std::uint64_t itemId=0;std::uint32_t itemHigh=0,itemLow=0;
        unsigned itemDef=0;bool active=false,ready=false,hudReady=false;
        int worldMaterialJobs=-1,hudMaterialJobs=-1;
        std::string className,reason;Attributes attributes{};
    };
    struct Callbacks {
        std::function<std::vector<Weapon>()> weapons;
        std::function<bool(const Weapon&)> current;
        std::function<Paint(const Json&)> resolve;
        std::function<bool(const Weapon&,const Attributes&)> attributes;
        std::function<RestoreResult(const Weapon&,const Attributes&,const Attributes&)> restoreDetached;
        bool available=false;
        std::string reason="current client weapon paint adapter has not passed preflight";
    };
    void Bind(Callbacks value) {if(!owned_.empty())throw std::logic_error("restore weapon paints before rebinding");callbacks_=std::move(value);}
    void ValidateRequest(const Json& value,bool requireCurrent=false) const {
        ValidateWeaponPaints(value);
        if(value.empty())return;
        if(!callbacks_.available || !callbacks_.resolve)throw std::runtime_error(callbacks_.reason);
        const auto weapons=requireCurrent && callbacks_.weapons?callbacks_.weapons():std::vector<Weapon>{};
        for(const auto& entry:value) {
            callbacks_.resolve(entry);
            if(!requireCurrent)continue;
            bool found=false;
            for(const auto& weapon:weapons)if(weapon.actor.steamId==entry["steam_id"] && weapon.itemDef==entry["item_def"]) {
                if(!weapon.ready || !Current(weapon))throw std::runtime_error(weapon.reason.empty()?"current owned weapon did not pass lifetime/material preflight; no changes applied":weapon.reason);
                found=true;
            }
            if(!found)throw std::runtime_error("selected player does not currently own this item definition; no inventory or model swap was applied");
        }
    }
    void Apply(const Json& value) {ValidateRequest(value);if(desired_!=value)++revision_;desired_=value;Refresh(true);}
    void Refresh(bool active) {
        error_.clear();errors_.clear();std::vector<Weapon> weapons;
        try{if(callbacks_.weapons)weapons=callbacks_.weapons();}catch(const std::exception& e){error_=e.what();stale_=true;return;}
        stale_=weapons.empty();
        bool restoredAny=false,dirtySnapshot=false;std::map<std::uint32_t,Weapon> live;for(const auto& weapon:weapons)live.emplace(weapon.handle,weapon);
        for(auto it=owned_.begin();it!=owned_.end();) {
            const auto current=live.find(it->first);
            if(current==live.end()) {
                const bool newGeneration=std::any_of(weapons.begin(),weapons.end(),[&](const Weapon& weapon){return weapon.actor.generation!=it->second.weapon.actor.generation;});
                if(newGeneration){it=owned_.erase(it);continue;}
                if(callbacks_.restoreDetached) {
                    auto& owned=it->second;
                    if(owned.detachedRejectedRevision && *owned.detachedRejectedRevision==revision_){++it;continue;}
                    const auto result=callbacks_.restoreDetached(owned.weapon,owned.original,owned.last);
                    if(result==RestoreResult::Restored || result==RestoreResult::Expired) {
                        if(result==RestoreResult::Restored)dirtySnapshot=true;
                        it=owned_.erase(it);continue;
                    }
                    if(result==RestoreResult::Rejected)owned.detachedRejectedRevision=revision_;
                }
                ++it;
                continue;
            }
            auto& owned=it->second;const auto& weapon=current->second;
            // A reused entity handle or a new network inventory item is a new
            // lifetime. Never apply a former object's saved attributes to it.
            if(!SameItem(owned.weapon,weapon)){it=owned_.erase(it);continue;}
            if(!Current(weapon) || !weapon.ready){++it;continue;}
            const auto* wanted=active?Override(weapon):nullptr;
            if(!wanted || !SameActor(owned.weapon,weapon)) {
                auto restored=weapon.attributes;
                for(unsigned index=0;index<3;++index)if(restored[index]==owned.last[index])restored[index]=owned.original[index];
                if(restored!=weapon.attributes) {
                    const auto attempt=std::to_string(revision_)+Key(weapon.attributes)+Key(restored);
                    if(owned.restoreAttempt==attempt){errors_[weapon.handle]="original paint restoration was rejected; reselect or refresh the demo";++it;continue;}
                    owned.restoreAttempt=attempt;
                    dirtySnapshot=true;
                    if(!Set(weapon,restored)){errors_[weapon.handle]="client did not confirm original paint restoration";++it;continue;}restoredAny=true;
                }
                it=owned_.erase(it);continue;
            }
            ++it;
        }
        if(restoredAny)try{if(callbacks_.weapons)weapons=callbacks_.weapons();dirtySnapshot=false;}catch(const std::exception& e){error_=e.what();stale_=true;return;}
        for(auto& weapon:weapons) {
            const auto* wanted=active?Override(weapon):nullptr;
            if(!wanted || !weapon.ready || !Current(weapon))continue;
            const auto paint=callbacks_.resolve(*wanted);const auto target=ForPaint(paint);
            if(weapon.attributes==target)continue;
            auto found=owned_.find(weapon.handle);
            if(found==owned_.end())found=owned_.emplace(weapon.handle,Owned{weapon,weapon.attributes,target,{},{},false,{}}).first;
            auto& owned=found->second;
            // After a seek, the demo can reinstall a new baseline on the same
            // entity. Preserve that baseline before applying the override.
            if(!owned.failed && weapon.attributes!=owned.last)owned.original=weapon.attributes;
            const auto attempt=std::to_string(revision_)+Key(weapon.attributes)+Key(target);
            if(owned.attempt==attempt){errors_[weapon.handle]="paint readback failed; reselect or wait for a fresh network item";continue;}
            owned.attempt=attempt;owned.last=target;
            dirtySnapshot=true;owned.failed=!Set(weapon,target);
            if(owned.failed)errors_[weapon.handle]="client did not confirm texture attributes and material refresh";
        }
        // Read after the engine setter: an accepted command is not reported
        // as applied using the stale snapshot captured before the operation.
        if(dirtySnapshot)try{if(callbacks_.weapons)weapons=callbacks_.weapons();}catch(const std::exception& e){error_=e.what();stale_=true;return;}
        rows_=Json::array();
        for(const auto& weapon:weapons) {
            Json row={{"steam_id",weapon.actor.steamId},{"nickname",weapon.actor.nickname},{"team",weapon.actor.team},{"pawn_handle",weapon.actor.pawnHandle},
                {"weapon_handle",weapon.handle},{"item_def",weapon.itemDef},{"active",weapon.active},{"ready",weapon.ready},{"hud_ready",weapon.hudReady},{"applied",false},
                {"material_jobs",{{"world",weapon.worldMaterialJobs},{"hud",weapon.hudMaterialJobs}}},
                {"materials_pending",weapon.worldMaterialJobs!=0 || weapon.hudMaterialJobs!=0}};
            row["local_attributes"]=Json::object();unsigned attributeIndex=0;
            for(const char* name:{"paint_kit","seed","wear"}){const auto& value=weapon.attributes[attributeIndex++];row["local_attributes"][name]=value?Json(*value):Json(nullptr);}
            if(const auto* wanted=active?Override(weapon):nullptr) {
                const auto paint=callbacks_.resolve(*wanted);row["paint_kit"]=paint.kit;row["seed"]=paint.seed;row["wear"]=paint.wear;
                const auto owned=owned_.find(weapon.handle);const bool failed=owned!=owned_.end() && owned->second.failed;
                row["applied"]=weapon.ready && Current(weapon) && !failed && weapon.attributes==ForPaint(paint);
                if(failed)row["error"]="client did not confirm texture attributes and material refresh; reset/reselect is required";
            }
            if(!weapon.reason.empty())row["reason"]=weapon.reason;
            if(errors_.count(weapon.handle))row["error"]=errors_[weapon.handle];
            rows_.push_back(std::move(row));
        }
    }
    Json Capabilities() const {return {{"available",callbacks_.available},{"reason",callbacks_.reason},{"scope","paint on the selected player's currently owned matching item definition"},{"render_verified_1p",false},{"render_verified_3p",false}};}
    Json State() const {
        Json pending=Json::array();for(const auto& entry:owned_) {
            const bool listed=std::any_of(rows_.begin(),rows_.end(),[&](const Json& row){return row["weapon_handle"]==entry.first;});
            if(!listed)pending.push_back({{"steam_id",entry.second.weapon.actor.steamId},{"item_def",entry.second.weapon.itemDef},{"weapon_handle",entry.first},
                {"reason",entry.second.detachedRejectedRevision?"detached weapon restoration was rejected; reset/reselect or refresh the demo":"waiting for exact detached weapon identity and material readiness before restoring"}});
        }
        Json value={{"weapons",rows_},{"capability",Capabilities()},{"stale",stale_},{"owned_overrides",owned_.size()},{"restore_pending",pending}};if(!error_.empty())value["error"]=error_;return value;
    }
    static Attributes ForPaint(const Paint& paint){return {float(paint.kit),float(paint.seed),paint.wear};}
private:
    struct Owned {Weapon weapon;Attributes original,last;std::string attempt,restoreAttempt;bool failed=false;std::optional<std::uint64_t> detachedRejectedRevision;};
    Callbacks callbacks_;Json desired_=Json::array(),rows_=Json::array();std::map<std::uint32_t,Owned> owned_;std::map<std::uint32_t,std::string> errors_;std::uint64_t revision_=0;bool stale_=true;std::string error_;
    bool Current(const Weapon& weapon)const{return callbacks_.current && callbacks_.current(weapon);}
    bool Set(const Weapon& weapon,const Attributes& attributes)const{return Current(weapon) && callbacks_.attributes && callbacks_.attributes(weapon,attributes);}
    const Json* Override(const Weapon& weapon)const{for(const auto& entry:desired_)if(entry["steam_id"]==weapon.actor.steamId && entry["item_def"]==weapon.itemDef)return &entry;return nullptr;}
    static bool SameActor(const Weapon& a,const Weapon& b){return a.actor.steamId==b.actor.steamId && a.actor.instance==b.actor.instance && a.actor.pawnHandle==b.actor.pawnHandle && a.actor.controllerHandle==b.actor.controllerHandle;}
    static bool SameItem(const Weapon& a,const Weapon& b){return a.handle==b.handle && a.instance==b.instance && a.actor.generation==b.actor.generation && a.itemDef==b.itemDef && a.itemId==b.itemId && a.itemHigh==b.itemHigh && a.itemLow==b.itemLow;}
    static std::string Key(const Attributes& value){std::string result;for(const auto& item:value)result+=item?std::to_string(*item)+",":"-,";return result;}
};
} // namespace cs2_live

#ifdef CS2_CAMERA_SOURCE2_CVARS
#include <chrono>
#include <memory>
namespace cs2_live {
class Source2WeaponCosmetics {
public:
    using Address=ClientEntityProbe::Address;using Weapon=WeaponCosmetics::Weapon;using Actor=PlayerCosmetics::Actor;using Attributes=WeaponCosmetics::Attributes;using Json=nlohmann::json;
    static WeaponCosmetics::Callbacks Bind(CreateInterfaceFn factory,void* client) {
        auto runtime=std::make_shared<Source2WeaponCosmetics>();WeaponCosmetics::Callbacks result;
        try {
            if(!OfflineLaunch())throw std::runtime_error("weapon paint requires the isolated -insecure editor or cosmetics capture process");
            runtime->factory_=factory;runtime->client_=client;runtime->profile_=Source2ClientEntityProbe::ResolveProfile(factory,client);
            runtime->Preflight();
            const char* path=std::getenv("CS2_CAMERA_COSMETICS_CATALOG");if(!path || !*path)throw std::runtime_error("builtin weapon/paint catalogue is unavailable");
            const auto catalog=CosmeticsCatalog::ReadJson(path);runtime->ReadCatalog(catalog);
            result.weapons=[runtime]{return runtime->Weapons();};result.current=[runtime](const Weapon& weapon){return runtime->Current(weapon);};
            result.resolve=[runtime](const Json& entry){return runtime->Resolve(entry);};result.attributes=[runtime](const Weapon& weapon,const Attributes& attributes){return runtime->Set(weapon,attributes);};
            result.restoreDetached=[runtime](const Weapon& weapon,const Attributes& original,const Attributes& last){return runtime->RestoreDetached(weapon,original,last);};
            result.available=true;result.reason="strict client schema, ABI and owned-entity preflight passed; rendered result requires verification";
        }catch(const std::exception& error){result.reason=error.what();}
        return result;
    }
private:
    struct PaintRange {float minimum=0,maximum=1;};
    Source2ClientEntityProbe::Profile profile_;CreateInterfaceFn factory_=nullptr;void* client_=nullptr;
    std::uint64_t generation_=1;std::vector<std::uint32_t> controllers_;std::chrono::steady_clock::time_point nextScan_{};
    std::map<unsigned,std::set<unsigned>> allowed_;std::map<unsigned,PaintRange> paints_;
    static bool OfflineLaunch() {
        const char* live=std::getenv("CS2_CAMERA_LIVE");const char* capture=std::getenv("CS2_CAMERA_CAPTURE_COSMETICS");
        if((!live || std::strcmp(live,"1")) && (!capture || std::strcmp(capture,"1")))return false;
        const int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);if(fd<0)return false;std::array<char,65536> bytes{};const auto count=read(fd,bytes.data(),bytes.size());close(fd);
        if(count<=0 || count==ssize_t(bytes.size()))return false;
        for(std::size_t cursor=0;cursor<std::size_t(count);){const auto size=strnlen(bytes.data()+cursor,std::size_t(count)-cursor);if(size==std::size_t(count)-cursor)return false;if(std::string(bytes.data()+cursor,size)=="-insecure")return true;cursor+=size+1;}return false;
    }
    template<class T>bool Read(Address address,T& value)const{return ClientEntityProbe::Value(profile_.read,address,value);}
    void Fingerprint(Address offset,std::initializer_list<unsigned char> expected)const {std::vector<unsigned char> value(expected.size());if(!profile_.read(profile_.base+offset,value.data(),value.size()) || !std::equal(value.begin(),value.end(),expected.begin()))throw std::runtime_error("client weapon paint instruction fingerprint differs from the validated Linux build");}
    void Schema(const char* cls,const char* field,int size,unsigned expected)const{const auto value=Source2ClientEntityProbe::SchemaField(profile_.scope,cls,field,size);if(!value || *value!=expected)throw std::runtime_error(std::string("runtime weapon paint schema differs: ")+cls+"."+field);}
    void Preflight()const {
        if(profile_.buildId!="f1e04e3616921a3bb25940eea9b4b6bfd7ccada5")throw std::runtime_error("weapon paint material adapter requires the validated current Linux client build");
        if(!profile_.fields.steamId || !profile_.fields.playerName || !profile_.fields.pawnHandle)throw std::runtime_error("client controller identity schema is unavailable");
        Schema("C_BasePlayerPawn","m_pWeaponServices",8,0x1278);Schema("CPlayer_WeaponServices","m_hMyWeapons",-1,0x48);Schema("CPlayer_WeaponServices","m_hActiveWeapon",4,0x60);
        Schema("C_BaseEntity","m_hOwnerEntity",4,0x698);Schema("C_EconEntity","m_AttributeManager",-1,0x1218);Schema("C_AttributeContainer","m_Item",-1,0x50);
        Schema("C_CSWeaponBase","m_bClearWeaponIdentifyingUGC",1,0x29b0);
        Schema("C_EconItemView","m_iItemDefinitionIndex",2,0x10c2);Schema("C_EconItemView","m_bInitialized",1,0x10f0);Schema("C_EconItemView","m_AttributeList",-1,0x1110);
        Schema("CAttributeList","m_Attributes",-1,8);Schema("CEconItemAttribute","m_iAttributeDefinitionIndex",2,0x30);Schema("CEconItemAttribute","m_flValue",4,0x34);Schema("CEconItemAttribute","m_bSetBonus",1,0x40);
        Fingerprint(0x2086240,{0x55,0x48,0x89,0xe5,0x53,0x48,0x89,0xfb,0x48,0x8d,0xbf,0x10,0x11,0x00,0x00,0x48,0x83,0xec,0x08,0xe8,0x88,0xfc,0xff,0xff});
        Fingerprint(0x2080120,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x49,0x89,0xfe,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28,0x44,0x8b,0xbf,0x18,0x11,0x00,0x00});
        Fingerprint(0x2080170,{0x66,0x42,0x3b,0x74,0x20,0x30,0x75,0xe8});
        Fingerprint(0x14d59d0,{0x48,0x8b,0x05,0xb1,0x08,0x2f,0x03,0x80,0x78,0x58,0x00,0x75,0x03,0xc3});
        Fingerprint(0x14d5cf2,{0x48,0x81,0xc4,0xa0,0x00,0x00,0x00,0x41,0x0f,0xb6,0xcd,0x41,0x0f,0xb6,0xd4,0x4c,0x8d,0x83,0x10,0x29,0x00,0x00,0x4c,0x89,0xf6,0x48,0x89,0xdf});
        Fingerprint(0x14d5910,{0x48,0x85,0xff,0x74,0x0d,0x48,0x8b,0x05,0x6c,0x09,0x2f,0x03,0x80,0x78,0x58,0x00,0x75,0x06,0xc3});
        Fingerprint(0x1433f80,{0x48,0x8d,0x87,0x18,0x12,0x00,0x00,0xc3});
        Fingerprint(0x421a780,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x49,0x89,0xfd,0x41,0x54,0x53,0x48,0x83,0xec,0x28,0x89,0x75,0xbc,0xe8,0xb4,0xb2,0xfd,0xff});
        Fingerprint(0x14d5db6,{0x48,0x8d,0xbb,0x90,0x07,0x00,0x00,0xbe,0x01,0x00,0x00,0x00,0xe8,0xb9,0x49,0xd4,0x02});
    }
    void ReadCatalog(const Json& catalog) {
        if(!catalog.contains("source") || !catalog["source"].contains("client_build_id") || catalog["source"]["client_build_id"]!=profile_.buildId)throw std::runtime_error("builtin weapon catalogue does not match the current client build ID");
        if(!catalog.contains("weapons") || !catalog["weapons"].is_array() || !catalog.contains("paint_kits") || !catalog["paint_kits"].is_array())throw std::runtime_error("builtin weapon/paint catalogue is incomplete");
        for(const auto& item:catalog["weapons"]) {
            const auto kind=item.value("kind",std::string());if(kind!="weapon" && kind!="knife")continue;
            const auto id=item.at("item_def").get<unsigned>();if(!id || id>65535 || allowed_.count(id))throw std::runtime_error("invalid builtin weapon definition");
            auto& paints=allowed_[id];for(const auto& paint:item.at("paint_kits")){const auto kit=paint.get<unsigned>();if(!kit || kit>65535)throw std::runtime_error("invalid builtin paint definition");paints.insert(kit);}
        }
        for(const auto& paint:catalog["paint_kits"]) {
            const auto id=paint.at("id").get<unsigned>();if(!id)continue;if(id>65535 || paints_.count(id))throw std::runtime_error("invalid builtin paint catalogue");
            PaintRange range;if(paint.contains("wear_min") && !paint["wear_min"].is_null())range.minimum=paint["wear_min"].get<float>();if(paint.contains("wear_max") && !paint["wear_max"].is_null())range.maximum=paint["wear_max"].get<float>();
            if(!std::isfinite(range.minimum) || !std::isfinite(range.maximum) || range.minimum<0 || range.maximum>1 || range.minimum>range.maximum)throw std::runtime_error("invalid builtin paint wear bounds");
            paints_.emplace(id,range);
        }
        if(allowed_.empty() || paints_.empty())throw std::runtime_error("builtin weapon catalogue is empty");
    }
    WeaponCosmetics::Paint Resolve(const Json& entry)const {
        const auto item=entry.at("item_def").get<unsigned>(),kit=entry.at("paint_kit").get<unsigned>();const auto found=allowed_.find(item);const auto paint=paints_.find(kit);
        if(found==allowed_.end() || !found->second.count(kit) || paint==paints_.end())throw std::invalid_argument("paint kit is not associated with this builtin item definition");
        WeaponCosmetics::Paint result;result.kit=kit;result.seed=entry.value("seed",0u);result.wear=entry.contains("wear")?entry["wear"].get<float>():std::clamp(0.01f,paint->second.minimum,paint->second.maximum);
        if(result.wear<paint->second.minimum || result.wear>paint->second.maximum)throw std::invalid_argument("wear is outside the selected paint kit's builtin range");
        return result;
    }
    bool SystemCurrent()const{Address system=0;return Read(profile_.base+0x4b95610,system) && system==profile_.system;}
    bool ActorCurrent(const Actor& actor)const {
        if(!SystemCurrent() || actor.generation!=generation_)return false;
        const auto player=ClientEntityProbe::ResolvePlayer(profile_.read,profile_.system,profile_.fields,actor.controllerHandle);
        return player && player->pawn.handle==actor.pawnHandle && player->pawn.instance==actor.instance && std::to_string(player->steamId)==actor.steamId && player->team==actor.team && SystemCurrent();
    }
    bool WeaponAbi(Address instance)const {
        Address table=0,itemTable=0,accessor=0,setter=0,type=0;
        return Read(instance,table) && table>=profile_.base+0x4400000 && table<profile_.base+0x4600000 && Read(table+0x988,accessor) && accessor==profile_.base+0x1433f80 &&
            Read(table+0x7f0,setter) && setter==profile_.base+0x16683e0 && Read(table+0xc50,type) && (type==profile_.base+0x1434430 || type==profile_.base+0x14d7a00) &&
            Read(instance+0x1268,itemTable) && itemTable==profile_.base+0x4556c60;
    }
    bool AttributesAt(Address item,Attributes& result)const {
        Address table=0,data=0;int count=0;result={};
        if(!Read(item+0x1110,table) || table!=profile_.base+0x455b860 || !Read(item+0x1118,count) || count<0 || count>128 || !Read(item+0x1120,data) || (count && !data))return false;
        for(int index=0;index<count;++index) {
            const auto slot=data+std::size_t(index)*0x48;Address attributeTable=0;std::uint16_t definition=0;unsigned char bonus=0;float value=0;
            if(!Read(slot,attributeTable) || attributeTable!=profile_.base+0x455b828 || !Read(slot+0x30,definition) || !Read(slot+0x34,value) || !Read(slot+0x40,bonus))return false;
            if(definition>=6 && definition<=8) {
                if(bonus || !std::isfinite(value) || result[definition-6] || value<0 ||
                    (definition==6 && (value>65535 || std::floor(value)!=value)) || (definition==7 && (value>1000 || std::floor(value)!=value)) || (definition==8 && value>1))return false;
                result[definition-6]=value;
            }
        }
        int after=0;Address afterData=0;return Read(item+0x1118,after) && after==count && Read(item+0x1120,afterData) && afterData==data;
    }
    bool Owns(const Actor& actor,std::uint32_t weaponHandle)const {
        Address service=0,data=0;int count=0;
        if(!ActorCurrent(actor) || !Read(actor.instance+0x1278,service) || !service || !Read(service+0x48,count) || count<0 || count>64 || !Read(service+0x50,data) || (count && !data))return false;
        unsigned matches=0;for(int index=0;index<count;++index){std::uint32_t handle=0;if(!Read(data+4*index,handle))return false;if(handle==weaponHandle)++matches;}
        Address currentService=0;return matches==1 && Read(actor.instance+0x1278,currentService) && currentService==service && ActorCurrent(actor);
    }
    bool Current(const Weapon& weapon)const {
        if(!Owns(weapon.actor,weapon.handle))return false;
        std::uint32_t owner=0;return ItemCurrent(weapon) && Read(weapon.instance+0x698,owner) && owner==weapon.actor.pawnHandle;
    }
    bool ItemCurrent(const Weapon& weapon)const {
        if(!SystemCurrent() || weapon.actor.generation!=generation_)return false;
        const auto entity=ClientEntityProbe::Resolve(profile_.read,profile_.system,weapon.handle);if(!entity || entity->instance!=weapon.instance || entity->className!=weapon.className || !WeaponAbi(weapon.instance))return false;
        std::uint16_t definition=0;std::uint64_t id=0;std::uint32_t high=0,low=0;unsigned char initialized=0;
        return Read(weapon.instance+0x1268+0x10c2,definition) && definition==weapon.itemDef &&
            Read(weapon.instance+0x1268+0x10d0,id) && id==weapon.itemId && Read(weapon.instance+0x1268+0x10d8,high) && high==weapon.itemHigh &&
            Read(weapon.instance+0x1268+0x10dc,low) && low==weapon.itemLow && Read(weapon.instance+0x1268+0x10f0,initialized) && initialized==1 && SystemCurrent();
    }
    bool ExpiredIdentity(const Weapon& weapon)const {
        if(!SystemCurrent())return false;
        if(weapon.actor.generation!=generation_)return true;
        const auto index=weapon.handle&0x7fff;Address page=0,instance=0;std::uint32_t handle=0;
        if(!Read(profile_.system+0x10+8*(index/512),page))return false;
        if(!page)return SystemCurrent();
        const auto identity=page+0x70*(index%512);
        Address currentPage=0,afterInstance=0;std::uint32_t afterHandle=0;
        if(!Read(identity,instance) || !Read(identity+0x10,handle) ||
            !Read(profile_.system+0x10+8*(index/512),currentPage) || currentPage!=page ||
            !Read(identity,afterInstance) || instance!=afterInstance || !Read(identity+0x10,afterHandle) || handle!=afterHandle || !SystemCurrent())return false;
        if(instance!=weapon.instance || handle!=weapon.handle)return true;
        const auto entity=ClientEntityProbe::Resolve(profile_.read,profile_.system,weapon.handle);
        if(!entity || entity->instance!=weapon.instance)return false;
        if(entity->className!=weapon.className)return SystemCurrent();
        if(!WeaponAbi(weapon.instance))return false;
        // A new network inventory item on the same world entity is also a
        // completed former lifetime. A stable changed tuple releases our
        // journal without applying its former baseline to the new item.
        const auto item=weapon.instance+0x1268;
        std::uint16_t definition=0,afterDefinition=0;std::uint64_t id=0,afterId=0;std::uint32_t high=0,low=0,afterHigh=0,afterLow=0;
        return Read(item+0x10c2,definition) && Read(item+0x10d0,id) && Read(item+0x10d8,high) && Read(item+0x10dc,low) &&
            (definition!=weapon.itemDef || id!=weapon.itemId || high!=weapon.itemHigh || low!=weapon.itemLow) &&
            Read(item+0x10c2,afterDefinition) && definition==afterDefinition && Read(item+0x10d0,afterId) && id==afterId &&
            Read(item+0x10d8,afterHigh) && high==afterHigh && Read(item+0x10dc,afterLow) && low==afterLow &&
            ClientEntityProbe::StillCurrent(profile_.read,*entity) && SystemCurrent();
    }
    WeaponCosmetics::RestoreResult RestoreDetached(const Weapon& weapon,const Attributes& original,const Attributes& last)const {
        using Result=WeaponCosmetics::RestoreResult;
        if(!ItemCurrent(weapon))return ExpiredIdentity(weapon)?Result::Expired:Result::Deferred;
        // This path cleans up an already-owned effect only. A currently owned
        // selected-player weapon continues to use the ordinary strict setter.
        if(Owns(weapon.actor,weapon.handle))return Result::Deferred;
        Attributes before{};if(!AttributesAt(weapon.instance+0x1268,before))return Result::Deferred;
        auto target=before;for(unsigned index=0;index<3;++index)if(target[index]==last[index])target[index]=original[index];
        if(target==before)return Result::Restored;
        if(!MaterialsReady(weapon.instance))return Result::Deferred;
        const auto item=reinterpret_cast<void*>(weapon.instance+0x1268);const char* names[]={"set item texture prefab","set item texture seed","set item texture wear"};
        for(unsigned index=0;index<3;++index) {
            if(before[index]==target[index])continue;
            if(!ItemCurrent(weapon))return Result::Rejected;
            if(target[index])reinterpret_cast<void(*)(void*,const char*,float)>(profile_.base+0x2086240)(item,names[index],*target[index]);
            else reinterpret_cast<void(*)(void*,std::uint16_t)>(profile_.base+0x2080120)(item,std::uint16_t(index+6));
        }
        Attributes after{};if(!ItemCurrent(weapon) || !AttributesAt(weapon.instance+0x1268,after) || after!=target)return Result::Rejected;
        reinterpret_cast<void(*)(void*,bool)>(profile_.base+0x421a780)(reinterpret_cast<void*>(weapon.instance+0x790),true);
        if(!ItemCurrent(weapon))return Result::Rejected;
        reinterpret_cast<void(*)(void*,bool)>(profile_.base+0x14d59d0)(reinterpret_cast<void*>(weapon.instance),false);
        if(!ItemCurrent(weapon))return Result::Rejected;
        Weapon refreshed=weapon;
        if(Read(weapon.instance+0x2820,refreshed.hudHandle)) {
            const auto hud=ClientEntityProbe::Resolve(profile_.read,profile_.system,refreshed.hudHandle);
            if(hud) {
                refreshed.hudInstance=hud->instance;unsigned char clearIdentifyingUgc=0;
                if(HudCurrent(refreshed) && MaterialsReady(refreshed.hudInstance) && Read(weapon.instance+0x29b0,clearIdentifyingUgc) && clearIdentifyingUgc<=1) {
                    reinterpret_cast<void(*)(void*,bool)>(profile_.base+0x421a780)(reinterpret_cast<void*>(refreshed.hudInstance+0x790),true);
                    if(!ItemCurrent(refreshed) || !HudCurrent(refreshed))return Result::Rejected;
                    reinterpret_cast<void(*)(void*,void*,bool,bool,void*)>(profile_.base+0x14d5910)(reinterpret_cast<void*>(refreshed.hudInstance),item,false,bool(clearIdentifyingUgc),nullptr);
                }
            }
        }
        return ItemCurrent(weapon) && AttributesAt(weapon.instance+0x1268,after) && after==target?Result::Restored:Result::Rejected;
    }
    bool HudCurrent(const Weapon& weapon)const {
        std::uint32_t handle=0,owner=0;Address table=0;
        if(!weapon.hudInstance || !Read(weapon.instance+0x2820,handle) || handle!=weapon.hudHandle)return false;
        const auto hud=ClientEntityProbe::Resolve(profile_.read,profile_.system,handle);
        return hud && hud->instance==weapon.hudInstance && hud->className=="C_CS2HudModelWeapon" && Read(hud->instance,table) && table==profile_.base+0x4553b70 && Read(hud->instance+0x698,owner) && owner==weapon.handle;
    }
    bool SkinsEnabled()const {Address flag=0,name=0;unsigned char enabled=0;return Read(profile_.base+0x47c6288,flag) && flag && Read(flag,name) && ClientEntityProbe::Text(profile_.read,name,32)=="weapon_skins" && Read(flag+0x58,enabled) && enabled==1;}
    bool MaterialsReady(Address instance)const {
        Address node=0,binding=0,model=0;
        return SkinsEnabled() && Read(instance+0x4a0,node) && node && Read(node+0x1e0,binding) && binding && Read(binding,model) && model &&
            MaterialArray(instance+0xa30) && MaterialArray(instance+0xa48);
    }
    bool MaterialArray(Address vector)const {
        int count=0;Address data=0;
        if(!Read(vector,count) || count<0 || count>128 || !Read(vector+8,data) || (count && !data))return false;
        if(!count)return true;
        std::array<Address,128> entries{};
        return profile_.read(data,entries.data(),std::size_t(count)*sizeof(Address));
    }
    std::vector<Weapon> Weapons() {
        if(!SystemCurrent()) {const auto replacement=Source2ClientEntityProbe::ResolveProfile(factory_,client_);if(replacement.base!=profile_.base)throw std::runtime_error("client module changed during weapon paint session");profile_=replacement;++generation_;controllers_.clear();nextScan_={};Preflight();}
        const auto now=std::chrono::steady_clock::now();
        if(now>=nextScan_) {controllers_.clear();const auto players=ClientEntityProbe::Inspect(profile_.read,profile_.system,profile_.fields,128);for(const auto& entry:players["entries"])if(entry.contains("steam_id") && entry["steam_id"]!="0" && entry.contains("pawn_handle"))controllers_.push_back(entry["handle"].get<std::uint32_t>());nextScan_=now+std::chrono::seconds(1);}
        std::vector<Weapon> result;std::set<std::uint32_t> unique;
        for(const auto handle:controllers_) {
            const auto player=ClientEntityProbe::ResolvePlayer(profile_.read,profile_.system,profile_.fields,handle);if(!player)continue;
            Actor actor;actor.steamId=std::to_string(player->steamId);actor.nickname=player->nickname;actor.team=player->team;actor.controllerHandle=handle;actor.pawnHandle=player->pawn.handle;actor.instance=player->pawn.instance;actor.generation=generation_;
            Address service=0,data=0;int count=0;std::uint32_t active=0;
            if(!Read(actor.instance+0x1278,service) || !service || !Read(service+0x48,count) || count<0 || count>64 || !Read(service+0x50,data) || (count && !data) || !Read(service+0x60,active))continue;
            for(int index=0;index<count;++index) {
                std::uint32_t weaponHandle=0;if(!Read(data+index*4,weaponHandle) || !unique.insert(weaponHandle).second)continue;
                const auto entity=ClientEntityProbe::Resolve(profile_.read,profile_.system,weaponHandle);if(!entity || !WeaponAbi(entity->instance))continue;
                Weapon weapon;weapon.actor=actor;weapon.handle=weaponHandle;weapon.instance=entity->instance;weapon.className=entity->className;weapon.active=active==weaponHandle;
                std::uint16_t definition=0;const auto item=weapon.instance+0x1268;
                if(!Read(item+0x10c2,definition) || !allowed_.count(definition) || !Read(item+0x10d0,weapon.itemId) || !Read(item+0x10d8,weapon.itemHigh) || !Read(item+0x10dc,weapon.itemLow))continue;
                weapon.itemDef=definition;
                if(Read(weapon.instance+0x2820,weapon.hudHandle)){const auto hud=ClientEntityProbe::Resolve(profile_.read,profile_.system,weapon.hudHandle);if(hud)weapon.hudInstance=hud->instance;}
                weapon.hudReady=HudCurrent(weapon) && MaterialsReady(weapon.hudInstance);
                weapon.ready=Current(weapon) && AttributesAt(item,weapon.attributes) && MaterialsReady(weapon.instance) && weapon.hudReady;
                int worldJobs=-1,hudJobs=-1;
                if(weapon.ready && Read(weapon.instance+0xa48,worldJobs) && Read(weapon.hudInstance+0xa48,hudJobs) &&
                    worldJobs>=0 && worldJobs<=128 && hudJobs>=0 && hudJobs<=128) {
                    weapon.worldMaterialJobs=worldJobs;weapon.hudMaterialJobs=hudJobs;
                }
                if(!weapon.ready)weapon.reason="owned weapon/HUD model/materials are not current; wait for the player and weapon model to be available";
                result.push_back(std::move(weapon));
            }
        }
        std::map<std::string,std::set<std::uint32_t>> owners;
        for(const auto& weapon:result)owners[weapon.actor.steamId].insert(weapon.actor.controllerHandle);
        result.erase(std::remove_if(result.begin(),result.end(),[&](const Weapon& weapon){return owners[weapon.actor.steamId].size()!=1;}),result.end());
        return result;
    }
    bool Set(const Weapon& weapon,const Attributes& attributes)const {
        Attributes before{};if(!weapon.ready || !Current(weapon) || !HudCurrent(weapon) || !MaterialsReady(weapon.instance) || !MaterialsReady(weapon.hudInstance) || !AttributesAt(weapon.instance+0x1268,before))return false;
        const auto item=reinterpret_cast<void*>(weapon.instance+0x1268);const char* names[]={"set item texture prefab","set item texture seed","set item texture wear"};
        for(unsigned index=0;index<3;++index) {
            if(before[index]==attributes[index])continue;
            if(!Current(weapon) || !HudCurrent(weapon))return false;
            if(attributes[index])reinterpret_cast<void(*)(void*,const char*,float)>(profile_.base+0x2086240)(item,names[index],*attributes[index]);
            else reinterpret_cast<void(*)(void*,std::uint16_t)>(profile_.base+0x2080120)(item,std::uint16_t(index+6));
        }
        Attributes after{};if(!Current(weapon) || !HudCurrent(weapon) || !AttributesAt(weapon.instance+0x1268,after) || after!=attributes)return false;
        unsigned char clearIdentifyingUgc=0;if(!Read(weapon.instance+0x29b0,clearIdentifyingUgc) || clearIdentifyingUgc>1)return false;
        // These are the client's own bounded material paths. The console
        // regenerate command's global entity enumeration is never called.
        // The native regenerate command releases the entity's material arrays
        // through its component before rebuilding. This also cancels pending
        // jobs through the engine and releases its own reference counts.
        reinterpret_cast<void(*)(void*,bool)>(profile_.base+0x421a780)(reinterpret_cast<void*>(weapon.instance+0x790),true);
        if(!Current(weapon))return false;
        reinterpret_cast<void(*)(void*,bool)>(profile_.base+0x14d59d0)(reinterpret_cast<void*>(weapon.instance),false);
        if(!Current(weapon))return false;
        // Reacquire the full HUD handle after world regeneration. The engine
        // may have recreated it; no former HUD pointer is used after this.
        Weapon refreshed=weapon;
        if(!Read(weapon.instance+0x2820,refreshed.hudHandle))return false;
        const auto hud=ClientEntityProbe::Resolve(profile_.read,profile_.system,refreshed.hudHandle);
        if(!hud)return false;
        refreshed.hudInstance=hud->instance;
        if(!HudCurrent(refreshed) || !MaterialsReady(refreshed.hudInstance))return false;
        reinterpret_cast<void(*)(void*,bool)>(profile_.base+0x421a780)(reinterpret_cast<void*>(refreshed.hudInstance+0x790),true);
        if(!Current(refreshed) || !HudCurrent(refreshed))return false;
        reinterpret_cast<void(*)(void*,void*,bool,bool,void*)>(profile_.base+0x14d5910)(reinterpret_cast<void*>(refreshed.hudInstance),item,false,bool(clearIdentifyingUgc),nullptr);
        return Current(refreshed) && HudCurrent(refreshed) && AttributesAt(weapon.instance+0x1268,after) && after==attributes;
    }
};
} // namespace cs2_live
#endif
