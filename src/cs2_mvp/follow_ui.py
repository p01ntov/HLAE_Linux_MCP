"""Explicit desktop workflow for recorded-player camera drafts.

All parsing, hashing and disk writes happen on a worker. This module never
connects to the game or applies a camera; saving is a separate user action.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import queue
import re
import stat
import threading
import time
import uuid

from .demo import parser
from .demo_analysis import _parsed
from .follow_camera import (MAX_DURATION_SECONDS, TICKRATE, _fingerprint, _fingerprint_evidence,
                            _rows, _stat_fingerprint, _steam_id, bake_demo_follow)
from .live_track import LiveTrack


def _cancelled(event):
    if event is not None and event.is_set():
        raise InterruptedError("Follow draft operation cancelled")


def _hash_recording(path, expected, *, cancel_event=None):
    """Hash the same open recording, checking identity before and after I/O."""
    _cancelled(cancel_event)
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        if _stat_fingerprint(os.fstat(handle.fileno())) != expected:
            raise ValueError("Demo changed before fingerprinting; regenerate the draft")
        while block := handle.read(1024*1024):
            _cancelled(cancel_event)
            digest.update(block)
        if _stat_fingerprint(os.fstat(handle.fileno())) != expected:
            raise ValueError("Demo changed while fingerprinting; regenerate the draft")
    if _fingerprint(path) != expected:
        raise ValueError("Demo changed while fingerprinting; regenerate the draft")
    _cancelled(cancel_event)
    return digest.hexdigest()


def load_follow_players(demo):
    """Read roster identities, without selecting a player by name or slot."""
    path = Path(demo).expanduser().resolve()
    before = _fingerprint(path)
    source = _parsed(parser, path)
    players, identities = [], set()
    for entry in _rows(_parsed(source.parse_player_info), 4096):
        try:
            steam_id = _steam_id(entry.get("steamid"))
        except ValueError:
            continue  # Bots/unknown identities cannot support exact following.
        if steam_id in identities:
            raise ValueError("Demo has duplicate SteamID64 roster entries; cannot select an unambiguous target")
        identities.add(steam_id)
        nickname = entry.get("name")
        players.append({"steamid": steam_id, "name": nickname if isinstance(nickname, str) else steam_id})
    if _fingerprint(path) != before:
        raise ValueError("Demo changed while reading its players; reopen Follow from demo")
    if not players:
        raise ValueError("This demo has no recorded players with an exact SteamID64")
    return sorted(players, key=lambda value: (value["name"].casefold(), value["steamid"]))


def prepare_follow_draft(demo, player, start_tick, end_tick, options=None, *, cancel_event=None):
    """Bake and fingerprint the exact recording for the native project library."""
    _cancelled(cancel_event)
    path = Path(demo).expanduser().resolve()
    original = _fingerprint(path)
    # Stat timestamps can remain unchanged after a rapid same-size rewrite on
    # NTFS. Compare content around parsing as well as the file's identity, so a
    # recording edited during the worker cannot mix old camera data with a new
    # recording hash.
    original_sha256 = _hash_recording(path, original, cancel_event=cancel_event)
    result = bake_demo_follow(demo, player, start_tick, end_tick, options)
    _cancelled(cancel_event)
    source = result["evidence"]["source"]
    path = Path(source["demo"])
    before = _fingerprint(path)
    if (before != original or before[2:4] != (source["size"], source["mtime_ns"])
            or source.get("file_identity") != _fingerprint_evidence(before)):
        raise ValueError("Demo changed after baking; regenerate the draft")
    source["sha256"] = _hash_recording(path, before, cancel_event=cancel_event)
    if source["sha256"] != original_sha256:
        raise ValueError("Demo changed while fingerprinting the draft; regenerate the draft")
    return result


def save_follow_draft(draft, directory, *, project_id=None, cancel_event=None):
    """Publish a new library project + metadata + evidence, never overwrite.

    Metadata/evidence are published first and the project appears last. On any
    failure the files created by this call are removed. The portable JSON has
    no new schema fields; the .meta is compatible with native ProjectStore.
    """
    _cancelled(cancel_event)
    # Detach input before validating/writing so later UI changes cannot alter
    # either the saved keys or their associated provenance.
    draft = json.loads(json.dumps(draft, ensure_ascii=False, allow_nan=False))
    project, evidence = draft["project"], draft["evidence"]
    LiveTrack(project).validate_capture_fov()
    if (evidence.get("adapter") != "recorded_demo_follow" or evidence.get("status") != "draft_unverified"
            or evidence.get("geometry_checked") is not False or evidence.get("visual_verified") is not False):
        raise ValueError("Save requires the generated, unverified Follow draft and its evidence")
    source = evidence["source"]
    if not re.fullmatch(r"[0-9a-f]{64}", source.get("sha256", "")):
        raise ValueError("Follow draft has no recording SHA256; regenerate it before saving")
    demo = Path(source["demo"])
    if not demo.is_absolute() or len(str(demo).encode("utf-8")) > 4096:
        raise ValueError("Follow draft recording path is invalid")
    fingerprint = _fingerprint(demo)
    if (fingerprint[2:4] != (source["size"], source["mtime_ns"])
            or source.get("file_identity") != _fingerprint_evidence(fingerprint)):
        raise ValueError("Demo changed since this draft was generated; regenerate before saving")
    if _hash_recording(demo, fingerprint, cancel_event=cancel_event) != source["sha256"]:
        raise ValueError("Demo content changed since this draft was generated; regenerate before saving")
    project_id = project_id or "follow-" + uuid.uuid4().hex
    if not isinstance(project_id, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}", project_id):
        raise ValueError("Follow project ID must contain 1..128 ASCII letters, digits, underscores or hyphens")
    if project_id == "last-project":
        raise ValueError("Follow drafts cannot replace the editor's recovery project")
    directory = Path(directory).expanduser().resolve(strict=True)
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    published, temporary = [], []
    try:
        info = os.fstat(descriptor)
        if info.st_uid != os.getuid() or not stat.S_ISDIR(info.st_mode):
            raise ValueError("Follow drafts require a directory owned by the current user")
        metadata = {"version": 1, "id": project_id, "title": project["name"], "saved_at_ns": time.time_ns(), "opened_at_ns": 0,
                    "demo": {"path": str(demo), "fingerprint": {"sha256": source["sha256"], "bytes": source["size"], "mtime_ns": source["mtime_ns"]}}}
        documents = [(project_id+".meta", metadata), (project_id+".evidence", evidence), (project_id+".json", project)]
        for name, value in documents:
            _cancelled(cancel_event)
            if _fingerprint(demo) != fingerprint:
                raise ValueError("Demo changed while saving this draft; regenerate before saving")
            contents = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)+"\n").encode("utf-8")
            limit = 16384 if name.endswith(".meta") else 4*1024*1024
            if len(contents) > limit:
                raise ValueError("Follow draft exceeds the native library's file size limit")
            temp = ".follow-" + uuid.uuid4().hex + ".tmp"
            fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600, dir_fd=descriptor)
            temporary.append(temp)
            with os.fdopen(fd, "wb") as handle:
                handle.write(contents)
                handle.flush()
                os.fsync(handle.fileno())
            _cancelled(cancel_event)
            # linkat is atomic and fails on any existing destination, including
            # symlinks. A concurrent save can never silently replace a project.
            os.link(temp, name, src_dir_fd=descriptor, dst_dir_fd=descriptor, follow_symlinks=False)
            published.append((name, os.stat(name, dir_fd=descriptor, follow_symlinks=False).st_ino))
            os.unlink(temp, dir_fd=descriptor)
            temporary.remove(temp)
        _cancelled(cancel_event)
        if _fingerprint(demo) != fingerprint:
            raise ValueError("Demo changed while saving this draft; regenerate before saving")
        os.fsync(descriptor)
    except BaseException:
        for name, inode in reversed(published):
            try:
                if os.stat(name, dir_fd=descriptor, follow_symlinks=False).st_ino == inode:
                    os.unlink(name, dir_fd=descriptor)
            except FileNotFoundError:
                pass
        raise
    finally:
        for temp in temporary:
            try:
                os.unlink(temp, dir_fd=descriptor)
            except FileNotFoundError:
                pass
        os.close(descriptor)
    return {"project": str(directory/(project_id+".json")), "metadata": str(directory/(project_id+".meta")),
            "evidence": str(directory/(project_id+".evidence")), "id": project_id}


def create_follow_dialog(parent, demo, projects_dir, *, initial_tick=0):
    """Create a modeless Qt dialog; the caller retains it and calls show().

    ``draftSaved(str)`` emits the successfully saved portable JSON path. No
    game command is issued; the current live project is never replaced here.
    """
    from PySide6 import QtCore, QtWidgets

    class FollowDialog(QtWidgets.QDialog):
        draftSaved = QtCore.Signal(str)

        def __init__(self):
            super().__init__(parent)
            self.setWindowTitle("Follow from demo")
            self.setObjectName("follow_dialog")
            self.setModal(False)
            self.demo = Path(demo).expanduser().resolve()
            self.projects_dir = Path(projects_dir).expanduser().absolute()
            self.draft = self.saved_paths = None
            self.busy = False
            self._closed = False
            self._close_after_cancel = False
            self._generation = 0
            self._operation = None
            self._events = queue.Queue()
            self._cancel = threading.Event()
            outer = QtWidgets.QVBoxLayout(self)
            heading = QtWidgets.QLabel("Create editable camera keys from a recorded player.")
            heading.setWordWrap(True)
            outer.addWidget(heading)
            self.recording = QtWidgets.QLabel(self.demo.name)
            self.recording.setTextFormat(QtCore.Qt.TextFormat.PlainText)
            self.recording.setToolTip(str(self.demo))
            self.recording.setWordWrap(True)
            outer.addWidget(self.recording)
            scroll = QtWidgets.QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setFrameShape(QtWidgets.QFrame.Shape.NoFrame)
            self.form_widget = QtWidgets.QWidget()
            form = QtWidgets.QFormLayout(self.form_widget)
            scroll.setWidget(self.form_widget)
            outer.addWidget(scroll, 1)
            self.player = QtWidgets.QComboBox()
            self.player.setObjectName("follow_player")
            self.player.setSizeAdjustPolicy(QtWidgets.QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
            self.player.setMinimumContentsLength(24)
            self.player.addItem("Reading recorded players…", None)
            form.addRow("Player", self.player)
            self.name = QtWidgets.QLineEdit("Follow draft")
            self.name.setObjectName("follow_name")
            form.addRow("Project name", self.name)
            self.start_tick, self.end_tick = QtWidgets.QSpinBox(), QtWidgets.QSpinBox()
            self.start_tick.setObjectName("follow_start_tick")
            self.end_tick.setObjectName("follow_end_tick")
            self.start_tick.setRange(0, 2_000_000_000-1)
            self.end_tick.setRange(1, 2_000_000_000)
            begin = initial_tick if type(initial_tick) is int and 0 <= initial_tick < 2_000_000_000 else 0
            self.start_tick.setValue(begin)
            self.end_tick.setValue(min(2_000_000_000, begin+3*TICKRATE))
            row = QtWidgets.QHBoxLayout()
            row.addWidget(self.start_tick, 1)
            row.addWidget(QtWidgets.QLabel("→"))
            row.addWidget(self.end_tick, 1)
            form.addRow("Absolute ticks", row)
            self.range_label = QtWidgets.QLabel()
            form.addRow(self.range_label)
            self.mode = QtWidgets.QComboBox()
            self.mode.setObjectName("follow_mode")
            for label, value in (("Follow · look at player", "look_at"), ("Attach · recorded view", "attach"), ("Lock-on · fixed position", "lock_on")):
                self.mode.addItem(label, value)
            form.addRow("Mode", self.mode)
            self.offset_space = QtWidgets.QComboBox()
            self.offset_space.setObjectName("follow_offset_space")
            for label, value in (("Player yaw · forward / left / up", "yaw"), ("Player view · pitch and yaw", "view"), ("World X / Y / Z", "world")):
                self.offset_space.addItem(label, value)
            form.addRow("Offset axes", self.offset_space)
            self.offset, self.offset_widget = self.vector_fields("follow_offset", [-160, 0, 72])
            self.target, self.target_widget = self.vector_fields("follow_target", [0, 0, 48])
            self.position, self.position_widget = self.vector_fields("follow_fixed", [0, 0, 0])
            self.angles, self.angles_widget = self.vector_fields("follow_trim", [0, 0, 0])
            form.addRow("Camera offset", self.offset_widget)
            form.addRow("Aim offset · world XYZ", self.target_widget)
            form.addRow("Fixed camera · XYZ", self.position_widget)
            form.addRow("Local rotation · P / Y / R", self.angles_widget)
            self.fov = QtWidgets.QDoubleSpinBox()
            self.fov.setObjectName("follow_fov")
            self.fov.setRange(30, 140)
            self.fov.setValue(90)
            self.fov.setSuffix("°")
            self.fov.setDecimals(1)
            form.addRow("Field of view", self.fov)
            self.position_smoothing, self.rotation_smoothing = QtWidgets.QDoubleSpinBox(), QtWidgets.QDoubleSpinBox()
            for field, name in ((self.position_smoothing, "follow_position_smoothing"), (self.rotation_smoothing, "follow_rotation_smoothing")):
                field.setObjectName(name)
                field.setRange(0, 1)
                field.setDecimals(3)
                field.setSingleStep(.01)
                field.setSuffix(" s")
                field.setToolTip("Filter half-life in demo time. 0 keeps recorded motion exactly. Smoothing still needs collision review.")
            form.addRow("Position smoothing", self.position_smoothing)
            form.addRow("Rotation smoothing", self.rotation_smoothing)
            self.on_loss = QtWidgets.QComboBox()
            self.on_loss.setObjectName("follow_on_loss")
            self.on_loss.addItem("Stop with an error", "error")
            self.on_loss.addItem("End draft before target is lost", "truncate")
            form.addRow("Death / missing target", self.on_loss)
            notice = QtWidgets.QLabel("Offsets use the player's recorded origin. Collision clearance and rendered visibility require review before use.")
            notice.setWordWrap(True)
            form.addRow(notice)
            self.progress = QtWidgets.QProgressBar()
            self.progress.setTextVisible(False)
            self.progress.setRange(0, 1)
            self.progress.setValue(0)
            outer.addWidget(self.progress)
            self.status = QtWidgets.QLabel("Reading the demo…")
            self.status.setObjectName("follow_status")
            self.status.setTextFormat(QtCore.Qt.TextFormat.PlainText)
            self.status.setWordWrap(True)
            self.status.setTextInteractionFlags(QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)
            outer.addWidget(self.status)
            controls = QtWidgets.QHBoxLayout()
            self.generate_button = QtWidgets.QPushButton("Generate draft")
            self.generate_button.setObjectName("follow_generate")
            self.save_button = QtWidgets.QPushButton("Save draft…")
            self.save_button.setObjectName("follow_save")
            self.close_button = QtWidgets.QPushButton("Close")
            self.close_button.setObjectName("follow_close")
            for button in (self.generate_button, self.save_button, self.close_button):
                button.setAutoDefault(False)
                controls.addWidget(button)
            outer.addLayout(controls)
            self.generate_button.clicked.connect(self.generate)
            self.save_button.clicked.connect(self.save)
            self.close_button.clicked.connect(self.reject)
            self.start_tick.valueChanged.connect(self.change_range)
            self.end_tick.valueChanged.connect(self.change_range)
            self.mode.currentIndexChanged.connect(self.change_mode)
            self.form = form
            for widget in (self.player, self.offset_space, self.on_loss):
                widget.currentIndexChanged.connect(self.invalidate)
            for widget in (*self.offset, *self.target, *self.position, *self.angles, self.fov, self.position_smoothing, self.rotation_smoothing):
                widget.valueChanged.connect(self.invalidate)
            self.name.textChanged.connect(self.invalidate)
            self._timer = QtCore.QTimer(self)
            self._timer.setInterval(30)
            self._timer.timeout.connect(self.poll)
            self._timer.start()
            self.change_range()
            self.change_mode()
            self.set_busy(True)
            recording = self.demo
            self._start("roster", lambda cancel: load_follow_players(recording))
            available = self.screen().availableGeometry()
            self.resize(min(700, available.width()), min(700, available.height()-40))

        def vector_fields(self, name, values):
            widget = QtWidgets.QWidget()
            layout = QtWidgets.QHBoxLayout(widget)
            layout.setContentsMargins(0, 0, 0, 0)
            fields = []
            for axis, value in zip(("x", "y", "z"), values):
                field = QtWidgets.QDoubleSpinBox()
                field.setObjectName(name+"_"+axis)
                field.setRange(-1_000_000, 1_000_000)
                field.setDecimals(2)
                field.setValue(value)
                layout.addWidget(field, 1)
                fields.append(field)
            return fields, widget

        def set_busy(self, busy):
            self.busy = busy
            self.form_widget.setEnabled(not busy)
            self.generate_button.setEnabled(not busy and self.player.currentData() is not None)
            self.save_button.setEnabled(not busy and self.draft is not None and self.saved_paths is None)
            self.close_button.setEnabled(True)
            self.close_button.setText("Cancel" if busy else "Close")
            self.progress.setRange(0, 0 if busy else 1)
            if not busy:
                self.progress.setValue(1 if self.draft else 0)

        def invalidate(self, *unused):
            if self.draft is not None:
                self.draft = self.saved_paths = None
                self.status.setText("Settings changed. Generate a new draft.")
            self.save_button.setEnabled(False)

        def change_range(self, *unused):
            begin = self.start_tick.value()
            old = self.end_tick.blockSignals(True)
            self.end_tick.setRange(begin+1, min(2_000_000_000, begin+MAX_DURATION_SECONDS*TICKRATE))
            self.end_tick.blockSignals(old)
            self.range_label.setText(f"{(self.end_tick.value()-begin)/TICKRATE:.3f} s · end excluded · maximum {MAX_DURATION_SECONDS} s")
            self.invalidate()

        def change_mode(self, *unused):
            mode = self.mode.currentData()
            for field, visible in ((self.offset_space, mode != "lock_on"), (self.offset_widget, mode != "lock_on"),
                                   (self.target_widget, mode != "attach"), (self.position_widget, mode == "lock_on"),
                                   (self.angles_widget, mode == "attach"), (self.position_smoothing, mode != "lock_on")):
                field.setVisible(visible)
                self.form.labelForField(field).setVisible(visible)
            self.invalidate()

        def options(self):
            mode = self.mode.currentData()
            result = {"mode": mode, "name": self.name.text().strip(), "fov": self.fov.value(),
                      "rotation_half_life": self.rotation_smoothing.value(), "on_loss": self.on_loss.currentData()}
            if mode == "lock_on":
                result["camera_position"] = [field.value() for field in self.position]
            else:
                result.update(offset=[field.value() for field in self.offset], offset_space=self.offset_space.currentData(),
                              position_half_life=self.position_smoothing.value())
            result["angle_offset" if mode == "attach" else "look_at_offset"] = [field.value() for field in (self.angles if mode == "attach" else self.target)]
            return result

        def _start(self, kind, function):
            self._generation += 1
            self._operation = kind
            self._close_after_cancel = False
            token = self._generation
            self._cancel = threading.Event()
            cancel, events = self._cancel, self._events
            self.set_busy(True)
            def work():
                try:
                    _cancelled(cancel)
                    result, error = function(cancel), None
                    # Save owns its transaction's cancellation points. Once it
                    # returns, the project is committed and its path must be
                    # reported even if a late Close set the event meanwhile.
                    if kind != "save":
                        _cancelled(cancel)
                except Exception as failure:
                    result, error = None, failure
                events.put((token, kind, result, error))
            threading.Thread(target=work, name="cs2-follow-"+kind, daemon=True).start()

        def generate(self):
            if self.busy or self.player.currentData() is None:
                return
            self.draft = self.saved_paths = None
            demo, player = self.demo, self.player.currentData()
            begin, end, options = self.start_tick.value(), self.end_tick.value(), self.options()
            self.status.setText("Generating camera keys and checking recording identity…")
            self._start("draft", lambda cancel: prepare_follow_draft(demo, player, begin, end, options, cancel_event=cancel))

        def save(self):
            if self.busy or self.draft is None or self.saved_paths is not None:
                return
            selected = QtWidgets.QFileDialog.getExistingDirectory(self, "Save draft in project folder", str(self.projects_dir))
            if not selected:
                return
            draft = self.draft
            self.status.setText("Saving a new draft and its recording evidence…")
            self._start("save", lambda cancel: save_follow_draft(draft, selected, cancel_event=cancel))

        def poll(self):
            while True:
                try:
                    token, kind, result, error = self._events.get_nowait()
                except queue.Empty:
                    return
                if self._closed or token != self._generation:
                    continue
                if error:
                    self.status.setText(str(error))
                elif kind == "roster":
                    self.player.clear()
                    for player in result:
                        self.player.addItem(player["name"]+" · "+player["steamid"], player["steamid"])
                    self.status.setText("Choose a player and an absolute tick range.")
                elif kind == "draft":
                    self.draft = result
                    evidence = result["evidence"]
                    stop = f" Target {evidence['loss']['reason']} at tick {evidence['loss']['tick']}; range shortened." if evidence["loss"] else ""
                    self.status.setText(f"Draft ready: {len(result['project']['keys'])} editable keys, {result['project']['duration']:.3f} s.{stop} Collision clearance and visibility are unverified. Save to review it in the editor.")
                elif kind == "save":
                    self.saved_paths = result
                    self.status.setText("Saved draft: "+result["project"]+"\nOpen this demo and use Ctrl+O to review the draft. Collision clearance and visibility remain unverified.")
                    self.draftSaved.emit(result["project"])
                self.set_busy(False)
                if self._close_after_cancel and isinstance(error, InterruptedError):
                    self.reject()

        def reject(self):
            if self.busy and self._operation == "save":
                self._close_after_cancel = True
                self._cancel.set()
                self.status.setText("Cancelling the draft save…")
                return
            self._closed = True
            self._generation += 1
            self._cancel.set()
            self._timer.stop()
            super().reject()

        def closeEvent(self, event):
            if self.busy and self._operation == "save":
                self.reject()
                event.ignore()
                return
            self.reject()
            event.accept()

    return FollowDialog()
