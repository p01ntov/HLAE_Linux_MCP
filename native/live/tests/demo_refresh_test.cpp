#include "../demo_refresh.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
#include <unistd.h>

using Refresh = cs2_live::DemoRefresh;
using Result = Refresh::Result;
using namespace std::chrono_literals;

struct Fixture {
    std::string directory, path;
    std::vector<std::string> commands;
    Refresh refresh;
    Refresh::TimePoint now{};
    Fixture() {
        char pattern[] = "/tmp/cs2-demo-refresh-test-XXXXXX";
        const auto* created = ::mkdtemp(pattern); assert(created);
        directory = created; path = directory + "/private demo.dem";
        std::ofstream(path) << "fake demo bytes";
    }
    ~Fixture() { ::unlink(path.c_str()); ::rmdir(directory.c_str()); }
    Refresh::Execute Execute() { return [this](const std::string& command) { commands.push_back(command); }; }
    Result Start(int tick = 100, bool paused = true) { return refresh.Start(path, tick, paused, Execute(), now); }
    Result Advance(bool demo, int tick, bool ready) { return refresh.Advance(demo, tick, ready, Execute(), now); }
};

template<class Function> static void Reject(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}

static void TestObservedRefreshAndPauseRestore(bool paused) {
    Fixture fixture;
    assert(!fixture.refresh.Active() && fixture.refresh.State()["phase"] == "idle");
    assert(fixture.Advance(false, 0, false) == Result::Complete && fixture.commands.empty());
    assert(fixture.Start(100, paused) == Result::Pending);
    assert(fixture.commands == std::vector<std::string>({"demo_pause", "disconnect"}));
    assert(fixture.refresh.State()["target_tick"] == 100 && fixture.refresh.State()["saved_paused"] == paused);
    fixture.now += 1ms;
    assert(fixture.Advance(true, 100, true) == Result::Pending && fixture.commands.size() == 2);
    assert(fixture.Advance(false, 100, true) == Result::Pending);
    assert(fixture.commands.back() == "playdemo \"" + fixture.path + "\"");
    assert(fixture.refresh.State()["phase"] == "waiting_load");
    assert(fixture.Advance(false, 0, false) == Result::Pending);
    assert(fixture.Advance(true, 0, true) == Result::Pending);
    assert(fixture.Advance(true, 2, false) == Result::Pending && fixture.commands.size() == 3);
    fixture.now += 1ms;
    assert(fixture.Advance(true, 2, true) == Result::Pending);
    assert(fixture.commands[3] == "demo_pause" && fixture.commands[4] == std::string("demo_gototick 100")+(paused?" pause":""));
    assert(fixture.refresh.State()["phase"] == "waiting_seek");
    assert(fixture.Advance(false, 100, true) == Result::Pending);
    assert(fixture.Advance(true, 100, false) == Result::Pending);
    assert(fixture.Advance(true, 98, true) == Result::Pending && fixture.commands.size() == 5);
    assert(fixture.Advance(true, 101, true) == Result::Complete);
    assert(fixture.commands.back() == (paused ? "demo_pause" : "demo_resume"));
    assert(!fixture.refresh.Active() && fixture.refresh.State()["phase"] == "complete");
    assert(fixture.Advance(true, 101, true) == Result::Complete && fixture.commands.size() == 6);
    assert(fixture.Start(20) == Result::Pending && fixture.commands.size() == 8);
}

static void TestValidationAndConcurrentStart() {
    Fixture fixture;
    for (const auto& path : std::vector<std::string>{"", "relative.dem", fixture.path + ";quit", fixture.path + "\nquit",
            fixture.path + "\r", fixture.path + "\"", fixture.path + "\\", fixture.path + "\t",
            fixture.directory + "/./private demo.dem", fixture.directory + "/../private demo.dem",
            fixture.directory + "//private demo.dem", fixture.directory, fixture.directory + "/", "/",
            fixture.directory + "/missing.dem", std::string(4097, 'x')})
        Reject([&] { fixture.refresh.Start(path, 0, true, fixture.Execute(), fixture.now); });
    Reject([&] { fixture.refresh.Start(fixture.path + std::string(1, '\0'), 0, true, fixture.Execute(), fixture.now); });
    Reject([&] { fixture.Start(-1); });
    Reject([&] { fixture.refresh.Start(fixture.path, 0, true, {}, fixture.now); });
    const auto symlink = fixture.directory + "/link.dem";
    assert(::symlink(fixture.path.c_str(), symlink.c_str()) == 0);
    assert(::chmod(fixture.directory.c_str(), 0755) == 0);
    Reject([&] { fixture.refresh.Start(symlink, 0, true, fixture.Execute(), fixture.now); });
    assert(::chmod(fixture.directory.c_str(), 0700) == 0);
    ::unlink(symlink.c_str());
    const auto alias = fixture.directory + "/alias";
    assert(::symlink(fixture.directory.c_str(), alias.c_str()) == 0);
    Reject([&] { fixture.refresh.Start(alias + "/private demo.dem", 0, true, fixture.Execute(), fixture.now); });
    ::unlink(alias.c_str());
    const auto empty = fixture.directory + "/empty.dem";
    std::ofstream(empty).close();
    Reject([&] { fixture.refresh.Start(empty, 0, true, fixture.Execute(), fixture.now); });
    ::unlink(empty.c_str());
    assert(fixture.commands.empty() && fixture.refresh.State()["phase"] == "idle");
    fixture.Start(); const auto state = fixture.refresh.State();
    Reject([&] { fixture.Start(); });
    assert(state == fixture.refresh.State() && fixture.commands.size() == 2);
}

