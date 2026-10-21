#include "../team_palette.h"
#include <cassert>
#include <iostream>
#include <limits>

using Probe = cs2_live::ClientEntityProbe;
using Json = Probe::Json;
using Address = Probe::Address;

struct FakeMemory {
    struct Region { Address address; std::vector<unsigned char> bytes; };
    struct Actor { Address identity, instance, entityClass; std::uint32_t handle; };
    std::vector<Region> regions;
    std::vector<std::pair<Address, std::size_t>> reads;
    std::function<void(Address, std::size_t)> beforeRead;
    Address next = 0x100000, system;
    std::array<Address, 64> pages{};
    Probe::Fields fields{0x557, 0xc20, 0x1000, 0x1080, 0x1088};
    FakeMemory() { system = Allocate(0x210); }
    Address Allocate(std::size_t size) {
        const auto address = next; next += ((size + 4095) & ~Address(4095)) + 4096;
        regions.push_back({address, std::vector<unsigned char>(size)}); return address;
    }
    void Bytes(Address address, const void* bytes, std::size_t size) {
        for (auto& region : regions) {
            if (address >= region.address && address - region.address <= region.bytes.size() &&
                size <= region.bytes.size() - (address - region.address)) {
                std::memcpy(region.bytes.data() + address - region.address, bytes, size); return;
            }
        }
        assert(false && "fake write must fit an allocated region");
    }
    template<class T> void Put(Address address, T value) { Bytes(address, &value, sizeof(value)); }
    Address Text(const std::string& value) {
        assert(value.size() < 128); const auto address = Allocate(128);
        Bytes(address, value.data(), value.size()); return address;
    }
    Address Class(const std::string& name) {
        const auto entityClass = Allocate(0x100), info = Allocate(0x80), text = Text(name);
        Put(entityClass + 0x58, info); Put(info + 8, text); return entityClass;
    }
    Actor Add(unsigned index, const std::string& className = "C_CSPlayerPawn", unsigned team = 2,
              std::uint32_t serial = 7, std::uint32_t flags = 0) {
        assert(index < 32768 && serial < 131072);
        const auto pageNumber = index / Probe::PageEntries;
        if (!pages[pageNumber]) {
            pages[pageNumber] = Allocate(Probe::PageEntries * Probe::IdentitySize);
            Put(system + 0x10 + pageNumber * sizeof(Address), pages[pageNumber]);
        }
        const auto identity = pages[pageNumber] + (index % Probe::PageEntries) * Probe::IdentitySize;
        const auto instance = Allocate(0x2000), entityClass = Class(className);
        const auto handle = (serial << 15) | index;
        Put(identity, instance); Put(identity + 8, entityClass); Put(identity + 0x10, handle);
        Put(identity + 0x20, Text("player")); Put(identity + 0x30, flags);
        Put(instance + fields.team, static_cast<std::uint8_t>(team));
        const std::array<unsigned char, 4> color{12, 34, 56, 255};
        Bytes(instance + fields.renderColor, color.data(), color.size());
        const std::string player = "Игрок";
        Bytes(instance + *fields.playerName, player.data(), player.size());
        Put(instance + *fields.steamId, std::uint64_t(76561198012345678ull));
        Put(instance + *fields.pawnHandle, std::uint32_t((9 << 15) | 7));
        return {identity, instance, entityClass, handle};
    }
    bool Read(Address address, void* output, std::size_t size) {
        reads.push_back({address, size});
        if (beforeRead) beforeRead(address, size);
        if (address + size < address) return false;
        for (const auto& region : regions) {
            if (address >= region.address && address - region.address <= region.bytes.size() &&
                size <= region.bytes.size() - (address - region.address)) {
                std::memcpy(output, region.bytes.data() + address - region.address, size); return true;
            }
        }
        return false;
    }
    Probe::Read Reader() { return [this](Address address, void* output, std::size_t size) { return Read(address, output, size); }; }
    Json Inspect(unsigned limit = 64) { return Probe::Inspect(Reader(), system, fields, limit); }
};

template<class F> static void Reject(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}

