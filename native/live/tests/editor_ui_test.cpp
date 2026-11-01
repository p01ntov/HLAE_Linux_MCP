// Headless interaction regression for the real editor and its JSON callbacks.
// ImGui's item hooks locate controls by label/ID, avoiding screenshot coordinates,
// private EditorUi access, platform input backends, and installed game fonts.
#include "../editor_ui.h"
#include "../shot_library.h"

#include <iostream>
#include <fstream>
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

bool WindowMatches(const std::string& actual,const std::string& expected){return actual==expected||actual.find("/"+expected+"_")!=std::string::npos||(expected==kInspector&&actual.rfind(expected+"/",0)==0);}
ImGuiWindow* TestWindow(const char* name){
  if(auto* exact=ImGui::FindWindowByName(name))return exact;
  for(auto* window:ImGui::GetCurrentContext()->Windows)if(WindowMatches(window->Name,name))return window;
  return nullptr;
}

ImGuiWindow* TestContentWindow(const char* name){
  if(std::string(name)==kInspector)for(auto* window:ImGui::GetCurrentContext()->Windows)if(std::string(window->Name).find("##CS2CinematicInspector/##inspector_content_")==0)return window;
  return TestWindow(name);
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
  std::vector<Command> wire_commands;
  Capture submitted;
  float scale;
  bool echo_scene = true, log_next = false;
  std::string logged_text;
  std::function<void(const std::string&,const Json&)> command_sink;

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
        {"editor_preferences",{{"compact_transport",false}}}, {"editor_preferences_revision",0},
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
      if(log_next)ImGui::LogToBuffer();
      editor.Draw(state, [&](const std::string& wire_method, const Json& wire_args) {
        wire_commands.push_back({wire_method,wire_args});
        const bool guarded=wire_method=="guarded_mutation";
        const std::string method=guarded?wire_args.at("method").get<std::string>():wire_method;
        const Json& args=guarded?wire_args.at("params"):wire_args;
        Require(method!="set_project"||!args.contains("keys")||guarded,"Full UI key-list mutation bypassed revision guard");
        commands.push_back({method, args});
        if(command_sink){command_sink(wire_method,wire_args);return;}
        if(guarded){Require(wire_args.size()==3&&method=="set_project"&&args.contains("keys"),"Malformed UI key-list guard");
          if(wire_args["expected_revision"]!=state["revision"]){state["error"]="camera changed during geometry validation; retry from current state";return;}}
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
        } else if (method == "set_demo_playback_range") {
          state["demo_playback_range"].update(args);return;
        } else if(method=="request_safe_exit") {
          state["safe_exit_request_id"]=state.value("safe_exit_request_id",0)+1;
          state["safe_exit"]={{"phase","requested"},{"error",""}};return;
        } else if(method=="set_safe_exit_status") {
          state["safe_exit"]={{"phase",args.at("phase")},{"error",args.value("error",std::string())}};return;
        } else if (method == "set_camera") {
          state["camera"].update(args);
        } else if(method=="set_editor_preferences"){
          state["editor_preferences"].update(args);state["editor_preferences_revision"]=state.value("editor_preferences_revision",0)+1;return;
        } else if (method == "set_scene" && echo_scene) {
          for (auto setting = args["settings"].begin(); setting != args["settings"].end(); ++setting)
            state["scene_capabilities"][setting.key()]["value"] = setting.value();
        }
        state["revision"] = state["revision"].get<int>() + 1;
      });
      if(log_next){logged_text=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();log_next=false;}
      ImGui::Render();
    } while (ImGui::GetCurrentContext()->InputEventsQueue.Size != 0);
  }
  std::string DrawText(){log_next=true;Step();return logged_text;}
  Item Find(const char* label, size_t index = 0, const char* window = kInspector) const {
    for (ImGuiID id : submitted.order) {
      const Item& item = submitted.items.at(id);
      if (WindowMatches(item.window,window) && item.label == label) {
        if (index == 0) return item;
        --index;
      }
    }
    // BeginCombo in pinned ImGui registers ItemAdd without ItemInfo; locate
    // its ordinary, unscoped ID using the same captured item registry.
    if (index == 0) {
      if (auto* target = TestContentWindow(window)) {
        const auto found = submitted.items.find(target->GetID(label));
        if (found != submitted.items.end() && WindowMatches(found->second.window,window))
          return found->second;
      }
    }
    std::string present;
    for (const auto& entry : submitted.items)
      if (WindowMatches(entry.second.window,window) && !entry.second.label.empty()) present += entry.second.label + ", ";
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
  void Section(const char* label){Click(label);Step();commands.clear();}
  void Tool(const char* label){Click(label,0,"##CS2CinematicTransport");Step();commands.clear();}
  void ClickScoped(const char* label,const char* scope,const char* parent=nullptr) {
    auto* window=TestContentWindow(kInspector);
    Require(window!=nullptr,"Inspector is missing");
    const ImGuiID seed=parent?ImHashStr(scope,0,window->GetID(parent)):window->GetID(scope);
    const auto item=submitted.items.find(ImHashStr(label,0,seed));
    Require(item!=submitted.items.end(),std::string("Scoped control missing: ")+scope+"/"+label);
    ClickAt(item->second.bounds.GetCenter());
  }
  void Choose(const char* label) {
    for(const auto& entry:submitted.items)
      if(entry.second.label==label && entry.second.window!=kInspector) {
        Click(label,0,entry.second.window.c_str());return;
      }
    throw std::runtime_error(std::string("Popup choice missing: ")+label);
  }
  void Key(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    Step();
    ImGui::GetIO().AddKeyEvent(key, false);
    Step();
  }
  void SelectKey() {
    Click("Camera");Step();
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

void CameraMapGestures(float height) {
  Fixture f(height);
  f.state["project_document_id"]=51;
  const Json original_keys=f.state["project"]["keys"];
  const Json original=original_keys[0];
  f.Tool("Map");f.Step();
  constexpr const char* window="Camera map / XY";
  auto canvas=f.Find("##camera_map_canvas",0,window).bounds;
  auto first=f.Find("##map_handle_0",0,window).bounds.GetCenter();
  f.commands.clear();f.Mouse(first);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();
  f.Mouse(ImVec2(first.x+36*f.scale,first.y-20*f.scale));f.Step();
  Require(f.commands.empty(),"Map drag emitted a write before release");
  ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.commands.size()==1 && f.commands[0].method=="update_key","Map drag must commit one update_key");
  const auto& moved=f.state["project"]["keys"][0];
  Require(moved["position"][0].get<double>()>original["position"][0].get<double>() && moved["position"][1].get<double>()>original["position"][1].get<double>(),"Map drag did not change XY in world-axis direction");
  Require(moved["position"][2]==original["position"][2],"Map drag changed Z");
  for(const char* field:{"id","time","angles","fov","easing"})Require(moved[field]==original[field],std::string("Map drag changed ")+field);
  Require(f.commands[0].arguments.size()==2,"Map drag wrote fields beyond id and position");
  f.state["project"]["keys"]=original_keys;f.Step();
  f.Click("Fit path",0,window);f.Step();canvas=f.Find("##camera_map_canvas",0,window).bounds;
  // Cancel on a known handle after re-fitting; no history item may be created.
  first=f.Find("##map_handle_0",0,window).bounds.GetCenter();
  f.commands.clear();f.Mouse(first);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();
  f.Mouse(ImVec2(first.x+50*f.scale,first.y));f.Step();f.Key(ImGuiKey_Escape);
  ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();Require(f.commands.empty(),"Cancelled map drag created history");
  // The live camera can also move in XY without overwriting its lens/rotation.
  f.state["camera"]["position"]={395.6,-1700.,77.125};f.Step();
  const Json original_camera=f.state["camera"];
  auto camera_point=f.Find("##map_handle_@camera",0,window).bounds.GetCenter();
  f.commands.clear();f.Mouse(camera_point);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();
  f.Mouse(ImVec2(camera_point.x+20*f.scale,camera_point.y+15*f.scale));f.Step();
  ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.commands.size()==1 && f.commands[0].method=="set_camera","Live map handle did not commit one set_camera");
  Require(f.commands[0].arguments.size()==1 && f.state["camera"]["position"][2]==original_camera["position"][2],"Live map handle modified Z or other camera fields");
  Require(f.state["camera"]["angles"]==original_camera["angles"] && f.state["camera"]["fov"]==original_camera["fov"],"Live map handle changed orientation/lens");
  f.commands.clear();
  // An actual model slice is optional and explicitly distinct from collision proof.
  f.state["map_preview"]={{"map","test"},{"status","partial"},{"reason","Static slice; dynamic objects unverified"},{"bounds",{-1000,-2500,1000,0}},
    {"layers",Json::array({{{"z",0},{"lines",Json::array({Json::array({-500,-1800,500,-1800})})}}})}};
  f.Step();Require(f.DrawText().find("Doors, glass and moving objects require visual review")!=std::string::npos,"Map coverage status is missing");
  f.Click("Fit map",0,window);f.Step();
  f.Mouse(canvas.GetCenter());ImGui::GetIO().AddMouseWheelEvent(0,1);f.Step();
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle,true);f.Step();
  f.Mouse(ImVec2(canvas.GetCenter().x+30*f.scale,canvas.GetCenter().y));f.Step();
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle,false);f.Step();
  Require(f.commands.empty(),"Map zoom/pan sent a project mutation");
  f.Click("Check 3D path",0,window);
  Require(f.commands.size()==1 && f.commands[0].method=="request_geometry_validation" && f.commands[0].arguments.empty(),"3D check must request diagnostics without moving a camera");
  f.commands.clear();
  f.state["geometry_validation"]={{"status","blocked"},{"passed",false},{"collision",{{"position",{245.6,-1732.5,68.3}}}}};
  f.state["geometry_validation_current"]=true;
  Require(f.DrawText().find("Blocked by indexed geometry")!=std::string::npos,"3D blockage is not visible");
  f.state["geometry_validation_current"]=false;
  Require(f.DrawText().find("Route not checked after latest edit")!=std::string::npos,"Stale check was presented as current");
  f.state["paused"]=false;f.Step();f.Click("Check 3D path",0,window);
  Require(f.commands.empty(),"3D check ran while demo was playing");
  f.state["map_preview"]=nullptr;f.Step(); // Missing optional preview never crashes the editor.
}