static void TestPrivateDemoAlias() {
    Fixture fixture;
    const auto original = fixture.directory + "/original; quoted \" demo.dem";
    const auto alias = fixture.directory + "/input.dem";
    std::ofstream(original) << "fake demo bytes";
    assert(::symlink(original.c_str(), alias.c_str()) == 0);
    Refresh::CheckPath(alias);
    assert(fixture.commands.empty());
    assert(fixture.refresh.Start(alias, 12, true, fixture.Execute(), fixture.now) == Result::Pending);
    fixture.Advance(false, 0, false);
    assert(fixture.commands.back() == "playdemo \"" + alias + "\"");
    assert(fixture.refresh.State()["path"] == alias);
    for (const auto mode : {0755, 0777, 0711, 0770, 01700}) {
        assert(::chmod(fixture.directory.c_str(), mode) == 0);
        Reject([&] { Refresh::CheckPath(alias); });
    }
    assert(::chmod(fixture.directory.c_str(), 0700) == 0);
    ::unlink(original.c_str());
    Reject([&] { Refresh::CheckPath(alias); });
    ::unlink(alias.c_str());
    assert(::symlink(fixture.directory.c_str(), alias.c_str()) == 0);
    Reject([&] { Refresh::CheckPath(alias); });
    ::unlink(alias.c_str());
    const auto empty = fixture.directory + "/empty.dem";
    std::ofstream(empty).close();
    assert(::symlink(empty.c_str(), alias.c_str()) == 0);
    Reject([&] { Refresh::CheckPath(alias); });
    ::unlink(alias.c_str()); ::unlink(empty.c_str());
}

static void TestTimeouts() {
    {
        Fixture fixture; fixture.Start(); fixture.now += 5s;
        assert(fixture.Advance(true, 100, true) == Result::Failed);
        assert(fixture.refresh.Error().find("unload") != std::string::npos && fixture.commands.size() == 2);
        assert(fixture.Advance(false, 0, false) == Result::Failed && fixture.commands.size() == 2);
    }
    {
        Fixture fixture; fixture.Start(); fixture.now += 4s; fixture.Advance(false, 0, false);
        fixture.now += 59s;
        assert(fixture.Advance(false, 0, false) == Result::Pending);
        fixture.now += 1s;
        assert(fixture.Advance(true, 2, true) == Result::Failed);
        assert(fixture.refresh.Error().find("load") != std::string::npos && fixture.commands.size() == 3);
    }
    {
        Fixture fixture; fixture.Start(); fixture.Advance(false, 0, false); fixture.now += 59s;
        fixture.Advance(true, 2, true); fixture.now += 19s;
        assert(fixture.Advance(true, 5, true) == Result::Pending); fixture.now += 1s;
        assert(fixture.Advance(true, 100, true) == Result::Failed);
        assert(fixture.refresh.Error().find("tick") != std::string::npos && fixture.commands.size() == 5);
    }
}

static void TestCallbackFailuresAndClock() {
    for (int failAt = 0; failAt < 6; ++failAt) {
        Fixture fixture; int count = 0;
        Refresh::Execute execute = [&](const std::string& command) {
            fixture.commands.push_back(command);
            if (count++ == failAt) throw std::runtime_error("synthetic engine failure");
        };
        auto result = fixture.refresh.Start(fixture.path, 100, false, execute, fixture.now);
        if (result != Result::Failed) result = fixture.refresh.Advance(false, 0, false, execute, fixture.now);
        if (result != Result::Failed) result = fixture.refresh.Advance(true, 2, true, execute, fixture.now);
        if (result != Result::Failed) result = fixture.refresh.Advance(true, 100, true, execute, fixture.now);
        assert(result == Result::Failed && !fixture.refresh.Active());
        assert(fixture.refresh.Error().find("synthetic engine failure") != std::string::npos);
        assert(fixture.commands.size() == std::size_t(failAt + 1));
    }
    {
        Fixture fixture; fixture.Start();
        assert(fixture.refresh.Advance(false, 0, false, {}, fixture.now) == Result::Failed);
        assert(fixture.commands.size() == 2);
    }
    {
        Fixture fixture; fixture.Start(); fixture.now -= 1ms;
        assert(fixture.Advance(false, 0, false) == Result::Failed && fixture.commands.size() == 2);
    }
    {
        Fixture fixture; Refresh::Execute execute = [](const std::string&) { throw 42; };
        assert(fixture.refresh.Start(fixture.path, 0, true, execute, fixture.now) == Result::Failed);
        assert(fixture.refresh.Error().find("unknown") != std::string::npos);
    }
}

static void TestBoundaryTicks() {
    for (const int target : {0, std::numeric_limits<int>::max()}) {
        Fixture fixture; fixture.Start(target); fixture.Advance(false, 0, false); fixture.Advance(true, 2, true);
        assert(fixture.commands.back() == "demo_gototick " + std::to_string(target)+" pause");
        assert(fixture.Advance(true, -1, true) == Result::Pending);
        assert(fixture.Advance(true, target, true) == Result::Complete);
    }
}

int main() {
    TestObservedRefreshAndPauseRestore(true); TestObservedRefreshAndPauseRestore(false);
    TestValidationAndConcurrentStart(); TestPrivateDemoAlias(); TestTimeouts(); TestCallbackFailuresAndClock(); TestBoundaryTicks();
    std::cout << "demo_refresh_test: PASS\n";
}
