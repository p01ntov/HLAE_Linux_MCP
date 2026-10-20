#!/usr/bin/env python3
"""Exercise the actual preloaded SDL resolver without opening a game/window."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[3]
overlay=root / ".cache/cs2-overlay/libcs2_camera_overlay.so"
code='''
import ctypes
process=ctypes.CDLL(None)
process.strlen.argtypes=[ctypes.c_char_p]
process.strlen.restype=ctypes.c_size_t
assert process.strlen(b"native resolver") == 15
sdl=ctypes.CDLL("libSDL3.so.0")
# Source 2 uses per-module dlsym lookup. It must receive the input adapter,
# while unrelated lookups remain real and the resolver never calls itself.
for name in ["SDL_HideCursor","SDL_SetWindowRelativeMouseMode","SDL_WarpMouseInWindow"]:
    assert ctypes.cast(getattr(sdl,name),ctypes.c_void_p).value == ctypes.cast(getattr(process,name),ctypes.c_void_p).value
print("Overlay resolver: real libc lookup and scoped SDL input interception OK")
'''
env=dict(os.environ,LD_PRELOAD=str(overlay),CS2_CAMERA_LIVE="1")
subprocess.run([sys.executable,"-c",code],env=env,check=True,timeout=10)

# dlsym(RTLD_NEXT, ...) must retain the original caller's lookup origin.
# A normal C++ forwarding function makes libc see the overlay as the caller,
# causing the middle interposer to find itself instead of the following DSO.
with tempfile.TemporaryDirectory(prefix="cs2-overlay-resolver-") as temporary:
    directory=Path(temporary)
    sources={
        "middle": '''
#include <dlfcn.h>
extern "C" int overlay_resolver_chain_target() { return 22; }
extern "C" int overlay_resolver_chain_probe() {
    auto next=reinterpret_cast<int(*)()>(dlsym(RTLD_NEXT,"overlay_resolver_chain_target"));
    return next ? next() : -1;
}
''',
        "leaf": '''
extern "C" int overlay_resolver_chain_target() { return 11; }
''',
        "probe": '''
#include <dlfcn.h>
#include <cstdio>
int main() {
    auto probe=reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT,"overlay_resolver_chain_probe"));
    std::printf("%d\\n",probe ? probe() : -2);
}
''',
    }
    binaries={}
    for name,source in sources.items():
        binary=directory / (name if name=="probe" else name+".so")
        command=["g++","-x","c++","-"]
        if name!="probe":
            command += ["-shared","-fPIC"]
        command += ["-ldl","-o",str(binary)]
        subprocess.run(command,input=source,text=True,check=True,timeout=30)
        binaries[name]=binary
    for use_overlay in (False,True):
        libraries=([overlay] if use_overlay else [])+[binaries["middle"],binaries["leaf"]]
        chain_env=dict(os.environ,LD_PRELOAD=":".join(map(str,libraries)),CS2_CAMERA_LIVE="1")
        result=subprocess.run([str(binaries["probe"])],env=chain_env,text=True,
                              capture_output=True,check=True,timeout=10)
        expected="11"
        assert result.stdout.strip()==expected, (
            f"RTLD_NEXT lookup {'with overlay' if use_overlay else 'baseline'} "
            f"returned {result.stdout.strip()!r}; expected next-library value {expected}. "
            "The resolver must preserve the calling DSO, rather than resolve from the overlay."
        )
print("Overlay resolver: RTLD_NEXT keeps the original interposer's lookup origin OK")