// Shape editing is exercised through semantic regions emitted by the real UI.
// These fixtures are local JSON only: no live game or spatial command is used.
void CurveFixture(Fixture& f) {
  auto& p=f.state["project"];p["interpolation"]="bezier";
  p["rotation_interpolation"]="slerp";p["fov_interpolation"]="linear";
  p["render_range"]={{"mode","region"},{"in",.25},{"out",7.25}};
  const double times[]={0,1.34375,4.25,7.5};
  for(int i=0;i<4;++i){auto& k=p["keys"][i];k["time"]=times[i];k["position"]={100.*i,50.*i,20.+10*i};k["angles"]={-10.+i*5.,20.+i*25.,0.};k["fov"]=70.+i*10.;
    k["rotation_easing"]={.25,.1,.75,.9};k["fov_easing"]={0,0,.58,1};}
  p["keys"][1]["position_in"]={-25,-10,-7};p["keys"][1]["position_out"]={35,14,9};
  p["keys"][1]["rotation_out"]={2,5,1};p["keys"][1]["fov_out"]=3;
  p["keys"][0]["position_easing"]={.188426,.769231,1,1};p["keys"][1]["position_easing"]={0,0,.58,1};
  f.state["camera"]["position"]={-50.,-70.,40.};f.state["project_document_id"]=71;f.Step();
}
void PointerDrag(Fixture& f,ImVec2 from,ImVec2 to,bool cancel=false,bool back=false){
  f.commands.clear();f.Mouse(from);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();
  f.Mouse(to);f.Step();Require(f.commands.empty(),"Curve/map draft sent history during drag");
  if(cancel)f.Key(ImGuiKey_Escape);
  if(back){f.Mouse(from);f.Step();}
  ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();f.Step();
}
void MapTangents(float height){
  Fixture f(height);CurveFixture(f);constexpr const char* window="Camera map / XY";
  f.Tool("Map");f.Step();f.Click("##map_handle_1",0,window);f.Step();
  f.Click("Free",0,window);f.Step();const auto before=f.state["project"];
  auto point=f.Find("##map_tangent_out_1",0,window).bounds.GetCenter();
  PointerDrag(f,point,ImVec2(point.x+24*f.scale,point.y-20*f.scale));
  const auto patch=f.One("set_project","keys");const auto& key=patch[1];
  Require(f.commands[0].arguments.size()==2&&f.commands[0].arguments["interpolation"]=="bezier","Map handle mode+keys were not atomic");
  Require(key["position_out"][0]>before["keys"][1]["position_out"][0]&&key["position_out"][1]>before["keys"][1]["position_out"][1],"Map tangent did not move in XY");
  Require(key["position_out"][2]==9&&key["position_in"]==before["keys"][1]["position_in"],"Free XY handle changed height or opposite handle");
  for(const char* field:{"position","time","angles","fov","easing","rotation_easing","fov_easing","rotation_out","fov_out"})Require(key[field]==before["keys"][1][field],std::string("Map tangent changed unrelated field ")+field);
  Require(patch[0]==before["keys"][0]&&patch[2]==before["keys"][2]&&f.state["project"]["render_range"]==before["render_range"],"Map tangent overwrote other keys/project fields");
  point=f.Find("##map_tangent_out_1",0,window).bounds.GetCenter();
  PointerDrag(f,point,ImVec2(point.x+35*f.scale,point.y),true);Require(f.commands.empty(),"Escape committed map handle");
  PointerDrag(f,point,ImVec2(point.x+35*f.scale,point.y),false,true);Require(f.commands.empty(),"Return to original map handle committed history");
  f.commands.clear();f.Click("Aligned",0,window);f.Step();const auto aligned_click=f.One("set_project","keys")[1];
  Near(aligned_click["position_in"][0],-aligned_click["position_out"][0].get<double>()*1.34375/(4.25-1.34375),"Aligned radio applies immediately");
  point=f.Find("##map_tangent_out_1",0,window).bounds.GetCenter();
  PointerDrag(f,point,ImVec2(point.x+14*f.scale,point.y-12*f.scale));
  auto aligned=f.One("set_project","keys")[1];const double ratio=1.34375/(4.25-1.34375);
  for(int axis=0;axis<2;++axis)Near(aligned["position_in"][axis],-aligned["position_out"][axis].get<double>()*ratio,"Time-weighted XY tangent");
  Require(aligned["position_in"][2]==-7&&aligned["position_out"][2]==9,"Aligned XY edit changed either Z handle");
  const auto before_free=f.state["project"]["keys"][1];
  point=f.Find("##map_tangent_out_1",0,window).bounds.GetCenter();ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);f.Step();
  PointerDrag(f,point,ImVec2(point.x+8*f.scale,point.y-12*f.scale));ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);f.Step();
  Require(f.One("set_project","keys")[1]["position_in"]==before_free["position_in"],"Ctrl handle did not break alignment");
  f.commands.clear();f.Click("Auto handles",0,window);f.Step();
  const auto automatic=f.One("set_project","keys")[1];Require(automatic["position_in"].is_null()&&automatic["position_out"].is_null(),"Auto did not clear to nullable handles");
  Require(automatic["rotation_out"]==before["keys"][1]["rotation_out"]&&automatic["position_easing"]==before["keys"][1]["position_easing"],"Auto silently changed other curves/timing");
  f.commands.clear();f.Click("Auto handles",0,window);f.Step();Require(f.commands.empty(),"Repeated Auto created empty history");
  const auto before_smooth=f.state["project"];f.Click("Smooth through key",0,window);f.Step();const auto smooth=f.One("set_project","keys");
  Require(f.commands[0].arguments.size()==4&&f.commands[0].arguments["rotation_interpolation"]=="bezier"&&f.commands[0].arguments["fov_interpolation"]=="bezier","Whole-pose smoothing was not one atomic mode+key change");
  for(const char* channel:{"position","rotation","fov"}){
    for(const char* suffix:{"_in","_out"})Require(smooth[1][std::string(channel)+suffix].is_null(),"Smooth did not reset an automatic handle");
    for(int i:{0,1})Require(smooth[i][std::string(channel)+"_easing"]==Json::array({0,0,1,1}),"Smooth did not linearize timing on both sides");
  }
  Require(smooth[2]==before_smooth["keys"][2]&&smooth[3]==before_smooth["keys"][3],"Smooth rewrote unrelated keys");
  const auto native=cs2_live::ReadProject(f.state["project"]);const double t=native.keys[1].time,h=.0001;
  const auto a=cs2_live::Sample(native,t-h),b=cs2_live::Sample(native,t),c=cs2_live::Sample(native,t+h);
  for(int axis=0;axis<3;++axis)Require(std::abs((b.position[axis]-a.position[axis])/h-(c.position[axis]-b.position[axis])/h)<.03,"Smooth retained a speed jump across unequal spans");
  Require(std::abs((b.fov-a.fov)/h-(c.fov-b.fov)/h)<.01,"Smooth retained FOV velocity jump");
  // Feed exactly the emitted atomic command through the native history engine.
  setenv("CS2_CAMERA_LIVE","1",1);cs2_live::Controller controller;float xyz[3]={0,0,0},angles[3]={0,0,0},lens=90;
  controller.View(xyz,angles,&lens,true);controller.Frame(true,640,true,64,true,"",[](const std::string&){});
  auto command=[&](const char* method,const Json& params){const auto request=Json{{"method",method},{"params",params}}.dump();controller.EnqueueText(request.c_str());controller.Frame(true,640,true,64,true,"",[](const std::string&){});return Json::parse(controller.StateJson())["project"];};
  const auto initial=command("set_project",before_smooth);const auto final=command("set_project",f.commands[0].arguments);
  Require(command("undo",Json::object())==initial,"One undo did not restore smoothing's full previous project");
  Require(command("redo",Json::object())==final,"Redo did not restore all smoothed channels");
  f.state["project"]["keys"][1]["position_out"]={9000,-15000,9};f.Step();f.commands.clear();f.Click("Fit path",0,window);f.Step();
  const auto canvas=f.Find("##camera_map_canvas",0,window).bounds;
  Require(canvas.Contains(f.Find("##map_tangent_out_1",0,window).bounds.GetCenter()),"Fit path left the selected shape handle outside the canvas");
  Require(f.commands.empty(),"Fit path changed project data");
}
void ValueCurves(float height){
  Fixture f(height);CurveFixture(f);constexpr const char* window="Camera curves / values";
  f.Tool("Curves");f.Step();f.Click("##curve_key_1",0,window);f.Step();f.Click("Free",0,window);f.Step();
  auto type=[&](const char* label,const char* value,bool enter=true){f.commands.clear();f.Click(label,0,window);ImGui::GetIO().AddInputCharactersUTF8(value);f.Step();Require(f.commands.empty(),"Curve typed value emitted before commit");if(enter)f.Key(ImGuiKey_Enter);else f.ClickAt(ImVec2(1200*f.scale,40*f.scale));f.Step();};
  const auto original=f.state["project"]["keys"][1];type("##curve_value","125.125");
  Near(f.One("update_key","position")[0],125.125,"Typed curve key X");Require(f.state["project"]["keys"][1]["position_out"]==original["position_out"],"Typed key value altered relative handles");
  type("##curve_time","1.75",false);Near(f.One("update_key","time"),1.75,"Typed curve time");
  type("##curve_out","180.5");Near(f.One("set_project","keys")[1]["position_out"][0],180.5-125.125,"Typed absolute outgoing control");
  Require(f.commands[0].arguments["interpolation"]=="bezier","Typed shape edit did not atomically enable Bezier");
  auto point=f.Find("##curve_handle_out_1",0,window).bounds.GetCenter();const auto before=f.state["project"];
  PointerDrag(f,point,ImVec2(point.x+25*f.scale,point.y-18*f.scale));const auto edited=f.One("set_project","keys")[1];
  Require(edited["position_out"][0]>before["keys"][1]["position_out"][0],"Vertical value handle did not increase value");
  Require(edited["position"]==before["keys"][1]["position"]&&edited["time"]==before["keys"][1]["time"]&&edited["position_out"][1]==before["keys"][1]["position_out"][1]&&edited["position_out"][2]==before["keys"][1]["position_out"][2],"Value handle also moved key or other components");
  Require(edited["position_easing"]==before["keys"][1]["position_easing"],"Value handle silently rewrote timing");
  point=f.Find("##curve_handle_out_1",0,window).bounds.GetCenter();
  PointerDrag(f,point,ImVec2(point.x+25*f.scale,point.y));Require(f.commands.empty(),"Horizontal-only shape handle created history");
  PointerDrag(f,point,ImVec2(point.x,point.y-20*f.scale),true);Require(f.commands.empty(),"Escape committed value handle");
  PointerDrag(f,point,ImVec2(point.x,point.y-20*f.scale),false,true);Require(f.commands.empty(),"Return to original value handle created history");
  point=f.Find("##curve_key_1",0,window).bounds.GetCenter();const auto before_key=f.state["project"]["keys"][1];
  PointerDrag(f,point,ImVec2(point.x+15*f.scale,point.y-15*f.scale));const auto moved_key=f.One("set_project","keys")[1];
  Require(moved_key["time"]>before_key["time"]&&moved_key["position"][0]>before_key["position"][0],"Graph key did not edit time + value together");
  Require(f.commands[0].arguments.size()==1&&moved_key["position_out"]==before_key["position_out"]&&moved_key["rotation_out"]==before_key["rotation_out"],"Graph key move changed interpolation or relative handles");
  f.Click("##value_channel",0,window);f.Step();f.Choose("Rotation yaw");f.Step();
  type("##curve_out","90");const auto rotation=f.One("set_project","keys")[1];
  Require(f.commands[0].arguments["rotation_interpolation"]=="bezier"&&f.commands[0].arguments.size()==2,"Rotation handle did not promote only its own mode atomically");
  Near(rotation["rotation_out"][1],45.,"Typed yaw outgoing control");Require(rotation["rotation_easing"]==original["rotation_easing"],"Rotation shape edit erased timing");
  Require(rotation["rotation_out"][0]==original["rotation_out"][0]&&rotation["rotation_out"][2]==original["rotation_out"][2],"Editing yaw overwrote dormant pitch/roll handles from SLERP mode");
  f.Click("##value_channel",0,window);f.Step();f.Choose("Field of view");f.Step();
  type("##curve_out","999");Near(f.One("set_project","keys")[1]["fov_out"],99.,"FOV handle physical bound");
  Require(f.commands[0].arguments["fov_interpolation"]=="bezier"&&f.commands[0].arguments.size()==2,"FOV handle mode promotion was not atomic");
  type("##curve_value","179");Near(f.One("update_key","fov"),179.,"Typed FOV value");
  f.commands.clear();f.Click("##curve_value",0,window);ImGui::GetIO().AddInputCharactersUTF8("50");f.Step();f.Key(ImGuiKey_Escape);f.Step();Require(f.commands.empty(),"Escape committed typed graph value");
  // The shared native numeric widget retains its middle-scrub gesture in the graph.
  f.commands.clear();const auto field=f.Find("##curve_value",0,window).bounds.GetCenter();f.Mouse(field);ImGui::GetIO().AddMouseButtonEvent(2,true);f.Step();
  f.Mouse(ImVec2(field.x-std::ceil(4*f.scale),field.y));f.Step();f.Key(ImGuiKey_K);f.Key(ImGuiKey_Space);Require(f.commands.empty(),"Graph numeric scrub leaked history/shortcuts");
  ImGui::GetIO().AddMouseButtonEvent(2,false);f.Step();Near(f.One("update_key","fov"),178.,"Graph numeric middle step");
  // Hiding the editor mid-drag cancels the local draft across the frame gap.
  point=f.Find("##curve_handle_in_1",0,window).bounds.GetCenter();f.commands.clear();f.Mouse(point);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(point.x,point.y+20*f.scale));f.Step();
  f.editor.visible=false;f.Step();ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();f.editor.visible=true;f.Step();Require(f.commands.empty(),"Hidden graph committed an abandoned drag");
}
void IndependentTiming(){
  Fixture f;f.SelectKey();auto& key=f.state["project"]["keys"][0];key["easing"]={.42,0,.58,1};f.Step();f.Step();
  constexpr const char* window="Camera curves / values";f.Tool("Curves");f.Click("Timing",0,window);f.Step();
  f.commands.clear();f.Click("Linear",0,window);f.Step();Require(f.One("update_key","position_easing")==Json::array({0,0,1,1}),"Position timing did not use its independent field");Require(key["easing"]==Json::array({.42,0,.58,1})&&!key.contains("rotation_easing"),"Position timing overwrote legacy/shared fallback");
  f.Click("##timing_channel",0,window);f.Step();f.Choose("Rotation");f.Step();f.commands.clear();f.Click("Ease out",0,window);f.Step();Require(f.One("update_key","rotation_easing")[2].get<double>()>.57,"Rotation timing field missing");
  f.Click("##timing_channel",0,window);f.Step();f.Choose("FOV");f.Step();f.commands.clear();f.Click("Linear",0,window);f.Step();Require(f.One("update_key","fov_easing")==Json::array({0,0,1,1}),"FOV timing field missing");
  auto point=f.Find("##timing_handle_in",0,window).bounds.GetCenter();PointerDrag(f,point,ImVec2(point.x+20,point.y-15),true);Require(f.commands.empty(),"Escape committed timing draft");
  point=f.Find("##timing_handle_in",0,window).bounds.GetCenter();PointerDrag(f,point,point);Require(f.commands.empty(),"Timing handle click created no-op history");
  PointerDrag(f,point,ImVec2(point.x+20,point.y-15),false,true);Require(f.commands.empty(),"Timing handle returned to origin but committed history");
  point=f.Find("##timing_handle_in",0,window).bounds.GetCenter();PointerDrag(f,point,ImVec2(point.x+20,point.y-15));Require(f.One("update_key","fov_easing")[0].get<double>()>0,"Timing handle did not save selected channel");
  f.commands.clear();f.Click("Use shared",0,window);f.Step();Require(f.One("update_key","fov_easing").is_null(),"Use shared did not restore nullable channel fallback");
  Require(key["easing"]==Json::array({.42,0,.58,1})&&key["position_easing"]==Json::array({0,0,1,1}),"Use shared changed another timing curve");
}
void CurveDiagnosticLayout(){
  Fixture f;f.commands.clear();f.editor.OpenCameraCurvesForDiagnostics();f.Step();f.Step();
  f.Find("##curve_handle_in_1",0,"Camera curves / values");f.Find("##curve_handle_out_1",0,"Camera curves / values");
  Require(f.commands.empty(),"Diagnostic curves layout mutated camera data");
  auto raw=f.state["project"];const double yaw[]={170,-170,-150,-130};for(int i=0;i<4;++i)raw["keys"][i]["angles"]={0,yaw[i],0};
  const auto native=cs2_live::ReadProject(raw);const double t=native.keys[1].time;
  for(double time:{t-.0001,t,t+.0001}){
    const double displayed=cs2_live_ui::camera_curve::SampleValue(native,time,1,1),sample=cs2_live::Sample(native,time).angles[1];
    Require(std::abs(displayed-190)<.01,"SLERP yaw graph jumped at an authored wrap boundary");
    Near(std::remainder(displayed-sample,360.),0,"Unwrapped display changed native rotation");
  }
  Require(raw["keys"][1]["angles"][1]==-170,"Graph display rewrote the authored yaw");
}

