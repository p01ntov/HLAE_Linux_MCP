// CS2 Camera: a process-local Vulkan layer. It does not patch game render code.
// Loaded only by the live launcher, with the same lifetime as its CS2 process.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <link.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "editor_ui.h"
#include "cosmetics_catalog.h"
#include "shot_library.h"
#include "overlay_blur.h"
#include "overlay_capture.h"
#include "overlay_platform.h"

namespace camera_overlay {
using Lookup=void*(*)(void*,const char*);
Lookup LibcResolver() {
    // A versioned dlsym import alone can still resolve to our unversioned
    // preloaded definition. dlvsym is not intercepted and finds libc directly.
    static auto original=reinterpret_cast<Lookup>(dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.2.5"));
    return original;
}
void* RealSymbol(void* library,const char* name) {
    const auto original=LibcResolver();
    return original?original(library,name):nullptr;
}
void Log(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    std::fprintf(stderr, "[CS2 Camera overlay] ");
    std::vfprintf(stderr, fmt, args); std::fprintf(stderr, "\n"); va_end(args);
}
template<class T> void* Key(T handle) { return handle ? *reinterpret_cast<void**>(handle) : nullptr; }
struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkDestroyInstance destroy{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties{};
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties{};
    PFN_vkGetPhysicalDeviceFormatProperties format_properties{};
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_capabilities{};
};
struct Frame {
    VkImage image{}; VkImageView view{}; VkFramebuffer framebuffer{}; VkCommandBuffer cmd{};
    VkFence fence{}; VkSemaphore complete{};
};
struct Device;
struct Swapchain {
    Device* device{}; VkSwapchainKHR handle{}; VkFormat format{}; VkExtent2D extent{};
    uint32_t min_images=2; VkRenderPass pass{}; VkCommandPool pool{};
    uint32_t family=~0u; VkQueue queue{}; std::vector<Frame> frames;
    bool ready=false, failed=false, blur_supported=false,frame_blur_enabled=false;
    cs2_camera_blur::Backdrop blur;
    cs2_overlay_capture::Probe capture;
    std::uint64_t capture_mode_revision=0;
    VkPresentModeKHR present_mode=VK_PRESENT_MODE_FIFO_KHR;
};
struct Device {
    VkDevice handle{}; VkPhysicalDevice physical{}; Instance* instance{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkSetDeviceLoaderData set_loader_data{};
    std::unordered_map<VkQueue,uint32_t> queues;
    std::unordered_map<VkSwapchainKHR,std::unique_ptr<Swapchain>> swaps;
#define DEVICE_FN(name) PFN_vk##name name{};
    DEVICE_FN(DestroyDevice) DEVICE_FN(GetDeviceQueue) DEVICE_FN(GetDeviceQueue2)
    DEVICE_FN(CreateSwapchainKHR) DEVICE_FN(DestroySwapchainKHR) DEVICE_FN(GetSwapchainImagesKHR)
    DEVICE_FN(QueuePresentKHR) DEVICE_FN(QueueSubmit) DEVICE_FN(QueueWaitIdle) DEVICE_FN(DeviceWaitIdle)
    DEVICE_FN(CreateRenderPass) DEVICE_FN(DestroyRenderPass) DEVICE_FN(CreateImageView) DEVICE_FN(DestroyImageView)
    DEVICE_FN(CreateFramebuffer) DEVICE_FN(DestroyFramebuffer) DEVICE_FN(CreateCommandPool) DEVICE_FN(DestroyCommandPool)
    DEVICE_FN(AllocateCommandBuffers) DEVICE_FN(FreeCommandBuffers) DEVICE_FN(ResetCommandBuffer) DEVICE_FN(BeginCommandBuffer) DEVICE_FN(EndCommandBuffer)
    DEVICE_FN(CmdBeginRenderPass) DEVICE_FN(CmdEndRenderPass) DEVICE_FN(CmdPipelineBarrier)
    DEVICE_FN(CreateFence) DEVICE_FN(DestroyFence) DEVICE_FN(WaitForFences) DEVICE_FN(ResetFences)
    DEVICE_FN(CreateSemaphore) DEVICE_FN(DestroySemaphore)
#undef DEVICE_FN
};
std::recursive_mutex mutex;
std::unordered_map<void*,std::unique_ptr<Instance>> instances;
std::unordered_map<void*,std::unique_ptr<Device>> devices;
Swapchain* active=nullptr;
std::atomic<bool> shown{true};
std::atomic<bool> wants_mouse{true}, wants_keyboard{true};
std::mutex events_mutex;
struct InputEvent { SDL_Event event; std::string text; };
std::deque<InputEvent> events;
std::atomic<SDL_Window*> game_window{nullptr};
std::atomic<SDL_WindowID> game_window_id{0},pending_window_id{0};
std::atomic<int> logical_width{0},logical_height{0};
std::atomic<bool> window_focused{false};
std::atomic<std::uint64_t> mouse_button_events{0},mouse_button_delivered{0},foreign_mouse_button_events{0};
std::atomic<int> relative_requested{-1},grab_requested{-1},cursor_visible_requested{-1};
std::atomic<bool> filter_installed{false}, overlay_available{false};
bool InputMode() { return shown.load() && filter_installed.load() && overlay_available.load() && game_window_id.load()!=0 && window_focused.load(); }
SDL_EventFilter previous_filter=nullptr;
void* previous_filter_data=nullptr;
std::mutex filter_mutex;
cs2_live_ui::EditorUi editor;
using StateFn=const char*(*)();
using CommandFn=void(*)(const char*);
StateFn state_fn=nullptr,map_preview_fn=nullptr; CommandFn command_fn=nullptr;
std::atomic<bool> bridge_ready{false};
template<class T> T Sdl(const char* name);
void CursorMode(bool capture);
void MainThreadService();
std::atomic<bool> text_input{false};
// SDL promises a lifetime-stable keyboard array. Source 2 caches this pointer;
// swapping arrays when opening the editor would leave roaming controls stuck.
std::mutex keyboard_mutex;
bool keyboard_state[SDL_SCANCODE_COUNT]={};
bool physical_keys[SDL_SCANCODE_COUNT]={};
void KeyboardMode(bool capture) {
    std::lock_guard<std::mutex> lock(keyboard_mutex);
    cs2_camera_platform::KeyboardSnapshot(physical_keys,keyboard_state,nullptr,0,capture,window_focused.load());
}
void RefreshKeyboard(bool capture) {
    // SDL_GetKeyboardState is documented thread-safe. MainThreadService calls
    // this again after Pump/Poll so a cached engine pointer sees native changes
    // even if no key event passed through our filter.
    static auto real=Sdl<const bool*(*)(int*)>("SDL_GetKeyboardState");
    int count=0;const bool* native=real?real(&count):nullptr;
    std::lock_guard<std::mutex> lock(keyboard_mutex);
    cs2_camera_platform::KeyboardSnapshot(physical_keys,keyboard_state,native,count,capture,window_focused.load());
}
struct Hotkey { std::string name; SDL_Scancode scan; SDL_Keymod modifiers; };
std::unordered_map<std::string,Hotkey> hotkeys={
    {"capture",{"F6",SDL_SCANCODE_F6,SDL_KMOD_NONE}},
    {"camera",{"F7",SDL_SCANCODE_F7,SDL_KMOD_NONE}},
    {"editor",{"F8",SDL_SCANCODE_F8,SDL_KMOD_NONE}},
    {"preview",{"F9",SDL_SCANCODE_F9,SDL_KMOD_NONE}}
};
std::string hotkeys_path;
void LoadSavedShots() {
    static bool attempted=false;
    if(attempted)return;
    attempted=true;
    const char* path=std::getenv("CS2_CAMERA_SHOT_LIBRARY");
    if(!path || !*path)return;
    try {editor.SetSavedShots(cs2_live::ReadShotLibrary(path,std::getenv("CS2_CAMERA_DEMO_PATH")));}
    catch(const std::exception& error) {
        editor.saved_shot_error=std::string("Saved shots unavailable: ")+error.what();
        Log("%s",editor.saved_shot_error.c_str());
    }
}
void LoadCosmeticsCatalog() {
    static bool attempted=false;
    if(attempted)return;
    attempted=true;
    if(const char* error=std::getenv("CS2_CAMERA_COSMETICS_CATALOG_ERROR"))
        editor.cosmetics_catalog_error=error;
    const char* path=std::getenv("CS2_CAMERA_COSMETICS_CATALOG");
    if(!path || !*path)return;
    try {
        auto catalog=cs2_live::CosmeticsCatalog::ReadJson(path);
        cs2_live::CosmeticsCatalog::Parse(catalog);
        editor.cosmetics_catalog=std::move(catalog);
        editor.cosmetics_catalog_error.clear();
    } catch(const std::exception& error) {
        editor.cosmetics_catalog_error=std::string("Game catalog unavailable: ")+error.what();
        Log("%s",editor.cosmetics_catalog_error.c_str());
    }
}
void LoadHotkeys() {
    const char* path=std::getenv("CS2_CAMERA_HOTKEYS");
    if(!path || !*path)return;
    hotkeys_path=path;
    std::ifstream file(path);
    if(!file)return;
    try {
        nlohmann::json data;file>>data;
        static auto scan_name=Sdl<SDL_Scancode(*)(const char*)>("SDL_GetScancodeFromName");
        if(!scan_name)return;
        for(auto& pair:hotkeys) {
            if(!data.contains(pair.first) || !data[pair.first].is_string())continue;
            const auto name=data[pair.first].get<std::string>();
            auto key=name;SDL_Keymod modifiers=SDL_KMOD_NONE;
            for(const auto& item:std::vector<std::pair<std::string,SDL_Keymod>>{{"Ctrl+",SDL_KMOD_CTRL},{"Shift+",SDL_KMOD_SHIFT},{"Alt+",SDL_KMOD_ALT}}) {
                auto at=key.find(item.first);
                if(at!=std::string::npos){key.erase(at,item.first.size());modifiers=SDL_Keymod(modifiers|item.second);}
            }
            auto scan=scan_name(key.c_str());
            if(scan!=SDL_SCANCODE_UNKNOWN)pair.second={name,scan,modifiers};
        }
    }catch(...){Log("Invalid hotkeys file; keeping previous bindings");}
}
bool Match(const SDL_KeyboardEvent& event,const Hotkey& key) {
    if(event.scancode!=key.scan)return false;
    return bool(event.mod&SDL_KMOD_CTRL)==bool(key.modifiers&SDL_KMOD_CTRL)
        && bool(event.mod&SDL_KMOD_SHIFT)==bool(key.modifiers&SDL_KMOD_SHIFT)
        && bool(event.mod&SDL_KMOD_ALT)==bool(key.modifiers&SDL_KMOD_ALT);
}
void Send(const char* method,const nlohmann::json& params=nlohmann::json::object()) {
    if(command_fn){const auto value=nlohmann::json{{"method",method},{"params",params}}.dump();command_fn(value.c_str());}
}
bool Shortcut(const SDL_KeyboardEvent& event) {
    if(event.repeat)return false;
    if(Match(event,hotkeys.at("editor"))) {shown=!shown.load();KeyboardMode(InputMode());return true;}
    if(!Match(event,hotkeys.at("camera")) && !Match(event,hotkeys.at("preview")) && !Match(event,hotkeys.at("capture")))return false;
    if(text_input || !bridge_ready.load())return false;
    try {
        const auto state=nlohmann::json::parse(state_fn());
        if(!state.value("demo",false))return false;
        if(Match(event,hotkeys.at("camera"))) {
            Send(state.value("mode",std::string("free"))=="free"?"set_camera":"release_camera");return true;
        }
        if(Match(event,hotkeys.at("preview"))) {
            Send("preview",{{"playing",!state.value("preview_playing",false)},{"sync_demo",state.value("sync_demo",false)}});return true;
        }
        if(Match(event,hotkeys.at("capture"))) {
            const double time=state.value("cursor",0.0);
            for(const auto& key:state.at("project").at("keys")) {
                if(std::abs(key.value("time",0.0)-time)<.5/state.value("tick_rate",64.0)) {
                    auto update=state.at("camera");update["id"]=key.at("id");Send("update_key",update);return true;
                }
            }
            Send("add_key",{{"time",time}});return true;
        }
    }catch(...){return false;}
    return false;
}
std::chrono::steady_clock::time_point previous_frame;
template<class T> T Sdl(const char* name) {
    static std::atomic<void*> cached{nullptr};
    auto library=cached.load();
    if(!library) {
        static std::mutex load_mutex;
        std::lock_guard<std::mutex> lock(load_mutex);
        library=cached.load();
        if(!library) { library=dlopen("libSDL3.so.0",RTLD_NOW|RTLD_NOLOAD);cached=library; }
    }
    return reinterpret_cast<T>(library ? RealSymbol(library,name) : nullptr);
}
void CursorMode(bool capture) {
    static auto is_main=Sdl<bool(*)()>("SDL_IsMainThread");if(!is_main || !is_main())return;
    auto window=game_window.load();
    static cs2_camera_platform::CursorCapture cursor;
    static auto relative=Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowRelativeMouseMode");
    static auto grab=Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowMouseGrab");
    static auto show=Sdl<bool(*)()>("SDL_ShowCursor");
    static auto hide=Sdl<bool(*)()>("SDL_HideCursor");
    static auto get_relative=Sdl<bool(*)(SDL_Window*)>("SDL_GetWindowRelativeMouseMode");
    static auto get_grab=Sdl<bool(*)(SDL_Window*)>("SDL_GetWindowMouseGrab");
    static auto get_cursor=Sdl<bool(*)()>("SDL_CursorVisible");static auto get_text=Sdl<bool(*)(SDL_Window*)>("SDL_TextInputActive");
    static auto start_text=Sdl<bool(*)(SDL_Window*)>("SDL_StartTextInput");static auto stop_text=Sdl<bool(*)(SDL_Window*)>("SDL_StopTextInput");
    cs2_camera_platform::CursorCapture::Functions f;
    f.relative=[&]{return get_relative && get_relative(window);};f.grab=[&]{return get_grab && get_grab(window);};f.visible=[&]{return !get_cursor || get_cursor();};f.text=[&]{return get_text && get_text(window);};
    f.setRelative=[&](bool value){if(relative)relative(window,value);};f.setGrab=[&](bool value){if(grab)grab(window,value);};f.setVisible=[&](bool value){if(value){if(show)show();}else if(hide)hide();};f.setText=[&](bool value){if(value){if(start_text)start_text(window);}else if(stop_text)stop_text(window);};
    cursor.Update(true,reinterpret_cast<std::uintptr_t>(window),capture,relative_requested.load(),grab_requested.load(),cursor_visible_requested.load(),f);
}
SDL_WindowID EventWindow(const SDL_Event& event) {
    switch(event.type){
    case SDL_EVENT_MOUSE_MOTION:return event.motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:case SDL_EVENT_MOUSE_BUTTON_UP:return event.button.windowID;
    case SDL_EVENT_MOUSE_WHEEL:return event.wheel.windowID;
    case SDL_EVENT_KEY_DOWN:case SDL_EVENT_KEY_UP:return event.key.windowID;
    case SDL_EVENT_TEXT_INPUT:return event.text.windowID;
    default:if(event.type>=SDL_EVENT_WINDOW_FIRST && event.type<=SDL_EVENT_WINDOW_LAST)return event.window.windowID;
    }return 0;
}
bool SDLCALL Filter(void*, SDL_Event* event) {
    if (!event) return true;
    static thread_local bool filtering=false;
    if(filtering)return true;
    struct Guard { bool& value; Guard(bool& v):value(v){value=true;} ~Guard(){value=false;} } guard(filtering);
    const bool keyboard=event->type==SDL_EVENT_KEY_DOWN || event->type==SDL_EVENT_KEY_UP || event->type==SDL_EVENT_TEXT_INPUT;
    const bool mouse=event->type>=SDL_EVENT_MOUSE_MOTION && event->type<=SDL_EVENT_MOUSE_WHEEL;
    const auto id=EventWindow(*event);
    if(id && !game_window_id.load())pending_window_id=id;
    const bool own=cs2_camera_platform::OwnEvent(game_window_id.load(),id);
    if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN || event->type==SDL_EVENT_MOUSE_BUTTON_UP){if(own)++mouse_button_events;else ++foreign_mouse_button_events;}
    if(own && (event->type==SDL_EVENT_WINDOW_FOCUS_GAINED || event->type==SDL_EVENT_WINDOW_FOCUS_LOST)) {
        window_focused=event->type==SDL_EVENT_WINDOW_FOCUS_GAINED;
        if(!window_focused){std::lock_guard<std::mutex> lock(keyboard_mutex);std::fill(std::begin(physical_keys),std::end(physical_keys),false);std::fill(std::begin(keyboard_state),std::end(keyboard_state),false);}
    }
    if(own && (event->type==SDL_EVENT_KEY_DOWN || event->type==SDL_EVENT_KEY_UP)) {
        const auto scan=event->key.scancode;
        if(scan>=0 && scan<SDL_SCANCODE_COUNT) {
            std::lock_guard<std::mutex> lock(keyboard_mutex);
            physical_keys[scan]=event->type==SDL_EVENT_KEY_DOWN;
            keyboard_state[scan]=!InputMode() && window_focused.load() && physical_keys[scan];
        }
    }
    if(own && event->type==SDL_EVENT_KEY_DOWN && Shortcut(event->key))return false;
    if(own) {
        std::lock_guard<std::mutex> lock(events_mutex);
        if(events.size()<2048)events.push_back({*event,event->type==SDL_EVENT_TEXT_INPUT && event->text.text ? event->text.text : ""});
    }
    const bool consume=own && InputMode() && ((mouse && wants_mouse) || (keyboard && wants_keyboard));
    // Releases must reach the engine even if their presses preceded F8.
    if(consume && event->type!=SDL_EVENT_KEY_UP && event->type!=SDL_EVENT_MOUSE_BUTTON_UP)return false;
    SDL_EventFilter next=nullptr;void* data=nullptr;
    {std::lock_guard<std::mutex> lock(filter_mutex);next=previous_filter;data=previous_filter_data;}
    return next ? next(data,event) : true;
}
void InstallFilter() {
    static auto set=Sdl<void(*)(SDL_EventFilter,void*)>("SDL_SetEventFilter");
    static auto get=Sdl<bool(*)(SDL_EventFilter*,void**)>("SDL_GetEventFilter");
    if(!set)return;
    SDL_EventFilter current=nullptr;void* data=nullptr;
    if(get)get(&current,&data);
    if(current==Filter)return;
    {std::lock_guard<std::mutex> lock(filter_mutex);previous_filter=current;previous_filter_data=data;}
    if(!filter_installed)LoadHotkeys();
    set(Filter,nullptr);
    if(!filter_installed)Log("SDL3 editor input enabled; F8 toggles editor");
    filter_installed=true;
}
void UninstallFilter() {
    if(!filter_installed)return;
    static auto set=Sdl<void(*)(SDL_EventFilter,void*)>("SDL_SetEventFilter");
    SDL_EventFilter next=nullptr;void* data=nullptr;
    {std::lock_guard<std::mutex> lock(filter_mutex);next=previous_filter;data=previous_filter_data;}
    if(set)set(next,data);
    filter_installed=false;
}
void RememberWindow(SDL_Window* window) {
    static auto is_main=Sdl<bool(*)()>("SDL_IsMainThread");
    static auto get_id=Sdl<SDL_WindowID(*)(SDL_Window*)>("SDL_GetWindowID");
    if(window && is_main && is_main() && get_id)pending_window_id=get_id(window);
}
void MainThreadService() {
    static auto is_main=Sdl<bool(*)()>("SDL_IsMainThread");if(!is_main || !is_main())return;
    static thread_local bool servicing=false;if(servicing)return;
    struct Guard{bool& value;explicit Guard(bool& v):value(v){value=true;}~Guard(){value=false;}} guard(servicing);
    try {
        static auto from_id=Sdl<SDL_Window*(*)(SDL_WindowID)>("SDL_GetWindowFromID");
        static auto get_id=Sdl<SDL_WindowID(*)(SDL_Window*)>("SDL_GetWindowID");
        static auto flags=Sdl<SDL_WindowFlags(*)(SDL_Window*)>("SDL_GetWindowFlags");
        if(!from_id || !get_id || !flags)return;
        const auto vulkan=[&](SDL_Window* window){return window && (flags(window)&SDL_WINDOW_VULKAN)!=0;};
        SDL_Window* window=game_window_id.load()?from_id(game_window_id.load()):nullptr;
        if(!vulkan(window))window=nullptr;
        if(!window && pending_window_id.load()){auto candidate=from_id(pending_window_id.load());if(vulkan(candidate))window=candidate;}
        if(!window) {
            static auto windows=Sdl<SDL_Window**(*)(int*)>("SDL_GetWindows");static auto release=Sdl<void(*)(void*)>("SDL_free");
            if(windows && release){int count=0;auto list=windows(&count);unsigned matches=0;
                if(list && count>=0 && count<=64)for(int i=0;i<count;++i)if(vulkan(list[i])){window=list[i];++matches;}
                if(matches!=1)window=nullptr;
                if(list)release(list);
            }
        }
        const auto previous=game_window.load();const auto id=window?get_id(window):0;
        game_window=window;game_window_id=id;
        if(previous!=window){
            {std::lock_guard<std::mutex> lock(events_mutex);events.clear();if(id){SDL_Event reset{};reset.type=SDL_EVENT_WINDOW_FOCUS_LOST;reset.window.windowID=id;events.push_back({reset,{}});}}
            {std::lock_guard<std::mutex> lock(keyboard_mutex);std::fill(std::begin(physical_keys),std::end(physical_keys),false);std::fill(std::begin(keyboard_state),std::end(keyboard_state),false);}
            relative_requested=-1;grab_requested=-1;cursor_visible_requested=-1;
        }
        int width=0,height=0;static auto size=Sdl<bool(*)(SDL_Window*,int*,int*)>("SDL_GetWindowSize");
        if(window && size && !size(window,&width,&height))width=height=0;
        logical_width=width;logical_height=height;
        const auto current_flags=window?flags(window):SDL_WindowFlags(0);
        window_focused=window && (current_flags&SDL_WINDOW_INPUT_FOCUS)!=0;
        if(overlay_available.load())InstallFilter();
        CursorMode(InputMode());RefreshKeyboard(InputMode());
        static cs2_camera_platform::DesktopFullscreen fullscreen;
        static const bool requested=[] {const char* value=std::getenv("CS2_CAMERA_FULLSCREEN_WINDOWED");return value && std::strcmp(value,"1")==0;}();
        fullscreen.Requested(requested);
        static auto desktop=Sdl<bool(*)(SDL_Window*,const SDL_DisplayMode*)>("SDL_SetWindowFullscreenMode");
        static auto set_fullscreen=Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowFullscreen");
        static auto fullscreen_mode=Sdl<const SDL_DisplayMode*(*)(SDL_Window*)>("SDL_GetWindowFullscreenMode");
        const bool desktop_fullscreen=window && (current_flags&SDL_WINDOW_FULLSCREEN) && fullscreen_mode && fullscreen_mode(window)==nullptr;
        fullscreen.Update(true,reinterpret_cast<std::uintptr_t>(window),id,bool(window),desktop_fullscreen,
            std::chrono::steady_clock::now(),[&]{return desktop && desktop(window,nullptr);},[&]{return set_fullscreen && set_fullscreen(window,true);});
        static std::string previous_diagnostic;
        const auto diagnostic=std::to_string(id)+":"+std::to_string(width)+":"+std::to_string(height)+":"+std::to_string(window_focused.load())+":"+std::to_string(InputMode())+":"+fullscreen.Name()+":"+std::to_string(fullscreen.Attempts());
        if(diagnostic!=previous_diagnostic){
            previous_diagnostic=diagnostic;
            static auto get_relative=Sdl<bool(*)(SDL_Window*)>("SDL_GetWindowRelativeMouseMode");static auto get_grab=Sdl<bool(*)(SDL_Window*)>("SDL_GetWindowMouseGrab");static auto visible=Sdl<bool(*)()>("SDL_CursorVisible");
            Log("SDL window id=%u logical=%dx%d focused=%d input_available=%d editor_input=%d relative=%d mouse_grab=%d cursor_visible=%d fullscreen=%s attempts=%u",unsigned(id),width,height,int(window_focused.load()),int(id && width>0 && height>0 && filter_installed.load()),int(InputMode()),window && get_relative?int(get_relative(window)):-1,window && get_grab?int(get_grab(window)):-1,visible?int(visible()):-1,fullscreen.Name(),fullscreen.Attempts());
        }
    }catch(const std::exception& error){Log("SDL main-thread service: %s",error.what());}
    catch(...){Log("SDL main-thread service failed");}
}
using EventDispatch=cs2_camera_platform::EventDispatch;
ImGuiKey MapKey(SDL_Scancode code) {
    if(code>=SDL_SCANCODE_A && code<=SDL_SCANCODE_Z)return ImGuiKey(ImGuiKey_A+code-SDL_SCANCODE_A);
    if(code>=SDL_SCANCODE_1 && code<=SDL_SCANCODE_9)return ImGuiKey(ImGuiKey_1+code-SDL_SCANCODE_1);
    if(code>=SDL_SCANCODE_F1 && code<=SDL_SCANCODE_F12)return ImGuiKey(ImGuiKey_F1+code-SDL_SCANCODE_F1);
    switch(code) {
    case SDL_SCANCODE_0:return ImGuiKey_0; case SDL_SCANCODE_TAB:return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT:return ImGuiKey_LeftArrow; case SDL_SCANCODE_RIGHT:return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP:return ImGuiKey_UpArrow; case SDL_SCANCODE_DOWN:return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP:return ImGuiKey_PageUp; case SDL_SCANCODE_PAGEDOWN:return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME:return ImGuiKey_Home; case SDL_SCANCODE_END:return ImGuiKey_End;
    case SDL_SCANCODE_INSERT:return ImGuiKey_Insert; case SDL_SCANCODE_DELETE:return ImGuiKey_Delete;
    case SDL_SCANCODE_BACKSPACE:return ImGuiKey_Backspace; case SDL_SCANCODE_SPACE:return ImGuiKey_Space;
    case SDL_SCANCODE_RETURN:return ImGuiKey_Enter; case SDL_SCANCODE_ESCAPE:return ImGuiKey_Escape;
    case SDL_SCANCODE_LCTRL:return ImGuiKey_LeftCtrl; case SDL_SCANCODE_RCTRL:return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LSHIFT:return ImGuiKey_LeftShift; case SDL_SCANCODE_RSHIFT:return ImGuiKey_RightShift;
    case SDL_SCANCODE_LALT:return ImGuiKey_LeftAlt; case SDL_SCANCODE_RALT:return ImGuiKey_RightAlt;
    case SDL_SCANCODE_LGUI:return ImGuiKey_LeftSuper; case SDL_SCANCODE_RGUI:return ImGuiKey_RightSuper;
    case SDL_SCANCODE_MINUS:return ImGuiKey_Minus; case SDL_SCANCODE_EQUALS:return ImGuiKey_Equal;
    case SDL_SCANCODE_PERIOD:return ImGuiKey_Period; case SDL_SCANCODE_COMMA:return ImGuiKey_Comma;
    case SDL_SCANCODE_SLASH:return ImGuiKey_Slash; case SDL_SCANCODE_SEMICOLON:return ImGuiKey_Semicolon;
    case SDL_SCANCODE_APOSTROPHE:return ImGuiKey_Apostrophe;
    default:return ImGuiKey_None;
    }
}
void Input(ImGuiIO& io,unsigned framebuffer_width,unsigned framebuffer_height) {
    std::deque<InputEvent> pending;
    {std::lock_guard<std::mutex> lock(events_mutex);pending.swap(events);}
    for(const auto& item:pending) {
        const auto& event=item.event;
        if(!cs2_camera_platform::OwnEvent(game_window_id.load(),EventWindow(event)))continue;
        const auto position=[&](float x,float y){const auto p=cs2_camera_platform::Pointer(game_window_id.load(),EventWindow(event),logical_width.load(),logical_height.load(),framebuffer_width,framebuffer_height,x,y);if(p)io.AddMousePosEvent(p->x,p->y);return bool(p);};
        switch(event.type) {
        case SDL_EVENT_MOUSE_MOTION: position(event.motion.x,event.motion.y);break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int button=-1;
            if(event.button.button==SDL_BUTTON_LEFT)button=0;
            if(event.button.button==SDL_BUTTON_RIGHT)button=1;
            if(event.button.button==SDL_BUTTON_MIDDLE)button=2;
            const auto p=cs2_camera_platform::Pointer(game_window_id.load(),EventWindow(event),logical_width.load(),logical_height.load(),framebuffer_width,framebuffer_height,event.button.x,event.button.y);
            if(button>=0)cs2_camera_platform::PointerButton(p,event.type==SDL_EVENT_MOUSE_BUTTON_DOWN,[&](const cs2_camera_platform::Point& value){io.AddMousePosEvent(value.x,value.y);},[&](bool down){io.AddMouseButtonEvent(button,down);++mouse_button_delivered;});
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: io.AddMouseWheelEvent(event.wheel.x,event.wheel.y);break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            auto key=MapKey(event.key.scancode);
            if(key!=ImGuiKey_None)io.AddKeyEvent(key,event.type==SDL_EVENT_KEY_DOWN);
            io.AddKeyEvent(ImGuiMod_Ctrl,(event.key.mod & SDL_KMOD_CTRL)!=0);
            io.AddKeyEvent(ImGuiMod_Shift,(event.key.mod & SDL_KMOD_SHIFT)!=0);
            io.AddKeyEvent(ImGuiMod_Alt,(event.key.mod & SDL_KMOD_ALT)!=0);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: io.AddInputCharactersUTF8(item.text.c_str());break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:io.AddFocusEvent(true);break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:io.AddFocusEvent(false);break;
        }
    }
}
int FindBridge(dl_phdr_info* info,size_t,void*) {
    if(!info->dlpi_name || !std::strstr(info->dlpi_name,"libserver.so"))return 0;
    void* library=dlopen(info->dlpi_name,RTLD_NOW|RTLD_NOLOAD);
    if(library) {
        state_fn=reinterpret_cast<StateFn>(dlsym(library,"cs2_live_ui_state_json"));
        if(!state_fn)state_fn=reinterpret_cast<StateFn>(dlsym(library,"cs2_live_state_json"));
        map_preview_fn=reinterpret_cast<StateFn>(dlsym(library,"cs2_live_map_preview_json"));
        command_fn=reinterpret_cast<CommandFn>(dlsym(library,"cs2_live_command_json"));
        dlclose(library);
    }
    if(state_fn && command_fn){bridge_ready=true;return 1;}
    return 0;
}
bool Draw(Swapchain& s,uint32_t image_index) {
    const bool frame_shown=shown.load();
    if(!bridge_ready.load())dl_iterate_phdr(FindBridge,nullptr);
    auto& io=ImGui::GetIO();
    io.DisplaySize=ImVec2(float(s.extent.width),float(s.extent.height));
    io.DisplayFramebufferScale=ImVec2(1,1);
    const auto now=std::chrono::steady_clock::now();
    io.DeltaTime=previous_frame.time_since_epoch().count() ? std::chrono::duration<float>(now-previous_frame).count() : 1.f/60.f;
    if(io.DeltaTime<=0 || io.DeltaTime>1)io.DeltaTime=1.f/60.f;
    previous_frame=now;
    Input(io,s.extent.width,s.extent.height);
    io.AddFocusEvent(window_focused.load());
    static std::string previous_input_dimensions;
    const auto dimensions=std::to_string(game_window_id.load())+":"+std::to_string(logical_width.load())+":"+std::to_string(logical_height.load())+":"+std::to_string(s.extent.width)+":"+std::to_string(s.extent.height);
    if(dimensions!=previous_input_dimensions){previous_input_dimensions=dimensions;Log("SDL input mapping window_id=%u logical=%dx%d framebuffer=%ux%u",unsigned(game_window_id.load()),logical_width.load(),logical_height.load(),s.extent.width,s.extent.height);}
    static std::uint64_t previous_buttons=0;
    const auto buttons=mouse_button_events.load();if(buttons!=previous_buttons){previous_buttons=buttons;Log("SDL mouse buttons window_id=%u observed=%llu queued_for_imgui=%llu foreign=%llu focused=%d",unsigned(game_window_id.load()),static_cast<unsigned long long>(buttons),static_cast<unsigned long long>(mouse_button_delivered.load()),static_cast<unsigned long long>(foreign_mouse_button_events.load()),int(window_focused.load()));}
    ImGui_ImplVulkan_NewFrame(); ImGui::NewFrame();
    nlohmann::json state={{"connected",false},{"demo",false},{"tick",0},{"tick_rate",64},{"paused",true},{"mode","free"},
      {"camera",{{"position",{0,0,0}},{"angles",{0,0,0}},{"fov",90}}},
      {"project",{{"name","Untitled"},{"start_tick",0},{"duration",5},{"keys",nlohmann::json::array()}}},
      {"cursor",0},{"preview_playing",false},{"revision",0},{"error","Waiting for live camera module"}};
    if(state_fn) {try{state=nlohmann::json::parse(state_fn());}catch(...){state["error"]="Invalid camera state";}}
    if(map_preview_fn) {
        static std::string previous_map;
        const char* raw=map_preview_fn();
        if(raw && previous_map!=raw)try {
            auto preview=nlohmann::json::parse(raw);
            editor.SetMapPreview(preview);previous_map=raw;
        }catch(...){state["error"]="Invalid map preview";}
    }
    state["hotkeys"]={{"capture",hotkeys.at("capture").name},{"camera",hotkeys.at("camera").name},
        {"editor",hotkeys.at("editor").name},{"preview",hotkeys.at("preview").name},{"config_path",hotkeys_path}};
    s.frame_blur_enabled=!s.capture.BlurDisabled();
    const auto preferences=state.find("editor_preferences");
    if(preferences!=state.end() && preferences->is_object()) {
        const auto enabled=preferences->find("blur_enabled");
        if(enabled!=preferences->end() && enabled->is_boolean())s.frame_blur_enabled=s.frame_blur_enabled && enabled->get<bool>();
    }
    editor.backdrop_texture=!s.frame_blur_enabled?0:
        static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(s.blur.Texture(image_index)));
    editor.visible=frame_shown;
    if(frame_shown) {
        editor.Draw(state,[](const std::string& method,const nlohmann::json& params){
            if(command_fn) { const auto request=nlohmann::json{{"method",method},{"params",params}}.dump(); command_fn(request.c_str()); }
        });
        if (!editor.visible) {
            shown=false;
            KeyboardMode(InputMode());
        }
    }
    // The editor is an explicit input mode. Hide it for the game's roaming
    // controls; otherwise engine polling/warping must not fight widget input.
    wants_mouse=shown.load();
    wants_keyboard=shown.load();
    text_input=shown && (io.WantTextInput || ImGui::IsAnyItemActive());
    io.MouseDrawCursor=false; // The main-thread service keeps the native cursor visible.
    ImGui::Render();
    if(s.format==VK_FORMAT_B8G8R8A8_SRGB || s.format==VK_FORMAT_R8G8B8A8_SRGB || s.format==VK_FORMAT_A8B8G8R8_SRGB_PACK32) {
        static const auto linear=[] {std::array<ImU32,256> table{};for(int i=0;i<256;++i) {
            float value=i/255.f;value=value<=.04045f?value/12.92f:std::pow((value+.055f)/1.055f,2.4f);
            table[i]=ImU32(value*255.f+.5f);
        }return table;}();
        auto* data=ImGui::GetDrawData();
        for(int list=0;list<data->CmdListsCount;++list)for(auto& vertex:data->CmdLists[list]->VtxBuffer) {
            const auto color=vertex.col;
            vertex.col=(color&IM_COL32_A_MASK)|(linear[(color>>IM_COL32_R_SHIFT)&255]<<IM_COL32_R_SHIFT)
                |(linear[(color>>IM_COL32_G_SHIFT)&255]<<IM_COL32_G_SHIFT)|(linear[(color>>IM_COL32_B_SHIFT)&255]<<IM_COL32_B_SHIFT);
        }
    }
    return frame_shown;
}
bool Check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) {Log("%s failed (%d)",operation,int(result));return false;}return true;
}
Device* Find(VkDevice h);
VKAPI_ATTR VkResult VKAPI_CALL AllocateOverlayCommandBuffers(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* commands) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto* d=Find(device);
    if(!d || !d->AllocateCommandBuffers || !d->FreeCommandBuffers)return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=d->AllocateCommandBuffers(device,info,commands);
    if(result!=VK_SUCCESS)return result;
    // We allocate below the loader trampoline, including ImGui's private font
    // upload command buffer. Every new dispatchable handle needs loader data,
    // not just the swapchain frame buffers allocated explicitly in Setup.
    for(uint32_t i=0;i<info->commandBufferCount;++i) {
        VkResult initialized=VK_SUCCESS;
        if(d->set_loader_data)initialized=d->set_loader_data(device,commands[i]);
        else *reinterpret_cast<void**>(commands[i])=*reinterpret_cast<void**>(device);
        if(initialized!=VK_SUCCESS) {
            d->FreeCommandBuffers(device,info->commandPool,info->commandBufferCount,commands);
            std::fill(commands,commands+info->commandBufferCount,VK_NULL_HANDLE);
            return initialized;
        }
    }
    return VK_SUCCESS;
}
PFN_vkVoidFunction BackendFunction(const char* name,void* opaque) {
    auto* d=static_cast<Device*>(opaque);
    if(std::strcmp(name,"vkAllocateCommandBuffers")==0)
        return reinterpret_cast<PFN_vkVoidFunction>(AllocateOverlayCommandBuffers);
    // Physical-device and instance functions are not valid gdpa queries. Keep
    // this private ImGui dispatch separate from application Vulkan dispatch.
    if(std::strncmp(name,"vkGetPhysicalDevice",19)==0 ||
       std::strcmp(name,"vkEnumeratePhysicalDevices")==0 || std::strcmp(name,"vkDestroySurfaceKHR")==0)
        return d->instance->gipa(d->instance->handle,name);
    return d->gdpa(d->handle,name);
}
void CheckBackendResult(VkResult result) {
    // The pinned backend reports allocation errors but otherwise continues.
    // Stop before it begins a null/uninitialized font command buffer instead.
    if(result<0)throw std::runtime_error("ImGui Vulkan operation failed ("+std::to_string(int(result))+")");
}
void DisableOverlay(Swapchain& s) {
    s.failed=true;overlay_available=false;wants_mouse=false;wants_keyboard=false;text_input=false;
    KeyboardMode(false); // MainThreadService releases the SDL cursor/grab.
}
VkResult CompleteOverlaySubmission(Device& d,const Frame& frame,bool panels_drawn,bool capture,bool diagnostic_wait) {
    if(!panels_drawn && !capture && !diagnostic_wait)return VK_SUCCESS;
    // On the tested NVIDIA/IMMEDIATE path, waiting for our own submitted frame
    // removes intermittent transparent UI bands; layout-only barriers do not.
    // This is a measured timing workaround, not a proven driver/root-cause or
    // ring-buffer/spec diagnosis. Hidden-editor frames pay no additional wait.
    return d.WaitForFences(d.handle,1,&frame.fence,VK_TRUE,1000000000ull);
}
void Cleanup(Swapchain& s) {
    auto& d=*s.device;
    if(active==&s){overlay_available=false;KeyboardMode(false);}
    if(s.ready || s.pool)d.DeviceWaitIdle(d.handle);
    s.capture.Destroy();
    s.blur.Destroy();
    if(active==&s) {if(ImGui::GetCurrentContext()){
        if(ImGui::GetIO().BackendRendererUserData)ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();}active=nullptr;}
    for(auto& f:s.frames) {
        if(f.fence)d.DestroyFence(d.handle,f.fence,nullptr);
        if(f.complete)d.DestroySemaphore(d.handle,f.complete,nullptr);
        if(f.framebuffer)d.DestroyFramebuffer(d.handle,f.framebuffer,nullptr);
        if(f.view)d.DestroyImageView(d.handle,f.view,nullptr);
    }
    if(s.pool)d.DestroyCommandPool(d.handle,s.pool,nullptr);
    if(s.pass)d.DestroyRenderPass(d.handle,s.pass,nullptr);
    s.frames.clear();s.pool={};s.pass={};s.ready=false;
}
bool Setup(Swapchain& s,VkQueue queue,uint32_t family) {
    auto& d=*s.device;
    if(active && active!=&s)Cleanup(*active);
    uint32_t count=0;
    if(!Check(d.GetSwapchainImagesKHR(d.handle,s.handle,&count,nullptr),"swapchain images") || count<2)return false;
    std::vector<VkImage> images(count);
    if(!Check(d.GetSwapchainImagesKHR(d.handle,s.handle,&count,images.data()),"swapchain images"))return false;
    s.frames.resize(count);s.family=family;s.queue=queue;
    VkAttachmentDescription attachment{};
    attachment.format=s.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;attachment.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
    VkSubpassDependency dependency{};dependency.srcSubpass=VK_SUBPASS_EXTERNAL;dependency.dstSubpass=0;
    dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;dependency.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;dependency.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=1;rp.pDependencies=&dependency;
    if(!Check(d.CreateRenderPass(d.handle,&rp,nullptr,&s.pass),"render pass"))return false;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=family;
    if(!Check(d.CreateCommandPool(d.handle,&pool,nullptr,&s.pool),"command pool"))return false;
    for(uint32_t i=0;i<count;++i) {
        auto& f=s.frames[i];f.image=images[i];
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=images[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=s.format;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(!Check(d.CreateImageView(d.handle,&vi,nullptr,&f.view),"image view"))return false;
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=s.pass;fb.attachmentCount=1;fb.pAttachments=&f.view;fb.width=s.extent.width;fb.height=s.extent.height;fb.layers=1;
        if(!Check(d.CreateFramebuffer(d.handle,&fb,nullptr,&f.framebuffer),"framebuffer"))return false;
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};alloc.commandPool=s.pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;
        if(!Check(AllocateOverlayCommandBuffers(d.handle,&alloc,&f.cmd),"command buffer"))return false;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if(!Check(d.CreateFence(d.handle,&fence,nullptr,&f.fence),"fence") || !Check(d.CreateSemaphore(d.handle,&sem,nullptr,&f.complete),"semaphore"))return false;
    }
    ImGui::CreateContext();active=&s;auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    LoadCosmeticsCatalog();
    LoadSavedShots();
    const char* custom_font=std::getenv("CS2_CAMERA_FONT");
    const char* installed_font=std::getenv("CS2_CAMERA_GAME_FONT");
    auto font_exists=[](const char* path){if(!path)return false;auto* file=std::fopen(path,"rb");if(!file)return false;std::fclose(file);return true;};
    const char* font=font_exists(installed_font)?installed_font:(font_exists(custom_font)?custom_font:"/usr/share/fonts/noto/NotoSans-Regular.ttf");
    const float font_size=18.f*s.extent.height/1080.f;
    if(font_exists(font))io.FontDefault=io.Fonts->AddFontFromFileTTF(font,font_size,nullptr,io.Fonts->GetGlyphRangesCyrillic());
    editor.mono_font=io.FontDefault;
    const char* mono=std::getenv("CS2_CAMERA_MONO_FONT");
    if(font_exists(mono) && font_exists(font)) {
        static const ImWchar digits[]={0x0020,0x0020,0x0030,0x003a,0};
        editor.mono_font=io.Fonts->AddFontFromFileTTF(mono,font_size,nullptr,digits);
        ImFontConfig merge;merge.MergeMode=true;
        io.Fonts->AddFontFromFileTTF(font,font_size,&merge,io.Fonts->GetGlyphRangesCyrillic());
    }
    io.BackendPlatformName="cs2-camera-sdl3";io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if(!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1,BackendFunction,&d))return false;
    ImGui_ImplVulkan_InitInfo init{};init.ApiVersion=VK_API_VERSION_1_1;init.Instance=d.instance->handle;init.PhysicalDevice=d.physical;init.Device=d.handle;
    init.QueueFamily=family;init.Queue=queue;init.RenderPass=s.pass;init.MinImageCount=std::min(count,std::max(2u,s.min_images));init.ImageCount=count;init.MSAASamples=VK_SAMPLE_COUNT_1_BIT;init.DescriptorPoolSize=std::max(16u,count+2);
    init.CheckVkResultFn=CheckBackendResult;
    if(!ImGui_ImplVulkan_Init(&init))return false;
    active=&s;s.ready=true;overlay_available=true;
    if(s.blur_supported) {
        cs2_camera_blur::InitInfo blur;
        blur.physical=d.physical;blur.device=d.handle;blur.gdpa=d.gdpa;
        blur.getMemoryProperties=d.instance->memory_properties;blur.getFormatProperties=d.instance->format_properties;
        blur.extent=s.extent;blur.sourceFormat=s.format;blur.imageCount=count;blur.encodedDimAlpha=160.f/255;
        blur.AddTexture=[](VkSampler sampler,VkImageView view,VkImageLayout layout){return ImGui_ImplVulkan_AddTexture(sampler,view,layout);};
        blur.RemoveTexture=[](VkDescriptorSet texture){ImGui_ImplVulkan_RemoveTexture(texture);};
        if(!s.blur.Initialize(blur))Log("backdrop unavailable: %s",s.blur.Error().c_str());
        else Log("Gaussian scene backdrop ready");
    }
    Log("swapchain format=%d backdrop=%d",int(s.format),s.blur.Ready());
    Log("in-game editor ready: %ux%u, %u swapchain images",s.extent.width,s.extent.height,count);return true;
}
Device* Find(VkDevice h) {auto i=devices.find(Key(h));return i==devices.end()?nullptr:i->second.get();}
Device* FindQueue(VkQueue h) {auto i=devices.find(Key(h));return i==devices.end()?nullptr:i->second.get();}
Instance* Find(VkInstance h) {auto i=instances.find(Key(h));return i==instances.end()?nullptr:i->second.get();}
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci,const VkAllocationCallbacks* alloc,VkInstance* output) {
    auto* link=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while(link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO))link=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    if(!link)return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE,"vkCreateInstance"));link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    auto result=create(ci,alloc,output);
    if(result==VK_SUCCESS) {
        auto data=std::make_unique<Instance>();data->handle=*output;data->gipa=gipa;
        data->destroy=reinterpret_cast<PFN_vkDestroyInstance>(gipa(*output,"vkDestroyInstance"));
        data->queue_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(*output,"vkGetPhysicalDeviceQueueFamilyProperties"));
        data->memory_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(*output,"vkGetPhysicalDeviceMemoryProperties"));
        data->format_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(gipa(*output,"vkGetPhysicalDeviceFormatProperties"));
        data->surface_capabilities=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(gipa(*output,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        std::lock_guard<std::recursive_mutex> lock(mutex);instances[Key(*output)]=std::move(data);
        Log("Vulkan instance attached");
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::recursive_mutex> lock(mutex);auto* data=Find(instance);if(!data)return;
    if(instances.size()==1)UninstallFilter();
    auto destroy=data->destroy;instances.erase(Key(instance));destroy(instance,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* ci,const VkAllocationCallbacks* alloc,VkDevice* output) {
    auto* link=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    PFN_vkSetDeviceLoaderData set_data=nullptr;
    for(auto* p=link;p;p=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(p->pNext)))if(p->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && p->function==VK_LOADER_DATA_CALLBACK)set_data=p->u.pfnSetDeviceLoaderData;
    while(link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO))link=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    if(!link)return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;auto gdpa=link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    VkInstance instance{};
    {std::lock_guard<std::recursive_mutex> lock(mutex);auto it=instances.find(Key(physical));if(it!=instances.end())instance=it->second->handle;}
    auto create=reinterpret_cast<PFN_vkCreateDevice>(gipa(instance,"vkCreateDevice"));link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    if(!create)return VK_ERROR_INITIALIZATION_FAILED;
    auto result=create(physical,ci,alloc,output);
    if(result==VK_SUCCESS) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        auto found=instances.find(Key(physical));if(found==instances.end())return result;
        auto d=std::make_unique<Device>();d->handle=*output;d->physical=physical;d->instance=found->second.get();d->gdpa=gdpa;d->set_loader_data=set_data;
#define LOAD(name) d->name=reinterpret_cast<PFN_vk##name>(gdpa(*output,"vk" #name));
        LOAD(DestroyDevice) LOAD(GetDeviceQueue) LOAD(GetDeviceQueue2)
        LOAD(CreateSwapchainKHR) LOAD(DestroySwapchainKHR) LOAD(GetSwapchainImagesKHR) LOAD(QueuePresentKHR) LOAD(QueueSubmit) LOAD(QueueWaitIdle) LOAD(DeviceWaitIdle)
        LOAD(CreateRenderPass) LOAD(DestroyRenderPass) LOAD(CreateImageView) LOAD(DestroyImageView) LOAD(CreateFramebuffer) LOAD(DestroyFramebuffer)
        LOAD(CreateCommandPool) LOAD(DestroyCommandPool) LOAD(AllocateCommandBuffers) LOAD(FreeCommandBuffers) LOAD(ResetCommandBuffer) LOAD(BeginCommandBuffer) LOAD(EndCommandBuffer)
        LOAD(CmdBeginRenderPass) LOAD(CmdEndRenderPass) LOAD(CmdPipelineBarrier) LOAD(CreateFence) LOAD(DestroyFence) LOAD(WaitForFences) LOAD(ResetFences) LOAD(CreateSemaphore) LOAD(DestroySemaphore)
#undef LOAD
        devices[Key(*output)]=std::move(d);Log("Vulkan device attached");
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device,uint32_t family,uint32_t index,VkQueue* queue) {
    std::lock_guard<std::recursive_mutex> lock(mutex);auto* d=Find(device);if(!d)return;
    d->GetDeviceQueue(device,family,index,queue);d->queues[*queue]=family;
}
VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device,const VkDeviceQueueInfo2* info,VkQueue* queue) {
    std::lock_guard<std::recursive_mutex> lock(mutex);auto* d=Find(device);if(!d || !d->GetDeviceQueue2)return;
    d->GetDeviceQueue2(device,info,queue);d->queues[*queue]=info->queueFamilyIndex;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device,const VkSwapchainCreateInfoKHR* ci,const VkAllocationCallbacks* alloc,VkSwapchainKHR* output) {
    std::lock_guard<std::recursive_mutex> lock(mutex);auto* d=Find(device);if(!d)return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR request=*ci;
    VkSurfaceCapabilitiesKHR capabilities{};
    const bool linear_pass=ci->imageFormat==VK_FORMAT_B8G8R8A8_SRGB || ci->imageFormat==VK_FORMAT_R8G8B8A8_SRGB;
    const bool backdrop=linear_pass && ci->imageColorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && d->instance->surface_capabilities
        && d->instance->surface_capabilities(d->physical,ci->surface,&capabilities)==VK_SUCCESS
        && (capabilities.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if(backdrop)request.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto result=d->CreateSwapchainKHR(device,&request,alloc,output);
    if(result==VK_SUCCESS && (ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        auto s=std::make_unique<Swapchain>();s->device=d;s->handle=*output;s->format=ci->imageFormat;s->extent=ci->imageExtent;s->min_images=ci->minImageCount;s->blur_supported=backdrop;s->present_mode=ci->presentMode;
        d->swaps[*output]=std::move(s);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice device,VkSwapchainKHR swap,const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::recursive_mutex> lock(mutex);auto* d=Find(device);if(!d)return;
    auto it=d->swaps.find(swap);if(it!=d->swaps.end()){Cleanup(*it->second);d->swaps.erase(it);}d->DestroySwapchainKHR(device,swap,alloc);
}
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device,const VkAllocationCallbacks* alloc) {
    std::lock_guard<std::recursive_mutex> lock(mutex);auto* d=Find(device);if(!d)return;
    for(auto& pair:d->swaps)Cleanup(*pair.second);
    auto destroy=d->DestroyDevice;devices.erase(Key(device));destroy(device,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue,const VkPresentInfoKHR* present) {
    std::lock_guard<std::recursive_mutex> lock(mutex);auto* d=FindQueue(queue);if(!d)return VK_ERROR_INITIALIZATION_FAILED;
    Swapchain* s=nullptr;uint32_t image_index=0;
    for(uint32_t i=0;i<present->swapchainCount;++i) {auto it=d->swaps.find(present->pSwapchains[i]);if(it!=d->swaps.end()){s=it->second.get();image_index=present->pImageIndices[i];break;}}
    if(!s || s->failed)return d->QueuePresentKHR(queue,present);
    auto family=d->queues.find(queue);
    if(family==d->queues.end())return d->QueuePresentKHR(queue,present);
    if(!s->ready) {
        uint32_t count=0;d->instance->queue_properties(d->physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);d->instance->queue_properties(d->physical,&count,families.data());
        bool ready=false;
        try {ready=family->second<count && (families[family->second].queueFlags&VK_QUEUE_GRAPHICS_BIT) && Setup(*s,queue,family->second);}
        catch(const std::exception& error){Log("editor setup failed: %s",error.what());}
        if(!ready) {Cleanup(*s);DisableOverlay(*s);Log("overlay disabled for unsupported swapchain");return d->QueuePresentKHR(queue,present);}
    }
    if(image_index>=s->frames.size())return d->QueuePresentKHR(queue,present);
    auto& frame=s->frames[image_index];
    if(!Check(d->WaitForFences(d->handle,1,&frame.fence,VK_TRUE,1000000000ull),"wait frame")){DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    const bool capture=s->blur_supported && s->capture.Requested({d->physical,d->handle,d->gdpa,
        d->instance->memory_properties,s->extent,s->format});
    if(s->capture_mode_revision!=s->capture.ModeRevision()) {
        s->capture_mode_revision=s->capture.ModeRevision();
        Log("GPU overlay diagnostic mode no_blur=%d layout_only=%d wait_only=%d cpu_delay_ms=%d",
            s->capture.BlurDisabled(),s->capture.LayoutOnly(),s->capture.WaitOnly(),s->capture.CpuDelay()?10:0);
    }
    if(!capture && !s->capture.Error().empty()) {
        const auto error=s->capture.TakeError();
        Log("GPU overlay capture request %llu unavailable: %s",
            static_cast<unsigned long long>(s->capture.LastRequest()),error.c_str());
    }
    if(s->capture.TakeOpenShot())editor.OpenShotSettingsForDiagnostics();
    if(s->capture.TakeOpenMap())editor.OpenCameraMapForDiagnostics();
    if(s->capture.TakeOpenCurves())editor.OpenCameraCurvesForDiagnostics();
    if(s->capture.TakeOpenTimeline())editor.OpenTimelineForDiagnostics();
    if(s->capture.TakeOpenSettings())editor.OpenSettingsForDiagnostics();
    if(s->capture.TakeOpenCurveMenu())editor.OpenCurveChannelMenuForDiagnostics();
    bool panels_drawn=false;
    try{panels_drawn=Draw(*s,image_index);}
    catch(const std::exception& error){Log("editor draw failed: %s",error.what());s->capture.Cancel();DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    auto* draw=ImGui::GetDrawData();
    if(!draw || !draw->TotalVtxCount){s->capture.Cancel();return d->QueuePresentKHR(queue,present);}
    std::string capture_draw;
    if(capture) {
        // Capture geometry from exactly the frame that will be copied on the
        // GPU. Do not read a later ImGui frame to explain an earlier image.
        nlohmann::json snapshot={{"version",1},{"frame",ImGui::GetFrameCount()},
            {"request",s->capture.Pending()},{"image_index",image_index},{"format",int(s->format)},
            {"present_mode",int(s->present_mode)},
            {"blur",panels_drawn && s->blur.Ready() && s->frame_blur_enabled},
            {"layout_only",s->capture.LayoutOnly()},{"wait_only",s->capture.WaitOnly()},
            {"cpu_delay_ms",s->capture.CpuDelay()?10:0},
            {"setup_queue_matches",queue==s->queue},{"present_wait_count",present->waitSemaphoreCount},
            {"present_extensions",nlohmann::json::array()},
            {"display_pos",{draw->DisplayPos.x,draw->DisplayPos.y}},
            {"display_size",{draw->DisplaySize.x,draw->DisplaySize.y}},
            {"framebuffer_scale",{draw->FramebufferScale.x,draw->FramebufferScale.y}},
            {"total_vertices",draw->TotalVtxCount},{"total_indices",draw->TotalIdxCount},
            {"lists",nlohmann::json::array()}};
        const auto* extension=static_cast<const VkBaseInStructure*>(present->pNext);
        for(unsigned count=0;extension && count<32;++count,extension=extension->pNext)
            snapshot["present_extensions"].push_back(int(extension->sType));
        if(state_fn) {
            const auto* raw=state_fn();
            const auto state=nlohmann::json::parse(raw?raw:"",nullptr,false);
            if(state.is_object() && state.contains("tick"))snapshot["demo_tick"]=state["tick"];
        }
        for(int index=0;index<draw->CmdListsCount;++index) {
            const auto& list=*draw->CmdLists[index];
            nlohmann::json item={{"commands",nlohmann::json::array()},
                {"vertices",nlohmann::json::array()},{"indices",nlohmann::json::array()}};
            for(const auto* window:ImGui::GetCurrentContext()->Windows)if(window->DrawList==&list) {
                item["window"]={{"name",window->Name},{"position",{window->Pos.x,window->Pos.y}},
                    {"size",{window->Size.x,window->Size.y}},{"scroll",{window->Scroll.x,window->Scroll.y}}};
                break;
            }
            for(const auto& command:list.CmdBuffer) item["commands"].push_back({
                {"clip_rect",{command.ClipRect.x,command.ClipRect.y,command.ClipRect.z,command.ClipRect.w}},
                {"texture",std::uint64_t(command.GetTexID())},{"vertex_offset",command.VtxOffset},
                {"index_offset",command.IdxOffset},{"element_count",command.ElemCount},
                {"callback",command.UserCallback!=nullptr}});
            for(const auto& vertex:list.VtxBuffer) item["vertices"].push_back({
                vertex.pos.x,vertex.pos.y,vertex.uv.x,vertex.uv.y,vertex.col});
            for(const auto vertex:list.IdxBuffer)item["indices"].push_back(vertex);
            snapshot["lists"].push_back(std::move(item));
        }
        capture_draw=snapshot.dump();
    }
    if(s->capture.CpuDelay())std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(!Check(d->ResetCommandBuffer(frame.cmd,0),"reset editor command buffer")){s->capture.Cancel();DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(!Check(d->BeginCommandBuffer(frame.cmd,&begin),"begin editor command buffer")){s->capture.Cancel();DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    if(panels_drawn && s->blur.Ready() && s->frame_blur_enabled)s->blur.Record(frame.cmd,image_index,frame.image);
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=s->pass;pass.framebuffer=frame.framebuffer;pass.renderArea.extent=s->extent;
    d->CmdBeginRenderPass(frame.cmd,&pass,VK_SUBPASS_CONTENTS_INLINE);
    try{ImGui_ImplVulkan_RenderDrawData(draw,frame.cmd);}
    catch(const std::exception& error){Log("editor rendering failed: %s",error.what());s->capture.Cancel();DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    d->CmdEndRenderPass(frame.cmd);
    if(s->capture.LayoutOnly())cs2_overlay_capture::Probe::LayoutRoundTrip(d->CmdPipelineBarrier,frame.cmd,frame.image);
    if(capture)s->capture.Record(frame.cmd,frame.image);
    if(d->EndCommandBuffer(frame.cmd)!=VK_SUCCESS){s->capture.Cancel();DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    std::vector<VkPipelineStageFlags> stages(present->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=present->waitSemaphoreCount;submit.pWaitSemaphores=present->pWaitSemaphores;submit.pWaitDstStageMask=stages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&frame.cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&frame.complete;
    if(!Check(d->ResetFences(d->handle,1,&frame.fence),"reset editor fence")){s->capture.Cancel();DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    if(!Check(d->QueueSubmit(queue,1,&submit,frame.fence),"submit editor")){s->capture.Cancel();DisableOverlay(*s);return d->QueuePresentKHR(queue,present);}
    const auto completed=CompleteOverlaySubmission(*d,frame,panels_drawn,capture,s->capture.WaitOnly());
    if(!Check(completed,"complete editor frame")) {
        s->capture.Abandon();DisableOverlay(*s);
        // Submit consumed the game's wait semaphores already. This current
        // present must still wait on our completion semaphore, even on error.
    } else if(capture) {
        const auto sequence=s->capture.Pending();
        if(s->capture.Complete(capture_draw))Log("GPU overlay capture request %llu completed for image %u",
            static_cast<unsigned long long>(sequence),image_index);
        else {const auto error=s->capture.TakeError();Log("GPU overlay capture request %llu failed: %s",
            static_cast<unsigned long long>(sequence),error.c_str());}
    }
    VkPresentInfoKHR modified=*present;modified.waitSemaphoreCount=1;modified.pWaitSemaphores=&frame.complete;
    return d->QueuePresentKHR(queue,&modified);
}
PFN_vkVoidFunction Intercept(const char* name) {
#define FN(vk_name,fn) if(std::strcmp(name,vk_name)==0)return reinterpret_cast<PFN_vkVoidFunction>(fn);
    FN("vkCreateInstance",CreateInstance) FN("vkDestroyInstance",DestroyInstance) FN("vkCreateDevice",CreateDevice) FN("vkDestroyDevice",DestroyDevice)
    FN("vkGetDeviceQueue",GetDeviceQueue) FN("vkGetDeviceQueue2",GetDeviceQueue2) FN("vkCreateSwapchainKHR",CreateSwapchain) FN("vkDestroySwapchainKHR",DestroySwapchain) FN("vkQueuePresentKHR",Present)
#undef FN
    return nullptr;
}
} // namespace camera_overlay

extern "C" {
__attribute__((visibility("default"))) bool SDLCALL SDL_PollEvent(SDL_Event* event) {
    static auto real=camera_overlay::Sdl<bool(*)(SDL_Event*)>("SDL_PollEvent");
    camera_overlay::EventDispatch dispatch(camera_overlay::MainThreadService);return real && real(event);
}
__attribute__((visibility("default"))) void SDLCALL SDL_PumpEvents() {
    static auto real=camera_overlay::Sdl<void(*)()>("SDL_PumpEvents");
    camera_overlay::EventDispatch dispatch(camera_overlay::MainThreadService);if(real)real();
}
__attribute__((visibility("default"))) bool SDLCALL SDL_SetWindowRelativeMouseMode(SDL_Window* window,bool enabled) {
    camera_overlay::RememberWindow(window);
    const bool own=window && window==camera_overlay::game_window.load();
    if(own)camera_overlay::relative_requested=enabled?1:0;
    static auto real=camera_overlay::Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowRelativeMouseMode");
    return real && real(window,own && camera_overlay::InputMode()?false:enabled);
}
__attribute__((visibility("default"))) bool SDLCALL SDL_SetWindowMouseGrab(SDL_Window* window,bool enabled) {
    camera_overlay::RememberWindow(window);
    const bool own=window && window==camera_overlay::game_window.load();
    if(own)camera_overlay::grab_requested=enabled?1:0;
    static auto real=camera_overlay::Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowMouseGrab");
    return real && real(window,own && camera_overlay::InputMode()?false:enabled);
}
__attribute__((visibility("default"))) bool SDLCALL SDL_HideCursor() {
    camera_overlay::cursor_visible_requested=0;
    static auto show=camera_overlay::Sdl<bool(*)()>("SDL_ShowCursor");
    static auto hide=camera_overlay::Sdl<bool(*)()>("SDL_HideCursor");
    const auto real=camera_overlay::InputMode()?show:hide;return real && real();
}
__attribute__((visibility("default"))) bool SDLCALL SDL_ShowCursor() {
    camera_overlay::cursor_visible_requested=1;
    static auto real=camera_overlay::Sdl<bool(*)()>("SDL_ShowCursor");return real && real();
}
__attribute__((visibility("default"))) void SDLCALL SDL_WarpMouseInWindow(SDL_Window* window,float x,float y) {
    if(window==camera_overlay::game_window.load() && camera_overlay::InputMode())return;
    static auto real=camera_overlay::Sdl<void(*)(SDL_Window*,float,float)>("SDL_WarpMouseInWindow");if(real)real(window,x,y);
}
__attribute__((visibility("default"))) bool SDLCALL SDL_WarpMouseGlobal(float x,float y) {
    if(camera_overlay::InputMode())return true;
    static auto real=camera_overlay::Sdl<bool(*)(float,float)>("SDL_WarpMouseGlobal");return real && real(x,y);
}
__attribute__((visibility("default"))) SDL_MouseButtonFlags SDLCALL SDL_GetRelativeMouseState(float* x,float* y) {
    static auto real=camera_overlay::Sdl<SDL_MouseButtonFlags(*)(float*,float*)>("SDL_GetRelativeMouseState");
    auto flags=real?real(x,y):0;
    if(camera_overlay::InputMode()){if(x)*x=0;if(y)*y=0;return 0;}return flags;
}
__attribute__((visibility("default"))) SDL_MouseButtonFlags SDLCALL SDL_GetMouseState(float* x,float* y) {
    static auto real=camera_overlay::Sdl<SDL_MouseButtonFlags(*)(float*,float*)>("SDL_GetMouseState");
    auto flags=real?real(x,y):0;return camera_overlay::InputMode()?0:flags;
}
__attribute__((visibility("default"))) const bool* SDLCALL SDL_GetKeyboardState(int* count) {
    camera_overlay::RefreshKeyboard(camera_overlay::InputMode());
    if(count)*count=SDL_SCANCODE_COUNT;
    return camera_overlay::keyboard_state;
}
// resolver.S preserves the original return address when forwarding to libc.
// RTLD_NEXT must bypass our overrides so an SDL interposer can find its next link.
__attribute__((visibility("hidden"))) void* camera_libc_dlsym() noexcept {
    return reinterpret_cast<void*>(camera_overlay::LibcResolver());
}
__attribute__((visibility("hidden"))) void* camera_dlsym_override(void* library,const char* name) noexcept {
    if(library==RTLD_NEXT)return nullptr;
    const char* live=std::getenv("CS2_CAMERA_LIVE");
    if(live && std::strcmp(live,"1")==0) {
#define SDL_INTERCEPT(fn) if(std::strcmp(name,#fn)==0)return reinterpret_cast<void*>(fn);
        SDL_INTERCEPT(SDL_PollEvent) SDL_INTERCEPT(SDL_PumpEvents)
        SDL_INTERCEPT(SDL_SetWindowRelativeMouseMode) SDL_INTERCEPT(SDL_SetWindowMouseGrab)
        SDL_INTERCEPT(SDL_HideCursor) SDL_INTERCEPT(SDL_ShowCursor) SDL_INTERCEPT(SDL_WarpMouseInWindow)
        SDL_INTERCEPT(SDL_WarpMouseGlobal) SDL_INTERCEPT(SDL_GetRelativeMouseState)
        SDL_INTERCEPT(SDL_GetMouseState) SDL_INTERCEPT(SDL_GetKeyboardState)
#undef SDL_INTERCEPT
    }
    return nullptr;
}
__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CameraGetDeviceProcAddr(VkDevice device,const char* name) {
    if(std::strcmp(name,"vkGetDeviceProcAddr")==0)return reinterpret_cast<PFN_vkVoidFunction>(CameraGetDeviceProcAddr);
    if(auto fn=camera_overlay::Intercept(name))return fn;
    std::lock_guard<std::recursive_mutex> lock(camera_overlay::mutex);auto* d=camera_overlay::Find(device);return d?d->gdpa(device,name):nullptr;
}
__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CameraGetInstanceProcAddr(VkInstance instance,const char* name) {
    if(std::strcmp(name,"vkGetInstanceProcAddr")==0)return reinterpret_cast<PFN_vkVoidFunction>(CameraGetInstanceProcAddr);
    if(std::strcmp(name,"vkGetDeviceProcAddr")==0)return reinterpret_cast<PFN_vkVoidFunction>(CameraGetDeviceProcAddr);
    if(auto fn=camera_overlay::Intercept(name))return fn;
    std::lock_guard<std::recursive_mutex> lock(camera_overlay::mutex);auto* i=camera_overlay::Find(instance);return i?i->gipa(instance,name):nullptr;
}
__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if(!version || version->sType!=LAYER_NEGOTIATE_INTERFACE_STRUCT)return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion=std::min(version->loaderLayerInterfaceVersion,2u);
    version->pfnGetInstanceProcAddr=CameraGetInstanceProcAddr;version->pfnGetDeviceProcAddr=CameraGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr=nullptr;return VK_SUCCESS;
}
}
