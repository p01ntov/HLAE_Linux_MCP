#include "../appearance_pack.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using cs2_live::AppearancePack;
using Json = AppearancePack::Json;
namespace fs = std::filesystem;

static std::string ResourceHeader(std::size_t payloadSize) {
    std::string header(28, '\0');
    const auto write = [&](std::size_t offset, std::uint32_t value) {
        for (int i = 0; i != 4; ++i) header[offset + i] = static_cast<char>((value >> (i * 8)) & 255);
    };
    write(0, static_cast<std::uint32_t>(28 + payloadSize));
    header[4] = 12; header[6] = 1;
    write(8, 8); write(12, 1); write(16, 0x41544144); write(20, 8);
    write(24, static_cast<std::uint32_t>(payloadSize));
    return header;
}

struct Temporary {
    fs::path root;
    Temporary() {
        char pattern[] = "/tmp/cs2-appearance-pack-test-XXXXXX";
        const auto path = ::mkdtemp(pattern);
        assert(path); root = path;
    }
    ~Temporary() { std::error_code ignored; fs::remove_all(root, ignored); }
    std::string Pack(const std::string& name, const std::string& model = "models/test/agent.vmdl", const std::string& bytes = "compiled-model-A") {
        const auto directory = root / name;
        const auto file = directory / (model + "_c");
        fs::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::binary); output << ResourceHeader(bytes.size()) << bytes; output.close();
        assert(output.good());
        return directory.string();
    }
};

struct FakeEngine {
    std::vector<std::string> paths, calls;
    std::vector<std::string> reloads;
    std::map<std::string, AppearancePack::Bytes> stock;
    std::string rejectMount, rejectUnmount, rejectReload;
    std::string throwMount, throwRead;
    int rejectReloadCount = 0;
    bool ignorePriority = false;

    static AppearancePack::Bytes ReadFile(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return AppearancePack::Bytes(std::istreambuf_iterator<char>(stream), {});
    }
    AppearancePack::Callbacks Callbacks() {
        return {
            [this](const std::string& path) {
                calls.push_back("mount:" + path);
                if (path == rejectMount) return false;
                paths.insert(paths.begin(), path);
                if (path == throwMount) throw std::runtime_error("mount failed after adding path");
                return true;
            },
            [this](const std::string& path) {
                calls.push_back("unmount:" + path);
                if (path == rejectUnmount) return false;
                paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
                return true;
            },
            [this](const std::string& relative) {
                calls.push_back("read:" + relative);
                if (relative == throwRead) throw std::runtime_error("GAME read failed");
                if (!ignorePriority) for (const auto& path : paths) {
                    const auto file = fs::path(path) / relative;
                    if (fs::exists(file)) return ReadFile(file);
                }
                return stock[relative];
            },
            [this](const std::string& model) {
                calls.push_back("reload:" + model); reloads.push_back(model);
                if (model == rejectReload && rejectReloadCount != 0) {
                    if (rejectReloadCount > 0) --rejectReloadCount;
                    return false;
                }
                return true;
            }
        };
    }
};

static Json Params(const std::string& path, Json models = {"models/test/agent.vmdl"}) {
    return {{"path", path}, {"models", models}};
}
template<class F> static std::string Reject(F operation) {
    try { operation(); }
    catch (const std::exception& error) { return error.what(); }
    assert(false && "operation should have failed"); return {};
}

static void TestApplyReplaceReset() {
    Temporary files;
    const auto first = files.Pack("first");
    const auto second = files.Pack("second", "models/test/weapon.vmdl", "compiled-model-B");
    FakeEngine engine;
    AppearancePack pack;
    assert(!pack.Capabilities()["available"].get<bool>());
    pack.Bind(engine.Callbacks());
    assert(pack.Capabilities()["available"] == true);
    assert(pack.State()["status"] == "idle");
    pack.Apply(Params(first));
    auto state = pack.State();
    assert(state["mounted_verified"] == true && state["reload_requested"] == true);
    assert(state["render_verified"] == false);
    assert(state["scope"] == "global resource replacement");
    assert(state["path"] == first);
    assert(engine.paths == std::vector<std::string>{first});
    assert(engine.calls[0] == "mount:" + first);
    assert(engine.calls[1] == "read:models/test/agent.vmdl_c");
    assert(engine.calls[2] == "reload:models/test/agent.vmdl");
    Reject([&] { pack.Bind(engine.Callbacks()); });
    engine.calls.clear(); engine.reloads.clear();
    pack.Apply(Params(second, {"models/test/weapon.vmdl"}));
    assert(engine.calls[0] == "unmount:" + first && engine.calls[1] == "mount:" + second);
    assert(engine.reloads == (std::vector<std::string>{"models/test/agent.vmdl", "models/test/weapon.vmdl"}));
    assert(engine.paths == std::vector<std::string>{second});
    engine.reloads.clear();
    pack.Reset();
    assert(engine.paths.empty() && engine.reloads.size() == 2);
    state = pack.State();
    assert(state["status"] == "reset_reload_requested");
    assert(state["mounted_verified"] == false && state["reload_requested"] == false);
    assert(state["reset_reload_requested"] == true && state["render_verified"] == false);
    assert(state["mounted_paths"].empty() && state["pending_restore_models"].empty());
    pack.Reset();
    assert(pack.State()["status"] == "idle");
}

