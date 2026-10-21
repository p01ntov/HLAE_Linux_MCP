#pragma once

// Offline demo-only adapter for the explicitly validated Linux client builds.
// Addresses and field accesses come from their ELF disassembly, not Windows
// offsets. The two builds share the patch site and CViewSetup layout.
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
    // Steam build 26087496. Verified against libclient.so SHA-256
    // 6c822356681db5ed978debb5337ad546f23b91f526762aa0a3ae4c844baaae46.
    static constexpr const char* BuildIdText = "f1e04e3616921a3bb25940eea9b4b6bfd7ccada5";
    static constexpr std::array<unsigned char, 20> BuildId = {
        0xf1,0xe0,0x4e,0x36,0x16,0x92,0x1a,0x3b,0xb2,0x59,
        0x40,0xee,0xa9,0xb4,0xb6,0xbf,0xd7,0xcc,0xad,0xa5};
    // Previously validated Steam build 25966348 retains its original guards.
    static constexpr std::array<unsigned char, 20> PreviousBuildId = {
        0xd5,0xba,0xcd,0x2f,0x5d,0xf4,0xa3,0xa6,0x8f,0xa8,
        0xbe,0xe8,0x69,0x23,0x37,0x99,0x35,0x48,0xe1,0x4a};
    static constexpr std::array<unsigned char, 19> Original = {
        0x48,0x8d,0x05,0x51,0x23,0x00,0x03,0x48,0x8b,0x38,
        0x48,0x8b,0x07,0xff,0x90,0x50,0x01,0x00,0x00};

    static bool IsValidatedBuildId(const unsigned char* bytes, std::size_t size) {
        return bytes && size == BuildId.size()
            && (std::memcmp(bytes, BuildId.data(), size) == 0
                || std::memcmp(bytes, PreviousBuildId.data(), size) == 0);
    }

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
        if (search.currentBuild) {
            // Revalidated in 26087496: the second SysV argument is retained in
            // r14, the original bool result is tested immediately after our
            // replacement call, and FOV is initialized/read as float at 0x498.
            const unsigned char prologue[]{
                0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x49,0x89,
                0xf6,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78};
            const unsigned char boolResult[]{0x84,0xc0,0x74,0x4f};
            const unsigned char fovWrite[]{0xf3,0x41,0x0f,0x11,0x86,0x98,0x04,0x00,0x00};
            const unsigned char fovRead[]{0xf3,0x41,0x0f,0x5c,0x8e,0x98,0x04,0x00,0x00};
            if (std::memcmp(reinterpret_cast<void*>(search.base + ViewFunction), prologue, sizeof(prologue)) != 0
                || std::memcmp(site + Original.size(), boolResult, sizeof(boolResult)) != 0
                || std::memcmp(reinterpret_cast<void*>(search.base + 0x1937216), fovWrite, sizeof(fovWrite)) != 0
                || std::memcmp(reinterpret_cast<void*>(search.base + 0x1937538), fovRead, sizeof(fovRead)) != 0)
                return "Linux main-view ABI/FOV instructions do not match the validated build";
        }
        // Replace the engine IsPlayingDemo call sequence with callback(r14).
        // r14 holds this function's CViewSetup; SysV preserves that register.
        replacement_.fill(0x90);
        replacement_[0]=0x4c; replacement_[1]=0x89; replacement_[2]=0xf7; // mov rdi,r14
        replacement_[3]=0x48; replacement_[4]=0xb8; // movabs rax,callback
        std::memcpy(replacement_.data()+5, &callback, sizeof(callback));
        replacement_[13]=0xff; replacement_[14]=0xd0; // call rax
        site_ = site;
        loadedBuildId_ = search.currentBuild ? BuildIdText : "d5bacd2f5df4a3a68fa8bee8692337993548e14a";
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
    const char* LoadedBuildId() const { return site_ ? loadedBuildId_ : ""; }

private:
    static constexpr std::uintptr_t ViewFunction = 0x1936e40, GuardEnd = 0x1937541;
    struct Search { std::uintptr_t base=0; int matches=0; bool buildMatches=false, currentBuild=false, siteExecutable=false; };
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
                && ViewFunction>=segment.p_vaddr && GuardEnd<=segment.p_vaddr+segment.p_memsz)
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
                    && IsValidatedBuildId(value,note->n_descsz)) {
                    result->buildMatches=true;
                    result->currentBuild=std::memcmp(value,BuildId.data(),BuildId.size())==0;
                }
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
    const char* loadedBuildId_="";
    std::array<unsigned char,19> replacement_{};
};

} // namespace cs2_mvp
