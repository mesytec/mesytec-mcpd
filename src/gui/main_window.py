from __future__ import annotations

import datetime
import html
import json
import logging
import signal
import sys
from pathlib import Path
from time import monotonic
from typing import Optional

import mesytec_mcpd as mcpd
import pyqtgraph as pg
import pyqtgraph.console
from pyqtgraph.dockarea.Dock import Dock
from pyqtgraph.dockarea.DockArea import DockArea
from pyqtgraph.Qt import QtCore, QtGui, QtWidgets
from pyqtgraph.Qt.QtCore import Signal, Slot

from .commands import format_result
from .config import DeviceConfig, Setup
from .device_panel import DevicePanel
from .device_worker import DeviceWorkers
from .histo_view import HistogramView
from .pulser_test import LabelPrefix as PulserTestLabelPrefix
from .pulser_test import PulserTest, PulserTestPanel
from .stats import DeviceRow, StatsTracker

log = logging.getLogger("mcpd_gui")

StatsInterval_ms = 500
HistoInterval_ms = 250
DrainDelay_ms = 300
SigintPollInterval_ms = 200


def default_setup_path() -> Path:
    import platformdirs

    return platformdirs.user_config_path("mesytec-mcpd") / "mcpd_gui_setup.json"


class LogEmitter(QtCore.QObject):
    message = Signal(int, str)


class QtLogHandler(logging.Handler):
    def __init__(self):
        super().__init__()
        self.emitter = LogEmitter()
        self.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%H:%M:%S"))

    def emit(self, record):
        self.emitter.message.emit(record.levelno, self.format(record))


class DaqPanel(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)

        self.spin_port = QtWidgets.QSpinBox()
        self.spin_port.setRange(0, 65535)
        self.cb_listfile = QtWidgets.QCheckBox("Write listfile")
        self.le_listdir = QtWidgets.QLineEdit()
        self.le_listdir.setPlaceholderText("listfile directory (default: current directory)")
        self.pb_listdir = QtWidgets.QPushButton("...")
        self.pb_listdir.setMaximumWidth(30)
        self.spin_run_id = QtWidgets.QSpinBox()
        self.spin_run_id.setRange(0, 65535)
        self.cb_clear_on_start = QtWidgets.QCheckBox("Clear histograms on start")
        self.cb_clear_on_start.setChecked(True)

        self.pb_start_run = QtWidgets.QPushButton("Start Run")
        self.pb_stop_run = QtWidgets.QPushButton("Stop Run")
        self.pb_continue = QtWidgets.QPushButton("Continue")
        self.pb_reset = QtWidgets.QPushButton("Reset DAQ")
        self.pb_start_run.setToolTip(
            "Start readout, send the run id to all enabled devices and start_daq to the timing master")
        self.pb_stop_run.setToolTip("Send stop_daq to the timing master, then stop the readout")
        self.pb_continue.setToolTip("Send continue_daq to the timing master")
        self.pb_reset.setToolTip("Send reset_daq to all enabled devices")
        self.pb_start_readout = QtWidgets.QPushButton("Start Readout Only")
        self.pb_start_readout.setToolTip("Receive data without sending DAQ commands to devices")
        self.pb_stop_readout = QtWidgets.QPushButton("Stop Readout")
        self.pb_replay = QtWidgets.QPushButton("Replay...")
        self.pb_clear = QtWidgets.QPushButton("Clear Histograms")

        self.label_state = QtWidgets.QLabel("Idle")
        self.label_counters = QtWidgets.QLabel()
        self.label_counters.setTextInteractionFlags(QtCore.Qt.TextInteractionFlag.TextSelectableByMouse)

        listdir = QtWidgets.QHBoxLayout()
        listdir.addWidget(self.le_listdir)
        listdir.addWidget(self.pb_listdir)

        form = QtWidgets.QFormLayout()
        form.addRow("Data port", self.spin_port)
        form.addRow(self.cb_listfile)
        form.addRow(listdir)
        form.addRow("Run id", self.spin_run_id)
        form.addRow(self.cb_clear_on_start)

        grid = QtWidgets.QGridLayout()
        grid.addWidget(self.pb_start_run, 0, 0)
        grid.addWidget(self.pb_stop_run, 0, 1)
        grid.addWidget(self.pb_continue, 1, 0)
        grid.addWidget(self.pb_reset, 1, 1)
        grid.addWidget(self.pb_start_readout, 2, 0)
        grid.addWidget(self.pb_stop_readout, 2, 1)
        grid.addWidget(self.pb_replay, 3, 0)
        grid.addWidget(self.pb_clear, 3, 1)

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(2, 2, 2, 2)
        layout.addLayout(form)
        layout.addLayout(grid)
        layout.addWidget(self.label_state)
        layout.addWidget(self.label_counters)
        layout.addStretch(1)

    def set_state(self, state: str):
        idle = state == "idle"
        self.pb_start_run.setEnabled(idle)
        self.pb_start_readout.setEnabled(idle)
        self.pb_replay.setEnabled(idle)
        self.pb_stop_run.setEnabled(state == "readout")
        self.pb_stop_readout.setEnabled(not idle)
        self.spin_port.setEnabled(idle)
        self.cb_listfile.setEnabled(idle)
        self.le_listdir.setEnabled(idle)