static void TestAllValidationPrecedesMount() {
    Temporary files; const auto valid = files.Pack("valid");
    FakeEngine engine; AppearancePack pack; pack.Bind(engine.Callbacks());
    const std::vector<std::string> invalidModels = {
        "../agent.vmdl", "/models/agent.vmdl", "models/../agent.vmdl", "models/./agent.vmdl",
        "models//agent.vmdl", "models/agent.vmdl/", "models/.vmdl", "models/agent.vmdl_c",
        "models/agent.mdl", "models/agent\\x.vmdl", "models/agent\".vmdl", "models/a\n.vmdl",
        "models/a;quit.vmdl", "models/a b.vmdl", std::string("models/a\0.vmdl", 14)
    };
    for (const auto& model : invalidModels) Reject([&] { pack.Apply(Params(valid, {model})); });
    for (const Json& invalid : std::vector<Json>{
        Json::array(), Json::object(), {{"path", valid}}, Params(valid, Json::array()),
        Params(valid, {true}), Params(valid, {"models/test/agent.vmdl", "models/test/agent.vmdl"}),
        {{"path", valid}, {"models", {"models/test/agent.vmdl"}}, {"command", "quit"}},
        Params("relative/path"), Params(valid + "/../valid"), Params(valid + "/./"), Params(valid + "//a"),
        Params(valid + "\n"), Params(valid + std::string("\0suffix", 7)), Params("/missing-appearance-directory")
    }) Reject([&] { pack.Apply(invalid); });
    Json tooMany = Json::array();
    for (int i = 0; i != 33; ++i) tooMany.push_back("models/test/" + std::to_string(i) + ".vmdl");
    Reject([&] { pack.Apply(Params(valid, tooMany)); });
    Reject([&] { pack.Apply(Params(valid, {"models/test/missing.vmdl"})); });
    assert(engine.calls.empty());
    assert(pack.State()["status"] == "idle");
    assert(!pack.State()["last_error"].get<std::string>().empty());
    pack.Apply(Params(valid));
    engine.calls.clear();
    Reject([&] { pack.Apply(Params(valid, {"models/../agent.vmdl"})); });
    assert(engine.calls.empty());
    assert(pack.State()["mounted_verified"] == true && pack.State()["path"] == valid);
    pack.Reset();
}

