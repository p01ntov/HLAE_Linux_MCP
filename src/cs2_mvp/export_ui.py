"""Desktop export controls and the sequential live → capture → resume job."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import threading
import time
import uuid

from .live import LiveError, NativeClient, state_root
from .live_track import select_render_range
from .render import (RenderCancelled, render_clip, resolve_capture_fps, validate_export_destination,
                     estimate_capture_storage, validate_resolution, resolve_video_encoding)


@dataclass(frozen=True)
class ExportOptions:
    output: Path
    width: int = 2560
    height: int = 1440
    capture_fps: int = 120
    fps: int = 60
    scope: str | None = None
    slow_motion: bool = False
    audio: bool = True
    keep_raw: bool = False
    reopen: bool = True
    export_format: str = "mp4"
    crf: int | None = None
    video_bitrate_mbps: float | None = None


def describe_export(project, options):
    if options.export_format not in {"mp4", "png"}:
        raise ValueError("Export format must be mp4 or png")
    start, end, selected = select_render_range(project, scope=options.scope)
    capture = resolve_capture_fps(options.fps, options.capture_fps, True)
    if options.export_format == "png" and options.fps > 999:
        raise ValueError("After Effects interpretation is limited to 999 FPS; keep native capture at 1000 and interpret at 60 for slow motion")
    validate_resolution(options.width, options.height)
    encoding = resolve_video_encoding(options.export_format, options.crf, options.video_bitrate_mbps)
    if start < 193 or end <= start or (end-start)/64 > 300:
        raise ValueError("Export needs start tick ≥193 and a camera range between 0 and 300 seconds")
    samples = math.ceil((end-start)/64 * capture)
    preserve_samples = options.slow_motion or options.export_format == "png"
    frames = samples if preserve_samples else round(samples * options.fps / capture)
    if frames < 2:
        raise ValueError("The selected range produces fewer than two output frames")
    storage = estimate_capture_storage(samples, options.width, options.height, options.export_format)
    return {"start_tick": start, "end_tick": end, "range": selected, "samples": samples,
            "frames": frames, "source_duration": (end-start)/64, "output_duration": frames/options.fps,
            "speed": options.fps/capture if preserve_samples else 1,
            "estimated_raw_bytes": storage["raw"], "estimated_working_bytes": storage["working"], "video_encoding": encoding}


def _demo_tick(value):
    if type(value) is not int or not 0 <= value <= 2000000000:
        raise ValueError("Demo tick must be an integer between 0 and 2000000000")
    return value


def save_export_snapshot(client, directory):
    """Persist a fresh project and session view before any stop."""
    client.request("preview", {"playing": False})
    state = client.request("get_state")
    playhead = {"demo_tick": _demo_tick(state["tick"])} if "tick" in state else {}
    project = client.request("export_project")
    project = json.loads(json.dumps(project, allow_nan=False))
    # Preserve legacy requests in the recovery snapshot; validation below must
    # reject them explicitly before closing the live session.
    appearance = state.get("appearance", {})
    if appearance.get("active") and "appearance" not in project:
        project["appearance"] = {"path": appearance.get("path"), "models": appearance.get("models")}
    directory.mkdir(parents=True, mode=0o700, exist_ok=False)
    path = directory / "camera-project.json"
    with path.open("x") as handle:
        import os
        os.chmod(path, 0o600)
        json.dump(project, handle, ensure_ascii=False, indent=2, allow_nan=False)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    return {"path": str(path), "project": project, "cursor": state.get("cursor", 0), **playhead,
            "camera": state.get("camera"), "appearance": project.get("appearance")}


def perform_export(options, *, demo, game_dir, client, stop_session, wait_session,
                   cancel_event=None, notify=None, render=None, directory=None):
    """Always save first; wait for restoration/lock release before capture."""
    notify = notify or (lambda value: None)
    cancel_event = cancel_event or threading.Event()
    outcome = {"snapshot": None, "stopped": False, "cancelled": False, "plan": None, "error": None}
    def cancelled():
        if cancel_event.is_set():
            raise RenderCancelled("Export cancelled")
    try:
        cancelled()
        notify({"stage": "saving", "message": "Saving the camera project and current view"})
        directory = directory or state_root() / "exports" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8])
        snapshot = save_export_snapshot(client, directory)
        outcome["snapshot"] = snapshot
        describe_export(snapshot["project"], options)
        output = options.output.expanduser().absolute()
        validate_export_destination(output, options.export_format)
        output.parent.mkdir(parents=True, exist_ok=True)
        from .capture_environment import validate_environment
        environment = validate_environment(snapshot["project"])
        if environment.get("cosmetics"):
            from .camera_backend import verify_cosmetics_capture_support
            verify_cosmetics_capture_support()
        if render is None:
            from .discovery import validate_media_tools
            validate_media_tools(export_format=options.export_format)
        cancelled()
        notify({"stage": "closing", "message": "Closing this live session and restoring game configuration"})
        stop_session()
        outcome["stopped"] = True
        wait_session()  # Session transaction must have completed, not just sent quit.
        cancelled()
        outcome["plan"] = (render or render_clip)(Path(demo), None, camera=Path(snapshot["path"]),
            game_dir=game_dir, output=output, width=options.width, height=options.height,
            fps=options.fps, capture_fps=options.capture_fps, render_scope=options.scope,
            slow_motion=options.slow_motion, audio=options.audio and not options.slow_motion,
            keep_raw=options.keep_raw, export_format=options.export_format,
            crf=options.crf, video_bitrate_mbps=options.video_bitrate_mbps, cancel_event=cancel_event, notify=notify)
    except RenderCancelled as error:
        outcome["cancelled"], outcome["error"] = True, str(error)
    except BaseException as error:
        outcome["error"] = f"{type(error).__name__}: {error}"
    return outcome


_RESTORE_TIMEOUT = 15.0
_RESTORE_STABLE_SECONDS = 2.5


def _restore_demo_playhead(client, tick, requested_view=None):
    """Confirm the asynchronous seek, including a late startup observer resync."""
    def seek():
        client.request("demo_control", {"action": "pause"})
        client.request("demo_control", {"action": "seek", "tick": tick})
    seek()
    deadline = time.monotonic() + _RESTORE_TIMEOUT
    stable_since, retry_armed, retries = None, False, 0
    while time.monotonic() < deadline:
        state = client.request("get_state")
        presentation = state.get("presentation", {})
        view_ready = (not requested_view or "view" not in presentation or
                      (presentation["view"] == requested_view and not presentation.get("pending")))
        at_tick = state.get("tick") == tick and state.get("paused") is True
        if at_tick and view_ready:
            retry_armed = True
            now = time.monotonic()
            if stable_since is None:
                stable_since = now
            if now - stable_since >= _RESTORE_STABLE_SECONDS:
                return
        else:
            stable_since = None
            # Do not flood an in-flight seek. Retry only after a confirmed
            # arrival is subsequently displaced by the engine's observer resync.
            if retry_armed and not at_tick:
                if retries >= 3:
                    break
                seek()
                retries += 1
                retry_armed = False
        time.sleep(.1)
    raise LiveError(f"Demo did not settle paused at saved tick {tick}; the project remains saved")


def restore_export_snapshot(client, snapshot):
    demo_tick = _demo_tick(snapshot["demo_tick"]) if "demo_tick" in snapshot else None
    project = dict(snapshot["project"])
    from .live_mcp import ProtocolError, reject_removed_cosmetics
    try:
        reject_removed_cosmetics(project)
        if snapshot.get("appearance"):
            reject_removed_cosmetics({"appearance":snapshot["appearance"]})
    except ProtocolError as error:
        raise LiveError(error.message) from error
    client.request("import_project", {"project": project})
    # A fresh project has no sampled camera yet; its saved live camera is
    # restored below without asking the native track evaluator for a key.
    if project.get("keys"):
        client.request("scrub", {"time": snapshot.get("cursor", 0), "seek_demo": demo_tick is None})
    if demo_tick is not None:
        _restore_demo_playhead(client, demo_tick, project.get("presentation", {}).get("view"))
    if snapshot.get("camera"):
        client.request("set_camera", snapshot["camera"])


def create_export_dialog(parent, project, *, start_export):
    from PySide6 import QtCore, QtWidgets

    class ExportDialog(QtWidgets.QDialog):
        def __init__(self):
            super().__init__(parent)
            self.setWindowTitle("Export cinematic · CS2 Camera Studio")
            self.setWindowFlag(QtCore.Qt.WindowType.WindowStaysOnTopHint, True)
            self.setMinimumWidth(570)
            self.busy = False
            self.project = project
            self.cancel_event = threading.Event()
            form = QtWidgets.QFormLayout(self)
            form.setContentsMargins(24, 22, 24, 22)
            form.setSpacing(13)
            self.export_format = QtWidgets.QComboBox()
            self.export_format.addItem("MP4 · H.264 video", "mp4")
            self.export_format.addItem("PNG sequence · lossless RGB · After Effects", "png")
            form.addRow("Format", self.export_format)
            destination_row = QtWidgets.QHBoxLayout()
            default = Path.home() / "Videos" / ("cs2-camera-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".mp4")
            self.destination = QtWidgets.QLineEdit(str(default))
            browse = QtWidgets.QPushButton("Browse…")
            destination_row.addWidget(self.destination, 1)
            destination_row.addWidget(browse)
            self.destination_label = QtWidgets.QLabel("Video file")
            form.addRow(self.destination_label, destination_row)
            self.scope = QtWidgets.QComboBox()
            self.scope_touched = False
            for label, value in (("Whole composition", "composition"), ("Render region · In / Out", "region")):
                self.scope.addItem(label, value)
            self.scope.setCurrentIndex(1 if project.get("render_range", {}).get("mode") == "region" else 0)
            form.addRow("Time range", self.scope)
            self.resolution = QtWidgets.QComboBox()
            for label, value in (("2560 × 1440 · QHD", (2560,1440)), ("1920 × 1080 · Full HD", (1920,1080)), ("3840 × 2160 · 4K", (3840,2160)), ("1280 × 720 · HD", (1280,720))):
                self.resolution.addItem(label, value)
            self.resolution.addItem("Custom · enter width and height", None)
            form.addRow("Resolution", self.resolution)
            self.custom_dimensions = QtWidgets.QWidget()
            dimensions_row = QtWidgets.QHBoxLayout(self.custom_dimensions)
            dimensions_row.setContentsMargins(0, 0, 0, 0)
            self.custom_width, self.custom_height = QtWidgets.QSpinBox(), QtWidgets.QSpinBox()
            self.custom_width.setRange(320, 8192)
            self.custom_height.setRange(240, 8192)
            for field, value in ((self.custom_width, 2560), (self.custom_height, 1440)):
                field.setSingleStep(2)
                field.setValue(value)
                field.setToolTip("Even dimensions, up to 8,294,400 total pixels (3840 × 2160). Portrait and custom aspect ratios are allowed.")
            dimensions_row.addWidget(self.custom_width, 1)
            dimensions_row.addWidget(QtWidgets.QLabel("×"))
            dimensions_row.addWidget(self.custom_height, 1)
            self.custom_dimensions_label = QtWidgets.QLabel("Width × height")
            form.addRow(self.custom_dimensions_label, self.custom_dimensions)
            self.capture_fps, self.output_fps = QtWidgets.QSpinBox(), QtWidgets.QSpinBox()
            self.capture_fps.setRange(120, 1000)
            self.capture_fps.setValue(120)
            self.output_fps.setRange(15, 1000)
            self.output_fps.setValue(60)
            form.addRow("Native capture FPS", self.capture_fps)
            self.output_fps_label = QtWidgets.QLabel("Video playback FPS")
            form.addRow(self.output_fps_label, self.output_fps)
            self.encoding_options = QtWidgets.QWidget()
            encoding_row = QtWidgets.QHBoxLayout(self.encoding_options)
            encoding_row.setContentsMargins(0, 0, 0, 0)
            self.quality_mode = QtWidgets.QComboBox()
            self.quality_mode.addItem("CRF quality", "crf")
            self.quality_mode.addItem("Average bitrate", "bitrate")
            self.crf = QtWidgets.QSpinBox()
            self.crf.setRange(0, 51)
            self.crf.setValue(18)
            self.crf.setToolTip("Lower CRF gives higher quality and larger files. MP4 uses YUV 4:2:0; use PNG for exact captured RGB pixels.")
            self.bitrate = QtWidgets.QDoubleSpinBox()
            self.bitrate.setRange(.1, 1000)
            self.bitrate.setDecimals(2)
            self.bitrate.setValue(20)
            self.bitrate.setSuffix(" Mbps")
            self.bitrate.setToolTip("Target average video bitrate, excluding audio. The encoder may vary the actual bitrate.")
            self.png_encoding_info = QtWidgets.QLabel("Lossless RGB PNG · compression 6 · no bitrate")
            encoding_row.addWidget(self.quality_mode, 1)
            encoding_row.addWidget(self.crf)
            encoding_row.addWidget(self.bitrate)
            encoding_row.addWidget(self.png_encoding_info)
            self.encoding_label = QtWidgets.QLabel("MP4 encoding")
            form.addRow(self.encoding_label, self.encoding_options)
            self.mode = QtWidgets.QComboBox()
            self.mode.addItem("Normal duration · resample to video FPS", False)
            self.mode.addItem("Slow motion · keep every captured frame", True)
            form.addRow("Playback", self.mode)
            self.audio = QtWidgets.QCheckBox("Include game audio")
            self.audio.setChecked(True)
            form.addRow("Audio", self.audio)
            self.reopen = QtWidgets.QCheckBox("Reopen this demo and restore the editor after export")
            self.reopen.setChecked(True)
            form.addRow(self.reopen)
            self.keep_raw = QtWidgets.QCheckBox("Keep raw frames for another encode · uses disk space")
            form.addRow(self.keep_raw)
            self.summary = QtWidgets.QLabel()
            self.summary.setWordWrap(True)
            form.addRow(self.summary)
            info = QtWidgets.QLabel("Your project is saved first. The owned live session closes for capture, then the editor can reopen with the same project and view.")
            info.setWordWrap(True)
            form.addRow(info)
            self.progress = QtWidgets.QProgressBar()
            self.progress.setTextVisible(False)
            self.progress.setRange(0, 1)
            self.progress.setValue(0)
            form.addRow(self.progress)
            self.status = QtWidgets.QLabel("Ready")
            self.status.setWordWrap(True)
            self.status.setTextInteractionFlags(QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)
            form.addRow(self.status)
            controls = QtWidgets.QHBoxLayout()
            self.render_button, self.cancel_button = QtWidgets.QPushButton("Export video"), QtWidgets.QPushButton("Close")
            controls.addWidget(self.render_button, 1)
            controls.addWidget(self.cancel_button)
            form.addRow(controls)
            self.inputs = [self.export_format, self.destination, browse, self.scope, self.resolution, self.custom_width, self.custom_height,
                           self.capture_fps, self.output_fps, self.quality_mode, self.crf, self.bitrate, self.mode, self.audio, self.reopen, self.keep_raw]
            browse.clicked.connect(self.browse)
            self.cancel_button.clicked.connect(self.cancel)
            self.render_button.clicked.connect(self.begin)
            for item in (self.scope, self.mode, self.quality_mode):
                item.currentIndexChanged.connect(self.update_summary)
            self.resolution.currentIndexChanged.connect(self.select_resolution)
            self.scope.currentIndexChanged.connect(self.select_scope)
            self.capture_fps.valueChanged.connect(self.update_summary)
            self.output_fps.valueChanged.connect(self.update_summary)
            for item in (self.custom_width, self.custom_height, self.crf, self.bitrate):
                item.valueChanged.connect(self.update_summary)
            self.export_format.currentIndexChanged.connect(self.select_format)
            self.update_summary()

        def options(self):
            width, height = self.resolution.currentData() or (self.custom_width.value(), self.custom_height.value())
            mp4 = self.export_format.currentData() == "mp4"
            return ExportOptions(output=Path(self.destination.text()), width=width, height=height,
                capture_fps=self.capture_fps.value(), fps=self.output_fps.value(), scope=self.scope.currentData() if self.scope_touched else None,
                slow_motion=self.mode.currentData() if mp4 else False, audio=self.audio.isChecked(),
                keep_raw=self.keep_raw.isChecked(), reopen=self.reopen.isChecked(), export_format=self.export_format.currentData(),
                crf=self.crf.value() if mp4 and self.quality_mode.currentData() == "crf" else None,
                video_bitrate_mbps=self.bitrate.value() if mp4 and self.quality_mode.currentData() == "bitrate" else None)

        def select_resolution(self):
            preset = self.resolution.currentData()
            if preset:
                self.custom_width.setValue(preset[0])
                self.custom_height.setValue(preset[1])
            self.update_summary()

        def select_format(self):
            png = self.export_format.currentData() == "png"
            path = Path(self.destination.text())
            if png:
                self.output_fps.setMaximum(999)
                if path.suffix.casefold() == ".mp4":
                    self.destination.setText(str(path.with_suffix("")) + "-frames")
                self.capture_fps.setValue(1000)
                self.output_fps.setValue(60)
                self.audio.setChecked(False)
            elif path.name.endswith("-frames"):
                self.destination.setText(str(path.with_name(path.name[:-7] + ".mp4")))
            if not png:
                self.output_fps.setMaximum(1000)
            self.update_summary()

        def select_scope(self):
            self.scope_touched = True
            self.update_summary()

        def update_summary(self):
            png = self.export_format.currentData() == "png"
            custom = self.resolution.currentData() is None
            self.custom_dimensions.setVisible(custom)
            self.custom_dimensions_label.setVisible(custom)
            self.custom_width.setEnabled(custom and not self.busy)
            self.custom_height.setEnabled(custom and not self.busy)
            bitrate = self.quality_mode.currentData() == "bitrate"
            self.quality_mode.setVisible(not png)
            self.quality_mode.setEnabled(not png and not self.busy)
            self.crf.setVisible(not png and not bitrate)
            self.crf.setEnabled(not png and not bitrate and not self.busy)
            self.bitrate.setVisible(not png and bitrate)
            self.bitrate.setEnabled(not png and bitrate and not self.busy)
            self.png_encoding_info.setVisible(png)
            self.encoding_label.setText("PNG compression" if png else "MP4 encoding")
            slow = bool(self.mode.currentData()) and not png
            self.mode.setEnabled(not png and not self.busy)
            self.audio.setEnabled(not slow and not self.busy)
            self.audio.setText("Separate original WAV · normal game timing" if png else "Muted for slow motion" if slow else "Include game audio")
            self.destination_label.setText("New sequence folder" if png else "Video file")
            self.output_fps_label.setText("AE interpretation FPS" if png else "Video playback FPS")
            self.render_button.setText("Export lossless frames" if png else "Export video")
            try:
                info = describe_export(self.project, self.options())
                if png:
                    text = (f"{info['frames']:,} lossless RGB PNG frames · every native sample is kept\n"
                            f"Demo {info['source_duration']:.3f}s → {info['output_duration']:.3f}s when interpreted at {self.output_fps.value()} FPS in AE\n"
                            "Compressed file size depends on the image; no resampling or chroma subsampling.\n")
                else:
                    encoding = info["video_encoding"]
                    quality = f"Target average {encoding['target_bitrate_mbps']:g} Mbps" if encoding["rate_control"] == "bitrate" else f"CRF {encoding['crf']}"
                    text = f"Demo {info['source_duration']:.3f}s → video {info['output_duration']:.3f}s · {info['frames']:,} video frames · {quality}\n"
                self.summary.setText(text + f"{info['samples']:,} native samples · reserve approximately {info['estimated_working_bytes']/1024**3:.1f} GiB working space · ticks {info['start_tick']}–{info['end_tick']}")
                self.render_button.setEnabled(not self.busy)
            except (ValueError, TypeError) as error:
                self.summary.setText(str(error))
                self.render_button.setEnabled(False)
            # QLabel's changing wrapped height must remain visible when switching
            # from the shorter MP4 summary to lossless sequence instructions.
            self.summary.setMinimumHeight(self.summary.heightForWidth(max(1, self.minimumWidth() - 48)))

        def browse(self):
            png = self.export_format.currentData() == "png"
            path, _ = QtWidgets.QFileDialog.getSaveFileName(self, "New PNG sequence folder" if png else "Export video", self.destination.text(),
                "Sequence folder name (*)" if png else "MP4 video (*.mp4)", options=QtWidgets.QFileDialog.Option.DontConfirmOverwrite)
            if path:
                self.destination.setText(path)

        def refresh_project(self, project):
            if not self.busy:
                self.project = project
                if not self.scope_touched:
                    self.scope.blockSignals(True)
                    self.scope.setCurrentIndex(1 if project.get("render_range", {}).get("mode") == "region" else 0)
                    self.scope.blockSignals(False)
                self.update_summary()

        def begin(self):
            self.busy = True
            self.cancel_event.clear()
            for widget in self.inputs:
                widget.setEnabled(False)
            self.render_button.setEnabled(False)
            self.cancel_button.setText("Cancel export")
            self.progress.setRange(0, 0)
            self.status.setText("Saving project…")
            start_export(self, self.options())

        def report(self, value):
            self.status.setText(value.get("message", str(value)))
            if "progress" in value:
                self.progress.setRange(0, 1000)
                self.progress.setValue(round(1000*value["progress"]))
            else:
                self.progress.setRange(0, 0)

        def complete(self, outcome):
            self.busy = False
            self.progress.setRange(0, 1)
            self.progress.setValue(1 if outcome.get("plan") else 0)
            self.cancel_button.setText("Close")
            self.cancel_button.setEnabled(True)
            for widget in self.inputs:
                widget.setEnabled(True)
            self.update_summary()
            if outcome.get("plan"):
                self.status.setText("Export complete\n" + str(outcome["plan"]["output"]))
            else:
                suffix = "\nProject saved: " + outcome["snapshot"]["path"] if outcome.get("snapshot") else ""
                self.status.setText((outcome.get("error") or "Export stopped") + suffix)

        def cancel(self):
            if self.busy:
                self.cancel_event.set()
                self.status.setText("Cancelling owned capture and restoring configuration…")
                self.cancel_button.setEnabled(False)
            else:
                self.close()

        def closeEvent(self, event):
            if self.busy:
                self.cancel()
                event.ignore()
            else:
                event.accept()

    return ExportDialog()
