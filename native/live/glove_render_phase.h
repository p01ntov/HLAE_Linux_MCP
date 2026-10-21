#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include "team_palette.h"

namespace cs2_live {
// The one verified ProcessInterpolatedList call, after both native render
// readiness setters. Original integer/float arguments always pass unchanged.
class GloveRenderPhase {
public:
    using Address=std::uintptr_t;using Read=ClientEntityProbe::Read;
    using Original=void(*)(int,float);
    static constexpr Address Site=0x188786b,Target=0x1885400;
    static bool Guard(const Read& read,Address base) {
        const std::array<unsigned char,15> arguments={0xf3,0x0f,0x10,0x85,0x7c,0xff,0xff,0xff,0x89,0xdf,0xe8,0x90,0xdb,0xff,0xff};
        const std::array<unsigned char,16> prologue={0x4c,0x8d,0x54,0x24,0x08,0x48,0x83,0xe4,0x80,0x41,0xff,0x72,0xf8,0x55,0x48,0x8d};
        std::array<unsigned char,15> a{};std::array<unsigned char,16> p{};
        return read && base<=std::numeric_limits<Address>::max()-Site-16 && read(base+Site-10,a.data(),a.size()) && a==arguments && read(base+Target,p.data(),p.size()) && p==prologue;
    }
    static void Install(Address base,const Read& read,std::function<void()> after) {
        if(installed_) {if(!Current(base,read))throw std::runtime_error("glove render phase callsite ownership changed");if(base_!=base)throw std::runtime_error("glove render phase module changed");after_=std::move(after);return;}
        if(!Guard(read,base))throw std::runtime_error("glove post-interpolation callsite ABI differs");
        const long page=sysconf(_SC_PAGESIZE);if(page<=0 || (page&(page-1)))throw std::runtime_error("invalid glove render phase page size");page_=std::size_t(page);
        const auto reference=(base+Site)&~Address(page_-1);
        for(unsigned step=1;step<32 && !thunk_;++step)for(int direction:{1,-1}) {
            const Address candidate=direction>0?reference+Address(step)*0x4000000:reference-Address(step)*0x4000000;
            void* ptr=mmap(reinterpret_cast<void*>(candidate),page_,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(ptr==MAP_FAILED)continue;
            const auto delta=std::int64_t(Address(ptr))-std::int64_t(base+Site+5);
            if(delta>=std::numeric_limits<std::int32_t>::min() && delta<=std::numeric_limits<std::int32_t>::max())thunk_=ptr;else munmap(ptr,page_);
        }
        if(!thunk_)throw std::runtime_error("could not allocate a nearby glove render phase thunk");
        auto* code=static_cast<unsigned char*>(thunk_);code[0]=0x48;code[1]=0xb8;const auto callback=&Callback;std::memcpy(code+2,&callback,8);code[10]=0xff;code[11]=0xe0;
        if(mprotect(thunk_,page_,PROT_READ|PROT_EXEC)!=0){munmap(thunk_,page_);thunk_=nullptr;throw std::runtime_error("could not protect glove render phase thunk");}
        base_=base;original_=reinterpret_cast<Original>(base+Target);after_=std::move(after);replacement_={0xe8,0,0,0,0};const auto delta=std::int32_t(Address(thunk_)-(base+Site+5));std::memcpy(replacement_.data()+1,&delta,4);
        installed_=true;if(!Write(base+Site,replacement_.data(),5)){after_={};throw std::runtime_error("could not install glove render phase callsite");}installed_=true;
    }
    static bool Remove(const Read& read) {
        if(!installed_)return true;
        std::array<unsigned char,5> actual{};
        if(!read(base_+Site,actual.data(),actual.size()) || (actual!=replacement_ && actual!=Bytes))return false;
        if(actual==replacement_ && !Write(base_+Site,Bytes.data(),Bytes.size()))return false;
        installed_=false;after_={};original_=nullptr;if(thunk_)munmap(thunk_,page_);thunk_=nullptr;return true;
    }
    static bool Current(Address base,const Read& read) {std::array<unsigned char,5> bytes{};return !installed_ || (base_==base && read(base+Site,bytes.data(),bytes.size()) && bytes==replacement_);}
    static bool Installed(){return installed_;}
private:
    inline static constexpr std::array<unsigned char,5> Bytes={0xe8,0x90,0xdb,0xff,0xff};
    inline static std::array<unsigned char,5> replacement_{};
    inline static Address base_=0;inline static std::size_t page_=0;inline static void* thunk_=nullptr;
    inline static Original original_=nullptr;inline static std::function<void()> after_;inline static bool installed_=false;
    static void Callback(int phase,float time) {const auto original=original_;if(original)original(phase,time);try{if(after_)after_();}catch(...){}}
    static bool Write(Address address,const unsigned char* data,std::size_t size) {
        const auto begin=address&~Address(page_-1),end=(address+size+page_-1)&~Address(page_-1);
        if(mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_WRITE|PROT_EXEC)!=0)return false;
        std::memcpy(reinterpret_cast<void*>(address),data,size);__builtin___clear_cache(reinterpret_cast<char*>(address),reinterpret_cast<char*>(address+size));
        return mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_EXEC)==0;
    }
};
} // namespace cs2_live
