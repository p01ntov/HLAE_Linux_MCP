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
          for (auto field = args.begin(); field != args.end(); ++field) {
            if (field.key() == "render_range") {
              if (!state["project"].contains("render_range"))
                state["project"]["render_range"] = {{"mode", "composition"}, {"in", 0},
                                                      {"out", state["project"]["duration"]}};
              state["project"]["render_range"].update(field.value());
            } else state["project"][field.key()] = field.value();
          }
        } else if (method == "set_camera") {
          state["camera"].update(args);
        } else if (method == "set_scene") {
          for (auto setting = args["settings"].begin(); setting != args["settings"].end(); ++setting)
            state["scene_capabilities"][setting.key()]["value"] = setting.value();
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
    // BeginCombo in pinned ImGui registers ItemAdd without ItemInfo; locate
    // its ordinary, unscoped ID using the same captured item registry.
    if (index == 0) {
      if (auto* target = ImGui::FindWindowByName(window)) {
        const auto found = submitted.items.find(target->GetID(label));
        if (found != submitted.items.end() && found->second.window == window)
          return found->second;
      }
    }
    std::string present;
    for (const auto& entry : submitted.items)
      if (entry.second.window == window && !entry.second.label.empty()) present += entry.second.label + ", ";
    throw std::runtime_error(std::string("Control not found: ") + window + "/" + label + "; present: " + present);
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
  f.Key(ImGuiKey_E);
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
  f.Key(ImGuiKey_E);
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
  f.state["project"]["keys"] = Json::array();
  f.Step();
  f.Type("##fov", "88");
  Near(f.One("set_camera", "fov").get<double>(), 88, "Live camera replacement");
  Require(f.commands[0].arguments.size() == 1,
          "A lens change must preserve the latest roaming position and angles");
  f.BeginScrub("##fov");
  f.MoveScrub(1);
  f.EndScrub();
  Near(f.One("set_camera", "fov").get<double>(), 89, "Live camera middle step");
}

