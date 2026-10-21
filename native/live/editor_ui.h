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
  // The host reads the private installed-game catalog once, outside rendering.
  Json cosmetics_catalog;
  std::string cosmetics_catalog_error;

  void Draw(const Json& state, const Send& send) {
    if (!visible || !ImGui::GetCurrentContext()) return;
    const int frame = ImGui::GetFrameCount();
    if (last_draw_frame_ >= 0 && frame != last_draw_frame_ + 1) {
      timeline_active_ = false;
      timeline_drag_id_.clear();
      range_drag_handle_ = -1;
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
  bool general_panel_ = false;
  bool shortcuts_focus_requested_ = false;
  int tick_ = 0, tick_rate_ = 64, revision_ = 0, last_draw_frame_ = -1;
  float cursor_ = 0, duration_ = 8, draft_duration_ = 8;
  double exact_duration_ = 8;
  bool render_region_ = false;
  double range_in_ = 0, range_out_ = 8, draft_range_in_ = 0, draft_range_out_ = 8;
  int range_drag_handle_ = -1;
  double range_drag_time_ = 0, range_drag_initial_ = 0;
  float range_drag_start_x_ = 0;
  bool range_drag_moved_ = false;
  int start_tick_ = 0, draft_start_tick_ = 0;
  std::string mode_, project_name_, interpolation_, error_, selected_id_, draft_target_;
  char draft_name_[128] = {};
  Key camera_, draft_;
  Json hotkeys_;
  Json scene_capabilities_;
  Json presentation_;
  Json appearance_;
  Json appearance_capabilities_;
  Json cosmetics_;
  Json weapon_cosmetics_;
  Json glove_cosmetics_;
  std::string cosmetic_player_;
  int cosmetic_category_ = 0, cosmetic_agent_ = 0;
  int cosmetic_seen_revision_ = -1;
  std::string cosmetic_draft_target_;
  char cosmetic_filter_[128] = {};
  int cosmetic_weapon_ = 0, cosmetic_paint_ = 0, cosmetic_seed_ = 0;
  float cosmetic_wear_ = .01f;
  std::string weapon_draft_target_, weapon_draft_native_;
  char paint_filter_[128] = {};
  int glove_item_ = 0, glove_paint_ = 0, glove_seed_ = 0;
  float glove_wear_ = .01f;
  std::string glove_draft_target_, glove_draft_native_;
  bool glove_draft_dirty_ = false;
  char glove_filter_[128] = {};
  bool palette_enabled_ = false;
  std::array<float, 3> palette_t_{{1, .25f, .25f}}, palette_ct_{{.25f, .5f, 1}};
  char appearance_path_[4096] = {};
  char appearance_models_[4096] = {};
  std::unordered_map<std::string, double> scene_drafts_;
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
  static double ExactNumber(const Json& value, double fallback = 0) {
    if (!value.is_number()) return fallback;
    const double number = value.get<double>();
    return std::isfinite(number) ? number : fallback;
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
    general_panel_ = false;
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
    scene_capabilities_ = Member(state, "scene_capabilities");
    presentation_ = Member(state, "presentation");
    appearance_ = Member(state, "appearance");
    appearance_capabilities_ = Member(state, "appearance_capabilities");
    cosmetics_ = Member(state, "cosmetics");
    weapon_cosmetics_ = Member(state, "weapon_cosmetics");
    glove_cosmetics_ = Member(state, "glove_cosmetics");
    camera_ = ReadKey(Member(state, "camera"));
    const Json& project = Member(state, "project");
    project_name_ = String(Member(project, "name"), "Untitled shot");
    interpolation_ = String(Member(project, "interpolation"), "smooth");
    exact_duration_ = std::max(.01, ExactNumber(Member(project, "duration"), 8));
    duration_ = static_cast<float>(exact_duration_);
    start_tick_ = Integer(Member(project, "start_tick"));
    const Json& range = Member(project, "render_range");
    render_region_ = String(Member(range, "mode"), "composition") == "region";
    range_in_ = std::clamp(ExactNumber(Member(range, "in")), 0.0, exact_duration_);
    range_out_ = std::clamp(ExactNumber(Member(range, "out"), exact_duration_), range_in_, exact_duration_);
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
      draft_range_in_ = range_in_;
      draft_range_out_ = range_out_;
      if (scene_capabilities_.is_object()) {
        for (auto entry = scene_capabilities_.begin(); entry != scene_capabilities_.end(); ++entry)
          scene_drafts_[entry.key()] = ExactNumber(Member(entry.value(), "value"));
      }
      const Json& palette = Member(cosmetics_, "team_palette");
      palette_enabled_ = Boolean(Member(palette, "enabled"));
      const auto t = Array<3>(Member(palette, "t"), {{255, 64, 64}});
      const auto ct = Array<3>(Member(palette, "ct"), {{64, 128, 255}});
      for (size_t i = 0; i < 3; ++i) {
        palette_t_[i] = Clamp(t[i] / 255.f, 0, 1);
        palette_ct_[i] = Clamp(ct[i] / 255.f, 0, 1);
      }
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
      // Patch only the edited property: the roaming camera may have moved
      // since numeric editing began, and a lens edit must not move it back.
      Emit(send, "set_camera", {{field, value}});
    }
  }
  double RangeSpan() const { return std::min(.01, std::max(1e-12, range_out_ - range_in_)); }
  void SetRangeBoundary(const Send& send, bool end) {
    const double span = RangeSpan();
    const double time = std::clamp(static_cast<double>(cursor_), end ? span : 0.0,
                                   end ? exact_duration_ : exact_duration_ - span);
    Json range = {{"mode", "region"}};
    if (end) {
      range["out"] = time;
      range["in"] = std::min(range_in_, time - span);
    } else {
      range["in"] = time;
      range["out"] = std::max(range_out_, time + span);
    }
    Emit(send, "set_project", {{"render_range", range}});
  }
  void DrawRenderScope(const Send& send, float width) {
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##render_scope", render_region_ ? "Render region" : "Whole composition")) {
      DrawBackdrop();
      if (ImGui::Selectable("Whole composition", !render_region_) && render_region_)
        Emit(send, "set_project", {{"render_range", {{"mode", "composition"}}}});
      if (ImGui::Selectable("Render region", render_region_) && !render_region_)
        Emit(send, "set_project", {{"render_range", {{"mode", "region"}}}});
      ImGui::EndCombo();
    }
    Tip("Preview and render the whole composition or the saved In/Out region. Switching keeps both boundaries and all camera keys.");
  }
  void DrawRenderRange(const Send& send) {
    Caption("Preview / render range");
    DrawRenderScope(send, -1);
    const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
    const float second_column = ImGui::GetCursorPosX() + half + S(7);
    Caption("In");
    ImGui::SameLine(second_column);
    Caption("Out");
    ImGui::SetNextItemWidth(half);
    if (NumericField("##range_in", &draft_range_in_, 0, range_out_ - RangeSpan(), "%.3f s"))
      Emit(send, "set_project", {{"render_range", {{"in", draft_range_in_}}}});
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    if (NumericField("##range_out", &draft_range_out_, range_in_ + RangeSpan(), exact_duration_, "%.3f s"))
      Emit(send, "set_project", {{"render_range", {{"out", draft_range_out_}}}});
    if (ImGui::Button("Set In  [I]", ImVec2(half, 0))) SetRangeBoundary(send, false);
    ImGui::SameLine();
    if (ImGui::Button("Set Out  [O]", ImVec2(half, 0))) SetRangeBoundary(send, true);
    if (ImGui::Button("Reset region", ImVec2(-1, 0)))
      Emit(send, "set_project", {{"render_range", {{"in", 0}, {"out", exact_duration_}}}});
    Tip("Restore In/Out to the full duration. Ctrl+Z restores the previous region.");
  }
  void DrawSceneSettings(const Send& send) {
    if (!scene_capabilities_.is_object() || scene_capabilities_.empty()) return;
    if (!ImGui::CollapsingHeader("Scene settings")) return;
    ImGui::BeginDisabled(!demo_);
    if (ImGui::Button("Clean frame", ImVec2(-1, 0))) {
      Json settings = Json::object();
      for (const char* id : {"hud_visible", "viewmodel_visible", "fps_counter_visible",
                              "trueview_status_visible", "telemetry_frametime_visible",
                              "telemetry_network_visible", "telemetry_ping_visible",
                              "telemetry_margin_visible", "build_info_visible"})
        if (Boolean(Member(Member(scene_capabilities_, id), "available"))) settings[id] = false;
      if (!settings.empty()) Emit(send, "set_scene", {{"settings", settings}});
      visible = false;
    }
    ImGui::EndDisabled();
    Tip("Hide the game HUD, viewmodel and editor. F8 brings the editor back; Reset scene settings restores the game display.");
    const auto group_for = [&](const std::string& id, const Json& capability) {
      if (id == "hud_visible" && presentation_.is_object()) return std::string();
      if (id == "fps_counter_visible" || id == "trueview_status_visible" ||
          id == "build_info_visible" || id.rfind("telemetry_", 0) == 0) return std::string("Diagnostics");
      return String(Member(capability, "group"));
    };
    const char* groups[] = {"Lens", "Lighting", "Post processing", "Environment", "Diagnostics"};
    for (const char* group : groups) {
      bool present = false;
      for (auto item = scene_capabilities_.begin(); item != scene_capabilities_.end(); ++item)
        if (group_for(item.key(), item.value()) == group && Boolean(Member(item.value(), "available"))) present = true;
      if (!present) continue;
      if (!ImGui::TreeNode(group)) continue;
      for (auto entry = scene_capabilities_.begin(); entry != scene_capabilities_.end(); ++entry) {
        const Json& capability = entry.value();
        if (group_for(entry.key(), capability) != group || !Boolean(Member(capability, "available"))) continue;
        const bool available = Boolean(Member(capability, "available"));
        const std::string label = String(Member(capability, "label"), entry.key().c_str());
        ImGui::PushID(entry.key().c_str());
        ImGui::BeginDisabled(!demo_ || !available);
        if (String(Member(capability, "type")) == "boolean") {
          bool enabled = Boolean(Member(capability, "value"));
          if (ImGui::Checkbox(label.c_str(), &enabled))
            Emit(send, "set_scene", {{"settings", {{entry.key(), enabled}}}});
        } else {
          Caption(label.c_str());
          ImGui::SetNextItemWidth(-1);
          double& value = scene_drafts_[entry.key()];
          const double minimum = ExactNumber(Member(capability, "minimum"), -100000);
          const double maximum = ExactNumber(Member(capability, "maximum"), 100000);
          if (NumericField("##scene_value", &value, minimum, maximum, "%.3f"))
            Emit(send, "set_scene", {{"settings", {{entry.key(), value}}}});
        }
        ImGui::EndDisabled();
        const std::string help = String(Member(capability, available ? "help" : "reason"));
        if (!help.empty()) Tip(help.c_str());
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
    DrawTeamPalette(send);
    if (ImGui::Button("Reset scene settings", ImVec2(-1, 0))) Emit(send, "reset_scene");
    Tip("Restore the engine values from the start of this session. Ctrl+Z restores project overrides.");
    ImGui::Separator();
  }
  void DrawPresentation(const Send& send) {
    if (!presentation_.is_object()) return;
    const Json& capabilities = Member(presentation_, "capabilities");
    const std::string actual_view = String(Member(presentation_, "view"));
    std::string view = String(Member(presentation_, "requested_view"), actual_view.c_str());
    if (view != "player" && view != "free") view = actual_view;
    const std::string actual_hud = String(Member(presentation_, "hud"));
    std::string hud = String(Member(presentation_, "requested_hud"), actual_hud.c_str());
    if (hud != "gameplay" && hud != "demo" && hud != "hidden") hud = "demo";
    auto requestable = [&](const char* id) {
      const auto& capability = Member(capabilities, id);
      return Boolean(Member(capability, "requestable"), Boolean(Member(capability, "available")));
    };
    auto option = [&](const char* id, const char* label, bool selected, const Json& args) {
      ImGui::BeginDisabled(!demo_ || !requestable(id));
      if (ImGui::Selectable(label, selected)) Emit(send, "set_presentation", args);
      ImGui::EndDisabled();
      const auto reason = String(Member(Member(capabilities, id), "reason"));
      if (!requestable(id) && !reason.empty()) Tip(reason.c_str());
    };
    Caption("View");
    ImGui::SetNextItemWidth(-1);
    const char* view_name = view == "player" ? "First person" : view == "free" ? "Free camera" : "Current game camera";
    if (ImGui::BeginCombo("##presentation_view", view_name)) {
      DrawBackdrop();
      option("player", "First person", view == "player", {{"view", "player"}});
      Json free = {{"view", "free"}};
      if (hud == "gameplay") free["hud"] = "demo";
      option("free", "Free camera", view == "free", free);
      ImGui::EndCombo();
    }
    Caption("HUD");
    ImGui::SetNextItemWidth(-1);
    const char* hud_name = hud == "hidden" ? "Hidden" : hud == "gameplay" ? "As playing" : view == "free" ? "Shown" : "Demo spectator";
    if (ImGui::BeginCombo("##presentation_hud", hud_name)) {
      DrawBackdrop();
      if (view == "player") option("gameplay", "As playing", hud == "gameplay", {{"hud", "gameplay"}});
      option("demo", view == "free" ? "Shown" : "Demo spectator", hud == "demo", {{"hud", "demo"}});
      option("hidden", "Hidden", hud == "hidden", {{"hud", "hidden"}});
      ImGui::EndCombo();
    }
    if (Boolean(Member(presentation_, "pending"))) Caption("Applying view / HUD...");
    const auto error = String(Member(presentation_, "error"));
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    ImGui::Separator();
  }
  void DrawTeamPalette(const Send& send) {
    if (!cosmetics_.is_object() || !ImGui::TreeNode("Team palette")) return;
    const Json& capability = Member(Member(cosmetics_, "capabilities"), "team_palette");
    const bool available = Boolean(Member(capability, "available"));
    ImGui::BeginDisabled(!demo_ || !available);
    if (ImGui::Checkbox("Override player colors", &palette_enabled_))
      Emit(send, "set_team_palette", {{"enabled", palette_enabled_}});
    auto color = [&](const char* label, std::array<float, 3>& value, const char* field) {
      ImGui::SetNextItemWidth(-1);
      ImGui::ColorEdit3(label, value.data(), ImGuiColorEditFlags_NoAlpha |
                                              ImGuiColorEditFlags_NoOptions);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        Json rgb = Json::array();
        for (float channel : value) rgb.push_back(static_cast<int>(std::round(Clamp(channel, 0, 1) * 255)));
        Emit(send, "set_team_palette", {{field, rgb}});
      }
    };
    color("T", palette_t_, "t");
    color("CT", palette_ct_, "ct");
    if (ImGui::Button("Reset player colors", ImVec2(-1, 0))) Emit(send, "reset_team_palette");
    ImGui::EndDisabled();
    const std::string reason = String(Member(capability, "reason"));
    if (!available && !reason.empty()) ImGui::TextWrapped("%s", reason.c_str());
    ImGui::TreePop();
  }
  static std::string PlayerLabel(const Json& player) {
    const std::string id = String(Member(player, "steam_id"));
    const int team = Integer(Member(player, "team"));
    return String(Member(player, "nickname"), "Unknown player") + " [" +
        (team == 2 ? "T" : team == 3 ? "CT" : "Observer") + "] #" +
        (id.size() > 5 ? id.substr(id.size() - 5) : id);
  }
  void DrawWeaponPaint(const Send& send, bool knife, bool target_ready) {
    const Json& definitions = Member(cosmetics_catalog, "weapons");
    const Json& paints = Member(cosmetics_catalog, "paint_kits");
    const Json& rows = Member(weapon_cosmetics_, "weapons");
    const Json& capability = Member(weapon_cosmetics_, "capability");
    const bool available = Boolean(Member(capability, "available"));
    const Json* selected = nullptr;
    const Json* definition = nullptr;
    auto find_definition = [&](int id) -> const Json* {
      if (definitions.is_array()) for (const auto& item : definitions)
        if (Integer(Member(item, "item_def")) == id &&
            (String(Member(item, "kind")) == "knife") == knife) return &item;
      return nullptr;
    };
    std::vector<const Json*> owned;
    if (rows.is_array()) for (const auto& row : rows) {
      if (String(Member(row, "steam_id")) != cosmetic_player_) continue;
      const int id = Integer(Member(row, "item_def"));
      if (!find_definition(id)) continue;
      owned.push_back(&row);
      if (id == cosmetic_weapon_) selected = &row;
    }
    if (!selected && !owned.empty()) {
      selected = owned.front();
      for (const Json* row : owned) if (Boolean(Member(*row, "active"))) selected = row;
      cosmetic_weapon_ = Integer(Member(*selected, "item_def"));
    }
    if (selected) definition = find_definition(cosmetic_weapon_);
    Caption(knife ? "Owned knife" : "Owned weapon");
    ImGui::SetNextItemWidth(-1);
    const auto name = definition ? String(Member(*definition, "display_name")) : "No matching owned item";
    if (ImGui::BeginCombo("##cosmetic_weapon", name.c_str())) {
      DrawBackdrop();
      for (const Json* row : owned) {
        const int id = Integer(Member(*row, "item_def"));
        const Json* item = find_definition(id);
        ImGui::PushID(id);
        const auto label = String(Member(*item, "display_name"));
        if (ImGui::Selectable(label.c_str(), id == cosmetic_weapon_)) cosmetic_weapon_ = id;
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if (!selected || !definition) {
      ImGui::TextWrapped("Select a player who owns this category of item at the current demo tick.");
      return;
    }
    const std::string target = cosmetic_player_ + ":" + std::to_string(cosmetic_weapon_);
    const std::string native = Member(*selected, "paint_kit").dump() + ":" +
        Member(*selected, "seed").dump() + ":" + Member(*selected, "wear").dump();
    if (target != weapon_draft_target_ || native != weapon_draft_native_) {
      cosmetic_paint_ = Integer(Member(*selected, "paint_kit"));
      cosmetic_seed_ = Integer(Member(*selected, "seed"));
      cosmetic_wear_ = Number(Member(*selected, "wear"), .01f);
      weapon_draft_target_ = target;
      weapon_draft_native_ = native;
    }
    const Json* paint = nullptr;
    if (paints.is_array()) for (const auto& item : paints)
      if (Integer(Member(item, "id")) == cosmetic_paint_) paint = &item;
    const Json& allowed = Member(*definition, "paint_kits");
    Caption("Finish from game");
    ImGui::SetNextItemWidth(-1);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::numeric_limits<float>::max(), S(360)));
    const auto paint_name = paint ? String(Member(*paint, "display_name")) : "Original finish";
    if (ImGui::BeginCombo("##cosmetic_finish", paint_name.c_str())) {
      DrawBackdrop();
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##paint_filter", "Find finish...", paint_filter_, sizeof(paint_filter_));
      if (ImGui::Selectable("Original finish", cosmetic_paint_ == 0)) cosmetic_paint_ = 0;
      ImGuiTextFilter filter(paint_filter_);
      if (paints.is_array() && allowed.is_array()) for (const auto& item : paints) {
        const int id = Integer(Member(item, "id"));
        if (std::find(allowed.begin(), allowed.end(), Json(id)) == allowed.end()) continue;
        const auto label = String(Member(item, "display_name"));
        if (!filter.PassFilter(label.c_str())) continue;
        ImGui::PushID(id);
        if (ImGui::Selectable(label.c_str(), cosmetic_paint_ == id)) {
          cosmetic_paint_ = id;
          cosmetic_wear_ = Clamp(cosmetic_wear_, Number(Member(item, "wear_min")), Number(Member(item, "wear_max"), 1));
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
    const float second = ImGui::GetCursorPosX() + half + S(7);
    Caption("Wear");ImGui::SameLine(second);Caption("Pattern seed");
    ImGui::SetNextItemWidth(half);
    NumericField("##cosmetic_wear", &cosmetic_wear_, paint ? Number(Member(*paint, "wear_min")) : 0,
                 paint ? Number(Member(*paint, "wear_max"), 1) : 1, "%.4f");
    ImGui::SameLine();ImGui::SetNextItemWidth(half);
    NumericField("##cosmetic_seed", &cosmetic_seed_, 0, 1000);
    ImGui::BeginDisabled(!demo_ || !target_ready || !available ||
                        Boolean(Member(weapon_cosmetics_, "stale")) || !Boolean(Member(*selected, "ready")));
    if (ImGui::Button("Apply finish", ImVec2(half, 0))) {
      if (cosmetic_paint_ == 0) Emit(send, "reset_player_weapon_paint", {{"steam_id", cosmetic_player_}, {"item_def", cosmetic_weapon_}});
      else Emit(send, "set_player_weapon_paint", {{"steam_id", cosmetic_player_}, {"item_def", cosmetic_weapon_},
                         {"paint_kit", cosmetic_paint_}, {"wear", cosmetic_wear_}, {"seed", cosmetic_seed_}});
    }
    ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!demo_ || !target_ready || !available);
    if (ImGui::Button("Restore finish", ImVec2(half, 0)))
      Emit(send, "reset_player_weapon_paint", {{"steam_id", cosmetic_player_}, {"item_def", cosmetic_weapon_}});
    ImGui::EndDisabled();
    const auto error = String(Member(*selected, "error"));
    const auto reason = String(Member(*selected, "reason"));
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    else if (!Boolean(Member(*selected, "ready")) && !reason.empty()) ImGui::TextWrapped("%s", reason.c_str());
    else if (Boolean(Member(*selected, "applied"))) Caption("Finish attributes and material refresh confirmed");
    Tip("Applies a finish to this player's owned item. Switching the weapon model is a separate operation.");
  }
  void DrawGloves(const Send& send, int team, bool target_ready) {
    const Json& definitions = Member(cosmetics_catalog, "gloves");
    const Json& paints = Member(cosmetics_catalog, "paint_kits");
    const Json& rows = Member(glove_cosmetics_, "gloves");
    const Json* row = nullptr;
    if (rows.is_array()) for (const auto& candidate : rows)
      if (String(Member(candidate, "steam_id")) == cosmetic_player_) row = &candidate;
    const std::string target = cosmetic_player_ + ":" + std::to_string(team);
    const std::string native = row ? Member(*row, "requested_item_def").dump() + ":" +
        Member(*row, "paint_kit").dump() + ":" + Member(*row, "seed").dump() + ":" + Member(*row, "wear").dump() : "unavailable";
    const bool acknowledgement_pending = revision_ == pending_revision_ && ImGui::GetTime() < pending_until_;
    if (target != glove_draft_target_ || (row && native != glove_draft_native_ &&
        !glove_draft_dirty_ && !was_item_active_ && !acknowledgement_pending)) {
      glove_item_ = row ? Integer(Member(*row, "requested_item_def")) : 0;
      glove_paint_ = row ? Integer(Member(*row, "paint_kit")) : 0;
      glove_seed_ = row ? Integer(Member(*row, "seed")) : 0;
      glove_wear_ = row ? Number(Member(*row, "wear"), .01f) : .01f;
      glove_draft_target_ = target;
      glove_draft_native_ = native;
      glove_draft_dirty_ = false;
    }
    auto compatible = [&](const Json& item) {
      const Json& teams = Member(item, "allowed_teams");
      const Json& kits = Member(item, "paint_kits");
      return teams.is_array() && std::find(teams.begin(), teams.end(), Json(team)) != teams.end() &&
          kits.is_array() && !kits.empty();
    };
    auto definition = [&]() -> const Json* {
      if (definitions.is_array()) for (const auto& item : definitions)
        if (Integer(Member(item, "item_def")) == glove_item_ && compatible(item)) return &item;
      return nullptr;
    };
    const Json* item = definition();
    Caption("Gloves from game");
    ImGui::SetNextItemWidth(-1);
    const std::string name = item ? String(Member(*item, "display_name")) : "Original gloves";
    if (ImGui::BeginCombo("##glove_item", name.c_str())) {
      DrawBackdrop();
      if (ImGui::Selectable("Original gloves", glove_item_ == 0)) { glove_item_ = 0; glove_paint_ = 0; glove_draft_dirty_ = true; }
      if (definitions.is_array()) for (const auto& candidate : definitions) {
        if (!compatible(candidate)) continue;
        const int id = Integer(Member(candidate, "item_def"));
        ImGui::PushID(id);
        const auto label = String(Member(candidate, "display_name"));
        if (ImGui::Selectable(label.c_str(), id == glove_item_)) {
          glove_item_ = id;
          glove_draft_dirty_ = true;
          const Json& kits = Member(candidate, "paint_kits");
          if (std::find(kits.begin(), kits.end(), Json(glove_paint_)) == kits.end()) glove_paint_ = Integer(kits.front());
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    item = definition();
    auto paint_definition = [&]() -> const Json* {
      if (paints.is_array()) for (const auto& paint : paints)
        if (Integer(Member(paint, "id")) == glove_paint_ && String(Member(paint, "category")) == "glove") return &paint;
      return nullptr;
    };
    if (item) {
      const Json& kits = Member(*item, "paint_kits");
      const Json* paint = paint_definition();
      Caption("Glove finish");
      ImGui::SetNextItemWidth(-1);
      ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::numeric_limits<float>::max(), S(360)));
      const auto label = paint ? String(Member(*paint, "display_name")) : "Choose finish";
      if (ImGui::BeginCombo("##glove_finish", label.c_str())) {
        DrawBackdrop();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##glove_filter", "Find finish...", glove_filter_, sizeof(glove_filter_));
        ImGuiTextFilter filter(glove_filter_);
        if (paints.is_array()) for (const auto& candidate : paints) {
          const int id = Integer(Member(candidate, "id"));
          if (String(Member(candidate, "category")) != "glove" || std::find(kits.begin(), kits.end(), Json(id)) == kits.end()) continue;
          const auto finish_name = String(Member(candidate, "display_name"));
          if (!filter.PassFilter(finish_name.c_str())) continue;
          ImGui::PushID(id);
          if (ImGui::Selectable(finish_name.c_str(), glove_paint_ == id)) { glove_paint_ = id; glove_draft_dirty_ = true; }
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      paint = paint_definition();
      const float minimum = paint ? Number(Member(*paint, "wear_min")) : 0;
      const float maximum = paint ? Number(Member(*paint, "wear_max"), 1) : 1;
      glove_wear_ = Clamp(glove_wear_, minimum, maximum);
      const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
      const float second = ImGui::GetCursorPosX() + half + S(7);
      Caption("Wear");ImGui::SameLine(second);Caption("Pattern seed");
      ImGui::SetNextItemWidth(half);
      if (NumericField("##glove_wear", &glove_wear_, minimum, maximum, "%.4f")) glove_draft_dirty_ = true;
      ImGui::SameLine();ImGui::SetNextItemWidth(half);
      if (NumericField("##glove_seed", &glove_seed_, 0, 1000)) glove_draft_dirty_ = true;
    }
    const bool available = Boolean(Member(Member(glove_cosmetics_, "capability"), "available"));
    const bool current = target_ready && !Boolean(Member(glove_cosmetics_, "stale"));
    const bool ready = row && Boolean(Member(*row, "ready")) && Boolean(Member(*row, "item_readable")) &&
        !Boolean(Member(*row, "restoration_unresolved"));
    const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
    ImGui::BeginDisabled(!demo_ || !available || !current || !ready || (glove_item_ != 0 && (!item || !paint_definition())));
    if (ImGui::Button("Apply gloves", ImVec2(half, 0))) {
      glove_draft_dirty_ = false;
      if (!glove_item_) Emit(send, "reset_player_gloves", {{"steam_id", cosmetic_player_}});
      else Emit(send, "set_player_gloves", {{"steam_id", cosmetic_player_}, {"item_def", glove_item_},
                                        {"paint_kit", glove_paint_}, {"wear", glove_wear_}, {"seed", glove_seed_}});
    }
    ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!demo_ || !available || !target_ready);
    if (ImGui::Button("Restore gloves", ImVec2(half, 0))) { glove_draft_dirty_ = false; Emit(send, "reset_player_gloves", {{"steam_id", cosmetic_player_}}); }
    ImGui::EndDisabled();
    if (row) {
      const auto error = String(Member(*row, "error"));
      const auto reason = String(Member(*row, "reason"));
      if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
      else if (!ready && !reason.empty()) ImGui::TextWrapped("%s", reason.c_str());
      else if (Integer(Member(*row, "requested_item_def")) && Boolean(Member(*row, "materials_pending"))) {
        if (Member(*row, "world_pose_ready").is_boolean() && !Boolean(Member(*row, "world_pose_ready")))
          Caption("Updating glove pose...");
        else if (Member(*row, "world_render_ready").is_boolean() &&
                 (!Boolean(Member(*row, "world_render_ready")) || !Boolean(Member(*row, "hud_render_ready"))))
          Caption("Updating glove visibility...");
        else Caption("Preparing glove materials...");
      }
      else if (Boolean(Member(*row, "applied"))) Caption("Glove item and material refresh confirmed");
    } else Caption("Waiting for the player's glove item");
  }
  void DrawCosmetics(const Send& send) {
    if (!cosmetics_.is_object() && !cosmetics_catalog.is_object()) return;
    if (!ImGui::CollapsingHeader("Models and skins")) return;
    const Json& players = Member(cosmetics_, "players");
    const Json* selected = nullptr;
    if (players.is_array()) {
      for (const auto& player : players) {
        const std::string id = String(Member(player, "steam_id"));
        if (id.empty() || id == "0") continue;
        if (cosmetic_player_.empty()) cosmetic_player_ = id;
        if (id == cosmetic_player_) selected = &player;
      }
    }
    Caption("Player");
    ImGui::SetNextItemWidth(-1);
    const std::string preview = selected ? PlayerLabel(*selected) :
        cosmetic_player_.empty() ? "Waiting for players" : "Selected player unavailable";
    if (ImGui::BeginCombo("##cosmetic_player", preview.c_str())) {
      DrawBackdrop();
      if (players.is_array()) for (const auto& player : players) {
        const std::string id = String(Member(player, "steam_id"));
        if (id.empty() || id == "0") continue;
        const std::string label = PlayerLabel(player);
        ImGui::PushID(id.c_str());
        if (ImGui::Selectable(label.c_str(), id == cosmetic_player_)) {
          cosmetic_player_ = id;
          cosmetic_draft_target_.clear();
          cosmetic_filter_[0] = 0;
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    const char* categories[] = {"Agents", "Weapons", "Knives", "Gloves"};
    const char* ids[] = {"agents", "weapons", "knives", "gloves"};
    Caption("Category");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##cosmetic_category", categories[cosmetic_category_])) {
      DrawBackdrop();
      for (int i = 0; i < 4; ++i)
        if (ImGui::Selectable(categories[i], cosmetic_category_ == i)) cosmetic_category_ = i;
      ImGui::EndCombo();
    }
    const Json& capability = (cosmetic_category_ == 1 || cosmetic_category_ == 2) && weapon_cosmetics_.is_object()
        ? Member(weapon_cosmetics_, "capability") : cosmetic_category_ == 3 && glove_cosmetics_.is_object()
        ? Member(glove_cosmetics_, "capability") : Member(Member(cosmetics_, "capabilities"), ids[cosmetic_category_]);
    const bool available = Boolean(Member(capability, "available"));
    const int team = selected ? Integer(Member(*selected, "team")) : 0;
    const bool target_ready = selected && (team == 2 || team == 3) &&
        !Boolean(Member(cosmetics_, "stale"));
    if (cosmetic_category_ == 0) {
      const std::string target = cosmetic_player_ + ":" + std::to_string(team);
      if (target != cosmetic_draft_target_ ||
          (revision_ != cosmetic_seen_revision_ && !was_item_active_ &&
           (revision_ != pending_revision_ || ImGui::GetTime() >= pending_until_))) {
        cosmetic_agent_ = selected ? Integer(Member(*selected, "agent_item_def")) : 0;
        cosmetic_draft_target_ = target;
        cosmetic_seen_revision_ = revision_;
      }
      std::string name = "Current map model";
      const Json& agents = Member(cosmetics_catalog, "agents");
      if (agents.is_array()) for (const auto& agent : agents)
        if (Integer(Member(agent, "item_def")) == cosmetic_agent_)
          name = String(Member(agent, "display_name"), "Unknown agent");
      Caption(team == 2 ? "T agents from game" : team == 3 ? "CT agents from game" : "Agents from game");
      ImGui::BeginDisabled(!target_ready || !agents.is_array());
      ImGui::SetNextItemWidth(-1);
      ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::numeric_limits<float>::max(), S(360)));
      if (ImGui::BeginCombo("##cosmetic_agent", name.c_str())) {
        DrawBackdrop();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##agent_filter", "Find agent...", cosmetic_filter_, sizeof(cosmetic_filter_));
        if (ImGui::Selectable("Current map model", cosmetic_agent_ == 0)) cosmetic_agent_ = 0;
        ImGuiTextFilter filter(cosmetic_filter_);
        if (agents.is_array()) for (const auto& agent : agents) {
          const auto& teams = Member(agent, "allowed_teams");
          if (!teams.is_array() || teams.size() != 1 || Integer(teams[0]) != team ||
              Boolean(Member(agent, "map_based")) || String(Member(agent, "model")).empty()) continue;
          const std::string label = String(Member(agent, "display_name"));
          if (!filter.PassFilter(label.c_str())) continue;
          const int item = Integer(Member(agent, "item_def"));
          ImGui::PushID(item);
          if (ImGui::Selectable(label.c_str(), cosmetic_agent_ == item)) cosmetic_agent_ = item;
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
      const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
      const bool model_ready = selected && Boolean(Member(*selected, "model_ready"));
      ImGui::BeginDisabled(!demo_ || !target_ready || !available || !model_ready);
      if (ImGui::Button("Apply to player", ImVec2(half, 0))) {
        if (cosmetic_agent_ == 0) Emit(send, "reset_player_cosmetics", {{"steam_id", cosmetic_player_}});
        else Emit(send, "set_player_cosmetics", {{"steam_id", cosmetic_player_}, {"agent_item_def", cosmetic_agent_}});
      }
      ImGui::SameLine();
      if (ImGui::Button("Map default", ImVec2(half, 0))) {
        cosmetic_agent_ = 0;
        Emit(send, "reset_player_cosmetics", {{"steam_id", cosmetic_player_}});
      }
      ImGui::EndDisabled();
      if (target_ready && available && !model_ready) Caption("Waiting for the player's model binding");
    }
    if (cosmetic_category_ == 1 || cosmetic_category_ == 2) DrawWeaponPaint(send, cosmetic_category_ == 2, target_ready);
    if (cosmetic_category_ == 3) DrawGloves(send, team, target_ready);
    if (!available) {
      std::string reason = String(Member(capability, "reason"));
      if (reason.empty()) reason = "Live replacement is not available for this category on the current game build.";
      ImGui::TextWrapped("%s", reason.c_str());
    }
    if (selected) {
      const auto error = String(Member(*selected, "error"));
      if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
      else if (Boolean(Member(*selected, "applied"))) Caption("Player override active");
    }
    if (!cosmetics_catalog_error.empty()) ImGui::TextWrapped("%s", cosmetics_catalog_error.c_str());
    ImGui::BeginDisabled(true);
    ImGui::Button("Import...", ImVec2(-1, 0));
    ImGui::EndDisabled();
    Tip("Custom compiled Source 2 models require a compatible per-player resource adapter. The installed-game catalog is the primary source.");
    DrawAppearance(send);
    ImGui::Separator();
  }
  void DrawAppearance(const Send& send) {
    if (!appearance_capabilities_.is_object()) return;
    if (!ImGui::TreeNode("Advanced resource diagnostics")) return;
    ImGui::TextWrapped("Compiled Source 2 appearance pack. Replaces matching model resources throughout the scene.");
    Caption("Pack directory");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##appearance_path", appearance_path_, sizeof(appearance_path_));
    Caption("Model resources (.vmdl), one per line");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##appearance_models", appearance_models_, sizeof(appearance_models_), ImVec2(-1, S(76)));
    const bool available = Boolean(Member(appearance_capabilities_, "available"));
    ImGui::BeginDisabled(!demo_ || !available);
    const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
    if (ImGui::Button("Apply pack", ImVec2(half, 0))) {
      Json models = Json::array();
      std::string lines(appearance_models_);
      size_t begin = 0;
      while (begin < lines.size()) {
        const auto end = lines.find('\n', begin);
        std::string value = lines.substr(begin, end == std::string::npos ? end : end - begin);
        const auto first = value.find_first_not_of(" \t\r");
        const auto last = value.find_last_not_of(" \t\r");
        if (first != std::string::npos) models.push_back(value.substr(first, last - first + 1));
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      Emit(send, "apply_appearance_pack", {{"path", std::string(appearance_path_)}, {"models", models}});
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset appearance", ImVec2(half, 0))) Emit(send, "reset_appearance");
    ImGui::EndDisabled();
    if (!available) ImGui::TextWrapped("Appearance replacement is unavailable on this game build.");
    if (Boolean(Member(appearance_, "active"))) {
      const auto path = String(Member(appearance_, "path"));
      ImGui::TextWrapped("Active: %s", path.c_str());
      ImGui::TextWrapped("Resource pack mounted. This is a global resource override.");
    }
    const auto error = String(Member(appearance_, "error"));
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    Tip("An AI client can list installed models and prepare a pack with the asset MCP tools. Custom files must already be compiled for Source 2.");
    ImGui::TreePop();
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
    else if constexpr (std::is_same<T, double>::value)
      ImGui::InputDouble(label, value, 0, 0, format, ImGuiInputTextFlags_AutoSelectAll);
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
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
    const float panel_width = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
    if (general_panel_) AccentButton("General", ImVec2(panel_width, 0));
    else if (ImGui::Button("General", ImVec2(panel_width, 0))) general_panel_ = true;
    ImGui::SameLine();
    if (!general_panel_) AccentButton("Camera keys", ImVec2(panel_width, 0));
    else if (ImGui::Button("Camera keys", ImVec2(panel_width, 0))) general_panel_ = false;
    ImGui::Spacing();
    if (general_panel_) {
      DrawPresentation(send);
      DrawSceneSettings(send);
      DrawCosmetics(send);
    } else {
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
      DrawRenderRange(send);
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
    }
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
      row("Set render region In / Out", "I / O");
      row("Remove selected key", "Delete");
      row("Undo", "Ctrl+Z");
      row("Redo", "Ctrl+Shift+Z / Ctrl+Y");
      row("Save project", "Ctrl+S");
      row("Load project", "Ctrl+O");
      row("Export video", "Ctrl+E");
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
    Caption("Field of view / FOV");
    if (key_mode) {
      ImGui::SameLine(second_column);
      Caption("Time");
    }
    ImGui::SetNextItemWidth(width);
    if (NumericField("##fov", &draft_.fov, 1, 179, "%.1f deg"))
      CommitPose(send, "fov", draft_.fov);
    Tip(key_mode ? "Lens angle at this camera key. Different FOV values animate the lens between keys." :
                   "Change the current camera lens without adding a key. Capture a key to save this FOV in the path.");
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
    if (ImGui::MenuItem("Export video...", "Ctrl+E", false, demo_ && !keys_.empty()))
      Emit(send, "request_export");
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
    ImGui::SameLine(0, S(14));
    DrawRenderScope(send, S(210));
    ImGui::SameLine(0, S(14));
    ImGui::BeginDisabled(!demo_ || keys_.empty());
    if (ToolbarButton("Export", ImVec2(S(100), S(36)))) Emit(send, "request_export");
    Tip("Open the application's export window. Ctrl+E");
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
    const float region_in = range_drag_handle_ == 0 ? range_drag_time_ : range_in_;
    const float region_out = range_drag_handle_ == 1 ? range_drag_time_ : range_out_;
    if (render_region_ || range_drag_handle_ >= 0) {
      draw->AddRectFilled(ImVec2(left, pos.y), ImVec2(tx(region_in), track_bottom),
                          IM_COL32(0, 0, 0, 100));
      draw->AddRectFilled(ImVec2(tx(region_out), pos.y), ImVec2(right, track_bottom),
                          IM_COL32(0, 0, 0, 100));
      draw->AddRectFilled(ImVec2(tx(region_in), track_top),
                          ImVec2(tx(region_out), track_bottom), Ink(.16f));
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
    int hovered_range = -1;
    if (hovered && ImGui::GetIO().MousePos.y >= track_top - S(2)) {
      const float in_distance = std::abs(ImGui::GetIO().MousePos.x - tx(region_in));
      const float out_distance = std::abs(ImGui::GetIO().MousePos.x - tx(region_out));
      if (std::min(in_distance, out_distance) <= S(11))
        hovered_range = in_distance <= out_distance ? 0 : 1;
    }
    if (hovered_range >= 0 && connected_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      range_drag_handle_ = hovered_range;
      range_drag_initial_ = range_drag_time_ = hovered_range ? range_out_ : range_in_;
      range_drag_start_x_ = ImGui::GetIO().MousePos.x;
      range_drag_moved_ = false;
      timeline_active_ = false;
      timeline_drag_id_.clear();
    }
    if (range_drag_handle_ >= 0) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        range_drag_handle_ = -1;
      } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float delta = ImGui::GetIO().MousePos.x - range_drag_start_x_;
        if (std::abs(delta) > S(2)) range_drag_moved_ = true;
        if (range_drag_moved_) {
          double time = range_drag_initial_ + delta / width * exact_duration_;
          if (!ImGui::GetIO().KeyShift) time = std::round(time * tick_rate_) / tick_rate_;
          range_drag_time_ = range_drag_handle_ == 0 ? std::clamp(time, 0.0, range_out_ - RangeSpan()) :
                                                         std::clamp(time, range_in_ + RangeSpan(), exact_duration_);
        }
      } else {
        if (std::abs(range_drag_time_ - range_drag_initial_) > .000001f)
          Emit(send, "set_project", {{"render_range", {{"mode", "region"},
                                      {range_drag_handle_ == 0 ? "in" : "out", range_drag_time_}}}});
        range_drag_handle_ = -1;
      }
    }
    if (hovered && hovered_range < 0 && range_drag_handle_ < 0 && connected_ && !keys_.empty() &&
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
    // Brackets occupy the slider row, below camera diamonds, so a region
    // gesture never shifts a key or seeks the demo.
    for (int edge = 0; edge < 2; ++edge) {
      const float time = range_drag_handle_ == edge ? range_drag_time_ : (edge ? range_out_ : range_in_);
      const float x = tx(time), direction = edge ? -1.0f : 1.0f;
      const ImU32 color = Ink(render_region_ || hovered_range == edge || range_drag_handle_ == edge ? .95f : .4f);
      draw->AddLine(ImVec2(x, track_top - S(3)), ImVec2(x, pos.y + S(47)), color, S(2));
      draw->AddTriangleFilled(ImVec2(x, pos.y + S(39)),
                               ImVec2(x + direction * S(9), pos.y + S(43)),
                               ImVec2(x, pos.y + S(47)), color);
    }
    const float playhead = tx(timeline_active_ ? timeline_drag_time_ : cursor_);
    draw->AddRectFilled(ImVec2(playhead - S(8), pos.y + S(26)),
                        ImVec2(playhead + S(8), pos.y + S(42)),
                        IM_COL32(255, 255, 255, 255), S(2));
    if (hovered && !timeline_active_ && range_drag_handle_ < 0) {
      Tip(hovered_range == 0 ? "Render region In. Drag to trim the start. I sets In at the playhead." :
          hovered_range == 1 ? "Render region Out. Drag to trim the end. O sets Out at the playhead." :
          hovered_id.empty() ? "Click or drag to scrub. Release to seek. Shift disables tick snapping." :
                              "Click to select and seek. Drag to move the keyframe. Shift disables tick snapping.");
    }
    if (range_drag_handle_ >= 0) {
      char time[32];
      TimeText(time, sizeof(time), range_drag_time_);
      ImGui::BeginTooltip();
      DrawBackdrop();
      ImGui::Text("%s  %s", range_drag_handle_ ? "Out" : "In", time);
      ImGui::EndTooltip();
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
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false) && demo_ && !keys_.empty())
      Emit(send, "request_export");
    else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) &&
             !keys_.empty() && demo_)
      Emit(send, "preview", {{"playing", !playing_}, {"sync_demo", sync_demo_ && demo_}});
    else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false))
      Capture(send);
    else if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false))
      SetRangeBoundary(send, false);
    else if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_O, false))
      SetRangeBoundary(send, true);
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && inspect_key_ && Selected())
      Emit(send, "remove_key", {{"id", selected_id_}});
  }
};

}  // namespace cs2_live_ui