void TightCurveTimes(float height){
  Fixture f(height);constexpr const char* window="Camera curves / values";
  auto& project=f.state["project"];project["keys"].erase(3);project["duration"]=.01;
  for(int i=0;i<3;++i)project["keys"][i]["time"]=i*.000001;
  auto native=cs2_live::ReadProject(project);const auto locked=cs2_live_ui::camera_curve::TimeBounds(native,1);
  Require(locked.first==native.keys[1].time&&locked.second==native.keys[1].time,"Tight key time interval must include and lock its current time");
  // Bounds at larger absolute times must survive floating-point cancellation
  // when sent back through native Normalize's exact >= 1us condition.
  auto rounded=native;rounded.duration=4;rounded.keys[0].time=1.34375;
  for(int i=1;i<3;++i)rounded.keys[i].time=std::nextafter(rounded.keys[i-1].time+.000001,std::numeric_limits<double>::infinity());
  cs2_live::Normalize(rounded);
  for(std::size_t i=0;i<rounded.keys.size();++i){const auto bounds=cs2_live_ui::camera_curve::TimeBounds(rounded,i);Require(bounds.first<=rounded.keys[i].time&&bounds.second>=rounded.keys[i].time,"Time bounds excluded a valid authored time");
    for(double time:{bounds.first,bounds.second}){auto candidate=rounded;candidate.keys[i].time=time;cs2_live::Normalize(candidate);}}
  f.Step();f.editor.OpenCameraCurvesForDiagnostics();f.Step();f.Step();
  auto point=f.Find("##curve_key_1",0,window).bounds.GetCenter();
  PointerDrag(f,point,ImVec2(point.x+20*f.scale,point.y));Require(f.commands.empty(),"Locked horizontal key movement created history");
  const auto before=project["keys"][1];point=f.Find("##curve_key_1",0,window).bounds.GetCenter();
  PointerDrag(f,point,ImVec2(point.x+20*f.scale,point.y-20*f.scale));const auto moved=f.One("set_project","keys")[1];
  Require(moved["time"]==before["time"]&&moved["position"][0]>before["position"][0],"Tight key must keep time fixed while vertical value drag works");
  cs2_live::ReadProject(project);
  f.commands.clear();f.Click("##curve_time",0,window);ImGui::GetIO().AddInputCharactersUTF8("0.009");f.Step();f.Key(ImGuiKey_Enter);f.Step();
  Require(f.commands.empty()&&project["keys"][1]["time"]==.000001,"Tight numeric time emitted an invalid or unchanged mutation");
  f.commands.clear();f.Click("##curve_value",0,window);ImGui::GetIO().AddInputCharactersUTF8("350");f.Step();f.Key(ImGuiKey_Enter);f.Step();
  Near(f.One("update_key","position")[0],350,"Tight key's numeric value remains editable");cs2_live::ReadProject(project);
}
void GuardedCurveProjectWrites(){
  setenv("CS2_CAMERA_LIVE","1",1);Fixture f;CurveFixture(f);cs2_live::Controller controller;
  float xyz[3]={0,0,0},angles[3]={0,0,0},lens=90;controller.View(xyz,angles,&lens,true);
  auto frame=[&](){controller.Frame(true,640,true,64,true,"",[](const std::string&){});};frame();
  auto enqueue=[&](const std::string& method,const Json& args){const auto text=Json{{"method",method},{"params",args}}.dump();controller.EnqueueText(text.c_str());};
  enqueue("set_project",f.state["project"]);frame();
  for(int i=0;i<6;++i){enqueue("set_project",{{"name","Revision "+std::to_string(i)}});frame();}
  f.state=Json::parse(controller.StateJson());Require(f.state["revision"]==7,"CAS fixture did not reach R7");const auto original=f.state["project"];
  f.command_sink=[](const std::string&,const Json&){};f.Step();f.editor.OpenCameraCurvesForDiagnostics();f.Step();f.Step();
  f.wire_commands.clear();f.commands.clear();f.Click("Auto",0,"Camera curves / values");f.Step();
  Require(f.wire_commands.size()==1&&f.wire_commands[0].method=="guarded_mutation","Full curve key-list write bypassed native CAS");
  const auto stale=f.wire_commands[0];Require(stale.arguments.size()==3&&stale.arguments["expected_revision"]==7&&stale.arguments["method"]=="set_project"&&stale.arguments["params"].contains("keys"),"CAS wrapper lost the observed UI revision or added tick constraints");
  // A separate writer reaches R8 before the queued R7 UI edit is dispatched.
  enqueue("update_key",{{"id","2"},{"fov",112}});enqueue(stale.method,stale.arguments);frame();
  const auto changed=Json::parse(controller.StateJson());Require(changed["revision"]==8,"Stale UI mutation changed native revision/history");
  Require(changed["project"]["keys"][2]["fov"]==112,"Stale full key list overwrote the concurrent writer");
  Require(changed["project"]["keys"][1]==original["keys"][1],"Rejected UI edit partially changed selected handles");
  Require(changed.value("error",std::string()).find("changed")!=std::string::npos,"Stale UI rejection was not surfaced");
  enqueue("undo",Json::object());frame();f.state=Json::parse(controller.StateJson());Require(f.state["project"]==original,"Rejected UI edit inserted an undo record");
  f.Step();f.Step();f.wire_commands.clear();f.commands.clear();f.Click("Auto",0,"Camera curves / values");f.Step();
  Require(f.wire_commands.size()==1&&f.wire_commands[0].arguments["expected_revision"]==f.state["revision"],"Fresh retry did not use current UI revision");
  enqueue(f.wire_commands[0].method,f.wire_commands[0].arguments);frame();const auto accepted=Json::parse(controller.StateJson());
  const auto accepted_project=cs2_live::ReadProject(accepted["project"]);
  Require(!accepted_project.keys[1].positionIn&&!accepted_project.keys[1].positionOut,"Fresh guarded curve mutation failed");
  enqueue("undo",Json::object());frame();Require(Json::parse(controller.StateJson())["project"]==original,"Guarded edit was not exactly one undo step");
}

void RequireWindowBackdrop(ImGuiWindow* window,bool blurred,const char* surface) {
  Require(window && window->Active && !window->Hidden,std::string(surface)+" did not open a visible window");
  const auto* draw=window->DrawList;const ImDrawCmd* background=nullptr;
  for(const auto& command:draw->CmdBuffer)if(command.ElemCount){background=&command;break;}
  Require(background && background->ElemCount>=6,std::string(surface)+" has no background geometry");
  Require((background->GetTexID()==ImTextureID(1234))==blurred,std::string(surface)+" did not put its expected background before labels");
  const ImVec2 end(window->Pos.x+window->Size.x,window->Pos.y+window->Size.y);
  Near(background->ClipRect.x,window->Pos.x,"Popup background clip left");
  Near(background->ClipRect.y,window->Pos.y,"Popup background clip top");
  Near(background->ClipRect.z,end.x,"Popup background clip right");
  Near(background->ClipRect.w,end.y,"Popup background clip bottom");
  ImVec2 low(FLT_MAX,FLT_MAX),high(-FLT_MAX,-FLT_MAX);
  for(unsigned n=0;n<6;++n){
    const auto& v=draw->VtxBuffer[int(background->VtxOffset+draw->IdxBuffer[int(background->IdxOffset+n)])];
    low.x=std::min(low.x,v.pos.x);low.y=std::min(low.y,v.pos.y);high.x=std::max(high.x,v.pos.x);high.y=std::max(high.y,v.pos.y);
    Require((v.col>>IM_COL32_A_SHIFT)==255,std::string(surface)+" background leaks underlying editor labels");
    if(blurred){Near(v.uv.x,v.pos.x/ImGui::GetIO().DisplaySize.x,"Popup blur UV x");Near(v.uv.y,v.pos.y/ImGui::GetIO().DisplaySize.y,"Popup blur UV y");}
    else Require(v.col==IM_COL32(48,48,48,255),std::string(surface)+" fallback geometry is not opaque native gray");
  }
  Near(low.x,window->Pos.x,"Popup background left");Near(low.y,window->Pos.y,"Popup background top");
  Near(high.x,end.x,"Popup background right");Near(high.y,end.y,"Popup background bottom");
}

void FloatingMenuBackdrops(float height){
  struct Menu{const char* window;const char* id;int kind;};
  const Menu menus[]={
    {"Camera curves / values","##value_channel",0},{"Camera map / XY","##map_layer",1},
    {"Camera timeline / composition","##composition_snap",2},
    {"Camera timeline / composition","##composition_resize",2},{"Camera timeline / composition","##composition_range",2},
    {"Editor settings","##pref_time_unit",3},{"Editor settings","##pref_snap_mode",3},{"Editor settings","##pref_resize_mode",3},
    {"##CS2CinematicTransport","##transport_resize",4}};
  auto open=[](Fixture& f,int kind){
    if(kind==0)f.editor.OpenCameraCurvesForDiagnostics();
    else if(kind==1){f.SelectKey();f.state["map_preview"]={{"layers",Json::array({{{"z",0},{"lines",Json::array()}},{{"z",100},{"lines",Json::array()}}})}};f.editor.OpenCameraMapForDiagnostics();}
    else if(kind==2)f.editor.OpenTimelineForDiagnostics();
    else if(kind==3)f.editor.OpenSettingsForDiagnostics();
    f.Step();f.Step();
  };
  for(bool blurred:{true,false})for(const auto& menu:menus){
    Fixture f(height);f.editor.backdrop_texture=ImTextureID(1234);f.state["editor_preferences"]["blur_enabled"]=blurred;
    open(f,menu.kind);f.commands.clear();f.Click(menu.id,0,menu.window);f.Step();f.Step();
    const auto& popups=ImGui::GetCurrentContext()->OpenPopupStack;
    Require(!popups.empty(),std::string(menu.id)+" did not open its real ImGui popup");
    RequireWindowBackdrop(popups.back().Window,blurred,menu.id);
    Require(f.commands.empty(),"Opening a floating menu changed project/preferences");
  }
  // The private host diagnostic opens precisely the same menu without input.
  {Fixture f(height);f.editor.backdrop_texture=ImTextureID(1234);f.editor.OpenCurveChannelMenuForDiagnostics();f.Step();f.Step();f.Step();
    const auto& popups=ImGui::GetCurrentContext()->OpenPopupStack;Require(!popups.empty(),"Curve menu diagnostic did not open the real menu");
    RequireWindowBackdrop(popups.back().Window,true,"Diagnostic curve channel menu");Require(f.commands.empty(),"Menu diagnostic emitted a bridge command");}
  for(int kind=0;kind<4;++kind){
    Fixture f(height);f.editor.backdrop_texture=ImTextureID(1234);open(f,kind);f.commands.clear();
    const char* window=kind==0?"Camera curves / values":kind==1?"Camera map / XY":kind==2?"Camera timeline / composition":"Editor settings";
    const char* label=kind<2?"Smooth through key":kind==2?"Fit composition":"##pref_frame_rate";
    f.Mouse(f.Find(label,0,window).bounds.GetCenter());for(int frame=0;frame<40;++frame)f.Step();
    ImGuiWindow* tooltip=nullptr;for(auto* candidate:ImGui::GetCurrentContext()->Windows)
      if((candidate->Flags&ImGuiWindowFlags_Tooltip)&&candidate->Active&&!candidate->Hidden)tooltip=candidate;
    RequireWindowBackdrop(tooltip,true,window);Require(f.commands.empty(),"Hover tooltip emitted a bridge command");
  }
}

void SafeExitUi(float height,double ui_scale){
  Fixture f(height);f.state["editor_preferences"]["ui_scale"]=ui_scale;f.Step();f.Step();
  constexpr const char* bar="##CS2CinematicTransport";
  const auto project=f.state["project"],revision=f.state["revision"];
  const auto bounds=f.Find("Safe exit",0,bar).bounds;
  Require(bounds.Min.x>=0&&bounds.Max.x<=ImGui::GetIO().DisplaySize.x&&bounds.Max.y<=ImGui::GetIO().DisplaySize.y,"Safe exit button is clipped");
  f.Click("Safe exit",0,bar);f.Step();
  Require(f.commands.size()==1&&f.commands[0].method=="request_safe_exit"&&f.commands[0].arguments.empty(),"Safe exit bypassed launcher lifecycle or sent duplicate request");
  Require(f.state["project"]==project&&f.state["revision"]==revision,"Safe exit edited the project/history");
  f.commands.clear();f.Click("Safe exit",0,bar);f.Key(ImGuiKey_K);f.Key(ImGuiKey_Space);
  Require(f.commands.empty(),"Pending safe exit accepted another request or authoring shortcut");
  f.state["safe_exit"]={{"phase","error"},{"error","Fixture disk is full"}};f.Step();
  Require(f.DrawText().find("Safe exit failed; the session is still open.")!=std::string::npos,"Save failure was hidden");
  f.Click("Keep editing",0,bar);f.Step();Require(f.commands.size()==1&&f.commands[0].method=="set_safe_exit_status"&&f.commands[0].arguments["phase"]=="idle","Save failure cannot return to editing");
  f.commands.clear();f.state["demo"]=false;f.state["project"]["keys"]=Json::array();f.Tool("Demo");f.commands.clear();f.Step();
  f.Click("Safe exit",0,bar);f.Step();Require(f.commands.size()==1&&f.commands[0].method=="request_safe_exit","Safe exit is unavailable in menu/no-demo state");
}

void SafeExitCommitsActiveInput(){
  Fixture f;f.SelectKey();f.Click("##fov");
  ImGui::GetIO().AddInputCharactersUTF8("106");f.Step();
  Require(f.commands.empty(),"Lens draft was saved before it was committed");
  f.Click("Safe exit",0,"##CS2CinematicTransport");f.Step();
  Require(f.commands.size()==2&&f.commands[0].method=="update_key"&&f.commands[0].arguments["fov"]==106&&f.commands[1].method=="request_safe_exit",
          "Safe exit discarded the active lens input or requested exit before committing it");
}