void RenderRegion(float height) {
  Fixture f(height);
  f.state["project"]["render_range"] = {{"mode", "composition"}, {"in", 1.0}, {"out", 6.0}};
  f.Step();
  // The selector stays available even when the inspector's settings are closed.
  f.Click("##render_scope", 0, "##CS2CinematicTransport");
  f.Step();
  const char* popup = nullptr;
  for (const auto& entry : f.submitted.items)
    if (entry.second.label == "Render region") popup = entry.second.window.c_str();
  Require(popup, "Render scope options missing");
  const std::string popup_name = popup;
  f.Click("Render region", 0, popup_name.c_str());
  Require(f.One("set_project", "render_range")["mode"] == "region", "Scope selector did not enable region");
  Near(f.state["project"]["render_range"]["in"].get<double>(), 1, "Scope retained In");
  Near(f.state["project"]["render_range"]["out"].get<double>(), 6, "Scope retained Out");
  f.commands.clear();
  f.Click("Shot settings");
  f.Step();
  f.Type("##range_in", "2");
  Near(f.One("set_project", "render_range")["in"].get<double>(), 2, "Typed In");
  f.Type("##range_out", "5");
  Near(f.One("set_project", "render_range")["out"].get<double>(), 5, "Typed Out");
  f.commands.clear();
  f.state["cursor"] = 3;
  f.Step();
  f.Key(ImGuiKey_I);
  Near(f.One("set_project", "render_range")["in"].get<double>(), 3, "I sets cursor as In");
  f.commands.clear();
  f.state["cursor"] = 4;
  f.Step();
  f.Key(ImGuiKey_O);
  Near(f.One("set_project", "render_range")["out"].get<double>(), 4, "O sets cursor as Out");
  f.commands.clear();
  f.Click("Reset region");
  const auto& reset = f.One("set_project", "render_range");
  Near(reset["in"].get<double>(), 0, "Reset In");
  Near(reset["out"].get<double>(), 8, "Reset Out");

  f.Click("Shot settings");
  f.Step();
  f.commands.clear();
  const auto bounds = f.Find("##camera_timeline", 0, "##CS2CinematicTransport").bounds;
  const float left = bounds.Min.x + 10 * f.scale;
  const float width = bounds.GetWidth() - 20 * f.scale;
  const float y = bounds.Min.y + 44 * f.scale;
  // A click anywhere in the generous bracket hit area must not trim or snap.
  f.ClickAt(ImVec2(left + 6 * f.scale, y));
  Require(f.commands.empty(), "An off-center bracket click moved the range");
  f.Mouse(ImVec2(left, y));
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  f.Step();
  f.Mouse(ImVec2(left + width * .25f, y));
  f.Step();
  Require(f.commands.empty(), "Region drag created history before release");
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  f.Step();
  const auto& dragged = f.One("set_project", "render_range");
  Near(dragged["in"].get<double>(), 2, "Dragged In bracket");
  Require(dragged["mode"] == "region", "Dragging must activate saved region");
  f.commands.clear();
  f.Mouse(ImVec2(left + width, y));
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  f.Step();
  f.Mouse(ImVec2(left + width * .5f, y));
  f.Step();
  f.Key(ImGuiKey_Escape);
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  f.Step();
  Require(f.commands.empty(), "Escape committed a range drag");
  Near(f.state["project"]["render_range"]["out"].get<double>(), 8, "Escape retained Out");
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

void ExactRangeBounds() {
  Fixture f;
  f.state["project"]["keys"] = Json::array();
  f.state["project"]["duration"] = 3.14;
  f.state["project"]["render_range"] = {{"mode", "region"}, {"in", 3.139}, {"out", 3.14}};
  f.Step();
  f.Click("Shot settings");
  f.Step();
  f.Type("##range_out", "99");
  // The current boundary is already at the exact bound; no spurious history.
  Require(f.commands.empty(), "Out clamp exceeded the exact imported duration");
  f.commands.clear();
  f.Click("Reset region");
  const auto& range = f.One("set_project", "render_range");
  Require(range["out"].get<double>() == 3.14, "Reset rounded Out beyond native duration");
  f.commands.clear();
  f.Type("##range_out", "3");
  Require(f.One("set_project", "render_range")["out"].get<double>() == 3,
          "Out did not accept a valid exact value after reset");
}

void SceneControls() {
  Fixture f;
  f.state["scene_capabilities"] = {
      {"depth_of_field", {{"available", true}, {"type", "boolean"}, {"value", true},
                            {"label", "Depth of field renderer"}, {"group", "Lens"}}},
      {"exposure", {{"available", true}, {"type", "number"}, {"value", 1.0},
                       {"label", "Exposure"}, {"group", "Lighting"}, {"minimum", 0}, {"maximum", 16}}},
      {"fog_volume", {{"available", false}, {"type", "boolean"}, {"value", false},
                         {"label", "Volume fog"}, {"group", "Environment"}, {"reason", "Not registered"}}}};
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("General");
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("Scene settings");
  f.Step();
  f.Click("Lens");
  f.Step();
  f.commands.clear();
  f.Click("Depth of field renderer");
  Require(f.One("set_scene", "settings")["depth_of_field"] == false, "DOF checkbox did not send typed change");
  f.Click("Lighting");
  f.Step();
  f.Type("##scene_value", "99");
  Near(f.One("set_scene", "settings")["exposure"].get<double>(), 16, "Runtime effect bound");
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "Volume fog", "Unavailable engine effect leaked into the primary settings");
  f.commands.clear();
  f.Click("Reset scene settings");
  Require(f.commands.size() == 1 && f.commands[0].method == "reset_scene", "Scene reset command missing");
}

void ExportAndHide() {
  Fixture f;
  f.Click("Export", 0, "##CS2CinematicTransport");
  Require(f.commands.size() == 1 && f.commands[0].method == "request_export", "Export button did not request host window");
  f.commands.clear();
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
  f.Key(ImGuiKey_E);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
  f.Step();
  Require(f.commands.size() == 1 && f.commands[0].method == "request_export", "Ctrl+E did not request export window");
  f.commands.clear();
  f.Click("Hide", 0, "##CS2CinematicTransport");
  Require(!f.editor.visible, "Hide did not persist editor visibility request");
  f.Step();
  Require(ImGui::GetDrawData()->CmdListsCount == 0, "A hidden editor still emitted overlay pixels");
}