static void TestSymlinksNonRegularAndSize() {
    Temporary files; const auto valid = files.Pack("valid");
    FakeEngine engine; AppearancePack pack; pack.Bind(engine.Callbacks());
    fs::create_directory_symlink(valid, files.root / "alias");
    Reject([&] { pack.Apply(Params((files.root / "alias").string())); });
    fs::create_directory_symlink(files.root, files.root / "parent-alias");
    Reject([&] { pack.Apply(Params((files.root / "parent-alias/valid").string())); });
    const auto linkedDir = files.root / "linked-directory";
    fs::create_directory(linkedDir);
    fs::create_directory_symlink(fs::path(valid) / "models", linkedDir / "models");
    Reject([&] { pack.Apply(Params(linkedDir.string())); });
    const auto linkedFile = files.Pack("linked-file");
    const auto compiled = fs::path(linkedFile) / "models/test/agent.vmdl_c";
    fs::remove(compiled);
    fs::create_symlink(fs::path(valid) / "models/test/agent.vmdl_c", compiled);
    Reject([&] { pack.Apply(Params(linkedFile)); });
    fs::remove(compiled); fs::create_directory(compiled);
    Reject([&] { pack.Apply(Params(linkedFile)); });
    fs::remove(compiled); assert(::mkfifo(compiled.c_str(), 0600) == 0);
    Reject([&] { pack.Apply(Params(linkedFile)); });
    fs::remove(compiled);
    { std::ofstream empty(compiled); }
    Reject([&] { pack.Apply(Params(linkedFile)); });
    { const int fd = ::open(compiled.c_str(), O_WRONLY); assert(fd >= 0);
      assert(::ftruncate(fd, AppearancePack::MaxModelBytes + 1) == 0); ::close(fd); }
    Reject([&] { pack.Apply(Params(linkedFile)); });
    assert(engine.calls.empty());

    const auto totalDir = files.root / "total-limit";
    fs::create_directories(totalDir / "models"); Json models = Json::array();
    for (int i = 0; i != 5; ++i) {
        const auto name = "models/large" + std::to_string(i) + ".vmdl";
        models.push_back(name);
        const int fd = ::open((totalDir / (name + "_c")).c_str(), O_WRONLY | O_CREAT, 0600);
        assert(fd >= 0 && ::ftruncate(fd, AppearancePack::MaxModelBytes) == 0);
        const auto header = ResourceHeader(AppearancePack::MaxModelBytes - 28);
        assert(::write(fd, header.data(), header.size()) == static_cast<ssize_t>(header.size()));
        ::close(fd);
    }
    assert(Reject([&] { pack.Apply(Params(totalDir.string(), models)); }).find("256 MiB") != std::string::npos);
    assert(engine.calls.empty());
}

static void TestResourceEnvelope() {
    Temporary files; const auto directory = files.Pack("valid");
    const auto path = fs::path(directory) / "models/test/agent.vmdl_c";
    const auto good = FakeEngine::ReadFile(path);
    FakeEngine engine; AppearancePack pack; pack.Bind(engine.Callbacks());
    std::vector<AppearancePack::Bytes> invalid;
    invalid.push_back(AppearancePack::Bytes(8, 0));
    auto bytes = good; bytes[4] = 11; invalid.push_back(bytes);
    bytes = good; bytes[0] = 0xff; invalid.push_back(bytes); // Declared file is larger than bytes.
    bytes = good; bytes[8] = 0xff; invalid.push_back(bytes); // Table is outside file.
    bytes = good; bytes[12] = 0xff; invalid.push_back(bytes); // Table count overflows envelope.
    bytes = good; bytes[20] = 0; invalid.push_back(bytes); // DATA overlaps the table.
    bytes = good; bytes[24] = 0xff; invalid.push_back(bytes); // DATA exceeds the file.
    bytes = good; bytes[16] = 'X'; invalid.push_back(bytes); // No DATA block.
    for (const auto& content : invalid) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(content.data()), content.size()); file.close();
        assert(Reject([&] { pack.Apply(Params(directory)); }).find("resource envelope") != std::string::npos);
    }
    assert(engine.calls.empty());
}

static void TestWholePackValidationAndResourceRoots() {
    Temporary files;
    const auto valid = files.Pack("valid");
    FakeEngine engine; AppearancePack pack; pack.Bind(engine.Callbacks());
    const auto materials = fs::path(valid) / "materials";
    fs::create_directory(materials);
    const auto external = files.root / "outside";
    fs::create_directory(external);
    { std::ofstream data(external / "outside.vtex_c"); data << "external texture"; }
    fs::create_directory_symlink(external, materials / "escape");
    assert(Reject([&] { pack.Apply(Params(valid)); }).find("symlink") != std::string::npos);
    assert(engine.calls.empty());
    fs::remove(materials / "escape");
    fs::create_symlink(external / "outside.vtex_c", materials / "linked.vtex_c");
    assert(Reject([&] { pack.Apply(Params(valid)); }).find("symlink") != std::string::npos);
    assert(engine.calls.empty());
    fs::remove(materials / "linked.vtex_c");
    for (const auto* extension : {".cfg", ".lua", ".js", ".so", ".vpk", ".json"}) {
        const auto path = fs::path(valid) / (std::string("unrelated") + extension);
        { std::ofstream data(path); data << "untrusted active content"; }
        Reject([&] { pack.Apply(Params(valid)); });
        fs::remove(path);
    }
    const auto pipe = materials / "pipe.vtex_c";
    assert(::mkfifo(pipe.c_str(), 0600) == 0);
    Reject([&] { pack.Apply(Params(valid)); }); fs::remove(pipe);
    const auto oversized = materials / "large.vtex_c";
    { const int fd = ::open(oversized.c_str(), O_WRONLY | O_CREAT, 0600);
      assert(fd >= 0 && ::ftruncate(fd, AppearancePack::MaxTotalBytes) == 0); ::close(fd); }
    assert(Reject([&] { pack.Apply(Params(valid)); }).find("256 MiB") != std::string::npos);
    fs::remove(oversized);
    const auto manifest = fs::path(valid) / "appearance-pack.json";
    { std::ofstream data(manifest); data << "{\"format\":\"other\",\"version\":1}"; }
    Reject([&] { pack.Apply(Params(valid)); });
    { std::ofstream data(manifest); data << "{\"format\":\"cs2-camera-appearance-pack\",\"version\":1}"; }
    pack.Apply(Params(valid)); pack.Reset();
    assert(pack.State()["active"] == false && pack.State()["supported"] == true);

    for (const auto* root : {"characters/", "weapons/", "agents/"}) {
        const std::string model = std::string(root) + "test/example.vmdl";
        const auto directory = files.Pack("root-" + std::string(root).substr(0, std::strlen(root) - 1), model);
        pack.Apply(Params(directory, {model}));
        assert(pack.State()["models"][0] == model);
        assert(engine.reloads.back() == model);
        pack.Reset();
    }

    const auto lots = files.Pack("too-many");
    fs::create_directory(fs::path(lots) / "materials");
    for (std::size_t i = 0; i != AppearancePack::MaxFiles; ++i) {
        std::ofstream file(fs::path(lots) / "materials" / (std::to_string(i) + ".vtex_c"));
        file << "texture";
    }
    engine.calls.clear();
    assert(Reject([&] { pack.Apply(Params(lots)); }).find("4096-file") != std::string::npos);
    assert(engine.calls.empty());
}

