// Isolated synthetic ELF module: exercises fail-closed build/ABI gates and
// patch restoration without loading or touching the game.
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>

static int IterateClient(int (*callback)(dl_phdr_info*, std::size_t, void*), void* opaque);
static int FindMapped(const void* address, Dl_info* info);
#define dl_iterate_phdr IterateClient
#define dladdr FindMapped
#include "../linux_view_patch.h"
#undef dladdr
#undef dl_iterate_phdr

namespace {
unsigned char* mapped = nullptr;
constexpr std::size_t MappedSize = 0x1939000;
dl_phdr_info module{};
ElfW(Phdr) segments[2]{};
int moduleCount = 1;

void Writable() {
    assert(mprotect(mapped, MappedSize, PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
}

void Put(std::uintptr_t address, std::initializer_list<unsigned char> bytes) {
    std::memcpy(mapped + address, bytes.begin(), bytes.size());
}

void Fixture(const std::array<unsigned char, 20>& build) {
    using Patch = cs2_mvp::LinuxViewPatch;
    const auto* selected = Patch::ValidatedProfile(build.data(), build.size());
    const auto& profile = selected ? *selected : Patch::Profiles[0];
    Writable();
    moduleCount = 1;
    module.dlpi_addr = reinterpret_cast<std::uintptr_t>(mapped);
    module.dlpi_name = "/test/csgo/bin/linuxsteamrt64/libclient.so";
    module.dlpi_phdr = segments;
    module.dlpi_phnum = 2;
    segments[0].p_type = PT_NOTE;
    segments[0].p_vaddr = 0x100;
    segments[0].p_memsz = sizeof(ElfW(Nhdr)) + 4 + build.size();
    segments[1].p_type = PT_LOAD;
    segments[1].p_flags = PF_R | PF_X;
    segments[1].p_vaddr = profile.viewFunction;
    segments[1].p_memsz = profile.guardEnd - profile.viewFunction;
    const ElfW(Nhdr) note{4, static_cast<ElfW(Word)>(build.size()), NT_GNU_BUILD_ID};
    std::memcpy(mapped + 0x100, &note, sizeof(note));
    std::memcpy(mapped + 0x100 + sizeof(note), "GNU", 4);
    std::memcpy(mapped + 0x100 + sizeof(note) + 4, build.data(), build.size());
    std::memcpy(mapped + profile.site, profile.original.data(), profile.original.size());
    Put(profile.viewFunction, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x49,0x89,
                    0xf6,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78});
    Put(profile.fovWrite, {0xf3,0x41,0x0f,0x11,0x86,0x98,0x04,0x00,0x00});
    Put(profile.site + profile.original.size(), {0x84,0xc0,0x74,0x4f});
    Put(profile.originRead, {0x49,0x8b,0x86,0xa0,0x04,0x00,0x00});
    Put(profile.anglesRead, {0x49,0x8b,0x86,0xb8,0x04,0x00,0x00});
    Put(profile.fovRead, {0xf3,0x41,0x0f,0x5c,0x8e,0x98,0x04,0x00,0x00});
}

bool DemoView(void*) { return true; }

void MustReject() {
    cs2_mvp::LinuxViewPatch patch;
    assert(patch.Install(reinterpret_cast<void*>(&DemoView)) != nullptr);
    assert(!patch.Installed());
}
} // namespace

static int IterateClient(int (*callback)(dl_phdr_info*, std::size_t, void*), void* opaque) {
    for (int index = 0; index < moduleCount; ++index) {
        const int result = callback(&module, sizeof(module), opaque);
        if (result) return result;
    }
    return 0;
}

static int FindMapped(const void* address, Dl_info*) {
    return address >= mapped && address < mapped + MappedSize;
}

int main() {
    using Patch = cs2_mvp::LinuxViewPatch;
    mapped = static_cast<unsigned char*>(mmap(nullptr, MappedSize,
        PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(mapped != MAP_FAILED);
    assert(Patch::IsValidatedBuildId(Patch::BuildId.data(), Patch::BuildId.size()));
    assert(Patch::IsValidatedBuildId(Patch::PreviousBuildId.data(), Patch::PreviousBuildId.size()));
    assert(!Patch::IsValidatedBuildId(Patch::CurrentBuildId.data(), Patch::CurrentBuildId.size()));
    for (const auto& profile : Patch::Profiles)
        assert(Patch::IsValidatedViewBuildId(profile.buildId.data(), profile.buildId.size()));
    assert(!Patch::IsValidatedViewBuildId(nullptr, Patch::BuildId.size()));
    assert(!Patch::IsValidatedViewBuildId(Patch::CurrentBuildId.data(), Patch::CurrentBuildId.size()-1));
    assert(!Patch::IsValidatedBuildId(nullptr, Patch::BuildId.size()));
    assert(!Patch::IsValidatedBuildId(Patch::BuildId.data(), Patch::BuildId.size() - 1));
    for (const auto& profile : Patch::Profiles) {
        Fixture(profile.buildId);
        Patch patch;
        assert(patch.Install(reinterpret_cast<void*>(&DemoView)) == nullptr);
        assert(patch.Installed());
        assert(std::strcmp(patch.LoadedBuildId(),profile.buildIdText)==0);
        const auto* site = mapped + profile.site;
        assert(site[0] == 0x4c && site[1] == 0x89 && site[2] == 0xf7); // rdi <- r14
        void* callback = nullptr;
        std::memcpy(&callback, site + 5, sizeof(callback));
        assert(callback == reinterpret_cast<void*>(&DemoView));
        assert(site[13] == 0xff && site[14] == 0xd0); // call rax
        assert(patch.Remove());
        assert(!patch.Installed());
        assert(std::strcmp(patch.LoadedBuildId(),"")==0);
        assert(!std::memcmp(site, profile.original.data(), profile.original.size()));
    }
    auto unknown = Patch::BuildId;
    unknown[0] ^= 1;
    assert(!Patch::IsValidatedViewBuildId(unknown.data(), unknown.size()));
    Fixture(unknown);
    MustReject();
    for (const auto& profile : Patch::Profiles) {
        for (auto address : {profile.site, profile.originRead, profile.anglesRead}) {
            Fixture(profile.buildId);
            mapped[address] ^= 1;
            MustReject();
        }
        if (profile.checkAbi) for (auto address : {profile.viewFunction + 8, profile.fovWrite,
                                                   profile.site + profile.original.size(), profile.fovRead}) {
            Fixture(profile.buildId);
            mapped[address] ^= 1;
            MustReject();
        }
        Fixture(profile.buildId);
        segments[1].p_flags = PF_R;
        MustReject();
        Fixture(profile.buildId);
        --segments[1].p_memsz;
        MustReject(); // The final FOV instruction must also be executable/mapped.
        Fixture(profile.buildId);
        ++segments[1].p_vaddr;
        MustReject(); // Excluding the function prologue is not sufficient coverage.
    }
    // A valid new build ID cannot authorize the old patch bytes or old code range.
    Fixture(Patch::CurrentBuildId);
    std::memcpy(mapped + Patch::CurrentSite, Patch::Original.data(), Patch::Original.size());
    MustReject();
    Fixture(Patch::CurrentBuildId);
    segments[1].p_vaddr = Patch::Profiles[0].viewFunction;
    segments[1].p_memsz = Patch::Profiles[0].guardEnd - segments[1].p_vaddr;
    MustReject();
    // GNU notes may appear after the executable program header.
    Fixture(Patch::CurrentBuildId);
    const auto first = segments[0]; segments[0] = segments[1]; segments[1] = first;
    Patch reordered;
    assert(reordered.Install(reinterpret_cast<void*>(&DemoView)) == nullptr);
    assert(reordered.Remove());
    assert(!std::memcmp(mapped + Patch::CurrentSite, Patch::CurrentOriginal.data(), Patch::CurrentOriginal.size()));
    Fixture(Patch::BuildId);
    moduleCount = 2;
    MustReject();
    Fixture(Patch::BuildId);
    Patch patch;
    assert(patch.Install(reinterpret_cast<void*>(&DemoView)) == nullptr);
    Writable();
    mapped[Patch::Site] ^= 1;
    assert(!patch.Remove()); // Another patch must never be overwritten.
    assert(patch.Installed());
    assert(munmap(mapped, MappedSize) == 0);
}
