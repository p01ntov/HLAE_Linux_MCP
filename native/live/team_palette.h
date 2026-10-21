#pragma once

// Read-only discovery for the validated Linux CLIENT entity system. No entity
// pointers or fields are exposed to the socket and this adapter performs no
// writes. A palette setter is deliberately separate from proving this lookup.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cs2_live {
class ClientEntityProbe {
public:
    using Json=nlohmann::json;
    using Address=std::uintptr_t;
    using Read=std::function<bool(Address,void*,std::size_t)>;
    static constexpr unsigned MaxIndex=0x7ffe,PageEntries=512,IdentitySize=0x70;
    struct Fields {
        unsigned team=0,renderColor=0;
        std::optional<unsigned> playerName,steamId,pawnHandle;
    };
    struct Identity {
        Address address=0,instance=0,entityClass=0,system=0;
        std::uint32_t handle=0,flags=0;
        std::string className;
    };
    struct PlayerIdentity {Identity controller,pawn;std::uint64_t steamId=0;std::string nickname;unsigned team=0;};
    static std::optional<PlayerIdentity> ResolvePlayer(const Read& read,Address system,const Fields& fields,std::uint32_t controllerHandle) {
        if(!fields.steamId || !fields.pawnHandle || !fields.playerName || !fields.team)return {};
        const auto controller=Resolve(read,system,controllerHandle);
        if(!controller || (controller->className!="CCSPlayerController" && controller->className!="C_CSPlayerController"))return {};
        PlayerIdentity result;result.controller=*controller;std::uint32_t handle=0;
        if(!Value(read,controller->instance+*fields.steamId,result.steamId) || !result.steamId || !Value(read,controller->instance+*fields.pawnHandle,handle))return {};
        const auto pawn=Resolve(read,system,handle);if(!pawn || pawn->className!="C_CSPlayerPawn")return {};
        result.pawn=*pawn;unsigned char team=0,controllerTeam=0;
        if(!Value(read,pawn->instance+fields.team,team) || !Value(read,controller->instance+fields.team,controllerTeam) ||
           (team!=2 && team!=3) || controllerTeam!=team)return {};
        result.team=team;result.nickname=Text(read,controller->instance+*fields.playerName,128);
        std::uint32_t currentHandle=0;std::uint64_t currentSteam=0;
        if(!Value(read,controller->instance+*fields.pawnHandle,currentHandle) || currentHandle!=handle ||
           !Value(read,controller->instance+*fields.steamId,currentSteam) || currentSteam!=result.steamId ||
           !StillCurrent(read,*controller) || !StillCurrent(read,*pawn))return {};
        return result;
    }
    static std::optional<Identity> Resolve(const Read& read,Address system,std::uint32_t handle) {
        const unsigned index=handle&0x7fff;if(index>MaxIndex || !system || !read)return {};
        Address page=0;
        if(!Value(read,system+0x10+sizeof(Address)*(index/PageEntries),page) || !page)return {};
        Identity result;result.system=system;result.address=page+IdentitySize*(index%PageEntries);
        std::array<unsigned char,0x34> bytes{};
        if(!read(result.address,bytes.data(),bytes.size()))return {};
        result.instance=At<Address>(bytes.data(),0);result.entityClass=At<Address>(bytes.data(),8);
        result.handle=At<std::uint32_t>(bytes.data(),0x10);result.flags=At<std::uint32_t>(bytes.data(),0x30);
        if(!result.instance || !result.entityClass || result.handle!=handle || result.flags&(0x1|0x2|0x10|0x100|0x200|0x400))return {};
        Address info=0,name=0;
        if(!Value(read,result.entityClass+0x58,info) || !info || !Value(read,info+8,name))return {};
        result.className=Text(read,name,128);
        Address currentPage=0;
        if(result.className.empty() || !Value(read,system+0x10+sizeof(Address)*(index/PageEntries),currentPage) || currentPage!=page || !StillCurrent(read,result))return {};
        return result;
    }
    static bool StillCurrent(const Read& read,const Identity& identity) {
        Address page=0;const unsigned index=identity.handle&0x7fff;
        if(!identity.system || !Value(read,identity.system+0x10+sizeof(Address)*(index/PageEntries),page) ||
           !page || identity.address!=page+IdentitySize*(index%PageEntries))return false;
        std::array<unsigned char,0x34> bytes{};
        return read && identity.address && read(identity.address,bytes.data(),bytes.size()) &&
            At<Address>(bytes.data(),0)==identity.instance && At<Address>(bytes.data(),8)==identity.entityClass &&
            At<std::uint32_t>(bytes.data(),0x10)==identity.handle && At<std::uint32_t>(bytes.data(),0x30)==identity.flags;
    }
    // Only a stable empty slot proves a missing child. A foreign live object,
    // unreadable page or page replacement never authorizes recovery calls.
    static bool ProvenAbsent(const Read& read,Address system,std::uint32_t handle) {
        const unsigned index=handle&0x7fff;
        if(!read || !system || system>~Address(0)-0x210 || index>MaxIndex)return false;
        Address page=0,afterPage=0;if(!Value(read,system+0x10+sizeof(Address)*(index/PageEntries),page) || !page)return false;
        if(page>~Address(0)-PageEntries*IdentitySize)return false;
        const auto address=page+IdentitySize*(index%PageEntries);std::array<unsigned char,0x34> first{},after{};
        if(!read(address,first.data(),first.size()) || At<Address>(first.data(),0)!=0 ||
           !read(address,after.data(),after.size()) || first!=after ||
           !Value(read,system+0x10+sizeof(Address)*(index/PageEntries),afterPage) || afterPage!=page)return false;
        return true;
    }
    static Json Inspect(const Read& read,Address system,const Fields& fields,unsigned limit=64) {
        if(!read || !system || !fields.team || !fields.renderColor || !limit || limit>128)
            throw std::invalid_argument("client entity probe is not initialized or limit is outside 1..128");
        Json result={{"read_only",true},{"palette_supported",false},{"render_verified",false},
            {"scope","current local demo client entities"},{"max_index",MaxIndex},
            {"team_offset",fields.team},{"render_color_offset",fields.renderColor},
            {"identity_count",0},{"pawn_count",0},{"controller_count",0},
            {"skipped_lifecycle",0},{"unreadable_pages",0},{"entries",Json::array()}};
        unsigned identities=0,pawns=0,controllers=0,skipped=0,unreadable=0;
        for(unsigned pageIndex=0;pageIndex<=MaxIndex/PageEntries;++pageIndex) {
            Address page=0;
            if(!Value(read,system+0x10+sizeof(Address)*pageIndex,page)) {++unreadable;continue;}
            if(!page)continue;
            std::array<unsigned char,PageEntries*IdentitySize> bytes{};
            if(!read(page,bytes.data(),bytes.size())) {++unreadable;continue;}
            for(unsigned slot=0;slot<PageEntries;++slot) {
                const unsigned index=pageIndex*PageEntries+slot;if(index>MaxIndex)break;
                const auto* identity=bytes.data()+slot*IdentitySize;
                const auto instance=At<Address>(identity,0),entityClass=At<Address>(identity,8);
                const auto handle=At<std::uint32_t>(identity,0x10),flags=At<std::uint32_t>(identity,0x30);
                if(!instance || !entityClass || (handle&0x7fff)!=index)continue;
                // Invalid handles, construction and deletion are not stable
                // targets. Dormant entities remain useful for actor selection.
                if(flags&(0x1|0x2|0x10|0x100|0x200|0x400)) {++skipped;continue;}
                ++identities;
                Address classInfo=0,className=0;
                if(!Value(read,entityClass+0x58,classInfo) || !classInfo ||
                   !Value(read,classInfo+8,className) || !className)continue;
                const auto cpp=Text(read,className,128);
                const bool pawn=cpp=="C_CSPlayerPawn",controller=cpp=="CCSPlayerController" || cpp=="C_CSPlayerController";
                if(!pawn && !controller)continue;
                if(pawn)++pawns;else++controllers;
                if(result["entries"].size()>=limit)continue;
                std::uint8_t team=0;std::array<unsigned char,4> color{};
                if(!Value(read,instance+fields.team,team))continue;
                Json entry={{"index",index},{"handle",handle},{"serial",handle>>15},
                    {"class",cpp},{"team",team},{"dormant",bool(flags&0x80)},
                    {"designer_name",Text(read,At<Address>(identity,0x20),128)}};
                if(pawn && read(instance+fields.renderColor,color.data(),color.size()))entry["render_color"]=color;
                if(controller) {
                    if(fields.playerName)entry["player_name"]=Text(read,instance+*fields.playerName,128);
                    if(fields.steamId) {std::uint64_t steam=0;if(Value(read,instance+*fields.steamId,steam))entry["steam_id"]=std::to_string(steam);}
                    if(fields.pawnHandle) {std::uint32_t value=0;if(Value(read,instance+*fields.pawnHandle,value))entry["pawn_handle"]=value;}
                }
                // An identity can be recycled during loading. Reject the
                // sample unless pointer, full handle and flags still match.
                std::array<unsigned char,0x34> after{};
                if(!read(page+slot*IdentitySize,after.data(),after.size()) ||
                   At<Address>(after.data(),0)!=instance || At<Address>(after.data(),8)!=entityClass ||
                   At<std::uint32_t>(after.data(),0x10)!=handle ||
                   At<std::uint32_t>(after.data(),0x30)!=flags) {++skipped;continue;}
                Address currentPage=0;
                if(!Value(read,system+0x10+sizeof(Address)*pageIndex,currentPage) || currentPage!=page) {++skipped;continue;}
                result["entries"].push_back(std::move(entry));
            }
        }
        result["identity_count"]=identities;result["pawn_count"]=pawns;result["controller_count"]=controllers;
        result["skipped_lifecycle"]=skipped;result["unreadable_pages"]=unreadable;
        result["truncated"]=pawns+controllers>result["entries"].size();return result;
    }
    template<class T> static bool Value(const Read& read,Address address,T& value) {
        return address && address+sizeof(T)>=address && read(address,&value,sizeof(T));
    }
    static std::string Text(const Read& read,Address address,std::size_t maximum) {
        if(!address)return {};
        std::string result;result.reserve(maximum);
        // Read only as far as the current page so a short C string at its end
        // is not rejected just because the following page is inaccessible.
        while(result.size()<maximum) {
            std::array<char,64> bytes{};
            const auto count=std::min({bytes.size(),maximum-result.size(),std::size_t(4096-(address&4095))});
            if(!read(address,bytes.data(),count))return {};
            for(std::size_t i=0;i<count;++i) {
                if(!bytes[i])return Json::parse(Json(result).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
                result.push_back(bytes[i]);
            }
            address+=count;
        }
        return {}; // Unterminated strings are not accepted as names.
    }
private:
    template<class T> static T At(const unsigned char* bytes,std::size_t offset) {
        T value{};std::memcpy(&value,bytes+offset,sizeof(value));return value;
    }
};
} // namespace cs2_live

