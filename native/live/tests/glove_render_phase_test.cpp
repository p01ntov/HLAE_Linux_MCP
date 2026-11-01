#include "../glove_render_phase.h"
#include <cassert>
#include <iostream>
#include <sys/uio.h>
using Phase=cs2_live::GloveRenderPhase;using A=Phase::Address;
static int nativePhase=0,nativeCalls=0;static float nativeTime=0;
int main(){
    constexpr std::size_t size=0x1900000;void* memory=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(memory!=MAP_FAILED);const A base=A(memory);
    Phase::Read read=[](A a,void* p,std::size_t n){iovec l{p,n},r{reinterpret_cast<void*>(a),n};return process_vm_readv(getpid(),&l,1,&r,1,0)==ssize_t(n);};
    const unsigned char args[]={0xf3,0x0f,0x10,0x85,0x7c,0xff,0xff,0xff,0x89,0xdf,0xe8,0x90,0xdb,0xff,0xff};std::memcpy(reinterpret_cast<void*>(base+Phase::Site-10),args,sizeof(args));
    std::vector<unsigned char> native={0x4c,0x8d,0x54,0x24,0x08,0x48,0x83,0xe4,0x80,0x41,0xff,0x72,0xf8,0x55,0x48,0x8d,0x05,0,0,0,0};
    auto address=[&](const void* p){const auto a=A(p);const auto* b=reinterpret_cast<const unsigned char*>(&a);native.insert(native.end(),b,b+8);};
    native.insert(native.end(),{0x48,0xb8});address(&nativePhase);native.insert(native.end(),{0x89,0x38,0x48,0xb8});address(&nativeTime);native.insert(native.end(),{0xf3,0x0f,0x11,0x00,0x48,0xb8});address(&nativeCalls);native.insert(native.end(),{0x83,0x00,0x01,0x5d,0x49,0x8d,0x62,0xf8,0xc3});
    std::memcpy(reinterpret_cast<void*>(base+Phase::Target),native.data(),native.size());
    assert(Phase::Guard(read,base));*reinterpret_cast<unsigned char*>(base+Phase::Target)^=1;assert(!Phase::Guard(read,base));*reinterpret_cast<unsigned char*>(base+Phase::Target)^=1;
    assert(!Phase::Guard(read,std::numeric_limits<A>::max()-1));
    const unsigned char finish[]={0x48,0x83,0xc4,0x08,0xc3};std::memcpy(reinterpret_cast<void*>(base+Phase::Site+5),finish,sizeof(finish));
    // Enter this isolated CALL with the same SysV stack alignment as the real
    // native caller. The native stub stores the unchanged integer/XMM inputs.
    std::array<unsigned char,16> invoke={0x48,0x83,0xec,0x08,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};const A site=base+Phase::Site;std::memcpy(invoke.data()+6,&site,8);std::memcpy(reinterpret_cast<void*>(base+0x100),invoke.data(),invoke.size());
    assert(mprotect(memory,size,PROT_READ|PROT_EXEC)==0);int afterCalls=0;
    Phase::Install(base,read,[&]{assert(nativeCalls==afterCalls+1);assert(nativePhase==2 && nativeTime==0.25f);++afterCalls;});assert(Phase::Installed() && Phase::Current(base,read));
    reinterpret_cast<void(*)(int,float)>(base+0x100)(2,0.25f);assert(nativeCalls==1 && afterCalls==1);
    Phase::Install(base,read,[]{throw std::runtime_error("observer failure");});reinterpret_cast<void(*)(int,float)>(base+0x100)(3,0.5f);assert(nativeCalls==2 && nativePhase==3 && nativeTime==0.5f);
    std::array<unsigned char,5> owned{};assert(read(site,owned.data(),owned.size()));const auto page=std::size_t(sysconf(_SC_PAGESIZE));const auto pageAddress=site&~A(page-1);
    assert(mprotect(reinterpret_cast<void*>(pageAddress),page,PROT_READ|PROT_WRITE)==0);*reinterpret_cast<unsigned char*>(site)=0x90;assert(mprotect(reinterpret_cast<void*>(pageAddress),page,PROT_READ|PROT_EXEC)==0);
    assert(!Phase::Current(base,read) && !Phase::Remove(read));bool rejected=false;try{Phase::Install(base,read,[]{});}catch(const std::exception&){rejected=true;}assert(rejected);
    assert(mprotect(reinterpret_cast<void*>(pageAddress),page,PROT_READ|PROT_WRITE)==0);std::memcpy(reinterpret_cast<void*>(site),owned.data(),owned.size());assert(mprotect(reinterpret_cast<void*>(pageAddress),page,PROT_READ|PROT_EXEC)==0);
    assert(Phase::Remove(read) && !Phase::Installed() && Phase::Guard(read,base));assert(munmap(memory,size)==0);
    std::cout<<"Glove render phase: exact ABI, original-first integer/float forwarding, exception isolation, ownership reuse and foreign-unpatch guards passed\n";
}
