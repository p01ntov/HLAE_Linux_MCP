#!/usr/bin/env python3
"""Exercise the built overlay against a fake SDL; no window or input is created."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

FAKE = r'''
#include <SDL3/SDL.h>
#include <cstdlib>
#include <pthread.h>
struct SDL_Window { unsigned id; SDL_WindowFlags flags; const SDL_DisplayMode* mode; };
static SDL_Window own{7,SDL_WINDOW_VULKAN|SDL_WINDOW_INPUT_FOCUS,reinterpret_cast<const SDL_DisplayMode*>(1)};
static SDL_Window foreign{8,0,nullptr};
static bool keys[SDL_SCANCODE_COUNT]={};
static pthread_t mainThread=pthread_self();static int counters[8]={};static void(*nestedPump)()=nullptr;
static void Video(){if(!pthread_equal(mainThread,pthread_self()))++counters[0];++counters[1];}
extern "C" {
bool SDL_IsMainThread(){return pthread_equal(mainThread,pthread_self());}
SDL_Window* SDL_GetWindowFromID(SDL_WindowID id){Video();return id==7?&own:id==8?&foreign:nullptr;}
SDL_WindowID SDL_GetWindowID(SDL_Window* w){Video();return w?w->id:0;}
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* w){Video();return w?w->flags:0;}
SDL_Window** SDL_GetWindows(int* count){Video();*count=2;auto p=static_cast<SDL_Window**>(std::malloc(sizeof(void*)*2));p[0]=&foreign;p[1]=&own;return p;}
void SDL_free(void* p){std::free(p);}
bool SDL_GetWindowSize(SDL_Window* w,int* x,int* y){Video();if(w!=&own)return false;*x=1280;*y=720;return true;}
const SDL_DisplayMode* SDL_GetWindowFullscreenMode(SDL_Window* w){Video();return w->mode;}
bool SDL_SetWindowFullscreenMode(SDL_Window* w,const SDL_DisplayMode* mode){Video();if(w!=&own || mode)std::abort();w->mode=mode;++counters[2];counters[4]=counters[4]*10+1;return true;}
bool SDL_SetWindowFullscreen(SDL_Window* w,bool enabled){Video();if(w!=&own || !enabled)std::abort();++counters[3];counters[4]=counters[4]*10+2;return true;}
bool SDL_PollEvent(SDL_Event* event){++counters[5];counters[7]=event==nullptr;if(nestedPump)nestedPump();return event!=nullptr;}
void SDL_PumpEvents(){++counters[6];}
bool SDL_SetWindowRelativeMouseMode(SDL_Window* w,bool enabled){Video();if(w!=&foreign || !enabled)std::abort();return true;}
const bool* SDL_GetKeyboardState(int* count){if(count)*count=SDL_SCANCODE_COUNT;return keys;}
void fake_ctrl(bool down){keys[SDL_SCANCODE_LCTRL]=down;}
int fake_ctrl_scancode(){return SDL_SCANCODE_LCTRL;}
int fake_f8_scancode(){return SDL_SCANCODE_F8;}
bool fake_key(bool(*filter)(void*,SDL_Event*),unsigned window,int scan,bool down,unsigned mod){SDL_Event e{};e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;e.key.windowID=window;e.key.scancode=SDL_Scancode(scan);e.key.mod=SDL_Keymod(mod);e.key.down=down;return filter(nullptr,&e);}
bool fake_focus(bool(*filter)(void*,SDL_Event*),unsigned window,bool focused){SDL_Event e{};e.type=focused?SDL_EVENT_WINDOW_FOCUS_GAINED:SDL_EVENT_WINDOW_FOCUS_LOST;e.window.windowID=window;return filter(nullptr,&e);}

int fake_counter(int i){return counters[i];}
void fake_confirm(){own.flags|=SDL_WINDOW_FULLSCREEN;}
void fake_set_nested(void(*p)()){nestedPump=p;}
SDL_Window* fake_foreign(){return &foreign;}
}
'''


def child(fake_path, overlay_path):
    fake = ctypes.CDLL(str(fake_path), mode=ctypes.RTLD_GLOBAL)
    overlay = ctypes.CDLL(str(overlay_path))
    fake.fake_counter.argtypes = [ctypes.c_int]
    fake.fake_counter.restype = ctypes.c_int
    fake.fake_set_nested.argtypes = [ctypes.c_void_p]
    fake.fake_set_nested(ctypes.cast(overlay.SDL_PumpEvents, ctypes.c_void_p))
    overlay.SDL_PollEvent.argtypes = [ctypes.c_void_p]
    overlay.SDL_PollEvent.restype = ctypes.c_bool
    # A worker may reach the interceptors, but no SDL video operation may run.
    worker = threading.Thread(target=lambda: overlay.SDL_PollEvent(None))
    worker.start()
    worker.join(5)
    assert not worker.is_alive()
    assert fake.fake_counter(0) == fake.fake_counter(1) == 0
    assert fake.fake_counter(5) == fake.fake_counter(6) == 1
    assert not overlay.SDL_PollEvent(None)
    assert fake.fake_counter(5) == fake.fake_counter(6) == 2
    assert fake.fake_counter(7) == 1  # PollEvent(NULL) forwarded unchanged.
    requested = os.environ["CS2_CAMERA_FULLSCREEN_WINDOWED"] == "1"
    assert fake.fake_counter(2) == fake.fake_counter(3) == int(requested)
    assert fake.fake_counter(4) == (12 if requested else 0)
    fake.fake_confirm()
    event = ctypes.create_string_buffer(128)
    assert overlay.SDL_PollEvent(event)
    assert fake.fake_counter(5) == fake.fake_counter(6) == 3
    assert fake.fake_counter(7) == 0
    assert fake.fake_counter(2) == fake.fake_counter(3) == int(requested)
    # An auxiliary-window grab request passes through without becoming ours.
    fake.fake_foreign.restype = ctypes.c_void_p
    overlay.SDL_SetWindowRelativeMouseMode.argtypes = [ctypes.c_void_p, ctypes.c_bool]
    overlay.SDL_SetWindowRelativeMouseMode.restype = ctypes.c_bool
    assert overlay.SDL_SetWindowRelativeMouseMode(fake.fake_foreign(), True)
    overlay.SDL_PumpEvents()
    assert fake.fake_counter(0) == 0
    # Source2 caches the returned pointer. Native state can change without a
    # filter event (SDL disabled-event and PeepEvents paths). Pump must refresh
    # that same array, including releases: never leave the game map held open.
    overlay.SDL_GetKeyboardState.argtypes = [ctypes.POINTER(ctypes.c_int)]
    overlay.SDL_GetKeyboardState.restype = ctypes.POINTER(ctypes.c_bool)
    fake.fake_ctrl.argtypes = [ctypes.c_bool]
    count = ctypes.c_int()
    cached = overlay.SDL_GetKeyboardState(ctypes.byref(count))
    assert count.value >= 512
    ctrl = fake.fake_ctrl_scancode()
    assert not cached[ctrl]
    fake.fake_ctrl(True)
    overlay.SDL_PumpEvents()
    assert cached[ctrl]
    fake.fake_ctrl(False)
    overlay.SDL_PumpEvents()
    assert not cached[ctrl]
    assert ctypes.addressof(cached.contents) == ctypes.addressof(overlay.SDL_GetKeyboardState(None).contents)
    # Exercise the production event filter in this isolated process. No SDL
    # window/input exists and none of these events reaches the desktop.
    symbols = {}
    for line in subprocess.check_output(["nm", "-an", str(overlay_path)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    base = ctypes.cast(overlay.SDL_PumpEvents, ctypes.c_void_p).value - symbols["SDL_PumpEvents"]
    def flag(name):
        return ctypes.c_bool.from_address(base + symbols[name])
    shown = flag("_ZN14camera_overlay5shownE")
    available = flag("_ZN14camera_overlay17overlay_availableE")
    installed = flag("_ZN14camera_overlay16filter_installedE")
    filt = base + symbols["_ZN14camera_overlay6FilterEPvP9SDL_Event"]
    fake.fake_key.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_bool, ctypes.c_uint]
    fake.fake_key.restype = ctypes.c_bool
    fake.fake_focus.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_bool]
    fake.fake_focus.restype = ctypes.c_bool
    shown.value = False
    available.value = installed.value = True
    assert fake.fake_key(filt, 7, ctrl, True, 0x0040) and cached[ctrl]
    assert fake.fake_key(filt, 7, ctrl, False, 0) and not cached[ctrl]
    # F8 Show -> modifier captured -> modifier release forwarded -> F8 Hide.
    f8 = fake.fake_f8_scancode()
    assert not fake.fake_key(filt, 7, f8, True, 0) and shown.value
    assert fake.fake_key(filt, 7, f8, False, 0)
    assert not fake.fake_key(filt, 7, ctrl, True, 0x0040) and not cached[ctrl]
    assert fake.fake_key(filt, 7, ctrl, False, 0) and not cached[ctrl]
    assert not fake.fake_key(filt, 7, f8, True, 0) and not shown.value
    assert fake.fake_key(filt, 7, f8, False, 0)
    assert fake.fake_key(filt, 7, ctrl, True, 0x0040) and cached[ctrl]
    assert fake.fake_focus(filt, 7, False) and not cached[ctrl]
    assert fake.fake_key(filt, 7, ctrl, True, 0x0040) and not cached[ctrl]
    assert fake.fake_key(filt, 8, ctrl, True, 0x0040) and not cached[ctrl]
    assert fake.fake_key(filt, 7, ctrl, False, 0) and not cached[ctrl]
    assert fake.fake_focus(filt, 7, True)
    # Leave the private test instance inert before unloading it.
    available.value = installed.value = False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--overlay", type=Path, default=Path(__file__).resolve().parents[3] / ".cache/cs2-overlay/libcs2_camera_overlay.so")
    parser.add_argument("--child", type=Path)
    args = parser.parse_args()
    if args.child:
        child(args.child, args.overlay)
        return
    with tempfile.TemporaryDirectory(prefix="cs2-overlay-fake-sdl-") as temporary:
        folder = Path(temporary)
        here = Path(__file__).resolve().parent
        imgui = here.parents[2] / ".cache/cs2-overlay/vendor/imgui"
        subprocess.run(["g++", "-std=c++17", "-I"+str(imgui), str(here / "overlay_imgui_input_test.cpp"),
                        *(str(imgui / name) for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp")),
                        "-o", str(folder / "imgui-test")], check=True)
        subprocess.run([str(folder / "imgui-test")], check=True, timeout=15)
        source, library = folder / "fake.cpp", folder / "libSDL3.so.0"
        source.write_text(FAKE)
        subprocess.run(["g++", "-std=c++17", "-shared", "-fPIC", "-pthread", "-I/usr/include/SDL3",
                        "-Wl,-soname,libSDL3.so.0", str(source), "-o", str(library)], check=True)
        for enabled in ("0", "1"):
            environment = os.environ.copy()
            environment.update({"CS2_CAMERA_LIVE": "1", "CS2_CAMERA_FULLSCREEN_WINDOWED": enabled})
            subprocess.run([sys.executable, str(Path(__file__).resolve()), "--overlay", str(args.overlay.resolve()),
                            "--child", str(library)], env=environment, check=True, timeout=15)
    print("Built SDL bridge: no off-main video calls, exact nested Poll/Pump ABI, own Vulkan window, fullscreen opt-out/readback and cached Ctrl press/release passed")


if __name__ == "__main__":
    main()
