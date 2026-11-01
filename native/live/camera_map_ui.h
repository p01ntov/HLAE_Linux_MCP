#pragma once
#include "live_controller.h"
#include "camera_curve_ui.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <string>

namespace cs2_live_ui {
// A world XY drafting view. The displayed Z slice is context, never a collision
// oracle. The same native sampler as preview draws the camera's actual curve.
class CameraMapUi {
 public:
  using Json=nlohmann::json;
  using Send=std::function<void(const std::string&,const Json&)>;
  bool visible=false;
  void SetMapPreview(const Json& preview){preview_=preview;}
  void ResetGesture(){Cancel();}
  void Draw(const Json& state,const Send& send,const std::string& selected,
            const std::function<void(const std::string&)>& select,float scale,const std::function<void()>& backdrop={},bool embedded=false) {
    if(!visible){ResetGesture();return;}
    if(!embedded){
      const auto display=ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(std::min(680.f*scale,display.x*.66f),std::min(590.f*scale,display.y*.8f)),ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowPos(ImVec2(18*scale,36*scale),ImGuiCond_FirstUseEver);
      if(!ImGui::Begin("Camera map / XY",&visible,ImGuiWindowFlags_NoCollapse|(backdrop?ImGuiWindowFlags_NoBackground:0))){ImGui::End();return;}
      if(backdrop)backdrop();
    }
    const auto prefs=state.value("editor_preferences",Json::object());
    const bool tips=prefs.is_object()?prefs.value("show_tooltips",true):true,labels=prefs.is_object()?prefs.value("show_key_labels",true):true;
    auto hint=[&](const char* text){if(tips)camera_curve::Hint(text,backdrop);};
    Json raw=state.value("project",Json::object());cs2_live::Project project;
    try{project=cs2_live::ReadProject(state.at("project"));}
    catch(const std::exception&){ImGui::TextUnformatted("No valid camera project");if(!embedded)ImGui::End();return;}
    const auto document=state.value("project_document_id",std::uint64_t(0));
    if(last_document_!=document){Cancel();fitted_=false;last_document_=document;}
    if(tangent_ && raw!=tangent_source_)Cancel();
    if(!drag_id_.empty() && drag_id_!="@camera" && std::none_of(project.keys.begin(),project.keys.end(),[&](const auto& k){return k.id==drag_id_;}))Cancel();
    if(!drag_id_.empty() && drag_id_!="@camera")for(const auto& key:project.keys)
      if(key.id==drag_id_ && key.pose.position!=drag_original_)Cancel();
    const auto& preview=state.contains("map_preview") && state.at("map_preview").is_object()?state.at("map_preview"):preview_;
    const auto& layers=preview.contains("layers")?preview.at("layers"):Empty();
    double height=0;bool selected_found=false;
    for(const auto& key:project.keys)if(key.id==selected){height=key.pose.position[2];selected_found=true;break;}
    if(!selected_found && state.contains("camera"))height=state.at("camera").at("position").at(2).get<double>();
    ImGui::TextUnformatted("XY only - key height is preserved");
    const bool fit=ImGui::Button("Fit path");ImGui::SameLine();const bool fit_map=ImGui::Button("Fit map");
    ImGui::SameLine();const bool center_camera=ImGui::Button("Center camera");
    ImGui::SameLine();ImGui::Text("Z %.2f",height);
    const bool paused=state.value("paused",false) && !state.value("preview_playing",false);
    ImGui::BeginDisabled(!paused);
    if(ImGui::Button("Check 3D path") && send)send("request_geometry_validation",Json::object());
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto& validation=state.contains("geometry_validation") && state["geometry_validation"].is_object()?state["geometry_validation"]:Empty();
    const bool checked=state.value("geometry_validation_current",false);
    const auto check_status=validation.value("status",std::string("unchecked"));
    if(!paused)ImGui::TextDisabled("Pause demo for a path check");
    else if(!checked)ImGui::TextDisabled("Route not checked after latest edit");
    else if(check_status=="blocked")ImGui::TextColored(ImVec4(1,.4f,.3f,1),"Blocked by indexed geometry");
    else if(validation.value("passed",false))ImGui::TextUnformatted("3D check passed for indexed geometry");
    else ImGui::TextDisabled("%s",check_status=="pending"?"Checking...":"Partial geometry - route unverified");
    const bool map_available=layers.is_array() && !layers.empty();
    if(map_available){
      int nearest=0;double distance=std::numeric_limits<double>::max();
      for(std::size_t i=0;i<layers.size();++i){const double d=std::abs(layers[i].value("z",0.0)-height);if(d<distance){distance=d;nearest=int(i);}}
      if(auto_layer_)layer_=nearest;
      layer_=std::clamp(layer_,0,int(layers.size())-1);
      ImGui::Checkbox("Follow selected Z slice",&auto_layer_);
      ImGui::SameLine();char label[64];std::snprintf(label,sizeof(label),"Slice Z %.1f",layers[layer_].value("z",0.0));
      ImGui::SetNextItemWidth(160*scale);
      if(ImGui::BeginCombo("##map_layer",label)){
        if(backdrop)backdrop();
        for(std::size_t i=0;i<layers.size();++i){std::snprintf(label,sizeof(label),"Z %.1f",layers[i].value("z",0.0));if(ImGui::Selectable(label,int(i)==layer_)){layer_=int(i);auto_layer_=false;}}
        ImGui::EndCombo();
      }
    }else ImGui::TextDisabled("No verified map slice - showing camera XY plan");
    ImGui::TextWrapped("%s",map_available?
      "Map obstacles are partial. Doors, glass and moving objects require visual review.":
      "Map geometry is unavailable. Showing the camera path only.");
    if(tips && ImGui::IsItemHovered())camera_curve::Tooltip(preview.value("reason",std::string("No map geometry loaded.")).c_str(),backdrop);
    const auto selected_index=camera_curve::Index(project,selected);
    if(mode_target_!=selected){mode_target_=selected;mode_=selected_index<project.keys.size()?camera_curve::TangentMode(raw,project,selected_index,0):0;}
    ImGui::BeginDisabled(selected_index==project.keys.size());
    if(ImGui::Button("Auto handles")){auto patch=camera_curve::Automatic(raw,selected,0);if(camera_curve::Changed(raw,patch)&&send)send("set_project",patch);mode_=0;}
    ImGui::SameLine();ImGui::BeginDisabled(selected_index==0||selected_index+1>=project.keys.size());
    if(ImGui::RadioButton("Aligned",&mode_,1)){auto patch=camera_curve::EditHandle(raw,selected,0,false,camera_curve::Offset(project,selected_index,0,false),1,0,2);if(camera_curve::Changed(raw,patch)&&send)send("set_project",patch);}
    ImGui::EndDisabled();ImGui::SameLine();ImGui::RadioButton("Free",&mode_,2);
    hint("Drag shape handles in XY; their Z offsets stay fixed. Aligned links their XY direction using adjacent key durations. Timing can still change speed. Ctrl-drag edits one side.");
    ImGui::SameLine();if(ImGui::Button("Smooth through key")){auto patch=camera_curve::SmoothThrough(raw,selected);if(camera_curve::Changed(raw,patch)&&send)send("set_project",patch);mode_=0;}
    hint("Auto handles for position, rotation and FOV, with linear timing on both sides. Enables Bezier on all three tracks, including other segments. One undo restores the previous curves.");
    ImGui::EndDisabled();
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(160.f,ImGui::GetContentRegionAvail().x),std::max(120.f,ImGui::GetContentRegionAvail().y-58*scale));
    if(!fitted_ || fit || fit_map){
      double xmin=0,ymin=0,xmax=0,ymax=0;bool first=true;
      auto bound=[&](double x,double y){if(first){xmin=xmax=x;ymin=ymax=y;first=false;}else{xmin=std::min(xmin,x);xmax=std::max(xmax,x);ymin=std::min(ymin,y);ymax=std::max(ymax,y);}};
      if(fit_map && preview.contains("bounds") && preview["bounds"].is_array() && preview["bounds"].size()==4){bound(preview["bounds"][0],preview["bounds"][1]);bound(preview["bounds"][2],preview["bounds"][3]);}
      else if(!project.keys.empty()){
        for(int i=0;i<=256;++i){const auto p=cs2_live::Sample(project,project.duration*i/256.0).position;bound(p[0],p[1]);}
        if(selected_index<project.keys.size()){const auto base=project.keys[selected_index].pose.position;for(bool incoming:{true,false}){
          if((incoming&&selected_index==0)||(!incoming&&selected_index+1>=project.keys.size()))continue;
          const auto offset=camera_curve::Offset(project,selected_index,0,incoming);bound(base[0]+offset[0],base[1]+offset[1]);
        }}
      }
      else if(state.contains("camera")){const auto p=state.at("camera").at("position");bound(p[0],p[1]);}
      center_x_=(xmin+xmax)/2;center_y_=(ymin+ymax)/2;
      zoom_=std::clamp(std::min((size.x-50*scale)/std::max(160.,xmax-xmin),(size.y-50*scale)/std::max(160.,ymax-ymin)),.001,40.);fitted_=true;
    }
    if(center_camera && state.contains("camera")){center_x_=state.at("camera").at("position").at(0);center_y_=state.at("camera").at("position").at(1);}
    ImGui::InvisibleButton("##camera_map_canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
    const bool hover=ImGui::IsItemHovered();const auto mouse=ImGui::GetIO().MousePos;
    auto point=[&](double x,double y){return ImVec2(float(origin.x+size.x/2+(x-center_x_)*zoom_),float(origin.y+size.y/2-(y-center_y_)*zoom_));};
    auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(20,23,24,220));
    draw->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
    if(hover && drag_id_.empty() && ImGui::GetIO().MouseWheel!=0){
      const double wx=center_x_+(mouse.x-origin.x-size.x/2)/zoom_,wy=center_y_-(mouse.y-origin.y-size.y/2)/zoom_;
      zoom_=std::clamp(zoom_*std::pow(1.22,ImGui::GetIO().MouseWheel),.001,40.);
      center_x_=wx-(mouse.x-origin.x-size.x/2)/zoom_;center_y_=wy+(mouse.y-origin.y-size.y/2)/zoom_;
    }
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle) && drag_id_.empty()){
      center_x_-=ImGui::GetIO().MouseDelta.x/zoom_;center_y_+=ImGui::GetIO().MouseDelta.y/zoom_;
    }
    const double grid=std::pow(10.,std::floor(std::log10(65*scale/zoom_)));
    const double left=center_x_-size.x/(2*zoom_),right=center_x_+size.x/(2*zoom_);
    const double bottom=center_y_-size.y/(2*zoom_),top=center_y_+size.y/(2*zoom_);
    for(double x=std::ceil(left/grid)*grid;x<=right && (x-left)/grid<256;x+=grid){const auto a=point(x,bottom),b=point(x,top);draw->AddLine(a,b,IM_COL32(125,135,140,x==0?110:35));}
    for(double y=std::ceil(bottom/grid)*grid;y<=top && (y-bottom)/grid<256;y+=grid){draw->AddLine(point(left,y),point(right,y),IM_COL32(125,135,140,y==0?110:35));}
    if(map_available){
      const auto& lines=layers[layer_].at("lines");std::size_t count=0;
      for(const auto& line:lines){if(++count>1000)break;if(line.is_array() && line.size()==4)draw->AddLine(point(line[0],line[1]),point(line[2],line[3]),IM_COL32(180,187,187,130),scale);}
    }
    if(tangent_&&!tangent_patch_.empty()){Json preview_project=raw;preview_project.update(tangent_patch_);project=cs2_live::ReadProject(preview_project);}
    if(!tangent_&&!drag_id_.empty() && drag_id_!="@camera")for(auto& key:project.keys)if(key.id==drag_id_){key.pose.position[0]=drag_x_;key.pose.position[1]=drag_y_;}
    if(!project.keys.empty()){
      ImVec2 previous{};const int samples=std::min(2048,std::max(64,int(project.keys.size())*32));
      for(int i=0;i<=samples;++i){const auto p=cs2_live::Sample(project,project.duration*i/samples).position;const auto next=point(p[0],p[1]);if(i)draw->AddLine(previous,next,IM_COL32(220,207,151,230),2*scale);previous=next;}
    }
    if(checked && validation.contains("collision") && validation["collision"].is_object()){
      const auto& collision=validation["collision"];
      if(collision.contains("curve_segment") && collision["curve_segment"].is_array() && collision["curve_segment"].size()==2){
        const auto& a=collision["curve_segment"][0];const auto& b=collision["curve_segment"][1];
        if(a.is_array() && b.is_array() && a.size()==3 && b.size()==3)draw->AddLine(point(a[0],a[1]),point(b[0],b[1]),IM_COL32(255,80,55,255),5*scale);
      }
      if(collision.contains("position") && collision["position"].is_array() && collision["position"].size()==3){
        const auto& p=collision["position"];draw->AddCircle(point(p[0],p[1]),12*scale,IM_COL32(255,80,55,255),24,3*scale);
      }
    }
    std::string nearest;double best=12*scale;cs2_live::Pose nearest_pose;int nearest_tangent=0;
    if(selected_index<project.keys.size()){
      const auto& key=project.keys[selected_index];const auto base=key.pose.position;
      for(bool incoming:{true,false}){if((incoming&&selected_index==0)||(!incoming&&selected_index+1==project.keys.size()))continue;
        const auto offset=camera_curve::Offset(project,selected_index,0,incoming);const auto p=point(base[0]+offset[0],base[1]+offset[1]);
        draw->AddLine(point(base[0],base[1]),p,IM_COL32(215,215,215,170),scale);draw->AddCircleFilled(p,5*scale,IM_COL32(45,45,45,255));draw->AddCircle(p,5*scale,IM_COL32(245,245,245,255),16,1.5f*scale);
        camera_curve::TestHandle(std::string("##map_tangent_")+(incoming?"in_":"out_")+selected,p,8*scale);
        const double distance=std::hypot(mouse.x-p.x,mouse.y-p.y);if(distance<best){best=distance;nearest=selected;nearest_pose=key.pose;nearest_tangent=incoming?1:2;}
      }
    }
    auto handle=[&](const std::string& id,const cs2_live::Pose& pose,bool live,int number){
      const auto p=point(pose.position[0],pose.position[1]);const bool chosen=id==selected || id==drag_id_;
      const auto color=live?IM_COL32(110,205,220,255):chosen?IM_COL32(255,228,133,255):IM_COL32(225,225,215,230);
      draw->AddCircleFilled(p,(chosen?6.f:4.f)*scale,color);
#ifdef IMGUI_ENABLE_TEST_ENGINE
      // Expose the actual drawn hit region to the same optional item hooks as
      // ImGui controls; tests never guess the map's pan/zoom or pixel layout.
      const std::string label="##map_handle_"+id;
      auto& g=*ImGui::GetCurrentContext();
      const auto handle_id=ImGui::GetID(label.c_str());
      IMGUI_TEST_ENGINE_ITEM_ADD(handle_id,ImRect(ImVec2(p.x-8*scale,p.y-8*scale),ImVec2(p.x+8*scale,p.y+8*scale)),nullptr);
      IMGUI_TEST_ENGINE_ITEM_INFO(handle_id,label.c_str(),0);
#endif
      const double distance=std::hypot(mouse.x-p.x,mouse.y-p.y);
      if(distance<best || (nearest_tangent&&distance==best)){best=distance;nearest=id;nearest_pose=pose;nearest_tangent=0;}
      constexpr double radians=3.141592653589793/180.;const double yaw=pose.angles[1]*radians;
      draw->AddLine(p,ImVec2(p.x+float(std::cos(yaw))*16*scale,p.y-float(std::sin(yaw))*16*scale),color,scale);
      if(!live && labels){char label[32];std::snprintf(label,sizeof(label),"%02d",number);draw->AddText(ImVec2(p.x+7*scale,p.y-13*scale),color,label);}
    };
    // Keys win a tie with the current view, which often coincides with a key.
    for(std::size_t i=0;i<project.keys.size();++i)handle(project.keys[i].id,project.keys[i].pose,false,int(i)+1);
    if(state.contains("camera")){
      auto camera=cs2_live::ReadPose(state.at("camera"));
      if(drag_id_=="@camera"){camera.position[0]=drag_x_;camera.position[1]=drag_y_;}
      handle("@camera",camera,true,0);
    }
    if(hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !nearest.empty()){
      drag_id_=nearest;drag_original_=nearest_pose.position;drag_x_=drag_original_[0];drag_y_=drag_original_[1];drag_mouse_=mouse;moved_=false;
      tangent_=nearest_tangent;tangent_source_=raw;tangent_patch_=Json::object();tangent_mode_=ImGui::GetIO().KeyCtrl?2:mode_;
      if(tangent_)tangent_offset_=camera_curve::Offset(project,camera_curve::Index(project,nearest),0,tangent_==1);
      if(select)select(nearest);
    }
    if(!drag_id_.empty()){
      if(ImGui::IsKeyPressed(ImGuiKey_Escape,false)){Cancel();ImGui::ClearActiveID();}
      else if(ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        const double dx=mouse.x-drag_mouse_.x,dy=mouse.y-drag_mouse_.y;
        moved_=std::hypot(dx,dy)>3*scale;
        if(tangent_){tangent_patch_=Json::object();if(moved_){auto offset=tangent_offset_;offset[0]=std::clamp(drag_original_[0]+offset[0]+dx/zoom_,-1000000.,1000000.)-drag_original_[0];offset[1]=std::clamp(drag_original_[1]+offset[1]-dy/zoom_,-1000000.,1000000.)-drag_original_[1];if(offset!=tangent_offset_)tangent_patch_=camera_curve::EditHandle(tangent_source_,drag_id_,0,tangent_==1,offset,tangent_mode_,0,2);}}
        else{drag_x_=std::clamp(drag_original_[0]+dx/zoom_,-1000000.,1000000.);drag_y_=std::clamp(drag_original_[1]-dy/zoom_,-1000000.,1000000.);}
      }else{
        if(tangent_){if(!tangent_patch_.empty()&&camera_curve::Changed(tangent_source_,tangent_patch_)&&send)send("set_project",tangent_patch_);}
        else if(moved_ && send && (drag_x_!=drag_original_[0]||drag_y_!=drag_original_[1])){Json payload={{"position",{drag_x_,drag_y_,drag_original_[2]}}};if(drag_id_!="@camera")payload["id"]=drag_id_;send(drag_id_=="@camera"?"set_camera":"update_key",payload);}
        Cancel();
      }
    }
    draw->PopClipRect();
    ImGui::TextDisabled("+X right / +Y up | Wheel zoom | MMB pan | Esc cancel");
    ImGui::TextWrapped("Drag keys or shape handles. Z stays fixed. Ctrl: free handle. Check the full 3D route separately.");
    if(!embedded)ImGui::End();
    if(!visible)Cancel();
  }
 private:
  static const Json& Empty(){static const Json empty=Json::object();return empty;}
  Json preview_=Json::object();
  void Cancel(){drag_id_.clear();moved_=false;tangent_=0;tangent_patch_=Json::object();}
  int tangent_=0,tangent_mode_=0,mode_=0;std::string mode_target_;Json tangent_source_,tangent_patch_;camera_curve::Vector tangent_offset_{};
  bool fitted_=false,auto_layer_=true,moved_=false;int layer_=0;
  double center_x_=0,center_y_=0,zoom_=1,drag_x_=0,drag_y_=0;
  std::uint64_t last_document_=std::numeric_limits<std::uint64_t>::max();
  std::string drag_id_;std::array<double,3> drag_original_{};ImVec2 drag_mouse_{};
};
} // namespace cs2_live_ui