void WorkspaceNavigation(float height){
  Fixture f(height);constexpr const char* bar="##CS2CinematicTransport";
  const auto original=f.state["project"];const auto cursor=f.state["cursor"],revision=f.state["revision"];
  for(double scale:{1.,1.5}){
    f.state["editor_preferences"]["ui_scale"]=scale;f.Step();f.Step();
    for(const char* tool:{"Timeline","Map","Curves"}){
      f.commands.clear();f.Click(tool,0,bar);f.Step();
      for(const auto& command:f.commands)Require(command.method=="set_editor_preferences","Tool switch moved camera/project");
      auto* work=TestWindow("##CS2CameraWorkspace");auto* inspector=TestWindow(kInspector);auto* footer=TestWindow(bar);
      Require(work&&work->Active&&work->Pos.y>=0&&work->Pos.x+work->Size.x<=inspector->Pos.x&&work->Pos.y+work->Size.y<=footer->Pos.y,"Workspace overlaps inspector/footer or leaves display");
      Require(inspector->Pos.y+inspector->Size.y<=footer->Pos.y,"Inspector overlaps global toolbar");
      int active=0;for(const char* name:{"Camera timeline / composition","Camera map / XY","Camera curves / values"})if(auto* window=TestWindow(name);window&&window->Active){++active;Require(window->Flags&ImGuiWindowFlags_ChildWindow,"Tool reopened an independent window");}
      Require(active==1,"More than one tool surface is active");
    }
  }
  Require(f.state["project"]==original&&f.state["cursor"]==cursor&&f.state["revision"]==revision,"Navigation changed authored state or playhead");
  f.state["editor_preferences"]["ui_scale"]=1.;f.Step();f.SelectKey();
  f.commands.clear();f.Click("##fov");ImGui::GetIO().AddInputCharactersUTF8("106");f.Step();f.Click("Live camera");f.Step();
  Require(f.commands.empty()&&f.state["project"]==original,"Live target switch committed abandoned key input");
  f.Key(ImGuiKey_Delete);Require(f.commands.empty(),"Delete removed a remembered key while Live camera was active");f.SelectKey();
  f.commands.clear();f.Click("##fov");ImGui::GetIO().AddInputCharactersUTF8("105");f.Step();
  f.Click("Shot");f.Step();for(const auto& command:f.commands)Require(command.method=="set_editor_preferences","Section switch committed abandoned numeric input");
  Require(f.state["project"]==original,"Section switch wrote typed lens draft");f.Find("##shot_duration");
  f.Tool("Curves");f.Click("##curve_key_1",0,"Camera curves / values");f.Step();f.Find("##shot_duration");
  f.Click("Inspector",0,bar);f.Step();f.Click("##curve_key_2",0,"Camera curves / values");f.Step();Require(!TestWindow(kInspector)->Active,"Selecting a key reopened a closed inspector");
  f.Click("Inspector",0,bar);f.Step();f.Find("##shot_duration");
  f.commands.clear();f.Click("Demo",0,bar);f.Step();Require(f.state["project"]==original&&f.state["cursor"]==cursor,"Source switch retimed or sought");
  for(const auto& command:f.commands)Require(command.method=="set_editor_preferences","Source switch emitted camera command");
  f.commands.clear();f.Key(ImGuiKey_I);f.Key(ImGuiKey_O);Require(f.commands.empty(),"Demo context edited camera In/Out");
  f.Click("Camera",0,bar);f.Step();f.state["cursor"]=100.;f.Step();Require(f.DrawText().find("Outside shot +92.00 s")!=std::string::npos,"Outside-shot playhead was shown as in-range");
  Require(f.state["cursor"]==100.,"Displaying outside-shot time clamped native cursor");
}

void ContextualCapture(float height) {
  Fixture f(height);constexpr const char* bar="##CS2CinematicTransport";
  const int start=f.state["project"]["start_tick"];
  f.state["sync_demo"]=false;f.state["tick"]=start+640;f.state["cursor"]=3.125;f.Step();
  f.commands.clear();f.Click("##capture",0,bar);Near(f.One("add_key","time"),3.125,"Camera capture ignores divergent demo tick when Sync is off");
  f.Tool("Demo");f.commands.clear();f.Click("##demo_capture",0,"Demo navigation");Near(f.One("add_key","time"),10.,"Demo capture uses actual tick and extends past duration");
  f.state["tick"]=start-1;f.Step();f.commands.clear();f.Key(ImGuiKey_K);
  Require(f.commands.empty(),"Negative demo-relative capture silently added an edge key");
  Require(f.DrawText().find("Capture rejected: demo tick is before the shot start.")!=std::string::npos,"Rejected capture has no visible reason");
  f.state["tick"]=start+160;f.Step();f.commands.clear();f.Key(ImGuiKey_K);
  Require(f.One("update_key","id")=="1","Occupied demo-relative tick did not update its existing key");
  f.Tool("Camera");f.state["cursor"]=9.123456789;f.Step();f.commands.clear();f.Key(ImGuiKey_K);
  Require(f.One("add_key","time").get<double>()==9.123456789,"Camera capture clamped/rounded the outside-shot cursor");
  f.commands.clear();f.Key(ImGuiKey_I);f.Key(ImGuiKey_O);Require(f.commands.empty(),"Outside-shot I/O silently changed render boundary");
  Require(f.DrawText().find("Set Out rejected: camera playhead is outside the valid shot range.")!=std::string::npos,"Outside-shot I/O rejection is not visible");
  f.commands.clear();f.Key(ImGuiKey_Space);Require(f.One("preview","sync_demo")==false,"Camera Space changed independent Sync policy");
  f.Tool("Demo");f.commands.clear();f.Key(ImGuiKey_Space);Require(f.One("demo_control","action")=="resume","Demo Space controlled camera preview");
  f.state["project"]["keys"]=Json::array();f.state["tick"]=start-100;f.Step();f.commands.clear();f.Key(ImGuiKey_K);
  Near(f.One("add_key","time"),0.,"First demo capture must anchor a new shot at current tick");
}

void PlaybackTimeControls(float height,double ui_scale) {
  Fixture f(height);constexpr const char* bar="##CS2CinematicTransport";constexpr const char* demo="Demo navigation";
  f.state["editor_preferences"]["ui_scale"]=ui_scale;f.state["editor_preferences"]["time_unit"]="ticks";f.Step();f.Step();
  auto type=[&](const char* field,const char* value,const char* window){f.commands.clear();f.Click(field,0,window);ImGui::GetIO().AddInputCharactersUTF8(value);f.Step();Require(f.commands.empty(),"Playback field sent an unfinished edit");f.Key(ImGuiKey_Enter);f.Step();};
  float last=-1;
  for(const char* label:{"##preview","##prev","##next","##transport_time","##transport_duration","##transport_resize","##capture","##render_scope","Sync demo"}){
    const auto item=f.Find(label,0,bar);Require(item.bounds.Min.x>=last&&item.bounds.Max.x<=ImGui::GetIO().DisplaySize.x,"Playback fields overlap or leave the screen");last=item.bounds.Max.x;
  }
  type("##transport_time","128",bar);Near(f.One("scrub","time"),2.,"Camera inline ticks are relative seconds");Require(f.commands[0].arguments["seek_demo"]==false,"Inline position ignored independent Sync policy");
  f.state["cursor"]=2.;const auto keys=f.state["project"]["keys"];
  type("##transport_duration","640",bar);Near(f.One("set_project","duration"),10.,"Inline shot duration uses tick units");Require(f.state["project"]["keys"]==keys&&f.wire_commands.back().method=="guarded_mutation","Keep duration moved keys or bypassed atomic revision guard");
  f.Click("##transport_resize",0,bar);f.Step();f.Choose("Scale keys and region");f.Step();
  type("##transport_duration","1280",bar);Near(f.One("set_project","duration"),20.,"Inline Scale duration");
  for(std::size_t i=0;i<keys.size();++i)Near(f.state["project"]["keys"][i]["time"],2*keys[i]["time"].get<double>(),"Inline Scale retimes keys together");
  const auto scaled=f.state["project"]["keys"];
  f.state["editor_preferences"]["resize_mode"]="keep";f.Step();f.commands.clear();f.Click("##transport_duration",0,bar);ImGui::GetIO().AddInputCharactersUTF8("1920");f.Step();
  f.state["editor_preferences"]["time_unit"]="frames";f.state["editor_preferences"]["frame_rate"]=30;f.state["editor_preferences"]["resize_mode"]="scale";f.Step();f.Key(ImGuiKey_Enter);f.Step();
  Near(f.One("set_project","duration"),30.,"Inline duration keeps units during a remote preference change");Require(f.state["project"]["keys"]==scaled,"Remote resize mode changed an active Keep gesture into Scale");
  f.state["demo_timeline"]={{"available",true},{"metadata_revision",1},{"demo",{{"playback_ticks",8000},{"tick_rate",64},{"duration_seconds",125}}},{"rounds",Json::array()},{"selected_player",nullptr}};
  f.state["demo_playback_range"]={{"enabled",false},{"in_tick",1000},{"out_tick",3000}};f.state["tick"]=2000;f.Tool("Demo");
  const auto project=f.state["project"];const auto region=project.value("render_range",Json::object());
  for(const char* label:{"Play demo","##demo_current_tick","Limit playback","##demo_range_in","##demo_range_out"}){
    const auto item=f.Find(label,0,demo);const auto* window=TestWindow(demo);Require(item.bounds.Min.x>=window->InnerRect.Min.x&&item.bounds.Max.x<=window->InnerRect.Max.x&&item.bounds.Max.y<=window->InnerRect.Max.y,"Demo time control is clipped in the playback area");
  }
  type("##demo_current_tick","2500",demo);Require(f.One("demo_control","tick")==2500,"Current demo tick did not seek absolute recording time");
  type("##demo_range_in","1200",demo);Require(f.commands.size()==1&&f.commands[0].method=="set_demo_playback_range"&&f.commands[0].arguments==Json{{"in_tick",1200}},"Demo In changed another state or enabled playback limits implicitly");
  Require(!f.state["demo_playback_range"]["enabled"].get<bool>(),"Editing disabled demo bounds enabled them");
  type("##demo_range_out","999999",demo);Require(f.One("set_demo_playback_range","out_tick")==8000,"Demo Out exceeded authoritative recording length");
  type("##demo_range_in","999999",demo);Require(f.One("set_demo_playback_range","in_tick")==7999,"Demo In accepted an empty or reversed range");
  type("##demo_range_out","1",demo);Require(f.commands.empty(),"Clamping unchanged demo Out created duplicate history");
  f.commands.clear();f.Click("Limit playback",0,demo);Require(f.One("set_demo_playback_range","enabled")==true,"Playback limit checkbox did not enable the native range");
  type("##demo_range_in","500",demo);type("##demo_range_out","600",demo);
  f.commands.clear();const auto point=f.Find("##demo_range_out",0,demo).bounds.GetCenter();f.Mouse(point);ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle,true);f.Step();
  f.Mouse(ImVec2(point.x+std::ceil(4*f.scale*ui_scale),point.y));f.Step();f.Key(ImGuiKey_K);f.Key(ImGuiKey_Space);Require(f.commands.empty(),"Demo limit middle drag leaked edits or shortcuts");
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle,false);f.Step();Require(f.One("set_demo_playback_range","out_tick")==601,"Demo limit middle drag did not commit one absolute tick");
  Require(f.state["project"]==project&&f.state["project"].value("render_range",Json::object())==region,"Demo playback controls changed the camera project or render region");
  f.commands.clear();f.Click("##demo_range_in",0,demo);ImGui::GetIO().AddInputCharactersUTF8("513");f.Step();f.Click("Camera",0,bar);f.Step();for(const auto& command:f.commands)Require(command.method=="set_editor_preferences","Source switch committed an unfinished demo limit");
  f.Tool("Demo");f.state["demo_timeline"]["available"]=false;f.Step();f.commands.clear();f.Click("Limit playback",0,demo);f.Click("##demo_range_in",0,demo);ImGui::GetIO().AddInputCharactersUTF8("100");f.Step();f.Key(ImGuiKey_Enter);
  Require(f.commands.empty()&&f.DrawText().find("Playback limits unavailable:")!=std::string::npos,"Unavailable demo length allowed a limit edit or lacked a visible reason");
}