static void TestLookupProofAndRollback() {
    Temporary files; const auto first = files.Pack("first");
    const auto second = files.Pack("second", "models/test/agent.vmdl", "different-B");
    FakeEngine engine; AppearancePack pack;
    auto callbacks=engine.Callbacks();
    callbacks.describe=[&](const std::string& resource) {
        assert(!engine.paths.empty()); // Capture diagnostics before rollback.
        return Json{{"resource",resource},{"selected_path","stock/pak01.vpk"},{"search_paths",engine.paths}};
    };
    pack.Bind(std::move(callbacks));
    engine.ignorePriority = true;
    assert(Reject([&] { pack.Apply(Params(first)); }).find("GAME lookup") != std::string::npos);
    assert(engine.paths.empty());
    assert(pack.State()["mounted_verified"] == false);
    assert(pack.State()["render_verified"] == false);
    assert(pack.State()["last_lookup"]["bytes_match"]==false);
    assert(pack.State()["last_lookup"]["filesystem"]["selected_path"]=="stock/pak01.vpk");
    assert(pack.State()["last_lookup"]["filesystem"]["search_paths"]==Json::array({first}));
    assert(engine.reloads.empty() && pack.State()["pending_restore_models"].empty());
    pack.Reset();assert(engine.reloads.empty()); // A failed lookup never invalidates models, including during cleanup.
    engine.ignorePriority = false;
    pack.Apply(Params(first));
    engine.rejectReload = "models/test/agent.vmdl"; engine.rejectReloadCount = 1;
    assert(Reject([&] { pack.Apply(Params(second)); }).find("previous appearance state restored") != std::string::npos);
    assert(engine.paths == std::vector<std::string>{first});
    assert(pack.State()["path"] == first && pack.State()["mounted_verified"] == true);
    assert(pack.State()["reload_requested"] == true && pack.State()["render_verified"] == false);
    engine.rejectMount = second;
    const auto beforeFailedMountReloads=engine.reloads.size();
    Reject([&] { pack.Apply(Params(second)); });
    assert(engine.paths == std::vector<std::string>{first});
    assert(engine.reloads.size()==beforeFailedMountReloads);
    engine.rejectMount.clear(); engine.throwMount = second;
    Reject([&] { pack.Apply(Params(second)); });
    assert(engine.paths == std::vector<std::string>{first});
    engine.throwMount.clear();
    pack.Reset();
}

