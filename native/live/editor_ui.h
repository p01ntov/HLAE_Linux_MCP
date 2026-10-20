#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Native CS2 cinematic editor. All effects leave the UI through Send; neither
// rendering nor editing opens sockets, touches the game, or writes project files.
namespace cs2_live_ui {

class EditorUi {
 public:
  using Json = nlohmann::json;
  using Send = std::function<void(const std::string&, const Json&)>;

  bool visible = true;
  // Host loads this from the user's CS2 install; Valve font files are not bundled.
  ImFont* mono_font = nullptr;
  // Per-frame host texture of the blurred game scene, sampled in screen space.
  ImTextureID backdrop_texture = 0;
  bool shortcuts_visible = false;

  void Draw(const Json& state, const Send& send) {
    if (!visible || !ImGui::GetCurrentContext()) return;
    const int frame = ImGui::GetFrameCount();
    if (last_draw_frame_ >= 0 && frame != last_draw_frame_ + 1) {
      timeline_active_ = false;
      timeline_drag_id_.clear();
      graph_handle_ = -1;
      graph_changed_ = false;
      was_item_active_ = false;
      draft_target_.clear();
      CancelNumericScrub();
      numeric_edits_.clear();
    }
    last_draw_frame_ = frame;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 320.0f || display.y < 240.0f) return;
    // Panorama's native demo controller is authored against a 1080px viewport.
    scale_ = display.y / 1080.0f;
    ReadState(state);
    const ImGuiStyle saved_style = ImGui::GetStyle();
    ApplyStyle();
    const float margin = S(16);
    const float transport_height = S(96);
    const float inspector_width = std::min(S(430), display.x * .42f);
    const float inspector_height = display.y - transport_height - margin * 3;
    if (show_inspector_)
      DrawInspector(ImVec2(display.x - inspector_width - S(10),
                           display.y - transport_height - S(6)),
                    ImVec2(inspector_width, inspector_height), state, send);
    DrawTransport(ImVec2(0, display.y - transport_height),
                  ImVec2(display.x, transport_height), send);
    if (middle_field_ && middle_seen_frame_ != frame) CancelNumericScrub();
    HandleShortcuts(send);
    if (shortcuts_visible) DrawHotkeys();
    ImGui::GetStyle() = saved_style;
    was_item_active_ = ImGui::IsAnyItemActive();
  }

 private:
  struct Key {
    std::string id;
    float time = 0;
    std::array<float, 3> position{{0, 0, 0}};
    std::array<float, 3> angles{{0, 0, 0}};
    float fov = 90;
    std::array<float, 4> easing{{0, 0, 1, 1}};
  };
  enum class Icon { First, Previous, Play, Pause, Next, Last, Undo, Redo };
  static constexpr ImGuiWindowFlags kPanelFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings;

  float scale_ = 1;
  bool connected_ = false, demo_ = false, paused_ = false, playing_ = false;
  bool sync_demo_ = true, was_item_active_ = false, inspect_key_ = false;
  bool show_inspector_ = true;
  bool shortcuts_focus_requested_ = false;
  int tick_ = 0, tick_rate_ = 64, revision_ = 0, last_draw_frame_ = -1;
  float cursor_ = 0, duration_ = 8, draft_duration_ = 8;
  int start_tick_ = 0, draft_start_tick_ = 0;
  std::string mode_, project_name_, interpolation_, error_, selected_id_, draft_target_;
  char draft_name_[128] = {};
  Key camera_, draft_;
  Json hotkeys_;
  std::vector<Key> keys_;
  std::vector<std::string> capture_ids_;
  bool capture_pending_ = false;
  double capture_requested_at_ = 0, pending_until_ = 0;
  int pending_revision_ = -1, preset_ = 1;
  int graph_handle_ = -1;
  bool graph_changed_ = false;
  std::string timeline_drag_id_;
  bool timeline_active_ = false, timeline_moved_ = false;
  float timeline_drag_time_ = 0, timeline_original_time_ = 0;
  float timeline_drag_start_x_ = 0;
  double last_refresh_ = -100;
  struct NumericEdit { double initial = 0; bool active = false; };
  std::unordered_map<ImGuiID, NumericEdit> numeric_edits_;
  ImGuiID middle_field_ = 0, middle_active_id_ = 0;
  double middle_start_value_ = 0, middle_initial_ = 0, middle_step_ = 1;
  float middle_start_x_ = 0;
  int middle_seen_frame_ = -1;

