#!/usr/bin/env python3
"""Reproduce an engine exporting the same weak std::string implementation."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
INCLUDES = ROOT / ".cache/csdm-live/upstream/cs2-server-plugin/cs2-server-plugin/deps/json/include"
os.environ["CS2_CAMERA_LIVE"] = "1"
with tempfile.TemporaryDirectory(prefix="cs2-interposition-") as temporary:
    directory = Path(temporary)
    poison = directory / "poison.cpp"
    poison.write_text('''
static int calls=0;
extern "C" void poison_dispose(void*) asm("_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE10_M_disposeEv");
extern "C" void poison_dispose(void*) { ++calls; }
extern "C" int poison_calls() { return calls; }
''')
    probe = directory / "probe.cpp"
    probe.write_text('''
#include "live_controller.h"
// Force this exact template body to exist and exercise its ELF binding. Some
// libstdc++ builds keep short-string destructors entirely inside libstdc++.
template void std::basic_string<char>::_M_dispose();
extern "C" void probe_dispose(void*) asm("_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE10_M_disposeEv");
extern "C" __attribute__((visibility("default"))) const char* probe_state() {
    static cs2_live::Controller controller;
    static thread_local std::string snapshot;
    std::string small="SSO"; probe_dispose(&small);
    snapshot=controller.StateJson(); return snapshot.c_str();
}
''')
    exports = directory / "exports.map"
    exports.write_text("{ global: probe_state; local: *; };\n")
    common = ["g++", "-std=c++17", "-shared", "-fPIC", "-pthread", f"-I{INCLUDES}", f"-I{HERE.parent}"]
    subprocess.run(common + [str(poison), "-o", str(directory / "poison.so")], check=True)
    subprocess.run(common + ["-fvisibility=hidden", "-fvisibility-inlines-hidden", "-fno-gnu-unique",
                            "-Wl,-Bsymbolic-functions", f"-Wl,--version-script={exports}",
                            str(probe), str(HERE.parent / "compat_math.cpp"), "-o", str(directory / "isolated.so")], check=True)
    subprocess.run(common + [str(probe), str(HERE.parent / "compat_math.cpp"), "-o", str(directory / "control.so")], check=True)
    engine = ctypes.CDLL(str(directory / "poison.so"), mode=ctypes.RTLD_GLOBAL)
    engine.poison_calls.restype = ctypes.c_int
    isolated = ctypes.CDLL(str(directory / "isolated.so"))
    isolated.probe_state.restype = ctypes.c_char_p
    for _ in range(20):
        assert json.loads(isolated.probe_state())["connected"] is True
    assert engine.poison_calls() == 0, "isolated JSON used the engine's incompatible string destructor"
    control = ctypes.CDLL(str(directory / "control.so"))
    control.probe_state.restype = ctypes.c_char_p
    assert json.loads(control.probe_state())["connected"] is True
    assert engine.poison_calls() > 0, "test setup failed to reproduce weak-symbol interposition"
    print("ELF isolation: JSON ignores hostile engine string destructor; unisolated control reproduces interposition")