void EditorPreferencesUi(float height){
  Fixture f(height);constexpr const char* transport="##CS2CinematicTransport";constexpr const char* settings="Editor settings";
  f.state["editor_preferences"]["compact_transport"]=true;f.Step();f.Step();
  const Json original_project=f.state["project"];const int revision=f.state["revision"];
  auto* strip=TestWindow(transport);Require(std::abs(strip->Size.y-88*f.scale)<1.01,"Unified toolbar height");
  Require(std::abs(TestWindow(transport)->Size.y-88*f.scale)<1.01,"Unified toolbar height");
  for(const char* label:{"Demo","Camera","Timeline","Map","Curves","Inspector","Project","Settings","Export","Hide","##preview","##prev","##next","##capture","##render_scope","Sync demo"}){
    const auto item=f.Find(label,0,transport);Require(item.bounds.Min.x>=0&&item.bounds.Max.x<=ImGui::GetIO().DisplaySize.x,"Unified toolbar control outside viewport");
    if(std::string(label)!="Sync demo")Near(item.bounds.GetHeight(),36*f.scale,"Native toolbar control height");
  }
  f.commands.clear();f.Click("Settings",0,transport);f.Step();
  auto* panel=TestWindow(settings);Require(panel&&!(panel->Flags&ImGuiWindowFlags_NoMove)&&!(panel->Flags&ImGuiWindowFlags_Modal),"Settings is not a separate movable native panel");
  Require(f.commands.empty(),"Opening settings changed project state");
  auto choose=[&](const char* field,const char* value){f.commands.clear();f.Click(field,0,settings);f.Step();f.Choose(value);f.Step();};
  auto type=[&](const char* field,const char* value){f.commands.clear();f.Click(field,0,settings);ImGui::GetIO().AddInputCharactersUTF8(value);f.Step();Require(f.commands.empty(),"Preference numeric emitted before commit");f.Key(ImGuiKey_Enter);f.Step();};
  choose("##pref_time_unit","Demo ticks");Require(f.One("set_editor_preferences","time_unit")=="ticks","Time preference did not use its independent command");
  Require(f.state["project"]==original_project&&f.state["revision"]==revision,"Changing display units wrote project history");
  f.SelectKey();f.Type("##key_time","64");Near(f.One("update_key","time"),1.,"Tick key time converts to native seconds");
  f.Click("Shot");f.Step();const auto keep_keys=f.state["project"]["keys"];
  f.Type("##shot_duration","1024");Near(f.One("set_project","duration"),16.,"Tick duration converts to native seconds");Require(f.state["project"]["keys"]==keep_keys,"Keep duration moved keys");
  choose("##pref_time_unit","Frames");type("##pref_frame_rate","30");Require(f.One("set_editor_preferences","frame_rate")==30,"Frame grid preference was not typed");
  choose("##pref_resize_mode","Scale keys and render region");
  f.Type("##shot_duration","240");Near(f.One("set_project","duration"),8.,"Frame duration converts to native seconds");
  for(std::size_t i=0;i<keep_keys.size();++i)Near(f.state["project"]["keys"][i]["time"],keep_keys[i]["time"].get<double>()*.5,"Scale duration retimes all keys");
  Require(f.state["tick_rate"]==64&&!f.state["project"].contains("frame_rate"),"Editor frame rate changed demo or export data");
  f.editor.settings_visible=false;f.Step();f.editor.OpenCameraCurvesForDiagnostics();f.Step();f.Step();
  f.Click("##curve_key_2",0,"Camera curves / values");f.Step();f.commands.clear();f.Click("##curve_time",0,"Camera curves / values");ImGui::GetIO().AddInputCharactersUTF8("90");f.Step();f.Key(ImGuiKey_Enter);f.Step();
  Near(f.One("update_key","time"),3.,"Curve numeric time uses display frames");
  f.Tool("Timeline");
  f.editor.OpenSettingsForDiagnostics();f.Step();f.Step();f.Click("Time and editing",0,settings);f.Step();
  type("##pref_ui_scale","1.25");Near(f.One("set_editor_preferences","ui_scale"),1.25,"Interface scale preference");f.Step();
  Require(std::abs(TestWindow(transport)->Size.y-88*f.scale*1.25)<1.01,"Interface scale affects native toolbar layout");
  Near(f.Find("##preview",0,transport).bounds.GetHeight(),36*f.scale*1.25,"Interface scale affects real button height");
  type("##pref_inspector_width","520");Near(f.One("set_editor_preferences","inspector_width"),520,"Inspector width preference");f.Step();
  Require(std::abs(TestWindow(kInspector)->Size.x-520*f.scale*1.25)<1.01,"Inspector preference affects its real width");
  type("##pref_panel_opacity","0.5");Near(f.One("set_editor_preferences","panel_opacity"),.5,"Panel opacity preference");f.Step();
  bool dimmed=false;for(const auto& vertex:TestWindow(transport)->DrawList->VtxBuffer)if(vertex.col==IM_COL32(0,0,0,80))dimmed=true;
  Require(dimmed,"Opacity preference did not change rendered panel background");
  f.editor.backdrop_texture=ImTextureID(1234);f.Step();
  auto blurred=[&](){for(const auto& command:TestWindow(transport)->DrawList->CmdBuffer)if(command.GetTexID()==ImTextureID(1234))return true;return false;};
  Require(blurred(),"Enabled backdrop texture was not drawn");
  // The lower appearance fields require ordinary scrolling at native heights.
  panel=TestWindow(settings);ImGui::SetScrollY(panel,panel->ScrollMax.y);f.Step();f.Step();
  f.commands.clear();f.Click("Blur behind panels",0,settings);f.Step();Require(f.One("set_editor_preferences","blur_enabled")==false&&!blurred(),"Disabling blur did not remove the sampled backdrop");
  f.commands.clear();f.Click("Show tooltips",0,settings);f.Step();Require(f.One("set_editor_preferences","show_tooltips")==false,"Tooltip preference did not apply");
  f.commands.clear();f.Click("Position track",0,settings);f.Step();Require(f.One("set_editor_preferences","timeline_position")==false,"Track visibility preference missing");
  panel=TestWindow(settings);ImGui::SetScrollY(panel,0);f.Step();f.Step();
  f.commands.clear();f.Click("Keep demo navigation visible",0,settings);f.Step();Require(f.One("set_editor_preferences","demo_navigation_pinned")==true,"Pinned demo setting missing");
  Require(TestWindow("Demo navigation")&&TestWindow("Demo navigation")->Active,"Pinned demo navigation did not appear");

}

void CompactScaleAndTimeline(float height){
  Fixture f(height);constexpr const char* transport="##CS2CinematicTransport";constexpr const char* timeline="Camera timeline / composition";
  f.state["editor_preferences"]={{"compact_transport",true},{"ui_scale",1.5}};f.Step();f.Step();
  const float scaled=f.scale*1.5f;
  for(const auto& row:{std::vector<const char*>{"Demo","Camera","Timeline","Map","Curves","Inspector","Project","Settings","Export","Hide"},std::vector<const char*>{"##preview","##prev","##next","##capture","##render_scope","Sync demo"}}){
    float right=0;for(const char* label:row){const auto item=f.Find(label,0,transport);Require(item.bounds.Min.x>=right-.1f,"Maximum-scale unified controls overlap");right=item.bounds.Max.x;Require(right<=ImGui::GetIO().DisplaySize.x,"Maximum-scale unified control outside viewport");}
  }
  auto* work=TestWindow("##CS2CameraWorkspace");auto* inspector=TestWindow(kInspector);auto* footer=TestWindow(transport);
  Require(work->Pos.x+work->Size.x<=inspector->Pos.x&&work->Pos.y+work->Size.y<=footer->Pos.y&&inspector->Pos.y+inspector->Size.y<=footer->Pos.y,"Workspace, inspector and toolbar overlap");
  f.commands.clear();f.editor.OpenTimelineForDiagnostics();f.Step();f.Step();Require(f.commands.empty(),"Opening timeline changed the project");
  const auto input=f.Find("Duration##composition_duration",0,timeline).bounds.GetCenter();
  f.Mouse(input);ImGui::GetIO().AddMouseButtonEvent(2,true);f.Step();Require(ImGui::IsAnyItemActive(),"Composition duration MMB scrub did not acquire active ID");
  f.Mouse(ImVec2(input.x+std::ceil(4*scaled),input.y));f.Step();Require(f.commands.empty(),"Composition numeric scrub committed before release");
  f.Key(ImGuiKey_Space);f.Key(ImGuiKey_K);Require(f.commands.empty(),"Composition numeric scrub leaked shortcuts");
  ImGui::GetIO().AddMouseButtonEvent(2,false);f.Step();f.Step();Near(f.One("set_project","duration"),9.,"Composition duration shares the 1-unit middle scrub");
  Require(f.wire_commands.back().method=="guarded_mutation","Composition duration resize bypassed CAS");
  f.editor.OpenSettingsForDiagnostics();f.Step();f.Step();f.commands.clear();f.Click("##pref_frame_rate",0,"Editor settings");ImGui::GetIO().AddInputCharactersUTF8("48");f.Step();f.Key(ImGuiKey_Enter);f.Step();
  Require(f.One("set_editor_preferences","frame_rate")==48,"Grid FPS setting did not commit");f.editor.settings_visible=false;f.Step();
  const auto revision=f.state["revision"];f.commands.clear();f.Click("Position",0,timeline);f.Step();Require(f.One("set_editor_preferences","timeline_position")==false&&f.state["revision"]==revision,"Timeline lane visibility changed project history");
}

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

  f.Click("Shot");
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

void InspectorDurationValidation() {
  Fixture f;
  f.state["editor_preferences"]["resize_mode"]="scale";
  auto& project=f.state["project"];
  project["duration"]=10.;project["keys"].erase(project["keys"].begin()+2,project["keys"].end());
  project["keys"][0]["time"]=1.;project["keys"][1]["time"]=1.001;
  project=cs2_live::ToJson(cs2_live::ReadProject(project));
  f.editor.OpenShotSettingsForDiagnostics();f.Step();f.Step();
  f.Type("##shot_duration","0");
  Require(f.commands.size()==1&&f.commands[0].method=="set_project","Minimum scale duration did not produce one valid edit");
  const auto resized=cs2_live::ReadProject(f.state["project"]);
  Require(resized.keys[1].time-resized.keys[0].time>=1e-6,"Inspector scale crossed the native key separation bound");
  Require(f.editor.visible,"Duration entry closed the editor");

  // A rejected source snapshot must remain a local, visible error; throwing
  // out of EditorUi::Draw causes the Vulkan host to disable the whole overlay.
  f.commands.clear();const auto valid=f.state["project"];
  f.state["project"]["render_range"]["out"]=20.;
  const auto message=f.DrawText();
  Require(message.find("Duration was not changed: render range Out")!=std::string::npos,"Rejected duration state was not explained in the inspector");
  Require(f.commands.empty()&&f.editor.visible,"Rejected duration source emitted an edit or closed the editor");
  f.Find("Use current demo tick");
  f.state["project"]=valid;f.Step();f.Type("##shot_duration","2");
  Require(f.commands.size()==1,"Inspector did not recover after invalid duration source");
  cs2_live::ReadProject(f.state["project"]);
  Require(f.DrawText().find("Duration was not changed:")==std::string::npos,"Successful duration edit left a stale failure message");
}

void TimeUnitsDuringNumericEdit() {
  Fixture f;f.state["editor_preferences"]["resize_mode"]="scale";
  f.editor.OpenShotSettingsForDiagnostics();f.Step();f.Step();
  f.commands.clear();f.Click("##shot_duration");
  ImGui::GetIO().AddInputCharactersUTF8("10");f.Step();
  f.state["editor_preferences"]["time_unit"]="frames";f.Step();
  Require(f.commands.empty(),"Unit preference change committed an unfinished duration");
  f.Key(ImGuiKey_Enter);f.Step();
  Near(f.One("set_project","duration"),10,"Typed seconds survived remote switch to frames");
  f.BeginScrub("##shot_duration");
  f.state["editor_preferences"]["frame_rate"]=120;f.Step();
  f.MoveScrub(1);f.EndScrub();
  Near(f.One("set_project","duration"),10+1./60,"Middle drag kept its original frame-rate unit");
  Require(f.commands.size()==1,"Rate change split one numeric gesture into multiple edits");
  const auto original=f.state["project"]["keys"];
  f.state["editor_preferences"]["time_unit"]="seconds";f.state["editor_preferences"]["resize_mode"]="keep";f.Step();
  f.commands.clear();f.Click("##shot_duration");ImGui::GetIO().AddInputCharactersUTF8("12");f.Step();
  f.state["editor_preferences"]["resize_mode"]="scale";f.Step();f.Key(ImGuiKey_Enter);f.Step();
  Near(f.One("set_project","duration"),12,"Typed keep duration");
  Require(f.state["project"]["keys"]==original,"Remote resize preference changed an active Keep edit into Scale");
}

void NumericEditAcrossDocumentImport() {
  Fixture f;f.state["project_document_id"]=7;f.state["editor_preferences"]["resize_mode"]="scale";
  f.editor.OpenShotSettingsForDiagnostics();f.Step();f.Step();
  f.commands.clear();f.Click("##shot_duration");ImGui::GetIO().AddInputCharactersUTF8("12");f.Step();
  f.state["project_document_id"]=8;f.state["revision"]=2;
  f.state["project"]["duration"]=20.;
  for(auto& key:f.state["project"]["keys"])key["time"]=key["time"].get<double>()*2;
  const auto imported=f.state["project"];f.Step();f.Key(ImGuiKey_Enter);f.Step();
  Require(f.commands.empty()&&f.state["project"]==imported,"Pending numeric edit crossed into a newly imported document");
  f.Type("##shot_duration","24");
  Near(f.One("set_project","duration"),24,"New document numeric editing recovered");
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
  f.Click("Shot");
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

  f.Tool("Timeline");constexpr const char* timeline="Camera timeline / composition";
  auto in=f.Find("##composition_in",0,timeline).bounds.GetCenter();
  auto out=f.Find("##composition_out",0,timeline).bounds.GetCenter();
  f.commands.clear();f.ClickAt(in);Require(f.commands.empty(),"Region handle click created history");
  f.Mouse(in);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(in.x+(out.x-in.x)*.25f,in.y));f.Step();
  Require(f.commands.empty(),"Region drag created history before release");ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Near(f.One("set_project","render_range")["in"],2,"Composition In handle drag");
  f.commands.clear();out=f.Find("##composition_out",0,timeline).bounds.GetCenter();f.Mouse(out);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(out.x-40*f.scale,out.y));f.Step();f.Key(ImGuiKey_Escape);ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.commands.empty(),"Escape committed region drag");Near(f.state["project"]["render_range"]["out"],8,"Escape preserves Out");
}

void TimelineScrubAndRangeHandles(float height){
  Fixture f(height);constexpr const char* timeline="Camera timeline / composition";f.Tool("Timeline");
  f.state["project"]["render_range"]={{"mode","composition"},{"in",0},{"out",8}};f.state["sync_demo"]=true;f.Step();
  const auto canvas=f.Find("##camera_composition_canvas",0,timeline).bounds;
  const float left=f.Find("##composition_key_Position_0",0,timeline).bounds.GetCenter().x;
  const float end=f.Find("##composition_end",0,timeline).bounds.GetCenter().x;
  auto scrub=[&](float fraction){f.commands.clear();const ImVec2 start(left+2*f.scale,canvas.Min.y+22*f.scale);f.Mouse(start);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();
    for(int i=1;i<=20;++i){f.Mouse(ImVec2(left+(end-left)*fraction*i/20,start.y));f.Step();}
    Require(f.commands.empty(),"Composition scrub sent command before release");ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();};
  scrub(.5);Near(f.One("scrub","time"),4,"Composition ruler scrub");Require(f.commands[0].arguments["seek_demo"]==true&&f.state["project"]["render_range"]["mode"]=="composition","Scrub changed scope or ignored Sync");
  scrub(.25);Require(std::abs(f.One("scrub","time").get<double>()-2)<=8./(end-left)+.5/64,"Composition ruler quarter exceeds pixel and tick snap precision");
  f.state["project"]["render_range"]["mode"]="region";f.Step();scrub(.5);Near(f.One("scrub","time"),4,"Region mode keeps ruler scrub");Require(f.state["project"]["render_range"]["in"]==0,"Region handle stole ruler seek");
  const auto handle=f.Find("##composition_in",0,timeline).bounds.GetCenter();f.commands.clear();f.Mouse(handle);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(handle.x+(end-left)*.25f,handle.y));f.Step();ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();Near(f.One("set_project","render_range")["in"],2,"Separate region handle trim");
}