  static float Clamp(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(maximum, value));
  }
  float S(float value) const { return value * scale_; }
  static const Json& Member(const Json& value, const char* name) {
    static const Json empty;
    if (!value.is_object()) return empty;
    const auto found = value.find(name);
    return found == value.end() ? empty : *found;
  }
  static float Number(const Json& value, float fallback = 0) {
    if (!value.is_number()) return fallback;
    const double number = value.get<double>();
    return std::isfinite(number) &&
                   std::abs(number) < std::numeric_limits<float>::max()
               ? static_cast<float>(number)
               : fallback;
  }
  static int Integer(const Json& value, int fallback = 0) {
    if (!value.is_number()) return fallback;
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < std::numeric_limits<int>::min() ||
        number > std::numeric_limits<int>::max()) return fallback;
    return static_cast<int>(number);
  }
  static bool Boolean(const Json& value, bool fallback = false) {
    return value.is_boolean() ? value.get<bool>() : fallback;
  }
  static std::string String(const Json& value, const char* fallback = "") {
    return value.is_string() ? value.get<std::string>() : fallback;
  }
  template <size_t N>
  static std::array<float, N> Array(const Json& value,
                                   std::array<float, N> fallback) {
    if (!value.is_array() || value.size() != N) return fallback;
    for (size_t i = 0; i < N; ++i) fallback[i] = Number(value[i], fallback[i]);
    return fallback;
  }
  static Key ReadKey(const Json& value) {
    Key key;
    key.id = String(Member(value, "id"));
    key.time = Number(Member(value, "time"));
    key.position = Array<3>(Member(value, "position"), key.position);
    key.angles = Array<3>(Member(value, "angles"), key.angles);
    key.fov = Number(Member(value, "fov"), 90);
    key.easing = Array<4>(Member(value, "easing"), key.easing);
    return key;
  }
  const Key* Selected() const {
    for (const auto& key : keys_) if (key.id == selected_id_) return &key;
    return nullptr;
  }
  int SelectedIndex() const {
    for (size_t i = 0; i < keys_.size(); ++i)
      if (keys_[i].id == selected_id_) return static_cast<int>(i);
    return -1;
  }
  void Select(const std::string& id) {
    CancelNumericScrub();
    numeric_edits_.clear();
    selected_id_ = id;
    inspect_key_ = true;
    show_inspector_ = true;
    draft_target_.clear();
    graph_handle_ = -1;
    if (const Key* key = Selected()) {
      draft_ = *key;
      draft_target_ = id;
    }
  }
  void ReadState(const Json& state) {
    connected_ = Boolean(Member(state, "connected"));
    demo_ = Boolean(Member(state, "demo"));
    paused_ = Boolean(Member(state, "paused"));
    playing_ = Boolean(Member(state, "preview_playing"));
    sync_demo_ = Boolean(Member(state, "sync_demo"), sync_demo_);
    tick_ = Integer(Member(state, "tick"));
    tick_rate_ = std::max(1, Integer(Member(state, "tick_rate"), 64));
    revision_ = Integer(Member(state, "revision"));
    cursor_ = std::max(0.0f, Number(Member(state, "cursor")));
    mode_ = String(Member(state, "mode"), "free");
    error_ = String(Member(state, "error"));
    hotkeys_ = Member(state, "hotkeys");
    camera_ = ReadKey(Member(state, "camera"));
    const Json& project = Member(state, "project");
    project_name_ = String(Member(project, "name"), "Untitled shot");
    interpolation_ = String(Member(project, "interpolation"), "smooth");
    duration_ = std::max(.01f, Number(Member(project, "duration"), 8));
    start_tick_ = Integer(Member(project, "start_tick"));
    keys_.clear();
    const Json& keys = Member(project, "keys");
    if (keys.is_array()) {
      for (const auto& value : keys) {
        Key key = ReadKey(value);
        if (!key.id.empty()) keys_.push_back(std::move(key));
      }
      std::stable_sort(keys_.begin(), keys_.end(),
                       [](const Key& a, const Key& b) { return a.time < b.time; });
    }
    if (capture_pending_) {
      for (const auto& key : keys_) {
        if (std::find(capture_ids_.begin(), capture_ids_.end(), key.id) ==
            capture_ids_.end()) {
          Select(key.id);
          capture_pending_ = false;
          break;
        }
      }
      if (ImGui::GetTime() - capture_requested_at_ > 3) capture_pending_ = false;
    }
    if (!selected_id_.empty() && !Selected()) {
      selected_id_.clear();
      draft_target_.clear();
      inspect_key_ = false;
      graph_handle_ = -1;
    }
    if (timeline_active_ && !timeline_drag_id_.empty() &&
        std::none_of(keys_.begin(), keys_.end(), [&](const Key& key) {
          return key.id == timeline_drag_id_;
        })) {
      timeline_active_ = false;
      timeline_drag_id_.clear();
    }
    const Key* selected = Selected();
    const std::string target = inspect_key_ && selected ? selected->id : "@camera";
    const bool pending = revision_ == pending_revision_ &&
                         ImGui::GetTime() < pending_until_;
    if (target != draft_target_) {
      CancelNumericScrub();
      numeric_edits_.clear();
    }
    if (target != draft_target_ ||
        (!was_item_active_ && graph_handle_ < 0 && !pending)) {
      draft_ = inspect_key_ && selected ? *selected : camera_;
      draft_target_ = target;
    }
    if (!was_item_active_ && !pending) {
      std::snprintf(draft_name_, sizeof(draft_name_), "%s", project_name_.c_str());
      draft_start_tick_ = start_tick_;
      draft_duration_ = duration_;
    }
  }
  void Emit(const Send& send, const char* command, const Json& args = Json::object()) {
    if (!connected_ || !send) return;
    send(command, args);
    pending_revision_ = revision_;
    pending_until_ = ImGui::GetTime() + .6;
  }
  void Capture(const Send& send) {
    if (!demo_) return;
    // Capturing on an occupied tick updates that key instead of submitting
    // an invalid pair of keys at the same timestamp.
    for (const auto& key : keys_) {
      if (std::abs(key.time - cursor_) < .5f / tick_rate_) {
        Emit(send, "update_key", {{"id", key.id}, {"position", camera_.position},
                                  {"angles", camera_.angles}, {"fov", camera_.fov}});
        Select(key.id);
        return;
      }
    }
    capture_ids_.clear();
    for (const auto& key : keys_) capture_ids_.push_back(key.id);
    capture_pending_ = true;
    capture_requested_at_ = ImGui::GetTime();
    Emit(send, "add_key", {{"time", Clamp(cursor_, 0, duration_)}});
  }
  void CommitPose(const Send& send, const char* field, const Json& value) {
    if (inspect_key_ && Selected()) {
      Emit(send, "update_key", {{"id", selected_id_}, {field, value}});
      // Editing the key currently under the playhead should visibly update
      // the controlled camera as soon as the edit is committed.
      if (demo_ && !playing_ && std::abs(cursor_ - Selected()->time) < .5f / tick_rate_)
        Emit(send, "scrub", {{"time", Selected()->time}, {"seek_demo", false}});
    } else {
      Emit(send, "set_camera", {{"position", draft_.position},
                                {"angles", draft_.angles}, {"fov", draft_.fov}});
    }
  }
  void ApplyStyle() const {
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(std::round(S(16)), std::round(S(14)));
    // Native interactive height: 18px label + 9px padding on each side = 36px.
    style.FramePadding = ImVec2(S(8), S(9));
    style.ItemSpacing = ImVec2(S(7), S(7));
    style.ItemInnerSpacing = ImVec2(S(5), S(4));
    style.WindowRounding = 0;
    style.ChildRounding = 0;
    style.FrameRounding = S(2);
    style.PopupRounding = 0;
    style.GrabRounding = 0;
    style.ScrollbarRounding = 0;
    style.ScrollbarSize = S(7);
    style.GrabMinSize = S(8);
    style.WindowBorderSize = S(2);
    style.FrameBorderSize = 1;
    style.PopupBorderSize = S(2);
    style.DisabledAlpha = .3f;
    auto* color = style.Colors;
    // Native huddemocontroller / csgostyles values. The inspector follows
    // its #RoundMenu / #PlayerList surface; toolbar uses hud-blur-bg-color.
    color[ImGuiCol_Text] = ImVec4(.8f, .8f, .8f, 1); // baseText #cccccc
    color[ImGuiCol_TextDisabled] = ImVec4(.62f, .62f, .62f, 1);
    color[ImGuiCol_WindowBg] = ImVec4(48.f/255, 48.f/255, 48.f/255, 240.f/255);
    color[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    // Popup surfaces receive the same screen-space backdrop as full panels.
    color[ImGuiCol_PopupBg] = ImVec4(0, 0, 0, 0);
    color[ImGuiCol_Border] = ImVec4(32.f/255, 32.f/255, 32.f/255, 240.f/255);
    color[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    color[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 3.f/255);
    color[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 40.f/255);
    color[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 64.f/255);
    color[ImGuiCol_Button] = ImVec4(1, 1, 1, 3.f/255);
    color[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 40.f/255);
    color[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 64.f/255);
    color[ImGuiCol_Header] = ImVec4(1, 1, 1, 16.f/255);
    color[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 32.f/255);
    color[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 40.f/255);
    color[ImGuiCol_CheckMark] = Accent();
    color[ImGuiCol_SliderGrab] = Accent(.65f);
    color[ImGuiCol_SliderGrabActive] = Accent();
    color[ImGuiCol_Separator] = ImVec4(1, 1, 1, 16.f/255);
    color[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, .04f);
    color[ImGuiCol_ScrollbarGrab] = ImVec4(.5f, .5f, .5f, .3f);
    color[ImGuiCol_ScrollbarGrabHovered] = ImVec4(.7f, .7f, .7f, .5f);
    color[ImGuiCol_ScrollbarGrabActive] = ImVec4(.8f, .8f, .8f, .6f);
    color[ImGuiCol_TextSelectedBg] = Accent(.28f);
    color[ImGuiCol_NavCursor] = Accent(.8f);
  }
  static ImVec4 Accent(float alpha = 1) { return ImVec4(1, 1, 1, alpha); }
  static ImU32 U32(const ImVec4& color) { return ImGui::ColorConvertFloat4ToU32(color); }
  static ImU32 Ink(float alpha = 1) { return U32(ImVec4(.8f, .8f, .8f, alpha)); }
  void CancelNumericScrub() {
    if (middle_active_id_ && ImGui::GetActiveID() == middle_active_id_)
      ImGui::ClearActiveID();
    middle_field_ = middle_active_id_ = 0;
  }
  template <typename T>
  bool NumericField(const char* label, T* value, double minimum, double maximum,
                    const char* format = "%.2f") {
    const ImGuiID id = ImGui::GetID(label);
    auto& edit = numeric_edits_[id];
    const double before = static_cast<double>(*value);
    if constexpr (std::is_integral<T>::value)
      ImGui::InputInt(label, value, 0, 0, ImGuiInputTextFlags_AutoSelectAll);
    else
      ImGui::InputFloat(label, value, 0, 0, format, ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemActivated()) {
      edit.initial = before;
      edit.active = true;
    }
    const auto bounded = [&](double next, double fallback) {
      if (!std::isfinite(next)) next = fallback;
      return static_cast<T>(std::max(minimum, std::min(maximum, next)));
    };
    bool commit = false;
    if (ImGui::IsItemDeactivatedAfterEdit() && edit.active) {
      *value = bounded(static_cast<double>(*value), edit.initial);
      commit = static_cast<double>(*value) != edit.initial;
      edit.active = false;
    } else if (ImGui::IsItemDeactivated()) {
      edit.active = false;
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && !middle_field_) {
      // A separate active ID keeps native InputText inactive during scrubbing,
      // while the host's ordinary IsAnyItemActive guard blocks global hotkeys.
      middle_initial_ = edit.active ? edit.initial : static_cast<double>(*value);
      middle_start_value_ = static_cast<double>(*value);
      middle_start_x_ = ImGui::GetIO().MousePos.x;
      middle_step_ = ImGui::GetIO().KeyShift ? 10.0 :
          (!std::is_integral<T>::value && ImGui::GetIO().KeyCtrl ? .1 : 1.0);
      edit.active = false;
      ImGui::ClearActiveID();
      ImGui::PushID(label);
      middle_active_id_ = ImGui::GetID("##middle_scrub");
      ImGui::PopID();
      middle_field_ = id;
      ImGui::SetActiveID(middle_active_id_, ImGui::GetCurrentWindow());
      ImGui::GetCurrentContext()->ActiveIdMouseButton = ImGuiMouseButton_Middle;
      ImGui::FocusWindow(ImGui::GetCurrentWindow());
    }
    if (middle_field_ == id) {
      middle_seen_frame_ = ImGui::GetFrameCount();
      ImGui::KeepAliveID(middle_active_id_);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        *value = bounded(middle_initial_, middle_initial_);
        CancelNumericScrub();
      } else {
        const double steps = std::trunc(
            (ImGui::GetIO().MousePos.x - middle_start_x_) / S(4));
        *value = bounded(middle_start_value_ + steps * middle_step_, middle_initial_);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
          commit = static_cast<double>(*value) != middle_initial_;
          CancelNumericScrub();
        }
      }
    }
    Tip("Left click: select and replace the value. Middle drag: 1 unit per 4 px. "
        "Hold Ctrl when starting for 0.1 steps (float fields), Shift for 10. "
        "Enter or click away to commit; Escape cancels.");
    return commit;
  }
  void FontScale() const {
    ImGui::SetWindowFontScale(S(18) / ImGui::GetFont()->FontSize);
  }
  void DrawBackdrop(bool border = true) const {
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto* draw = ImGui::GetWindowDrawList();
    // Begin() has already emitted window decorations, including scrollbars.
    // Keep them above the background by placing this complete channel first.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    // Begin() installs a content clip. The background includes padding and
    // decorations, so temporarily use the complete panel rectangle instead.
    draw->PushClipRect(pos, end, false);
    if (backdrop_texture != 0 && display.x > 0 && display.y > 0) {
      draw->AddImage(backdrop_texture, pos, end,
                      ImVec2(pos.x / display.x, pos.y / display.y),
                      ImVec2(end.x / display.x, end.y / display.y),
                      IM_COL32_WHITE);
    } else {
      // The provided backdrop already contains CSS black160/255 in encoded
      // sRGB space. Only the no-texture fallback needs a separate dim layer.
      draw->AddRectFilled(pos, end, IM_COL32(0, 0, 0, 160));
    }
    if (border)
      draw->AddRect(pos, end, IM_COL32(32, 32, 32, 240), 0, 0, S(2));
    draw->PopClipRect();
    draw->ChannelsSetCurrent(0);
    draw->CmdBuffer.swap(draw->_Splitter._Channels[1]._CmdBuffer);
    draw->IdxBuffer.swap(draw->_Splitter._Channels[1]._IdxBuffer);
    draw->ChannelsMerge();
  }
  void Tip(const char* text) const {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::BeginTooltip();
      DrawBackdrop();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 27);
      ImGui::TextUnformatted(text);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
  }
  void Caption(const char* text) const {
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", text);
  }
  static void TimeText(char* buffer, size_t length, float seconds) {
    seconds = std::max(0.0f, seconds);
    const int minutes = static_cast<int>(seconds / 60);
    std::snprintf(buffer, length, "%02d:%06.3f", minutes,
                  seconds - static_cast<float>(minutes * 60));
  }
  bool AccentButton(const char* label, const ImVec2& size = ImVec2(0, 0)) const {
    ImGui::PushStyleColor(ImGuiCol_Button, Accent(16.f/255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent(40.f/255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Accent(64.f/255));
    ImGui::PushStyleColor(ImGuiCol_Text, Accent());
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
  }
  void DrawInspector(ImVec2 pos, ImVec2 size, const Json&, const Send& send) {
    // Like the native demo settings/player menu, this surface fits its
    // contents and grows upward from the controller rather than filling a rail.
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(size.x, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(size.x, 0), size);
    ImGui::Begin("##CS2CinematicInspector", nullptr,
                 kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoBackground);
    DrawBackdrop();
    FontScale();
    // Interactive controls use the original demo toolbar's faint white outline.
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 7.f/255));
    ImGui::TextUnformatted("Camera track");
    const float dot_x = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - S(21);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(dot_x, ImGui::GetItemRectMin().y + ImGui::GetFontSize() * .5f),
        S(3), connected_ ? U32(Accent()) : Ink(.3f));
    Caption(connected_ ? (!demo_ ? "No demo loaded" :
                         mode_ == "preview" ? "Playing camera preview" :
                         mode_ == "hold" ? "Camera held" : "Free camera") :
                        "Waiting for game");
    if (!connected_) {
      ImGui::Spacing();
      ImGui::TextWrapped("Camera tools become available when the native game bridge connects.");
      if (ImGui::Button("Refresh connection") && send &&
          ImGui::GetTime() - last_refresh_ > .5) {
        send("get_state", Json::object());
        last_refresh_ = ImGui::GetTime();
      }
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!connected_);
    if (ImGui::CollapsingHeader("Shot settings")) {
      Caption("Project");
      ImGui::SetNextItemWidth(-1);
      ImGui::InputText("##shot_name", draft_name_, sizeof(draft_name_));
      if (ImGui::IsItemDeactivatedAfterEdit())
        Emit(send, "set_project", {{"name", std::string(draft_name_)}});
      const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
      const float second_column = ImGui::GetCursorPosX() + half + S(7);
      Caption("Start tick");
      ImGui::SameLine(second_column);
      Caption("Duration");
      ImGui::SetNextItemWidth(half);
      if (NumericField("##shot_start", &draft_start_tick_, 0, 2000000000))
        Emit(send, "set_project", {{"start_tick", std::max(0, draft_start_tick_)}});
      ImGui::SameLine();
      ImGui::SetNextItemWidth(half);
      const float minimum_duration = keys_.empty() ? .01f : std::max(.01f, keys_.back().time);
      if (NumericField("##shot_duration", &draft_duration_, minimum_duration, 86400, "%.2f s"))
        Emit(send, "set_project", {{"duration", Clamp(draft_duration_, minimum_duration, 86400)}});
      if (ImGui::Button("Use current demo tick", ImVec2(-1, 0)))
        Emit(send, "set_project", {{"start_tick", std::max(0, tick_)}});
      Caption("Camera path");
      int interpolation = interpolation_ == "linear" ? 1 : 0;
      ImGui::SetNextItemWidth(-1);
      const char* path_names[] = {"Smooth", "Linear"};
      if (ImGui::BeginCombo("##interpolation", path_names[interpolation])) {
        DrawBackdrop();
        for (int i = 0; i < 2; ++i)
          if (ImGui::Selectable(path_names[i], interpolation == i))
            Emit(send, "set_project", {{"interpolation", i ? "linear" : "smooth"}});
        ImGui::EndCombo();
      }
      Tip("Smooth blends the spatial path through keys. Linear uses straight segments. "
          "The speed curve controls timing independently.");
      ImGui::Separator();
    }
    const float tabs_width = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
    if (!inspect_key_) {
      if (AccentButton("Live camera", ImVec2(tabs_width, 0))) {}
    } else if (ImGui::Button("Live camera", ImVec2(tabs_width, 0))) {
      inspect_key_ = false;
      draft_target_.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(keys_.empty());
    if (inspect_key_) {
      if (AccentButton("Keyframe", ImVec2(tabs_width, 0))) {}
    } else if (ImGui::Button("Keyframe", ImVec2(tabs_width, 0))) {
      if (!Selected() && !keys_.empty()) Select(keys_.front().id);
      inspect_key_ = true;
      draft_target_.clear();
    }
    ImGui::EndDisabled();
    // Switching targets must update this frame, before any input can edit it.
    const Key* selected = Selected();
    const std::string target = inspect_key_ && selected ? selected->id : "@camera";
    if (draft_target_ != target) {
      CancelNumericScrub();
      numeric_edits_.clear();
      draft_ = inspect_key_ && selected ? *selected : camera_;
      draft_target_ = target;
    }
    if (inspect_key_ && selected) {
      char key_label[80], time[32];
      TimeText(time, sizeof(time), selected->time);
      std::snprintf(key_label, sizeof(key_label), "Key %02d    %s",
                    SelectedIndex() + 1, time);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##key_select", key_label)) {
        DrawBackdrop();
        for (size_t i = 0; i < keys_.size(); ++i) {
          TimeText(time, sizeof(time), keys_[i].time);
          std::snprintf(key_label, sizeof(key_label), "%02d    %s", int(i) + 1, time);
          if (ImGui::Selectable(key_label, keys_[i].id == selected_id_))
            Select(keys_[i].id);
        }
        ImGui::EndCombo();
      }
    } else {
      Caption("Current world-space pose");
    }
    DrawPoseFields(send);
    if (inspect_key_ && Selected()) {
      ImGui::Spacing();
      DrawEasing(send);
      ImGui::Spacing();
      const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
      if (ImGui::Button("Go to key", ImVec2(half, 0)))
        Emit(send, "scrub", {{"time", draft_.time}, {"seek_demo", sync_demo_ && demo_}});
      ImGui::SameLine();
      if (ImGui::Button("Remove key", ImVec2(half, 0)))
        Emit(send, "remove_key", {{"id", selected_id_}});
      Tip("Remove selected keyframe. Ctrl+Z restores it.");
    } else {
      ImGui::Spacing();
      ImGui::BeginDisabled(!demo_);
      if (AccentButton("Capture keyframe", ImVec2(-1, 0))) Capture(send);
      Tip("Capture the current camera at the playhead. An existing key at this time is updated. Shortcut: K");
      ImGui::EndDisabled();
      if (ImGui::Button("Release camera", ImVec2(-1, 0)))
        Emit(send, "release_camera");
      Tip("Return camera control to CS2.");
    }
    ImGui::Spacing();
    ImGui::Separator();
    Caption("Create from current camera");
    ImGui::BeginDisabled(!demo_);
    static const char* preset_names[] = {"Hold current pose", "Dolly forward", "Orbit"};
    const float apply_width = S(62);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - apply_width - S(7));
    if (ImGui::BeginCombo("##preset", preset_names[preset_])) {
      DrawBackdrop();
      for (int i = 0; i < 3; ++i)
        if (ImGui::Selectable(preset_names[i], preset_ == i)) preset_ = i;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Create", ImVec2(apply_width, 0))) {
      static const char* names[] = {"hold", "dolly", "orbit"};
      Emit(send, "apply_preset", {{"name", names[preset_]}, {"duration", duration_}});
    }
    Tip("Replace this track with a camera preset. Ctrl+Z restores the previous track.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!error_.empty()) {
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.9f, .56f, .42f, 1));
      ImGui::TextWrapped("%s", error_.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    const std::string editor_key = String(Member(hotkeys_, "editor"), "F8");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s  show / hide", editor_key.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - S(78));
    if (ImGui::Button("Hotkeys", ImVec2(S(78), 0))) {
      shortcuts_visible = !shortcuts_visible;
      shortcuts_focus_requested_ = shortcuts_visible;
    }
    ImGui::PopStyleColor();
    ImGui::End();
  }
  void DrawHotkeys() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = std::min(S(560), display.x * .44f);
    ImGui::SetNextWindowPos(ImVec2(S(24), S(48)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0),
                                       ImVec2(width, display.y - S(160)));
    if (shortcuts_focus_requested_) {
      ImGui::SetNextWindowFocus();
      shortcuts_focus_requested_ = false;
    }
    ImGui::Begin("##CS2CameraShortcuts", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
    DrawBackdrop();
    FontScale();
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 7.f/255));
    const ImVec2 header = ImGui::GetCursorScreenPos();
    const float header_width = ImGui::GetContentRegionAvail().x - S(77);
    ImGui::InvisibleButton("##move_shortcuts", ImVec2(header_width, S(36)));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      const ImVec2 pos = ImGui::GetWindowPos(), delta = ImGui::GetIO().MouseDelta;
      ImGui::SetWindowPos(ImVec2(Clamp(pos.x + delta.x, 0, display.x - width),
                                 Clamp(pos.y + delta.y, 0, display.y - S(60))));
    }
    ImGui::GetWindowDrawList()->AddText(ImVec2(header.x, header.y + S(9)),
                                        IM_COL32_WHITE, "Keyboard shortcuts");
    Tip("Drag this heading to move the shortcuts panel.");
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(S(70), S(36)))) shortcuts_visible = false;
    ImGui::Separator();
    Caption("Global controls");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(6), S(8)));
    const auto row = [&](const char* action, const std::string& key) {
      ImGui::TableNextRow(ImGuiTableRowFlags_None, S(36));
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(action);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextColored(Accent(), "%s", key.c_str());
    };
    if (ImGui::BeginTable("##global_shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Action", 0, .58f);
      ImGui::TableSetupColumn("Key", 0, .42f);
      row("Capture keyframe", String(Member(hotkeys_, "capture"), "F6"));
      row("Free / hold camera", String(Member(hotkeys_, "camera"), "F7"));
      row("Show / hide editor", String(Member(hotkeys_, "editor"), "F8"));
      row("Play / pause preview", String(Member(hotkeys_, "preview"), "F9"));
      ImGui::EndTable();
    }
    ImGui::Spacing();
    Caption("With the editor open");
    if (ImGui::BeginTable("##editor_shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Action", 0, .58f);
      ImGui::TableSetupColumn("Key", 0, .42f);
      row("Capture keyframe", "K");
      row("Play / pause preview", "Space");
      row("Remove selected key", "Delete");
      row("Undo", "Ctrl+Z");
      row("Redo", "Ctrl+Shift+Z / Ctrl+Y");
      row("Save project", "Ctrl+S");
      row("Load project", "Ctrl+O");
      row("Drag without tick snapping", "Shift");
      row("Show / hide this panel", "F10");
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    Caption("Numeric fields");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(6), S(8)));
    if (ImGui::BeginTable("##numeric_shortcuts", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Action", 0, .58f);
      ImGui::TableSetupColumn("Key", 0, .42f);
      row("Select and replace value", "Left click, then type");
      row("Scrub by 1 per 4 px", "Middle drag");
      row("Fine 0.1 / fast 10", "Ctrl / Shift + middle drag");
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Global controls also work when the editor is hidden. "
                       "Shortcuts pause while you edit text or a number.");
    if (ImGui::CollapsingHeader("Configure global keys")) {
      ImGui::TextWrapped("CS2_CAMERA_HOTKEYS (JSON)");
      const std::string config = String(Member(hotkeys_, "config_path"));
      if (!config.empty()) ImGui::TextWrapped("%s", config.c_str());
      ImGui::TextWrapped("Global binding changes take effect when CS2 restarts.");
    }
    ImGui::PopStyleColor();
    ImGui::End();
  }
  void DrawPoseFields(const Send& send) {
    ImGui::BeginDisabled(!inspect_key_ && !demo_);
    Caption("Position   X / Y / Z");
    const float axis_width = (ImGui::GetContentRegionAvail().x - S(14)) / 3;
    bool pose_commit = false;
    ImGui::PushID("position");
    for (int axis = 0; axis < 3; ++axis) {
      if (axis) ImGui::SameLine();
      ImGui::PushID(axis);
      ImGui::SetNextItemWidth(axis_width);
      pose_commit |= NumericField("##value", &draft_.position[axis], -1000000, 1000000);
      ImGui::PopID();
    }
    ImGui::PopID();
    if (pose_commit) CommitPose(send, "position", draft_.position);
    Caption("Rotation   pitch / yaw / roll");
    pose_commit = false;
    ImGui::PushID("angles");
    for (int axis = 0; axis < 3; ++axis) {
      if (axis) ImGui::SameLine();
      ImGui::PushID(axis);
      ImGui::SetNextItemWidth(axis_width);
      pose_commit |= NumericField("##value", &draft_.angles[axis],
                                  axis ? -1000000 : -90, axis ? 1000000 : 90);
      ImGui::PopID();
    }
    ImGui::PopID();
    if (pose_commit) CommitPose(send, "angles", draft_.angles);
    const bool key_mode = inspect_key_ && Selected();
    const float width = key_mode ?
        (ImGui::GetContentRegionAvail().x - S(7)) / 2 : ImGui::GetContentRegionAvail().x;
    const float second_column = ImGui::GetCursorPosX() + width + S(7);
    Caption("Field of view");
    if (key_mode) {
      ImGui::SameLine(second_column);
      Caption("Time");
    }
    ImGui::SetNextItemWidth(width);
    if (NumericField("##fov", &draft_.fov, 1, 179, "%.1f deg"))
      CommitPose(send, "fov", draft_.fov);
    if (key_mode) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(width);
      if (NumericField("##key_time", &draft_.time, 0, duration_, "%.3f s"))
        Emit(send, "update_key", {{"id", selected_id_},
                                  {"time", Clamp(draft_.time, 0, duration_)}});
    }
    ImGui::EndDisabled();
  }
  void DrawEasing(const Send& send) {
    const int index = SelectedIndex();
    const bool has_segment = index >= 0 && index + 1 < static_cast<int>(keys_.size());
    Caption("Speed curve");
    ImGui::SameLine();
    const char* segment = has_segment ? "to next key" : "last key";
    const float x = ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(segment).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
    Caption(segment);
    ImGui::BeginDisabled(!has_segment);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 extent(ImGui::GetContentRegionAvail().x, S(126));
    ImGui::InvisibleButton("##bezier_graph", extent);
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const float pad_x = S(24), pad_top = S(12), pad_bottom = S(23);
    const ImVec2 lo(origin.x + pad_x, origin.y + pad_top);
    const ImVec2 hi(origin.x + extent.x - S(13), origin.y + extent.y - pad_bottom);
    const float width = hi.x - lo.x, height = hi.y - lo.y;
    auto point = [&](float x, float y) {
      return ImVec2(lo.x + x * width, hi.y - y * height);
    };
    draw->AddRectFilled(origin, ImVec2(origin.x + extent.x, origin.y + extent.y),
                        IM_COL32(12, 12, 10, 55));
    draw->AddRect(origin, ImVec2(origin.x + extent.x, origin.y + extent.y),
                  Ink(.09f));
    for (int i = 0; i <= 4; ++i) {
      const float t = i * .25f;
      draw->AddLine(point(t, 0), point(t, 1), Ink(i == 0 ? .2f : .07f));
      draw->AddLine(point(0, t), point(1, t), Ink(i == 0 ? .2f : .07f));
    }
    draw->AddLine(point(0, 0), point(1, 1), Ink(.12f));
    auto& bezier = draft_.easing;
    ImVec2 h1 = point(bezier[0], bezier[1]);
    ImVec2 h2 = point(bezier[2], bezier[3]);
    const auto distance2 = [](ImVec2 a, ImVec2 b) {
      const float dx = a.x - b.x, dy = a.y - b.y;
      return dx * dx + dy * dy;
    };
    if (has_segment && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const float d1 = distance2(mouse, h1), d2 = distance2(mouse, h2);
      if (std::min(d1, d2) < S(16) * S(16)) {
        graph_handle_ = d1 <= d2 ? 0 : 2;
        graph_changed_ = false;
      }
    }
    if (graph_handle_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const float bx = Clamp((mouse.x - lo.x) / width, 0, 1);
      const float by = Clamp((hi.y - mouse.y) / height, 0, 1);
      if (bezier[graph_handle_] != bx || bezier[graph_handle_ + 1] != by) {
        bezier[graph_handle_] = bx;
        bezier[graph_handle_ + 1] = by;
        graph_changed_ = true;
      }
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (graph_handle_ >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (graph_changed_) Emit(send, "update_key", {{"id", selected_id_}, {"easing", bezier}});
      graph_handle_ = -1;
      graph_changed_ = false;
    }
    h1 = point(bezier[0], bezier[1]);
    h2 = point(bezier[2], bezier[3]);
    const float alpha = has_segment ? 1.0f : .3f;
    draw->AddLine(point(0, 0), h1, U32(Accent(.48f * alpha)), S(1));
    draw->AddLine(point(1, 1), h2, U32(Accent(.48f * alpha)), S(1));
    ImVec2 previous = point(0, 0);
    for (int i = 1; i <= 64; ++i) {
      const float t = i / 64.0f, u = 1 - t;
      const float bx = 3 * u * u * t * bezier[0] + 3 * u * t * t * bezier[2] + t * t * t;
      const float by = 3 * u * u * t * bezier[1] + 3 * u * t * t * bezier[3] + t * t * t;
      const ImVec2 next = point(bx, by);
      draw->AddLine(previous, next, U32(Accent(alpha)), S(2));
      previous = next;
    }
    for (const auto handle : {h1, h2}) {
      draw->AddCircleFilled(handle, S(4), IM_COL32(48, 48, 48, 255));
      draw->AddCircle(handle, S(4), U32(Accent(alpha)), 16, S(1.5f));
    }
    draw->AddCircleFilled(point(0, 0), S(2.5f), Ink(alpha));
    draw->AddCircleFilled(point(1, 1), S(2.5f), Ink(alpha));
    draw->AddText(ImVec2(lo.x, hi.y + S(6)), Ink(.4f), "0");
    const char* axis = "Time";
    draw->AddText(ImVec2(hi.x - ImGui::CalcTextSize(axis).x, hi.y + S(6)),
                  Ink(.4f), axis);
    if (hovered && graph_handle_ < 0)
      Tip(has_segment ?
          "Drag either handle. X is elapsed time; Y is camera progress. "
          "The slope controls speed between this key and the next." :
          "The last key has no outgoing segment. Select an earlier key to edit its curve.");
    const float button_width = (ImGui::GetContentRegionAvail().x - S(14)) / 3;
    if (ImGui::Button("Linear", ImVec2(button_width, 0))) SetEasing(send, {{0, 0, 1, 1}});
    ImGui::SameLine();
    if (ImGui::Button("Smooth", ImVec2(button_width, 0))) SetEasing(send, {{.42f, 0, .58f, 1}});
    ImGui::SameLine();
    if (ImGui::Button("Ease out", ImVec2(button_width, 0))) SetEasing(send, {{0, 0, .58f, 1}});
    ImGui::EndDisabled();
  }
  void SetEasing(const Send& send, std::array<float, 4> easing) {
    draft_.easing = easing;
    Emit(send, "update_key", {{"id", selected_id_}, {"easing", easing}});
  }
  bool ToolbarButton(const char* label, ImVec2 size) const {
    const bool pressed = ImGui::Button(label, size);
    if (ImGui::IsItemHovered())
      ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                         IM_COL32(255, 255, 255, 64), S(2));
    return pressed;
  }
  bool IconButton(const char* id, Icon icon, const char* tooltip,
                  ImVec2 size = ImVec2(0, 0)) const {
    if (size.x == 0) size = ImVec2(S(36), S(36));
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const bool pressed = ToolbarButton(id, size);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 c(top.x + size.x / 2, top.y + size.y / 2);
    const ImU32 ink = U32(ImVec4(1, 1, 1, ImGui::GetStyle().Alpha));
    const float r = S(8.5f);
    if (icon == Icon::Pause) {
      draw->AddRectFilled(ImVec2(c.x - S(7), c.y - r), ImVec2(c.x - S(2), c.y + r), ink);
      draw->AddRectFilled(ImVec2(c.x + S(2), c.y - r), ImVec2(c.x + S(7), c.y + r), ink);
    } else if (icon == Icon::Undo || icon == Icon::Redo) {
      const float direction = icon == Icon::Undo ? -1.0f : 1.0f;
      draw->AddLine(ImVec2(c.x - direction * S(6), c.y + S(5)),
                    ImVec2(c.x - direction * S(6), c.y - S(4)), ink, S(2));
      draw->AddLine(ImVec2(c.x - direction * S(6), c.y - S(4)),
                    ImVec2(c.x + direction * S(5), c.y - S(4)), ink, S(2));
      draw->AddTriangleFilled(ImVec2(c.x + direction * S(9), c.y - S(4)),
                              ImVec2(c.x + direction * S(2), c.y - S(9)),
                              ImVec2(c.x + direction * S(2), c.y + S(1)), ink);
    } else {
      const bool reverse = icon == Icon::First || icon == Icon::Previous;
      const float direction = reverse ? -1.0f : 1.0f;
      draw->AddTriangleFilled(ImVec2(c.x + direction * r, c.y),
                              ImVec2(c.x - direction * S(6), c.y - r),
                              ImVec2(c.x - direction * S(6), c.y + r), ink);
      if (icon != Icon::Play)
        draw->AddRectFilled(ImVec2(c.x + direction * S(10) - S(1.5f), c.y - r),
                             ImVec2(c.x + direction * S(10) + S(1.5f), c.y + r), ink);
    }
    Tip(tooltip);
    return pressed;
  }
  bool CameraButton(const char* id, const char* label, float width) const {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ToolbarButton(id, ImVec2(width, S(36)));
    auto* draw = ImGui::GetWindowDrawList();
    const float y = pos.y + S(18);
    const ImU32 color = U32(ImVec4(1, 1, 1, ImGui::GetStyle().Alpha));
    draw->AddRectFilled(ImVec2(pos.x + S(9), y - S(6)), ImVec2(pos.x + S(25), y + S(6)), color);
    draw->AddTriangleFilled(ImVec2(pos.x + S(25), y), ImVec2(pos.x + S(31), y - S(6)),
                            ImVec2(pos.x + S(31), y + S(6)), color);
    draw->AddText(ImVec2(pos.x + S(39), y - ImGui::GetFontSize() / 2), Ink(ImGui::GetStyle().Alpha), label);
    return pressed;
  }
  void DrawProjectMenu(const Send& send) {
    if (!ImGui::BeginPopup("##camera_project_menu", ImGuiWindowFlags_NoBackground)) return;
    DrawBackdrop();
    FontScale();
    ImGui::TextUnformatted(project_name_.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Save project", "Ctrl+S")) Emit(send, "save_project");
    if (ImGui::MenuItem("Load project", "Ctrl+O")) Emit(send, "load_project");
    ImGui::Separator();
    if (ImGui::MenuItem("Undo", "Ctrl+Z")) Emit(send, "undo");
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z")) Emit(send, "redo");
    ImGui::Separator();
    if (ImGui::MenuItem("Go to beginning", nullptr, false, demo_ && !keys_.empty()))
      Emit(send, "scrub", {{"time", 0}, {"seek_demo", sync_demo_}});
    if (ImGui::MenuItem("Go to end", nullptr, false, demo_ && !keys_.empty()))
      Emit(send, "scrub", {{"time", duration_}, {"seek_demo", sync_demo_}});
    if (ImGui::MenuItem("Camera inspector", nullptr, show_inspector_))
      show_inspector_ = !show_inspector_;
    if (ImGui::MenuItem("Keyboard shortcuts", "F10", shortcuts_visible)) {
      shortcuts_visible = !shortcuts_visible;
      shortcuts_focus_requested_ = shortcuts_visible;
    }
    ImGui::EndPopup();
  }
  void DrawTransport(ImVec2 pos, ImVec2 size, const Send& send) {
    // Exact geometry from native huddemocontroller: 96px at 1080p, comprising
    // a 50px slider row, a 41px control row and 5px bottom margin. World blur
    // belongs to the host render pass; no captured game image is used here.
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 160.f/255));
    ImGui::Begin("##CS2CinematicTransport", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    DrawBackdrop(false);
    FontScale();
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 7.f/255));
    ImGui::BeginDisabled(!connected_);
    DrawTimeline(send);
    ImGui::SetCursorPos(ImVec2(S(5), S(50)));
    ImGui::BeginDisabled(keys_.empty() || !demo_);
    if (IconButton("##preview", playing_ ? Icon::Pause : Icon::Play,
                   playing_ ? "Pause camera preview. Space" : "Play camera preview. Space",
                   ImVec2(S(82), S(41))))
      Emit(send, "preview", {{"playing", !playing_}, {"sync_demo", sync_demo_ && demo_}});
    ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(S(102), S(61)));
    ImGui::TextUnformatted(playing_ ? "Playing" : "Paused");
    char current[24], total[24], precise[32];
    const float time = timeline_active_ ? timeline_drag_time_ : cursor_;
    std::snprintf(current, sizeof(current), "%02d:%02d", int(time) / 60, int(time) % 60);
    std::snprintf(total, sizeof(total), "%02d:%02d", int(duration_) / 60, int(duration_) % 60);
    ImGui::SetCursorPos(ImVec2(S(190), S(61)));
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::Text("Time:  %s / %s    Tick:  %06d", current, total, tick_);
    if (mono_font) ImGui::PopFont();
    TimeText(precise, sizeof(precise), time);
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      DrawBackdrop();
      ImGui::Text("Camera time: %s", precise);
      ImGui::EndTooltip();
    }
    const float controls_x = std::max(S(505), ImGui::GetItemRectMax().x - pos.x + S(30));
    ImGui::SetCursorPos(ImVec2(controls_x, S(52.5f)));
    ImGui::BeginDisabled(keys_.empty() || !demo_);
    if (IconButton("##prev", Icon::First, "Previous keyframe")) JumpKey(send, -1);
    ImGui::SameLine();
    char key_count[32];
    std::snprintf(key_count, sizeof(key_count), "Key: %02d", SelectedIndex() + 1);
    if (ToolbarButton(key_count, ImVec2(S(85), S(36))) && Selected())
      Emit(send, "scrub", {{"time", Selected()->time}, {"seek_demo", sync_demo_}});
    Tip("Return to selected keyframe.");
    ImGui::SameLine();
    if (IconButton("##next", Icon::Last, "Next keyframe")) JumpKey(send, 1);
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(30));
    std::string name = project_name_;
    while (name.size() > 3 && ImGui::CalcTextSize(name.c_str()).x > S(175))
      name = name.substr(0, name.size() - 4) + "...";
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(181.f/255, 212.f/255, 238.f/255, 1));
    if (ToolbarButton((name + "##project").c_str(), ImVec2(S(190), S(36))))
      ImGui::OpenPopup("##camera_project_menu");
    ImGui::PopStyleColor();
    Tip("Project: save, load, undo and redo.");
    ImGui::SameLine(0, S(10));
    ImGui::BeginDisabled(!demo_);
    if (CameraButton("##capture", "Capture", S(110))) Capture(send);
    Tip("Capture current camera. An existing key at this time is updated.");
    ImGui::EndDisabled();
    const float right_start = size.x - S(480);
    ImGui::SetCursorPos(ImVec2(std::max(right_start, ImGui::GetItemRectMax().x - pos.x + S(20)), S(52.5f)));
    if (CameraButton("##camera_inspector", "Camera", S(116))) show_inspector_ = !show_inspector_;
    Tip("Show or hide camera inspector.");
    ImGui::SameLine(0, S(14));
    ImGui::BeginDisabled(!demo_);
    ImGui::AlignTextToFramePadding();
    if (ImGui::Checkbox("Sync demo", &sync_demo_))
      Emit(send, "preview", {{"playing", false}, {"sync_demo", sync_demo_}});
    Tip("Camera scrubbing and preview also control the demo.");
    ImGui::SameLine(0, S(14));
    if (ToolbarButton(paused_ ? "Play demo" : "Pause demo", ImVec2(S(105), S(36))))
      Emit(send, "demo_control", {{"action", paused_ ? "resume" : "pause"}});
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(20));
    if (ToolbarButton("Hide", ImVec2(S(65), S(36)))) visible = false;
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    DrawProjectMenu(send);
    ImGui::End();
  }
  void DrawTimeline(const Send& send) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(50));
    ImGui::InvisibleButton("##camera_timeline", size);
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const float left = pos.x + S(10), right = pos.x + size.x - S(10);
    const float width = std::max(1.0f, right - left), y = pos.y + S(15);
    auto tx = [&](float time) { return left + Clamp(time / duration_, 0, 1) * width; };
    const float track_top = pos.y + S(30), track_bottom = pos.y + S(38);
    draw->AddRectFilled(ImVec2(left, track_top), ImVec2(right, track_bottom),
                        IM_COL32(204, 204, 204, 32));
    // Camera intervals replace the native demo's round intervals, retaining
    // the same alternating #ffffff20 / #ffffff50 strip.
    float segment_start = 0;
    for (size_t i = 0; i <= keys_.size(); ++i) {
      const float segment_end = i < keys_.size() ? keys_[i].time : duration_;
      if (segment_end > segment_start)
        draw->AddRectFilled(ImVec2(tx(segment_start), track_top),
                            ImVec2(tx(segment_end), track_bottom),
                            IM_COL32(255, 255, 255, i % 2 ? 80 : 32));
      segment_start = segment_end;
    }
    if (keys_.empty()) {
      const char* hint = "Capture a keyframe or create a preset";
      const ImVec2 text_size = ImGui::CalcTextSize(hint);
      const ImVec2 text_pos(left + (width - text_size.x) / 2, y - text_size.y / 2);
      draw->AddText(text_pos, Ink(.6f), hint);
    }
    std::string hovered_id;
    float nearest = S(12);
    if (hovered) {
      const auto mouse = ImGui::GetIO().MousePos;
      for (const auto& key : keys_) {
        const float dx = std::abs(mouse.x - tx(key.time));
        if (dx < nearest && std::abs(mouse.y - y) < S(13)) {
          nearest = dx;
          hovered_id = key.id;
        }
      }
    }
    if (hovered && connected_ && !keys_.empty() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      timeline_active_ = true;
      timeline_drag_id_ = hovered_id;
      timeline_drag_start_x_ = ImGui::GetIO().MousePos.x;
      timeline_moved_ = false;
      if (!hovered_id.empty()) {
        Select(hovered_id);
        timeline_original_time_ = Selected()->time;
        timeline_drag_time_ = timeline_original_time_;
      } else {
        timeline_original_time_ = 0;
        timeline_drag_time_ = Clamp((ImGui::GetIO().MousePos.x - left) / width, 0, 1) * duration_;
      }
    }
    if (timeline_active_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const float delta = ImGui::GetIO().MousePos.x - timeline_drag_start_x_;
      if (std::abs(delta) > S(2)) timeline_moved_ = true;
      if (timeline_drag_id_.empty())
        timeline_drag_time_ = Clamp((ImGui::GetIO().MousePos.x - left) / width, 0, 1) * duration_;
      else if (timeline_moved_)
        timeline_drag_time_ = Clamp(timeline_original_time_ + delta / width * duration_, 0, duration_);
      if (!ImGui::GetIO().KeyShift)
        timeline_drag_time_ = Clamp(std::round(timeline_drag_time_ * tick_rate_) / tick_rate_, 0, duration_);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (timeline_active_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!timeline_drag_id_.empty() && timeline_moved_)
        Emit(send, "update_key", {{"id", timeline_drag_id_}, {"time", timeline_drag_time_}});
      if (demo_)
        Emit(send, "scrub", {{"time", timeline_drag_time_}, {"seek_demo", sync_demo_}});
      timeline_active_ = false;
      timeline_drag_id_.clear();
    }
    for (size_t i = 0; i < keys_.size(); ++i) {
      const Key& key = keys_[i];
      const bool selected = key.id == selected_id_;
      const float x = tx(timeline_active_ && key.id == timeline_drag_id_ ?
                            timeline_drag_time_ : key.time);
      const float r = S(key.id == hovered_id ? 10 : 8);
      const ImU32 color = selected || key.id == hovered_id ?
          IM_COL32(255, 255, 255, 255) : IM_COL32(112, 112, 112, 255);
      if (selected)
        draw->AddLine(ImVec2(x, y + r + S(2)), ImVec2(x, track_top), U32(Accent(.3f)));
      const ImVec2 diamond[] = {ImVec2(x, y - r), ImVec2(x + r, y),
                                ImVec2(x, y + r), ImVec2(x - r, y)};
      draw->AddConvexPolyFilled(diamond, 4, color);
    }
    const float playhead = tx(timeline_active_ ? timeline_drag_time_ : cursor_);
    draw->AddRectFilled(ImVec2(playhead - S(8), pos.y + S(26)),
                        ImVec2(playhead + S(8), pos.y + S(42)),
                        IM_COL32(255, 255, 255, 255), S(2));
    if (hovered && !timeline_active_) {
      Tip(hovered_id.empty() ? "Click or drag to scrub. Release to seek. Shift disables tick snapping." :
                              "Click to select and seek. Drag to move the keyframe. Shift disables tick snapping.");
    }
    if (timeline_active_) {
      char time[32];
      TimeText(time, sizeof(time), timeline_drag_time_);
      ImGui::BeginTooltip();
      DrawBackdrop();
      ImGui::Text("%s%s", timeline_drag_id_.empty() ? "" : "Key  ", time);
      ImGui::EndTooltip();
    }
  }
  void JumpKey(const Send& send, int direction) {
    float target = direction < 0 ? 0.0f : duration_;
    if (direction < 0) {
      for (auto it = keys_.rbegin(); it != keys_.rend(); ++it) {
        if (it->time < cursor_ - .001f) { target = it->time; Select(it->id); break; }
      }
    } else {
      for (const auto& key : keys_) {
        if (key.time > cursor_ + .001f) { target = key.time; Select(key.id); break; }
      }
    }
    Emit(send, "scrub", {{"time", target}, {"seek_demo", sync_demo_ && demo_}});
  }
  void HandleShortcuts(const Send& send) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || middle_field_ || ImGui::IsAnyItemActive()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) {
      shortcuts_visible = !shortcuts_visible;
      shortcuts_focus_requested_ = shortcuts_visible;
      return;
    }
    if (!connected_ || !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
      Emit(send, io.KeyShift ? "redo" : "undo");
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
      Emit(send, "redo");
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
      Emit(send, "save_project");
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
      Emit(send, "load_project");
    else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) &&
             !keys_.empty() && demo_)
      Emit(send, "preview", {{"playing", !playing_}, {"sync_demo", sync_demo_ && demo_}});
    else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false))
      Capture(send);
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && inspect_key_ && Selected())
      Emit(send, "remove_key", {{"id", selected_id_}});
  }
};

}  // namespace cs2_live_ui
