#pragma once

// Offline demo-only adapter for this exact Linux client build. Addresses and
// field accesses were derived from its ELF disassembly, not Windows offsets.
#include <array>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace cs2_mvp {

class LinuxViewPatch {
public:
    static constexpr std::uintptr_t Site = 0x1937460;
    static constexpr std::size_t Origin = 0x4a0, Angles = 0x4b8, Fov = 0x498;
    static constexpr std::array<unsigned char, 20> BuildId = {
        0xd5,0xba,0xcd,0x2f,0x5d,0xf4,0xa3,0xa6,0x8f,0xa8,
        0xbe,0xe8,0x69,0x23,0x37,0x99,0x35,0x48,0xe1,0x4a};
    static constexpr std::array<unsigned char, 19> Original = {
        0x48,0x8d,0x05,0x51,0x23,0x00,0x03,0x48,0x8b,0x38,
        0x48,0x8b,0x07,0xff,0x90,0x50,0x01,0x00,0x00};

    const char* Install(void* callback) {
        if (site_) return nullptr;
        Search search{};
        dl_iterate_phdr(&FindClient, &search);
        if (!search.base || search.matches != 1 || !search.buildMatches || !search.siteExecutable)
            return "Linux main-view hook requires the validated libclient GNU build-ID";
        auto site = reinterpret_cast<unsigned char*>(search.base + Site);
        const unsigned char originRead[]{0x49,0x8b,0x86,0xa0,0x04,0x00,0x00};
        const unsigned char anglesRead[]{0x49,0x8b,0x86,0xb8,0x04,0x00,0x00};
        if (std::memcmp(site, Original.data(), Original.size()) != 0
            || std::memcmp(reinterpret_cast<void*>(search.base + 0x1937490), originRead, sizeof(originRead)) != 0
            || std::memcmp(reinterpret_cast<void*>(search.base + 0x19374ab), anglesRead, sizeof(anglesRead)) != 0)
            return "Linux main-view instructions/layout do not match the validated build";
        // Replace the engine IsPlayingDemo call sequence with callback(r14).
        // r14 holds this function's CViewSetup; SysV preserves that register.
        replacement_.fill(0x90);
        replacement_[0]=0x4c; replacement_[1]=0x89; replacement_[2]=0xf7; // mov rdi,r14
        replacement_[3]=0x48; replacement_[4]=0xb8; // movabs rax,callback
        std::memcpy(replacement_.data()+5, &callback, sizeof(callback));
        replacement_[13]=0xff; replacement_[14]=0xd0; // call rax
        site_ = site;
        if (!Write(replacement_.data())) {
            if (std::memcmp(site_, Original.data(), Original.size()) == 0) site_=nullptr;
            else Remove();
            return "Could not install the Linux main-view patch";
        }
        return nullptr;
    }

    bool Remove() {
        if (!site_) return true;
        Dl_info info{};
        if (!dladdr(site_, &info)) { site_=nullptr; return true; }
        if (std::memcmp(site_, replacement_.data(), replacement_.size()) != 0) return false;
        if (!Write(Original.data())) return false;
        site_=nullptr;
        return true;
    }
    bool Installed() const { return site_ != nullptr; }

private:
    struct Search { std::uintptr_t base=0; int matches=0; bool buildMatches=false, siteExecutable=false; };
    static std::size_t Align4(std::size_t value) { return (value+3)&~std::size_t(3); }
    static int FindClient(dl_phdr_info* module, std::size_t, void* opaque) {
        auto* result=static_cast<Search*>(opaque);
        const char* suffix="/csgo/bin/linuxsteamrt64/libclient.so";
        if (!module->dlpi_name || !std::strstr(module->dlpi_name,suffix)) return 0;
        ++result->matches;
        result->base=module->dlpi_addr;
        for (int i=0;i<module->dlpi_phnum;++i) {
            const auto& segment=module->dlpi_phdr[i];
            if (segment.p_type==PT_LOAD && (segment.p_flags&PF_X)
                && Site>=segment.p_vaddr && Site+Original.size()<=segment.p_vaddr+segment.p_memsz)
                result->siteExecutable=true;
            if (segment.p_type!=PT_NOTE) continue;
            auto* data=reinterpret_cast<const unsigned char*>(module->dlpi_addr+segment.p_vaddr);
            std::size_t offset=0;
            while (offset+sizeof(ElfW(Nhdr))<=segment.p_memsz) {
                const auto* note=reinterpret_cast<const ElfW(Nhdr)*>(data+offset);
                const std::size_t total=sizeof(*note)+Align4(note->n_namesz)+Align4(note->n_descsz);
                if (total>segment.p_memsz-offset) break;
                const auto* name=data+offset+sizeof(*note);
                const auto* value=name+Align4(note->n_namesz);
                if (note->n_type==NT_GNU_BUILD_ID && note->n_namesz==4 && std::memcmp(name,"GNU",4)==0
                    && note->n_descsz==BuildId.size() && std::memcmp(value,BuildId.data(),BuildId.size())==0)
                    result->buildMatches=true;
                offset+=total;
            }
        }
        return 0;
    }
    bool Write(const unsigned char* bytes) {
        const auto size=static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
        if (!size || (size&(size-1))) return false;
        const auto begin=reinterpret_cast<std::uintptr_t>(site_)&~(size-1);
        const auto end=(reinterpret_cast<std::uintptr_t>(site_)+Original.size()+size-1)&~(size-1);
        if (mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_WRITE|PROT_EXEC)!=0) return false;
        std::memcpy(site_,bytes,Original.size());
        __builtin___clear_cache(reinterpret_cast<char*>(site_),reinterpret_cast<char*>(site_+Original.size()));
        return mprotect(reinterpret_cast<void*>(begin),end-begin,PROT_READ|PROT_EXEC)==0;
    }
    unsigned char* site_=nullptr;
    std::array<unsigned char,19> replacement_{};
};

} // namespace cs2_mvp