class StatsTable(QtWidgets.QTableWidget):
    Columns = (
        "Device", "Source", "Id", "Type", "Packets", "Packets/s", "Neutrons/s", "MB/s",
        "Lost", "Lost/s", "Seq Jumps", "Triggers", "Run", "Status", "Buffer#", "Notes",
    )

    def __init__(self, parent=None):
        super().__init__(0, len(self.Columns), parent)
        self.setHorizontalHeaderLabels(self.Columns)
        self.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
        self.verticalHeader().hide()
        self.horizontalHeader().setStretchLastSection(True)

    def update_rows(self, rows: list[DeviceRow]):
        self.setRowCount(len(rows))
        warn = QtGui.QBrush(QtGui.QColor(255, 120, 120, 90))
        for r, row in enumerate(rows):
            st = row.stats
            values = (
                row.config.name if row.config else "?",
                row.ip,
                str(row.device_id),
                row.type_name,
                str(st.packets),
                f"{row.packet_rate:.0f}",
                f"{row.event_rate:.0f}",
                f"{row.byte_rate / 1e6:.2f}",
                str(st.packets_lost),
                f"{row.loss_rate:.0f}",
                str(st.buffer_number_jumps),
                str(st.trigger_events),
                str(st.last_run_id),
                f"{st.last_device_status:#04x}",
                str(st.last_buffer_number),
                "; ".join(row.notes),
            )
            for c, text in enumerate(values):
                item = self.item(r, c)
                if item is None:
                    item = QtWidgets.QTableWidgetItem()
                    self.setItem(r, c, item)
                item.setText(text)
                bad = (
                    bool(row.notes)
                    or (c in (8, 9) and st.packets_lost > 0)
                    or (c == 10 and st.buffer_number_jumps > 0)
                )
                item.setBackground(warn if bad else QtGui.QBrush())
        self.resizeColumnsToContents()


