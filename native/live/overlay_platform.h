#pragma once
#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

namespace cs2_camera_platform {
struct Point {float x=0,y=0;};
inline std::optional<Point> Pointer(std::uint32_t target,std::uint32_t source,int logicalWidth,int logicalHeight,
                                    unsigned framebufferWidth,unsigned framebufferHeight,float x,float y) {
    if(!target || target!=source || logicalWidth<=0 || logicalHeight<=0 || !framebufferWidth || !framebufferHeight ||
       !std::isfinite(x) || !std::isfinite(y))return std::nullopt;
    const Point result{x*float(framebufferWidth)/float(logicalWidth),y*float(framebufferHeight)/float(logicalHeight)};
    if(!std::isfinite(result.x) || !std::isfinite(result.y))return std::nullopt;
    return result; // Preserve off-window coordinates while dragging.
}
inline bool OwnEvent(std::uint32_t target,std::uint32_t source){return target && source==target;}
template<class Position,class Button> bool PointerButton(const std::optional<Point>& position,bool down,Position putPosition,Button putButton) {
    if(position)putPosition(*position);
    if(!position && down)return false;
    putButton(down);return true; // A release must never leave a button stuck.
}
// SDL updates its internal array even when key events are disabled or skip
// the event filter. Keep the game-facing cached pointer current after pumping.
template<std::size_t N> void KeyboardSnapshot(bool (&physical)[N],bool (&forwarded)[N],const bool* native,int count,bool capture,bool focused) {
    if(native && count>0){std::fill(std::begin(physical),std::end(physical),false);std::copy_n(native,std::min<std::size_t>(N,std::size_t(count)),physical);}
    for(std::size_t i=0;i<N;++i)forwarded[i]=!capture && focused && physical[i];
}
class EventDispatch {
public:
    explicit EventDispatch(void(*service)()):service_(service),outer_(depth_++==0){if(outer_ && service_)service_();}
    ~EventDispatch(){if(outer_ && service_)service_();--depth_;}
    EventDispatch(const EventDispatch&)=delete;EventDispatch& operator=(const EventDispatch&)=delete;
private:
    inline static thread_local unsigned depth_=0;void(*service_)();bool outer_;
};
class CursorCapture {
public:
    struct Functions {
        std::function<bool()> relative,grab,visible,text;
        std::function<void(bool)> setRelative,setGrab,setVisible,setText;
    };
    void Update(bool mainThread,std::uintptr_t window,bool capture,int relativeRequest,int grabRequest,int visibleRequest,const Functions& f) {
        if(!mainThread)return;
        if(window_!=window){held_=false;window_=window;}
        if(!window)return;
        if(capture && !held_){
            relative_=f.relative && f.relative();grab_=f.grab && f.grab();visible_=!f.visible || f.visible();text_=f.text && f.text();
            if(f.setRelative)f.setRelative(false);
            if(f.setGrab)f.setGrab(false);
            if(f.setVisible)f.setVisible(true);
            if(f.setText)f.setText(true);
            held_=true;
        }else if(!capture && held_){
            if(f.setRelative)f.setRelative(relativeRequest<0?relative_:relativeRequest!=0);
            if(f.setGrab)f.setGrab(grabRequest<0?grab_:grabRequest!=0);
            if(f.setVisible)f.setVisible(visibleRequest<0?visible_:visibleRequest!=0);
            if(!text_ && f.setText)f.setText(false);
            held_=false;
        }
    }
private:
    std::uintptr_t window_=0;bool held_=false,relative_=false,grab_=false,visible_=true,text_=false;
};

// The callbacks are supplied only by the main-thread SDL event service. This
// policy neither selects displays nor requests focus or a display-mode change.
class DesktopFullscreen {
public:
    using Clock=std::chrono::steady_clock;using Window=std::uintptr_t;
    enum class Status {Disabled,Waiting,Pending,Confirmed,Failed};
    void Requested(bool value){requested_=value;}
    void Update(bool mainThread,Window window,std::uint32_t id,bool vulkan,bool actual,Clock::time_point now,
                const std::function<bool()>& desktopMode,const std::function<bool()>& fullscreen) {
        if(!mainThread)return;
        if(!requested_){status_=Status::Disabled;return;}
        if(!window || !id || !vulkan){status_=Status::Waiting;return;}
        if(window!=window_ || id!=id_){window_=window;id_=id;attempts_=0;deadline_=now+std::chrono::seconds(5);next_=now;status_=Status::Pending;}
        if(actual){status_=Status::Confirmed;return;}
        if(now>=deadline_){status_=Status::Failed;return;}
        if(status_==Status::Failed || attempts_>=3 || now<next_)return;
        ++attempts_;next_=now+std::chrono::seconds(1);status_=Status::Pending;
        if(desktopMode && fullscreen && desktopMode())fullscreen();
    }
    Status State()const{return status_;}unsigned Attempts()const{return attempts_;}
    const char* Name()const{switch(status_){case Status::Disabled:return "windowed";case Status::Waiting:return "waiting_window";case Status::Pending:return "pending";case Status::Confirmed:return "confirmed";case Status::Failed:return "failed";}return "unknown";}
private:
    bool requested_=false;Window window_=0;std::uint32_t id_=0;unsigned attempts_=0;
    Status status_=Status::Waiting;Clock::time_point deadline_{},next_{};
};
} // namespace cs2_camera_platform
