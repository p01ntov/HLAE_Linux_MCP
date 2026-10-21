#pragma once

// Called on the engine owner thread. All engine commands are supplied by the
// caller; observations, rather than command submission, advance the refresh.
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

namespace cs2_live {

class DemoRefresh {
public:
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Execute = std::function<void(const std::string&)>;
    enum class Result { Pending, Complete, Failed };

    static constexpr auto UnloadTimeout = std::chrono::seconds(5);
    static constexpr auto LoadTimeout = std::chrono::seconds(60);
    static constexpr auto SeekTimeout = std::chrono::seconds(20);

    static void CheckPath(const std::string& path) { ValidatePath(path); }

    bool Active() const { return phase_ == PhaseValue::Unload || phase_ == PhaseValue::Load || phase_ == PhaseValue::Seek; }
    const std::string& Error() const { return error_; }
    const char* Phase() const {
        switch (phase_) {
        case PhaseValue::Idle: return "idle";
        case PhaseValue::Unload: return "waiting_unload";
        case PhaseValue::Load: return "waiting_load";
        case PhaseValue::Seek: return "waiting_seek";
        case PhaseValue::Complete: return "complete";
        case PhaseValue::Failed: return "failed";
        }
        return "failed";
    }
    Json State() const {
        return {{"phase", Phase()}, {"active", Active()}, {"target_tick", targetTick_},
            {"saved_paused", savedPaused_}, {"path", path_}, {"error", error_}};
    }

    Result Start(const std::string& path, int tick, bool paused, const Execute& execute,
                 TimePoint now = Clock::now()) {
        if (Active()) throw std::logic_error("a demo appearance refresh is already active");
        if (tick < 0) throw std::invalid_argument("demo refresh tick must be nonnegative");
        if (!execute) throw std::invalid_argument("demo refresh requires an engine command callback");
        CheckPath(path);
        path_ = path; targetTick_ = tick; savedPaused_ = paused; error_.clear();
        phase_ = PhaseValue::Unload; phaseStarted_ = now; lastNow_ = now;
        return Commands(execute, {"demo_pause", "disconnect"});
    }

    Result Advance(bool demo, int tick, bool viewReady, const Execute& execute,
                   TimePoint now = Clock::now()) {
        if (!Active()) return phase_ == PhaseValue::Failed ? Result::Failed : Result::Complete;
        if (!execute) return Fail("demo refresh lost its engine command callback");
        if (now < lastNow_) return Fail("demo refresh clock moved backwards");
        lastNow_ = now;
        switch (phase_) {
        case PhaseValue::Unload:
            if (now - phaseStarted_ >= UnloadTimeout) return Fail("timed out waiting for the demo to unload");
            if (!demo) {
                phase_ = PhaseValue::Load; phaseStarted_ = now;
                return Commands(execute, {"playdemo \"" + path_ + "\""});
            }
            break;
        case PhaseValue::Load:
            if (now - phaseStarted_ >= LoadTimeout) return Fail("timed out waiting for the demo view to load");
            if (demo && tick > 0 && viewReady) {
                phase_ = PhaseValue::Seek; phaseStarted_ = now;
                return Commands(execute, {"demo_pause", "demo_gototick " + std::to_string(targetTick_)+(savedPaused_?" pause":"")});
            }
            break;
        case PhaseValue::Seek: {
            if (now - phaseStarted_ >= SeekTimeout) return Fail("timed out restoring the demo tick");
            const auto difference = std::int64_t(tick) - std::int64_t(targetTick_);
            if (demo && tick >= 0 && viewReady && difference >= -1 && difference <= 1) {
                const auto result = Commands(execute, {savedPaused_ ? "demo_pause" : "demo_resume"});
                if (result == Result::Failed) return result;
                phase_ = PhaseValue::Complete;
                return Result::Complete;
            }
            break;
        }
        default: break;
        }
        return Result::Pending;
    }

private:
    enum class PhaseValue { Idle, Unload, Load, Seek, Complete, Failed };
    PhaseValue phase_ = PhaseValue::Idle;
    std::string path_, error_;
    int targetTick_ = 0;
    bool savedPaused_ = true;
    TimePoint phaseStarted_{}, lastNow_{};

    static void ValidatePath(const std::string& path) {
        if (path.empty() || path.front() != '/' || path.size() > 4096)
            throw std::invalid_argument("demo refresh requires an absolute demo file path");
        for (const unsigned char character : path)
            if (character < 32 || character == 127 || character == '"' || character == ';' || character == '\\')
                throw std::invalid_argument("demo refresh path contains a console delimiter or control character");
        // The launcher supplies a clean private alias even if the original demo
        // filename cannot safely appear in console text. Only its final link is
        // allowed; all parent components must be real directories.
        struct stat parentStatus{};
        if (::lstat("/", &parentStatus) != 0)
            throw std::invalid_argument("demo refresh cannot validate the root directory");
        std::size_t begin = 1;
        while (begin < path.size()) {
            const auto end = path.find('/', begin);
            const auto component = path.substr(begin, end == std::string::npos ? end : end - begin);
            if (component.empty() || component == "." || component == "..")
                throw std::invalid_argument("demo refresh path must use canonical path components");
            struct stat status{};
            const auto prefix = path.substr(0, end);
            if (::lstat(prefix.c_str(), &status) != 0)
                throw std::invalid_argument("demo refresh path must exist");
            if (end == std::string::npos) {
                if (S_ISLNK(status.st_mode)) {
                    if (parentStatus.st_uid != ::getuid() ||
                        (parentStatus.st_mode & 07777) != 0700 || status.st_uid != ::getuid())
                        throw std::invalid_argument("demo refresh aliases require a private owner-only parent directory");
                    if (::stat(prefix.c_str(), &status) != 0)
                        throw std::invalid_argument("demo refresh alias target must exist");
                }
                if (!S_ISREG(status.st_mode) || status.st_size <= 0)
                    throw std::invalid_argument("demo refresh path must name a nonempty regular file");
                return;
            }
            if (!S_ISDIR(status.st_mode))
                throw std::invalid_argument("demo refresh parent path must be a directory without symlinks");
            parentStatus = status;
            begin = end + 1;
        }
        throw std::invalid_argument("demo refresh path must name a regular file");
    }
    Result Fail(std::string message) {
        phase_ = PhaseValue::Failed; error_ = std::move(message); return Result::Failed;
    }
    Result Commands(const Execute& execute, std::initializer_list<std::string> commands) {
        try {
            for (const auto& command : commands) execute(command);
        } catch (const std::exception& error) {
            return Fail(std::string("demo refresh command failed: ") + error.what());
        } catch (...) {
            return Fail("demo refresh command failed with an unknown exception");
        }
        return Result::Pending;
    }
};

} // namespace cs2_live