#ifdef CS2_CAMERA_SOURCE2_CVARS
#if __has_include("linux_view_patch.h")
#include "linux_view_patch.h"
#else
#include "../linux_view_patch.h"
#endif
// The pinned SDK hides this declaration with NO_MALLOC_OVERRIDE although its
// schema hash-container declarations still reference it. We instantiate no
// SDK pools and keep the game allocator override disabled.
#ifdef NO_MALLOC_OVERRIDE
enum MemAllocAttribute_t {MemAllocAttribute_Unk0=0,MemAllocAttribute_Unk1=1,MemAllocAttribute_Unk2=2};
#endif
#include "schemasystem/schemasystem.h"
#include <sys/uio.h>
#include <unistd.h>

namespace cs2_live {
class Source2ClientEntityProbe {
public:
    using Json=nlohmann::json;
    using Address=ClientEntityProbe::Address;
    struct Profile {
        Address base=0,system=0;
        ClientEntityProbe::Read read;
        ClientEntityProbe::Fields fields;
        CSchemaSystemTypeScope* scope=nullptr;
        std::string buildId;
    };
    static Profile ResolveProfile(CreateInterfaceFn factory,void* client) {
        Module module;dl_iterate_phdr(&FindClient,&module);
        if(module.matches!=1 || !module.build || !module.base)
            throw std::runtime_error("client entity inspection requires the validated Linux client build-ID");
        const auto read=[](Address address,void* output,std::size_t size) {
            if(!address || !size || address+size<address)return false;
            iovec local{output,size},remote{reinterpret_cast<void*>(address),size};
            return process_vm_readv(getpid(),&local,1,&remote,1,0)==ssize_t(size);
        };
        Address vtable=0,first=0,second=0;
        if(!client || !ClientEntityProbe::Value(read,Address(client),vtable) || vtable!=module.base+0x44f83b8 ||
           !ClientEntityProbe::Value(read,vtable+174*8,first) || first!=module.base+0x18eee90 ||
           !ClientEntityProbe::Value(read,vtable+81*8,second) || second!=module.base+0x18ee880)
            throw std::runtime_error("Source2Client002 entity lookup vtable does not match the validated client");
        constexpr unsigned char firstBytes[]={0x48,0x8d,0x15,0x6a,0x67,0x2a,0x03,0x48,0x8b,0x0a,0x89,0xf2,0xc1,0xfa,0x09,0x48,0x63,0xd2,0x48,0x8b,0x54,0xd1,0x10,0x48,0x85,0xd2,0x74,0x1d,0x89,0xf1,0x81,0xe1,0xff,0x01,0x00,0x00,0x48,0x6b,0xc9,0x70,0x48,0x01,0xca,0x0f,0xb7,0x4a,0x10,0x81,0xe1,0xff,0x7f,0x00,0x00,0x39,0xf1,0x74,0x08};
        constexpr unsigned char secondBytes[]={0xb8,0x01,0x00,0x00,0x00,0x81,0xfe,0xfe,0x7f,0x00,0x00,0x77,0x39,0x48,0x8d,0x15,0x7c,0x6d,0x2a,0x03,0x48,0x8b,0x0a,0x89,0xf2,0xc1,0xfa,0x09,0x48,0x63,0xd2,0x48,0x8b,0x54,0xd1,0x10};
        CheckBytes(read,module.base+0x18eee9f,firstBytes,sizeof(firstBytes));
        CheckBytes(read,module.base+0x18ee880,secondBytes,sizeof(secondBytes));
        auto* schema=factory?static_cast<ISchemaSystem*>(factory(SCHEMASYSTEM_INTERFACE_VERSION,nullptr)):nullptr;
        if(!schema) {
            if(void* loaded=dlopen("libschemasystem.so",RTLD_NOW|RTLD_NOLOAD)) {
                auto query=reinterpret_cast<CreateInterfaceFn>(dlsym(loaded,"CreateInterface"));
                if(query)schema=static_cast<ISchemaSystem*>(query(SCHEMASYSTEM_INTERFACE_VERSION,nullptr));
                dlclose(loaded);
            }
        }
        if(!schema)throw std::runtime_error("SchemaSystem_001 is unavailable for runtime entity field validation");
        auto* scope=schema->FindTypeScopeForModule("libclient.so");
        if(!scope)throw std::runtime_error("client runtime schema scope is unavailable");
        ClientEntityProbe::Fields fields;
        fields.team=Field(scope,"C_BaseEntity","m_iTeamNum",1).value_or(0);
        fields.renderColor=Field(scope,"C_BaseModelEntity","m_clrRender",4).value_or(0);
        if(fields.team!=0x557 || fields.renderColor!=0xc20)
            throw std::runtime_error("runtime team/color schema fields do not match the validated client profile");
        fields.playerName=Field(scope,"CBasePlayerController","m_iszPlayerName",128);
        fields.steamId=Field(scope,"CBasePlayerController","m_steamID",8);
        fields.pawnHandle=Field(scope,"CCSPlayerController","m_hPlayerPawn",4);
        if(!fields.pawnHandle)fields.pawnHandle=Field(scope,"C_CSPlayerController","m_hPlayerPawn",4);
        Address system=0;
        if(!ClientEntityProbe::Value(read,module.base+0x4b95610,system) || !system)
            throw std::runtime_error("validated client entity system is not initialized");
        return {module.base,system,read,fields,scope,module.buildId};
    }
    static Json Inspect(CreateInterfaceFn factory,void* client,unsigned limit=64) {
        const auto profile=ResolveProfile(factory,client);
        const auto& fields=profile.fields;
        auto result=ClientEntityProbe::Inspect(profile.read,profile.system,fields,limit);
        result["build_id"]=profile.buildId;
        result["profile_validated"]=true;
        result["player_name_available"]=bool(fields.playerName);
        result["steam_id_available"]=bool(fields.steamId);
        result["pawn_handle_available"]=bool(fields.pawnHandle);
        return result;
    }
    static std::optional<unsigned> SchemaField(CSchemaSystemTypeScope* scope,const char* className,const char* name,int expectedSize) {
        return Field(scope,className,name,expectedSize);
    }
private:
    struct Module {Address base=0;unsigned matches=0;bool build=false;std::string buildId;};
    static int FindClient(dl_phdr_info* info,std::size_t,void* data) {
        auto& result=*static_cast<Module*>(data);
        if(!info->dlpi_name || !std::strstr(info->dlpi_name,"/csgo/bin/linuxsteamrt64/libclient.so"))return 0;
        ++result.matches;result.base=info->dlpi_addr;
        for(unsigned i=0;i<info->dlpi_phnum;++i) {
            const auto& segment=info->dlpi_phdr[i];if(segment.p_type!=PT_NOTE)continue;
            const auto* bytes=reinterpret_cast<const unsigned char*>(info->dlpi_addr+segment.p_vaddr);
            for(std::size_t offset=0;offset+sizeof(ElfW(Nhdr))<=segment.p_memsz;) {
                const auto* note=reinterpret_cast<const ElfW(Nhdr)*>(bytes+offset);
                const auto aligned=[](std::size_t n){return (n+3)&~std::size_t(3);};
                const auto size=sizeof(*note)+aligned(note->n_namesz)+aligned(note->n_descsz);
                if(size>segment.p_memsz-offset)break;
                const auto* name=bytes+offset+sizeof(*note);const auto* value=name+aligned(note->n_namesz);
                if(note->n_type==NT_GNU_BUILD_ID && note->n_namesz==4 && !std::memcmp(name,"GNU",4) &&
                   cs2_mvp::LinuxViewPatch::IsValidatedBuildId(value,note->n_descsz)) {
                    result.build=true;static constexpr char hex[]="0123456789abcdef";
                    for(unsigned j=0;j<note->n_descsz;++j){result.buildId.push_back(hex[value[j]>>4]);result.buildId.push_back(hex[value[j]&15]);}
                }
                offset+=size;
            }
        }
        return 0;
    }
    static void CheckBytes(const ClientEntityProbe::Read& read,Address address,const unsigned char* expected,std::size_t size) {
        std::vector<unsigned char> actual(size);
        if(!read(address,actual.data(),actual.size()) || std::memcmp(actual.data(),expected,size))
            throw std::runtime_error("client entity lookup instructions do not match the validated build");
    }
    static std::optional<unsigned> Field(CSchemaSystemTypeScope* scope,const char* className,const char* name,int expectedSize) {
        const auto* info=scope->FindDeclaredClass(className).Get();
        if(!info || info->m_nFieldCount>4096 || !info->m_pFields || info->m_nSize<=0 || info->m_nSize>1024*1024)return {};
        for(unsigned i=0;i<info->m_nFieldCount;++i) {
            const auto& field=info->m_pFields[i];
            if(!field.m_pszName || std::strcmp(field.m_pszName,name) || !field.m_pType)continue;
            int size=0;uint8 alignment=0;
            if(!field.m_pType->GetSizeAndAlignment(size,alignment) || (expectedSize>=0 && size!=expectedSize) ||
               field.m_nSingleInheritanceOffset<0 || field.m_nSingleInheritanceOffset+size>info->m_nSize)return {};
            return unsigned(field.m_nSingleInheritanceOffset);
        }
        return {};
    }
};
} // namespace cs2_live
#endif