void PlayerCosmetics() {
  Fixture f;
  f.editor.cosmetics_catalog = {{"agents", Json::array({
      {{"item_def", 1001}, {"display_name", "CT example"}, {"allowed_teams", {3}}, {"model", "agents/ct.vmdl"}},
      {{"item_def", 1002}, {"display_name", "T example"}, {"allowed_teams", {2}}, {"model", "agents/t.vmdl"}},
      {{"item_def", 5037}, {"display_name", "Map based CT"}, {"allowed_teams", {3}}, {"map_based", true}}})}};
  f.state["cosmetics"] = {
      {"players", Json::array({{{"steam_id", "76561198000000001"}, {"nickname", "cat :3"}, {"team", 3}, {"model_ready", true}}})},
      {"stale", false}, {"capabilities", {{"agents", {{"available", true}}}}}};
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("General");
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("Models and skins");
  for (int i = 0; i < 3; ++i) f.Step();
  auto popup = [&](const char* wanted) {
    for (const auto& entry : f.submitted.items)
      if (entry.second.label == wanted) return entry.second.window;
    throw std::runtime_error(std::string("Missing popup choice: ") + wanted);
  };
  f.Click("##cosmetic_agent");
  f.Step();
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "T example" && entry.second.label != "Map based CT",
            "Agent list exposed wrong-team or hardcoded map default model");
  const std::string choices = popup("CT example");
  f.Click("CT example", 0, choices.c_str());
  f.commands.clear();
  f.Click("Apply to player");
  Require(f.One("set_player_cosmetics", "steam_id") == "76561198000000001", "Agent addressed by nickname instead of stable SteamID");
  Require(f.commands[0].arguments["agent_item_def"] == 1001, "Selected agent definition lost");
  f.commands.clear();
  f.Click("Map default");
  Require(f.One("reset_player_cosmetics", "steam_id") == "76561198000000001", "Map default must restore original pawn, not choose generic agent");
  f.commands.clear();
  f.state["cosmetics"]["stale"] = true;
  f.Step();
  f.Click("Apply to player");
  Require(f.commands.empty(), "Stale player snapshot allowed apply");
  f.state["cosmetics"]["stale"] = false;
  f.state["cosmetics"]["players"][0]["team"] = 2;
  f.Step();
  f.Click("##cosmetic_agent");
  f.Step();
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "CT example", "Team switch retained opposite-team agent option");
  const std::string t_choices = popup("T example");
  f.Click("T example", 0, t_choices.c_str());
  f.state["cosmetics"]["players"] = Json::array();
  f.Step();
  f.commands.clear();
  f.Click("Apply to player");
  Require(f.commands.empty(), "Missing selected player allowed apply to another target");
  f.Click("Import...");
  Require(f.commands.empty(), "Unavailable per-player custom import emitted an engine command");
}

void PresentationSelectors() {
  Fixture f;
  f.state["presentation"] = {{"view", "player"}, {"hud", "gameplay"},
      {"requested_view", "player"}, {"requested_hud", "gameplay"},
      {"capabilities", Json::object()}};
  for (const char* id : {"player", "free", "gameplay", "demo", "hidden"})
    f.state["presentation"]["capabilities"][id] = {{"requestable", true}, {"available", false}};
  f.Step();
  f.Click("General");
  for (int i = 0; i < 3; ++i) f.Step();
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "Live camera", "Camera key fields leaked into General");
  auto popup = [&](const char* label) {
    for (const auto& entry : f.submitted.items)
      if (entry.second.label == label) return entry.second.window;
    throw std::runtime_error(std::string("Missing presentation option: ") + label);
  };
  f.Click("##presentation_view");
  f.Step();
  const std::string views = popup("Free camera");
  f.Click("Free camera", 0, views.c_str());
  Require(f.One("set_presentation", "view") == "free", "Free-camera selector did not request the real engine view");
  Require(f.commands[0].arguments["hud"] == "demo", "Free camera retained incompatible gameplay HUD");
  f.state["presentation"]["view"] = "free";
  f.state["presentation"]["hud"] = "demo";
  f.state["presentation"]["requested_view"] = "free";
  f.state["presentation"]["requested_hud"] = "demo";
  f.Step();
  f.commands.clear();
  f.Click("##presentation_hud");
  f.Step();
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "As playing", "Free camera exposed gameplay-only HUD option");
  const std::string hud = popup("Hidden");
  f.Click("Hidden", 0, hud.c_str());
  Require(f.One("set_presentation", "hud") == "hidden", "Hidden HUD selection missing");
  f.Click("Camera keys");
  for (int i = 0; i < 3; ++i) f.Step();
  f.SelectKey();
  f.Type("##fov", "70");
  Require(f.One("update_key", "fov") == 70, "FOV editing must update the selected animation key");
}