static void TestReadOnlyActorsAndHandleSerial() {
    FakeMemory memory;
    const auto pawn = memory.Add(7, "C_CSPlayerPawn", 2, 0x12345);
    memory.Add(8, "CCSPlayerController", 2);
    memory.Add(9, "C_DynamicProp", 3);
    const auto state = memory.Inspect();
    assert(state["read_only"] == true && state["palette_supported"] == false && state["render_verified"] == false);
    assert(state["identity_count"] == 3 && state["pawn_count"] == 1 && state["controller_count"] == 1);
    assert(state["unreadable_pages"] == 0 && state["skipped_lifecycle"] == 0 && state["truncated"] == false);
    const auto& entries = state["entries"];
    assert(entries.size() == 2);
    assert(entries[0]["index"] == 7 && entries[0]["handle"] == pawn.handle && entries[0]["serial"] == 0x12345);
    assert(entries[0]["class"] == "C_CSPlayerPawn" && entries[0]["team"] == 2);
    assert(entries[0]["render_color"] == Json::array({12, 34, 56, 255}));
    assert(entries[0]["designer_name"] == "player" && entries[0]["dormant"] == false);
    assert(entries[1]["class"] == "CCSPlayerController" && entries[1]["player_name"] == "Игрок");
    assert(entries[1]["steam_id"] == "76561198012345678" && entries[1]["pawn_handle"] == ((9 << 15) | 7));
    assert(!entries[1].contains("render_color"));
    for (const auto& entry : entries) {
        assert(!entry.contains("instance") && !entry.contains("entity_class") && !entry.contains("address"));
    }
    assert(Json::parse(state.dump()) == state);
    const auto actor=Probe::Resolve(memory.Reader(),memory.system,pawn.handle);
    assert(actor && actor->instance==pawn.instance && actor->className=="C_CSPlayerPawn");
    assert(Probe::StillCurrent(memory.Reader(),*actor));
    assert(!Probe::Resolve(memory.Reader(),memory.system,pawn.handle+(1u<<15))); // Same index, stale serial.
    memory.Put(pawn.identity+8,memory.Class("C_DynamicProp"));
    assert(!Probe::StillCurrent(memory.Reader(),*actor));
}

static void TestPageAndIndexBounds() {
    FakeMemory memory;
    const auto first = memory.Add(0), last = memory.Add(Probe::MaxIndex);
    const auto reserved = memory.Add(0x7fff);
    const auto mismatch = memory.Add(17);
    memory.Put(mismatch.identity + 0x10, std::uint32_t((7 << 15) | 18));
    const auto noInstance = memory.Add(19); memory.Put(noInstance.identity, Address(0));
    const auto noClass = memory.Add(20); memory.Put(noClass.identity + 8, Address(0));
    const auto state = memory.Inspect();
    assert(state["identity_count"] == 2 && state["entries"].size() == 2);
    assert(state["entries"][0]["index"] == 0 && state["entries"][1]["index"] == Probe::MaxIndex);
    bool readFirst = false, readLast = false;
    for (const auto& read : memory.reads) {
        if (read.second == 0x34 && read.first == first.identity) readFirst = true;
        if (read.second == 0x34 && read.first == last.identity) readLast = true;
        assert(!(read.second == 0x34 && read.first == reserved.identity));
    }
    assert(readFirst && readLast);
    assert(!Probe::Resolve(memory.Reader(),memory.system,reserved.handle));
    assert(!Probe::Resolve(memory.Reader(),memory.system,mismatch.handle));
    memory.Put(memory.system + 0x10 + sizeof(Address) * 30, Address(0xdead0000));
    assert(memory.Inspect()["unreadable_pages"] == 1);
    Reject([&] { memory.Inspect(0); }); Reject([&] { memory.Inspect(129); });
    Reject([&] { Probe::Inspect({}, memory.system, memory.fields); });
    Reject([&] { Probe::Inspect(memory.Reader(), 0, memory.fields); });
    auto fields = memory.fields; fields.team = 0;
    Reject([&] { Probe::Inspect(memory.Reader(), memory.system, fields); });
    std::uint64_t value = 0;
    const auto reads = memory.reads.size();
    assert(!Probe::Value(memory.Reader(), std::numeric_limits<Address>::max() - 3, value));
    assert(memory.reads.size() == reads);
}

static void TestLifecycleAndDormancy() {
    FakeMemory memory; unsigned index = 1;
    for (const auto flag : {0x1u, 0x2u, 0x10u, 0x100u, 0x200u, 0x400u})
        memory.Add(index++, "C_CSPlayerPawn", 2, 7, flag);
    memory.Add(index, "C_CSPlayerPawn", 3, 7, 0x80);
    const auto state = memory.Inspect();
    assert(state["skipped_lifecycle"] == 6 && state["identity_count"] == 1 && state["entries"].size() == 1);
    assert(state["entries"][0]["index"] == index && state["entries"][0]["dormant"] == true);
}

static void TestRecyclingDuringRead() {
    for (unsigned change = 0; change != 4; ++change) {
        FakeMemory memory; const auto actor = memory.Add(7);
        const auto replacement = memory.Class("C_DynamicProp"); bool changed = false;
        memory.beforeRead = [&](Address address, std::size_t size) {
            if (changed || address != actor.identity || size != 0x34) return;
            changed = true;
            if (change == 0) memory.Put(actor.identity, Address(actor.instance + 8));
            if (change == 1) memory.Put(actor.identity + 0x10, std::uint32_t(actor.handle + (1 << 15)));
            if (change == 2) memory.Put(actor.identity + 0x30, std::uint32_t(0x80));
            if (change == 3) memory.Put(actor.identity + 8, replacement);
        };
        const auto state = memory.Inspect();
        assert(changed && state["entries"].empty() && state["skipped_lifecycle"] == 1);
    }
}

