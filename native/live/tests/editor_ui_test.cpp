// Headless interaction regression for the real editor and its JSON callbacks.
// ImGui's item hooks locate controls by label/ID, avoiding screenshot coordinates,
// private EditorUi access, platform input backends, and installed game fonts.
#include "../editor_ui.h"

#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

static_assert(IMGUI_VERSION_NUM == 19191, "Review interaction tests when upgrading pinned ImGui");

namespace {
using Json = nlohmann::json;
constexpr const char* kInspector = "##CS2CinematicInspector";
constexpr const char* kShortcuts = "##CS2CameraShortcuts";

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void Near(double actual, double expected, const char* what) {
  Require(std::abs(actual - expected) < .001,
          std::string(what) + ": expected " + std::to_string(expected) +
              ", got " + std::to_string(actual));
}

struct Item {
  ImRect bounds;
  std::string window, label;
};
struct Capture {
  std::map<ImGuiID, Item> items;
  std::vector<ImGuiID> order;
};
Capture* capture = nullptr;
}  // namespace

// These are the four optional hooks exposed by pinned ImGui. Only item discovery
// is implemented; no external ImGui test engine package is needed.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, ImGuiID id,
                                const ImRect& bounds, const ImGuiLastItemData*) {
  if (!capture || !id || !context->CurrentWindow) return;
  auto inserted = capture->items.emplace(id, Item{});
  if (inserted.second) capture->order.push_back(id);
  inserted.first->second.bounds = bounds;
  inserted.first->second.window = context->CurrentWindow->Name;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label,
                                 ImGuiItemStatusFlags) {
  if (!capture || !label) return;
  auto found = capture->items.find(id);
  if (found != capture->items.end()) found->second.label = label;
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

namespace {
struct Command {
  std::string method;
  Json arguments;
};

class Fixture {
 public:
  cs2_live_ui::EditorUi editor;
  Json state;
  std::vector<Command> commands;
  Capture submitted;
  float scale;

  explicit Fixture(float height = 1080) : scale(height / 1080) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(height * 16 / 9, height);
    io.DeltaTime = 1.0f / 60;
    ImFontConfig font;
    font.SizePixels = 18 * scale;
    io.Fonts->AddFontDefault(&font);
    unsigned char* pixels;
    int width, atlas_height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &atlas_height);
    ImGui::GetCurrentContext()->TestEngineHookItems = true;
    capture = &submitted;
    state = {
        {"connected", true}, {"demo", true}, {"tick", 89413}, {"tick_rate", 64},
        {"paused", true}, {"mode", "hold"}, {"cursor", 1.65},
        {"preview_playing", false}, {"sync_demo", false}, {"revision", 1},
        {"camera", {{"position", {245.6, -1732.5, 68.3}},
                    {"angles", {-4.2, 90.0, 0.0}}, {"fov", 90}}},
        {"project", {{"name", "Interaction test"}, {"start_tick", 89308},
                     {"duration", 8.0}, {"interpolation", "smooth"},
                     {"keys", Json::array()}}}};
    for (int i = 0; i < 4; ++i)
      state["project"]["keys"].push_back(
          {{"id", std::to_string(i)}, {"time", i * 2.5},
           {"position", {245.6 + i * 100, -1732.5, 68.3}},
           {"angles", {-4.2, 90.0, 0.0}}, {"fov", 90}, {"easing", {0, 0, 1, 1}}});
    for (int i = 0; i < 5; ++i) Step();
  }
  ~Fixture() {
    capture = nullptr;
    ImGui::DestroyContext();
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  void Step() {
    // Preserve ImGui's normal input trickling. A synthetic modifier and mouse
    // event may take separate frames, just as they do in the real application.
    int frames = 0;
    do {
      Require(++frames <= 16, "ImGui input queue did not drain");
      submitted = {};
      ImGui::NewFrame();
      editor.Draw(state, [&](const std::string& method, const Json& args) {
        commands.push_back({method, args});
        // Echo successful writes as the native host does. Keeping stale state
        // would conceal draft synchronization regressions between gestures.
        if (method == "update_key") {
          for (auto& key : state["project"]["keys"])
            if (key["id"] == args["id"])
              for (auto field = args.begin(); field != args.end(); ++field)
                key[field.key()] = field.value();
        } else if (method == "set_project") {
          for (auto field = args.begin(); field != args.end(); ++field)
            state["project"][field.key()] = field.value();
        } else if (method == "set_camera") {
          state["camera"] = args;
        }
        state["revision"] = state["revision"].get<int>() + 1;
      });
      ImGui::Render();
    } while (ImGui::GetCurrentContext()->InputEventsQueue.Size != 0);
  }
  Item Find(const char* label, size_t index = 0, const char* window = kInspector) const {
    for (ImGuiID id : submitted.order) {
      const Item& item = submitted.items.at(id);
      if (item.window == window && item.label == label) {
        if (index == 0) return item;
        --index;
      }
    }
    throw std::runtime_error(std::string("Control not found: ") + window + "/" + label);
  }
  void Mouse(ImVec2 position) {
    ImGui::GetIO().AddMousePosEvent(position.x, position.y);
  }
  void ClickAt(ImVec2 position) {
    Mouse(position);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Step();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Step();
  }
  void Click(const char* label, size_t index = 0, const char* window = kInspector) {
    ClickAt(Find(label, index, window).bounds.GetCenter());
  }
  void Key(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    Step();
    ImGui::GetIO().AddKeyEvent(key, false);
    Step();
  }
  void SelectKey() {
    Click("Keyframe");
    Step();
    commands.clear();
  }
  void Type(const char* label, const char* text, bool enter = true, size_t index = 0) {
    commands.clear();
    Click(label, index);
    ImGui::GetIO().AddInputCharactersUTF8(text);
    Step();
    Require(commands.empty(), "Typing submitted a command before commit");
    if (enter) Key(ImGuiKey_Enter);
    else ClickAt(ImVec2(700 * scale, 20 * scale));
    Step();
  }
  void BeginScrub(const char* label, size_t index = 0, ImGuiKey modifier = ImGuiKey_None) {
    commands.clear();
    if (modifier != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(modifier, true);
    Mouse(Find(label, index).bounds.GetCenter());
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
    Step();
    Require(ImGui::IsAnyItemActive(), std::string("Middle scrub must hold an active ID: ") +
        label + "[" + std::to_string(index) + "] at frame " +
        std::to_string(ImGui::GetFrameCount()) + ", mouse " +
        std::to_string(ImGui::GetIO().MousePos.x) + "," +
        std::to_string(ImGui::GetIO().MousePos.y) + ", down=" +
        std::to_string(ImGui::GetIO().MouseDown[2]) + ", clicked=" +
        std::to_string(ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) +
        ", hovered=" + (ImGui::GetCurrentContext()->HoveredWindow ?
        ImGui::GetCurrentContext()->HoveredWindow->Name : "(none)") +
        ", queued=" + std::to_string(ImGui::GetCurrentContext()->InputEventsQueue.Size));
    Require(commands.empty(), "Middle press submitted a command");
  }
  void MoveScrub(float steps, bool outside = false) {
    const ImVec2 start = ImGui::GetIO().MousePos;
    // ImGui floors pointer positions to screen pixels. Round a logical step
    // outward so 4px@1080 remains a complete gesture at 720p (3 screen pixels).
    const float distance = std::ceil(std::abs(steps) * 4 * scale);
    const float delta = steps > 0 ? distance : steps < 0 ? -distance : 0;
    Mouse(ImVec2(start.x + delta, outside ? 5 * scale : start.y));
    Step();
    Require(commands.empty(), "Middle movement submitted a command before release");
  }
  void EndScrub(ImGuiKey modifier = ImGuiKey_None) {
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
    Step();
    if (modifier != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(modifier, false);
    Step();
    Step();
  }
  const Json& One(const char* method, const char* field) const {
    Require(commands.size() == 1, std::string("Expected one history command, got ") +
                std::to_string(commands.size()) + " for " + method + "/" + field +
                " at frame " + std::to_string(ImGui::GetFrameCount()));
    Require(commands[0].method == method, "Unexpected command: " + commands[0].method);
    Require(commands[0].arguments.contains(field), std::string("Missing field: ") + field);
    return commands[0].arguments[field];
  }
};

void NumericGestures(float height) {
  Fixture f(height);
  f.SelectKey();
  f.Type("##fov", "105");
  Near(f.One("update_key", "fov").get<double>(), 105, "Single-click replacement");
  f.BeginScrub("##fov");
  f.MoveScrub(1);

  // Keyboard actions during a scrub must neither capture keys nor save, play,
  // or open the shortcuts panel. Global host handlers use this same active ID.
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
  f.Key(ImGuiKey_S);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
  f.Key(ImGuiKey_K);
  f.Key(ImGuiKey_Space);
  f.Key(ImGuiKey_F10);
  Require(f.commands.empty() && !f.editor.shortcuts_visible, "Shortcut escaped middle-scrub guard");
  f.EndScrub();
  Near(f.One("update_key", "fov").get<double>(), 106, "Middle +1");

  f.BeginScrub("##fov");
  f.MoveScrub(-1, true);
  f.EndScrub();
  Near(f.One("update_key", "fov").get<double>(), 105, "Middle -1 released outside");
  f.BeginScrub("##fov");
  f.EndScrub();
  Require(f.commands.empty(), "Unchanged middle click created history");

  f.BeginScrub("##fov");
  f.MoveScrub(1000, true);
  f.EndScrub();
  Near(f.One("update_key", "fov").get<double>(), 179, "FOV upper clamp");
  f.BeginScrub("##fov");
  f.MoveScrub(1);
  f.EndScrub();
  Require(f.commands.empty(), "Scrub against an unchanged bound created history");
  f.BeginScrub("##fov");
  f.MoveScrub(-1000, true);
  f.EndScrub();
  Near(f.One("update_key", "fov").get<double>(), 1, "FOV lower clamp");
  f.BeginScrub("##fov");
  f.MoveScrub(10);
  f.Key(ImGuiKey_Escape);
  f.EndScrub();
  Require(f.commands.empty(), "Escape submitted a middle scrub");

  f.Type("##fov", "200", false);
  Near(f.One("update_key", "fov").get<double>(), 179, "Text click-away clamp");
  f.commands.clear();
  f.Click("##fov");
  ImGui::GetIO().AddInputCharactersUTF8("100");
  f.Step();
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
  f.Key(ImGuiKey_S);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
  f.Key(ImGuiKey_K);
  f.Key(ImGuiKey_Space);
  f.Key(ImGuiKey_F10);
  Require(f.commands.empty() && !f.editor.shortcuts_visible, "Shortcut escaped text editing guard");
  f.Key(ImGuiKey_Escape);
  Require(f.commands.empty(), "Escape submitted text edit");

  f.BeginScrub("##fov", 0, ImGuiMod_Ctrl);
  f.MoveScrub(-1);
  f.EndScrub(ImGuiMod_Ctrl);
  Near(f.One("update_key", "fov").get<double>(), 178.9, "Ctrl fine step");
  f.BeginScrub("##fov", 0, ImGuiMod_Shift);
  f.MoveScrub(-1);
  f.EndScrub(ImGuiMod_Shift);
  Near(f.One("update_key", "fov").get<double>(), 168.9, "Shift fast step");
}

void PoseAndProjectBounds() {
  Fixture f;
  f.SelectKey();
  for (size_t axis = 0; axis < 3; ++axis) {
    f.Type("##value", "12", true, axis);
    Near(f.One("update_key", "position")[axis].get<double>(), 12, "XYZ text replacement");
    f.BeginScrub("##value", axis);
    f.MoveScrub(1);
    f.EndScrub();
    Near(f.One("update_key", "position")[axis].get<double>(), 13, "XYZ middle step");
  }
  f.Type("##value", "1000001", true, 0);
  Near(f.One("update_key", "position")[0].get<double>(), 1000000, "Position bound");
  for (size_t axis = 0; axis < 3; ++axis) {
    f.Type("##value", "20", true, axis + 3);
    Near(f.One("update_key", "angles")[axis].get<double>(), 20, "Rotation text replacement");
    f.BeginScrub("##value", axis + 3);
    f.MoveScrub(-1);
    f.EndScrub();
    Near(f.One("update_key", "angles")[axis].get<double>(), 19, "Rotation middle step");
  }
  f.Type("##value", "120", true, 3);
  Near(f.One("update_key", "angles")[0].get<double>(), 90, "Pitch upper clamp");
  f.Type("##value", "-120", true, 3);
  Near(f.One("update_key", "angles")[0].get<double>(), -90, "Pitch lower clamp");
  f.Type("##value", "-1000001", true, 4);
  Near(f.One("update_key", "angles")[1].get<double>(), -1000000, "Yaw finite bound");
  f.Type("##key_time", "1.25");
  Near(f.One("update_key", "time").get<double>(), 1.25, "Key time replacement");
  f.BeginScrub("##key_time");
  f.MoveScrub(1);
  f.EndScrub();
  Near(f.One("update_key", "time").get<double>(), 2.25, "Key time middle step");
  f.Type("##key_time", "999");
  Near(f.One("update_key", "time").get<double>(), 8, "Key time upper bound");
  f.Type("##key_time", "-1");
  Near(f.One("update_key", "time").get<double>(), 0, "Key time lower bound");

  f.Click("Shot settings");
  f.Step();
  f.Type("##shot_start", "2147483647");
  Require(f.One("set_project", "start_tick") == 2000000000, "Start tick upper bound");
  f.BeginScrub("##shot_start");
  f.MoveScrub(-1);
  f.EndScrub();
  Require(f.One("set_project", "start_tick") == 1999999999, "Integer step lost precision near 2e9");
  f.BeginScrub("##shot_start");
  f.MoveScrub(1);
  f.EndScrub();
  Require(f.One("set_project", "start_tick") == 2000000000, "Integer +1 lost precision");
  f.BeginScrub("##shot_start");
  f.MoveScrub(1);
  f.EndScrub();
  Require(f.commands.empty(), "Unchanged integer bound created history");
  f.Type("##shot_start", "-5");
  Require(f.One("set_project", "start_tick") == 0, "Start tick lower bound");
  f.BeginScrub("##shot_start", 0, ImGuiMod_Ctrl);
  f.MoveScrub(1);
  f.EndScrub(ImGuiMod_Ctrl);
  Require(f.One("set_project", "start_tick") == 1, "Ctrl must preserve integer ticks");
  f.BeginScrub("##shot_start", 0, ImGuiMod_Shift);
  f.MoveScrub(1);
  f.EndScrub(ImGuiMod_Shift);
  Require(f.One("set_project", "start_tick") == 11, "Shift integer step");
  f.Type("##shot_duration", "100000");
  Near(f.One("set_project", "duration").get<double>(), 86400, "Duration upper clamp");
  f.Type("##shot_duration", "-1");
  Near(f.One("set_project", "duration").get<double>(), 7.5, "Duration cannot exclude last key");
  f.BeginScrub("##shot_duration");
  f.MoveScrub(1);
  f.EndScrub();
  Near(f.One("set_project", "duration").get<double>(), 8.5, "Duration middle step");
}

void LiveCamera() {
  Fixture f;
  f.Type("##fov", "88");
  Near(f.One("set_camera", "fov").get<double>(), 88, "Live camera replacement");
  Require(f.commands[0].arguments.contains("position") &&
              f.commands[0].arguments.contains("angles"),
          "Live camera commit must retain the complete pose");
  f.BeginScrub("##fov");
  f.MoveScrub(1);
  f.EndScrub();
  Near(f.One("set_camera", "fov").get<double>(), 89, "Live camera middle step");
}

void IndependentShortcutsPanel() {
  Fixture f;
  f.Key(ImGuiKey_F10);
  Require(f.editor.shortcuts_visible, "F10 did not open shortcuts");
  auto* window = ImGui::FindWindowByName(kShortcuts);
  Require(window && window->Active, "Shortcuts must have an independent active window");
  Require(!(window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal)),
          "Shortcuts must be nonmodal");
  Require(ImGui::FindWindowByName(kInspector)->Active, "Inspector vanished while shortcuts open");
  const ImVec2 original = window->Pos;
  const ImVec2 heading = f.Find("##move_shortcuts", 0, kShortcuts).bounds.GetCenter();
  f.Mouse(heading);
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  f.Step();
  f.Mouse(ImVec2(heading.x + 80, heading.y + 30));
  f.Step();
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  f.Step();
  Require(window->Pos.x > original.x + 50 && window->Pos.y > original.y + 20,
          "Shortcuts heading did not move the panel");
  f.Click("Close", 0, kShortcuts);
  Require(!f.editor.shortcuts_visible, "Close did not hide shortcuts");
  f.Key(ImGuiKey_F10);
  Require(f.editor.shortcuts_visible, "F10 did not reopen shortcuts");
  f.Key(ImGuiKey_F10);
  Require(!f.editor.shortcuts_visible, "F10 did not close shortcuts");
  f.Click("Hotkeys");
  Require(f.editor.shortcuts_visible, "Inspector Hotkeys did not open separate panel");
  Require(f.commands.empty(), "Opening/moving shortcuts emitted native commands");
}
}  // namespace

int main() {
  try {
    std::cout << "Numeric gestures at 1080p\n" << std::flush;
    NumericGestures(1080);
    std::cout << "Numeric gestures at 720p\n" << std::flush;
    NumericGestures(720);
    std::cout << "Pose and project bounds\n" << std::flush;
    PoseAndProjectBounds();
    std::cout << "Live camera\n" << std::flush;
    LiveCamera();
    std::cout << "Independent shortcuts panel\n" << std::flush;
    IndependentShortcutsPanel();
    std::cout << "PASS: numeric replacement, Enter/click-away, middle steps and bounds, "
                 "single history commits, Escape/no-op, shortcut guards, and independent F10 panel\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
