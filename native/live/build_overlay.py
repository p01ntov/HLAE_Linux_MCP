#!/usr/bin/env python3
"""Build a private, relocatable Vulkan layer for the CS2 Camera live editor."""
from __future__ import annotations
import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request
import fcntl

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCES = {
    "imgui": ("https://codeload.github.com/ocornut/imgui/tar.gz/f5befd2d29e66809cd1110a152e375a7f1981f06", "85f4ce357df05bcc331b587f01976f47fb55f19fadf477a7907289686bc3f4c8"),
    "vulkan": ("https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/b379292b2ab6df5771ba9870d53cf8b2c9295daf", "2ad177c0e468ac2eb3db9b5be6b8a18a6730d339378ac670e5fbda3503ac7d64"),
    "json": ("https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp", "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6"),
}

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def vendor(cache: Path) -> None:
    for name, (url, expected) in SOURCES.items():
        destination=cache / "vendor" / name
        marker=destination / ".source-sha256"
        if marker.is_file() and marker.read_text().strip()==expected:
            continue
        data=urllib.request.urlopen(url, timeout=60).read()
        actual=hashlib.sha256(data).hexdigest()
        if actual!=expected:
            raise RuntimeError(f"{name} source checksum mismatch: {actual}")
        destination.mkdir(parents=True,exist_ok=True)
        if name=="json":
            (destination / "nlohmann").mkdir(exist_ok=True)
            (destination / "nlohmann/json.hpp").write_bytes(data)
        else:
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                for member in archive.getmembers():
                    parts=Path(member.name).parts[1:]
                    if not parts: continue
                    member.name=str(Path(*parts))
                    if member.issym() or member.islnk():
                        raise RuntimeError("Unexpected symlink in dependency archive")
                    if ".." in parts:raise RuntimeError("Invalid archive path")
                    archive.extract(member,destination,filter="data")
        marker.write_text(expected+"\n")

def build(cache: Path | None=None) -> Path:
    if platform.system()!="Linux" or platform.machine().lower() not in {"x86_64","amd64"}:
        raise RuntimeError("The live overlay currently requires Linux x86-64 (AppImage target)")
    cache=(cache or ROOT / ".cache/cs2-overlay").resolve()
    cache.mkdir(parents=True,exist_ok=True)
    with (cache / "build.lock").open("a") as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        if not (HERE / "editor_ui.h").is_file():raise RuntimeError("editor_ui.h is required")
        fingerprint=hashlib.sha256((sha(HERE/"overlay.cpp")+sha(HERE/"editor_ui.h")+sha(HERE/"overlay_blur.h")+sha(HERE/"overlay_blur_spv.h")+sha(HERE/"compat_math.cpp")+sha(HERE/"resolver.S")+sha(HERE/"overlay_exports.map")+sha(Path(__file__))+json.dumps(SOURCES)).encode()).hexdigest()
        library=cache / "libcs2_camera_overlay.so"
        manifest=cache / "build.json"
        if library.is_file() and manifest.is_file():
            data=json.loads(manifest.read_text())
            if data.get("source_fingerprint")==fingerprint and data.get("sha256")==sha(library):return library
        vendor(cache)
        imgui=cache / "vendor/imgui"
        sources=[HERE/"overlay.cpp",HERE/"compat_math.cpp",HERE/"resolver.S",*(imgui / name for name in ("imgui.cpp","imgui_draw.cpp","imgui_widgets.cpp","imgui_tables.cpp")),imgui/"backends/imgui_impl_vulkan.cpp"]
        flags=["g++","-m64","-std=c++17","-shared","-fPIC","-O2","-fvisibility=hidden","-fvisibility-inlines-hidden","-static-libstdc++","-static-libgcc","-Wl,--exclude-libs,ALL","-Wl,-Bsymbolic-functions","-Wl,--version-script="+str(HERE/"overlay_exports.map"),"-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES","-I"+str(imgui),"-I"+str(imgui/"backends"),"-I"+str(cache/"vendor/vulkan/include"),"-I"+str(cache/"vendor/json"),"-I/usr/include/SDL3",*[str(p) for p in sources],"-ldl","-pthread","-o",str(library.with_suffix(".tmp.so"))]
        subprocess.run(flags,check=True)
        library.with_suffix(".tmp.so").replace(library)
        layer={"file_format_version":"1.2.0","layer":{"name":"VK_LAYER_CS2_camera","type":"GLOBAL","library_path":"./libcs2_camera_overlay.so","api_version":"1.3.0","implementation_version":"1","description":"CS2 Camera live cinematic editor","functions":{"vkGetInstanceProcAddr":"CameraGetInstanceProcAddr","vkGetDeviceProcAddr":"CameraGetDeviceProcAddr"}}}
        (cache / "VkLayer_cs2_camera.json").write_text(json.dumps(layer,indent=2)+"\n")
        manifest.write_text(json.dumps({"source_fingerprint":fingerprint,"sha256":sha(library),"sources":SOURCES},indent=2)+"\n")
        return library

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir",type=Path)
    args=parser.parse_args()
    print(build(args.cache_dir))
