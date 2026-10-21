#pragma once

// Runs only on the engine owner thread. A filesystem mount proves file lookup,
// while reload() only acknowledges a request; neither proves a rendered change.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cs2_live {

class AppearancePack {
public:
    using Json = nlohmann::json;
    using Bytes = std::vector<unsigned char>;
    struct Callbacks {
        // mount uses GAME, PATH_ADD_TO_HEAD and the validated priority group.
        // mount(false) means no path was added; unmount(false) means the path
        // may remain mounted. Callbacks may throw after partially completing.
        std::function<bool(const std::string&)> mount;
        std::function<bool(const std::string&)> unmount;
        std::function<Bytes(const std::string&)> readGame;
        // Takes a validated logical model resource path, never console text.
        std::function<bool(const std::string&)> reload;
        // Optional, bounded diagnostic captured while the failing mount is
        // still present. This does not replace exact GAME byte verification.
        std::function<Json(const std::string&)> describe{};
        std::string reloadReason{};
        // Safe adapters can refresh all affected resources by reopening the
        // current demo in the same game process instead of a tool-only command.
        std::function<bool()> refresh{};
    };

    static constexpr std::size_t MaxModels = 32;
    static constexpr std::size_t MaxFiles = 4096;
    static constexpr std::size_t MaxModelBytes = 64ull * 1024 * 1024;
    static constexpr std::size_t MaxTotalBytes = 256ull * 1024 * 1024;

    void Bind(Callbacks callbacks) {
        if (!mounted_.empty() || active_ || !restoreModels_.empty())
            throw std::logic_error("reset the appearance pack before rebinding its backend");
        callbacks_ = std::move(callbacks);
    }

    Json Capabilities() const {
        return {{"available", Ready()}, {"scope", Scope()},
            {"mount", bool(callbacks_.mount)}, {"unmount", bool(callbacks_.unmount)},
            {"read_game", bool(callbacks_.readGame)}, {"reload_model", bool(callbacks_.reload)},
            {"demo_reload",bool(callbacks_.refresh)},
            {"probe_available",bool(callbacks_.mount && callbacks_.unmount && callbacks_.readGame)},
            {"reload_reason",callbacks_.reloadReason},
            {"max_models", MaxModels}, {"max_files", MaxFiles}, {"max_model_bytes", MaxModelBytes},
            {"resource_roots", {"models/", "characters/", "weapons/", "agents/"}},
            {"max_total_bytes", MaxTotalBytes}, {"validation", "regular files, Source2 resource envelope, and GAME lookup bytes"},
            {"compiled_resource_verification", false}, {"render_verification", false}};
    }

    Json State() const {
        Json models = Json::array();
        if (active_) for (const auto& model : active_->files) models.push_back(model.logical);
        return {{"scope", Scope()}, {"status", status_}, {"supported", Ready()}, {"active", bool(active_)},
            {"path", active_ ? active_->path : std::string()}, {"models", models},
            {"mounted_paths", mounted_}, {"pending_restore_models", restoreModels_},
            {"mounted_verified", mountedVerified_}, {"reload_requested", reloadRequested_},
            {"render_verified", false}, {"reset_reload_requested", resetReloadRequested_},
            {"compiled_resource_verified", false}, {"last_error", error_}, {"error", error_},
            {"last_lookup",lastLookup_}};
    }

