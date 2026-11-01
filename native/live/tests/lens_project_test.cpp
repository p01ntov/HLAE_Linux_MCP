#include "../live_controller.h"
#include <cassert>
#include <iostream>

using namespace cs2_live;

static Json State(Controller& controller) { return Json::parse(controller.StateJson()); }
static Json Command(Controller& controller, const char* method, const Json& params=Json::object()) {
    const auto request=Json{{"method",method},{"params",params}}.dump();
    controller.EnqueueText(request.c_str());
    controller.Frame(true,640,true,64,true,"",[](const std::string&){});
    return State(controller);
}

static void Capability(Controller& controller, bool available) {
    const auto value=Json{{"available",available},{"backend","vulkan-color"},
        {"depth_available",false},{"reason",available?"":"fixture unavailable"}}.dump();
    controller.LensStatusText(value.c_str());
}

static void InvalidImportsAreAtomic() {
    Controller controller;
    const Json original={{"name","Keep this composition"},{"start_tick",640},{"duration",1},
        {"lens",{{"enabled",true},{"vignette",.4}}}};
    const auto initial=Command(controller,"import_project",{{"project",original}});
    assert(initial.value("error",std::string()).empty());
    const Json malformed=Json::array({nullptr,Json::array(),"",false,0,
        Json{{"enabled",1}},Json{{"vignette",true}},Json{{"vignette",nullptr}},
        Json{{"depth",.5}},Json{{"chromatic",8.001}},Json{{"distortion",-.751}}});
    for(const auto& lens:malformed) {
        auto project=original;project["name"]="Must not replace";project["lens"]=lens;
        bool rejected=false;
        try { ReadProject(project); } catch(const std::exception&) { rejected=true; }
        assert(rejected);
        // The native command must report an error, not throw while publishing
        // an already-mutated invalid document (notably null and empty arrays).
        const auto after=Command(controller,"import_project",{{"project",project}});
        assert(after["error"].is_string() && !after["error"].get<std::string>().empty());
        for(const char* field:{"project","project_document_id","revision","camera","undo_available","redo_available"})
            assert(after[field]==initial[field]);
        assert(after["lens"]==initial["lens"]);
    }
    const auto cleared=Command(controller,"import_project",{{"project",{{"name","Legacy"},{"lens",Json::object()}}}});
    assert(cleared.value("error",std::string()).empty() && !cleared["project"].contains("lens"));
    assert(cleared["lens"]==NormalizeLens(Json::object()));
}

static void SettingsHistoryAndAvailability() {
    Controller controller;
    const auto rejected=Command(controller,"set_lens",{{"settings",{{"enabled",true},{"vignette",.5}}}});
    assert(rejected["revision"]==0 && !rejected["undo_available"].get<bool>());
    assert(!rejected["lens"]["enabled"].get<bool>());
    Capability(controller,true);
    const auto active=Command(controller,"set_lens",{{"settings",{{"enabled",true},{"distortion",-.2},{"vignette",.5}}}});
    assert(active.value("error",std::string()).empty());
    assert(active["lens"]["enabled"]==true && active["lens"]["vignette"]==.5);
    const auto changed=Command(controller,"set_lens",{{"settings",{{"chromatic",3.0}}}});
    assert(changed["lens"]["vignette"]==.5 && changed["lens"]["chromatic"]==3.0);
    const auto undone=Command(controller,"undo");
    assert(undone["lens"]==active["lens"] && undone["redo_available"].get<bool>());
    assert(Command(controller,"redo")["lens"]==changed["lens"]);
    const auto reset=Command(controller,"reset_lens");
    assert(!reset["project"].contains("lens") && !reset["lens"]["enabled"].get<bool>());
    assert(Command(controller,"undo")["lens"]==changed["lens"]);
    Capability(controller,false);
    const auto disabled=Command(controller,"set_lens",{{"settings",{{"enabled",false}}}});
    assert(disabled.value("error",std::string()).empty() && !disabled["lens"]["enabled"].get<bool>());
    assert(disabled["lens"]["chromatic"]==3.0);
    // A saved effect remains portable when the renderer is unavailable. The
    // preview capability is distinct from the persisted export parameters.
    const auto imported=Command(controller,"import_project",{{"project",changed["project"]}});
    assert(imported.value("error",std::string()).empty() && imported["project"]["lens"]==changed["lens"]);
    controller.Frame(false,0,false,64,false,"",[](const std::string&){});
    const auto noDemo=State(controller);
    assert(!noDemo["lens"]["enabled"].get<bool>());
    assert(noDemo["project"]["lens"]["enabled"].get<bool>());
}

int main() {
    setenv("CS2_CAMERA_LIVE","1",1);
    InvalidImportsAreAtomic();
    SettingsHistoryAndAvailability();
    std::cout<<"lens project validation/history tests passed\n";
}
