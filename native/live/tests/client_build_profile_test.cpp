#include "../client_build_profile.h"
#include <cassert>
#include <iostream>
#include <set>
#include <limits>
using Profile=cs2_live::ClientBuildProfile;
template<class F>void Reject(F operation){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}assert(rejected);}
static std::array<unsigned char,20> Build(const char* text) {
    std::array<unsigned char,20> result{};
    const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(unsigned i=0;i<20;++i)result[i]=(digit(text[i*2])<<4)|digit(text[i*2+1]);
    return result;
}
int main() {
    auto id=Build(Profile::CurrentBuildId);const auto* profile=Profile::Find(id.data(),id.size());
    assert(profile && profile->current && profile->presentation);
    assert(profile->Offset(0x44f83b8)==0x44f9ff8 && profile->Offset(0x4b95610)==0x4b97250);
    // Code, data globals, command strings and RTTI moved independently.
    assert(profile->Offset(0x1907110)==0x1908350 && profile->Offset(0x4687d38)==0x4689958);
    assert(profile->Offset(0x49397b0)==0x493b3b0 && profile->Offset(0xb6d0b1)==0xb6d07f);
    assert(profile->Offset(0x44c6f58)==0x44c8b98 && profile->Offset(0x44c6368)==0x44c7fa8);
    Reject([&]{profile->Offset(0x123456);});
    assert(!Profile::Find(nullptr,20) && !Profile::Find(id.data(),19));
    id[4]^=1;assert(!Profile::Find(id.data(),id.size()));
    const auto oldId=Build("f1e04e3616921a3bb25940eea9b4b6bfd7ccada5");const auto* old=Profile::Find(oldId.data(),oldId.size());
    assert(old && !old->current && old->presentation && old->Offset(0x4b95610)==0x4b95610);
    const auto previousId=Build("d5bacd2f5df4a3a68fa8bee8692337993548e14a");
    assert(Profile::Find(previousId.data(),previousId.size()) && !Profile::Find(previousId.data(),previousId.size())->presentation);
    constexpr Profile::Address base=0x10000000;
    std::set<Profile::Address> offsets;
    for(const auto& site:Profile::Instructions()) {
        assert(offsets.insert(site.previous).second && !site.bytes.empty());
        unsigned reads=0;bool corrupt=false,unreadable=false;
        Profile::Read read=[&](Profile::Address address,void* output,std::size_t size){
            ++reads;if(address!=base+site.current || size!=site.bytes.size() || unreadable)return false;
            std::memcpy(output,site.bytes.data(),size);if(corrupt)static_cast<unsigned char*>(output)[size-1]^=1;return true;
        };
        // The current build never falls back to the old address/byte sequence.
        profile->Check(read,base,site.previous,{0xcc});assert(reads==1);
        corrupt=true;Reject([&]{profile->Check(read,base,site.previous,{0xcc});});
        corrupt=false;unreadable=true;Reject([&]{profile->Check(read,base,site.previous,{0xcc});});
    }
    unsigned reads=0;Profile::Read unused=[&](Profile::Address,void*,std::size_t){++reads;return true;};
    Reject([&]{profile->Check(unused,base,0x49083b8,{0x90});});assert(reads==0);
    for(const auto& mapping:Profile::Mappings())assert(offsets.insert(mapping.previous).second);
    const auto latestId=Build(Profile::LatestBuildId);
    const auto* latest=Profile::Find(latestId.data(),latestId.size());
    assert(latest && latest->current && latest->presentation);
    assert(latest->revision==Profile::Revision::Steam26334290);
    assert(profile->revision==Profile::Revision::Steam26144732);
    assert(latest->Offset(0x44f83b8)==0x44fc600 && latest->Offset(0x4b95610)==0x4b99c90);
    assert(latest->Offset(0x1907110)==0x19095d0 && latest->Offset(0x4687d38)==0x468c2f8);
    assert(latest->Offset(0x491ca70)==0x4920ff0 && latest->Offset(0x49083b8)==0x490c998);
    assert(latest->Offset(0xb0e236)==0xb0e240 && latest->Offset(0xb6d0b1)==0xb6d183);
    assert(latest->Offset(0x44c6f58)==0x44cb198 && latest->Offset(0x44c6368)==0x44ca5a8);
    assert(latest->Offset(0x49397b0)==0x493dd30 && latest->Offset(0x450bd30)==0x450ff70);
    std::set<Profile::Address> latestOffsets;
    for(const auto& site:latest->SelectedInstructions()) {
        assert(latestOffsets.insert(site.previous).second && !site.bytes.empty());
        const auto* prior=Profile::FindInstruction(site.previous);
        assert(prior && prior->current!=site.current);
        assert(Profile::FindInstruction(site.previous,latest->revision)==&site);
        unsigned readCount=0;bool corrupt=false,unreadable=false;
        Profile::Read exact=[&](Profile::Address address,void* output,std::size_t size) {
            ++readCount;if(address!=base+site.current || size!=site.bytes.size() || unreadable)return false;
            std::memcpy(output,site.bytes.data(),size);if(corrupt)static_cast<unsigned char*>(output)[0]^=1;return true;
        };
        latest->Check(exact,base,site.previous,{0xcc});assert(readCount==1);
        corrupt=true;Reject([&]{latest->Check(exact,base,site.previous,{0xcc});});
        corrupt=false;unreadable=true;Reject([&]{latest->Check(exact,base,site.previous,{0xcc});});
        // A correct previous-build instruction at its old address cannot
        // satisfy the new profile, even where the machine code is identical.
        Profile::Read priorOnly=[&](Profile::Address address,void* output,std::size_t size) {
            if(address!=base+prior->current || size!=prior->bytes.size())return false;
            std::memcpy(output,prior->bytes.data(),size);return true;
        };
        Reject([&]{latest->Check(priorOnly,base,site.previous,{0xcc});});
        readCount=0;
        Reject([&]{latest->Check(exact,0,site.previous,{0xcc});});
        Reject([&]{latest->Check(exact,std::numeric_limits<Profile::Address>::max()-site.current+1,site.previous,{0xcc});});
        assert(readCount==0);
    }
    for(const auto& mapping:latest->SelectedMappings()) {
        assert(latestOffsets.insert(mapping.previous).second);
        assert(latest->Offset(mapping.previous)==mapping.current);
        assert(profile->Offset(mapping.previous)!=mapping.current);
        Reject([&]{latest->Check(unused,base,mapping.previous,{0x90});});
    }
    assert(latestOffsets==offsets && latestOffsets.size()==35);
    Reject([&]{latest->Offset(0x123456);});
    std::cout<<"Client profiles: two independent Steam runtime mappings, legacy isolation, unknown build/site, overflow and changed/unreadable code rejection passed\n";
}