    void Apply(const Json& params) {
        // Finish all private-file validation before changing the search paths.
        Pack next;
        try {
            if (!Ready()) throw std::runtime_error("appearance filesystem/reload backend is unavailable"+
                (callbacks_.reloadReason.empty()?std::string():": "+callbacks_.reloadReason));
            if (status_ == "rollback_incomplete" || status_ == "reset_incomplete")
                throw std::runtime_error("reset the incomplete appearance operation before applying a pack");
            next = Prepare(params);
        } catch (const std::exception& e) {
            error_ = e.what();
            throw;
        }

        lastLookup_=Json::object();
        auto previous = std::move(active_);
        const auto previousRestore = restoreModels_;
        const bool previousReloadRequested=reloadRequested_,previousResetRequested=resetReloadRequested_;
        std::set<std::string> affected = restoreModels_;
        for (const auto& model : next.files) affected.insert(model.logical);
        resetReloadRequested_ = false;
        bool reloadStarted=false;
        std::string failure;
        try {
            if (previous) Unmount(previous->path);
            active_.reset();
            mountedVerified_ = reloadRequested_ = false;
            Mount(next.path);
            VerifyGameBytes(next);
            active_ = std::move(next);
            mountedVerified_ = true;
            // Until byte verification succeeds the resource system has seen
            // no reload request. Never invalidate live models just to undo a
            // failed search-path experiment.
            restoreModels_ = affected;
            reloadStarted=true;
            RequestReloads(affected);
            reloadRequested_ = true;
            status_ = "reload_requested";
            error_.clear();
            return;
        } catch (const std::exception& e) {
            failure = e.what();
        }

        // Clear every attempted new mount before restoring the prior search
        // path. All affected names must reload, including names removed by an edit.
        const bool rollback = Rollback(previous, affected,reloadStarted,previousReloadRequested,previousResetRequested);
        if (rollback) restoreModels_ = previousRestore;
        error_ = failure + (rollback ? (reloadStarted?"; previous appearance state restored (reload requested)":"; previous appearance state restored (no resource reload)")
                                    : "; appearance rollback incomplete; reset is required");
        throw std::runtime_error(error_);
    }

    Json Probe(const Json& params) {
        if(!callbacks_.mount || !callbacks_.unmount || !callbacks_.readGame)
            throw std::runtime_error("appearance filesystem probe backend is unavailable");
        if(active_ || !mounted_.empty() || !restoreModels_.empty())
            throw std::runtime_error("reset the previous appearance operation before probing a pack");
        const auto pack=Prepare(params);lastLookup_=Json::object();
        try {
            Mount(pack.path);VerifyGameBytes(pack);
            const Json proof={{"proof_only",true},{"mounted_verified",true},{"reload_requested",false},
                {"render_verified",false},{"last_lookup",lastLookup_},{"path",pack.path}};
            Unmount(pack.path);error_.clear();status_="idle";return proof;
        } catch(const std::exception& error) {
            std::string message=error.what();
            try{Unmount(pack.path);}catch(const std::exception& cleanup){message+="; ";message+=cleanup.what();}
            status_=mounted_.empty()?"idle":"rollback_incomplete";error_=message;
            throw std::runtime_error(message);
        }
    }

    void Reset(bool refreshResources=true) {
        if (mounted_.empty() && restoreModels_.empty() && !active_) {
            mountedVerified_ = reloadRequested_ = resetReloadRequested_ = false;
            status_ = "idle";
            error_.clear();
            return;
        }
        if (!callbacks_.unmount || (refreshResources && !restoreModels_.empty() && !callbacks_.reload && !callbacks_.refresh)) {
            error_ = "appearance reset backend is unavailable";
            status_ = "reset_incomplete";
            throw std::runtime_error(error_);
        }
        std::vector<std::string> errors;
        const auto paths = mounted_;
        for (const auto& path : paths) {
            try { Unmount(path); } catch (const std::exception& e) { errors.push_back(e.what()); }
        }
        mountedVerified_ = reloadRequested_ = resetReloadRequested_ = false;
        active_.reset();
        // Never reload an original while any replacement search path remains.
        if (mounted_.empty()) {
            if(!refreshResources) {
                restoreModels_.clear();status_="unmounted";error_.clear();return;
            }
            std::set<std::string> pending;
            if(!restoreModels_.empty() && callbacks_.refresh) {
                try {if(!callbacks_.refresh())throw std::runtime_error("appearance demo reload could not be requested");}
                catch(const std::exception& e){pending=restoreModels_;errors.push_back(e.what());}
            } else for (const auto& model : restoreModels_) {
                try {
                    if (!callbacks_.reload(model)) throw std::runtime_error("reload_model request failed: " + model);
                } catch (const std::exception& e) {
                    pending.insert(model); errors.push_back(e.what());
                }
            }
            restoreModels_ = std::move(pending);
            resetReloadRequested_ = restoreModels_.empty();
        }
        if (!errors.empty()) {
            status_ = "reset_incomplete";
            error_ = Join(errors);
            throw std::runtime_error(error_);
        }
        status_ = "reset_reload_requested";
        error_.clear();
    }

private:
    struct File {
        std::string logical, compiled;
        Bytes bytes;
    };
    struct Pack {
        std::string path;
        std::vector<File> files;
    };
    struct Descriptor {
        int value = -1;
        explicit Descriptor(int fd = -1) : value(fd) {}
        Descriptor(const Descriptor&) = delete;
        Descriptor& operator=(const Descriptor&) = delete;
        Descriptor(Descriptor&& other) noexcept : value(other.value) { other.value = -1; }
        Descriptor& operator=(Descriptor&& other) noexcept {
            if (this != &other) {
                if (value >= 0) ::close(value);
                value = other.value; other.value = -1;
            }
            return *this;
        }
        ~Descriptor() { if (value >= 0) ::close(value); }
    };

