#include "../observer_player.h"
#include <cassert>
#include <iostream>

using Entity=cs2_live::ClientEntityProbe;
using Probe=cs2_live::ObserverPlayerProbe;
using Address=Entity::Address;
struct ObserverMemory {
    struct Region{Address address;std::vector<unsigned char> bytes;};
    struct Actor{Address identity,instance;std::uint32_t handle;};
    static constexpr Address Base=0x10000000;
    static constexpr std::uint64_t Steam=76561198012345678ull;
    std::vector<Region> regions;std::array<Address,64> pages{};Address next=0x100000,system=0,services=0;
    Entity::Fields actors{0x557,0xc20,0x1000,0x1080,0x1088};
    Probe::Fields fields{0x80,0x90,0x48,0xa0};
    Actor local{},observer{},controller{},pawn{};
    std::function<void(Address,std::size_t)> beforeRead;
    Address Allocate(std::size_t size){const auto result=next;next+=((size+4095)&~Address(4095))+4096;regions.push_back({result,std::vector<unsigned char>(size)});return result;}
    void Bytes(Address address,const void* value,std::size_t size){
        for(auto& region:regions)if(address>=region.address && address-region.address<=region.bytes.size() && size<=region.bytes.size()-(address-region.address)){
            std::memcpy(region.bytes.data()+address-region.address,value,size);return;
        }
        assert(false && "fixture write outside allocated region");
    }
    template<class T>void Put(Address address,const T& value){Bytes(address,&value,sizeof(value));}
    Address Text(const std::string& text){const auto result=Allocate(128);assert(text.size()<128);Bytes(result,text.data(),text.size());return result;}
    Actor Add(unsigned index,const std::string& name,unsigned team=2,std::uint32_t serial=7){
        assert(index<Entity::MaxIndex);
        const auto page=index/Entity::PageEntries;
        if(!pages[page]){pages[page]=Allocate(Entity::PageEntries*Entity::IdentitySize);Put(system+0x10+page*sizeof(Address),pages[page]);}
        const auto identity=pages[page]+index%Entity::PageEntries*Entity::IdentitySize;
        const auto instance=Allocate(0x2000),entityClass=Allocate(0x100),info=Allocate(0x80),text=Text(name);
        const auto handle=(serial<<15)|index;
        Put(identity,instance);Put(identity+8,entityClass);Put(identity+0x10,handle);
        Put(identity+0x20,Text("player"));Put(entityClass+0x58,info);Put(info+8,text);
        Put(instance+0x10,identity);Put(instance+actors.team,std::uint8_t(team));
        const std::string nickname="Same name";Bytes(instance+*actors.playerName,nickname.data(),nickname.size());
        return {identity,instance,handle};
    }
    explicit ObserverMemory(unsigned index=2,const cs2_live::ClientBuildProfile* layout=nullptr){
        const auto at=[&](Address previous){return Base+(layout?layout->Offset(previous):previous);};
        system=Allocate(0x210);local=Add(1,"CCSPlayerController");observer=Add(70,"C_CSObserverPawn");
        controller=Add(index,"C_CSPlayerController",2,0x12345);pawn=Add(71,"C_CSPlayerPawn",2,9);
        services=Allocate(0x80);
        regions.push_back({at(0x49083b8),std::vector<unsigned char>(8)});
        regions.push_back({at(0x44c6f58)-8,std::vector<unsigned char>(8)});
        Put(at(0x49083b8),local.instance);Put(at(0x44c6f58)-8,at(0x44c6368));
        Put(local.instance+fields.controllerPawn,observer.handle);
        Put(observer.instance+fields.observerServices,services);
        Put(services,at(0x44c6f58));Put(services+0x38,observer.instance);Put(services+fields.observerTarget,pawn.handle);
        Put(pawn.instance+fields.pawnController,controller.handle);
        Put(controller.instance+*actors.steamId,Steam);Put(controller.instance+*actors.pawnHandle,pawn.handle);
        Put(controller.instance+fields.controllerPawn,pawn.handle);
    }
    Entity::Read Reader(){return [&](Address address,void* output,std::size_t size){
        if(beforeRead)beforeRead(address,size);
        for(const auto& region:regions)if(address>=region.address && address-region.address<=region.bytes.size() && size<=region.bytes.size()-(address-region.address)){
            std::memcpy(output,region.bytes.data()+address-region.address,size);return true;
        }
        return false;
    };}
    std::optional<Entity::PlayerIdentity> Target(){return Probe::Target(Reader(),Base,system,actors,fields);}
    Entity::PlayerIdentity Find(const std::string& id=std::to_string(Steam)){return Probe::Find(Reader(),system,actors,fields,id);}
};
template<class F>void Reject(F operation){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}assert(rejected);}