static void TestTextAndLimit() {
    FakeMemory memory; const auto actor = memory.Add(4);
    const std::string broken("actor-\xff", 7);
    memory.Put(actor.identity + 0x20, memory.Text(broken));
    auto state = memory.Inspect();
    assert(state["entries"][0]["designer_name"] == "actor-\xef\xbf\xbd");
    assert(Json::parse(state.dump()) == state);
    const auto unterminated = memory.Allocate(128);
    const std::string full(128, 'x'); memory.Bytes(unterminated, full.data(), full.size());
    assert(Probe::Text(memory.Reader(), unterminated, 128).empty());
    assert(Probe::Text(memory.Reader(), 0, 128).empty());
    assert(Probe::Text(memory.Reader(), 0xdead0000, 128).empty());
    const Address pageEnd = 0x7fff0ffe;
    memory.regions.push_back({pageEnd, {'Q', '\0'}});
    assert(Probe::Text(memory.Reader(), pageEnd, 128) == "Q");
    assert(memory.reads.back() == std::make_pair(pageEnd, std::size_t(2)));
    for (unsigned i = 10; i < 15; ++i) memory.Add(i);
    state = memory.Inspect(2);
    assert(state["entries"].size() == 2 && state["pawn_count"] == 6 && state["truncated"] == true);
    state = memory.Inspect(128);
    assert(state["entries"].size() == 6 && state["truncated"] == false);
}
static void TestCurrentPageAndPlayerBinding() {
    FakeMemory memory;const auto pawn=memory.Add(7,"C_CSPlayerPawn",2,9),controller=memory.Add(8,"CCSPlayerController",2,3);
    const auto player=Probe::ResolvePlayer(memory.Reader(),memory.system,memory.fields,controller.handle);
    assert(player && player->pawn.handle==pawn.handle && player->steamId==76561198012345678ull);
    memory.Put(controller.instance+*memory.fields.pawnHandle,pawn.handle+(1u<<15));
    assert(!Probe::ResolvePlayer(memory.Reader(),memory.system,memory.fields,controller.handle)); // Same index, wrong serial.
    memory.Put(controller.instance+*memory.fields.pawnHandle,pawn.handle);
    memory.Put(controller.instance+memory.fields.team,std::uint8_t(3));
    assert(!Probe::ResolvePlayer(memory.Reader(),memory.system,memory.fields,controller.handle));
    memory.Put(controller.instance+memory.fields.team,std::uint8_t(2));
    const auto identity=Probe::Resolve(memory.Reader(),memory.system,pawn.handle);assert(identity);
    const auto newPage=memory.Allocate(Probe::PageEntries*Probe::IdentitySize);
    memory.Put(memory.system+0x10,newPage); // Old readable page remains unchanged.
    assert(!Probe::StillCurrent(memory.Reader(),*identity));
    assert(!Probe::ResolvePlayer(memory.Reader(),memory.system,memory.fields,controller.handle));
}

static void TestProvenAbsentChildRecovery() {
    FakeMemory memory;const auto child=memory.Add(17288,"C_WorldModelGloves",3,42);
    assert(!Probe::ProvenAbsent(memory.Reader(),memory.system,child.handle));
    memory.Put(child.identity,Address(0));memory.Put(child.identity+0x10,child.handle+(1u<<15));
    assert(Probe::ProvenAbsent(memory.Reader(),memory.system,child.handle));
    memory.Put(child.identity,child.instance); // A reused foreign live slot is never an absent child.
    assert(!Probe::ProvenAbsent(memory.Reader(),memory.system,child.handle));
    memory.Put(child.identity,Address(0));unsigned reads=0;
    memory.beforeRead=[&](Address address,std::size_t){if(address==child.identity && ++reads==2)memory.Put(child.identity,child.instance);};
    assert(!Probe::ProvenAbsent(memory.Reader(),memory.system,child.handle));
    memory.beforeRead={};memory.Put(child.identity,Address(0));const auto oldPage=memory.pages[17288/512];
    memory.beforeRead=[&](Address address,std::size_t){if(address==child.identity)memory.Put(memory.system+0x10+sizeof(Address)*(17288/512),oldPage+4096);};
    assert(!Probe::ProvenAbsent(memory.Reader(),memory.system,child.handle));
    assert(!Probe::ProvenAbsent(memory.Reader(),0,child.handle));
    assert(!Probe::ProvenAbsent(memory.Reader(),memory.system,0xffffffffu));
}

int main() {
    TestReadOnlyActorsAndHandleSerial();
    TestPageAndIndexBounds();
    TestLifecycleAndDormancy();
    TestRecyclingDuringRead();
    TestTextAndLimit();
    TestCurrentPageAndPlayerBinding();
    TestProvenAbsentChildRecovery();
    std::cout << "Client entity read-only probe tests passed\n";
}
