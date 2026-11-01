#!/usr/bin/env python3
"""Independently inspect and smoke-test a built AppImage, without starting CS2.

Requires the build's Python minor version and PyInstaller. All generated files
stay under --artifacts and --temporary. HOME is inherited, never overridden.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.abc
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import tomllib

from PyInstaller.archive.readers import CArchiveReader


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class Audit:
    def __init__(self, args):
        self.args = args
        self.source = args.source.resolve()
        self.candidate = args.candidate.resolve()
        self.root = args.artifacts.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.run_root = self.root / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + f"-{os.getpid()}")
        self.run_root.mkdir(mode=0o700)
        self.report = {"started_utc": datetime.now(timezone.utc).isoformat(),
                       "candidate": str(self.candidate), "source": str(self.source),
                       "artifacts": str(self.run_root), "checks": {}, "ok": False}
        excluded = {"PYTHONPATH", "PYTHONHOME", "APPIMAGE", "APPDIR", "APPIMAGE_EXTRACT_AND_RUN",
                    "LD_LIBRARY_PATH", "LD_LIBRARY_PATH_ORIG", "LD_PRELOAD", "QT_PLUGIN_PATH",
                    "QML2_IMPORT_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "VK_INSTANCE_LAYERS",
                    "VK_LAYER_PATH", "VK_ADD_LAYER_PATH"}
        self.env = {key: value for key, value in os.environ.items()
                    if key not in excluded and not key.startswith(("CS2_CAMERA_", "_PYI_"))}
        for name in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"):
            path = self.run_root / name.lower()
            path.mkdir(mode=0o700)
            self.env[name] = str(path)
        temporary = args.temporary.resolve() / f"p{os.getpid()}"
        temporary.mkdir(parents=True, mode=0o700)
        runtime = temporary / "r"
        runtime.mkdir(mode=0o700)
        self.env.update(TMPDIR=str(temporary), XDG_RUNTIME_DIR=str(runtime), QT_QPA_PLATFORM="offscreen",
                        PYTHONDONTWRITEBYTECODE="1", CS2_CAMERA_EXPORT_PRESETS=str(self.run_root / "exports.json"))
        self.check("private_runtime_short", len(os.fsencode(runtime / "cs2-camera/control.sock")) <= 107,
                   {"runtime": str(runtime)})
        self.check("home_preserved", self.env.get("HOME") == os.environ.get("HOME"))

    def check(self, name, condition, details=None):
        self.report["checks"][name] = {"ok": bool(condition), "details": details}
        if not condition:
            raise RuntimeError("Audit failed: " + name)

    def run(self, name, command, *, env=None, input=None, timeout=45, expected=0):
        result = subprocess.run(list(map(str, command)), cwd=self.run_root, env=env or self.env,
                                input=input, capture_output=True, text=True, timeout=timeout)
        (self.run_root / (name + ".stdout")).write_text(result.stdout)
        (self.run_root / (name + ".stderr")).write_text(result.stderr)
        self.check(name + "_exit", result.returncode == expected,
                   {"returncode": result.returncode, "stderr": name + ".stderr"})
        return result.stdout

    def contents(self):
        metadata = json.loads(self.candidate.with_suffix(self.candidate.suffix + ".json").read_text())
        version = tomllib.loads((self.source / "pyproject.toml").read_text())["project"]["version"]
        self.version = version
        self.metadata = metadata
        digest = sha256(self.candidate)
        self.report["candidate_sha256"] = digest
        self.report["candidate_bytes"] = self.candidate.stat().st_size
        self.check("metadata_version", metadata["version"] == version, version)
        self.check("candidate_hash", metadata["sha256"] == digest, digest)
        self.check("python_minor_matches_builder", metadata["python"].split(".")[:2] ==
                   [str(value) for value in sys.version_info[:2]], metadata["python"])
        self.run("extract", [self.candidate, "--appimage-extract"], timeout=120)
        self.app = self.run_root / "squashfs-root"
        self.binary = self.app / "usr/bin/cs2-camera-studio"
        self.internal = self.app / "usr/bin/_internal"
        self.share = self.app / "usr/share/cs2-camera-studio"
        self.env["APPDIR"] = str(self.app)
        self.check("launcher_executable", os.access(self.binary, os.X_OK) and os.access(self.app / "AppRun", os.X_OK))
        documents = {
            "usr/bin/_internal/docs/mcp-agent-guide.md": "docs/mcp-agent-guide.md",
            "usr/share/cs2-camera-studio/README.md": "docs/live-editor.md",
            "usr/share/cs2-camera-studio/AGENTS.md": "AGENTS.md",
            "usr/share/cs2-camera-studio/review-" + version + ".md": "docs/review-" + version + ".md",
            "AppRun": "packaging/live/AppRun", "cs2-camera-studio.desktop": "packaging/live/cs2-camera-studio.desktop",
            "cs2-camera-studio.svg": "packaging/live/cs2-camera-studio.svg",
        }
        for document in ("mcp-agent-guide.md", "high-fps-capture.md", "map-geometry.md", "cinematic-effects-research.md"):
            for prefix in ("usr/share/cs2-camera-studio/", "usr/share/cs2-camera-studio/docs/"):
                documents[prefix + document] = "docs/" + document
        for filename in ("mcp-config.example.json", "mcp-config.example.toml"):
            documents["usr/share/cs2-camera-studio/" + filename] = "packaging/live/" + filename
        mismatches = [bundle for bundle, source in documents.items()
                      if not (self.app / bundle).is_file() or sha256(self.app / bundle) != sha256(self.source / source)]
        self.check("documents_exact", not mismatches, {"count": len(documents), "mismatches": mismatches})
        self.check("font_exact", sha256(self.internal / "native/live/NotoSans-Regular.ttf") ==
                   metadata["font_sha256"] == sha256(self.source / "packaging/live/fonts/NotoSans-Regular.ttf"))
        notices = self.share / "licenses"
        provenance = json.loads((notices / "provenance.json").read_text())
        mismatches = []
        for item in provenance["files"]:
            path = notices / "upstream" / item["path"]
            if not path.is_file() or sha256(path) != item["sha256"]:
                mismatches.append(item["path"])
        self.check("pinned_licenses_exact", not mismatches and bool(provenance["files"]),
                   {"count": len(provenance["files"]), "mismatches": mismatches})
        self.check("license_provenance_exact", sha256(notices / "provenance.json") ==
                   sha256(self.source / "packaging/licenses/provenance.json"))
        self.check("license_notice_exact", sha256(notices / "NOTICE.md") ==
                   sha256(self.source / "packaging/licenses/NOTICE.md"))
        required = ("CSDM-LICENSE", "CSDM-capture-LICENSE", "Dear-ImGui-LICENSE", "nlohmann-json-LICENSE",
                    "easywsclient-COPYING", "Vulkan-Headers-MIT", "Vulkan-Headers-Apache-2.0", "NotoSans-OFL.txt",
                    "bundled-distributions.json")
        self.check("native_and_font_licenses", all((notices / name).is_file() and (notices / name).stat().st_size for name in required))
        self.check("python_package_licenses", all(any((notices / name).rglob("*")) for name in
                   ("PyInstaller", "numpy", "pandas", "polars", "polars-runtime-32", "pyarrow", "tqdm")))

    def cli_mcp(self):
        help_text = self.run("help", [self.app / "AppRun", "--help"])
        self.check("live_help_features", all(flag in help_text for flag in ("--mcp", "--projects-dir", "--shot-library")))
        render = self.run("render-help", [self.app / "AppRun", "render", "--help"])
        self.check("render_help_features", all(flag in render for flag in
                   ("--width", "--height", "--crf", "--video-bitrate-mbps", "--capture-fps", "--format")))
        follow = self.run("follow-help", [self.app / "AppRun", "follow-draft", "--help"])
        self.check("follow_help_features", all(flag in follow for flag in
                   ("--player", "--start", "--end", "--mode", "--on-loss", "--output")))
        index = self.run("index-maps-help", [self.app / "AppRun", "index-maps", "--help"])
        self.check("map_help_features", all(flag in index for flag in ("--cache-root", "--bootstrap-tool", "--map", "--game-dir")))
        messages = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25"}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
            {"jsonrpc": "2.0", "id": 3, "method": "resources/read", "params": {"uri": "cs2-camera://guides/agent"}},
        ]
        output = self.run("mcp", [self.app / "AppRun", "--mcp"],
                          input="".join(json.dumps(message) + "\n" for message in messages))
        responses = [json.loads(line) for line in output.splitlines() if line.strip()]
        self.check("mcp_responses", len(responses) == 3 and all("result" in response and "error" not in response for response in responses))
        by_id = {response["id"]: response["result"] for response in responses}
        self.check("mcp_version", by_id[1]["serverInfo"]["version"] == self.version, by_id[1]["serverInfo"])
        expected = self.run("source-mcp-tools", [sys.executable, "-B", "-c",
                            "import sys,json; sys.path.insert(0,sys.argv[1]); from cs2_mvp.live_mcp import TOOLS; print(json.dumps(TOOLS))",
                            self.source / "src"])
        self.check("mcp_schema_exact", by_id[2]["tools"] == json.loads(expected),
                   {"count": len(by_id[2]["tools"]), "names": [tool["name"] for tool in by_id[2]["tools"]]})
        guide = (self.source / "docs/mcp-agent-guide.md").read_text()
        self.check("mcp_guide_exact", by_id[3]["contents"][0]["text"] == guide)

    def native(self):
        native = [
            ("native/live/libserver.so", ".cache/csdm-live/libserver.so", "plugin_sha256", {"libtier0.so"}),
            ("native/live/libcs2_camera_overlay.so", ".cache/cs2-overlay/libcs2_camera_overlay.so", "overlay_sha256", set()),
            ("native/capture/libcamera_capture.so", ".cache/csdm-motion-capture/libserver.so", "capture_plugin_sha256", {"libtier0.so"}),
            ("native/capture/libserver_stock.so", ".cache/csdm/7270bc95c8cab87b7f9ddd490fea13c6eda3ace7/libserver.so", "stock_plugin_sha256", {"libtier0.so"}),
        ]
        details = []
        for relative, built, key, expected_missing in native:
            bundled = self.internal / relative
            self.check(key, sha256(bundled) == self.metadata[key] == sha256(self.source / built), sha256(bundled))
            versions = self.run("readelf-" + key, ["readelf", "--version-info", bundled])
            versions = {tuple(map(int, value.split("."))) for value in re.findall(r"GLIBC_([0-9.]+)", versions)}
            highest = max(versions) if versions else None
            self.check(key + "_glibc", highest is not None and highest <= (2, 38), highest)
            dependencies = self.run("ldd-" + key, ["ldd", bundled])
            missing = set(re.findall(r"^\s*(\S+)\s*=> not found", dependencies, re.MULTILINE))
            self.check(key + "_dependencies", missing <= expected_missing,
                       {"missing": sorted(missing), "provided_by_game": sorted(expected_missing)})
            details.append({"file": relative, "sha256": sha256(bundled), "glibc_max": highest})
        self.report["native"] = details
        self.check("native_abis", b"CS2_CAMERA_LIVE_ABI 1" in (self.internal / native[0][0]).read_bytes()
                   and all(marker in (self.internal / native[2][0]).read_bytes()
                           for marker in (b"CS2_MVP_MOTION_ABI 3", b"CS2_MVP_ENVIRONMENT_ABI 1")))
        layer = json.loads((self.internal / "native/live/VkLayer_cs2_camera.json").read_text())["layer"]
        self.check("vulkan_manifest_relative", layer["name"] == "VK_LAYER_CS2_camera" and
                   layer["library_path"] == "./libcs2_camera_overlay.so", layer["library_path"])
        env = dict(self.env, LD_LIBRARY_PATH=str(self.internal) + ":" + str(self.internal / "PySide6/Qt/lib"))
        paths = [self.binary, *self.internal.glob("libpython*.so*"), *self.internal.glob("PySide6/QtCore*.so"),
                 *self.internal.glob("PySide6/QtWidgets*.so"), *self.internal.glob("PySide6/QtGui*.so"),
                 *self.internal.glob("PySide6/Qt/plugins/platforms/libqoffscreen.so"),
                 *self.internal.glob("PySide6/Qt/plugins/platforms/libqxcb.so")]
        all_dependencies = []
        for number, path in enumerate(paths):
            output = self.run(f"runtime-ldd-{number}", ["ldd", path], env=env)
            missing = re.findall(r"^\s*(\S+)\s*=> not found", output, re.MULTILINE)
            self.check(f"runtime-deps-{number}", not missing, {"file": str(path.relative_to(self.app)), "missing": missing})
            all_dependencies.append(str(path.relative_to(self.app)))
        self.report["dynamic_dependency_files"] = all_dependencies
        qt = self.internal / "PySide6/Qt/lib/libQt6Core.so.6"
        self.check("qt_shared_replaceable", qt.is_file() and not qt.is_symlink(), str(qt.relative_to(self.app)))

    def modules(self):
        archive = CArchiveReader(str(self.binary))
        pyz = archive.open_embedded_archive(next(name for name in archive.toc if name.endswith(".pyz")))
        details = []
        missing = []
        for path in sorted((self.source / "src/cs2_mvp").glob("*.py")):
            name = "cs2_mvp" if path.name == "__init__.py" else "cs2_mvp." + path.stem
            if name not in pyz.toc:
                missing.append(name)
                continue
            code = pyz.extract(name)
            expected = compile(path.read_bytes(), code.co_filename, "exec", dont_inherit=True, optimize=0)
            self.check("module_exact_" + name, code == expected)
            details.append({"name": name, "source_sha256": sha256(path), "code_matches_source": True})
        self.report["modules"] = details
        self.check("all_application_modules_present", not missing, {"count": len(details), "missing": missing})
        self.check("export_presets_present", "cs2_mvp.export_presets" in pyz.toc)
        # The executable's entry script is outside PYZ.
        import marshal
        code = marshal.loads(archive.extract("entry"))
        self.check("entry_code_exact", code == compile((self.source / "packaging/live/entry.py").read_bytes(),
                   code.co_filename, "exec", dont_inherit=True, optimize=0))

    def launcher(self):
        empty_game = self.run_root / "empty-game"
        empty_game.mkdir()
        # Explicit missing game directory prevents discovery from scanning the user's
        # Steam libraries. No demo or --start-session is supplied.
        command = [str(self.app / "AppRun"), "--game-dir", str(empty_game)]
        with (self.run_root / "launcher.stdout").open("w") as stdout, (self.run_root / "launcher.stderr").open("w") as stderr:
            process = subprocess.Popen(command, cwd=self.run_root, env=self.env, stdout=stdout, stderr=stderr,
                                       start_new_session=True)
            started = time.monotonic()
            qt_loaded = False
            maps = ""
            try:
                while time.monotonic() - started < self.args.launcher_seconds:
                    if process.poll() is not None:
                        break
                    try:
                        maps = Path(f"/proc/{process.pid}/maps").read_text()
                        qt_loaded = qt_loaded or "libQt6Widgets.so.6" in maps
                    except OSError:
                        pass
                    time.sleep(.1)
                self.check("launcher_stays_alive", process.poll() is None and qt_loaded,
                           {"seconds": round(time.monotonic() - started, 2), "qt_widgets_loaded": qt_loaded,
                            "returncode": process.poll()})
                (self.run_root / "launcher.maps").write_text(maps)
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=8)
                self.check("launcher_clean_close", process.returncode == 0, process.returncode)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
        errors = (self.run_root / "launcher.stderr").read_text()
        self.check("launcher_no_fatal_import", not any(text in errors for text in
                   ("Traceback (most recent call last)", "ModuleNotFoundError", "Failed to execute script", "Aborted", "could not load")))

    def export_dialog(self):
        command = [sys.executable, "-B", Path(__file__).resolve(), "--probe-export", self.binary,
                   "--artifacts", self.run_root]
        output = self.run("archive-export-dialog", command, timeout=45)
        result = json.loads(output.splitlines()[-1])
        self.check("archive_export_dialog", result.get("ok"), result)

    def finish(self):
        self.check("candidate_unchanged", sha256(self.candidate) == self.report["candidate_sha256"])
        # Guard against an edit racing the audit, even when a module was checked early.
        changed = []
        for item in self.report["modules"]:
            name = item["name"]
            path = self.source / "src" / (name.replace(".", "/") + ("/__init__.py" if name == "cs2_mvp" else ".py"))
            if sha256(path) != item["source_sha256"]:
                changed.append(name)
        self.check("source_modules_unchanged_during_audit", not changed, changed)
        self.report["ok"] = True


def probe_export(binary, artifacts):
    """Exercise archived app code with the audit interpreter and Qt runtime.

    This complements the actual executable smoke test. It is not described as
    a second execution of the AppImage's embedded interpreter.
    """
    archive = CArchiveReader(str(binary))
    pyz = archive.open_embedded_archive(next(name for name in archive.toc if name.endswith(".pyz")))

    class ArchivedApplication(importlib.abc.MetaPathFinder, importlib.abc.Loader):
        def find_spec(self, fullname, path=None, target=None):
            if fullname == "cs2_mvp" or fullname.startswith("cs2_mvp."):
                if fullname not in pyz.toc:
                    raise ModuleNotFoundError("Application module absent from the package: " + fullname)
                return importlib.util.spec_from_loader(fullname, self, is_package=fullname == "cs2_mvp")

        def create_module(self, spec):
            return None

        def exec_module(self, module):
            module.__file__ = str(binary.parent / "_internal" / module.__name__.replace(".", "/")) + ".pyc"
            if module.__name__ == "cs2_mvp":
                module.__path__ = []
            exec(pyz.extract(module.__name__), module.__dict__)

    sys.meta_path.insert(0, ArchivedApplication())
    from PySide6 import QtWidgets
    from cs2_mvp.export_ui import create_export_dialog
    from cs2_mvp import export_presets
    app = QtWidgets.QApplication([])
    export_presets.save_preset("Package audit · portrait", {"width": 1080, "height": 1920, "capture_fps": 240, "fps": 30})
    project = {"name": "Package audit", "start_tick": 1000, "duration": 1,
               "render_range": {"mode": "region", "in": .125, "out": .5},
               "keys": [{"id": "1", "time": 0, "position": [0, 0, 64], "angles": [0, 0, 0], "fov": 90}]}
    started = []
    dialog = create_export_dialog(None, project, start_export=lambda *args: started.append(args))
    destination = artifacts / "never-rendered.mp4"
    dialog.destination.setText(str(destination))
    dialog.scope.setCurrentIndex(0)
    dialog.scope.setCurrentIndex(1)
    dialog.show()
    app.processEvents()
    index = dialog.preset.findData("Package audit · portrait")
    assert index > 0
    dialog.preset.setCurrentIndex(index)
    dialog.select_preset()
    options = dialog.options()
    assert (options.width, options.height, options.capture_fps, options.fps) == (1080, 1920, 240, 30)
    assert options.output == destination and options.scope == "region"
    assert not started and not destination.exists()
    assert dialog.grab().save(str(artifacts / "archive-export-dialog.png"))
    dialog.close()
    app.processEvents()
    assert not dialog.isVisible()
    print(json.dumps({"ok": True, "code": "from the candidate PYZ archive", "qt": "audit environment",
                      "preset_restored": True, "destination_and_range_preserved": True, "render_started": False}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--temporary", type=Path)
    parser.add_argument("--launcher-seconds", type=float, default=8)
    parser.add_argument("--probe-export", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.probe_export:
        probe_export(args.probe_export, args.artifacts)
        return 0
    if args.source is None or args.candidate is None or args.temporary is None:
        parser.error("--source, --candidate and --temporary are required")
    audit = Audit(args)
    try:
        for name in ("contents", "cli_mcp", "native", "modules", "launcher", "export_dialog", "finish"):
            getattr(audit, name)()
            print(name + " PASS", flush=True)
    except BaseException as error:
        audit.report["error"] = type(error).__name__ + ": " + str(error)
        import traceback
        (audit.run_root / "audit-error.log").write_text(traceback.format_exc())
    finally:
        audit.report["completed_utc"] = datetime.now(timezone.utc).isoformat()
        report = audit.run_root / "audit.json"
        report.write_text(json.dumps(audit.report, ensure_ascii=False, indent=2) + "\n")
        (audit.root / "latest.json").write_text(json.dumps({"ok": audit.report["ok"], "report": str(report),
            "candidate_sha256": audit.report.get("candidate_sha256"), "error": audit.report.get("error")}, indent=2) + "\n")
        print((audit.root / "latest.json").read_text().strip(), flush=True)
    return 0 if audit.report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
