#include "../client_build_profile.h"
#include <cassert>
#include <iostream>
#include <set>
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
    std::cout<<"Client profiles: current entity/spectator/cosmetics mappings, legacy isolation, unknown build/site and changed/unreadable code rejection passed\n";
}
