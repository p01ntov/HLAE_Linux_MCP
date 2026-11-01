#include "../glove_pose.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <map>

using Probe=cs2_live::GlovePoseProbe;using Address=Probe::Address;
struct Memory {
    std::array<unsigned char,4096> bytes{};bool unreadable=false,replaceAfterRead=false;unsigned poseReads=0;
    static constexpr Address Node=0x1000,Poses=0x1400,Bits=0x1800;
    template<class T>void Put(Address a,const T& value){assert(a>=Node && a-Node+sizeof(T)<=bytes.size());std::memcpy(bytes.data()+a-Node,&value,sizeof(value));}
    void Pose(unsigned index,const std::array<float,8>& value){Put(Poses+index*32,value);}
    Memory(){Put(Node+0x1c0,Poses);Put(Node+0x1c8,Bits);Put(Node+0x1d0,int(4));Put(Node+0x1d4,std::uint16_t(3));Put(Bits,std::uint32_t(7));for(unsigned i=0;i<4;++i)Pose(i,{0,0,0,1,0,0,0,1});}
    Probe::Read Reader(){return [&](Address a,void* out,std::size_t n){if(unreadable || a<Node || n>bytes.size() || a-Node>bytes.size()-n)return false;std::memcpy(out,bytes.data()+a-Node,n);if(a==Poses){++poseReads;if(replaceAfterRead)Put(Node+0x1c0,Poses+32);}return true;};}
    void Populate(){Pose(0,{635,-1474,-360,1,0,0,0,1});Pose(1,{640,-1490,-368,1,0,0,0,1});Pose(2,{638,-1490,-370,1,0,0,0,1});}
};
struct RenderMemory {
    using Render=cs2_live::GloveRenderProbe;std::map<Address,unsigned char> bytes;bool replaceAfterEntry=false;
    static constexpr Address Base=0x10000000,Entity=0x1000,Node=0x2000,Component=0x3000,Data=0x4000,Entry=0x5000,Scene=0x6000;
    template<class T>void Put(Address a,const T& v){const auto* p=reinterpret_cast<const unsigned char*>(&v);for(unsigned i=0;i<sizeof(T);++i)bytes[a+i]=p[i];}
    RenderMemory(){Put(Entity+0x4a8,Component);Put(Component,Base+0x43ebf10);Put(Base+0x43ebf10-8,Base+0x43ea320);Put(Component+8,Entity);Put(Component+0x10,Entity);Put(Component+0x38,int(1));Put(Component+0x40,Data);Put(Component+0x58,std::uint8_t(1));Put(Component+0xb8,std::uint8_t(0));Put(Data,Entry);Put(Entry,Node);Put(Entry+0x28,Entity);Put(Entry+0x20,Scene);Put(Entry+0x48,std::uint32_t(0x3fc1));Put(Scene+0x70,std::uint64_t(1)<<60);}
    Probe::Read Reader(){return [&](Address a,void* out,std::size_t n){auto* p=static_cast<unsigned char*>(out);for(std::size_t i=0;i<n;++i){const auto it=bytes.find(a+i);if(it==bytes.end())return false;p[i]=it->second;}if(replaceAfterEntry && a==Entry)Put(Entity+0x4a8,Component+0x100);return true;};}
    Render::Result Inspect(){return Render::Inspect(Reader(),Base,Entity,Node);}
    void Published(){Put(Component+0xb8,std::uint8_t(1));Put(Entry+0x48,std::uint32_t(0x3fc3));Put(Scene+0x70,std::uint64_t(0));}
};
static void RunningPublicationPromotesOnlyCurrentPausedChild(){
    using Scope=cs2_live::GlovePublicationScope;using namespace std::chrono;
    const auto now=Scope::Clock::time_point{}+seconds(10);
    Scope original;original.tick=11780;original.deadline=now+seconds(5);original.worldHandle=0x12340007;original.worldInstance=0x5000;
    auto current=original;
    assert(current.PromotePaused(true,true,11779,now+seconds(1),true,true,original.worldHandle,original.worldInstance));
    assert(current.pausedScope && current.tick==11779 && current.deadline==original.deadline);
    // The paused lifetime stays pinned, including after its preparation budget.
    assert(!current.PromotePaused(true,true,11778,now+seconds(9),true,true,original.worldHandle,original.worldInstance));
    assert(current.tick==11779 && current.deadline==original.deadline);
    for(unsigned failure=0;failure<8;++failure){
        current=original;
        const bool promoted=current.PromotePaused(failure!=0,failure!=1,11779,
            failure==2?original.deadline+milliseconds(1):now+seconds(1),failure!=3,failure!=4,
            failure==5?original.worldHandle+0x8000:original.worldHandle,
            failure==6?original.worldInstance+0x1000:failure==7?0:original.worldInstance);
        assert(!promoted && !current.pausedScope && current.tick==11780 && current.deadline==original.deadline);
    }
    current=original;current.worldHandle=0xffffffff;current.worldInstance=0;
    assert(!current.PromotePaused(true,true,11779,now,true,true,0xffffffff,0));
}
int main(){
    RunningPublicationPromotesOnlyCurrentPausedChild();
    Memory m;auto r=Probe::Inspect(m.Reader(),Memory::Node);assert(r.layoutValid && !r.ready && r.bones==3);
    m.Populate();r=Probe::Inspect(m.Reader(),Memory::Node);assert(r.ready && r.bones==3 && m.poseReads==2); // spare capacity pose is intentionally identity
    for(auto capacity:{-1,0,1,257}){Memory bad;bad.Put(Memory::Node+0x1d0,capacity);assert(!Probe::Inspect(bad.Reader(),Memory::Node).layoutValid);}
    for(std::uint16_t count:{0,1,5,65535}){Memory bad;bad.Put(Memory::Node+0x1d4,count);assert(!Probe::Inspect(bad.Reader(),Memory::Node).layoutValid);}
    {Memory bad;bad.Populate();bad.Put(Memory::Bits,std::uint32_t(0));const auto cached=Probe::Inspect(bad.Reader(),Memory::Node);assert(cached.ready && cached.requestedBones==0);}
    {Memory bad;bad.Populate();bad.unreadable=true;assert(!Probe::Inspect(bad.Reader(),Memory::Node).layoutValid);}
    {Memory bad;bad.Populate();bad.replaceAfterRead=true;assert(!Probe::Inspect(bad.Reader(),Memory::Node).layoutValid);}
    {Memory bad;bad.Populate();bad.Put(Memory::Node+0x118,int(3));assert(!Probe::Inspect(bad.Reader(),Memory::Node).layoutValid);}
    {Memory bad;bad.Put(Memory::Node+0x1c0,std::numeric_limits<Address>::max()-16);assert(!Probe::Inspect(bad.Reader(),Memory::Node).layoutValid);}
    for(const auto& pose:std::vector<std::array<float,8>>{{std::numeric_limits<float>::quiet_NaN(),0,0,1,0,0,0,1},{10,0,0,0,0,0,0,1},{10,0,0,1,0,0,0,0},{10,0,0,1,0,0,0,2},{10000001,0,0,1,0,0,0,1}}){Memory bad;bad.Populate();bad.Pose(1,pose);assert(!Probe::Inspect(bad.Reader(),Memory::Node).ready);}
    {Memory bad;for(unsigned i=0;i<3;++i)bad.Pose(i,{10,20,30,1,0,0,0,1});assert(!Probe::Inspect(bad.Reader(),Memory::Node).ready);}
    assert(!Probe::Inspect(m.Reader(),std::numeric_limits<Address>::max()-16).layoutValid);
    {RenderMemory r;auto value=r.Inspect();assert(value.layoutValid && !value.ready && value.hiddenObjects==1);r.Put(RenderMemory::Component+0xb8,std::uint8_t(1));r.Put(RenderMemory::Entry+0x48,std::uint32_t(0x3fc3));assert(!r.Inspect().ready);r.Published();assert(r.Inspect().ready);}
    {RenderMemory r;r.Published();r.Put(RenderMemory::Component+0x58,std::uint8_t(0));assert(r.Inspect().layoutValid && !r.Inspect().ready);}
    {RenderMemory r;r.Put(RenderMemory::Component+0x38,int(0));r.Put(RenderMemory::Component+0x40,Address(0));assert(r.Inspect().layoutValid && !r.Inspect().ready);}
    {RenderMemory r;r.Put(RenderMemory::Entry+0x20,Address(0));assert(r.Inspect().layoutValid && !r.Inspect().ready);}
    {RenderMemory r;r.Published();r.Put(RenderMemory::Entry+0x28,RenderMemory::Entity+1);assert(!r.Inspect().layoutValid);}
    {RenderMemory r;r.Put(RenderMemory::Entry,RenderMemory::Entity);r.Put(RenderMemory::Entry+0x20,Address(0));assert(!r.Inspect().layoutValid);const auto parent=RenderMemory::Render::InspectParent(r.Reader(),RenderMemory::Base,RenderMemory::Entity,RenderMemory::Node);assert(parent.layoutValid && parent.enabled && !parent.interpolationReady && !parent.ready);r.Put(RenderMemory::Component+8,RenderMemory::Entity+1);assert(!RenderMemory::Render::InspectParent(r.Reader(),RenderMemory::Base,RenderMemory::Entity,RenderMemory::Node).layoutValid);}
    {RenderMemory r;r.Put(RenderMemory::Entry,RenderMemory::Node+1);assert(!r.Inspect().layoutValid);}
    {RenderMemory r;r.Put(RenderMemory::Component+8,RenderMemory::Entity+1);assert(!r.Inspect().layoutValid);}
    {RenderMemory r;r.Put(RenderMemory::Component+0x38,int(129));assert(!r.Inspect().layoutValid);}
    {RenderMemory r;r.Put(RenderMemory::Component+0xb8,std::uint8_t(2));assert(!r.Inspect().layoutValid);}
    {RenderMemory r;r.Published();r.replaceAfterEntry=true;assert(!r.Inspect().layoutValid);}
    std::cout<<"Glove pose: identity/unpopulated, active count, finite transforms, cleared request bits, busy worker and changing metadata guards passed\n";
    std::cout<<"Glove render: interpolation publication, scene hidden bit, exact owner/node, bounds and replacement guards passed\n";
}