# Returns the names of the docks in a DockArea.saveState() result.
def _saved_dock_names(state: dict) -> set[str]:
    def walk(node) -> set[str]:
        kind, content, _ = node
        if kind == "dock":
            return {content}
        return set().union(*(walk(c) for c in content))

    roots = ([state["main"]] if state["main"] is not None else []) + [f[0]["main"] for f in state["float"]]
    return set().union(*(walk(r) for r in roots if r is not None))


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, setup: Setup, setup_path: Optional[Path]):
        super().__init__()
        self.setup = setup
        self.setup_path = setup_path
        self.daq = mcpd.Daq(setup.data_port)
        self.state = "idle"  # idle | readout | replay
        self.stats_tracker = StatsTracker()
        self.rows: list[DeviceRow] = []
        self.workers = DeviceWorkers(self)
        self.histo_views: list[tuple[Dock, HistogramView]] = []
        self._pending_stop: set[int] = set()
        self._current_listfile = ""

        self._setup_ui()
        self._restore_ui_state()
        self._load_setup_into_ui()
        self._set_state("idle")

        self.workers.finished.connect(self._on_command_finished)

        self.stats_timer = QtCore.QTimer(self)
        self.stats_timer.timeout.connect(self._update_stats)
        self.stats_timer.start(StatsInterval_ms)
        self.histo_timer = QtCore.QTimer(self)
        self.histo_timer.timeout.connect(self._update_histograms)
        self.histo_timer.start(HistoInterval_ms)

    # UI setup
    def _setup_ui(self):
        self.setWindowTitle("MPSD DAQ GUI")
        self.resize(1600, 1000)
        self.dock_area = DockArea()
        self.setCentralWidget(self.dock_area)
        self.setStatusBar(QtWidgets.QStatusBar())

        menu_file = self.menuBar().addMenu("&File")
        menu_file.addAction("&New Setup", self._new_setup, QtGui.QKeySequence.StandardKey.New)
        menu_file.addAction("&Open Setup...", self._open_setup)
        menu_file.addAction("&Save Setup", self._save_setup, QtGui.QKeySequence.StandardKey.Save)
        menu_file.addAction("Save Setup &As...", self._save_setup_as)
        menu_file.addSeparator()
        menu_file.addAction("E&xit", self.close, QtGui.QKeySequence("Ctrl+Q"))
        menu_view = self.menuBar().addMenu("&View")
        menu_view.addAction("New &Histogram View", lambda: self._add_histo_view())
        menu_view.addSeparator()
        menu_view.addAction("&Reset UI to Defaults", self._reset_ui)

        self.daq_panel = DaqPanel()
        self.device_panel = DevicePanel(self.setup, self.workers)
        self.pulser_test = PulserTest(self.workers, lambda: self.setup.devices, self)
        self.pulser_test_panel = PulserTestPanel(self.pulser_test)
        self.stats_table = StatsTable()

        self.log_view = QtWidgets.QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(10000)
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.SystemFont.FixedFont)
        self.log_view.setFont(font)
        self.log_view.setContextMenuPolicy(QtCore.Qt.ContextMenuPolicy.CustomContextMenu)
        self.log_view.customContextMenuRequested.connect(self._log_context_menu)
        self.log_handler = QtLogHandler()
        self.log_handler.emitter.message.connect(self._append_log)
        logging.getLogger().addHandler(self.log_handler)

        self.console = pyqtgraph.console.ConsoleWidget(
            namespace={"mcpd": mcpd, "daq": self.daq, "mainwin": self, "setup": self.setup}
        )

        self.dock_daq = Dock("DAQ", size=(350, 300))
        self.dock_daq.addWidget(self.daq_panel)
        self.dock_devices = Dock("Devices", size=(350, 700))
        self.dock_devices.addWidget(self.device_panel)
        self.dock_pulser_test = Dock("Pulser Test", size=(350, 150))
        self.dock_pulser_test.addWidget(self.pulser_test_panel)
        self.dock_stats = Dock("Statistics", size=(1200, 200))
        self.dock_stats.addWidget(self.stats_table)
        self.dock_log = Dock("Log", size=(600, 200), autoOrientation=False)
        self.dock_log.addWidget(self.log_view)
        self.dock_console = Dock("Console", size=(1200, 200))
        self.dock_console.addWidget(self.console)

        self._build_default_layout()

        dp = self.daq_panel
        dp.pb_start_run.clicked.connect(self.start_run)
        dp.pb_stop_run.clicked.connect(self.stop_run)
        dp.pb_continue.clicked.connect(self.continue_run)
        dp.pb_reset.clicked.connect(lambda: self._send_to_enabled("reset_daq"))
        dp.pb_start_readout.clicked.connect(lambda: self.start_readout(write_listfile=False))
        dp.pb_stop_readout.clicked.connect(self.stop_readout)
        dp.pb_replay.clicked.connect(self._start_replay)
        dp.pb_clear.clicked.connect(self._clear_histograms)
        dp.pb_listdir.clicked.connect(self._choose_listdir)
        dp.spin_port.valueChanged.connect(lambda v: setattr(self.setup, "data_port", v))
        dp.cb_listfile.toggled.connect(lambda v: setattr(self.setup, "write_listfile", v))
        dp.le_listdir.textChanged.connect(lambda v: setattr(self.setup, "listfile_dir", v))

    def _build_default_layout(self):
        self.dock_area.addDock(self.dock_daq, "left")
        self.dock_area.addDock(self.dock_devices, "bottom", self.dock_daq)
        self.dock_area.addDock(self.dock_pulser_test, "bottom", self.dock_devices)
        self.dock_area.addDock(self.dock_stats, "right")
        self.dock_area.addDock(self.dock_console, "above", self.dock_stats)
        self.dock_stats.raiseDock()

        self._add_histo_view().restore_state(dict(type="xy", mode="bus"))
        self._add_histo_view("right").restore_state(dict(type="amplitude", mode="overview"))
        self.dock_area.addDock(self.dock_log, "right", self.dock_stats)

    def _add_histo_view(self, position: str = "bottom", name: Optional[str] = None):
        view = HistogramView()
        if name is None:
            used = {d.name() for d, _ in self.histo_views}
            name = next(f"Histogram {i}" for i in range(len(used) + 1) if f"Histogram {i}" not in used)
        dock = Dock(name, size=(800, 500), closable=bool(self.histo_views))
        dock.addWidget(view)
        if self.histo_views:
            self.dock_area.addDock(dock, position, self.histo_views[-1][0])
        else:
            self.dock_area.addDock(dock, "top", self.dock_stats)
        self.histo_views.append((dock, view))
        dock.sigClosed.connect(self._on_histo_dock_closed)
        view.set_devices(self._device_choices())
        view.selection_changed.connect(self._update_histograms)
        return view

    def _on_histo_dock_closed(self, dock):
        self.histo_views = [(d, v) for d, v in self.histo_views if d is not dock]

    # UI state persistence
    def _save_ui_state(self):
        state = dict(
            geometry=bytes(self.saveGeometry().toBase64()).decode(),
            histograms=[dict(name=d.name(), **v.save_state()) for d, v in self.histo_views],
            docks=self.dock_area.saveState(),
        )
        QtCore.QSettings().setValue("ui_state", json.dumps(state))

    def _restore_ui_state(self):
        text = QtCore.QSettings().value("ui_state", "")
        if not text:
            return
        try:
            state = json.loads(text)
            self._close_histo_views()
            for h in state["histograms"]:
                self._add_histo_view(name=h["name"]).restore_state(h)
            self.dock_area.restoreState(state["docks"], missing="ignore")
            # Docks not in the saved state end up at the bottom spanning the whole width.
            if self.dock_pulser_test.name() not in _saved_dock_names(state["docks"]):
                self.dock_area.moveDock(self.dock_pulser_test, "bottom", self.dock_devices)
            self.restoreGeometry(QtCore.QByteArray.fromBase64(state["geometry"].encode()))
        except Exception as e:
            log.warning(f"Failed to restore ui state, using defaults: {e}")
            self._reset_ui()

    def _close_histo_views(self):
        for dock, _ in list(self.histo_views):
            dock.close()
        self.histo_views.clear()

    def _reset_ui(self):
        self._close_histo_views()
        self._build_default_layout()
        self.resize(1600, 1000)

    def _load_setup_into_ui(self):
        self.pulser_test_panel.stop()
        dp = self.daq_panel
        dp.spin_port.setValue(self.setup.data_port)
        dp.cb_listfile.setChecked(self.setup.write_listfile)
        dp.le_listdir.setText(self.setup.listfile_dir)
        self.device_panel.set_setup(self.setup)
        self.console.localNamespace["setup"] = self.setup
        self.setWindowTitle(f"MPSD DAQ GUI - {self.setup_path}" if self.setup_path else "MPSD DAQ GUI")

    @Slot(int, str)
    def _append_log(self, level: int, text: str):
        if level >= logging.WARNING:
            color = "#d03030" if level >= logging.ERROR else "#c08000"
            self.log_view.appendHtml(f'<span style="color:{color}">{html.escape(text)}</span>')
        else:
            self.log_view.appendPlainText(text)

    def _log_context_menu(self, pos: QtCore.QPoint):
        menu = self.log_view.createStandardContextMenu()
        menu.addSeparator()
        menu.addAction("Clear", self.log_view.clear)
        menu.exec(self.log_view.mapToGlobal(pos))

    def _set_state(self, state: str):
        self.state = state
        self.daq_panel.set_state(state)
        text = {"idle": "Idle", "readout": "Readout running", "replay": "Replay running"}[state]
        if state == "readout" and self._current_listfile:
            text += f"\nListfile: {self._current_listfile}"
        self.daq_panel.label_state.setText(text)
        self.statusBar().showMessage(text.splitlines()[0])

    # Setup persistence
    def _setup_dialog_dir(self) -> str:
        d = QtCore.QSettings().value("last_setup_dir", "")
        if d and Path(d).is_dir():
            return d
        return QtCore.QStandardPaths.writableLocation(QtCore.QStandardPaths.StandardLocation.DocumentsLocation)

    def _remember_setup_dir(self, path: str):
        QtCore.QSettings().setValue("last_setup_dir", str(Path(path).parent))

    def _remember_setup_path(self):
        QtCore.QSettings().setValue("last_setup_path", str(self.setup_path))

    def _new_setup(self):
        self.setup = Setup()
        self.setup_path = None
        self._load_setup_into_ui()
        log.info("Created new setup")

    def _open_setup(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Open Setup", self._setup_dialog_dir(), "Setup (*.json)")
        if not path:
            return
        self._remember_setup_dir(path)
        try:
            self.setup = Setup.load(Path(path))
        except Exception as e:
            log.error(f"Failed to load setup {path}: {e}")
            return
        self.setup_path = Path(path)
        self._remember_setup_path()
        self._load_setup_into_ui()
        log.info(f"Loaded setup {path}")

    def _save_setup(self):
        if self.setup_path is None:
            return self._save_setup_as()
        try:
            self.setup.save(self.setup_path)
            self._remember_setup_path()
            log.info(f"Saved setup to {self.setup_path}")
        except Exception as e:
            log.error(f"Failed to save setup {self.setup_path}: {e}")

    def _save_setup_as(self):
        path, _ = QtWidgets.QFileDialog.getSaveFileName(self, "Save Setup", self._setup_dialog_dir(), "Setup (*.json)")
        if path:
            self._remember_setup_dir(path)
            self.setup_path = Path(path)
            self._save_setup()
            self._load_setup_into_ui()

    def _choose_listdir(self):
        path = QtWidgets.QFileDialog.getExistingDirectory(self, "Listfile Directory", self.setup.listfile_dir)
        if path:
            self.daq_panel.le_listdir.setText(path)

    # Readout and run control
    def _ensure_daq_port(self):
        if self.daq.listen_port != self.setup.data_port:
            self.daq = mcpd.Daq(self.setup.data_port)
            self.console.localNamespace["daq"] = self.daq
            self.stats_tracker.reset()

    def _make_listfile_path(self) -> str:
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        name = f"run{self.daq_panel.spin_run_id.value():05d}_{ts}.mcpdlst"
        return str(Path(self.setup.listfile_dir or ".") / name)

    def start_readout(self, write_listfile: bool) -> bool:
        self._ensure_daq_port()
        self._current_listfile = self._make_listfile_path() if write_listfile else ""
        try:
            self.daq.start_readout(self._current_listfile)
        except Exception as e:
            log.error(f"Failed to start readout: {e}")
            self._current_listfile = ""
            return False
        log.info(f"Readout started on port {self.daq.local_port}"
                 + (f", writing {self._current_listfile}" if self._current_listfile else ""))
        self._set_state("readout")
        return True

    def stop_readout(self):
        self.daq.stop()
        log.info("Readout stopped" + (f", listfile {self._current_listfile}" if self._current_listfile else ""))
        self._current_listfile = ""
        self._set_state("idle")
        self._update_stats()
        self._update_histograms()

    def _enabled_devices(self) -> list[DeviceConfig]:
        return [d for d in self.setup.devices if d.enabled]

    def _send_to_enabled(self, method: str, *args, label: Optional[str] = None) -> list[DeviceConfig]:
        devices = self._enabled_devices()
        if not devices:
            log.warning("No enabled devices")
        for cfg in devices:
            self.workers.submit(cfg, label or method, lambda conn, m=method: getattr(conn, m)(*args))
        return devices

    def _master_device(self) -> Optional[DeviceConfig]:
        devices = self._enabled_devices()
        masters = [d for d in devices if d.timing_role == "Master"]
        if len(masters) != 1:
            names = ", ".join(d.name for d in masters) or "none"
            log.error(f"Need exactly one enabled timing master, have {len(masters)} ({names})")
            return None
        return masters[0]

    def _send_to_master(self, method: str, label: Optional[str] = None) -> Optional[DeviceConfig]:
        if (master := self._master_device()) is not None:
            self.workers.submit(master, label or method, lambda conn: getattr(conn, method)())
        return master

    def start_run(self):
        if self._master_device() is None:
            return
        if not self.start_readout(self.setup.write_listfile):
            return
        self.daq.reset_stats()
        self.stats_tracker.reset()
        if self.daq_panel.cb_clear_on_start.isChecked():
            self.daq.clear_histograms()
        run_id = self.daq_panel.spin_run_id.value()
        self._send_to_enabled("set_run_id", run_id, label=f"set_run_id({run_id})")
        self._send_to_master("start_daq")

    def continue_run(self):
        if self._master_device() is None:
            return
        if self.state == "idle" and not self.start_readout(self.setup.write_listfile):
            return
        self._send_to_master("continue_daq")

    def stop_run(self):
        master = self._send_to_master("stop_daq", label="run:stop_daq")
        self._pending_stop = {id(master)} if master is not None else set()
        if not self._pending_stop:
            self.stop_readout()

    def _start_replay(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "Replay Listfile", self.setup.listfile_dir, "MCPD listfiles (*.mcpdlst);;All files (*)")
        if not path:
            return
        self.daq.clear_sources()
        self.daq.reset_stats()
        self.stats_tracker.reset()
        try:
            self.daq.start_replay(path)
        except Exception as e:
            log.error(f"Failed to start replay: {e}")
            return
        log.info(f"Replaying {path}")
        self._set_state("replay")

    def _clear_histograms(self):
        self.daq.clear_histograms()
        self._update_histograms()

    @Slot(object, str, object, object, object)
    def _on_command_finished(self, cfg: DeviceConfig, label: str, result, error, on_success):
        if error is not None:
            log.error(f"{cfg.name}: {label}: {error}")
        else:
            level = logging.DEBUG if label.startswith(PulserTestLabelPrefix) else logging.INFO
            log.log(level, f"{cfg.name}: {label}: {format_result(result)}")
            if on_success is not None:
                on_success(result)

        if label == "run:stop_daq" and id(cfg) in self._pending_stop:
            self._pending_stop.discard(id(cfg))
            if not self._pending_stop and self.state == "readout":
                QtCore.QTimer.singleShot(DrainDelay_ms, self.stop_readout)

    # Periodic updates
    def _device_choices(self) -> list[tuple[tuple[int, int], str, str]]:
        return [
            (row.key, row.label, "mdll" if row.is_mdll else "mcpd")
            for row in self.rows
            if row.is_mdll or row.is_mcpd
        ]

    @Slot()
    def _update_stats(self):
        if self.daq.has_exception():
            try:
                self.daq.rethrow_exception()
            except Exception as e:
                log.error(f"Readout error: {e}")
            self.daq.stop()
            self._set_state("idle")

        # Sync with the daq state, which may also be changed from the console.
        running = self.daq.is_running()
        if self.state != "idle" and not running:
            self.daq.stop()
            log.info("Replay finished" if self.state == "replay" else "Readout stopped")
            self._current_listfile = ""
            self._set_state("idle")
        elif self.state == "idle" and running:
            self._current_listfile = ""
            self._set_state("readout" if self.daq.local_port else "replay")

        self.rows, new_notes = self.stats_tracker.update(
            self.daq.get_source_stats(), self.setup.devices, monotonic()
        )
        for note in new_notes:
            log.warning(note)
        self.stats_table.update_rows(self.rows)

        choices = self._device_choices()
        for _, view in self.histo_views:
            view.set_devices(choices)

        c = self.daq.get_counters()
        self.daq_panel.label_counters.setText(
            f"packets: {c.packets}, invalid: {c.invalid_packets}, timeouts: {c.timeouts}\n"
            f"received: {c.bytes / 1e6:.1f} MB, listfile: {c.listfile_bytes / 1e6:.1f} MB"
        )

    @Slot()
    def _update_histograms(self):
        cache = {}
        for _, view in self.histo_views:
            if not view.isVisible() or (key := view.device_key()) is None:
                continue
            kind = view.device_kind()
            if (kind, key) not in cache:
                get = self.daq.get_mdll_histograms if kind == "mdll" else self.daq.get_mcpd_histograms
                cache[(kind, key)] = get(*key)
            view.update_histograms(cache[(kind, key)])

    def closeEvent(self, event: QtGui.QCloseEvent):
        self.stats_timer.stop()
        self.histo_timer.stop()
        self.daq.stop()
        pulser_test_running = self.pulser_test.is_running()
        self.pulser_test.stop()
        self.workers.shutdown(drain=pulser_test_running)
        logging.getLogger().removeHandler(self.log_handler)
        self._save_ui_state()
        if self.setup_path is not None:
            try:
                self.setup.save(self.setup_path)
            except Exception as e:
                print(f"Failed to save setup {self.setup_path}: {e}", file=sys.stderr)
        super().closeEvent(event)


def add_qt_font(font_path: str) -> Optional[QtGui.QFont]:
    f = QtCore.QFile(font_path)
    try:
        f.open(QtCore.QIODevice.OpenModeFlag.ReadOnly)
        if f.isOpen():
            font_id = QtGui.QFontDatabase.addApplicationFontFromData(f.readAll())
            if font_id >= 0:
                return QtGui.QFont(QtGui.QFontDatabase.applicationFontFamilies(font_id)[0])
        return None
    finally:
        f.close()


def main():
    import argparse

    parser = argparse.ArgumentParser(description="mesytec MPSD DAQ GUI")
    parser.add_argument("setup", nargs="?",
                        help=f"setup file (default: last used setup or {default_setup_path()})")
    parser.add_argument("--log-level", default="info")
    args = parser.parse_args()

    logging.basicConfig(level=args.log_level.upper(), format="%(name)s %(message)s")
    mcpd.set_log_level(args.log_level)

    app = pg.mkQApp("mesytec-mcpd-gui")
    app.setOrganizationName("mesytec")

    from . import resources  # noqa: F401  registers the embedded fonts

    roboto = add_qt_font(":/fonts/Roboto-VariableFont_wdth,wght.ttf")
    if roboto is not None:
        roboto.setPointSizeF(roboto.pointSizeF() * 0.8)
        app.setFont(roboto)

    pg.setConfigOptions(antialias=True, imageAxisOrder="row-major")

    if args.setup:
        setup_path = Path(args.setup)
    else:
        last = QtCore.QSettings().value("last_setup_path", "")
        setup_path = Path(last) if last and Path(last).exists() else default_setup_path()
    setup = Setup()
    if setup_path.exists():
        try:
            setup = Setup.load(setup_path)
        except Exception as e:
            logging.error(f"Failed to load setup {setup_path}: {e}")

    mainwin = MainWindow(setup, setup_path)
    mainwin.show()

    # Ctrl-C closes the window, running the regular shutdown. The timer returns control
    # to the interpreter periodically, otherwise python signal handlers never run.
    signal.signal(signal.SIGINT, lambda *_: mainwin.close())
    sigint_timer = QtCore.QTimer()
    sigint_timer.timeout.connect(lambda: None)
    sigint_timer.start(SigintPollInterval_ms)

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
