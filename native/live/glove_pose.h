#pragma once
#include "team_palette.h"
#include <chrono>
#include <cmath>
#include <limits>

namespace cs2_live {
// Preparation begun during an asynchronous seek may observe running playback
// before the requested pause settles. Promote only the same validated child;
// the original preparation deadline is never extended by this transition.
struct GlovePublicationScope {
    using Clock=std::chrono::steady_clock;using Address=ClientEntityProbe::Address;
    Clock::time_point deadline;int tick=0;bool pausedScope=false;
    std::uint32_t worldHandle=0xffffffff;Address worldInstance=0;
    bool PromotePaused(bool demo,bool paused,int observedTick,Clock::time_point now,
                       bool actorCurrent,bool itemCurrent,std::uint32_t observedWorld,Address observedInstance) {
        if(pausedScope || !demo || !paused || now>deadline || !actorCurrent || !itemCurrent ||
           !worldInstance || worldHandle==0xffffffff || worldHandle!=observedWorld || worldInstance!=observedInstance)return false;
        pausedScope=true;tick=observedTick;return true;
    }
};

// Read the same bounded world-pose vector consumed immediately after the
// current client's native skeleton evaluation. This does not evaluate or write
// bones. A loaded model and finished material jobs alone do not prove a pose.
class GlovePoseProbe {
public:
    using Address=ClientEntityProbe::Address;using Read=ClientEntityProbe::Read;
    struct Result {bool layoutValid=false,ready=false;unsigned bones=0,requestedBones=0;};
    static Result Inspect(const Read& read,Address node) {
        Result result;if(!node || node>std::numeric_limits<Address>::max()-0x1d6)return result;
        Address poses=0,bits=0;int capacity=0,locked=0;std::uint16_t count=0;
        if(!ClientEntityProbe::Value(read,node+0x118,locked) || locked ||
           !ClientEntityProbe::Value(read,node+0x1c0,poses) || !poses || poses%16 ||
           !ClientEntityProbe::Value(read,node+0x1c8,bits) || !bits ||
           !ClientEntityProbe::Value(read,node+0x1d0,capacity) || capacity<2 || capacity>256 ||
           !ClientEntityProbe::Value(read,node+0x1d4,count) || count<2 || count>capacity ||
           poses>std::numeric_limits<Address>::max()-std::size_t(count)*32 ||
           bits>std::numeric_limits<Address>::max()-((count+31)/32)*4)return result;
        std::vector<std::array<float,8>> values(count);std::array<std::uint32_t,8> valid{};
        if(!read(poses,values.data(),values.size()*sizeof(values[0])) || !read(bits,valid.data(),((count+31)/32)*4))return result;
        Address afterPoses=0,afterBits=0;int afterCapacity=0;std::uint16_t afterCount=0;
        if(!ClientEntityProbe::Value(read,node+0x118,locked) || locked ||
           !ClientEntityProbe::Value(read,node+0x1c0,afterPoses) || afterPoses!=poses ||
           !ClientEntityProbe::Value(read,node+0x1c8,afterBits) || afterBits!=bits ||
           !ClientEntityProbe::Value(read,node+0x1d0,afterCapacity) || afterCapacity!=capacity ||
           !ClientEntityProbe::Value(read,node+0x1d4,afterCount) || afterCount!=count)return result;
        result.layoutValid=true;result.bones=count;bool separated=false;
        for(unsigned i=0;i<count;++i){
            const auto& p=values[i];if(valid[i/32]&(1u<<(i%32)))++result.requestedBones;
            for(float f:p)if(!std::isfinite(f) || std::fabs(f)>10000000.f)return result;
            const float norm=p[4]*p[4]+p[5]*p[5]+p[6]*p[6]+p[7]*p[7];
            if(p[3]<=0.f || p[3]>1000.f || norm<0.5f || norm>1.5f)return result;
            for(unsigned axis=0;axis<3;++axis)if(std::fabs(p[axis]-values[0][axis])>0.001f)separated=true;
        }
        // Native invalidation clears the per-request bits while retaining a
        // populated cached pose. They are diagnostics, not pose ownership or
        // readiness; newly allocated identity poses still fail this check.
        result.ready=separated;return result;
    }
};

// Exact F1 CRenderComponent ownership and native interpolation publication.
// This is a readback only; the runtime invokes the client's typed setter.
class GloveRenderProbe {
public:
    using Address=ClientEntityProbe::Address;using Read=ClientEntityProbe::Read;
    struct Result {bool layoutValid=false,ready=false,enabled=false,interpolationReady=false;Address component=0;unsigned objects=0,hiddenObjects=0;};
    static Result Inspect(const Read& read,Address base,Address entity,Address node) {return InspectComponent(read,base,entity,node,true);}
    // The parent is never mutated. Pawn components also contain entity-backed
    // auxiliary entries, so only their exact typed header/ownership is needed.
    static Result InspectParent(const Read& read,Address base,Address entity,Address node) {return InspectComponent(read,base,entity,node,false);}
private:
    static Result InspectComponent(const Read& read,Address base,Address entity,Address node,bool inspectEntries) {
        Result result;Address component=0,table=0,type=0,owner=0,chain=0,data=0;int count=0;unsigned char enabled=0,interpolation=0;
        if(!entity || !node || base>std::numeric_limits<Address>::max()-0x43ebf18 || entity>std::numeric_limits<Address>::max()-0x4b0 ||
           !ClientEntityProbe::Value(read,entity+0x4a8,component) || !component || component>std::numeric_limits<Address>::max()-0xb9 ||
           !ClientEntityProbe::Value(read,component,table) || table!=base+0x43ebf10 ||
           !ClientEntityProbe::Value(read,table-8,type) || type!=base+0x43ea320 ||
           !ClientEntityProbe::Value(read,component+8,owner) || owner!=entity ||
           !ClientEntityProbe::Value(read,component+0x10,chain) || chain!=entity ||
           !ClientEntityProbe::Value(read,component+0x38,count) || count<0 || count>128 ||
           !ClientEntityProbe::Value(read,component+0x40,data) || (count && !data) || data>std::numeric_limits<Address>::max()-std::size_t(count)*8 ||
           !ClientEntityProbe::Value(read,component+0x58,enabled) || enabled>1 ||
           !ClientEntityProbe::Value(read,component+0xb8,interpolation) || interpolation>1)return result;
        bool visible=true;unsigned hidden=0;
        for(int i=0;inspectEntries && i<count;++i){Address entry=0,entryNode=0,entryOwner=0,scene=0;std::uint32_t flags=0;std::uint64_t sceneFlags=0;
            if(!ClientEntityProbe::Value(read,data+std::size_t(i)*8,entry) || !entry || entry>std::numeric_limits<Address>::max()-0x50 ||
               !ClientEntityProbe::Value(read,entry,entryNode) || entryNode!=node ||
               !ClientEntityProbe::Value(read,entry+0x28,entryOwner) || entryOwner!=entity ||
               !ClientEntityProbe::Value(read,entry+0x20,scene) || scene>std::numeric_limits<Address>::max()-0x78 ||
               !ClientEntityProbe::Value(read,entry+0x48,flags) || (scene && !ClientEntityProbe::Value(read,scene+0x70,sceneFlags)))return result;
            const bool sceneHidden=(sceneFlags&(std::uint64_t{1}<<60))!=0;hidden+=sceneHidden;
            visible=visible && (flags&2u) && scene && !sceneHidden;
        }
        Address afterComponent=0,afterData=0;int afterCount=0;
        if(!ClientEntityProbe::Value(read,entity+0x4a8,afterComponent) || afterComponent!=component ||
           !ClientEntityProbe::Value(read,component+0x38,afterCount) || afterCount!=count ||
           !ClientEntityProbe::Value(read,component+0x40,afterData) || afterData!=data)return result;
        result.layoutValid=true;result.component=component;result.objects=unsigned(count);result.hiddenObjects=hidden;result.enabled=enabled;result.interpolationReady=interpolation;
        result.ready=inspectEntries && enabled && interpolation && count && visible;return result;
    }
};
} // namespace cs2_live
