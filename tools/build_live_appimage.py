#!/usr/bin/env python3
"""Build the Linux Camera Studio AppImage; never launch or alter CS2.

Requires the already built native controller and Vulkan overlay. The finished
AppImage bundles Python, Qt and the native libraries, not Steam or the game.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
APPIMAGETOOL_VERSION = "1.9.1"
APPIMAGETOOL_URL = f"https://github.com/AppImage/appimagetool/releases/download/{APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256 = "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command, **kwargs):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def ensure_appimagetool(cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / f"appimagetool-{APPIMAGETOOL_VERSION}-x86_64.AppImage"
    if not destination.is_file() or sha256(destination) != APPIMAGETOOL_SHA256:
        temporary = destination.with_suffix(".download")
        try:
            with urllib.request.urlopen(APPIMAGETOOL_URL, timeout=60) as source, temporary.open("wb") as output:
                shutil.copyfileobj(source, output)
            if sha256(temporary) != APPIMAGETOOL_SHA256:
                raise RuntimeError("Official appimagetool download checksum mismatch")
            temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)
    destination.chmod(0o755)
    return destination


def require_native(path: Path):
    if not path.is_file() or path.read_bytes()[:6] != b"\x7fELF\x02\x01":
        raise RuntimeError(f"Build the ELF64 native library first: {path}")


def copy_vendored_licenses(target: Path):
    """Keep release-pinned notices even when upstream wheels omit license files."""
    root = ROOT / "packaging/licenses"
    manifest = json.loads((root / "provenance.json").read_text())
    if manifest.get("format") != 1:
        raise RuntimeError("Unsupported vendored license provenance format")
    for source in manifest["sources"]:
        for name, expected in source["distribution_versions"].items():
            actual = importlib.metadata.version(name)
            if actual != expected:
                raise RuntimeError(f"Refresh pinned license sources for {name}: expected {expected}, found {actual}")
    verified = []
    for item in manifest["files"]:
        relative = Path(item["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError("Invalid vendored license path")
        source = root / relative
        if not source.is_file() or sha256(source) != item["sha256"]:
            raise RuntimeError(f"Pinned license checksum mismatch: {relative}")
        verified.append((source, target / "upstream" / relative))
    for source, destination in verified:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    for name in ("NOTICE.md", "provenance.json"):
        shutil.copy2(root / name, target / name)
    distributions = []
    for name in ("PySide6-Essentials", "shiboken6", "demoparser2", "PyInstaller"):
        distribution = importlib.metadata.distribution(name)
        distributions.append({"name": name, "version": distribution.version,
                              "license": distribution.metadata.get("License"),
                              "license_expression": distribution.metadata.get("License-Expression"),
                              "project_urls": distribution.metadata.get_all("Project-URL", [])})
    (target / "bundled-distributions.json").write_text(json.dumps({
        "python": platform.python_version(), "platform": platform.platform(),
        "distributions": distributions,
        "qt_linking": "Shared libraries in usr/bin/_internal/PySide6/Qt/lib; see NOTICE.md",
    }, indent=2) + "\n")


def copy_licenses(target: Path):
    target.mkdir(parents=True, exist_ok=True)
    copy_vendored_licenses(target)
    for name in ("PySide6-Essentials", "shiboken6", "PyInstaller", "demoparser2", "numpy", "pandas", "polars", "polars-runtime-32", "pyarrow", "tqdm"):
        try:
            distribution = importlib.metadata.distribution(name)
        except importlib.metadata.PackageNotFoundError:
            continue
        for relative in distribution.files or []:
            if any(part.lower() in {"licenses", "license", "copying"} or part.lower().startswith(("license.", "copying.")) for part in relative.parts):
                source = distribution.locate_file(relative)
                if source.is_file():
                    destination = target / name / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
    # Preserve the actual upstream licenses alongside bundled native binaries.
    candidates = [(ROOT / ".cache/csdm-live/UPSTREAM-LICENSE", "CSDM-LICENSE"),
                  (ROOT / ".cache/csdm-live/upstream/cs2-server-plugin/cs2-server-plugin/deps/json/LICENSE.MIT", "nlohmann-json-LICENSE"),
                  (ROOT / ".cache/csdm-live/upstream/cs2-server-plugin/cs2-server-plugin/deps/easywsclient/COPYING", "easywsclient-COPYING"),
                  (ROOT / ".cache/csdm-motion/UPSTREAM-LICENSE", "CSDM-LICENSE"),
                  (ROOT / ".cache/csdm-motion-capture/UPSTREAM-LICENSE", "CSDM-capture-LICENSE"),
                  (ROOT / ".cache/cs2-overlay/vendor/imgui/LICENSE.txt", "Dear-ImGui-LICENSE"),
                  (ROOT / ".cache/cs2-overlay/vendor/vulkan/LICENSES/MIT.txt", "Vulkan-Headers-MIT"),
                  (ROOT / ".cache/cs2-overlay/vendor/vulkan/LICENSES/Apache-2.0.txt", "Vulkan-Headers-Apache-2.0"),
                  (ROOT / "packaging/live/fonts/OFL.txt", "NotoSans-OFL.txt"),
                  (ROOT / "packaging/live/fonts/source.json", "NotoSans-source.json")]
    for source, name in candidates:
        if source.is_file() and not (target / name).exists():
            shutil.copy2(source, target / name)


def prepare_capture_assets(cache: Path) -> Path:
    """Stage verified, already-built capture binaries; never build in the app."""
    source = ROOT / ".cache/csdm-motion-capture"
    capture = source / "libserver.so"
    require_native(capture)
    manifest = json.loads((source / "build.json").read_text())
    if (manifest.get("motion_abi") != 3 or manifest.get("sha256") != sha256(capture)
            or b"CS2_MVP_MOTION_ABI 3" not in capture.read_bytes() or b"CS2_MVP_ENVIRONMENT_ABI 1" not in capture.read_bytes()):
        raise RuntimeError("Build and verify the ABI 3 capture plugin before packaging")
    commit = "7270bc95c8cab87b7f9ddd490fea13c6eda3ace7"
    stock = ROOT / ".cache/csdm" / commit / "libserver.so"
    require_native(stock)
    stock_hash = "c6bab67a072f83d7ec8e57c1fe243179e267bd9631cb4ee2d9cbf192cea86780"
    if manifest.get("upstream_commit") != commit or sha256(stock) != stock_hash:
        raise RuntimeError("Capture/stock plugin upstream pins do not match")
    assets = cache / "capture-assets"
    assets.mkdir(parents=True, exist_ok=True)
    shutil.copy2(capture, assets / "libcamera_capture.so")
    shutil.copy2(stock, assets / "libserver_stock.so")
    value = {"version": 1, "motion_abi": 3, "environment_abi": 1, "upstream_commit": commit,
             "dependencies": manifest["dependencies"], "capture_sha256": sha256(capture),
             "stock_sha256": stock_hash, "source_fingerprint": manifest["source_fingerprint"]}
    (assets / "plugins.json").write_text(json.dumps(value, indent=2) + "\n")
    return assets


def build(plugin: Path, overlay: Path, output: Path, *, appdir_only=False) -> Path:
    if platform.system() != "Linux" or platform.machine() not in {"x86_64", "AMD64"}:
        raise RuntimeError("This prototype builds Linux x86_64 AppImages only")
    require_native(plugin)
    require_native(overlay)
    manifest = overlay.parent / "VkLayer_cs2_camera.json"
    layer = json.loads(manifest.read_text())
    if layer.get("layer", {}).get("name") != "VK_LAYER_CS2_camera":
        raise RuntimeError("Unexpected Vulkan layer manifest")
    cache = ROOT / ".cache/live-appimage"
    appdir = cache / "CS2CameraStudio.AppDir"
    distribution = cache / "freeze"
    capture_assets = prepare_capture_assets(cache)
    run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir",
         "--name", "cs2-camera-studio", "--paths", ROOT / "src",
         "--hidden-import", "cs2_mvp.cli", "--hidden-import", "cs2_mvp.live_track", "--hidden-import", "pandas",
         "--hidden-import", "polars", "--hidden-import", "pyarrow", "--hidden-import", "tqdm",
         "--distpath", distribution, "--workpath", cache / "work", "--specpath", cache,
         "--add-data", f"{plugin.resolve()}:native/live",
         "--add-data", f"{overlay.resolve()}:native/live",
         "--add-data", f"{manifest.resolve()}:native/live",
         "--add-data", f"{ROOT / 'packaging/live/fonts/NotoSans-Regular.ttf'}:native/live",
         "--add-data", f"{capture_assets}:native/capture",
         "--add-data", f"{ROOT / 'docs/mcp-agent-guide.md'}:docs",
         ROOT / "packaging/live/entry.py"], cwd=ROOT)
    if appdir.exists():
        shutil.rmtree(appdir)
    (appdir / "usr").mkdir(parents=True)
    shutil.copytree(distribution / "cs2-camera-studio", appdir / "usr/bin", symlinks=True)
    # Normalize the bundled native names and make the manifest relocatable.
    native = appdir / "usr/bin/_internal/native/live"
    plugin_destination = native / "libserver.so"
    if plugin.name != plugin_destination.name:
        (native / plugin.name).rename(plugin_destination)
    overlay_destination = native / "libcs2_camera_overlay.so"
    if overlay.name != overlay_destination.name:
        (native / overlay.name).rename(overlay_destination)
    layer["layer"]["library_path"] = "./libcs2_camera_overlay.so"
    (native / manifest.name).write_text(json.dumps(layer, indent=2) + "\n")
    for name in ("AppRun", "cs2-camera-studio.desktop", "cs2-camera-studio.svg"):
        shutil.copy2(ROOT / "packaging/live" / name, appdir / name)
    (appdir / ".DirIcon").symlink_to("cs2-camera-studio.svg")
    share = appdir / "usr/share/cs2-camera-studio"
    share.mkdir(parents=True)
    shutil.copy2(ROOT / "packaging/live/mcp-config.example.json", share)
    shutil.copy2(ROOT / "packaging/live/mcp-config.example.toml", share)
    shutil.copy2(ROOT / "docs/live-editor.md", share / "README.md")
    shutil.copy2(ROOT / "examples/live-nuke-flyby.json", share)
    shutil.copy2(ROOT / "docs/high-fps-capture.md", share)
    shutil.copy2(ROOT / "docs/mcp-agent-guide.md", share)
    shutil.copy2(ROOT / "AGENTS.md", share)
    shutil.copy2(ROOT / "docs/map-geometry.md", share)
    shutil.copy2(ROOT / "docs/cinematic-effects-research.md", share)
    shutil.copy2(ROOT / "docs/review-0.1.14.md", share)
    (share / "docs").mkdir()
    for document in ("mcp-agent-guide.md", "high-fps-capture.md", "map-geometry.md", "cinematic-effects-research.md"):
        shutil.copy2(ROOT / "docs" / document, share / "docs" / document)
    shutil.copy2(ROOT / "examples/high-fps-probe-camera.json", share)
    shutil.copy2(ROOT / "examples/live-high-fps-probe.json", share)
    copy_licenses(share / "licenses")
    if appdir_only:
        return appdir
    appimagetool = ensure_appimagetool(cache / "tools")
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
    run([appimagetool, "--no-appstream", appdir, output.resolve()], env=environment)
    output.chmod(0o755)
    metadata = {"version": "0.1.14", "appimage": str(output.resolve()), "sha256": sha256(output),
                "plugin_sha256": sha256(plugin), "overlay_sha256": sha256(overlay),
                "capture_plugin_sha256": sha256(capture_assets / "libcamera_capture.so"),
                "stock_plugin_sha256": sha256(capture_assets / "libserver_stock.so"),
                "font_sha256": sha256(ROOT / "packaging/live/fonts/NotoSans-Regular.ttf"),
                "appimagetool_version": APPIMAGETOOL_VERSION,
                "python": platform.python_version(), "build_platform": platform.platform(),
                "build_libc": platform.libc_ver(),
                "game_fonts": "Read from the installed game at runtime; Valve fonts are not bundled",
                "note": "Native prototype: CS2 build support is pinned by the view hook. Built on this host; other Linux distributions are not yet validated."}
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(metadata, indent=2) + "\n")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin", type=Path, default=ROOT / ".cache/csdm-live/libserver.so")
    parser.add_argument("--overlay", type=Path, default=ROOT / ".cache/cs2-overlay/libcs2_camera_overlay.so")
    parser.add_argument("--output", type=Path, default=ROOT / "outputs/CS2-Camera-Studio-x86_64.AppImage")
    parser.add_argument("--appdir-only", action="store_true", help="Freeze and stage without downloading appimagetool")
    args = parser.parse_args()
    print(build(args.plugin, args.overlay, args.output, appdir_only=args.appdir_only))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