static void ExactTargetAndCommandIndex(){
    ObserverMemory memory;const auto target=memory.Target();assert(target);
    assert(target->controller.handle==memory.controller.handle && target->pawn.handle==memory.pawn.handle && target->steamId==ObserverMemory::Steam);
    const auto selected=memory.Find();assert(selected.controller.handle==memory.controller.handle && (selected.controller.handle&0x7fff)==2);
    const auto json=Probe::Json(selected);assert(json["steam_id"]==std::to_string(ObserverMemory::Steam) && json["nickname"]=="Same name");
    assert(json["controller_handle"]==memory.controller.handle && json["pawn_handle"]==memory.pawn.handle && !json.contains("instance"));
    const auto from=Probe::FromInstance(memory.Reader(),memory.system,memory.local.instance);assert(from && from->handle==memory.local.handle);
    memory.Put(memory.local.identity,memory.local.instance+0x100);assert(!Probe::FromInstance(memory.Reader(),memory.system,memory.local.instance));
    ObserverMemory last(64);assert((last.Find().controller.handle&0x7fff)==64);
}
static void RejectWrongServicesAndSerials(){
    for(unsigned failure=0;failure<7;++failure){
        ObserverMemory memory;
        if(failure==0)memory.Put(memory.services,ObserverMemory::Base+0x44c6f58+8);
        if(failure==1)memory.Put(ObserverMemory::Base+0x44c6f58-8,ObserverMemory::Base+0x44c6368+8);
        if(failure==2)memory.Put(memory.services+0x38,memory.pawn.instance);
        if(failure==3)memory.Put(memory.services+memory.fields.observerTarget,memory.pawn.handle+(1u<<15));
        if(failure==4)memory.Put(memory.local.instance+memory.fields.controllerPawn,memory.observer.handle+(1u<<15));
        if(failure==5)memory.Put(memory.pawn.instance+memory.fields.pawnController,memory.controller.handle+(1u<<15));
        if(failure==6)memory.Put(memory.controller.instance+*memory.actors.pawnHandle,memory.pawn.handle+(1u<<15));
        assert(!memory.Target());
    }
    ObserverMemory changed;unsigned reads=0;
    changed.beforeRead=[&](Address address,std::size_t size){if(address==changed.services+changed.fields.observerTarget && size==4 && ++reads==2)changed.Put(address,changed.pawn.handle+(1u<<15));};
    assert(!changed.Target());
}
static void UniqueSteamAndGenericPawnMapping(){
    {ObserverMemory memory;Reject([&]{memory.Find("76561198012345679");});}
    {ObserverMemory memory;memory.Put(memory.controller.instance+memory.fields.controllerPawn,memory.pawn.handle+(1u<<15));Reject([&]{memory.Find();});}
    {ObserverMemory memory(65);Reject([&]{memory.Find();});}
    {ObserverMemory memory(0);Reject([&]{memory.Find();});}
    {ObserverMemory memory;memory.Put(memory.controller.instance+memory.actors.team,std::uint8_t(3));Reject([&]{memory.Find();});}
    {ObserverMemory memory;const auto duplicate=memory.Add(3,"CCSPlayerController");
     memory.Put(duplicate.instance+*memory.actors.steamId,ObserverMemory::Steam);
     memory.Put(duplicate.instance+*memory.actors.pawnHandle,memory.pawn.handle);memory.Put(duplicate.instance+memory.fields.controllerPawn,memory.pawn.handle);
     Reject([&]{memory.Find();});}
    // Inspect is merely a candidate snapshot. A SteamID changing before the
    // fresh resolve must never redirect a request to a different person.
    {ObserverMemory memory;unsigned reads=0;
     memory.beforeRead=[&](Address address,std::size_t size){if(address==memory.controller.instance+*memory.actors.steamId && size==8 && ++reads==2)memory.Put(address,ObserverMemory::Steam+1);};
     Reject([&]{memory.Find();});}
}
static void CurrentBuildObserverReadback(){
    constexpr std::array<unsigned char,20> id={0xf0,0x09,0x42,0xca,0x6d,0x0c,0x48,0x6d,0x11,0xf9,0x88,0xc5,0x33,0x81,0xc4,0x7a,0x5b,0xe2,0x71,0x43};
    const auto* profile=cs2_live::ClientBuildProfile::Find(id.data(),id.size());assert(profile);
    ObserverMemory memory(2,profile);
    assert(!memory.Target()); // The old global must not accidentally resolve a new build.
    const auto target=Probe::Target(memory.Reader(),ObserverMemory::Base,memory.system,memory.actors,memory.fields,profile);
    assert(target && target->steamId==ObserverMemory::Steam && target->pawn.handle==memory.pawn.handle);
    memory.Put(memory.services,ObserverMemory::Base+0x44c6f58); // Reject a stale legacy vtable even with otherwise valid ownership.
    assert(!Probe::Target(memory.Reader(),ObserverMemory::Base,memory.system,memory.actors,memory.fields,profile));
}
int main(){ExactTargetAndCommandIndex();RejectWrongServicesAndSerials();UniqueSteamAndGenericPawnMapping();CurrentBuildObserverReadback();std::cout<<"Observer player: exact service/owner/target, full serials, unique current SteamID and controller-index mapping passed\n";}