void PendingFullDemoSeek(float height) {
  Fixture f(height);f.Tool("Demo");constexpr const char* transport="Demo navigation";
  f.state["tick"]=38664;
  f.state["demo_timeline"]={{"available",true},{"metadata_revision",1},
    {"demo",{{"path","/pending.dem"},{"fingerprint",{{"sha256","pending"}}},
              {"playback_ticks",111040},{"tick_rate",64},{"duration_seconds",1735}}},
    {"rounds",Json::array()},{"selected_player",nullptr}};
  f.state["demo_seek"]={{"active",true},{"target_tick",55520},{"dispatched_tick",10000},{"queued",true}};
  f.Step();const auto bounds=f.Find("##demo_timeline",0,transport).bounds;
  const float left=bounds.Min.x+10*f.scale,width=bounds.GetWidth()-20*f.scale;
  const auto point=[&](int tick){return ImVec2(left+width*float(double(tick)/111040),bounds.Min.y+37*f.scale);};
  const ImVec2 pendingPoint=point(55520);
  f.state["demo_seek"]["target_tick"]=int(std::llround((std::floor(pendingPoint.x)-left)/width*111040));f.Step();
  f.commands.clear();f.ClickAt(pendingPoint);
  Require(f.commands.empty(),"Repeated pending demo destination started a duplicate seek");
  f.ClickAt(point(70000));
  Require(std::abs(f.One("demo_control","tick").get<int>()-70000)<=int(std::ceil(111040/width)),
          "Pending demo seek prevented selecting a newer destination");
  Require(f.commands.size()==1 && f.state["tick"]==38664,
          "Pending demo seek invented a completed native game tick");
}

void IndependentShortcutsPanel() {
  Fixture f;
  f.Key(ImGuiKey_F10);
  Require(f.editor.shortcuts_visible, "F10 did not open shortcuts");
  auto* window = TestWindow(kShortcuts);
  Require(window && window->Active, "Shortcuts must have an independent active window");
  Require(!(window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal)),
          "Shortcuts must be nonmodal");
  Require(TestWindow(kInspector)->Active, "Inspector vanished while shortcuts open");
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
  f.Click("Project",0,"##CS2CinematicTransport");f.Step();f.Choose("Keyboard shortcuts");
  Require(f.editor.shortcuts_visible, "Inspector Hotkeys did not open separate panel");
  Require(f.commands.empty(), "Opening/moving shortcuts emitted native commands");
}

void ExactRangeBounds() {
  Fixture f;
  f.state["project"]["keys"] = Json::array();
  f.state["project"]["duration"] = 3.14;
  f.state["project"]["render_range"] = {{"mode", "region"}, {"in", 3.139}, {"out", 3.14}};
  f.Step();
  f.Click("Shot");
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
  f.Section("Scene");
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("Scene settings");
  f.Step();
  f.Click("Lens");
  f.Step();
  f.commands.clear();
  f.Click("Depth of field renderer");
  Require(f.One("set_scene", "settings")["depth_of_field"] == false, "DOF checkbox did not send typed change");
  f.Step();f.Step(); // Let the inactive-effect hint resize the anchored panel.
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

void SceneCombatControls(float height) {
  Fixture f(height);
  const std::vector<std::string> controls={"xray_enabled","blood_enabled","metal_sparks_enabled"};
  f.state["scene_capabilities"]=Json::object();
  for(const auto& id:controls) {
    const auto& control=cs2_live::SceneControlById(id);
    Require(control.boolean && std::string(control.group)=="Combat","Combat control metadata is not a boolean in the Combat group");
    f.state["scene_capabilities"][id]={{"available",true},{"type","boolean"},{"value",true},
        {"label",control.label},{"group",control.group}};
  }
  for(const char* id:{"blood_amount","spark_count","spark_spread"})
    f.state["scene_capabilities"][id]={{"available",false},{"type","number"},{"value",nullptr},
        {"label",id},{"group","Combat"},{"reason","Particle adapter is not available"}};
  f.Step();f.Section("Scene");f.Step();f.Click("Scene settings");f.Step();f.Click("Combat");f.Step();
  for(const auto& id:controls) {
    const auto& control=cs2_live::SceneControlById(id);
    for(bool expected:{false,true}) {
      f.commands.clear();f.Click(control.label);
      const auto& settings=f.One("set_scene","settings");
      Require(settings.size()==1 && settings.contains(id) && settings.at(id).is_boolean() && settings.at(id)==expected,
              "Combat checkbox did not emit one typed boolean for "+id);
      f.Step();f.Step();
    }
  }
  for(const auto& entry:f.submitted.items)
    for(const char* label:{"blood_amount","spark_count","spark_spread"})
      Require(entry.second.label!=label,"Unsupported particle amount/spread leaked into Combat settings");
  f.commands.clear();
  for(const auto& id:controls)f.state["scene_capabilities"][id]["available"]=false;
  f.Step();f.Step();
  for(const auto& entry:f.submitted.items) {
    Require(entry.second.label!="Combat","Combat group remained visible without available controls");
    for(const auto& id:controls)
      Require(entry.second.label!=cs2_live::SceneControlById(id).label,"Unavailable Combat checkbox remained visible");
  }
  Require(f.commands.empty(),"Capability refresh emitted an unintended scene mutation");
}

void SceneCleanFrameXray(bool available) {
  Fixture f;
  const auto xray=cs2_live::SceneControlById("xray_enabled");
  f.state["scene_capabilities"]={
      {"xray_enabled",{{"available",available},{"type","boolean"},{"value",true},{"label",xray.label},{"group","Combat"}}},
      {"hud_visible",{{"available",true},{"type","boolean"},{"value",true},{"label","Game HUD"},{"group","Environment"}}},
      {"blood_enabled",{{"available",true},{"type","boolean"},{"value",true},{"label","Blood effects"},{"group","Combat"}}},
      {"metal_sparks_enabled",{{"available",true},{"type","boolean"},{"value",true},{"label","Metal-impact sparks"},{"group","Combat"}}}};
  f.Step();f.Section("Scene");f.Step();f.Click("Scene settings");f.Step();
  f.commands.clear();f.Click("Clean frame");
  const auto& settings=f.One("set_scene","settings");
  Require(settings.size()==(available?2:1) && settings.at("hud_visible").is_boolean() && settings.at("hud_visible")==false,
          "Clean frame changed unrelated combat effects or omitted the HUD");
  if(available)Require(settings.at("xray_enabled").is_boolean() && settings.at("xray_enabled")==false,
                       "Clean frame did not disable available X-ray with a typed boolean");
  else Require(!settings.contains("xray_enabled"),"Clean frame requested an unavailable X-ray control");
  Require(!f.editor.visible,"Clean frame did not hide the editor");
}

void SceneBloomAutomatic() {
  Fixture f;
  f.state["scene_capabilities"]={{"bloom_strength",{{"available",true},{"type","number"},{"value",3},
      {"label","Bloom strength (-1 = automatic)"},{"group","Post processing"},{"minimum",-1},{"maximum",10}}}};
  f.Step();f.Section("Scene");f.Step();f.Click("Scene settings");f.Step();f.Click("Post processing");f.Step();
  Require(f.DrawText().find("-1 = automatic bloom strength.")!=std::string::npos,"Automatic bloom hint missing");
  f.commands.clear();f.Type("##scene_value","-1");
  Near(f.One("set_scene","settings")["bloom_strength"].get<double>(),-1,"Automatic bloom strength");
  f.commands.clear();f.Type("##scene_value","2.5");
  Near(f.One("set_scene","settings")["bloom_strength"].get<double>(),2.5,"Manual bloom strength");
  f.state["scene_capabilities"]["bloom_strength"]["minimum"]=0;f.Step();f.Step();
  f.commands.clear();f.Type("##scene_value","-1");
  Near(f.One("set_scene","settings")["bloom_strength"].get<double>(),0,"Runtime bloom minimum");
}

void SceneDraftAcknowledgement(bool enter) {
  Fixture f;
  f.state["scene_capabilities"]={{"dof_far_blurry",{{"available",true},{"type","number"},{"value",2000.0},{"label","Far blur distance"},{"group","Lens"},{"minimum",0},{"maximum",100000}}}};
  f.Step();f.Section("Scene");f.Step();f.Click("Scene settings");f.Step();f.Click("Lens");f.Step();
  f.echo_scene=false;f.Type("##scene_value","20000",enter);
  Near(f.One("set_scene","settings")["dof_far_blurry"].get<double>(),20000,"Committed far blur");
  for(int i=0;i<120;++i)f.Step(); // Stale native DTO plus newer unrelated revision.
  f.BeginScrub("##scene_value");f.MoveScrub(1);f.EndScrub();
  Near(f.One("set_scene","settings")["dof_far_blurry"].get<double>(),20001,"Pending draft survived stale acknowledgement");
  f.state["scene_capabilities"]["dof_far_blurry"]["value"]=20001;f.Step();
  f.Type("##scene_value","20000",enter);
  f.state["error"]="dof_far_blurry engine refused the requested value";f.Step();
  Require(f.DrawText().find("engine refused the requested value")!=std::string::npos,"Native numeric rejection was not visible");
  for(int i=0;i<40;++i)f.Step();
  f.BeginScrub("##scene_value");f.MoveScrub(1);f.EndScrub();
  Near(f.One("set_scene","settings")["dof_far_blurry"].get<double>(),20002,"Rejected draft restored actual engine value");
}

void SceneBooleanAcknowledgement() {
  Fixture f;
  f.state["scene_capabilities"]={{"fullbright",{{"available",true},{"type","boolean"},{"value",false},{"label","Fullbright lighting"},{"group","Lighting"}}}};
  f.Step();f.Section("Scene");f.Step();f.Click("Scene settings");f.Step();f.Click("Lighting");f.Step();
  f.echo_scene=false;f.commands.clear();f.Click("Fullbright lighting");
  Require(f.One("set_scene","settings")["fullbright"]==true,"Legacy lighting checkbox lost boolean type");
  for(int i=0;i<120;++i)f.Step();
  Require(f.DrawText().find("Waiting for engine readback...")!=std::string::npos,"Boolean request did not show pending status");
  // A numeric 1 is not a typed boolean acknowledgement. Clicking the still-on
  // pending checkbox must send false, even though the stale DTO says false/1.
  f.state["scene_capabilities"]["fullbright"]["value"]=1;f.Step();
  f.commands.clear();f.Click("Fullbright lighting");
  Require(f.One("set_scene","settings")["fullbright"]==false,"Stale or mistyped readback undid pending boolean");
  f.state["scene_capabilities"]["fullbright"]["value"]=false;f.Step();
  Require(f.DrawText().find("Waiting for engine readback...")==std::string::npos,"Typed boolean readback did not acknowledge request");
  f.commands.clear();f.Click("Fullbright lighting");
  f.state["error"]="fullbright was rejected by the engine";f.Step();
  const auto text=f.DrawText();
  Require(text.find("fullbright was rejected by the engine")<text.find("Reset scene settings"),
          "Boolean rejection was not displayed next to the control");
  f.commands.clear();f.Click("Fullbright lighting");
  Require(f.One("set_scene","settings")["fullbright"]==true,"Rejected checkbox did not restore actual engine state");
}

void SceneShadingSelector() {
  Fixture f;
  f.state["scene_capabilities"]={
      {"fullbright",{{"available",true},{"type","boolean"},{"value",true},{"label","Fullbright lighting"},{"group","Lighting"}}},
      {"shading_mode",{{"available",true},{"type","number"},{"value",2},{"label","Legacy shading label"},{"group","Lighting"},{"minimum",0},{"maximum",2},{"integer",true}}},
      {"exposure",{{"available",true},{"type","number"},{"value",0},{"label","Exposure"},{"group","Lighting"},{"minimum",0},{"maximum",16}}}};
  f.Step();f.Section("Scene");f.Step();f.Click("Scene settings");f.Step();f.Click("Lighting");f.Step();
  for(const auto& entry:f.submitted.items)
    Require(entry.second.label!="Fullbright lighting","Duplicate alias checkbox leaked beside shading selector");
  Require(f.DrawText().find("0 = automatic exposure")!=std::string::npos,"Automatic exposure meaning was hidden");
  f.echo_scene=false;f.commands.clear();f.ClickScoped("##scene_shading","shading_mode","Lighting");f.Step();f.Choose("Normal");
  Require(f.One("set_scene","settings")["shading_mode"].is_number_integer() &&
          f.commands[0].arguments["settings"]["shading_mode"]==0,"Shading selector did not emit integer enum");
  for(int i=0;i<120;++i)f.Step();
  f.commands.clear();f.ClickScoped("##scene_shading","shading_mode","Lighting");f.Step();f.Choose("Normal");
  Require(f.commands.empty(),"Stale shading readback replaced pending Normal selection");
  f.state["scene_capabilities"]["shading_mode"]["value"]=0;f.Step();f.Step();f.Step();
  f.ClickScoped("##scene_shading","shading_mode","Lighting");f.Step();f.Choose("Fullbright");
  Require(f.One("set_scene","settings")["shading_mode"]==1,"Fullbright enum selection lost");
  f.state["error"]="shading_mode was rejected by the engine";f.Step();
  const auto text=f.DrawText();
  Require(text.find("shading_mode was rejected by the engine")<text.find("Reset scene settings"),"Shading rejection was not local");
}

void SceneDofActivation(bool renderer_available) {
  Fixture f;
  f.state["scene_capabilities"]={
      {"depth_of_field",{{"available",renderer_available},{"type","boolean"},{"value",false},{"label","Depth of field renderer"},{"group","Lens"}}},
      {"dof_override",{{"available",true},{"type","boolean"},{"value",false},{"label","Depth of field"},{"group","Lens"}}},
      {"dof_far_blurry",{{"available",true},{"type","number"},{"value",2000},{"label","Far blur distance"},{"group","Lens"},{"minimum",0},{"maximum",100000}}}};
  f.Step();f.Section("Scene");f.Step();f.Click("Scene settings");f.Step();f.Click("Lens");f.Step();
  Require(f.DrawText().find("Depth of field is off")!=std::string::npos,"Inactive DOF did not explain unchanged image");
  for(const auto& entry:f.submitted.items)
    Require(entry.second.label!="Depth of field renderer","DOF has two competing activation checkboxes");
  f.commands.clear();f.Click("Depth of field");
  const auto settings=f.One("set_scene","settings");
  Require(settings["dof_override"]==true,"DOF checkbox did not enable focus override");
  Require(settings.size()==(renderer_available?2:1),"DOF activation included an unavailable engine control");
  if(renderer_available)Require(settings["depth_of_field"]==true,"DOF activation omitted registered renderer gate");
  else Require(f.DrawText().find("Renderer status is unavailable")!=std::string::npos,"Unknown renderer state was presented as verified");
  f.Step();f.Step();f.Step();
  f.Type("##scene_value","20000");
  Near(f.One("set_scene","settings")["dof_far_blurry"].get<double>(),20000,"Available DOF distance stayed editable");
  f.commands.clear();f.Click("Depth of field");
  Require(f.One("set_scene","settings")["dof_override"]==false,"DOF disable did not disable focus override");
  if(renderer_available)Require(f.commands[0].arguments["settings"]["depth_of_field"]==false,"DOF disable omitted renderer gate");
}

void FullDemoTimeline(float height) {
  Fixture f(height);f.Tool("Demo");constexpr const char* transport="Demo navigation";
  f.state["tick"]=38664;f.state["cursor"]=1.65;
  f.state["demo_timeline"]={{"available",true},{"version",1},{"metadata_revision",1},{"demo",{{"path","/demo.dem"},{"fingerprint",{{"sha256","fixture"}}},{"playback_ticks",111040},{"tick_rate",64},{"duration_seconds",1735}}},
    {"rounds",Json::array({{{"tick",0},{"round",1}},{{"tick",30000},{"round",2}},{{"tick",60000},{"round",3}}})},
    {"selected_player",{{"steam_id","76561198000000001"},{"name","Alpha"},{"kills",{20000,80000}},{"deaths",{40000}}}}};
  f.Step();const Json project=f.state["project"];const auto timeline=f.Find("##demo_timeline",0,transport).bounds;
  const auto point=[&](int tick,float y){return ImVec2(timeline.Min.x+10*f.scale+(timeline.GetWidth()-20*f.scale)*float(double(tick)/111040),timeline.Min.y+y*f.scale);};
  f.ClickAt(ImVec2(point(20000,9).x+4*f.scale,point(20000,9).y));Require(f.One("demo_control","tick")==20000,"Kill marker did not seek its absolute demo tick");
  f.commands.clear();f.ClickAt(ImVec2(point(40000,21).x+3*f.scale,point(40000,21).y));Require(f.One("demo_control","tick")==40000,"Death marker did not seek its absolute demo tick");
  f.commands.clear();f.ClickAt(point(55520,37));Require(std::abs(f.One("demo_control","tick").get<int>()-55520)<=int(std::ceil(111040/(timeline.GetWidth()-20*f.scale))),"Full demo scrub used camera duration");
  Require(f.state["project"]==project,"Demo timeline altered camera keys or shot settings");
  f.commands.clear();f.Mouse(point(100,37));ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,true);f.Step();
  f.Mouse(ImVec2(timeline.Max.x+100*f.scale,point(100,37).y));f.Step();Require(f.commands.empty(),"Demo drag submitted before release");
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,false);f.Step();Require(f.One("demo_control","tick")==111040,"Demo drag did not clamp to authoritative end");
  f.commands.clear();f.Mouse(point(200,37));ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,true);f.Step();f.Key(ImGuiKey_Escape);
  ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left,false);f.Step();Require(f.commands.empty(),"Escape did not cancel full-demo scrub");

  f.commands.clear();f.Click("##prev_round",0,transport);Require(f.One("demo_control","tick")==30000,"Previous round used a camera key");
  f.commands.clear();f.Click("##next_round",0,transport);Require(f.One("demo_control","tick")==60000,"Next round used a camera key");
  f.commands.clear();f.Click("-15 s##demo_back",0,transport);Require(f.One("demo_control","tick")==37704,"Demo 15 second step used shot time");
  f.commands.clear();f.Click("+15 s##demo_forward",0,transport);Require(f.One("demo_control","tick")==39624,"Demo forward step used shot time");
  const std::string text=f.DrawText();
  Require(text.find("28:55")!=std::string::npos && text.find("Full demo")!=std::string::npos && text.find("Camera shot")==std::string::npos,"Full demo total/strip labels were missing");
  Require(text.find("38664")!=std::string::npos,"Full-demo playhead did not use actual game tick");
  f.state["demo_timeline"]["selected_player"]={{"steam_id","76561198000000002"},{"name","Bravo"},{"kills",{70007}},{"deaths",{90003}}};f.Step();f.commands.clear();
  f.ClickAt(ImVec2(point(70007,9).x+4*f.scale,point(70007,9).y));Require(f.One("demo_control","tick")==70007,"Player change did not update selected-player event markers");
  f.state["demo_timeline"]["metadata_revision"]=2;f.state["demo_timeline"]["selected_player"]["kills"]={71007};f.Step();f.commands.clear();
  f.ClickAt(ImVec2(point(71007,9).x+4*f.scale,point(71007,9).y));Require(f.One("demo_control","tick")==71007,"Same-file metadata refresh left stale cached markers");


  f.state["demo_timeline"]["available"]=false;f.state["demo_timeline"]["reason"]="Metadata is loading";f.Step();f.commands.clear();f.ClickAt(point(20000,9));Require(f.commands.empty(),"Unavailable metadata permitted a guessed demo seek");
  f.state["demo_timeline"]["available"]=true;f.state["demo_timeline"]["selected_player"]=nullptr;f.Step();
  Require(f.DrawText().find("Kills ")==std::string::npos,"Missing player metadata reused another player's event markers");
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
  f.Section("Scene");
  for (int i = 0; i < 3; ++i) f.Step();
  f.Click("Player agents");
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
  for (const auto& entry : f.submitted.items)
    Require(entry.second.label != "Import..." && entry.second.label != "##cosmetic_category" &&
            entry.second.label != "Apply gloves" && entry.second.label != "Apply finish",
            "Removed skin controls remained in the agent panel");
}