    Callbacks callbacks_;
    std::optional<Pack> active_;
    std::vector<std::string> mounted_;
    std::set<std::string> restoreModels_;
    std::string status_ = "idle", error_;
    Json lastLookup_=Json::object();
    bool mountedVerified_ = false, reloadRequested_ = false, resetReloadRequested_ = false;

    static const char* Scope() { return "global resource replacement"; }
    bool Ready() const { return callbacks_.mount && callbacks_.unmount && callbacks_.readGame && (callbacks_.reload || callbacks_.refresh); }

    static std::string Join(const std::vector<std::string>& errors) {
        std::string result;
        for (const auto& error : errors) { if (!result.empty()) result += "; "; result += error; }
        return result;
    }

    static std::vector<std::string> Components(const std::string& path, bool absolute) {
        std::vector<std::string> parts;
        std::size_t begin = absolute ? 1 : 0;
        while (begin < path.size()) {
            const auto slash = path.find('/', begin);
            const auto part = path.substr(begin, slash == std::string::npos ? slash : slash - begin);
            if (part.empty() || part == "." || part == "..")
                throw std::invalid_argument("appearance paths cannot contain empty, dot, or parent components");
            parts.push_back(part);
            if (slash == std::string::npos) break;
            begin = slash + 1;
            if (begin == path.size()) throw std::invalid_argument("appearance paths cannot end with a slash");
        }
        return parts;
    }