static void TestReadOnlyFilesystemProbe() {
    Temporary files;const auto directory=files.Pack("probe");
    FakeEngine engine;AppearancePack pack;
    auto callbacks=engine.Callbacks();callbacks.reload={};callbacks.reloadReason="unsafe engine reload";
    pack.Bind(std::move(callbacks));
    assert(pack.Capabilities()["available"]==false && pack.Capabilities()["probe_available"]==true);
    assert(Reject([&]{pack.Apply(Params(directory));}).find("unsafe engine reload")!=std::string::npos);
    assert(engine.calls.empty());
    const auto proof=pack.Probe(Params(directory));
    assert(proof["proof_only"]==true && proof["mounted_verified"]==true);
    assert(proof["reload_requested"]==false && proof["render_verified"]==false);
    assert(engine.paths.empty() && engine.reloads.empty() && pack.State()["active"]==false);
    engine.ignorePriority=true;
    Reject([&]{pack.Probe(Params(directory));});
    assert(engine.paths.empty() && engine.reloads.empty());
    engine.rejectUnmount=directory;
    Reject([&]{pack.Probe(Params(directory));});
    assert(!engine.paths.empty() && pack.State()["status"]=="rollback_incomplete");
    engine.rejectUnmount.clear();pack.Reset();
    assert(engine.paths.empty() && engine.reloads.empty());
}

static void TestDemoRefreshInsteadOfModelReload() {
    Temporary files;const auto directory=files.Pack("refresh");
    FakeEngine engine;AppearancePack pack;unsigned refreshes=0;
    auto callbacks=engine.Callbacks();callbacks.reload={};
    callbacks.refresh=[&]{++refreshes;return true;};pack.Bind(std::move(callbacks));
    assert(pack.Capabilities()["available"]==true && pack.Capabilities()["reload_model"]==false);
    assert(pack.Capabilities()["demo_reload"]==true);
    engine.ignorePriority=true;Reject([&]{pack.Apply(Params(directory));});
    assert(!refreshes && engine.reloads.empty());
    engine.ignorePriority=false;pack.Apply(Params(directory));
    assert(refreshes==1 && engine.reloads.empty() && pack.State()["mounted_verified"]==true);
    pack.Reset();assert(refreshes==2 && engine.reloads.empty() && engine.paths.empty());
}

static void TestIncompleteRollbackAndResetRetries() {
    Temporary files; const auto directory = files.Pack("valid");
    FakeEngine engine; AppearancePack pack; pack.Bind(engine.Callbacks());
    engine.ignorePriority = true; engine.rejectUnmount = directory;
    Reject([&] { pack.Apply(Params(directory)); });
    auto state = pack.State();
    assert(state["status"] == "rollback_incomplete");
    assert(state["mounted_paths"].size() == 1 && state["mounted_verified"] == false);
    const auto calls = engine.calls.size();
    Reject([&] { pack.Apply(Params(directory)); });
    assert(engine.calls.size() == calls);
    const auto reloads = engine.reloads.size();
    Reject([&] { pack.Reset(); });
    assert(engine.reloads.size() == reloads); // Never reload originals through a surviving override mount.
    assert(pack.State()["status"] == "reset_incomplete");
    engine.rejectUnmount.clear(); engine.ignorePriority = false;
    pack.Reset();assert(engine.reloads.empty());
    assert(pack.State()["pending_restore_models"].empty());
    // Once an actual new-resource reload was attempted, failed restoration
    // must remain pending and retry, unlike the untouched lookup failure.
    engine.rejectReload = "models/test/agent.vmdl"; engine.rejectReloadCount = 1;
    engine.rejectReloadCount=2;
    Reject([&] {pack.Apply(Params(directory));});
    engine.rejectReloadCount=1;
    Reject([&] { pack.Reset(); });
    assert(engine.paths.empty());
    assert(pack.State()["pending_restore_models"].size() == 1);
    pack.Reset();
    assert(pack.State()["status"] == "reset_reload_requested");
    assert(pack.State()["pending_restore_models"].empty());
    assert(pack.State()["render_verified"] == false);

    engine.rejectReload.clear();
    pack.Apply(Params(directory));
    engine.throwRead = "models/test/agent.vmdl_c";
    Reject([&] { pack.Apply(Params(directory)); });
    assert(pack.State()["status"] == "rollback_incomplete");
    engine.throwRead.clear(); pack.Reset();
    assert(engine.paths.empty());
}

int main() {
    TestApplyReplaceReset();
    TestAllValidationPrecedesMount();
    TestSymlinksNonRegularAndSize();
    TestResourceEnvelope();
    TestWholePackValidationAndResourceRoots();
    TestLookupProofAndRollback();
    TestReadOnlyFilesystemProbe();
    TestDemoRefreshInsteadOfModelReload();
    TestIncompleteRollbackAndResetRetries();
    std::cout << "Appearance pack core tests passed\n";
}
