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
#include "camera_map_ui.h"
#include "camera_timeline_ui.h"

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
  bool settings_visible = false;
  // The host reads the private installed-game catalog once, outside rendering.
  Json cosmetics_catalog;
  std::string cosmetics_catalog_error;
  // Host validates the bounded library once before assigning it. Drafts live
  // only in this editor session; the original portable projects stay intact.
  void SetSavedShots(Json library) {
    saved_shots_ = std::move(library.at("shots"));
    saved_shot_index_ = 0;
    saved_shot_drafts_.clear();
    saved_shot_documents_.clear();
    active_shot_id_.clear();
    pending_shot_id_.clear();
    saved_shot_error.clear();
  }
  std::string saved_shot_error;

  // Called only by the host's opt-in private diagnostic file hook. This changes
  // our own layout; it never emits a bridge command or changes project data.
  void OpenShotSettingsForDiagnostics(){show_inspector_=true;navigation_override_["inspector_section"]="shot";diagnostic_open_shot_=true;}
  void OpenCameraMapForDiagnostics(){navigation_override_.update({{"workspace_source","camera"},{"workspace_tool","map"},{"workspace_open",true}});}
  void OpenCameraCurvesForDiagnostics(){navigation_override_.update({{"workspace_source","camera"},{"workspace_tool","curves"},{"workspace_open",true}});timing_view_=false;
    if(selected_id_.empty()&&!keys_.empty())Select(keys_[std::min<std::size_t>(1,keys_.size()-1)].id);}
  void OpenCurveChannelMenuForDiagnostics(){OpenCameraCurvesForDiagnostics();camera_curves_.OpenChannelMenuForDiagnostics();}
  void SetMapPreview(const Json& preview){camera_map_.SetMapPreview(preview);}
  void OpenEditorSettingsForDiagnostics(){settings_visible=true;}
  void OpenCameraTimelineForDiagnostics(){navigation_override_.update({{"workspace_source","camera"},{"workspace_tool","timeline"},{"workspace_open",true}});}
  void OpenSettingsForDiagnostics(){OpenEditorSettingsForDiagnostics();}
  void OpenTimelineForDiagnostics(){OpenCameraTimelineForDiagnostics();}

  void Draw(const Json& state, const Send& send) {
    if (!visible || !ImGui::GetCurrentContext()) return;
    const int frame = ImGui::GetFrameCount();
    if (last_draw_frame_ >= 0 && frame != last_draw_frame_ + 1) {
      timeline_active_ = false;
      demo_timeline_active_ = false;
      timeline_drag_id_.clear();
      range_drag_handle_ = -1;
      graph_handle_ = -1;
      graph_changed_ = false;
      was_item_active_ = false;
      draft_target_.clear();
      CancelNumericScrub();
      numeric_edits_.clear();
      camera_map_.ResetGesture();
      camera_curves_.ResetGesture();
      camera_timeline_.ResetGesture();
    }
    last_draw_frame_ = frame;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 320.0f || display.y < 240.0f) return;
    // Panorama's native demo controller is authored against a 1080px viewport.
    editor_preferences_=Member(state,"editor_preferences");
    scale_ = display.y / 1080.0f * float(PreferenceNumber("ui_scale",1,.75,1.5));
    ReadNavigation();
    ReadState(state);
    if(SafeExitBusy() && !safe_exit_was_busy_)CancelEditorGesture();
    safe_exit_was_busy_=SafeExitBusy();
    ConfirmSavedShotLoad(state, send);
    SyncSavedShotAssociation(state);
    const ImGuiStyle saved_style = ImGui::GetStyle();
    ApplyStyle();
    const float margin=S(10),footer_height=S(workspace_source_=="camera"||SafeExitBusy()||!safe_exit_error_.empty()?88:48);
    DrawWorkspaceBar(ImVec2(0,display.y-footer_height),ImVec2(display.x,footer_height),send);
    const float actual_footer=footer_height;
    const float inspector_width=show_inspector_?std::min(S(float(PreferenceNumber("inspector_width",430,320,640))),display.x*.34f):0;
    const float work_width=display.x-(show_inspector_?inspector_width+margin*3:margin*2);
    const float work_height=std::min(display.y*.55f,S(workspace_source_=="demo"?250:PreferenceNumber("timeline_height",240,160,420)+190+(demo_navigation_pinned_?250:0)));
    ImGui::BeginDisabled(SafeExitBusy());
    if(workspace_open_)DrawWorkspace(ImVec2(margin,display.y-actual_footer-margin-work_height),ImVec2(work_width,work_height),state,send);
    if(show_inspector_)DrawInspector(ImVec2(display.x-inspector_width-margin,margin),ImVec2(inspector_width,display.y-actual_footer-margin*2),state,send);
    if(settings_visible)DrawEditorSettings(send);
    ImGui::EndDisabled();
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
    std::array<std::array<float,4>,3> channel_easing{{{{0,0,1,1}},{{0,0,1,1}},{{0,0,1,1}}}};
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
  std::string workspace_source_="camera",workspace_tool_="timeline",inspector_section_="camera";
  bool workspace_open_=true,demo_navigation_pinned_=false,timing_view_=false;
  Json navigation_seen_,navigation_override_=Json::object();
  bool diagnostic_open_shot_ = false;
  bool shortcuts_focus_requested_ = false;
  bool project_browser_requested_ = false;
  std::string project_browser_id_;
  Json project_library_, project_storage_;
  int tick_ = 0, tick_rate_ = 64, revision_ = 0, last_draw_frame_ = -1;
  float cursor_ = 0, duration_ = 8, draft_duration_ = 8;
  bool demo_timeline_available_ = false, demo_timeline_active_ = false, demo_timeline_moved_ = false;
  int demo_total_ticks_ = 0, demo_drag_tick_ = 0;
  bool demo_seek_pending_ = false;
  int demo_seek_target_ = 0;
  double demo_rate_ = 64, demo_duration_ = 0;
  float demo_drag_start_x_ = 0;
  std::string demo_timeline_key_, demo_timeline_reason_, demo_event_player_, demo_marker_player_;
  Json demo_players_ = Json::array();
  struct DemoRound {int tick=0, number=0;};
  std::vector<DemoRound> demo_rounds_;
  std::vector<int> demo_kills_, demo_deaths_;

  double exact_duration_ = 8, exact_cursor_ = 0;
  double transport_cursor_=0,transport_duration_=8;
  int transport_tick_=0,demo_range_in_=0,demo_range_out_=0,draft_demo_in_=0,draft_demo_out_=0;
  bool demo_range_enabled_=false;
  std::string capture_error_, range_error_;
  std::string safe_exit_phase_="idle",safe_exit_error_;
  bool safe_exit_was_busy_=false;
  bool SafeExitBusy()const{return safe_exit_phase_=="requested"||safe_exit_phase_=="saving"||safe_exit_phase_=="closing";}
  bool render_region_ = false;
  double range_in_ = 0, range_out_ = 8, draft_range_in_ = 0, draft_range_out_ = 8;
  int range_drag_handle_ = -1;
  double range_drag_time_ = 0, range_drag_initial_ = 0;
  float range_drag_start_x_ = 0;
  bool range_drag_moved_ = false;
  int start_tick_ = 0, draft_start_tick_ = 0;
  std::string mode_, project_name_, interpolation_, error_, selected_id_, draft_target_, duration_error_;
  char draft_name_[128] = {};
  Key camera_, draft_;
  Json hotkeys_;
  Json saved_shots_ = Json::array(), pending_shot_project_;
  std::unordered_map<std::string, Json> saved_shot_drafts_;
  std::unordered_map<std::uint64_t, std::string> saved_shot_documents_;
  int saved_shot_index_ = 0;
  std::string active_shot_id_, pending_shot_id_, pending_shot_token_;
  std::uint64_t saved_shot_sequence_=0, saved_shot_observed_document_=0;
  int saved_shot_observed_revision_=-1;
  double pending_shot_time_ = 0;
  Json scene_capabilities_;
  CameraMapUi camera_map_;
  CameraCurveUi camera_curves_;
  CameraTimelineUi camera_timeline_;
  Json editor_preferences_;
  std::unordered_map<std::string,double> preference_drafts_;
  Json current_project_;
  Json presentation_;
  Json cosmetics_;
  std::string cosmetic_player_;
  int cosmetic_agent_ = 0;
  int cosmetic_seen_revision_ = -1;
  std::string cosmetic_draft_target_;
  char cosmetic_filter_[128] = {};
  bool palette_enabled_ = false;
  std::array<float, 3> palette_t_{{1, .25f, .25f}}, palette_ct_{{.25f, .5f, 1}};
  std::unordered_map<std::string, double> scene_drafts_;
  struct ScenePending {Json value;double sent_at=0;std::string prior_error;};
  std::unordered_map<std::string, ScenePending> scene_pending_;
  std::unordered_map<std::string, std::string> scene_errors_;
  std::vector<Key> keys_;
  std::vector<std::string> capture_ids_;
  bool capture_pending_ = false;
  double capture_requested_at_ = 0, pending_until_ = 0;
  int pending_revision_ = -1, preset_ = 1;
  int graph_handle_ = -1;
  bool graph_changed_ = false;
  int easing_channel_=0;
  std::array<float,4> graph_original_{{0,0,1,1}};
  ImVec2 graph_mouse_{};
  std::string timeline_drag_id_;
  bool timeline_active_ = false, timeline_moved_ = false;
  float timeline_drag_time_ = 0, timeline_original_time_ = 0;
  float timeline_drag_start_x_ = 0;
  double last_refresh_ = -100;
  struct NumericEdit { double initial = 0; bool active = false; double time_factor = 1; std::string time_unit, resize_mode; };
  std::unordered_map<ImGuiID, NumericEdit> numeric_edits_;
  std::uint64_t editing_document_ = std::numeric_limits<std::uint64_t>::max();
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
    for(int channel=0;channel<3;++channel)key.channel_easing[channel]=Array<4>(Member(value,camera_curve::Field(channel,"_easing").c_str()),key.easing);
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
    draft_target_.clear();
    graph_handle_ = -1;
    if (const Key* key = Selected()) {
      draft_ = *key;
      draft_target_ = id;
    }
  }
  void ReadState(const Json& state) {
    const auto document=ShotDocumentId(state);
    if(document!=editing_document_) {
      if(editing_document_!=std::numeric_limits<std::uint64_t>::max()) {
        // A live import can replace the document while InputText still owns
        // its old buffer. Never apply that unfinished edit to the new shot.
        ImGui::ClearActiveID();CancelNumericScrub();numeric_edits_.clear();
        was_item_active_=false;draft_target_.clear();duration_error_.clear();
        timeline_active_=demo_timeline_active_=false;timeline_drag_id_.clear();
        range_drag_handle_=graph_handle_=-1;graph_changed_=false;
        camera_map_.ResetGesture();camera_curves_.ResetGesture();camera_timeline_.ResetGesture();
      }
      editing_document_=document;
    }
    connected_ = Boolean(Member(state, "connected"));
    demo_ = Boolean(Member(state, "demo"));
    paused_ = Boolean(Member(state, "paused"));
    const auto& seek=Member(state,"demo_seek");
    demo_seek_pending_=Boolean(Member(seek,"active"));
    demo_seek_target_=Integer(Member(seek,"target_tick"));
    playing_ = Boolean(Member(state, "preview_playing"));
    sync_demo_ = Boolean(Member(state, "sync_demo"), sync_demo_);
    tick_ = Integer(Member(state, "tick"));
    tick_rate_ = std::max(1, Integer(Member(state, "tick_rate"), 64));
    revision_ = Integer(Member(state, "revision"));
    exact_cursor_ = ExactNumber(Member(state, "cursor"));
    cursor_ = static_cast<float>(exact_cursor_);
    mode_ = String(Member(state, "mode"), "free");
    error_ = String(Member(state, "error"));
    safe_exit_phase_=String(Member(Member(state,"safe_exit"),"phase"),"idle");
    safe_exit_error_=String(Member(Member(state,"safe_exit"),"error"));
    hotkeys_ = Member(state, "hotkeys");
    scene_capabilities_ = Member(state, "scene_capabilities");
    // Revision is shared by unrelated edits. A scene value is acknowledged
    // only by its own engine readback, never merely by a newer revision.
    for(auto it=scene_pending_.begin();it!=scene_pending_.end();) {
      const auto& cap=Member(scene_capabilities_,it->first.c_str());
      const auto& observed=Member(cap,"value");
      const auto& expected=it->second.value;
      const bool matched=expected.is_boolean() ? observed.is_boolean() && observed==expected :
          expected.is_number() && observed.is_number() &&
          std::abs(ExactNumber(observed)-ExactNumber(expected))<=std::max(1e-6,std::abs(ExactNumber(expected))*1e-6);
      if(matched){scene_errors_.erase(it->first);it=scene_pending_.erase(it);}
      else if((!error_.empty() && error_!=it->second.prior_error) || ImGui::GetTime()-it->second.sent_at>5) {
        scene_errors_[it->first]=!error_.empty()?error_:"Engine readback did not confirm this value; the current value is shown.";
        it=scene_pending_.erase(it);
      }else ++it;
    }
    presentation_ = Member(state, "presentation");
    cosmetics_ = Member(state, "cosmetics");
    project_library_ = Member(state,"project_library");
    project_storage_ = Member(state,"project_storage");
    ReadDemoTimeline(Member(state, "demo_timeline"));
    const auto& playback_range=Member(state,"demo_playback_range");
    demo_range_enabled_=Boolean(Member(playback_range,"enabled"));
    demo_range_in_=Integer(Member(playback_range,"in_tick"));
    demo_range_out_=Integer(Member(playback_range,"out_tick"),demo_total_ticks_);
    if(demo_range_out_<=0&&demo_total_ticks_>0)demo_range_out_=demo_total_ticks_;
    camera_ = ReadKey(Member(state, "camera"));
    const Json& project = Member(state, "project");
    current_project_=project;
    project_name_ = String(Member(project, "name"), "Untitled shot");
    interpolation_ = String(Member(project, "interpolation"), "smooth");
    exact_duration_ = std::max(.01, ExactNumber(Member(project, "duration"), 8));
    duration_ = static_cast<float>(exact_duration_);
    start_tick_ = Integer(Member(project, "start_tick"));
    const Json& range = Member(project, "render_range");
    render_region_ = String(Member(range, "mode"), "composition") == "region";
    if(!render_region_)range_drag_handle_=-1;
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
          if(!scene_pending_.count(entry.key()))scene_drafts_[entry.key()] = ExactNumber(Member(entry.value(), "value"));
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
  void ReadDemoTimeline(const Json& timeline) {
    const auto& demo = Member(timeline, "demo");
    demo_total_ticks_ = Integer(Member(demo, "playback_ticks"));
    demo_rate_ = ExactNumber(Member(demo, "tick_rate"));
    demo_duration_ = ExactNumber(Member(demo, "duration_seconds"));
    demo_timeline_available_ = Boolean(Member(timeline, "available")) && demo_total_ticks_ > 0 &&
        demo_rate_ > 0 && demo_duration_ > 0;
    demo_timeline_reason_ = String(Member(timeline, "reason"), "Demo metadata is loading or unavailable");
    demo_players_ = Member(timeline,"players");
    demo_marker_player_ = String(Member(timeline,"marker_player"));
    const auto& player = Member(timeline, "selected_player");
    const std::string player_id = String(Member(player, "steam_id"));
    demo_event_player_ = String(Member(player, "name"));
    if(demo_event_player_.empty() && !player_id.empty())demo_event_player_=player_id;
    const auto& metadata_revision=Member(timeline,"metadata_revision");
    const std::string revision=metadata_revision.is_number_unsigned()?std::to_string(metadata_revision.get<std::uint64_t>()):std::to_string(Integer(metadata_revision));
    const std::string key = demo_timeline_available_ ? revision + ":" + String(Member(Member(demo, "fingerprint"), "sha256")) +
        ":" + String(Member(demo, "path")) + ":" + std::to_string(demo_total_ticks_) + ":" + player_id : "unavailable";
    if(key == demo_timeline_key_)return;
    demo_timeline_key_ = key;demo_rounds_.clear();demo_kills_.clear();demo_deaths_.clear();demo_timeline_active_ = false;
    if(!demo_timeline_available_)return;
    const auto& rounds = Member(timeline, "rounds");
    if(rounds.is_array())for(const auto& round : rounds) {
      const int tick = Integer(Member(round, "tick"), -1);
      if(tick >= 0 && tick <= demo_total_ticks_)demo_rounds_.push_back({tick,Integer(Member(round, "round"))});
    }
    std::sort(demo_rounds_.begin(),demo_rounds_.end(),[](const auto& a,const auto& b){return a.tick < b.tick;});
    const auto readEvents = [&](const char* name,std::vector<int>& target) {
      const auto& ticks = Member(player,name);
      if(ticks.is_array())for(const auto& value : ticks) {
        const int tick = Integer(value,-1);if(tick >= 0 && tick <= demo_total_ticks_)target.push_back(tick);
      }
      std::sort(target.begin(),target.end());
    };
    readEvents("kills",demo_kills_);readEvents("deaths",demo_deaths_);
  }
  void SeekDemo(const Send& send,int tick) {
    if(!demo_ || !demo_timeline_available_)return;
    const int target=std::clamp(tick,0,demo_total_ticks_);
    if(demo_seek_pending_ && target==demo_seek_target_)return;
    Emit(send,"demo_control",{{"action","seek"},{"tick",target}});
  }
  void JumpRound(const Send& send,int direction) {
    if(direction < 0) {
      for(auto it=demo_rounds_.rbegin();it!=demo_rounds_.rend();++it)if(it->tick < tick_){SeekDemo(send,it->tick);return;}
      SeekDemo(send,0);
    }else {
      for(const auto& round:demo_rounds_)if(round.tick > tick_){SeekDemo(send,round.tick);return;}
      SeekDemo(send,demo_total_ticks_);
    }
  }
  void Emit(const Send& send, const char* command, const Json& args = Json::object()) {
    if (!connected_ || !send) return;
    if(std::strcmp(command,"set_editor_preferences")==0){send(command,args);return;}
    // Curve edits replace a complete key list. Compare the state used to build
    // that list at native dispatch, so a newer MCP/UI edit cannot be lost.
    if(std::strcmp(command,"set_project")==0 && args.contains("keys"))
      send("guarded_mutation",{{"expected_revision",revision_},{"method","set_project"},{"params",args}});
    else send(command, args);
    pending_revision_ = revision_;
    pending_until_ = ImGui::GetTime() + .6;
  }
  bool PreferenceBool(const char* field,bool fallback) const { return Boolean(Member(editor_preferences_,field),fallback); }
  double PreferenceNumber(const char* field,double fallback,double minimum,double maximum) const {return std::clamp(ExactNumber(Member(editor_preferences_,field),fallback),minimum,maximum);}
  std::string TimeUnit() const {return String(Member(editor_preferences_,"time_unit"),"seconds");}
  double FrameRate() const {return PreferenceNumber("frame_rate",60,1,1000);}
  double TimeFactor() const {const auto unit=TimeUnit();return unit=="ticks"?tick_rate_:unit=="frames"?FrameRate():1;}
  const char* TimeFormat() const {const auto unit=TimeUnit();return unit=="ticks"?"%.6f ticks":unit=="frames"?"%.6f frames":"%.6f s";}
  double SnapTime(double seconds) const {const auto mode=String(Member(editor_preferences_,"snap_mode"),"tick");const double rate=mode=="frame"?FrameRate():mode=="tick"?tick_rate_:0;return rate>0?std::round(seconds*rate)/rate:seconds;}
  template<typename T>bool TimeField(const char* label,T* seconds,double minimum,double maximum){
    const auto id=ImGui::GetID(label);auto& edit=numeric_edits_[id];
    // Preferences may also change through MCP while this field is active.
    // Keep the typed or scrubbed quantity in its original unit until release.
    if(!edit.active && middle_field_!=id){edit.time_factor=TimeFactor();edit.time_unit=TimeUnit();}
    const double factor=edit.time_factor;double value=static_cast<double>(*seconds)*factor;
    const char* format=edit.time_unit=="ticks"?"%.6f ticks":edit.time_unit=="frames"?"%.6f frames":"%.6f s";
    const bool commit=NumericField(label,&value,minimum*factor,maximum*factor,format);
    *seconds=static_cast<T>(value/factor);return commit;
  }
  bool NumericEditing(const char* label) const {
    const auto id=ImGui::GetID(label);const auto edit=numeric_edits_.find(id);
    return middle_field_==id||(edit!=numeric_edits_.end()&&edit->second.active);
  }
  template<typename T>void DurationField(const Send& send,const char* label,T* value) {
    const auto id=ImGui::GetID(label);auto& edit=numeric_edits_[id];
    if(!edit.active&&middle_field_!=id)edit.resize_mode=String(Member(editor_preferences_,"resize_mode"),"keep");
    const auto resize=edit.resize_mode;
    try {
      const double minimum=timeline_time::MinimumDuration(current_project_,resize);
      if(TimeField(label,value,minimum,86400)) {
        const auto patch=timeline_time::ResizeProject(current_project_,std::clamp(double(*value),minimum,86400.),resize);
        if(timeline_time::Changed(current_project_,patch))Emit(send,"set_project",patch);
        duration_error_.clear();
      }
    }catch(const std::exception& error){duration_error_=std::string("Duration was not changed: ")+error.what();}
  }
  void SetPreference(const Send& send,const char* name,const Json& value){if(connected_&&send)send("set_editor_preferences",{{name,value}});}
  double CaptureTime() const {
    return workspace_source_=="demo"?(keys_.empty()?0.:(double(tick_)-start_tick_)/tick_rate_):exact_cursor_;
  }
  void CaptureTip() {
    Tip(workspace_source_=="demo"?
      "Capture at the current demo tick, relative to the shot start. The first key anchors a new shot here. A later key extends the shot; an occupied tick updates its key. K":
      "Capture at the camera playhead, independently of the demo when Sync is off. A later key extends the shot; an occupied tick updates its key. K");
  }
  void Capture(const Send& send) {
    if (!demo_) return;
    const double time=CaptureTime();
    if(time<0){capture_error_="Capture rejected: demo tick is before the shot start.";return;}
    if(time>86400){capture_error_="Capture rejected: shot time exceeds 24 hours.";return;}
    capture_error_.clear();
    // Capturing on an occupied tick updates that key instead of submitting
    // an invalid pair of keys at the same timestamp.
    for (const auto& key : keys_) {
      if (std::abs(key.time - time) < .5 / tick_rate_) {
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
    Emit(send, "add_key", {{"time", time}});
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
    if(!demo_)return;
    const double span = RangeSpan();
    if(exact_cursor_<(end?span:0.) || exact_cursor_>(end?exact_duration_:exact_duration_-span)) {
      range_error_=end?"Set Out rejected: camera playhead is outside the valid shot range.":"Set In rejected: camera playhead is outside the valid shot range.";
      return;
    }
    range_error_.clear();
    const double time=exact_cursor_;
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
  void DrawRenderScope(const Send& send, float width, bool compact=false) {
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##render_scope", compact?(render_region_?"In / Out":"Full shot"):(render_region_ ? "Render region" : "Whole composition"))) {
      DrawBackdrop();
      if (ImGui::Selectable("Whole composition", !render_region_) && render_region_)
        Emit(send, "set_project", {{"render_range", {{"mode", "composition"}}}});
      if (ImGui::Selectable("Render region", render_region_) && !render_region_)
        Emit(send, "set_project", {{"render_range", {{"mode", "region"}}}});
      ImGui::EndCombo();
    }
    Tip("Preview and render the whole composition or the saved In/Out region. Switching keeps both boundaries and all camera keys.");
  }
  void DrawTimelinePlayerSelector(const Send& send,float width) {
    std::string label=demo_event_player_.empty()?"Choose marker player":demo_event_player_;
    ImGui::BeginDisabled(!demo_ || !demo_timeline_available_ || !demo_players_.is_array() || demo_players_.empty());
    ImGui::SetNextItemWidth(width);
    if(ImGui::BeginCombo("##timeline_player",label.c_str())) {
      DrawBackdrop();
      if(ImGui::Selectable("Follow viewed player",demo_marker_player_.empty()))
        Emit(send,"select_timeline_player",{{"steam_id",nullptr}});
      ImGui::Separator();
      if(demo_players_.is_array())for(const auto& player:demo_players_) {
        const std::string id=String(Member(player,"steam_id"));if(id.empty())continue;
        const std::string name=String(Member(player,"name"),id.c_str());
        const bool duplicate=std::count_if(demo_players_.begin(),demo_players_.end(),[&](const Json& other){return String(Member(other,"name"))==name;})>1;
        const std::string display=name.empty()?id:duplicate?name+" #"+id:name;
        ImGui::PushID(id.c_str());
        if(ImGui::Selectable(display.c_str(),id==demo_marker_player_))Emit(send,"select_timeline_player",{{"steam_id",id}});
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Tip(demo_timeline_available_?"Filter kill / death markers using this demo's recorded roster. This does not change the spectator camera. Watch a player from Scene > Player when the game adapter is available.":demo_timeline_reason_.c_str());
  }
  void DrawPlayerSelector(const Send& send, float width) {
    const Json& capability = Member(Member(presentation_, "capabilities"), "select_player");
    const Json& selected = Member(presentation_, "selected_player");
    const std::string selected_id = String(Member(selected, "steam_id"));
    const bool pending = Boolean(Member(presentation_, "selection_pending"));
    const std::string requested = String(Member(presentation_, "requested_player"));
    const Json& players = Member(cosmetics_, "players");
    std::string label = String(Member(selected, "nickname"), "Choose player");
    if (pending && players.is_array())
      for (const auto& player : players)
        if (String(Member(player, "steam_id")) == requested)
          label = String(Member(player, "nickname"), "Player") + "...";
    const bool requestable = Boolean(Member(capability, "requestable"));
    ImGui::BeginDisabled(!demo_ || !requestable || Boolean(Member(cosmetics_, "stale")));
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##observed_player", label.c_str())) {
      DrawBackdrop();
      bool found = false;
      for (const int team : {3, 2}) {
        bool heading = false;
        if (!players.is_array()) continue;
        for (const auto& player : players) {
          if (Integer(Member(player, "team")) != team) continue;
          const std::string id = String(Member(player, "steam_id"));
          if (id.empty()) continue;
          if (!heading) {
            Caption(team == 3 ? "Counter-Terrorists" : "Terrorists");
            heading = true;
          }
          found = true;
          ImGui::PushID(id.c_str());
          const std::string nickname = PlayerLabel(player);
          if (ImGui::Selectable(nickname.c_str(), id == selected_id))
            Emit(send, "select_player", {{"steam_id", id}});
          ImGui::PopID();
        }
      }
      if (!found) Caption("Waiting for demo players...");
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    const std::string reason = String(Member(capability, "reason"));
    Tip(!requestable && !reason.empty() ? reason.c_str() :
        "Watch a demo player by nickname. This releases the held camera and keeps your camera keys.");
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
    if (TimeField("##range_in", &draft_range_in_, 0, range_out_ - RangeSpan()))
      Emit(send, "set_project", {{"render_range", {{"in", draft_range_in_}}}});
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    if (TimeField("##range_out", &draft_range_out_, range_in_ + RangeSpan(), exact_duration_))
      Emit(send, "set_project", {{"render_range", {{"out", draft_range_out_}}}});
    if (ImGui::Button("Set In  [I]", ImVec2(half, 0))) SetRangeBoundary(send, false);
    ImGui::SameLine();
    if (ImGui::Button("Set Out  [O]", ImVec2(half, 0))) SetRangeBoundary(send, true);
    if (ImGui::Button("Reset region", ImVec2(-1, 0)))
      Emit(send, "set_project", {{"render_range", {{"in", 0}, {"out", exact_duration_}}}});
    Tip("Restore In/Out to the full duration. Ctrl+Z restores the previous region.");
  }
  bool SceneAvailable(const char* id) const {
    return Boolean(Member(Member(scene_capabilities_, id), "available"));
  }
  const Json& SceneValue(const char* id) const {
    const auto pending=scene_pending_.find(id);
    return pending==scene_pending_.end() ? Member(Member(scene_capabilities_,id),"value") : pending->second.value;
  }
  void SetScene(const Send& send,const Json& settings) {
    if(!connected_ || !send)return;
    for(auto entry=settings.begin();entry!=settings.end();++entry) {
      scene_pending_[entry.key()]={entry.value(),ImGui::GetTime(),error_};
      scene_errors_.erase(entry.key());
    }
    Emit(send,"set_scene",{{"settings",settings}});
  }
  void DrawSceneStatus(const char* id,const char* paired=nullptr) {
    if(scene_pending_.count(id) || (paired && scene_pending_.count(paired)))Caption("Waiting for engine readback...");
    std::string previous;
    for(const char* field:{id,paired}) {
      if(!field)continue;
      const auto failure=scene_errors_.find(field);
      if(failure!=scene_errors_.end() && failure->second!=previous) {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.9f,.56f,.42f,1));
        ImGui::TextWrapped("%s",failure->second.c_str());
        ImGui::PopStyleColor();
        previous=failure->second;
      }
    }
  }
  void SceneHint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s",text);
    ImGui::PopStyleColor();
  }
  void DrawSceneSettings(const Send& send) {
    if (!scene_capabilities_.is_object() || scene_capabilities_.empty()) return;
    if (!ImGui::CollapsingHeader("Scene settings")) return;
    ImGui::BeginDisabled(!demo_);
    if (ImGui::Button("Clean frame", ImVec2(-1, 0))) {
      Json settings = Json::object();
      for (const char* id : {"hud_visible", "viewmodel_visible", "xray_enabled", "fps_counter_visible",
                              "trueview_status_visible", "telemetry_frametime_visible",
                              "telemetry_network_visible", "telemetry_ping_visible",
                              "telemetry_margin_visible", "build_info_visible"})
        if (Boolean(Member(Member(scene_capabilities_, id), "available"))) settings[id] = false;
      if (!settings.empty()) SetScene(send,settings);
      visible = false;
    }
    ImGui::EndDisabled();
    Tip("Hide the game HUD, viewmodel, player X-ray outlines and editor. F8 brings the editor back; Reset scene settings restores the game display.");
    const auto group_for = [&](const std::string& id, const Json& capability) {
      if (id == "hud_visible" && presentation_.is_object()) return std::string();
      if (id == "fps_counter_visible" || id == "trueview_status_visible" ||
          id == "build_info_visible" || id.rfind("telemetry_", 0) == 0) return std::string("Diagnostics");
      return String(Member(capability, "group"));
    };
    const char* groups[] = {"Lens", "Lighting", "Post processing", "Combat", "Environment", "Diagnostics"};
    for (const char* group : groups) {
      bool present = false;
      for (auto item = scene_capabilities_.begin(); item != scene_capabilities_.end(); ++item)
        if (group_for(item.key(), item.value()) == group && Boolean(Member(item.value(), "available"))) present = true;
      if (!present) continue;
      if (!ImGui::TreeNode(group)) continue;
      if(std::strcmp(group,"Lens")==0) {
        if(SceneAvailable("dof_override") && !Boolean(SceneValue("dof_override")))
          SceneHint("Depth of field is off. Enable it to apply the focus distances below.");
        else if(SceneAvailable("depth_of_field") && !Boolean(SceneValue("depth_of_field")))
          SceneHint("The depth of field renderer is off.");
        else if(!SceneAvailable("depth_of_field"))
          SceneHint("Renderer status is unavailable on this build. Available focus controls can still be edited.");
      }
      if(std::strcmp(group,"Post processing")==0) {
        if(SceneAvailable("postprocess_enabled") && !Boolean(SceneValue("postprocess_enabled")))
          SceneHint("Post processing is off. Bloom and color correction will not be visible.");
        else if(SceneAvailable("bloom_enabled") && !Boolean(SceneValue("bloom_enabled")))
          SceneHint("Bloom is off. Enable it to apply bloom strength.");
      }
      for (auto entry = scene_capabilities_.begin(); entry != scene_capabilities_.end(); ++entry) {
        const Json& capability = entry.value();
        if (group_for(entry.key(), capability) != group || !Boolean(Member(capability, "available"))) continue;
        // These engine controls have one coherent UI owner each. The legacy
        // controls remain usable when their preferred counterpart is absent.
        if(entry.key()=="fullbright" && SceneAvailable("shading_mode"))continue;
        if(entry.key()=="depth_of_field" && SceneAvailable("dof_override"))continue;
        const bool available = Boolean(Member(capability, "available"));
        const std::string label = String(Member(capability, "label"), entry.key().c_str());
        ImGui::PushID(entry.key().c_str());
        ImGui::BeginDisabled(!demo_ || !available);
        if(entry.key()=="shading_mode") {
          Caption("Shading");
          static const char* modes[]={"Normal","Fullbright","Diffuse lighting"};
          const int mode=std::clamp(Integer(SceneValue("shading_mode")),0,2);
          ImGui::SetNextItemWidth(-1);
          if(ImGui::BeginCombo("##scene_shading",modes[mode])) {
            DrawBackdrop();
            for(int choice=0;choice<3;++choice)
              if(ImGui::Selectable(modes[choice],choice==mode) && choice!=mode)
                SetScene(send,{{"shading_mode",choice}});
            ImGui::EndCombo();
          }
        } else if (String(Member(capability, "type")) == "boolean") {
          bool enabled = Boolean(SceneValue(entry.key().c_str()));
          if (ImGui::Checkbox(label.c_str(), &enabled)) {
            Json settings={{entry.key(),enabled}};
            if(entry.key()=="dof_override" && SceneAvailable("depth_of_field"))
              settings["depth_of_field"]=enabled;
            SetScene(send,settings);
          }
        } else {
          Caption(label.c_str());
          ImGui::SetNextItemWidth(-1);
          double& value = scene_drafts_[entry.key()];
          const double minimum = ExactNumber(Member(capability, "minimum"), -100000);
          const double maximum = ExactNumber(Member(capability, "maximum"), 100000);
          if (NumericField("##scene_value", &value, minimum, maximum, "%.3f")) {
            SetScene(send,{{entry.key(),value}});
          }
        }
        const std::string help = String(Member(capability, available ? "help" : "reason"));
        if (!help.empty()) Tip(help.c_str());
        DrawSceneStatus(entry.key().c_str(),entry.key()=="dof_override" && SceneAvailable("depth_of_field")?"depth_of_field":nullptr);
        if(entry.key()=="exposure")SceneHint("0 = automatic exposure. A positive value overrides the engine exposure scale.");
        if(entry.key()=="bloom_strength")SceneHint("-1 = automatic bloom strength. Nonnegative values override the engine strength; Bloom and Post processing must be enabled.");
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
    DrawTeamPalette(send);
    if (ImGui::Button("Reset scene settings", ImVec2(-1, 0))) {
      scene_pending_.clear();scene_errors_.clear();Emit(send,"reset_scene");
    }
    Tip("Restore the engine values from the start of this session. Ctrl+Z restores project overrides.");
    ImGui::Separator();
  }
  void DrawPresentation(const Send& send) {
    if (!presentation_.is_object()) return;
    Caption("Player");
    DrawPlayerSelector(send, -1);
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
  void DrawCosmetics(const Send& send) {
    if (!cosmetics_.is_object() && !cosmetics_catalog.is_object()) return;
    if (!ImGui::CollapsingHeader("Player agents")) return;
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
    const Json& capability = Member(Member(cosmetics_, "capabilities"), "agents");
    const bool available = Boolean(Member(capability, "available"));
    const int team = selected ? Integer(Member(*selected, "team")) : 0;
    const bool target_ready = selected && (team == 2 || team == 3) &&
        !Boolean(Member(cosmetics_, "stale"));
    {
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
    ImGui::Separator();
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
    color[ImGuiCol_TitleBg] = ImVec4(35.f/255, 35.f/255, 35.f/255, .94f);
    color[ImGuiCol_TitleBgActive] = ImVec4(55.f/255, 55.f/255, 55.f/255, .96f);
    color[ImGuiCol_TitleBgCollapsed] = color[ImGuiCol_TitleBg];
    color[ImGuiCol_ResizeGrip] = ImVec4(.65f,.65f,.65f,.16f);
    color[ImGuiCol_ResizeGripHovered] = ImVec4(.75f,.75f,.75f,.45f);
    color[ImGuiCol_ResizeGripActive] = ImVec4(.85f,.85f,.85f,.65f);
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
    const bool menu=(ImGui::GetCurrentWindow()->Flags&(ImGuiWindowFlags_Popup|ImGuiWindowFlags_Tooltip))!=0;
    // Menus must cover sibling editor windows. This texture already contains
    // the clean game blur/dimming, so alpha here would reveal old UI labels.
    const auto opacity=menu?255:static_cast<int>(std::round(255*PreferenceNumber("panel_opacity",.94,.3,1)));
    if (backdrop_texture != 0 && PreferenceBool("blur_enabled",true) && display.x > 0 && display.y > 0) {
      draw->AddImage(backdrop_texture, pos, end,
                      ImVec2(pos.x / display.x, pos.y / display.y),
                      ImVec2(end.x / display.x, end.y / display.y),
                      IM_COL32(255,255,255,opacity));
    } else {
      // The provided backdrop already contains game dimming. Without it,
      // menus use opaque gray; body panels retain the configured dim layer.
      draw->AddRectFilled(pos, end, menu?IM_COL32(48,48,48,255):IM_COL32(0,0,0,160*opacity/255));
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
    if(!PreferenceBool("show_tooltips",true))return;
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
  void TimeText(char* buffer, size_t length, float seconds) const {
    seconds = std::max(0.0f, seconds);
    const auto unit=TimeUnit();if(unit!="seconds"){std::snprintf(buffer,length,"%.3f %s",seconds*TimeFactor(),unit=="ticks"?"ticks":"frames");return;}
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
  static std::uint64_t ShotDocumentId(const Json& state) {
    const auto& value=Member(state,"project_document_id");
    return value.is_number_unsigned()?value.get<std::uint64_t>():
        value.is_number_integer() && value.get<std::int64_t>()>0?std::uint64_t(value.get<std::int64_t>()):0;
  }
  void SyncSavedShotAssociation(const Json& state) {
    if(saved_shots_.empty())return;
    const auto document=ShotDocumentId(state);
    const auto found=saved_shot_documents_.find(document);
    active_shot_id_=found==saved_shot_documents_.end()?"":found->second;
    if(!active_shot_id_.empty() && (document!=saved_shot_observed_document_ || revision_!=saved_shot_observed_revision_))
      saved_shot_drafts_[active_shot_id_]=Member(state,"project");
    saved_shot_observed_document_=document;saved_shot_observed_revision_=revision_;
  }
  void ConfirmSavedShotLoad(const Json& state, const Send& send) {
    if(pending_shot_id_.empty())return;
    // Equality or a newer global revision is not an import receipt: preview
    // changes revisions too, and a repeated load may equal an old snapshot.
    const auto& receipt=Member(state,"project_import_receipt");
    if(String(Member(receipt,"token"))==pending_shot_token_) {
      if(!Boolean(Member(receipt,"applied")))
        saved_shot_error=String(Member(receipt,"error"),"Shot import failed; the demo was not moved.");
      else if(ShotDocumentId(state)!=0 && Member(receipt,"project_document_id")==Member(state,"project_document_id")) {
        saved_shot_documents_[ShotDocumentId(state)]=pending_shot_id_;
        if(Member(state,"project")==pending_shot_project_) {
          Emit(send,"demo_control",{{"action","seek"},{"tick",Integer(Member(pending_shot_project_,"start_tick"))}});
          Emit(send,"scrub",{{"time",0},{"seek_demo",false}});
          selected_id_.clear();inspect_key_=false;draft_target_.clear();
        }else saved_shot_error="Shot changed after import; the demo was not moved.";
      }else saved_shot_error="Shot was replaced after import; the demo was not moved.";
      pending_shot_id_.clear();
    } else if(ImGui::GetTime()-pending_shot_time_>5) {
      saved_shot_error="Shot import was not confirmed; the demo was not moved.";
      pending_shot_id_.clear();
    }
  }
  void DrawSavedShots(const Json& state, const Send& send) {
    if(saved_shots_.empty() && saved_shot_error.empty())return;
    Caption("Saved shots");
    if(!saved_shots_.empty()) {
      ImGui::BeginDisabled(!demo_ || !pending_shot_id_.empty());
      const float load_width=S(62);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-load_width-S(7));
      const auto label=saved_shots_[saved_shot_index_]["label"].get<std::string>();
      if(ImGui::BeginCombo("##saved_shots",label.c_str())) {
        DrawBackdrop();
        for(std::size_t i=0;i<saved_shots_.size();++i) {
          ImGui::PushID(int(i));
          const auto text=saved_shots_[i]["label"].get<std::string>();
          if(ImGui::Selectable(text.c_str(),saved_shot_index_==int(i)))saved_shot_index_=int(i);
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if(ImGui::Button("Load##saved_shot",ImVec2(load_width,0))) {
        if(!active_shot_id_.empty())saved_shot_drafts_[active_shot_id_]=Member(state,"project");
        const auto& shot=saved_shots_[saved_shot_index_];
        pending_shot_id_=shot["id"].get<std::string>();
        const auto draft=saved_shot_drafts_.find(pending_shot_id_);
        pending_shot_project_=draft==saved_shot_drafts_.end()?shot["project"]:draft->second;
        pending_shot_time_=ImGui::GetTime();saved_shot_error.clear();
        pending_shot_token_="saved-shot-"+std::to_string(++saved_shot_sequence_)+"-"+
            std::to_string(std::uint64_t(ImGui::GetTime()*1000000));
        Emit(send,"preview",{{"playing",false}});
        Emit(send,"demo_control",{{"action","pause"}});
        Emit(send,"import_project",{{"project",pending_shot_project_},{"import_token",pending_shot_token_}});
      }
      Tip("Load an editable camera shot and seek its start. Ctrl+Z restores the previous track. "
          "Switching keeps edits until this session ends; Ctrl+S saves only the current project.");
      ImGui::EndDisabled();
    }
    if(!pending_shot_id_.empty())Caption("Loading saved shot...");
    if(!saved_shot_error.empty())ImGui::TextWrapped("%s",saved_shot_error.c_str());
    ImGui::Spacing();
  }
  bool NavigationButton(const char* label,ImVec2 size){const bool pressed=ImGui::Button(label,size);return pressed||ImGui::IsItemClicked(ImGuiMouseButton_Left);}
  void CancelEditorGesture(){
    ImGui::ClearActiveID();CancelNumericScrub();numeric_edits_.clear();
    timeline_active_=demo_timeline_active_=false;timeline_drag_id_.clear();range_drag_handle_=graph_handle_=-1;graph_changed_=false;
    camera_map_.ResetGesture();camera_curves_.ResetGesture();camera_timeline_.ResetGesture();
    was_item_active_=false;draft_=inspect_key_&&Selected()?*Selected():camera_;draft_duration_=duration_;draft_start_tick_=start_tick_;
    draft_range_in_=range_in_;draft_range_out_=range_out_;std::snprintf(draft_name_,sizeof(draft_name_),"%s",project_name_.c_str());
    ImGui::ClosePopupsOverWindow(nullptr,false);
  }
  void ApplyNavigation(const Json& values){
    const auto source=values.value("workspace_source",workspace_source_),tool=values.value("workspace_tool",workspace_tool_),section=values.value("inspector_section",inspector_section_);
    const bool open=values.value("workspace_open",workspace_open_),pin=values.value("demo_navigation_pinned",demo_navigation_pinned_);
    if(source!=workspace_source_||tool!=workspace_tool_||section!=inspector_section_||open!=workspace_open_||pin!=demo_navigation_pinned_)CancelEditorGesture();
    if(inspector_section_!=section)diagnostic_open_shot_=true;
    if(workspace_source_!=source){capture_error_.clear();range_error_.clear();}
    workspace_source_=source;workspace_tool_=tool;inspector_section_=section;workspace_open_=open;demo_navigation_pinned_=pin;
  }
  void ReadNavigation(){
    Json incoming={{"workspace_source",String(Member(editor_preferences_,"workspace_source"),"camera")},{"workspace_tool",String(Member(editor_preferences_,"workspace_tool"),"timeline")},
      {"workspace_open",PreferenceBool("workspace_open",true)},{"inspector_section",String(Member(editor_preferences_,"inspector_section"),"camera")},{"demo_navigation_pinned",PreferenceBool("demo_navigation_pinned",false)}};
    Json changed=Json::object();for(auto it=incoming.begin();it!=incoming.end();++it)if(!navigation_seen_.contains(it.key())||navigation_seen_[it.key()]!=it.value())changed[it.key()]=it.value();
    navigation_seen_=incoming;if(!changed.empty())ApplyNavigation(changed);
    if(!navigation_override_.empty()){ApplyNavigation(navigation_override_);navigation_override_=Json::object();}
  }
  void Navigate(const Send& send,const char* field,const Json& value){ApplyNavigation({{field,value}});SetPreference(send,field,value);}
  bool KeyEditingFocused()const{
    auto* focus=ImGui::GetCurrentContext()->NavWindow;if(!focus)return false;const std::string name=focus->Name;
    return (name.rfind("##CS2CinematicInspector",0)==0&&inspector_section_=="camera")||
      (workspace_source_=="camera"&&name.find("##CS2CameraWorkspace")!=std::string::npos);
  }
  void DrawWorkspace(ImVec2 pos,ImVec2 size,const Json& state,const Send& send){
    ImGui::SetNextWindowPos(pos);ImGui::SetNextWindowSize(size);
    ImGui::Begin("##CS2CameraWorkspace",nullptr,kPanelFlags|ImGuiWindowFlags_NoBackground);
    DrawBackdrop(false);FontScale();
    const auto route=[&](const std::string& method,const Json& args){Emit(send,method.c_str(),args);};
    const auto select=[&](const std::string& id){if(id=="@camera"){CancelNumericScrub();numeric_edits_.clear();inspect_key_=false;draft_target_.clear();}else Select(id);};
    const auto background=[&]{DrawBackdrop(false);};
    const auto number=[&](const char* label,double* value,double lo,double hi,const char* format){return NumericField(label,value,lo,hi,format);};
    if(workspace_source_=="demo"||demo_navigation_pinned_){
      if(ImGui::BeginChild("Demo navigation",ImVec2(0,workspace_source_=="demo"?0:S(250)),ImGuiChildFlags_None,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_HorizontalScrollbar)){
        DrawDemoTimeline(send);DrawDemoTransport(send,ImGui::GetContentRegionAvail().x);
      }ImGui::EndChild();
    }
    if(workspace_source_=="camera"){
      const char* child=workspace_tool_=="map"?"Camera map / XY":workspace_tool_=="curves"?"Camera curves / values":"Camera timeline / composition";
      if(ImGui::BeginChild(child,ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_HorizontalScrollbar)){
        camera_map_.visible=workspace_tool_=="map";camera_curves_.visible=workspace_tool_=="curves";camera_timeline_.visible=workspace_tool_=="timeline";
        if(workspace_tool_=="map")camera_map_.Draw(state,route,selected_id_,select,scale_,background,true);
        else if(workspace_tool_=="curves"){
          if(timing_view_){if(NavigationButton("Values",ImVec2(S(90),S(36)))){CancelEditorGesture();timing_view_=false;}}else AccentButton("Values",ImVec2(S(90),S(36)));
          ImGui::SameLine();if(!timing_view_){if(NavigationButton("Timing",ImVec2(S(90),S(36)))){CancelEditorGesture();timing_view_=true;}}else AccentButton("Timing",ImVec2(S(90),S(36)));
          if(timing_view_){
            if(Selected()){char text[96];std::snprintf(text,sizeof(text),"Key %02d / %zu",SelectedIndex()+1,keys_.size());ImGui::SameLine();Caption(text);DrawEasing(send);}
            else Caption("Select a camera key to edit timing");
          }else camera_curves_.Draw(state,route,selected_id_,select,scale_,[&](const char* label,double* value,double lo,double hi,const char* format){return std::strcmp(label,"##curve_time")==0?TimeField(label,value,lo,hi):number(label,value,lo,hi,format);},background,true);
        }else camera_timeline_.Draw(state,route,selected_id_,select,scale_,background,number,true);
      }ImGui::EndChild();
    }
    ImGui::End();
  }
  void DrawWorkspaceBar(ImVec2 pos,ImVec2 size,const Send& send){
    ImGui::SetNextWindowPos(pos);ImGui::SetNextWindowSize(size);ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(S(8),S(5)));ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(S(5),S(4)));
    ImGui::Begin("##CS2CinematicTransport",nullptr,kPanelFlags|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar);DrawBackdrop(false);FontScale();
    ImGui::BeginDisabled(SafeExitBusy());
    auto tab=[&](const char* label,const char* field,const char* value,float width){const bool active=std::string(field)=="workspace_source"?workspace_source_==value:workspace_tool_==value;
      if(active)AccentButton(label,ImVec2(S(width),S(36)));else if(NavigationButton(label,ImVec2(S(width),S(36))))Navigate(send,field,value);ImGui::SameLine();};
    tab("Demo","workspace_source","demo",66);tab("Camera","workspace_source","camera",78);
    if(workspace_source_=="camera"){tab("Timeline","workspace_tool","timeline",82);tab("Map","workspace_tool","map",60);tab("Curves","workspace_tool","curves",76);}
    if(NavigationButton(workspace_open_?"-##workspace":"+##workspace",ImVec2(S(30),S(36))))Navigate(send,"workspace_open",!workspace_open_);
    Tip("Collapse or expand the working area");ImGui::SameLine();
    if(NavigationButton("Inspector",ImVec2(S(90),S(36)))){CancelEditorGesture();show_inspector_=!show_inspector_;}ImGui::SameLine();
    const float right=ImGui::GetWindowContentRegionMax().x-S(410);ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),right));
    if(ImGui::Button("Project",ImVec2(S(78),S(36))))ImGui::OpenPopup("##camera_project_menu");
    ImGui::SameLine();
    if(NavigationButton("Settings",ImVec2(S(82),S(36)))){CancelEditorGesture();settings_visible=!settings_visible;}ImGui::SameLine();
    ImGui::BeginDisabled(!demo_||keys_.empty());if(ImGui::Button("Export",ImVec2(S(70),S(36))))Emit(send,"request_export");ImGui::EndDisabled();ImGui::SameLine();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!connected_||SafeExitBusy());
    if(ImGui::Button("Safe exit",ImVec2(S(104),S(36)))){CancelEditorGesture();Emit(send,"request_safe_exit");}
    Tip("Save the current project, close this Camera Studio game and restore its temporary settings. The application exits after cleanup succeeds.");
    ImGui::EndDisabled();ImGui::SameLine();
    if(NavigationButton("Hide",ImVec2(S(54),S(36)))){CancelEditorGesture();visible=false;}
    if(SafeExitBusy()||!safe_exit_error_.empty()){
      ImGui::SetCursorPos(ImVec2(S(8),S(47)));
      if(SafeExitBusy())ImGui::TextUnformatted(safe_exit_phase_=="closing"?"Closing session and restoring settings...":"Saving project before safe exit...");
      else {
        if(ImGui::Button("Keep editing",ImVec2(S(120),S(36))))Emit(send,"set_safe_exit_status",{{"phase","idle"}});
        ImGui::SameLine();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Safe exit failed; the session is still open.");Tip(safe_exit_error_.c_str());
      }
    }
    if(workspace_source_=="camera"&&!SafeExitBusy()&&safe_exit_error_.empty()){
      ImGui::SetCursorPos(ImVec2(S(8),S(47)));ImGui::BeginDisabled(!demo_||keys_.empty());
      if(IconButton("##preview",playing_?Icon::Pause:Icon::Play,"Camera preview / Space",ImVec2(S(40),S(36))))Emit(send,"preview",{{"playing",!playing_},{"sync_demo",sync_demo_&&demo_}});
      ImGui::SameLine();if(IconButton("##prev",Icon::First,"Previous camera key",ImVec2(S(32),S(36))))JumpKey(send,-1);
      ImGui::SameLine();if(IconButton("##next",Icon::Last,"Next camera key",ImVec2(S(32),S(36))))JumpKey(send,1);ImGui::EndDisabled();ImGui::SameLine();
      ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Shot");ImGui::SameLine();
      if(!NumericEditing("##transport_time"))transport_cursor_=exact_cursor_;
      ImGui::SetNextItemWidth(S(140));ImGui::BeginDisabled(!demo_||keys_.empty());
      if(TimeField("##transport_time",&transport_cursor_,0,exact_duration_))Emit(send,"scrub",{{"time",transport_cursor_},{"seek_demo",sync_demo_}});
      Tip("Editable camera position, relative to the shot start. Sync demo also seeks start tick + this time.");ImGui::EndDisabled();
      ImGui::SameLine();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Duration");ImGui::SameLine();
      if(!NumericEditing("##transport_duration"))transport_duration_=exact_duration_;
      ImGui::SetNextItemWidth(S(140));ImGui::BeginDisabled(!demo_);DurationField(send,"##transport_duration",&transport_duration_);ImGui::EndDisabled();
      ImGui::SameLine();ImGui::SetNextItemWidth(S(85));
      const auto resize=String(Member(editor_preferences_,"resize_mode"),"keep");
      if(ImGui::BeginCombo("##transport_resize",resize=="scale"?"Scale":"Keep")){
        DrawBackdrop();if(ImGui::Selectable("Keep key times",resize=="keep"))SetPreference(send,"resize_mode","keep");
        if(ImGui::Selectable("Scale keys and region",resize=="scale"))SetPreference(send,"resize_mode","scale");
        ImGui::EndCombo();
      }
      Tip("Duration: Keep preserves key times. Scale stretches all keys and the render region together.");
      ImGui::SameLine();ImGui::BeginDisabled(!demo_);if(CameraButton("##capture",CaptureTime()>exact_duration_?"Capture +extend":"Capture",S(160)))Capture(send);CaptureTip();ImGui::EndDisabled();
      ImGui::SameLine();DrawRenderScope(send,S(145),true);ImGui::SameLine();ImGui::BeginDisabled(!demo_);ImGui::AlignTextToFramePadding();if(ImGui::Checkbox("Sync demo",&sync_demo_))Emit(send,"preview",{{"playing",false},{"sync_demo",sync_demo_}});ImGui::EndDisabled();
      if(!duration_error_.empty()||!range_error_.empty()||!capture_error_.empty()){
        ImGui::SameLine();ImGui::TextUnformatted("Action rejected");Tip((!duration_error_.empty()?duration_error_:!range_error_.empty()?range_error_:capture_error_).c_str());
      }else if(cursor_>exact_duration_+1e-6){ImGui::SameLine();ImGui::Text("Outside shot +%.2f s",double(cursor_)-exact_duration_);}

    }
    DrawProjectMenu(send);DrawProjectBrowser(send);ImGui::End();ImGui::PopStyleVar(2);
  }
  void DrawInspector(ImVec2 pos, ImVec2 size, const Json& state, const Send& send) {
    ImGui::SetNextWindowPos(pos);ImGui::SetNextWindowSize(size);
    ImGui::Begin("##CS2CinematicInspector",nullptr,kPanelFlags|ImGuiWindowFlags_NoBackground);
    DrawBackdrop();FontScale();
    const float tab_width=(ImGui::GetContentRegionAvail().x-S(14))/3;
    for(const auto& section:{std::pair<const char*,const char*>{"camera","Camera"},{"shot","Shot"},{"scene","Scene"}}){
      if(section.first!=std::string("camera"))ImGui::SameLine();
      if(inspector_section_==section.first)AccentButton(section.second,ImVec2(tab_width,S(36)));
      else if(NavigationButton(section.second,ImVec2(tab_width,S(36))))Navigate(send,"inspector_section",section.first);
    }
    ImGui::Separator();
    ImGui::BeginChild("##inspector_content",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_NoBackground);
    if(diagnostic_open_shot_){ImGui::SetScrollY(0);diagnostic_open_shot_=false;}
    if(!connected_){Caption("Waiting for game");if(ImGui::Button("Refresh connection")&&send&&ImGui::GetTime()-last_refresh_>.5){send("get_state",Json::object());last_refresh_=ImGui::GetTime();}}
    else Caption(!demo_?"No demo loaded":mode_=="preview"?"Playing camera preview":mode_=="hold"?"Camera held":"Free camera");
    ImGui::BeginDisabled(!connected_);
    if(inspector_section_=="scene"){DrawPresentation(send);DrawSceneSettings(send);DrawCosmetics(send);}
    else if(inspector_section_=="shot")DrawShotInspector(state,send);
    else DrawCameraInspector(send);
    ImGui::EndDisabled();
    if(!range_error_.empty())ImGui::TextWrapped("%s",range_error_.c_str());
    if(!capture_error_.empty())ImGui::TextWrapped("%s",capture_error_.c_str());
    if(!error_.empty()){ImGui::Separator();ImGui::TextColored(ImVec4(.9f,.56f,.42f,1),"%s",error_.c_str());}
    ImGui::EndChild();ImGui::End();
  }
  void DrawShotInspector(const Json& state,const Send& send) {
      Caption("Project");
      ImGui::SetNextItemWidth(-1);
      ImGui::InputText("##shot_name", draft_name_, sizeof(draft_name_));
      if (ImGui::IsItemDeactivatedAfterEdit())
        Emit(send, "set_project", {{"name", std::string(draft_name_)}});
      const float half = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
      const float second_column = ImGui::GetCursorPosX() + half + S(7);
      Caption("Start tick / demo");
      ImGui::SameLine(second_column);
      Caption("Duration");
      ImGui::SetNextItemWidth(half);
      if (NumericField("##shot_start", &draft_start_tick_, 0, 2000000000))
        Emit(send, "set_project", {{"start_tick", std::max(0, draft_start_tick_)}});
      ImGui::SameLine();
      ImGui::SetNextItemWidth(half);
      DurationField(send,"##shot_duration",&draft_duration_);
      if(!duration_error_.empty())ImGui::TextWrapped("%s",duration_error_.c_str());
      if (ImGui::Button("Use current demo tick", ImVec2(-1, 0)))
        Emit(send, "set_project", {{"start_tick", std::max(0, tick_)}});
      Caption("Camera path");
      int interpolation = interpolation_ == "linear" ? 1 : interpolation_=="bezier"?2:0;
      ImGui::SetNextItemWidth(-1);
      const char* path_names[] = {"Smooth", "Linear", "Bezier"};
      if (ImGui::BeginCombo("##interpolation", path_names[interpolation])) {
        DrawBackdrop();
        for (int i = 0; i < 3; ++i)
          if (ImGui::Selectable(path_names[i], interpolation == i))
            Emit(send, "set_project", {{"interpolation", i==2?"bezier":i ? "linear" : "smooth"}});
        ImGui::EndCombo();
      }
      Tip("Smooth blends the spatial path through keys. Linear uses straight segments. "
          "Bezier exposes editable path handles. Timing is independent of path shape.");
      ImGui::Separator();
      DrawRenderRange(send);
      ImGui::Separator();
    DrawSavedShots(state, send);
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
  void DrawCameraInspector(const Send& send) {
    const float tabs_width = (ImGui::GetContentRegionAvail().x - S(7)) / 2;
    if (!inspect_key_) {
      if (AccentButton("Live camera", ImVec2(tabs_width, 0))) {}
    } else if (NavigationButton("Live camera", ImVec2(tabs_width, S(36)))) {
      CancelEditorGesture();
      inspect_key_ = false;
      draft_target_.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(keys_.empty());
    if (inspect_key_) {
      if (AccentButton("Keyframe", ImVec2(tabs_width, 0))) {}
    } else if (NavigationButton("Keyframe", ImVec2(tabs_width, S(36)))) {
      CancelEditorGesture();
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
      CaptureTip();
      ImGui::EndDisabled();
      if (ImGui::Button("Release camera", ImVec2(-1, 0)))
        Emit(send, "release_camera");
      Tip("Return camera control to CS2.");
    }
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
      row("Capture at camera playhead", String(Member(hotkeys_, "capture"), "F6"));
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
      row("Capture in selected source", "K");
      row("Play / pause selected source", "Space");
      row("Camera region In / Out", "I / O");
      row("Remove selected key", "Delete");
      row("Undo", "Ctrl+Z");
      row("Redo", "Ctrl+Shift+Z / Ctrl+Y");
      row("Save project", "Ctrl+S");
      row("Load project", "Ctrl+O");
      row("Save project copy", "Ctrl+Shift+S");
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
      Caption("Shot time");
    }
    ImGui::SetNextItemWidth(width);
    if (NumericField("##fov", &draft_.fov, 1, 179, "%.1f deg"))
      CommitPose(send, "fov", draft_.fov);
    Tip(key_mode ? "Lens angle at this camera key. Different FOV values animate the lens between keys." :
                   "Change the current camera lens without adding a key. Capture a key to save this FOV in the path.");
    if (key_mode) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(width);
      if (TimeField("##key_time", &draft_.time, 0, duration_))
        Emit(send, "update_key", {{"id", selected_id_},
                                  {"time", Clamp(draft_.time, 0, duration_)}});
    }
    ImGui::EndDisabled();
  }
  void DrawEasing(const Send& send) {
    const int index = SelectedIndex();
    const bool has_segment = index >= 0 && index + 1 < static_cast<int>(keys_.size());
    Caption("Timing curve");
    ImGui::SameLine();
    const char* segment = has_segment ? "to next key" : "last key";
    const float x = ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(segment).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
    Caption(segment);
    const char* channel_names[]={"Position","Rotation","FOV"};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-S(117));
    if(ImGui::BeginCombo("##timing_channel",channel_names[easing_channel_])){
      DrawBackdrop();for(int channel=0;channel<3;++channel)if(ImGui::Selectable(channel_names[channel],easing_channel_==channel)){easing_channel_=channel;graph_handle_=-1;graph_changed_=false;}
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    const auto timing_field=camera_curve::Field(easing_channel_,"_easing");
    const auto* raw_key=camera_curve::Find(current_project_,selected_id_);
    const bool overridden=raw_key&&raw_key->contains(timing_field)&&!(*raw_key)[timing_field].is_null();
    ImGui::BeginDisabled(!overridden);
    if(ImGui::Button("Use shared",ImVec2(S(110),0))){draft_.channel_easing[easing_channel_]=draft_.easing;Emit(send,"update_key",{{"id",selected_id_},{timing_field,nullptr}});}
    Tip("Use the shared timing saved with this key. Other channels keep their own timing.");
    ImGui::EndDisabled();
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
    auto& bezier = draft_.channel_easing[easing_channel_];
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
        graph_original_=bezier;
        graph_mouse_=mouse;
      }
    }
    if(graph_handle_>=0 && ImGui::IsKeyPressed(ImGuiKey_Escape,false)){bezier=graph_original_;graph_handle_=-1;graph_changed_=false;ImGui::ClearActiveID();}
    if (graph_handle_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const float dx=mouse.x-graph_mouse_.x,dy=mouse.y-graph_mouse_.y;
      if(std::hypot(dx,dy)>S(2)){
        const float bx=Clamp(graph_original_[graph_handle_]+dx/width,0,1);
        const float by=Clamp(graph_original_[graph_handle_+1]-dy/height,0,1);
        bezier[graph_handle_] = bx;
        bezier[graph_handle_ + 1] = by;
      }else bezier=graph_original_;
      graph_changed_=bezier!=graph_original_;
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (graph_handle_ >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (graph_changed_ && bezier!=graph_original_) Emit(send, "update_key", {{"id", selected_id_}, {camera_curve::Field(easing_channel_,"_easing"), bezier}});
      graph_handle_ = -1;
      graph_changed_ = false;
    }
    h1 = point(bezier[0], bezier[1]);
    h2 = point(bezier[2], bezier[3]);
    camera_curve::TestHandle("##timing_handle_in",h1,S(8));
    camera_curve::TestHandle("##timing_handle_out",h2,S(8));
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
    draw->AddText(ImVec2(lo.x+S(7),lo.y+S(3)),Ink(.4f),"Progress");
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
    if(ImGui::Button("Smooth through key",ImVec2(-1,0))){const auto patch=camera_curve::SmoothThrough(current_project_,selected_id_);if(camera_curve::Changed(current_project_,patch))Emit(send,"set_project",patch);}
    Tip("Auto shape handles for position, rotation and FOV, plus linear timing on both sides of this key. Enables Bezier on all three tracks, including other segments. One undo restores the previous curves.");
  }
  void SetEasing(const Send& send, std::array<float, 4> easing) {
    const auto field=camera_curve::Field(easing_channel_,"_easing");
    const auto* key=camera_curve::Find(current_project_,selected_id_);
    if(draft_.channel_easing[easing_channel_]==easing&&key&&key->contains(field)&&!(*key)[field].is_null())return;
    draft_.channel_easing[easing_channel_] = easing;
    Emit(send, "update_key", {{"id", selected_id_}, {field, easing}});
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
    if (ImGui::MenuItem("Save as new project", "Ctrl+Shift+S")) Emit(send,"save_project_copy");
    if (ImGui::MenuItem("Load project", "Ctrl+O")) OpenProjectBrowser(send);
    ImGui::Separator();
    if (ImGui::MenuItem("Undo", "Ctrl+Z")) Emit(send, "undo");
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z")) Emit(send, "redo");
    ImGui::Separator();
    if(workspace_source_=="demo") {
      if(ImGui::MenuItem("Go to demo beginning",nullptr,false,demo_&&demo_timeline_available_))SeekDemo(send,0);
      if(ImGui::MenuItem("Go to demo end",nullptr,false,demo_&&demo_timeline_available_))SeekDemo(send,demo_total_ticks_);
    }else {
      if(ImGui::MenuItem("Go to shot beginning",nullptr,false,demo_&&!keys_.empty()))Emit(send,"scrub",{{"time",0},{"seek_demo",sync_demo_}});
      if(ImGui::MenuItem("Go to shot end",nullptr,false,demo_&&!keys_.empty()))Emit(send,"scrub",{{"time",exact_duration_},{"seek_demo",sync_demo_}});
    }
    if (ImGui::MenuItem("Keyboard shortcuts", "F10", shortcuts_visible)) {
      shortcuts_visible = !shortcuts_visible;
      shortcuts_focus_requested_ = shortcuts_visible;
    }
    ImGui::EndPopup();
  }
  void OpenProjectBrowser(const Send& send) {
    project_browser_requested_=true;
    project_browser_id_=String(Member(project_storage_,"id"));
    Emit(send,"refresh_project_library");
  }
  void DrawProjectBrowser(const Send& send) {
    if(project_browser_requested_) {
      ImGui::OpenPopup("Camera projects");project_browser_requested_=false;
    }
    ImGui::SetNextWindowSize(ImVec2(S(900),S(570)),ImGuiCond_Appearing);
    const auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    bool open=true;
    if(!ImGui::BeginPopupModal("Camera projects",&open,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse))return;
    DrawBackdrop();FontScale();
    ImGui::TextUnformatted("Saved projects / recent first");
    const auto directory=String(Member(project_library_,"directory"));
    if(!directory.empty())ImGui::TextWrapped("Folder: %s",directory.c_str());
    if(ImGui::Button("Refresh projects"))Emit(send,"refresh_project_library");
    ImGui::SameLine();if(ImGui::Button("Save current project"))Emit(send,"save_project");
    ImGui::SameLine();if(ImGui::Button("Save a new copy"))Emit(send,"save_project_copy");
    const auto libraryError=String(Member(project_library_,"error"));
    if(!libraryError.empty())ImGui::TextWrapped("%s",libraryError.c_str());
    if(!error_.empty())ImGui::TextWrapped("%s",error_.c_str());
    const Json& entries=Member(project_library_,"entries");
    const Json* selected=nullptr;
    if(entries.is_array())for(const auto& entry:entries)if(String(Member(entry,"id"))==project_browser_id_)selected=&entry;
    ImGui::Separator();
    const float listHeight=std::max(S(180),ImGui::GetContentRegionAvail().y-S(64));
    ImGui::BeginChild("##saved_project_list",ImVec2(S(370),listHeight),ImGuiChildFlags_Borders);
    if(entries.is_array())for(const auto& entry:entries) {
      const auto id=String(Member(entry,"id"));
      if(id.empty())continue;
      const auto title=String(Member(entry,"title"),id.c_str());
      ImGui::PushID(id.c_str());
      if(ImGui::Selectable(title.c_str(),id==project_browser_id_)){project_browser_id_=id;selected=&entry;}
      ImGui::TextDisabled("%d keys  /  %.2f s",Integer(Member(entry,"key_count")),ExactNumber(Member(entry,"duration")));
      ImGui::PopID();
    }
    if(!entries.is_array() || entries.empty())ImGui::TextWrapped("No saved projects yet. Save your current camera shot, or place portable project JSON files in this folder.");
    ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("##saved_project_details",ImVec2(0,listHeight));
    if(selected) {
      ImGui::TextWrapped("%s",String(Member(*selected,"title")).c_str());
      ImGui::TextWrapped("%s.json",project_browser_id_.c_str());
      ImGui::Separator();
      ImGui::Text("%d camera keys",Integer(Member(*selected,"key_count")));
      ImGui::Text("Duration: %.3f s",ExactNumber(Member(*selected,"duration")));
      ImGui::Text("Start tick: %d",Integer(Member(*selected,"start_tick")));
      const auto path=String(Member(Member(*selected,"demo"),"path"));
      if(!path.empty())ImGui::TextWrapped("Recording: %s",path.c_str());
      const auto reason=String(Member(*selected,"reason"));
      const auto warning=String(Member(*selected,"warning"));
      if(!reason.empty())ImGui::TextWrapped("%s",reason.c_str());
      else if(!warning.empty())ImGui::TextWrapped("%s",warning.c_str());
      else ImGui::TextWrapped("Recorded camera motion, FOV, timing curves, scene settings and render range are restored together. Undo restores your current project.");
    }else ImGui::TextWrapped("Choose a project to inspect its camera track and recording.");
    const int skipped=Integer(Member(project_library_,"skipped"));
    if(skipped)ImGui::TextWrapped("Skipped %d invalid or unavailable files.",skipped);
    if(Boolean(Member(project_library_,"truncated")))ImGui::TextWrapped("Folder scan reached its limit. Move older files to an archive and refresh.");
    ImGui::EndChild();ImGui::Separator();
    ImGui::BeginDisabled(!selected || !Boolean(Member(*selected,"compatible")));
    if(ImGui::Button("Open selected project",ImVec2(S(220),S(36)))) {
      Emit(send,"load_saved_project",{{"id",project_browser_id_}});ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Load recovery copy",ImVec2(S(200),S(36)))){Emit(send,"load_project");ImGui::CloseCurrentPopup();}
    Tip("Open the legacy last-project quick-save when recovering an older session.");
    ImGui::SameLine();if(ImGui::Button("Cancel##project_browser",ImVec2(S(100),S(36))))ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  void DrawEditorSettings(const Send& send) {
    const auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(S(570),display.x*.82f),std::min(S(740),std::max(S(280),display.y-S(220)))),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(40),S(50)),ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("Editor settings",&settings_visible,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoBackground)){ImGui::End();return;}
    DrawBackdrop(false);FontScale();
    ImGui::TextUnformatted("Camera editor");ImGui::TextDisabled("Preferences are saved separately from the shot.");
    ImGui::BeginDisabled(!connected_);
    auto choice=[&](const char* caption,const char* field,const char* fallback,std::initializer_list<std::pair<const char*,const char*>> choices){
      Caption(caption);ImGui::SetNextItemWidth(-1);const auto value=String(Member(editor_preferences_,field),fallback);const char* label=value.c_str();for(const auto& option:choices)if(value==option.first)label=option.second;
      const std::string id=std::string("##pref_")+field;
      if(ImGui::BeginCombo(id.c_str(),label)){DrawBackdrop();for(const auto& option:choices)if(ImGui::Selectable(option.second,value==option.first)&&value!=option.first)SetPreference(send,field,option.first);ImGui::EndCombo();}
    };
    auto numeric=[&](const char* caption,const char* field,double fallback,double low,double high,const char* format,bool integral=false){
      Caption(caption);auto insertion=preference_drafts_.emplace(field,PreferenceNumber(field,fallback,low,high));auto& value=insertion.first->second;
      if(!was_item_active_)value=PreferenceNumber(field,fallback,low,high);
      ImGui::SetNextItemWidth(-1);const std::string id=std::string("##pref_")+field;
      if(integral){int integer=int(std::llround(value));const bool commit=NumericField(id.c_str(),&integer,low,high);value=integer;if(commit)SetPreference(send,field,integer);}
      else if(NumericField(id.c_str(),&value,low,high,format))SetPreference(send,field,value);
    };
    auto toggle=[&](const char* caption,const char* field,bool fallback=true){bool value=PreferenceBool(field,fallback);if(ImGui::Checkbox(caption,&value))SetPreference(send,field,value);};
    ImGui::SetNextItemOpen(true,ImGuiCond_Once);
    if(ImGui::CollapsingHeader("Time and editing")){
      choice("Time display","time_unit","seconds",{{"seconds","Seconds"},{"ticks","Demo ticks"},{"frames","Frames"}});
      ImGui::TextDisabled("Demo tick rate: %d Hz",tick_rate_);
      numeric("Frame grid rate","frame_rate",60,1,1000,"%.0f fps",true);
      Tip("Frame numbers and frame snapping use this rate. It does not change the recording or export frame rate.");
      choice("Snap to","snap_mode","tick",{{"tick","Demo tick"},{"frame","Frame"},{"off","Off"}});
      Tip("Hold Shift while dragging to temporarily ignore snapping.");
      choice("When changing shot duration","resize_mode","keep",{{"keep","Keep key times"},{"scale","Scale keys and render region"}});
      Tip("Keep preserves key times and will not trim past the last key. Scale stretches all key times and In/Out with the duration.");
    }
    ImGui::SetNextItemOpen(true,ImGuiCond_Once);
    if(ImGui::CollapsingHeader("Layout and appearance")){
      toggle("Keep demo navigation visible","demo_navigation_pinned",false);
      numeric("Interface scale","ui_scale",1,.75,1.5,"%.2fx");
      numeric("Inspector width","inspector_width",430,320,640,"%.0f px",true);
      numeric("Timeline height","timeline_height",240,160,420,"%.0f px",true);
      numeric("Panel opacity","panel_opacity",.94,.3,1,"%.2f");
      toggle("Blur behind panels","blur_enabled");toggle("Show key labels","show_key_labels");toggle("Show tooltips","show_tooltips");
    }
    ImGui::SetNextItemOpen(true,ImGuiCond_Once);
    if(ImGui::CollapsingHeader("Timeline tracks")){
      toggle("Position track","timeline_position");toggle("Rotation track","timeline_rotation");toggle("FOV track","timeline_fov");
    }
    ImGui::EndDisabled();ImGui::End();
  }
  void DrawCompactCameraTransport(const Send& send,float width){
    ImGui::SetCursorPos(ImVec2(S(5),S(96)));
    ImGui::BeginDisabled(keys_.empty()||!demo_);
    if(IconButton("##preview",playing_?Icon::Pause:Icon::Play,playing_?"Pause camera preview. Space":"Play camera preview. Space",ImVec2(S(44),S(36))))Emit(send,"preview",{{"playing",!playing_},{"sync_demo",sync_demo_&&demo_}});
    ImGui::SameLine(0,S(3));if(IconButton("##prev",Icon::First,"Previous keyframe",ImVec2(S(32),S(36))))JumpKey(send,-1);
    ImGui::SameLine(0,S(3));if(IconButton("##next",Icon::Last,"Next keyframe",ImVec2(S(32),S(36))))JumpKey(send,1);
    ImGui::EndDisabled();ImGui::SameLine(0,S(8));
    const auto time_pos=ImGui::GetCursorPos();char current[48],total[48];const auto unit=TimeUnit();
    if(unit=="seconds"){std::snprintf(current,sizeof(current),"%.2f",double(cursor_));std::snprintf(total,sizeof(total),"%.2f s",exact_duration_);}
    else{std::snprintf(current,sizeof(current),"%.1f",cursor_*TimeFactor());std::snprintf(total,sizeof(total),"%.1f %s",exact_duration_*TimeFactor(),unit=="ticks"?"tk":"fr");}
    ImGui::AlignTextToFramePadding();if(mono_font)ImGui::PushFont(mono_font);ImGui::Text("%s / %s",current,total);if(mono_font)ImGui::PopFont();
    Tip("Camera time / shot duration. Choose seconds, ticks or frames in Settings.");
    const float time_width=std::clamp(ImGui::GetItemRectSize().x+S(12),S(160),S(240));
    ImGui::SetCursorPos(ImVec2(time_pos.x+time_width,time_pos.y));
    ImGui::BeginDisabled(!demo_);if(CameraButton("##capture","Capture",S(108)))Capture(send);ImGui::EndDisabled();
    ImGui::SameLine(0,S(6));DrawRenderScope(send,S(145),true);
    ImGui::SameLine(0,S(6));ImGui::BeginDisabled(!demo_||keys_.empty());if(ToolbarButton("Export",ImVec2(S(80),S(36))))Emit(send,"request_export");ImGui::EndDisabled();
    ImGui::SameLine(0,S(6));if(CameraButton("##camera_inspector","Camera",S(106)))show_inspector_=!show_inspector_;
    ImGui::SameLine(0,S(6));ImGui::BeginDisabled(!demo_);ImGui::AlignTextToFramePadding();if(ImGui::Checkbox("Sync demo",&sync_demo_))Emit(send,"preview",{{"playing",false},{"sync_demo",sync_demo_}});ImGui::EndDisabled();
    const bool wide=width-(ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x)>S(340);
    if(wide){ImGui::SameLine(0,S(10));if(ToolbarButton("Curves",ImVec2(S(78),S(36))))camera_curves_.visible=true;
      ImGui::SameLine(0,S(6));if(ToolbarButton("Timeline",ImVec2(S(90),S(36))))camera_timeline_.visible=true;
      ImGui::SameLine(0,S(6));if(ToolbarButton("Settings",ImVec2(S(92),S(36))))settings_visible=true;
    }
    ImGui::SameLine(0,S(6));if(ToolbarButton("...##project",ImVec2(S(36),S(36))))ImGui::OpenPopup("##camera_project_menu");
    Tip("Project, editor windows and keyboard shortcuts.");
  }
  void DrawTransport(ImVec2 pos, ImVec2 size, const Send& send) {
    // Two explicit native-sized strips: full demo navigation above the
    // authored camera shot. Demo gestures never modify camera keys or In/Out.
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
    DrawDemoTimeline(send);
    DrawDemoTransport(send,size.x);
    if(PreferenceBool("compact_transport",true))DrawCompactCameraTransport(send,size.x);
    else {
    ImGui::SetCursorPos(ImVec2(0,S(92)));
    DrawTimeline(send);
    ImGui::SetCursorPos(ImVec2(S(5), S(142)));
    ImGui::BeginDisabled(keys_.empty() || !demo_);
    if (IconButton("##preview", playing_ ? Icon::Pause : Icon::Play,
                   playing_ ? "Pause camera preview. Space" : "Play camera preview. Space",
                   ImVec2(S(82), S(41))))
      Emit(send, "preview", {{"playing", !playing_}, {"sync_demo", sync_demo_ && demo_}});
    ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(S(102), S(153)));
    ImGui::TextUnformatted("Camera shot");
    char current[24], total[24], precise[32];
    const float time = timeline_active_ ? timeline_drag_time_ : cursor_;
    std::snprintf(current, sizeof(current), "%02d:%02d", int(time) / 60, int(time) % 60);
    std::snprintf(total, sizeof(total), "%02d:%02d", int(duration_) / 60, int(duration_) % 60);
    ImGui::SetCursorPos(ImVec2(S(190), S(153)));
    if (mono_font) ImGui::PushFont(mono_font);
    ImGui::Text("Shot:  %s / %s", current, total);
    if (mono_font) ImGui::PopFont();
    TimeText(precise, sizeof(precise), time);
    if (PreferenceBool("show_tooltips",true) && ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      DrawBackdrop();
      ImGui::Text("Camera time: %s", precise);
      ImGui::EndTooltip();
    }
    const float controls_x = std::max(S(505), ImGui::GetItemRectMax().x - pos.x + S(30));
    ImGui::SetCursorPos(ImVec2(controls_x, S(144.5f)));
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
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(181.f/255, 212.f/255, 238.f/255, 1));
    if (ToolbarButton("...##project", ImVec2(S(32), S(36))))
      ImGui::OpenPopup("##camera_project_menu");
    ImGui::PopStyleColor();
    Tip(("Project: " + project_name_ + ". Save, load, undo and redo.").c_str());
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
    const float right_start = size.x - S(270);
    ImGui::SetCursorPos(ImVec2(std::max(right_start, ImGui::GetItemRectMax().x - pos.x + S(20)), S(144.5f)));
    if (CameraButton("##camera_inspector", "Camera", S(116))) show_inspector_ = !show_inspector_;
    Tip("Show or hide camera inspector.");
    ImGui::SameLine(0, S(14));
    ImGui::BeginDisabled(!demo_);
    ImGui::AlignTextToFramePadding();
    if (ImGui::Checkbox("Sync demo", &sync_demo_))
      Emit(send, "preview", {{"playing", false}, {"sync_demo", sync_demo_}});
    Tip("Camera scrubbing and preview also control the demo.");
    ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    DrawProjectMenu(send);
    DrawProjectBrowser(send);
    ImGui::End();
  }
  void DrawDemoTransport(const Send& send,float width) {
    ImGui::SetCursorPos(ImVec2(S(10),S(61)));Caption("Full demo");
    ImGui::SetCursorPos(ImVec2(S(115),S(52.5f)));
    ImGui::BeginDisabled(!demo_);
    if(ToolbarButton(paused_?"Play demo":"Pause demo",ImVec2(S(105),S(36))))
      Emit(send,"demo_control",{{"action",paused_?"resume":"pause"}});
    ImGui::SameLine(0,S(10));ImGui::BeginDisabled(!demo_timeline_available_);
    if(ToolbarButton("-15 s##demo_back",ImVec2(S(64),S(36))))SeekDemo(send,int(std::max(0.0,double(tick_)-std::round(demo_rate_*15))));
    Tip("Seek the demo back 15 seconds. Camera keys are unchanged.");
    ImGui::SameLine();
    if(ToolbarButton("+15 s##demo_forward",ImVec2(S(64),S(36))))SeekDemo(send,int(std::min(double(demo_total_ticks_),double(tick_)+std::round(demo_rate_*15))));
    Tip("Seek the demo forward 15 seconds. Camera keys are unchanged.");
    ImGui::SameLine(0,S(14));ImGui::BeginDisabled(demo_rounds_.empty());
    if(IconButton("##prev_round",Icon::First,"Previous round"))JumpRound(send,-1);
    ImGui::SameLine();if(IconButton("##next_round",Icon::Last,"Next round"))JumpRound(send,1);
    ImGui::EndDisabled();ImGui::EndDisabled();ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(S(500),S(61)));Caption("Tick");
    ImGui::SetCursorPos(ImVec2(S(545),S(52.5f)));
    if(!NumericEditing("##demo_current_tick"))transport_tick_=tick_;
    ImGui::SetNextItemWidth(S(130));ImGui::BeginDisabled(!demo_||!demo_timeline_available_);
    if(NumericField("##demo_current_tick",&transport_tick_,0,std::max(0,demo_total_ticks_)))SeekDemo(send,transport_tick_);
    Tip("Editable absolute playback tick of the full recording.");ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(S(10),S(95)));ImGui::BeginDisabled(!demo_||!demo_timeline_available_);
    bool limited=demo_range_enabled_;
    if(ImGui::Checkbox("Limit playback",&limited))Emit(send,"set_demo_playback_range",{{"enabled",limited}});
    Tip("Limit demo playback to absolute In/Out ticks. This does not change camera keys or the shot render region.");
    ImGui::SetCursorPos(ImVec2(S(190),S(104)));Caption("In tick");ImGui::SetCursorPos(ImVec2(S(252),S(95)));
    if(!NumericEditing("##demo_range_in"))draft_demo_in_=demo_range_in_;
    ImGui::SetNextItemWidth(S(120));
    if(NumericField("##demo_range_in",&draft_demo_in_,0,std::max(0,demo_range_out_-1)))Emit(send,"set_demo_playback_range",{{"in_tick",draft_demo_in_}});
    ImGui::SetCursorPos(ImVec2(S(385),S(104)));Caption("Out tick");ImGui::SetCursorPos(ImVec2(S(455),S(95)));
    if(!NumericEditing("##demo_range_out"))draft_demo_out_=demo_range_out_;
    ImGui::SetNextItemWidth(S(120));
    if(NumericField("##demo_range_out",&draft_demo_out_,std::min(demo_total_ticks_,demo_range_in_+1),std::max(0,demo_total_ticks_)))Emit(send,"set_demo_playback_range",{{"out_tick",draft_demo_out_}});
    ImGui::EndDisabled();ImGui::SetCursorPos(ImVec2(S(595),S(104)));
    if(mono_font)ImGui::PushFont(mono_font);
    if(demo_timeline_available_) {
      const double time=std::clamp(double(demo_timeline_active_?demo_drag_tick_:tick_)/demo_rate_,0.0,demo_duration_);
      const int total=int(demo_duration_),current=int(time);
      ImGui::Text("%02d:%02d / %02d:%02d",current/60,current%60,total/60,total%60);
    }else ImGui::TextUnformatted("Unavailable");
    if(mono_font)ImGui::PopFont();
    const float marker_x=S(115),marker_width=std::min(S(300),std::max(S(150),width-S(270)));
    ImGui::SetCursorPos(ImVec2(S(10),S(146)));Caption("Event player");
    ImGui::SetCursorPos(ImVec2(marker_x,S(137)));DrawTimelinePlayerSelector(send,marker_width);
    ImGui::SameLine();if(demo_timeline_available_&&!demo_event_player_.empty())ImGui::Text("Kills %zu / Deaths %zu",demo_kills_.size(),demo_deaths_.size());
    if(workspace_source_=="demo") {
      ImGui::SameLine();ImGui::BeginDisabled(!demo_);
      if(CameraButton("##demo_capture",CaptureTime()>exact_duration_?"Capture +extend":"Capture key",S(160)))Capture(send);
      CaptureTip();ImGui::EndDisabled();
      if(!capture_error_.empty()){ImGui::SetCursorPos(ImVec2(S(10),S(181)));ImGui::TextWrapped("%s",capture_error_.c_str());}
    }
    if(!demo_timeline_available_&&capture_error_.empty()){ImGui::SetCursorPos(ImVec2(S(10),S(181)));ImGui::TextWrapped("Playback limits unavailable: %s",demo_timeline_reason_.c_str());}
  }
  void DrawDemoTimeline(const Send& send) {
    const ImVec2 pos=ImGui::GetCursorScreenPos(),size(ImGui::GetContentRegionAvail().x,S(50));
    ImGui::BeginDisabled(!demo_ || !demo_timeline_available_);ImGui::InvisibleButton("##demo_timeline",size);const bool hovered=ImGui::IsItemHovered();ImGui::EndDisabled();
    auto* draw=ImGui::GetWindowDrawList();const float left=pos.x+S(10),right=pos.x+size.x-S(10),width=std::max(1.f,right-left);
    const auto tx=[&](int tick){return left+float(std::clamp(double(tick)/std::max(1,demo_total_ticks_),0.0,1.0))*width;};
    const float top=pos.y+S(30),bottom=pos.y+S(40);
    draw->AddRectFilled(ImVec2(left,top),ImVec2(right,bottom),IM_COL32(204,204,204,32));
    if(!demo_timeline_available_) {
      const std::string message="Full demo timeline: "+demo_timeline_reason_;
      draw->AddText(ImVec2(left,pos.y+S(8)),Ink(.7f),message.c_str());Tip(message.c_str());return;
    }
    int start=0;for(size_t i=0;i<=demo_rounds_.size();++i) {
      const int end=i<demo_rounds_.size()?demo_rounds_[i].tick:demo_total_ticks_;
      if(end>start)draw->AddRectFilled(ImVec2(tx(start),top),ImVec2(tx(end),bottom),IM_COL32(255,255,255,i%2?80:32));
      start=end;
    }
    for(size_t i=0;i<demo_rounds_.size();++i) {
      const auto& round=demo_rounds_[i];const int end=i+1<demo_rounds_.size()?demo_rounds_[i+1].tick:demo_total_ticks_;
      const float x=tx(round.tick);draw->AddLine(ImVec2(x,top),ImVec2(x,bottom),Ink(.35f));
      if(round.number>0 && tx(end)-x>S(35)) {
        char label[24];std::snprintf(label,sizeof(label),"R%d",round.number);
        draw->AddText(ImGui::GetFont(),S(11),ImVec2(x+S(4),top-S(1)),Ink(.75f),label);
      }
    }
    struct EventHit {int tick=-1;bool death=false;float distance=100000;};EventHit hit;
    const ImVec2 mouse=ImGui::GetIO().MousePos;
    const auto events=[&](const std::vector<int>& ticks,bool death) {
      for(int tick:ticks) {
        const float x=tx(tick),y=pos.y+S(death?21:9);const ImU32 ink=death?IM_COL32(215,130,130,255):IM_COL32(140,205,140,255);
        if(death) {draw->AddLine(ImVec2(x-S(4),y-S(4)),ImVec2(x+S(4),y+S(4)),ink,S(2));draw->AddLine(ImVec2(x-S(4),y+S(4)),ImVec2(x+S(4),y-S(4)),ink,S(2));}
        else {draw->AddCircle(ImVec2(x,y),S(3),ink,8,S(1.5f));draw->AddLine(ImVec2(x-S(5),y),ImVec2(x+S(5),y),ink);draw->AddLine(ImVec2(x,y-S(5)),ImVec2(x,y+S(5)),ink);}
        const float distance=std::hypot(mouse.x-x,mouse.y-y);
        if(hovered && distance<=S(9) && distance<hit.distance)hit={tick,death,distance};
      }
    };
    events(demo_kills_,false);events(demo_deaths_,true);
    const auto mouseTick=[&]{return int(std::llround(std::clamp(double(mouse.x-left)/width,0.0,1.0)*demo_total_ticks_));};
    if(hovered && connected_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      demo_timeline_active_=true;demo_timeline_moved_=false;demo_drag_start_x_=mouse.x;demo_drag_tick_=hit.tick>=0?hit.tick:mouseTick();
    }
    if(demo_timeline_active_) {
      if(ImGui::IsKeyPressed(ImGuiKey_Escape,false))demo_timeline_active_=false;
      else if(ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if(std::abs(mouse.x-demo_drag_start_x_)>S(2))demo_timeline_moved_=true;
        if(demo_timeline_moved_)demo_drag_tick_=mouseTick();
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      }else {SeekDemo(send,demo_drag_tick_);demo_timeline_active_=false;}
    }
    if(demo_seek_pending_) {
      const float destination=tx(demo_seek_target_);
      const ImU32 pending=IM_COL32(235,190,105,255);
      draw->AddLine(ImVec2(destination,pos.y+S(2)),ImVec2(destination,bottom+S(4)),pending,S(1));
      draw->AddCircle(ImVec2(destination,pos.y+S(5)),S(4),pending,8,S(1.5f));
      char status[96];std::snprintf(status,sizeof(status),"Seeking: tick %d  ->  %d",tick_,demo_seek_target_);
      const ImVec2 text_size=ImGui::CalcTextSize(status);
      draw->AddText(ImVec2(std::max(left,right-text_size.x-S(8)),pos.y+S(3)),pending,status);
    }
    const float playhead=tx(demo_timeline_active_?demo_drag_tick_:tick_);
    draw->AddLine(ImVec2(playhead,pos.y+S(2)),ImVec2(playhead,bottom+S(4)),Ink(.9f),S(2));
    draw->AddTriangleFilled(ImVec2(playhead-S(5),pos.y),ImVec2(playhead+S(5),pos.y),ImVec2(playhead,pos.y+S(6)),Ink());
    if(hovered && PreferenceBool("show_tooltips",true)) {
      ImGui::BeginTooltip();DrawBackdrop();
      const int tick=hit.tick>=0?hit.tick:mouseTick();
      ImGui::Text("Demo %02d:%02d  Tick %d",int(tick/demo_rate_)/60,int(tick/demo_rate_)%60,tick);
      if(hit.tick>=0)ImGui::Text("%s: %s",demo_event_player_.c_str(),hit.death?"Death":"Kill");
      else ImGui::TextUnformatted("Click or drag to seek the full demo. Camera keys and render region stay unchanged.");
      if(demo_seek_pending_)ImGui::Text("Seeking to tick %d. The white playhead shows the current game tick.",demo_seek_target_);
      ImGui::EndTooltip();
    }
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
    // Only the visible lower bracket triangles trim a selected render
    // region. The slider row always scrubs, including its very first/last
    // ticks; composition mode has no active hidden trimming handles.
    if (render_region_ && hovered && ImGui::GetIO().MousePos.y >= pos.y + S(39) &&
        ImGui::GetIO().MousePos.y <= pos.y + S(48)) {
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
          if (!ImGui::GetIO().KeyShift) time = SnapTime(time);
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
    if(timeline_active_ && ImGui::IsKeyPressed(ImGuiKey_Escape,false)) {
      timeline_active_=false;timeline_drag_id_.clear();
    }
    if (timeline_active_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const float delta = ImGui::GetIO().MousePos.x - timeline_drag_start_x_;
      if (std::abs(delta) > S(2)) timeline_moved_ = true;
      if (timeline_drag_id_.empty())
        timeline_drag_time_ = Clamp((ImGui::GetIO().MousePos.x - left) / width, 0, 1) * duration_;
      else if (timeline_moved_)
        timeline_drag_time_ = Clamp(timeline_original_time_ + delta / width * duration_, 0, duration_);
      if (!ImGui::GetIO().KeyShift)
        timeline_drag_time_ = Clamp(float(SnapTime(timeline_drag_time_)), 0, duration_);
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
    // Region brackets are visible only for Render region. Their lower
    // triangles trim; the slider row above remains available for scrubbing.
    for (int edge = 0; render_region_ && edge < 2; ++edge) {
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
    if (range_drag_handle_ >= 0 && PreferenceBool("show_tooltips",true)) {
      char time[32];
      TimeText(time, sizeof(time), range_drag_time_);
      ImGui::BeginTooltip();
      DrawBackdrop();
      ImGui::Text("%s  %s", range_drag_handle_ ? "Out" : "In", time);
      ImGui::EndTooltip();
    }
    if (timeline_active_ && PreferenceBool("show_tooltips",true)) {
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
    if(SafeExitBusy())return;
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
      Emit(send, io.KeyShift?"save_project_copy":"save_project");
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
      OpenProjectBrowser(send);
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false) && demo_ && !keys_.empty())
      Emit(send, "request_export");
    else if(!io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Space,false)&&demo_) {
      if(workspace_source_=="demo")Emit(send,"demo_control",{{"action",paused_?"resume":"pause"}});
      else if(!keys_.empty())Emit(send,"preview",{{"playing",!playing_},{"sync_demo",sync_demo_}});
    }
    else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false))
      Capture(send);
    else if (workspace_source_=="camera" && !io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false))
      SetRangeBoundary(send, false);
    else if (workspace_source_=="camera" && !io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_O, false))
      SetRangeBoundary(send, true);
    else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && inspect_key_ && Selected() && KeyEditingFocused())
      Emit(send, "remove_key", {{"id", selected_id_}});
  }
};

}  // namespace cs2_live_ui