    static void ValidateLogical(const std::string& path) {
        bool allowedRoot = false;
        for (const auto* root : {"models/", "characters/", "weapons/", "agents/"})
            if (path.compare(0, std::strlen(root), root) == 0) allowedRoot = true;
        if (!allowedRoot || path.size() < 6 || path.size() > 512 || path.compare(path.size() - 5, 5, ".vmdl") != 0)
            throw std::invalid_argument("appearance model must be a supported character/weapon/model .vmdl resource path");
        for (unsigned char ch : path)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '/' || ch == '_' || ch == '-' || ch == '.'))
                throw std::invalid_argument("appearance model path contains unsupported characters");
        const auto parts = Components(path, false);
        if (parts.back() == ".vmdl") throw std::invalid_argument("appearance model requires a filename");
    }

    static Descriptor OpenDirectory(const std::string& path) {
        if (path.empty() || path[0] != '/' || path.size() > 4096 || path.find('\0') != std::string::npos)
            throw std::invalid_argument("appearance pack path must be an absolute directory");
        for (unsigned char ch : path) if (ch < 32 || ch == 127)
            throw std::invalid_argument("appearance pack path contains control characters");
        auto parts = Components(path, true);
        Descriptor directory(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
        if (directory.value < 0) throw std::runtime_error("cannot open filesystem root for appearance pack");
        for (const auto& part : parts) {
            Descriptor next(::openat(directory.value, part.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            if (next.value < 0) throw std::runtime_error("appearance directory component is missing, inaccessible, or a symlink: " + part);
            directory = std::move(next);
        }
        return directory;
    }

    struct TreeBudget {
        std::size_t files = 0, directories = 0, bytes = 0;
    };

    static bool AllowedCompiledFile(const std::string& path) {
        // Include current Source2 AnimGraph2 dependencies as well as legacy
        // graph/animation resources. No scripts, libraries, cfg files or VPKs.
        static const char* extensions[] = {".vmdl_c", ".vmat_c", ".vtex_c", ".vanim_c", ".vagrp_c", ".vag_c",
            ".vmesh_c", ".vphys_c", ".vanmgrph_c", ".vnmskel_c", ".vnmgraph_c", ".vnmclip_c", ".vnmvar_c"};
        for (const auto* extension : extensions) {
            const auto size = std::strlen(extension);
            if (path.size() > size && path.compare(path.size() - size, size, extension) == 0) return true;
        }
        return false;
    }

    static void ValidateManifest(int file, std::size_t size) {
        if (size == 0 || size > 1024 * 1024)
            throw std::invalid_argument("appearance-pack.json must contain at most 1 MiB of JSON");
        std::string bytes(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            const auto count = ::read(file, bytes.data() + offset, size - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("appearance manifest changed while reading");
            offset += static_cast<std::size_t>(count);
        }
        const auto manifest = Json::parse(bytes);
        if (!manifest.is_object() || manifest.value("format", std::string()) != "cs2-camera-appearance-pack" ||
            !manifest.contains("version") || !manifest["version"].is_number_integer() || manifest["version"] != 1)
            throw std::invalid_argument("appearance-pack.json has an unsupported format or version");
    }

    static void ValidateTree(int directory, const std::string& prefix, unsigned depth, TreeBudget& budget) {
        if (depth > 32 || ++budget.directories > MaxFiles)
            throw std::invalid_argument("appearance pack exceeds its directory depth/count limit");
        struct stat before{};
        if (::fstat(directory, &before) != 0) throw std::runtime_error("cannot inspect appearance directory");
        const int iteratorFd = ::dup(directory);
        if (iteratorFd < 0) throw std::runtime_error("cannot enumerate appearance directory");
        struct Directory {
            DIR* value;
            ~Directory() { if (value) ::closedir(value); }
        } iterator{::fdopendir(iteratorFd)};
        if (!iterator.value) { ::close(iteratorFd); throw std::runtime_error("cannot enumerate appearance directory"); }
        while (true) {
            errno = 0;
            const auto* entry = ::readdir(iterator.value);
            if (!entry) {
                if (errno != 0) throw std::runtime_error("appearance directory enumeration failed");
                break;
            }
            const std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            const auto relative = prefix + name;
            if (relative.size() > 1024) throw std::invalid_argument("appearance pack resource path is too long");
            for (unsigned char ch : name)
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
                    throw std::invalid_argument("appearance pack entry contains unsupported characters: " + relative);
            struct stat named{};
            if (::fstatat(directory, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0)
                throw std::runtime_error("appearance pack entry changed while inspecting: " + relative);
            if (S_ISLNK(named.st_mode)) throw std::invalid_argument("appearance pack cannot contain symlinks: " + relative);
            if (S_ISDIR(named.st_mode)) {
                Descriptor child(::openat(directory, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                struct stat opened{};
                if (child.value < 0 || ::fstat(child.value, &opened) != 0 ||
                    opened.st_ino != named.st_ino || opened.st_dev != named.st_dev)
                    throw std::runtime_error("appearance directory changed or became a symlink: " + relative);
                ValidateTree(child.value, relative + "/", depth + 1, budget);
                continue;
            }
            if (!S_ISREG(named.st_mode))
                throw std::invalid_argument("appearance pack entries must be regular files or directories: " + relative);
            const bool manifest = relative == "appearance-pack.json";
            if (!manifest && !AllowedCompiledFile(relative))
                throw std::invalid_argument("appearance pack contains an unsupported file type: " + relative);
            if (++budget.files > MaxFiles) throw std::invalid_argument("appearance pack exceeds the 4096-file limit");
            Descriptor file(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
            struct stat opened{};
            if (file.value < 0 || ::fstat(file.value, &opened) != 0 || !S_ISREG(opened.st_mode) ||
                opened.st_ino != named.st_ino || opened.st_dev != named.st_dev || opened.st_size < 0)
                throw std::runtime_error("appearance file changed or became a symlink: " + relative);
            const auto size = static_cast<unsigned long long>(opened.st_size);
            if (size > MaxTotalBytes - budget.bytes)
                throw std::invalid_argument("appearance pack files exceed the 256 MiB total limit");
            if (relative.size() >= 7 && relative.compare(relative.size() - 7, 7, ".vmdl_c") == 0 && size > MaxModelBytes)
                throw std::invalid_argument("appearance compiled model exceeds the 64 MiB limit: " + relative);
            budget.bytes += static_cast<std::size_t>(size);
            if (manifest) ValidateManifest(file.value, static_cast<std::size_t>(size));
            struct stat after{};
            if (::fstat(file.value, &after) != 0 || opened.st_size != after.st_size ||
                opened.st_mtim.tv_sec != after.st_mtim.tv_sec || opened.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
                opened.st_ctim.tv_sec != after.st_ctim.tv_sec || opened.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
                throw std::runtime_error("appearance file changed while inspecting: " + relative);
        }
        struct stat after{};
        if (::fstat(directory, &after) != 0 || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
            throw std::runtime_error("appearance directory changed while inspecting");
    }

    static Bytes ReadPrivate(int root, const std::string& compiled, std::size_t& total) {
        const auto parts = Components(compiled, false);
        Descriptor directory(::dup(root));
        if (directory.value < 0) throw std::runtime_error("cannot retain appearance directory");
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            Descriptor next(::openat(directory.value, parts[i].c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            if (next.value < 0) throw std::runtime_error("appearance model directory is missing or a symlink: " + compiled);
            directory = std::move(next);
        }
        Descriptor file(::openat(directory.value, parts.back().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat before{};
        if (file.value < 0 || ::fstat(file.value, &before) != 0 || !S_ISREG(before.st_mode))
            throw std::runtime_error("appearance compiled model must be a regular file without symlinks: " + compiled);
        if (before.st_size <= 0 || static_cast<unsigned long long>(before.st_size) > MaxModelBytes)
            throw std::invalid_argument("appearance compiled model must be non-empty and at most 64 MiB: " + compiled);
        const auto size = static_cast<std::size_t>(before.st_size);
        if (size > MaxTotalBytes - total) throw std::invalid_argument("appearance compiled models exceed the 256 MiB total limit");
        Bytes bytes(size);
        std::size_t offset = 0;
        while (offset < size) {
            const auto count = ::read(file.value, bytes.data() + offset, size - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("appearance model changed or failed while reading: " + compiled);
            offset += static_cast<std::size_t>(count);
        }
        unsigned char extra = 0;
        ssize_t remaining;
        do { remaining = ::read(file.value, &extra, 1); } while (remaining < 0 && errno == EINTR);
        struct stat after{};
        if (remaining != 0 || ::fstat(file.value, &after) != 0 || before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
            throw std::runtime_error("appearance model changed while reading: " + compiled);
        ValidateEnvelope(bytes, compiled);
        total += size;
        return bytes;
    }

    static void ValidateEnvelope(const Bytes& bytes, const std::string& compiled) {
        // Resource.cs in ValveResourceFormat documents the little-endian v12
        // envelope. This validates bounds, not model data, dependencies or rigs.
        const auto invalid = [&]() {
            throw std::invalid_argument("appearance model has an invalid Source2 resource envelope: " + compiled);
        };
        if (bytes.size() < 28) invalid();
        const auto u32 = [&](std::size_t offset) {
            return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
                (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
        };
        const std::uint64_t declared = u32(0), table = std::uint64_t(u32(8)) + 8, count = u32(12);
        if (bytes[4] != 12 || bytes[5] != 0 || declared < 28 || declared > bytes.size() ||
            table < 16 || table > declared || count == 0 || count > (declared - table) / 12) invalid();
        bool data = false;
        const auto tableEnd = table + count * 12;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto entry = static_cast<std::size_t>(table + i * 12);
            const std::uint64_t start = std::uint64_t(entry) + 4 + u32(entry + 4), size = u32(entry + 8);
            if (start > declared || size > declared - start || (size != 0 && start < tableEnd)) invalid();
            if (u32(entry) == 0x41544144 && size != 0) data = true; // DATA
        }
        if (!data) invalid();
    }

    static Pack Prepare(const Json& params) {
        if (!params.is_object() || params.size() != 2 || !params.contains("path") || !params.contains("models") ||
            !params["path"].is_string() || !params["models"].is_array() || params["models"].empty() || params["models"].size() > MaxModels)
            throw std::invalid_argument("appearance pack requires path and 1 to 32 model resource names");
        Pack pack; pack.path = params["path"].get<std::string>();
        std::set<std::string> unique;
        for (const auto& value : params["models"]) {
            if (!value.is_string()) throw std::invalid_argument("appearance model names must be strings");
            auto logical = value.get<std::string>();
            ValidateLogical(logical);
            if (!unique.insert(logical).second) throw std::invalid_argument("appearance model names must be unique");
            pack.files.push_back({logical, logical + "_c", {}});
        }
        auto directory = OpenDirectory(pack.path);
        TreeBudget budget;
        ValidateTree(directory.value, "", 0, budget);
        std::size_t total = 0;
        for (auto& file : pack.files) file.bytes = ReadPrivate(directory.value, file.compiled, total);
        return pack;
    }

    void Mount(const std::string& path) {
        // An exception could occur after the native void AddSearchPath call;
        // retain the path conservatively so Reset can attempt cleanup.
        if (std::find(mounted_.begin(), mounted_.end(), path) == mounted_.end()) mounted_.push_back(path);
        if (!callbacks_.mount(path)) {
            mounted_.erase(std::remove(mounted_.begin(), mounted_.end(), path), mounted_.end());
            throw std::runtime_error("appearance mount failed: " + path);
        }
    }
    void Unmount(const std::string& path) {
        if (std::find(mounted_.begin(), mounted_.end(), path) == mounted_.end()) return;
        if (!callbacks_.unmount(path)) throw std::runtime_error("appearance unmount failed: " + path);
        mounted_.erase(std::remove(mounted_.begin(), mounted_.end(), path), mounted_.end());
    }
    void VerifyGameBytes(const Pack& pack) {
        for (const auto& file : pack.files) {
            const auto loaded = callbacks_.readGame(file.compiled);
            if(lastLookup_.empty() || loaded!=file.bytes) {
                lastLookup_={{"resource",file.compiled},{"expected_bytes",file.bytes.size()},{"actual_bytes",loaded.size()},
                    {"bytes_match",loaded==file.bytes}};
                if(callbacks_.describe) {
                    try{lastLookup_["filesystem"]=callbacks_.describe(file.compiled);}
                    catch(const std::exception& error){lastLookup_["diagnostic_error"]=error.what();}
                }
            }
            if (loaded.size() > MaxModelBytes || loaded != file.bytes)
                throw std::runtime_error("GAME lookup did not select appearance pack bytes: " + file.compiled);
        }
    }
    void RequestReloads(const std::set<std::string>& models) {
        if(!models.empty() && callbacks_.refresh) {
            if(!callbacks_.refresh())throw std::runtime_error("appearance demo reload could not be requested");
            return;
        }
        for (const auto& model : models)
            if (!callbacks_.reload(model)) throw std::runtime_error("reload_model request failed: " + model);
    }
    bool Rollback(std::optional<Pack>& previous, const std::set<std::string>& affected,bool reloadStarted,
                  bool previousReloadRequested,bool previousResetRequested) {
        bool okay = true;
        const auto paths = mounted_;
        for (const auto& path : paths) {
            try { Unmount(path); } catch (...) { okay = false; }
        }
        active_.reset();
        mountedVerified_ = reloadRequested_ = false;
        if (!mounted_.empty()) okay = false;
        if (previous && mounted_.empty()) {
            try {
                Mount(previous->path);
                VerifyGameBytes(*previous);
                active_ = std::move(*previous);
                mountedVerified_ = true;
            } catch (...) { okay = false; }
        }
        // If another mount could still win, a reload cannot restore the old view.
        const bool correctMount = previous ? mountedVerified_ && mounted_.size() == 1 : mounted_.empty();
        if (correctMount && reloadStarted) {
            bool allRequested = true;
            if(!affected.empty() && callbacks_.refresh) {
                try{allRequested=callbacks_.refresh();}catch(...){allRequested=false;}
            } else for (const auto& model : affected) {
                try { if (!callbacks_.reload(model)) allRequested = false; }
                catch (...) { allRequested = false; }
            }
            if (!allRequested) okay = false;
            reloadRequested_ = previous && allRequested;
            resetReloadRequested_ = !previous && allRequested;
        } else if(correctMount) {
            reloadRequested_=previousReloadRequested;resetReloadRequested_=previousResetRequested;
        } else okay = false;
        status_ = okay ? (previous ? "reload_requested" : (reloadStarted?"reset_reload_requested":"idle")) : "rollback_incomplete";
        return okay;
    }
};

} // namespace cs2_live