void PresentationSelectors() {
  Fixture f;
  f.state["presentation"] = {{"view", "player"}, {"hud", "gameplay"},
      {"requested_view", "player"}, {"requested_hud", "gameplay"},
      {"capabilities", Json::object()}};
  for (const char* id : {"player", "free", "gameplay", "demo", "hidden"})
    f.state["presentation"]["capabilities"][id] = {{"requestable", true}, {"available", false}};
  f.Step();
  f.Section("Scene");
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
  f.Click("Camera");
  for (int i = 0; i < 3; ++i) f.Step();
  f.SelectKey();
  f.Type("##fov", "70");
  Require(f.One("update_key", "fov") == 70, "FOV editing must update the selected animation key");
}

void ObservedPlayerSelector() {
  Fixture f;
  f.state["presentation"] = {{"view", "player"}, {"hud", "demo"},
      {"selected_player", nullptr}, {"selection_pending", false},
      {"capabilities", {{"select_player", {{"requestable", true}, {"available", true}}}}}};
  f.state["cosmetics"] = {{"stale", false}, {"players", Json::array({
      {{"steam_id", "76561198000000001"}, {"nickname", "Alpha"}, {"team", 3}},
      {{"steam_id", "76561198000000002"}, {"nickname", "Bravo"}, {"team", 2}},
      {{"steam_id", "76561198000000003"}, {"nickname", "Bravo"}, {"team", 3}}})}};
  f.Step();
  f.Section("Scene");f.Step();
  const Json keys = f.state["project"]["keys"];
  f.commands.clear();
  f.Click("##observed_player");
  f.Step();
  std::string popup;
  bool duplicate_distinguished = false;
  for (const auto& entry : f.submitted.items) {
    if (entry.second.label == "Bravo [T] #00002") popup = entry.second.window;
    if (entry.second.label == "Bravo [CT] #00003") duplicate_distinguished = true;
  }
  Require(!popup.empty(), "Observer player selector did not list demo nicknames");
  Require(duplicate_distinguished, "Duplicate nicknames were not distinguishable by team and SteamID suffix");
  f.Click("Bravo [T] #00002", 0, popup.c_str());
  Require(f.commands.size() == 1 && f.commands[0].method == "select_player",
          "Choosing a player did not emit exactly one spectator request");
  Require(f.commands[0].arguments == Json{{"steam_id", "76561198000000002"}},
          "Spectator selection used a nickname/index instead of exact SteamID");
  Require(f.state["project"]["keys"] == keys, "Player selection changed camera keys");
  f.commands.clear();
  f.state["cosmetics"]["stale"] = true;
  f.Step();
  f.Click("##observed_player");
  Require(f.commands.empty(), "Stale demo players allowed spectator selection");
  f.state["cosmetics"]["stale"] = false;
  f.Step();
  f.Find("##observed_player");
  f.state["presentation"]["capabilities"]["select_player"]["requestable"] = false;
  f.Step();
  f.Click("##observed_player");
  Require(f.commands.empty(), "Unavailable spectator adapter emitted a selection request");
}

void TimelinePlayerSelectorWithoutEntities(float height) {
  Fixture f(height);f.Tool("Demo");constexpr const char* transport="Demo navigation";
  f.state["presentation"]={{"selected_player",nullptr},{"capabilities",{{"select_player",{{"requestable",false},{"reason","client entity inspection requires the validated Linux client build-ID"}}}}}};
  f.state["cosmetics"]={{"stale",true},{"players",Json::array()}};
  f.state["demo_timeline"]={{"available",true},{"version",1},{"metadata_revision",1},
      {"demo",{{"path","/demo.dem"},{"fingerprint",{{"sha256","fixture"}}},{"playback_ticks",111040},{"tick_rate",64},{"duration_seconds",1735}}},
      {"rounds",Json::array()},{"marker_player",nullptr},{"selected_player",nullptr},
      {"players",Json::array({{{"steam_id","76561198000000001"},{"name","L201742"}},{{"steam_id","76561198000000002"},{"name","Bravo"}},{{"steam_id","76561198000000003"},{"name","Bravo"}}})}};
  f.Step();const auto project=f.state["project"];const auto presentation=f.state["presentation"];
  f.commands.clear();f.Click("##timeline_player",0,transport);f.Step();
  std::string popup;
  bool duplicate_distinguished=false;
  for(const auto& entry:f.submitted.items) {
    if(entry.second.label=="L201742")popup=entry.second.window;
    if(entry.second.label=="Bravo #76561198000000003")duplicate_distinguished=true;
  }
  Require(!popup.empty() && duplicate_distinguished,"Recorded marker roster was disabled by unsupported entities or ambiguous names");
  f.Click("L201742",0,popup.c_str());
  Require(f.commands.size()==1 && f.commands[0].method=="select_timeline_player" && f.commands[0].arguments==Json{{"steam_id","76561198000000001"}},"Marker selection requested observer mutation instead of exact recorded SteamID filter");
  Require(f.state["project"]==project && f.state["presentation"]==presentation,"Marker selection edited camera project or observer state");
  f.state["demo_timeline"]["marker_player"]="76561198000000001";
  f.state["demo_timeline"]["selected_player"]={{"steam_id","76561198000000001"},{"name","L201742"},{"kills",{20000,80000}},{"deaths",{40000}}};
  f.Step();Require(f.DrawText().find("Kills 2")!=std::string::npos,"Explicit marker filter did not display recorded events");
  f.commands.clear();f.Click("##timeline_player",0,transport);f.Step();f.Choose("Follow viewed player");
  Require(f.commands.size()==1 && f.commands[0].method=="select_timeline_player" && f.commands[0].arguments["steam_id"].is_null(),"Clearing marker filter did not send typed null");
  f.state["demo_timeline"]["available"]=false;f.Step();f.commands.clear();f.Click("##timeline_player",0,transport);
  Require(f.commands.empty(),"Unavailable demo metadata allowed a marker selection request");
}

void DiagnosticShotLayout() {
  Fixture f(1440);f.state["editor_preferences"]["ui_scale"]=1.5;f.Step();f.Step();
  f.Section("Scene");f.Step();
  const auto before=f.state;
  f.commands.clear();
  f.editor.OpenShotSettingsForDiagnostics();f.Step();f.Step();
  auto* window=TestContentWindow(kInspector);
  Require(window && f.state["editor_preferences"]["inspector_section"]=="scene",
          "Diagnostic token did not open Shot settings");
  f.Find("##shot_start");f.Find("##render_scope");
  f.Mouse(ImVec2(window->Pos.x+100,window->Pos.y+200));
  ImGui::GetIO().AddMouseWheelEvent(0,-15);f.Step();f.Step();
  Require(window->Scroll.y>0,"Diagnostic scroll test did not scroll the inspector");
  f.editor.OpenShotSettingsForDiagnostics();f.Step();f.Step();
  Near(window->Scroll.y,0,"Diagnostic inspector scroll reset");
  Require(f.commands.empty() && f.state==before,"Diagnostic layout emitted bridge commands or changed project state");
  f.Section("Camera");f.Step();f.Step();f.Find("##fov");
  for(const auto& item:f.submitted.items)Require(item.second.label!="##shot_duration","Consumed diagnostic token kept reopening Shot");
}

