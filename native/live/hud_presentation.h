#pragma once

// Scoped Linux Panorama presentation for the explicitly verified CS2 pair.
// Install/Frame/SetGameplay/Remove are game-thread operations. Five HUD call
// sites are redirected; the shared Panorama vtable is never modified. Native
// requested arguments are forwarded again when gameplay presentation is off.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

namespace cs2_live {

class HudPresentationEvidence {
public:
    static constexpr std::size_t Count=5;
    void Context(bool demo,std::optional<unsigned> observer) {
        const bool effective=requested_ && demo && observer && *observer==2;
        demo_=demo;observer_=observer;
        if(effective!=effective_){effective_=effective;Fresh();}
    }
    void Request(bool gameplay) {
        if(gameplay && (!demo_ || !observer_ || *observer_!=2))
            throw std::runtime_error("gameplay HUD requires an actual first-person demo view");
        if(requested_!=gameplay){requested_=gameplay;effective_=gameplay;Fresh();}
    }
    bool Override() const {return effective_;}
    bool Requested() const {return requested_;}
    void Record(std::size_t role,bool identity,bool nativeValue,bool applied,std::optional<bool> actual) {
        if(role>=Count)return;
        ++hits_[role];
        const bool expected=effective_?false:nativeValue;
        confirmed_[role]=identity && applied==expected && actual && *actual==expected;
        if(confirmed_[role])++confirmedHits_[role];else ++failures_[role];
    }
    bool Ready() const {
        return std::all_of(confirmed_.begin(),confirmed_.end(),[](bool value){return value;});
    }
    nlohmann::json Capabilities(bool installed,const std::string& reason) const {
        return {{"available",installed&&Ready()},{"installed",installed},{"render_verified",false},
            {"gameplay_requested",requested_},{"gameplay_active",effective_&&Ready()},
            {"pending",installed&&!Ready()},{"observer_mode",observer_?nlohmann::json(*observer_):nlohmann::json(nullptr)},
            {"first_person",demo_&&observer_&&*observer_==2},{"callsite_hits",hits_},
            {"confirmed_hits",confirmedHits_},{"readback_failures",failures_},
            {"classes_confirmed",confirmed_},{"reason",reason},
            {"restore","original native class arguments resume at the next HUD update"}};
    }
private:
    void Fresh(){confirmed_.fill(false);}
    bool requested_=false,effective_=false,demo_=false;
    std::optional<unsigned> observer_;
    std::array<std::uint64_t,Count> hits_{},confirmedHits_{},failures_{};
    std::array<bool,Count> confirmed_{};
};

class LinuxHudPresentation {
public:
    using Json=nlohmann::json;
    using Address=std::uintptr_t;
    using Setter=void(*)(void*,std::uint16_t,bool);
    static constexpr const char* ClientBuild="f1e04e3616921a3bb25940eea9b4b6bfd7ccada5";
    static constexpr const char* PanoramaBuild="2ae4ebf0bd1b90af8517139e08a7e6fe6d4f26bf";
    LinuxHudPresentation()=default;
    LinuxHudPresentation(const LinuxHudPresentation&)=delete;
    LinuxHudPresentation& operator=(const LinuxHudPresentation&)=delete;
    // Call Remove explicitly before unloading the plugin. Failure preserves
    // thunks so an unknown third-party modification cannot create dangling code.
    const char* Install() {
        if(installed_)return nullptr;
        reason_.clear();Module client{"/csgo/bin/linuxsteamrt64/libclient.so",ClientBuild};
        Module panorama{"/bin/linuxsteamrt64/libpanorama.so",PanoramaBuild};
        dl_iterate_phdr(&FindModule,&client);dl_iterate_phdr(&FindModule,&panorama);
        if(client.matches!=1 || panorama.matches!=1 || !client.valid || !panorama.valid)
            return Fail("gameplay HUD requires the verified libclient and libpanorama GNU build IDs");
        client_=client.base;panorama_=panorama.base;
        if(active_ && active_!=this)return Fail("another gameplay HUD adapter is already installed");
        // The string-to-symbol initializers prove which engine symbols the
        // intercepted class arguments represent; checking names alone is weak.
        if(!Bytes(client_+0x1d191df,{0x48,0x8d,0x35,0xe7,0xf3,0xe2,0xfe,0x48,0x8d,0x3d,0x7b,0x2a,0xca,0x02}) ||
           !Bytes(client_+0x1d1926f,{0x48,0x8d,0x35,0x96,0xd4,0xe4,0xfe,0x48,0x8d,0x3d,0xcb,0x29,0xca,0x02}) ||
           !Bytes(client_+0x1d28eb1,{0x48,0x8d,0x35,0x2e,0x70,0xe2,0xfe,0x48,0x8d,0x3d,0x81,0x21,0xc9,0x02}) ||
           !Text(client_+0xb485cd,"HUD--localplayer--spectator") ||
           !Text(client_+0xb6670c,"HUD--spectating-target") ||
           !Text(client_+0xb4fee6,"HudSpecplayerRoot--visible"))
            return Fail("gameplay HUD engine class-symbol identity differs from the verified build");
        if(!Bytes(client_+0x1907110,{0x55,0x31,0xff,0x48,0x89,0xe5,0xe8,0x65,0xd9,0xd1,0xff,0x48,0x85,0xc0,0x74,0x30,0x48,0x8b,0xb8,0x90,0x12,0x00,0x00}) ||
           !Bytes(client_+0x190712c,{0x48,0x8b,0x07,0x48,0x8d,0x15,0x8a,0x54,0x38,0xff,0x48,0x8b,0x80,0xf8,0x00,0x00,0x00,0x48,0x39,0xd0,0x75,0x16,0x0f,0xb6,0x47,0x48,0x5d,0xc3}) ||
           !Bytes(panorama_+0x26c6d0,{0x66,0x83,0xfe,0xff,0x74,0x3a,0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x84,0xd2}) ||
           !Bytes(panorama_+0x26c64e,{0x48,0x63,0x97,0x60,0x01,0x00,0x00,0x85,0xd2,0x7e,0x47,0x48,0x8b,0x8f,0x68,0x01,0x00,0x00}))
            return Fail("gameplay HUD observer or Panorama setter/class-list ABI differs from the verified build");
        Address setter=0;
        if(!Read(panorama_+0x6851e8+0x508,setter) || setter!=panorama_+0x26c6d0)
            return Fail("gameplay HUD CUIPanel SetHasClass identity differs from the verified build");
        const std::array<std::pair<Address,std::vector<unsigned char>>,5> guards={{
            {0x1d17bac,{0x48,0x8b,0x83,0xe8,0x01,0x00,0x00,0x0f,0xb7,0x35,0x8e,0x40,0xca,0x02,0x48,0x8b,0x78,0x08,0x48,0x8b,0x07,0xff,0x90,0x08,0x05,0x00,0x00}},
            {0x1d17bdc,{0x0f,0xb7,0x35,0x85,0x40,0xca,0x02,0x48,0x8b,0x07,0x0f,0x94,0xc2,0x0f,0xb6,0xd2,0xff,0x90,0x08,0x05,0x00,0x00}},
            {0x1d28a2b,{0x0f,0xb7,0x35,0x0e,0x26,0xc9,0x02,0x48,0x8b,0x78,0x08,0x41,0x0f,0xb6,0xc7,0x89,0xc2,0x89,0x45,0xb8,0x48,0x8b,0x07,0xff,0x90,0x08,0x05,0x00,0x00}},
            {0x1d28a8c,{0x8b,0x55,0xb8,0x48,0x8b,0x78,0x08,0x0f,0xb7,0x35,0xa6,0x25,0xc9,0x02,0x48,0x8b,0x07,0xff,0x90,0x08,0x05,0x00,0x00}},
            {0x1d28adf,{0x0f,0xb7,0x35,0x5a,0x25,0xc9,0x02,0x48,0x8b,0x78,0x08,0x8b,0x55,0xb8,0x48,0x8b,0x07,0xff,0x90,0x08,0x05,0x00,0x00}}
        }};
        for(const auto& guard:guards)if(!Bytes(client_+guard.first,guard.second))
            return Fail("gameplay HUD scoped callsite arguments differ from the verified build");
        if(!MakeThunks())return Fail("could not allocate a nearby executable gameplay HUD thunk page");
        active_=this;
        for(std::size_t i=0;i<Sites.size();++i) {
            replacements_[i]={0xe8,0,0,0,0,0x90};
            const auto displacement=std::int32_t(Address(thunks_)+i*16-(client_+Sites[i]+5));
            std::memcpy(replacements_[i].data()+1,&displacement,4);
            patched_[i]=true;
            if(!WriteCode(client_+Sites[i],replacements_[i].data(),6)) {
                Remove();return Fail("could not install all gameplay HUD callsites");
            }
        }
        installed_=true;return nullptr;
    }
    void Frame(bool demoPlayback) {evidence_.Context(demoPlayback,ObserverMode());}
    void SetGameplay(bool enabled) {
        if(enabled && !installed_)throw std::runtime_error(reason_.empty()?"gameplay HUD adapter is not installed":reason_);
        evidence_.Request(enabled);
    }
    std::optional<unsigned> ObserverMode() const {
        if(!installed_)return {};
        const auto mode=reinterpret_cast<unsigned(*)()>(client_+0x1907110)();
        return mode<=5?std::optional<unsigned>(mode):std::nullopt;
    }
    Json Capabilities() const {
        auto result=evidence_.Capabilities(installed_,reason_);
        result["client_build_id"]=ClientBuild;result["panorama_build_id"]=PanoramaBuild;
        result["scope"]="five native player-HUD class callsites";return result;
    }
    bool Installed() const{return installed_;}
    bool Remove() {
        evidence_.Request(false);bool restored=true;
        for(std::size_t i=0;i<Sites.size();++i)if(patched_[i]) {
            if(Bytes(client_+Sites[i],Original))patched_[i]=false;
            else if(Bytes(client_+Sites[i],replacements_[i]) && WriteCode(client_+Sites[i],Original.data(),Original.size()))patched_[i]=false;
            else restored=false;
        }
        if(!restored){reason_="could not restore all owned gameplay HUD callsites";return false;}
        installed_=false;if(active_==this)active_=nullptr;
        if(thunks_){munmap(thunks_,pageSize_);thunks_=nullptr;}
        return true;
    }
private:
    inline static LinuxHudPresentation* active_=nullptr;
    inline static constexpr std::array<Address,5> Sites={0x1d17bc1,0x1d17bec,0x1d28a42,0x1d28a9d,0x1d28af0};
    inline static constexpr std::array<Address,5> Symbols={0x49bbc48,0x49bbc68,0x49bb040,0x49bb040,0x49bb040};
    inline static constexpr std::array<unsigned char,6> Original={0xff,0x90,0x08,0x05,0x00,0x00};
    struct Module {const char* suffix;const char* build;Address base=0;unsigned matches=0;bool valid=false;};
    static int FindModule(dl_phdr_info* info,std::size_t,void* opaque) {
        auto& module=*static_cast<Module*>(opaque);
        if(!info->dlpi_name)return 0;
        const std::string name=info->dlpi_name,suffix=module.suffix;
        if(name.size()<suffix.size() || name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0)return 0;
        ++module.matches;module.base=info->dlpi_addr;
        for(unsigned i=0;i<info->dlpi_phnum;++i) {
            const auto& segment=info->dlpi_phdr[i];if(segment.p_type!=PT_NOTE)continue;
            const auto* bytes=reinterpret_cast<const unsigned char*>(module.base+segment.p_vaddr);
            std::size_t offset=0;
            while(offset+sizeof(ElfW(Nhdr))<=segment.p_memsz) {
                const auto* note=reinterpret_cast<const ElfW(Nhdr)*>(bytes+offset);
                const auto align=[](std::size_t size){return (size+3)&~std::size_t(3);};
                const auto total=sizeof(*note)+align(note->n_namesz)+align(note->n_descsz);
                if(total>segment.p_memsz-offset)break;
                const auto* nameBytes=bytes+offset+sizeof(*note);const auto* value=nameBytes+align(note->n_namesz);
                if(note->n_type==NT_GNU_BUILD_ID && note->n_namesz==4 && note->n_descsz==20 && std::memcmp(nameBytes,"GNU",4)==0) {
                    static constexpr char hex[]="0123456789abcdef";char text[41]{};
                    for(unsigned j=0;j<20;++j){text[2*j]=hex[value[j]>>4];text[2*j+1]=hex[value[j]&15];}
                    module.valid=std::strcmp(text,module.build)==0;
                }
                offset+=total;
            }
        }
        return 0;
    }
    static bool Memory(Address address,void* destination,std::size_t size) {
        if(!address || !size || address+size<address)return false;
        iovec local{destination,size},remote{reinterpret_cast<void*>(address),size};
        return process_vm_readv(getpid(),&local,1,&remote,1,0)==ssize_t(size);
    }
    template<class T>static bool Read(Address address,T& value){return Memory(address,&value,sizeof(value));}
    template<class Container>static bool Bytes(Address address,const Container& expected) {
        std::vector<unsigned char> actual(expected.size());
        return Memory(address,actual.data(),actual.size()) && std::equal(actual.begin(),actual.end(),expected.begin());
    }
    static bool Bytes(Address address,std::initializer_list<unsigned char> expected){return Bytes<std::initializer_list<unsigned char>>(address,expected);}
    static bool Text(Address address,const char* text) {
        std::vector<char> actual(std::strlen(text)+1);return Memory(address,actual.data(),actual.size()) && std::memcmp(actual.data(),text,actual.size())==0;
    }
    const char* Fail(const char* error){reason_=error;return reason_.c_str();}
    bool MakeThunks() {
        const long page=sysconf(_SC_PAGESIZE);if(page<=0 || (page&(page-1)))return false;pageSize_=std::size_t(page);
        const Address reference=(client_+Sites[0])&~Address(pageSize_-1);
        for(unsigned step=1;step<32 && !thunks_;++step)for(int direction:{1,-1}) {
            const Address candidate=direction>0?reference+Address(step)*0x4000000:reference-Address(step)*0x4000000;
            void* mapped=mmap(reinterpret_cast<void*>(candidate),pageSize_,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(mapped==MAP_FAILED)continue;
            bool close=true;for(Address site:Sites) {
                const auto delta=std::int64_t(Address(mapped))-std::int64_t(client_+site+5);
                if(delta<std::numeric_limits<std::int32_t>::min()+128 || delta>std::numeric_limits<std::int32_t>::max()-128)close=false;
            }
            if(close)thunks_=mapped;else munmap(mapped,pageSize_);
        }
        if(!thunks_)return false;
        const std::array<Setter,5> callbacks={&Callback0,&Callback1,&Callback2,&Callback3,&Callback4};
        for(unsigned i=0;i<5;++i) {
            auto* code=static_cast<unsigned char*>(thunks_)+16*i;code[0]=0x48;code[1]=0xb8;
            const auto callback=callbacks[i];std::memcpy(code+2,&callback,8);code[10]=0xff;code[11]=0xe0;
        }
        if(mprotect(thunks_,pageSize_,PROT_READ|PROT_EXEC)==0)return true;
        munmap(thunks_,pageSize_);thunks_=nullptr;return false;
    }
    bool WriteCode(Address address,const unsigned char* bytes,std::size_t count) {
        const auto begin=address&~Address(pageSize_-1),end=(address+count+pageSize_-1)&~Address(pageSize_-1);
        if(mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_WRITE|PROT_EXEC)!=0)return false;
        std::memcpy(reinterpret_cast<void*>(address),bytes,count);
        __builtin___clear_cache(reinterpret_cast<char*>(address),reinterpret_cast<char*>(address+count));
        return mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_EXEC)==0;
    }
    static std::optional<bool> HasClass(void* panel,std::uint16_t symbol) {
        // ABI independently proved by the guarded ToggleClass routine. The
        // native caller owns this panel for the duration of this callback.
        int count=0;Address symbols=0;
        if(!Read(Address(panel)+0x160,count) || count<0 || count>1024 || !Read(Address(panel)+0x168,symbols))return {};
        if(!count)return false;
        std::array<std::uint16_t,1024> values{};
        if(!Memory(symbols,values.data(),std::size_t(count)*2))return {};
        const auto end=values.begin()+count;
        return std::find(values.begin(),end,symbol)!=end;
    }
    static void Dispatch(std::size_t role,void* panel,std::uint16_t symbol,bool desired) noexcept {
        auto* self=active_;if(!self)return;
        Address vtable=0,target=0;std::uint16_t expected=0xffff;
        if(!Read(Address(panel),vtable) || !Read(vtable+0x508,target) || !target)return;
        const bool identity=vtable==self->panorama_+0x6851e8 && target==self->panorama_+0x26c6d0 &&
            Read(self->client_+Symbols[role],expected) && expected!=0xffff && expected==symbol;
        const bool applied=identity && self->evidence_.Override()?false:desired;
        reinterpret_cast<Setter>(target)(panel,symbol,applied);
        try {self->evidence_.Record(role,identity,desired,applied,identity?HasClass(panel,symbol):std::nullopt);}
        catch(...){/* diagnostics must not unwind through engine code */}
    }
    static void Callback0(void* p,std::uint16_t s,bool v){Dispatch(0,p,s,v);}
    static void Callback1(void* p,std::uint16_t s,bool v){Dispatch(1,p,s,v);}
    static void Callback2(void* p,std::uint16_t s,bool v){Dispatch(2,p,s,v);}
    static void Callback3(void* p,std::uint16_t s,bool v){Dispatch(3,p,s,v);}
    static void Callback4(void* p,std::uint16_t s,bool v){Dispatch(4,p,s,v);}
    Address client_=0,panorama_=0;void* thunks_=nullptr;std::size_t pageSize_=0;
    bool installed_=false;std::array<bool,5> patched_{};
    std::array<std::array<unsigned char,6>,5> replacements_{};
    std::string reason_;HudPresentationEvidence evidence_;
};
} // namespace cs2_live
