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
#include <unordered_map>
#include <vector>
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "editor_ui.h"
#include "cosmetics_catalog.h"
#include "overlay_blur.h"

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
    bool ready=false, failed=false, blur_supported=false;
    cs2_camera_blur::Backdrop blur;
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
    DEVICE_FN(AllocateCommandBuffers) DEVICE_FN(ResetCommandBuffer) DEVICE_FN(BeginCommandBuffer) DEVICE_FN(EndCommandBuffer)
    DEVICE_FN(CmdBeginRenderPass) DEVICE_FN(CmdEndRenderPass)
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
std::atomic<bool> relative_requested{false},grab_requested{false},cursor_hidden_requested{false};
std::atomic<bool> filter_installed{false}, overlay_available{false};
bool InputMode() { return shown.load() && filter_installed.load() && overlay_available.load(); }
SDL_EventFilter previous_filter=nullptr;
void* previous_filter_data=nullptr;
std::mutex filter_mutex;
cs2_live_ui::EditorUi editor;
using StateFn=const char*(*)();
using CommandFn=void(*)(const char*);
StateFn state_fn=nullptr; CommandFn command_fn=nullptr;
std::atomic<bool> bridge_ready{false};
template<class T> T Sdl(const char* name);
void CursorMode(bool capture);
std::atomic<bool> text_input{false};
// SDL promises a lifetime-stable keyboard array. Source 2 caches this pointer;
// swapping arrays when opening the editor would leave roaming controls stuck.
std::mutex keyboard_mutex;
bool keyboard_state[SDL_SCANCODE_COUNT]={};
bool physical_keys[SDL_SCANCODE_COUNT]={};
void KeyboardMode(bool capture) {
    std::lock_guard<std::mutex> lock(keyboard_mutex);
    for(int i=0;i<SDL_SCANCODE_COUNT;++i)keyboard_state[i]=!capture && physical_keys[i];
}
struct Hotkey { std::string name; SDL_Scancode scan; SDL_Keymod modifiers; };
std::unordered_map<std::string,Hotkey> hotkeys={
    {"capture",{"F6",SDL_SCANCODE_F6,SDL_KMOD_NONE}},
    {"camera",{"F7",SDL_SCANCODE_F7,SDL_KMOD_NONE}},
    {"editor",{"F8",SDL_SCANCODE_F8,SDL_KMOD_NONE}},
    {"preview",{"F9",SDL_SCANCODE_F9,SDL_KMOD_NONE}}
};
std::string hotkeys_path;
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
        auto scan_name=Sdl<SDL_Scancode(*)(const char*)>("SDL_GetScancodeFromName");
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
    if(Match(event,hotkeys.at("editor"))) {shown=!shown.load();KeyboardMode(InputMode());CursorMode(InputMode());return true;}
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
    static std::mutex cursor_mutex;
    std::lock_guard<std::mutex> lock(cursor_mutex);
    static bool held=false, previous_relative=false, previous_grab=false, previous_cursor=true, previous_text=false;
    auto window=game_window.load();
    auto relative=Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowRelativeMouseMode");
    auto grab=Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowMouseGrab");
    auto show=Sdl<bool(*)()>("SDL_ShowCursor");
    auto hide=Sdl<bool(*)()>("SDL_HideCursor");
    if(capture && window) {
        if(!held) {
            auto get_relative=Sdl<bool(*)(SDL_Window*)>("SDL_GetWindowRelativeMouseMode");
            auto get_grab=Sdl<bool(*)(SDL_Window*)>("SDL_GetWindowMouseGrab");
            auto get_cursor=Sdl<bool(*)()>("SDL_CursorVisible");
            auto get_text=Sdl<bool(*)(SDL_Window*)>("SDL_TextInputActive");
            previous_relative=get_relative && get_relative(window);previous_grab=get_grab && get_grab(window);previous_cursor=!get_cursor || get_cursor();held=true;
            previous_text=get_text && get_text(window);
        }
        if(relative)relative(window,false);if(grab)grab(window,false);if(show)show();
        auto start_text=Sdl<bool(*)(SDL_Window*)>("SDL_StartTextInput");if(start_text)start_text(window);
    }else if(held && window) {
        if(relative)relative(window,relative_requested.load() || previous_relative);
        if(grab)grab(window,grab_requested.load() || previous_grab);
        if(previous_cursor && !cursor_hidden_requested){if(show)show();}else if(hide)hide();held=false;
        auto stop_text=Sdl<bool(*)(SDL_Window*)>("SDL_StopTextInput");if(!previous_text && stop_text)stop_text(window);
    }
}
bool SDLCALL Filter(void*, SDL_Event* event) {
    if (!event) return true;
    static thread_local bool filtering=false;
    if(filtering)return true;
    struct Guard { bool& value; Guard(bool& v):value(v){value=true;} ~Guard(){value=false;} } guard(filtering);
    const bool keyboard=event->type==SDL_EVENT_KEY_DOWN || event->type==SDL_EVENT_KEY_UP || event->type==SDL_EVENT_TEXT_INPUT;
    const bool mouse=event->type>=SDL_EVENT_MOUSE_MOTION && event->type<=SDL_EVENT_MOUSE_WHEEL;
    if ((keyboard || mouse) && !game_window.load()) {
        SDL_WindowID id=keyboard ? event->key.windowID : event->motion.windowID;
        auto from_id=Sdl<SDL_Window*(*)(SDL_WindowID)>("SDL_GetWindowFromID");
        if(from_id && id)game_window=from_id(id);
    }
    if(event->type==SDL_EVENT_KEY_DOWN || event->type==SDL_EVENT_KEY_UP) {
        const auto scan=event->key.scancode;
        if(scan>=0 && scan<SDL_SCANCODE_COUNT) {
            std::lock_guard<std::mutex> lock(keyboard_mutex);
            physical_keys[scan]=event->type==SDL_EVENT_KEY_DOWN;
            keyboard_state[scan]=!InputMode() && physical_keys[scan];
        }
    }
    if(event->type==SDL_EVENT_KEY_DOWN && Shortcut(event->key))return false;
    if(keyboard || mouse) CursorMode(InputMode());
    {
        std::lock_guard<std::mutex> lock(events_mutex);
        if(events.size()<2048)events.push_back({*event,event->type==SDL_EVENT_TEXT_INPUT && event->text.text ? event->text.text : ""});
    }
    const bool consume=InputMode() && ((mouse && wants_mouse) || (keyboard && wants_keyboard));
    // Releases must reach the engine even if their presses preceded F8.
    if(consume && event->type!=SDL_EVENT_KEY_UP && event->type!=SDL_EVENT_MOUSE_BUTTON_UP)return false;
    SDL_EventFilter next=nullptr;void* data=nullptr;
    {std::lock_guard<std::mutex> lock(filter_mutex);next=previous_filter;data=previous_filter_data;}
    return next ? next(data,event) : true;
}
void InstallFilter() {
    auto set=Sdl<void(*)(SDL_EventFilter,void*)>("SDL_SetEventFilter");
    auto get=Sdl<bool(*)(SDL_EventFilter*,void**)>("SDL_GetEventFilter");
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
    auto set=Sdl<void(*)(SDL_EventFilter,void*)>("SDL_SetEventFilter");
    SDL_EventFilter next=nullptr;void* data=nullptr;
    {std::lock_guard<std::mutex> lock(filter_mutex);next=previous_filter;data=previous_filter_data;}
    if(set)set(next,data);
    filter_installed=false;
}
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
void Input(ImGuiIO& io) {
    std::deque<InputEvent> pending;
    {std::lock_guard<std::mutex> lock(events_mutex);pending.swap(events);}
    for(const auto& item:pending) {
        const auto& event=item.event;
        switch(event.type) {
        case SDL_EVENT_MOUSE_MOTION: io.AddMousePosEvent(event.motion.x,event.motion.y);break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int button=-1;
            if(event.button.button==SDL_BUTTON_LEFT)button=0;
            if(event.button.button==SDL_BUTTON_RIGHT)button=1;
            if(event.button.button==SDL_BUTTON_MIDDLE)button=2;
            if(button>=0)io.AddMouseButtonEvent(button,event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
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
        state_fn=reinterpret_cast<StateFn>(dlsym(library,"cs2_live_state_json"));
        command_fn=reinterpret_cast<CommandFn>(dlsym(library,"cs2_live_command_json"));
        dlclose(library);
    }
    if(state_fn && command_fn){bridge_ready=true;return 1;}
    return 0;
}
bool Draw(Swapchain& s,uint32_t image_index) {
    const bool frame_shown=shown.load();
    InstallFilter();
    if(!bridge_ready.load())dl_iterate_phdr(FindBridge,nullptr);
    auto& io=ImGui::GetIO();
    io.DisplaySize=ImVec2(float(s.extent.width),float(s.extent.height));
    io.DisplayFramebufferScale=ImVec2(1,1);
    const auto now=std::chrono::steady_clock::now();
    io.DeltaTime=previous_frame.time_since_epoch().count() ? std::chrono::duration<float>(now-previous_frame).count() : 1.f/60.f;
    if(io.DeltaTime<=0 || io.DeltaTime>1)io.DeltaTime=1.f/60.f;
    previous_frame=now;
    Input(io);
    ImGui_ImplVulkan_NewFrame(); ImGui::NewFrame();
    nlohmann::json state={{"connected",false},{"demo",false},{"tick",0},{"tick_rate",64},{"paused",true},{"mode","free"},
      {"camera",{{"position",{0,0,0}},{"angles",{0,0,0}},{"fov",90}}},
      {"project",{{"name","Untitled"},{"start_tick",0},{"duration",5},{"keys",nlohmann::json::array()}}},
      {"cursor",0},{"preview_playing",false},{"revision",0},{"error","Waiting for live camera module"}};
    if(state_fn) {try{state=nlohmann::json::parse(state_fn());}catch(...){state["error"]="Invalid camera state";}}
    state["hotkeys"]={{"capture",hotkeys.at("capture").name},{"camera",hotkeys.at("camera").name},
        {"editor",hotkeys.at("editor").name},{"preview",hotkeys.at("preview").name},{"config_path",hotkeys_path}};
    editor.backdrop_texture=static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(s.blur.Texture(image_index)));
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
    io.MouseDrawCursor=shown;
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
void Cleanup(Swapchain& s) {
    auto& d=*s.device;
    if(active==&s){overlay_available=false;KeyboardMode(false);}
    if(s.ready || s.pool)d.DeviceWaitIdle(d.handle);
    s.blur.Destroy();
    if(active==&s) {if(ImGui::GetCurrentContext()){ImGui_ImplVulkan_Shutdown();ImGui::DestroyContext();}active=nullptr;}
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
        if(!Check(d.AllocateCommandBuffers(d.handle,&alloc,&f.cmd),"command buffer"))return false;
        if(d.set_loader_data)d.set_loader_data(d.handle,f.cmd);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if(!Check(d.CreateFence(d.handle,&fence,nullptr,&f.fence),"fence") || !Check(d.CreateSemaphore(d.handle,&sem,nullptr,&f.complete),"semaphore"))return false;
    }
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    LoadCosmeticsCatalog();
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
    if(!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1,[](const char* name,void* opaque)->PFN_vkVoidFunction{
        auto* dev=static_cast<Device*>(opaque);auto fn=dev->gdpa(dev->handle,name);
        return fn ? fn : dev->instance->gipa(dev->instance->handle,name);
    },&d)) {ImGui::DestroyContext();return false;}
    ImGui_ImplVulkan_InitInfo init{};init.ApiVersion=VK_API_VERSION_1_1;init.Instance=d.instance->handle;init.PhysicalDevice=d.physical;init.Device=d.handle;
    init.QueueFamily=family;init.Queue=queue;init.RenderPass=s.pass;init.MinImageCount=std::min(count,std::max(2u,s.min_images));init.ImageCount=count;init.MSAASamples=VK_SAMPLE_COUNT_1_BIT;init.DescriptorPoolSize=std::max(16u,count+2);
    if(!ImGui_ImplVulkan_Init(&init)){ImGui::DestroyContext();return false;}
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
        LOAD(CreateCommandPool) LOAD(DestroyCommandPool) LOAD(AllocateCommandBuffers) LOAD(ResetCommandBuffer) LOAD(BeginCommandBuffer) LOAD(EndCommandBuffer)
        LOAD(CmdBeginRenderPass) LOAD(CmdEndRenderPass) LOAD(CreateFence) LOAD(DestroyFence) LOAD(WaitForFences) LOAD(ResetFences) LOAD(CreateSemaphore) LOAD(DestroySemaphore)
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
        auto s=std::make_unique<Swapchain>();s->device=d;s->handle=*output;s->format=ci->imageFormat;s->extent=ci->imageExtent;s->min_images=ci->minImageCount;s->blur_supported=backdrop;
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
        if(family->second>=count || !(families[family->second].queueFlags&VK_QUEUE_GRAPHICS_BIT) || !Setup(*s,queue,family->second)) {Cleanup(*s);s->failed=true;Log("overlay disabled for unsupported swapchain");return d->QueuePresentKHR(queue,present);}
    }
    if(image_index>=s->frames.size())return d->QueuePresentKHR(queue,present);
    auto& frame=s->frames[image_index];
    if(!Check(d->WaitForFences(d->handle,1,&frame.fence,VK_TRUE,1000000000ull),"wait frame"))return d->QueuePresentKHR(queue,present);
    const bool panels_drawn=Draw(*s,image_index);
    auto* draw=ImGui::GetDrawData();
    if(!draw || !draw->TotalVtxCount)return d->QueuePresentKHR(queue,present);
    d->ResetCommandBuffer(frame.cmd,0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(d->BeginCommandBuffer(frame.cmd,&begin)!=VK_SUCCESS)return d->QueuePresentKHR(queue,present);
    if(panels_drawn && s->blur.Ready())s->blur.Record(frame.cmd,image_index,frame.image);
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=s->pass;pass.framebuffer=frame.framebuffer;pass.renderArea.extent=s->extent;
    d->CmdBeginRenderPass(frame.cmd,&pass,VK_SUBPASS_CONTENTS_INLINE);ImGui_ImplVulkan_RenderDrawData(draw,frame.cmd);d->CmdEndRenderPass(frame.cmd);
    if(d->EndCommandBuffer(frame.cmd)!=VK_SUCCESS){s->failed=true;overlay_available=false;KeyboardMode(false);return d->QueuePresentKHR(queue,present);}
    std::vector<VkPipelineStageFlags> stages(present->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=present->waitSemaphoreCount;submit.pWaitSemaphores=present->pWaitSemaphores;submit.pWaitDstStageMask=stages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&frame.cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&frame.complete;
    d->ResetFences(d->handle,1,&frame.fence);
    if(!Check(d->QueueSubmit(queue,1,&submit,frame.fence),"submit editor")){s->failed=true;overlay_available=false;KeyboardMode(false);return d->QueuePresentKHR(queue,present);}
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
__attribute__((visibility("default"))) bool SDLCALL SDL_SetWindowRelativeMouseMode(SDL_Window* window,bool enabled) {
    camera_overlay::game_window=window;
    camera_overlay::relative_requested=enabled;
    auto real=camera_overlay::Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowRelativeMouseMode");
    return real && real(window,camera_overlay::InputMode()?false:enabled);
}
__attribute__((visibility("default"))) bool SDLCALL SDL_SetWindowMouseGrab(SDL_Window* window,bool enabled) {
    camera_overlay::game_window=window;
    camera_overlay::grab_requested=enabled;
    auto real=camera_overlay::Sdl<bool(*)(SDL_Window*,bool)>("SDL_SetWindowMouseGrab");
    return real && real(window,camera_overlay::InputMode()?false:enabled);
}
__attribute__((visibility("default"))) bool SDLCALL SDL_HideCursor() {
    camera_overlay::cursor_hidden_requested=true;
    auto real=camera_overlay::Sdl<bool(*)()>(camera_overlay::InputMode()?"SDL_ShowCursor":"SDL_HideCursor");
    return real && real();
}
__attribute__((visibility("default"))) void SDLCALL SDL_WarpMouseInWindow(SDL_Window* window,float x,float y) {
    if(camera_overlay::InputMode())return;
    auto real=camera_overlay::Sdl<void(*)(SDL_Window*,float,float)>("SDL_WarpMouseInWindow");if(real)real(window,x,y);
}
__attribute__((visibility("default"))) bool SDLCALL SDL_WarpMouseGlobal(float x,float y) {
    if(camera_overlay::InputMode())return true;
    auto real=camera_overlay::Sdl<bool(*)(float,float)>("SDL_WarpMouseGlobal");return real && real(x,y);
}
__attribute__((visibility("default"))) SDL_MouseButtonFlags SDLCALL SDL_GetRelativeMouseState(float* x,float* y) {
    auto real=camera_overlay::Sdl<SDL_MouseButtonFlags(*)(float*,float*)>("SDL_GetRelativeMouseState");
    auto flags=real?real(x,y):0;
    if(camera_overlay::InputMode()){if(x)*x=0;if(y)*y=0;return 0;}return flags;
}
__attribute__((visibility("default"))) SDL_MouseButtonFlags SDLCALL SDL_GetMouseState(float* x,float* y) {
    auto real=camera_overlay::Sdl<SDL_MouseButtonFlags(*)(float*,float*)>("SDL_GetMouseState");
    auto flags=real?real(x,y):0;return camera_overlay::InputMode()?0:flags;
}
__attribute__((visibility("default"))) const bool* SDLCALL SDL_GetKeyboardState(int* count) {
    auto real=camera_overlay::Sdl<const bool*(*)(int*)>("SDL_GetKeyboardState");
    int size=0;const bool* state=real?real(&size):nullptr;
    {
        std::lock_guard<std::mutex> lock(camera_overlay::keyboard_mutex);
        if(state)for(int i=0;i<std::min(size,int(SDL_SCANCODE_COUNT));++i)
            camera_overlay::physical_keys[i]=state[i];
        for(int i=0;i<SDL_SCANCODE_COUNT;++i)
            camera_overlay::keyboard_state[i]=!camera_overlay::InputMode() && camera_overlay::physical_keys[i];
    }
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
        SDL_INTERCEPT(SDL_SetWindowRelativeMouseMode) SDL_INTERCEPT(SDL_SetWindowMouseGrab)
        SDL_INTERCEPT(SDL_HideCursor) SDL_INTERCEPT(SDL_WarpMouseInWindow)
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