void SavedProjectBrowser(float height) {
  Fixture f(height);constexpr const char* transport="##CS2CinematicTransport";
  f.state["project_storage"]={{"id","project-a"},{"path","/projects/project-a.json"}};
  f.state["project_library"]={{"directory","/projects"},{"revision",1},{"entries",Json::array({
      {{"id","project-a"},{"title","Editable shot A"},{"compatible",true},{"reason",""},{"key_count",3},{"duration",5.25},{"start_tick",12345}},
      {{"id","project-b"},{"title","Other demo shot B"},{"compatible",false},{"reason","This project belongs to another recording. Open its demo first."},{"key_count",4},{"duration",8.0},{"start_tick",64000}}})}};
  f.Step();const auto project=f.state["project"];
  f.commands.clear();f.Click("Project",0,transport);f.Step();f.Choose("Load project");f.Step();
  Require(f.commands.size()==1 && f.commands[0].method=="refresh_project_library","Load menu bypassed the project browser or did not request an explicit scan");
  f.Choose("Other demo shot B");f.Step();f.commands.clear();
  Require(f.DrawText().find("another recording")!=std::string::npos,"Wrong-demo project did not explain why it cannot open");
  f.Choose("Open selected project");Require(f.commands.empty(),"Browser allowed a mismatched recording to load");
  f.Choose("Editable shot A");f.Step();f.Choose("Open selected project");
  Require(f.commands.size()==1 && f.commands[0].method=="load_saved_project" && f.commands[0].arguments==Json{{"id","project-a"}},"Browser did not load the exact cached project id");
  Require(f.state["project"]==project,"Browsing projects rewrote current camera metadata");
  f.Step();f.commands.clear();
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);f.Key(ImGuiKey_O);ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);f.Step();
  Require(f.commands.size()==1 && f.commands[0].method=="refresh_project_library","Ctrl+O loaded the fixed quick-save instead of opening the browser");
  f.Choose("Cancel##project_browser");f.Step();f.commands.clear();
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);f.Key(ImGuiKey_S);ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);f.Step();
  Require(f.commands.size()==1 && f.commands[0].method=="save_project","Ctrl+S no longer saved the current durable project");
  f.commands.clear();ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift,true);
  f.Key(ImGuiKey_S);ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift,false);ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);f.Step();
  Require(f.commands.size()==1 && f.commands[0].method=="save_project_copy","Ctrl+Shift+S did not create a separate saved project");
}

void SavedShotSelection() {
  setenv("CS2_CAMERA_LIVE","1",1);
  Fixture f;
  auto a=cs2_live::ToJson(cs2_live::ReadProject(f.state["project"]));
  a["name"]="R2 headshots";a["start_tick"]=20000;
  auto b=a;b["name"]="R5 multikill";b["start_tick"]=50000;
  cs2_live::Controller controller;
  float origin[3]={0,0,0},angles[3]={0,0,0},fov=90;
  controller.View(origin,angles,&fov,true);
  controller.Frame(true,640,true,64,true,"",[](const std::string&){});
  std::deque<Command> queued;
  f.command_sink=[&](const std::string& method,const Json& args){queued.push_back({method,args});};
  auto nativeCommand=[&](const std::string& method,const Json& arguments) {
    const auto text=Json{{"method",method},{"params",arguments}}.dump();
    controller.EnqueueText(text.c_str());
    controller.Frame(true,640,true,64,true,"",[](const std::string&){});
    f.state=Json::parse(controller.StateJson());
  };
  auto serviceOne=[&] {
    Require(!queued.empty(),"No UI command awaiting native service");
    const auto command=queued.front();queued.pop_front();
    nativeCommand(command.method,command.arguments);
  };
  auto serviceAll=[&] {while(!queued.empty())serviceOne();};
  auto settle=[&] {for(int i=0;i<4;++i)f.Step();};
  f.state=Json::parse(controller.StateJson());
  f.editor.SetSavedShots({{"shots",Json::array({{{"id","a"},{"label","R2 headshots"},{"project",a}},
      {{"id","b"},{"label","R5 multikill"},{"project",b}}})}});
  settle();f.editor.OpenShotSettingsForDiagnostics();settle();
  f.commands.clear();
  f.Click("Load##saved_shot");
  Require(f.commands.size()==3,"Saved shot must stop preview, pause, then import (got "+std::to_string(f.commands.size())+")");
  Require(f.commands[0].method=="preview" && f.commands[0].arguments["playing"]==false,"Shot did not stop preview");
  Require(f.commands[1].method=="demo_control" && f.commands[1].arguments["action"]=="pause","Shot did not pause the demo");
  Require(f.commands[2].method=="import_project" && f.commands[2].arguments["project"]==a,"Saved shot did not import editable keys");
  f.commands.clear();f.Step();Require(f.commands.empty(),"Shot moved the demo before confirmed import");
  serviceAll();f.Step();
  Require(f.commands.size()==2 && f.commands[0].method=="demo_control" && f.commands[0].arguments["tick"]==20000,"Confirmed shot did not seek its absolute start tick");
  Require(f.commands[1].method=="scrub" && f.commands[1].arguments["time"]==0,"Confirmed shot did not show its first camera pose");
  serviceAll();settle();
  const auto documentA=f.state["project_document_id"];
  nativeCommand("update_key",{{"id",a["keys"][0]["id"]},{"fov",75}});settle();
  Require(f.state["project_document_id"]==documentA,"Ordinary key edit changed shot document identity");
  const Json edited=f.state["project"];

  // Re-loading an equal active draft must not accept the old snapshot or the
  // revision bump caused by preview(false) as a successful import receipt.
  f.commands.clear();f.Click("Load##saved_shot");
  const auto reloadToken=f.commands.back().arguments["import_token"];
  f.commands.clear();f.Step();Require(f.commands.empty(),"Equal old project falsely acknowledged repeated shot import");
  serviceOne();f.Step();Require(f.commands.empty(),"Preview revision falsely acknowledged repeated shot import");
  serviceOne();f.Step();Require(f.commands.empty(),"Pause falsely acknowledged repeated shot import");
  Require(queued.size()==1 && queued.front().method=="import_project","Unexpected UI import queue");
  // A native rejection leaves the same equal project active. Its token still
  // has to report failure, without emitting a seek/scrub command.
  queued.front().arguments["project"]["duration"]=0;
  serviceOne();f.Step();
  Require(f.commands.empty() && !f.editor.saved_shot_error.empty(),"Rejected equal import moved the demo");
  Require(f.state["project_import_receipt"]["token"]==reloadToken && f.state["project_import_receipt"]["applied"]==false,"Native failure receipt lost the exact UI token");
  settle();

  f.commands.clear();f.Click("##saved_shots");f.Step();f.Choose("R5 multikill");f.Step();
  f.Click("Load##saved_shot");Require(f.commands.size()==3 && f.commands.back().method=="import_project" && f.commands.back().arguments["project"]==b,"Second saved shot was unavailable");
  serviceAll();f.Step();serviceAll();settle();
  const auto documentB=f.state["project_document_id"];
  Require(documentB!=documentA,"Different imported shots share a document identity");
  nativeCommand("undo",Json::object());settle();
  Require(f.state["project_document_id"]==documentA && f.state["project"]==edited,"Undo did not restore A and its shot association");
  f.commands.clear();
  f.Click("##saved_shots");f.Step();f.Choose("R2 headshots");f.Step();f.Click("Load##saved_shot");
  Require(f.commands.size()==3 && f.commands.back().method=="import_project" && f.commands.back().arguments["project"]==edited,"Switching saved shots lost current-session camera edits");
  serviceAll();f.Step();serviceAll();settle();
  const auto reloadedA=f.state["project_document_id"];
  f.commands.clear();f.Click("##saved_shots");f.Step();f.Choose("R5 multikill");f.Step();f.Click("Load##saved_shot");
  Require(f.commands.back().arguments["project"]==b,"A→B→Undo A→Load A corrupted the saved B draft");
  serviceAll();f.Step();serviceAll();settle();
  const auto reloadedB=f.state["project_document_id"];
  nativeCommand("undo",Json::object());settle();Require(f.state["project_document_id"]==reloadedA,"Undo lost imported document association");
  nativeCommand("redo",Json::object());settle();Require(f.state["project_document_id"]==reloadedB,"Redo lost imported document association");
  auto external=b;external["name"]="Unrelated externally loaded project";
  nativeCommand("import_project",{{"project",external}});settle();
  f.commands.clear();f.Click("Load##saved_shot");
  Require(f.commands.back().arguments["project"]==b,"External project import overwrote the associated saved B draft");
}

void SavedShotLibraryLoading() {
  const auto root=std::filesystem::temp_directory_path()/("cs2-shot-library-"+std::to_string(getpid()));
  std::filesystem::create_directory(root);
  const auto demo=root/"match.dem",other=root/"other.dem",path=root/"shots.json",link=root/"input.dem";
  {std::ofstream output(demo);output<<"PBDEMS2";}
  {std::ofstream output(other);output<<"PBDEMS2";}
  std::filesystem::create_symlink(demo,link);
  cs2_live::Project project;cs2_live::Key key;key.id="a";project.keys.push_back(key);
  Json value={{"version",1},{"demo",demo.string()},{"shots",Json::array({{{"id","a"},{"label","First clip"},{"project",cs2_live::ToJson(project)}}})}};
  auto write=[&] {std::ofstream output(path);output<<value.dump();};
  write();
  auto loaded=cs2_live::ReadShotLibrary(path.c_str(),link.c_str());
  Require(loaded["shots"][0]["project"]==cs2_live::ToJson(project),"Native library lost portable project fields");
  auto rejected=[&] {
    bool failed=false;
    try{cs2_live::ReadShotLibrary(path.c_str(),link.c_str());}catch(const std::exception&){failed=true;}
    Require(failed,"Malformed saved library reached the editor");
  };
  value["demo"]=other.string();write();rejected();
  value["demo"]=demo.string();value["shots"][0]["project"]["keys"][0]["fov"]=180;write();rejected();
  value["shots"][0]["project"]=cs2_live::ToJson(project);value["shots"].push_back(value["shots"][0]);write();rejected();
  value["shots"].erase(1);write();
  const auto alias=root/"library-link.json";std::filesystem::create_symlink(path,alias);
  bool refusedLink=false;
  try{cs2_live::ReadShotLibrary(alias.c_str(),link.c_str());}catch(const std::exception&){refusedLink=true;}
  Require(refusedLink,"Staged library symlink bypassed no-follow file access");
  std::filesystem::remove_all(root);
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
    std::cout << "Camera map XY gestures at 1080p / 720p\n" << std::flush;
    CameraMapGestures(1080);CameraMapGestures(720);
    std::cout << "Bezier XY / value handles, nullable auto, typed curves and independent timing\n" << std::flush;
    MapTangents(1080);MapTangents(720);ValueCurves(1080);ValueCurves(720);IndependentTiming();CurveDiagnosticLayout();TightCurveTimes(1080);TightCurveTimes(720);GuardedCurveProjectWrites();
    std::cout << "Editor settings and compact toolbar at 1080p / 720p\n" << std::flush;
    WorkspaceNavigation(1080);WorkspaceNavigation(720);WorkspaceNavigation(1440);
    SafeExitUi(1080,1);SafeExitUi(720,1.5);SafeExitUi(1440,1.5);SafeExitCommitsActiveInput();
    ContextualCapture(1080);ContextualCapture(720);
    PlaybackTimeControls(1080,1);PlaybackTimeControls(720,1);PlaybackTimeControls(720,1.5);PlaybackTimeControls(1440,1.5);
    EditorPreferencesUi(1080);EditorPreferencesUi(720);CompactScaleAndTimeline(1080);CompactScaleAndTimeline(720);
    FloatingMenuBackdrops(1080);FloatingMenuBackdrops(720);
    InspectorDurationValidation();TimeUnitsDuringNumericEdit();NumericEditAcrossDocumentImport();
    std::cout << "Live camera\n" << std::flush;
    LiveCamera();
    std::cout << "Render region at 1080p / 720p\n" << std::flush;
    RenderRegion(1080);
    RenderRegion(720);
    TimelineScrubAndRangeHandles(1080);TimelineScrubAndRangeHandles(720);
    PendingFullDemoSeek(1080);PendingFullDemoSeek(720);
    std::cout << "Exact/fine render region bounds\n" << std::flush;
    ExactRangeBounds();
    std::cout << "Scene effect capabilities\n" << std::flush;
    SceneControls();
    std::cout << "Combat switches at 1080p / 720p and capability-aware clean frame\n" << std::flush;
    SceneCombatControls(1080);SceneCombatControls(720);
    SceneCleanFrameXray(true);SceneCleanFrameXray(false);
    SceneBloomAutomatic();
    std::cout << "Export / hide controls\n" << std::flush;
    ExportAndHide();
    std::cout<<"Full demo timeline, events and independent navigation at 1080p / 720p\n"<<std::flush;
    FullDemoTimeline(1080);FullDemoTimeline(720);
    std::cout<<"Delayed scene numeric readback and native rejection\n"<<std::flush;
    SceneDraftAcknowledgement(true);SceneDraftAcknowledgement(false);
    std::cout<<"Typed scene toggles, one lighting selector and cohesive DOF activation\n"<<std::flush;
    SceneBooleanAcknowledgement();SceneShadingSelector();SceneDofActivation(true);SceneDofActivation(false);
    std::cout << "Player/team cosmetics gating\n" << std::flush;
    PlayerCosmetics();
    std::cout << "Presentation and separate camera-key controls\n" << std::flush;
    PresentationSelectors();
    std::cout << "Observed player selector and SteamID targeting\n" << std::flush;
    ObservedPlayerSelector();
    std::cout << "Recorded marker player selector without entity adapters at 1080p / 720p\n" << std::flush;
    TimelinePlayerSelectorWithoutEntities(1080);TimelinePlayerSelectorWithoutEntities(720);
    std::cout << "Independent shortcuts panel\n" << std::flush;
    IndependentShortcutsPanel();
    std::cout << "Opt-in diagnostic Shot layout has no bridge effects\n" << std::flush;
    DiagnosticShotLayout();
    std::cout << "Saved shot selection, confirmed import and session drafts\n" << std::flush;
    SavedProjectBrowser(1080);SavedProjectBrowser(720);
    SavedShotSelection();
    SavedShotLibraryLoading();
    std::cout << "PASS: numeric replacement, Enter/click-away, middle steps and bounds, "
                 "single history commits, Escape/no-op, shortcut guards, and independent F10 panel\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