void WeaponPaintControls() {
  Fixture f;
  f.editor.cosmetics_catalog = {
      {"agents", Json::array()},
      {"weapons", Json::array({{{"item_def", 7}, {"kind", "weapon"}, {"display_name", "AK-47"}, {"paint_kits", {282}}},
                               {{"item_def", 9}, {"kind", "weapon"}, {"display_name", "AWP"}, {"paint_kits", {344}}}})},
      {"paint_kits", Json::array({{{"id", 282}, {"display_name", "Redline"}, {"wear_min", .1}, {"wear_max", .7}},
                                  {{"id", 344}, {"display_name", "Dragon Lore"}, {"wear_min", 0}, {"wear_max", .7}}})}};
  f.state["cosmetics"] = {{"players", Json::array({{{"steam_id", "76561198000000001"}, {"nickname", "Target"}, {"team", 2}}})},
                            {"capabilities", Json::object()}, {"stale", false}};
  f.state["weapon_cosmetics"] = {{"capability", {{"available", true}}}, {"stale", false},
      {"weapons", Json::array({{{"steam_id", "76561198000000001"}, {"item_def", 7}, {"ready", true}, {"active", true}},
                               {{"steam_id", "76561198000000002"}, {"item_def", 9}, {"ready", true}}})}};
  f.Click("General");
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("Models and skins");
  for (int i = 0; i < 3; ++i) f.Step();
  auto popup = [&](const char* label) {
    for (const auto& entry : f.submitted.items)
      if (entry.second.label == label) return entry.second.window;
    throw std::runtime_error(std::string("Missing weapon option: ") + label);
  };
  f.Click("##cosmetic_category");f.Step();
  const auto categories = popup("Weapons");
  f.Click("Weapons", 0, categories.c_str());
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("##cosmetic_weapon");f.Step();
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "AWP", "Other player's weapon leaked into owned-item choices");
  const auto weapons = popup("AK-47");f.Click("AK-47", 0, weapons.c_str());
  f.Click("##cosmetic_finish");f.Step();
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "Dragon Lore", "Finish for another item leaked into compatible choices");
  const auto finishes = popup("Redline");f.Click("Redline", 0, finishes.c_str());
  f.Type("##cosmetic_wear", "0.01");
  f.Type("##cosmetic_seed", "1200");
  f.commands.clear();f.Click("Apply finish");
  Require(f.One("set_player_weapon_paint", "steam_id") == "76561198000000001", "Weapon finish lost selected player identity");
  Require(f.commands[0].arguments["item_def"] == 7 && f.commands[0].arguments["paint_kit"] == 282,
          "Finish application lost weapon/paint definition");
  Near(f.commands[0].arguments["wear"].get<double>(), .1, "Per-finish wear minimum");
  Require(f.commands[0].arguments["seed"] == 1000, "Pattern seed escaped supported bound");
  f.commands.clear();f.Click("Restore finish");
  Require(f.One("reset_player_weapon_paint", "item_def") == 7, "Finish restore must target this owned item");
}
void GloveControls() {
  Fixture f;
  f.editor.cosmetics_catalog = {
      {"gloves", Json::array({{{"item_def", 5030}, {"display_name", "Sport Gloves"}, {"allowed_teams", {2, 3}}, {"paint_kits", {10018}}},
                              {{"item_def", 5028}, {"display_name", "Default T gloves"}, {"allowed_teams", {2}}, {"paint_kits", Json::array()}},
                              {{"item_def", 5035}, {"display_name", "CT only test"}, {"allowed_teams", {3}}, {"paint_kits", {10026}}}})},
      {"paint_kits", Json::array({{{"id", 10018}, {"category", "glove"}, {"display_name", "Hedge Maze"}, {"wear_min", .06}, {"wear_max", .8}},
                                  {{"id", 10026}, {"category", "glove"}, {"display_name", "Other glove finish"}},
                                  {{"id", 282}, {"category", "weapon"}, {"display_name", "Redline"}}})}};
  f.state["cosmetics"] = {{"players", Json::array({{{"steam_id", "76561198000000001"}, {"nickname", "Target"}, {"team", 2}}})},
                          {"capabilities", Json::object()}, {"stale", false}};
  f.state["glove_cosmetics"] = {{"capability", {{"available", true}}}, {"stale", false},
      {"gloves", Json::array({{{"steam_id", "76561198000000001"}, {"item_def", 5028}, {"ready", true}, {"item_readable", true}},
                              {{"steam_id", "76561198000000002"}, {"item_def", 5035}, {"requested_item_def", 5035}, {"paint_kit", 10026}, {"ready", true}, {"item_readable", true}}})}};
  f.Click("General");
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("Models and skins");
  for (int i = 0; i < 3; ++i) f.Step();
  auto popup = [&](const char* label) {
    for (const auto& entry : f.submitted.items)
      if (entry.second.label == label) return entry.second.window;
    throw std::runtime_error(std::string("Missing glove option: ") + label);
  };
  f.Click("##cosmetic_category");f.Step();
  const auto categories = popup("Gloves");f.Click("Gloves", 0, categories.c_str());
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("##glove_item");f.Step();
  for (const auto& entry : f.submitted.items) {
    Require(entry.second.label != "CT only test", "Wrong-team glove leaked into current player's choices");
    Require(entry.second.label != "Default T gloves", "Unpaintable default must restore actual original instead of substituting an item");
  }
  const auto items = popup("Sport Gloves");f.Click("Sport Gloves", 0, items.c_str());
  f.Step();
  f.Click("##glove_finish");f.Step();
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "Other glove finish" && entry.second.label != "Redline", "Incompatible finish leaked into glove selector");
  const auto finishes = popup("Hedge Maze");f.Click("Hedge Maze", 0, finishes.c_str());
  f.Type("##glove_wear", "0.001");f.Type("##glove_seed", "1200");
  const Json glove_rows = f.state["glove_cosmetics"]["gloves"];
  f.state["glove_cosmetics"]["gloves"] = Json::array();f.Step();
  f.commands.clear();f.Click("Apply gloves");
  Require(f.commands.empty(), "Missing glove row permitted application");
  f.state["glove_cosmetics"]["gloves"] = glove_rows;
  for (int i = 0; i < 3; ++i) f.Step();
  f.commands.clear();f.Click("Apply gloves");
  Require(f.One("set_player_gloves", "steam_id") == "76561198000000001", "Glove selection lost selected player identity");
  Require(f.commands[0].arguments["item_def"] == 5030 && f.commands[0].arguments["paint_kit"] == 10018,
          "Glove selection lost compatible item/paint pair");
  Near(f.commands[0].arguments["wear"].get<double>(), .06, "Glove wear minimum");
  Require(f.commands[0].arguments["seed"] == 1000, "Glove seed escaped supported bound");
  f.Click("##glove_wear");
  ImGui::GetIO().AddInputCharactersUTF8("0.123");f.Step();
  auto& echoed = f.state["glove_cosmetics"]["gloves"][0];
  echoed.update({{"requested_item_def", 5030}, {"paint_kit", 10018}, {"wear", .6}, {"seed", 7}});
  f.Step();f.Key(ImGuiKey_Enter);f.Step();
  f.commands.clear();f.Click("Apply gloves");
  Near(f.One("set_player_gloves", "wear").get<double>(), .123, "Asynchronous echo overwrote an active glove edit");
  Require(f.commands[0].arguments["seed"] == 1000, "Asynchronous echo overwrote unsent glove draft");
  f.state["glove_cosmetics"]["stale"] = true;f.Step();
  f.commands.clear();f.Click("Apply gloves");
  Require(f.commands.empty(), "Stale glove state permitted a new mutation");
  f.Click("Restore gloves");
  Require(f.One("reset_player_gloves", "steam_id") == "76561198000000001", "Glove restore lost original target");
  f.state["glove_cosmetics"]["stale"] = false;
  f.state["glove_cosmetics"]["gloves"][0]["restoration_unresolved"] = true;f.Step();
  f.commands.clear();f.Click("Apply gloves");
  Require(f.commands.empty(), "Unresolved glove baseline permitted another mutation");
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
    std::cout << "Render region at 1080p / 720p\n" << std::flush;
    RenderRegion(1080);
    RenderRegion(720);
    std::cout << "Exact/fine render region bounds\n" << std::flush;
    ExactRangeBounds();
    std::cout << "Scene effect capabilities\n" << std::flush;
    SceneControls();
    std::cout << "Export / hide controls\n" << std::flush;
    ExportAndHide();
    std::cout << "Player/team cosmetics gating\n" << std::flush;
    PlayerCosmetics();
    std::cout << "Presentation and separate camera-key controls\n" << std::flush;
    PresentationSelectors();
    std::cout << "Owned weapon and compatible finish controls\n" << std::flush;
    WeaponPaintControls();
    std::cout << "Selected-player gloves and compatible finishes\n" << std::flush;
    GloveControls();
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
