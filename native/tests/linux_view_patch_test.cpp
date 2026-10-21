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
constexpr std::size_t MappedSize = 0x1938000;
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
    segments[1].p_vaddr = 0x1936e40;
    segments[1].p_memsz = 0x800;
    const ElfW(Nhdr) note{4, static_cast<ElfW(Word)>(build.size()), NT_GNU_BUILD_ID};
    std::memcpy(mapped + 0x100, &note, sizeof(note));
    std::memcpy(mapped + 0x100 + sizeof(note), "GNU", 4);
    std::memcpy(mapped + 0x100 + sizeof(note) + 4, build.data(), build.size());
    std::memcpy(mapped + cs2_mvp::LinuxViewPatch::Site,
                cs2_mvp::LinuxViewPatch::Original.data(), cs2_mvp::LinuxViewPatch::Original.size());
    Put(0x1936e40, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x49,0x89,
                    0xf6,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78});
    Put(0x1937216, {0xf3,0x41,0x0f,0x11,0x86,0x98,0x04,0x00,0x00});
    Put(0x1937473, {0x84,0xc0,0x74,0x4f});
    Put(0x1937490, {0x49,0x8b,0x86,0xa0,0x04,0x00,0x00});
    Put(0x19374ab, {0x49,0x8b,0x86,0xb8,0x04,0x00,0x00});
    Put(0x1937538, {0xf3,0x41,0x0f,0x5c,0x8e,0x98,0x04,0x00,0x00});
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
    assert(!Patch::IsValidatedBuildId(nullptr, Patch::BuildId.size()));
    assert(!Patch::IsValidatedBuildId(Patch::BuildId.data(), Patch::BuildId.size() - 1));
    for (const auto& build : {Patch::BuildId, Patch::PreviousBuildId}) {
        Fixture(build);
        Patch patch;
        assert(patch.Install(reinterpret_cast<void*>(&DemoView)) == nullptr);
        assert(patch.Installed());
        assert(std::strcmp(patch.LoadedBuildId(),build==Patch::BuildId?Patch::BuildIdText:"d5bacd2f5df4a3a68fa8bee8692337993548e14a")==0);
        const auto* site = mapped + Patch::Site;
        assert(site[0] == 0x4c && site[1] == 0x89 && site[2] == 0xf7); // rdi <- r14
        void* callback = nullptr;
        std::memcpy(&callback, site + 5, sizeof(callback));
        assert(callback == reinterpret_cast<void*>(&DemoView));
        assert(site[13] == 0xff && site[14] == 0xd0); // call rax
        assert(patch.Remove());
        assert(!patch.Installed());
        assert(std::strcmp(patch.LoadedBuildId(),"")==0);
        assert(!std::memcmp(site, Patch::Original.data(), Patch::Original.size()));
    }
    auto unknown = Patch::BuildId;
    unknown[0] ^= 1;
    Fixture(unknown);
    MustReject();
    for (auto address : {Patch::Site, std::uintptr_t{0x1937490}, std::uintptr_t{0x19374ab},
                         std::uintptr_t{0x1936e48}, std::uintptr_t{0x1937216},
                         std::uintptr_t{0x1937473}, std::uintptr_t{0x1937538}}) {
        Fixture(Patch::BuildId);
        mapped[address] ^= 1;
        MustReject();
    }
    Fixture(Patch::BuildId);
    segments[1].p_flags = PF_R;
    MustReject();
    Fixture(Patch::BuildId);
    segments[1].p_memsz = Patch::Site + Patch::Original.size() - segments[1].p_vaddr;
    MustReject(); // Guards after the patch site must also be executable/mapped.
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
