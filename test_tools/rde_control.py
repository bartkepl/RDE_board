# -*- coding: utf-8 -*-
"""
rde_control.py – GUI control application for RDE_board (Resistance DEcade)
Supports VXI-11/Ethernet and USB-TMC via PyVISA.
Calibration with HP 34401A, Agilent 34970A, Keithley 2000/2002 multimeters.
PDF calibration report via fpdf2.

Requirements:
    pip install pyvisa pyvisa-py ttkthemes fpdf2
    # Windows USB-TMC: pip install pyusb  (+ Zadig libusb driver)
"""

from __future__ import annotations

import abc
import dataclasses
import datetime
import ipaddress
import math
import os
import shutil
import socket
import statistics
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk
from typing import Callable, List, Optional

try:
    import pyvisa
    VISA_AVAILABLE = True
except ImportError:
    VISA_AVAILABLE = False

try:
    from ttkthemes import ThemedTk
    THEMES_AVAILABLE = True
except ImportError:
    THEMES_AVAILABLE = False

try:
    from fpdf import FPDF
    FPDF_AVAILABLE = True
except ImportError:
    FPDF_AVAILABLE = False

# ─── Constants ────────────────────────────────────────────────────────────────

APP_TITLE   = "RDE Control"
APP_VERSION = "1.0"
WIN_MIN_W   = 900
WIN_MIN_H   = 640

COL_OK   = "#22aa44"
COL_ERR  = "#cc3333"
COL_WARN = "#cc8800"
COL_CMD  = "#888888"
COL_INFO = "#3366cc"

DECADE_LABELS   = ["100kΩ (d6)", "10kΩ (d5)", "1kΩ (d4)", "100Ω (d3)", "10Ω (d2)", "1Ω (d1)"]
DECADE_UNITS_MO = [1_000, 10_000, 100_000, 1_000_000, 10_000_000, 100_000_000]

MUX_CARDS_OK   = {"34901A", "34902A"}
MUX_CARDS_WARN = {"34908A"}
MUX_CARDS_BAD  = {"34903A", "34904A", "34905A", "34906A", "34907A"}

NPLC_VALUES   = ["0.02", "0.2", "1", "2", "10", "100"]
DIGITS_VALUES = ["4.5", "5.5", "6.5"]

# ─── SCPI error catalogue ─────────────────────────────────────────────────────

# Subset of SCPI 1999 / IEEE 488.2 error codes with short descriptions.
# The firmware already returns descriptions in the standard format
# (-222,"Data out of range"), but this dict is used as a fallback and
# for local-side annotations.
SCPI_ERRORS: dict = {
      0: "No error",
   -100: "Command error",
   -101: "Invalid character",
   -102: "Syntax error",
   -103: "Invalid separator",
   -104: "Data type error",
   -105: "GET not allowed",
   -108: "Parameter not allowed",
   -109: "Missing parameter",
   -110: "Command header error",
   -111: "Header separator error",
   -112: "Program mnemonic too long",
   -113: "Undefined header",
   -114: "Header suffix out of range",
   -115: "Unexpected number of parameters",
   -200: "Execution error",
   -220: "Parameter error",
   -221: "Settings conflict",
   -222: "Data out of range",
   -223: "Too much data",
   -224: "Illegal parameter value",
   -240: "Hardware error",
   -241: "Hardware missing",
   -300: "Device error",
   -310: "System error",
   -311: "Memory error",
   -313: "Calibration memory lost",
   -315: "Configuration memory lost",
   -320: "Storage fault",
   -330: "Self-test failed",
   -340: "Calibration failed",
   -350: "Queue overflow",
   -360: "Communication error",
   -363: "Input buffer overrun",
   -400: "Query error",
   -410: "Query interrupted",
   -420: "Query unterminated",
   -430: "Query deadlocked",
}


def parse_scpi_error(response: str) -> "tuple[int, str]":
    """
    Parse a SCPI SYSTem:ERRor response into (code, description).

    Standard SCPI format: -222,"Data out of range"
    Also handles:         0,"No error"   or bare integer  -222
    Falls back to SCPI_ERRORS catalogue if the quoted string is absent.
    """
    s = response.strip()
    comma = s.find(',')
    if comma > 0:
        try:
            code = int(s[:comma].strip())
            desc = s[comma + 1:].strip().strip('"').strip("'").strip()
            if not desc:
                desc = SCPI_ERRORS.get(code, f"Unknown error {code}")
            return code, desc
        except ValueError:
            pass
    try:
        code = int(s)
        return code, SCPI_ERRORS.get(code, f"Unknown error {code}")
    except ValueError:
        pass
    return 0, "No error"

# ─── Shared VISA ResourceManager ─────────────────────────────────────────────
# One RM per process; creating multiple RMs with some VISA backends (NI-VISA)
# can invalidate existing sessions when a new RM is opened or closed.

_visa_rm: Optional["pyvisa.ResourceManager"] = None

def _get_visa_rm() -> "pyvisa.ResourceManager":
    """Shared process-wide VISA RM.
    Priority: NI-VISA (system default) → pyvisa-py (@py).
    Keysight/Agilent IO Libraries are deliberately skipped."""
    global _visa_rm
    if _visa_rm is not None:
        return _visa_rm

    # 1. Try the system default — accept it only if it is NOT Keysight/Agilent
    try:
        rm = pyvisa.ResourceManager()
        lib = str(getattr(rm.visalib, 'library_path', '')).lower()
        if 'keysight' not in lib and 'agilent' not in lib:
            _visa_rm = rm
            return _visa_rm
        try:
            rm.close()
        except Exception:
            pass
    except Exception:
        pass

    # 2. Fall back to pure-Python backend (pyvisa-py)
    try:
        _visa_rm = pyvisa.ResourceManager('@py')
        return _visa_rm
    except Exception:
        pass

    # 3. Last resort — accept whatever is available (including Keysight)
    _visa_rm = pyvisa.ResourceManager()
    return _visa_rm


# ─── Transport abstraction (extensible for future COM/RS485) ──────────────────

class BaseTransport(abc.ABC):
    @abc.abstractmethod
    def connect(self, address: str, timeout_ms: int = 5000) -> None: ...
    @abc.abstractmethod
    def disconnect(self) -> None: ...
    @abc.abstractmethod
    def query(self, cmd: str) -> str: ...
    @abc.abstractmethod
    def write(self, cmd: str) -> None: ...
    @abc.abstractmethod
    def is_connected(self) -> bool: ...


class PyVisaTransport(BaseTransport):
    def __init__(self):
        self._rm   = None
        self._inst = None

    @staticmethod
    def list_resources() -> tuple:
        if not VISA_AVAILABLE:
            return ()
        try:
            return _get_visa_rm().list_resources()
        except Exception:
            return ()

    def connect(self, address: str, timeout_ms: int = 5000) -> None:
        self._inst = _get_visa_rm().open_resource(address)
        self._inst.timeout = timeout_ms

    def disconnect(self) -> None:
        try:
            if self._inst:
                self._inst.close()
        except Exception:
            pass
        self._inst = None
        self._rm   = None

    def query(self, cmd: str) -> str:
        return self._inst.query(cmd).strip()

    def write(self, cmd: str) -> None:
        self._inst.write(cmd)

    def is_connected(self) -> bool:
        return self._inst is not None


# ─── SCPIDevice ───────────────────────────────────────────────────────────────

class SCPIDevice:
    def __init__(self):
        self._transport: Optional[BaseTransport] = None

    def connect(self, address: str, timeout_ms: int = 5000) -> None:
        t = PyVisaTransport()
        t.connect(address, timeout_ms)
        self._transport = t

    def disconnect(self) -> None:
        if self._transport:
            self._transport.disconnect()
            self._transport = None

    def is_connected(self) -> bool:
        return self._transport is not None and self._transport.is_connected()

    def query(self, cmd: str) -> str:
        return self._transport.query(cmd)

    def write(self, cmd: str) -> None:
        self._transport.write(cmd)

    @staticmethod
    def list_resources() -> tuple:
        return PyVisaTransport.list_resources()

    @staticmethod
    def parse_float(text: str) -> Optional[float]:
        try:
            return float(text.strip())
        except (ValueError, AttributeError):
            return None

    @staticmethod
    def parse_int(text: str) -> Optional[int]:
        try:
            return int(text.strip())
        except (ValueError, AttributeError):
            return None


# ─── MultimeterDevice ─────────────────────────────────────────────────────────

METER_MODELS = {
    "HP 34401A":      {"4W": True, "nplc": True, "mux": False, "digits": True},
    "Agilent 34970A": {"4W": True, "nplc": True, "mux": True,  "digits": False},
    "Keithley 2000":  {"4W": True, "nplc": True, "mux": False, "digits": True},
    "Keithley 2002":  {"4W": True, "nplc": True, "mux": False, "digits": True},
}


class MultimeterDevice:
    def __init__(self):
        self._transport: Optional[PyVisaTransport] = None
        self.model:      str   = "HP 34401A"
        self.mode_4w:    bool  = True
        self.nplc:       float = 10.0
        self.digits:     float = 5.5
        self.avg_count:  int   = 1
        self.slot:       int   = 1
        self.channel:    int   = 1
        self.idn:        str   = ""

    def connect(self, address: str, timeout_ms: int = 8000) -> None:
        t = PyVisaTransport()
        t.connect(address, timeout_ms)
        self._transport = t
        self.idn = t.query("*IDN?")

    def disconnect(self) -> None:
        if self._transport:
            self._transport.disconnect()
            self._transport = None
        self.idn = ""

    def is_connected(self) -> bool:
        return self._transport is not None and self._transport.is_connected()

    def scan_34970a_slots(self) -> dict:
        """Returns {1: 'card_type', 2: ..., 3: ...}"""
        result = {}
        for slot in (1, 2, 3):
            try:
                resp = self._transport.query(f"SYSTem:CTYPe? {slot}").strip()
                card = resp.split(",")[0].strip().upper()
                result[slot] = card if card else "EMPTY"
            except Exception:
                result[slot] = "ERROR"
        return result

    def configure(self) -> None:
        """Send configuration commands to multimeter."""
        t = self._transport
        func = "FRES" if self.mode_4w else "RES"
        if self.model == "Agilent 34970A":
            ch = f"(@{self.slot}{self.channel:02d})"
            t.write(f"CONF:{func} DEF,DEF,{ch}")
            t.write(f"SENS:{func}:NPLC {self.nplc},{ch}")
        else:
            t.write(f"CONF:{func}")
            t.write(f"SENS:{func}:NPLC {self.nplc}")

    def measure_once(self) -> float:
        """Trigger one measurement and return Ω."""
        resp = self._transport.query("READ?")
        return float(resp.strip().split(",")[0])

    def measure(self) -> float:
        """Configure + average measurements, return Ω."""
        self.configure()
        readings = []
        for _ in range(max(1, self.avg_count)):
            readings.append(self.measure_once())
        return statistics.mean(readings)


# ─── AppState ─────────────────────────────────────────────────────────────────

@dataclasses.dataclass
class AppState:
    rel_en:      bool = False
    output_on:   bool = False
    cal_enabled: bool = False
    calibrating: bool = False
    rde_idn:     str  = ""
    decade_vals: List[int] = dataclasses.field(default_factory=lambda: [0]*6)
    # Cal table [decade_idx 0-5][digit 0-9] in milliohms
    cal_table:   List[List[int]] = dataclasses.field(
        default_factory=lambda: [[d * u for d in range(10)]
                                 for u in DECADE_UNITS_MO])
    meter:       MultimeterDevice = dataclasses.field(default_factory=MultimeterDevice)


# ─── _ScpiMixin ───────────────────────────────────────────────────────────────

class _ScpiMixin:
    _device:     SCPIDevice
    _log_pane:   "LogPane"
    _status_bar: "StatusBar"
    _root:       tk.Tk

    def safe_query(self, cmd: str) -> Optional[str]:
        self._log_pane.log(f">> {cmd}", "cmd")
        self._status_bar.set_last(cmd)
        try:
            resp = self._device.query(cmd)
            self._log_pane.log(f"<< {resp}", "ok")
            return resp
        except Exception as exc:
            self._log_pane.log(f"ERROR: {exc}", "error")
            return None

    def safe_write(self, cmd: str) -> bool:
        self._log_pane.log(f">> {cmd}", "cmd")
        self._status_bar.set_last(cmd)
        try:
            self._device.write(cmd)
            self._log_pane.log("(sent)", "ok")
            return True
        except Exception as exc:
            self._log_pane.log(f"ERROR: {exc}", "error")
            self._root.after(0, lambda e=str(exc): messagebox.showerror("SCPI Error", e))
            return False

    def require_connection(self) -> bool:
        if not self._device.is_connected():
            messagebox.showwarning("Not connected", "Connect to RDE first.")
            return False
        return True

    def drain_scpi_errors(self, max_errors: int = 16,
                           silent_if_empty: bool = True) -> int:
        """
        Read SYSTem:ERRor:NEXT? in a loop until the queue is empty.
        Logs every error with numeric code and text description.
        Returns the number of errors found (0 = no errors).
        Call from a background thread only.
        """
        count = 0
        for _ in range(max_errors):
            try:
                resp = self._device.query("SYSTem:ERRor:NEXT?")
            except Exception as exc:
                self._log_pane.log(f"  [err drain] transport error: {exc}", "error")
                break
            code, desc = parse_scpi_error(resp)
            if code == 0:
                if not silent_if_empty and count == 0:
                    self._log_pane.log("Error queue: empty — no errors", "ok")
                break
            count += 1
            self._log_pane.log(f"  [{code:+d}] {desc}", "error")
        if count:
            self._log_pane.log(f"  ↑ {count} SCPI error(s) cleared from queue", "warn")
        return count


# ─── StatusBar ────────────────────────────────────────────────────────────────

class StatusBar:
    def __init__(self, parent: tk.Widget):
        frame = ttk.Frame(parent, relief="sunken")
        frame.pack(side="bottom", fill="x")

        self._dot_cv = tk.Canvas(frame, width=16, height=16, highlightthickness=0)
        self._dot_cv.pack(side="left", padx=(4, 2))
        self._dot = self._dot_cv.create_oval(2, 2, 14, 14, fill=COL_ERR, outline="")

        self._conn_lbl = ttk.Label(frame, text="Disconnected", foreground=COL_ERR)
        self._conn_lbl.pack(side="left", padx=(0, 12))

        self._cmd_lbl = ttk.Label(frame, text="", font=("Courier New", 8),
                                  foreground=COL_CMD)
        self._cmd_lbl.pack(side="left")

    def set_connected(self, connected: bool, label: str = "") -> None:
        color = COL_OK if connected else COL_ERR
        text  = label if label else ("Connected" if connected else "Disconnected")
        self._dot_cv.itemconfig(self._dot, fill=color)
        self._conn_lbl.config(text=text, foreground=color)

    def set_last(self, cmd: str) -> None:
        short = cmd if len(cmd) <= 60 else cmd[:57] + "…"
        self._cmd_lbl.config(text=short)


# ─── LogPane ──────────────────────────────────────────────────────────────────

class LogPane:
    def __init__(self, parent: tk.Widget, height: int = 10):
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)

        self._text = scrolledtext.ScrolledText(
            frame, height=height, state="disabled",
            font=("Courier New", 9), wrap=tk.WORD)
        self._text.pack(fill="both", expand=True)

        self._text.tag_configure("ok",   foreground=COL_OK)
        self._text.tag_configure("error",foreground=COL_ERR)
        self._text.tag_configure("warn", foreground=COL_WARN)
        self._text.tag_configure("cmd",  foreground=COL_CMD)
        self._text.tag_configure("info", foreground=COL_INFO)

        ttk.Button(frame, text="Clear", command=self.clear).pack(
            side="right", padx=4, pady=2)

    def log(self, msg: str, tag: str = "") -> None:
        def _append():
            self._text.config(state="normal")
            ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            line = f"[{ts}] {msg}\n"
            self._text.insert(tk.END, line, tag if tag else "")
            self._text.see(tk.END)
            self._text.config(state="disabled")
        try:
            self._text.after(0, _append)
        except tk.TclError:
            pass

    def clear(self) -> None:
        self._text.config(state="normal")
        self._text.delete("1.0", tk.END)
        self._text.config(state="disabled")


# ─── _BaseTab ─────────────────────────────────────────────────────────────────

class _BaseTab(_ScpiMixin):
    def __init__(self, notebook: ttk.Notebook, label: str,
                 state: AppState, device: SCPIDevice,
                 log_pane: LogPane, status_bar: StatusBar, root: tk.Tk):
        self.frame      = ttk.Frame(notebook)
        self._state     = state
        self._device    = device
        self._log_pane  = log_pane
        self._status_bar = status_bar
        self._root      = root
        self._connected_widgets: List[tk.Widget] = []
        notebook.add(self.frame, text=label)
        self._build()

    def _build(self) -> None:
        pass

    def _sync_ui_state(self, connected: bool) -> None:
        s = "normal" if connected else "disabled"
        for w in self._connected_widgets:
            try:
                w.config(state=s)
            except tk.TclError:
                pass


# ─── ConnectionTab ────────────────────────────────────────────────────────────

class ConnectionTab(_BaseTab):
    on_connect_callback:    Optional[Callable[[str], None]] = None
    on_disconnect_callback: Optional[Callable[[], None]]    = None

    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}

        # Protocol selection
        proto_f = ttk.LabelFrame(self.frame, text="Protocol")
        proto_f.pack(fill="x", padx=8, pady=(8, 4))

        ttk.Label(proto_f, text="Interface:").grid(row=0, column=0, **p)
        self._iface_var = tk.StringVar(value="VXI-11 / Ethernet")
        iface_cb = ttk.Combobox(
            proto_f, textvariable=self._iface_var, width=26, state="readonly",
            values=["VXI-11 / Ethernet", "VISA (USB/GPIB/ASRL/…)",
                    "COM / RS485 Modbus  (coming soon)"])
        iface_cb.grid(row=0, column=1, **p)
        iface_cb.bind("<<ComboboxSelected>>", self._on_iface_change)

        ttk.Label(proto_f, text="Address:").grid(row=0, column=2, **p)
        self._addr_var = tk.StringVar(value="TCPIP::192.168.1.6::INSTR")
        self._addr_cb  = ttk.Combobox(proto_f, textvariable=self._addr_var, width=36)
        self._addr_cb.grid(row=0, column=3, **p)

        self._btn_scan = ttk.Button(proto_f, text="Scan", command=self._start_scan)
        self._btn_scan.grid(row=0, column=4, **p)

        self._btn_conn = ttk.Button(proto_f, text="Connect", command=self._connect)
        self._btn_conn.grid(row=0, column=5, **p)

        self._btn_disc = ttk.Button(proto_f, text="Disconnect",
                                    command=self._disconnect, state="disabled")
        self._btn_disc.grid(row=0, column=6, **p)

        # Device info
        info_f = ttk.LabelFrame(self.frame, text="Device Info")
        info_f.pack(fill="x", padx=8, pady=4)

        ttk.Label(info_f, text="IDN:").grid(row=0, column=0, **p)
        self._idn_var = tk.StringVar(value="—")
        ttk.Label(info_f, textvariable=self._idn_var,
                  foreground=COL_INFO, font=("Courier New", 9)).grid(
            row=0, column=1, sticky="w", **p)

        self._btn_idn = ttk.Button(info_f, text="Query *IDN?",
                                   command=self._query_idn, state="disabled")
        self._btn_idn.grid(row=0, column=2, **p)
        self._connected_widgets.append(self._btn_idn)

        # Scan log
        scan_f = ttk.LabelFrame(self.frame, text="Scan / Connection Log")
        scan_f.pack(fill="both", expand=True, padx=8, pady=4)
        self._scan_log = LogPane(scan_f, height=12)

    def _on_iface_change(self, _=None) -> None:
        iface = self._iface_var.get()
        if "VISA" in iface:
            self._addr_var.set("")
            self._btn_scan.config(state="normal")
        elif "coming soon" in iface:
            self._addr_var.set("")
            self._btn_scan.config(state="disabled")
            messagebox.showinfo("Not yet implemented",
                                "This transport is reserved for future use.")
            self._iface_var.set("VXI-11 / Ethernet")
            self._addr_var.set("TCPIP::192.168.1.6::INSTR")
        else:
            self._addr_var.set("TCPIP::192.168.1.6::INSTR")
            self._btn_scan.config(state="normal")

    def _start_scan(self) -> None:
        if not VISA_AVAILABLE:
            self._scan_log.log("pyvisa not installed.", "error")
            return
        self._btn_scan.config(state="disabled")
        if "VISA" in self._iface_var.get():
            self._scan_log.log("Enumerating VISA resources (USB, GPIB, ASRL, …) …", "info")
            threading.Thread(target=self._scan_visa_thread, daemon=True).start()
        else:
            self._scan_log.log("Scanning /24 subnet for VXI-11 instruments …", "info")
            threading.Thread(target=self._scan_thread, daemon=True).start()

    def _scan_thread(self) -> None:
        import concurrent.futures
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
        except Exception:
            self._scan_log.log("Scan ERROR: cannot determine local IP", "error")
            self._root.after(0, lambda: self._btn_scan.config(state="normal"))
            return

        net   = ipaddress.ip_network(local_ip + "/24", strict=False)
        hosts = [str(h) for h in net.hosts()]

        def check(ip):
            try:
                sock = socket.socket()
                sock.settimeout(0.15)
                sock.connect((ip, 111))
                sock.close()
                return ip
            except Exception:
                return None

        with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
            candidates = [ip for ip in ex.map(check, hosts) if ip]

        results = []
        for ip in candidates:
            addr = f"TCPIP::{ip}::INSTR"
            try:
                inst = _get_visa_rm().open_resource(addr)
                inst.timeout = 1000
                try:
                    idn = inst.query("*IDN?").strip()
                finally:
                    inst.close()
                results.append((addr, idn))
                self._scan_log.log(f"Found: {addr}  →  {idn}", "ok")
            except Exception:
                pass

        if not results:
            self._scan_log.log("No VXI-11 instruments found.", "warn")
        else:
            addrs = [r[0] for r in results]
            self._root.after(0, lambda a=addrs: self._set_scan_results(a))
        self._root.after(0, lambda: self._btn_scan.config(state="normal"))

    def _scan_visa_thread(self) -> None:
        # Prefer pyvisa-py backend (@py) — uses pyusb directly, no NI-VISA cache.
        # Falls back to default backend (NI-VISA) if pyvisa-py is not installed.
        addrs = []
        backend_label = ""
        try:
            found = list(_get_visa_rm().list_resources())
            # Filter TCPIP — those are handled by the VXI-11 tab
            addrs = [r for r in found if not r.upper().startswith('TCPIP')]
            backend_label = "VISA"
        except Exception:
            pass

        if not addrs:
            self._scan_log.log("No VISA resources found (USB/GPIB/ASRL).", "warn")
        else:
            self._scan_log.log(f"Backend: {backend_label} — {len(addrs)} resource(s):", "info")
            for addr in addrs:
                self._scan_log.log(f"  {addr}", "ok")
        self._root.after(0, lambda a=addrs: self._set_scan_results(a))
        self._root.after(0, lambda: self._btn_scan.config(state="normal"))

    def _set_scan_results(self, addrs: list) -> None:
        self._addr_cb["values"] = addrs
        if addrs:
            self._addr_var.set(addrs[0])

    def _connect(self) -> None:
        if not VISA_AVAILABLE:
            self._scan_log.log("pyvisa not installed.", "error")
            return
        addr = self._addr_var.get().strip()
        if not addr:
            return
        self._btn_conn.config(state="disabled")
        self._scan_log.log(f"Connecting to {addr} …", "info")
        threading.Thread(target=self._connect_thread, args=(addr,), daemon=True).start()

    def _connect_thread(self, addr: str) -> None:
        try:
            self._device.connect(addr)
            self._root.after(0, self._on_connected)
        except Exception as exc:
            self._root.after(0, lambda e=exc: self._on_connect_error(e))

    def _on_connected(self) -> None:
        self._btn_conn.config(state="disabled")
        self._btn_disc.config(state="normal")
        self._btn_idn.config(state="normal")
        self._status_bar.set_connected(True)
        self._scan_log.log("Connected.", "ok")
        if self.on_connect_callback:
            threading.Thread(target=self._fetch_idn_and_notify, daemon=True).start()

    def _fetch_idn_and_notify(self) -> None:
        try:
            idn = self._device.query("*IDN?")
            self._root.after(0, lambda i=idn: self._idn_var.set(i))
            self._root.after(0, lambda i=idn: self._status_bar.set_connected(True, f"Connected: {i.split(',')[1].strip() if ',' in i else i[:30]}"))
            self._log_pane.log(f"*IDN? → {idn}", "ok")
            if self.on_connect_callback:
                self._root.after(0, lambda i=idn: self.on_connect_callback(i))
        except Exception as exc:
            self._log_pane.log(f"IDN error: {exc}", "warn")
            if self.on_connect_callback:
                self._root.after(0, lambda: self.on_connect_callback(""))

    def _on_connect_error(self, exc: Exception) -> None:
        self._btn_conn.config(state="normal")
        self._scan_log.log(f"Connect ERROR: {exc}", "error")
        messagebox.showerror("Connection failed", str(exc))

    def _disconnect(self) -> None:
        self._device.disconnect()
        self._btn_conn.config(state="normal")
        self._btn_disc.config(state="disabled")
        self._btn_idn.config(state="disabled")
        self._idn_var.set("—")
        self._status_bar.set_connected(False)
        self._scan_log.log("Disconnected.", "warn")
        if self.on_disconnect_callback:
            self.on_disconnect_callback()

    def _query_idn(self) -> None:
        def _do():
            resp = self.safe_query("*IDN?")
            if resp:
                self._root.after(0, lambda r=resp: self._idn_var.set(r))
        threading.Thread(target=_do, daemon=True).start()

    def disconnect_programmatic(self) -> None:
        self._disconnect()


# ─── ResistanceTab ────────────────────────────────────────────────────────────

class ResistanceTab(_BaseTab):
    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}

        # ── Resistance value ──
        val_f = ttk.LabelFrame(self.frame, text="Resistance Value")
        val_f.pack(fill="x", padx=8, pady=(8, 4))

        # Step buttons row
        step_f = ttk.Frame(val_f)
        step_f.pack(fill="x", padx=4, pady=(4, 0))

        for delta, label in ((-1000, "−1k"), (-100, "−100"), (-10, "−10"), (-1, "−1")):
            b = ttk.Button(step_f, text=label, width=6,
                           command=lambda d=delta: self._step(d))
            b.pack(side="left", padx=2)
            self._connected_widgets.append(b)

        self._res_var = tk.StringVar(value="0")
        res_entry = ttk.Entry(step_f, textvariable=self._res_var, width=10,
                              justify="center", font=("Courier New", 11, "bold"))
        res_entry.pack(side="left", padx=6)
        res_entry.bind("<Return>", lambda _: self._set_resistance())
        self._connected_widgets.append(res_entry)

        ttk.Label(step_f, text="Ω").pack(side="left")

        for delta, label in ((1, "+1"), (10, "+10"), (100, "+100"), (1000, "+1k")):
            b = ttk.Button(step_f, text=label, width=6,
                           command=lambda d=delta: self._step(d))
            b.pack(side="left", padx=2)
            self._connected_widgets.append(b)

        btn_row = ttk.Frame(val_f)
        btn_row.pack(fill="x", padx=4, pady=2)
        b_set = ttk.Button(btn_row, text="Set", command=self._set_resistance)
        b_set.pack(side="left", padx=2)
        b_qry = ttk.Button(btn_row, text="Query", command=self._query_resistance)
        b_qry.pack(side="left", padx=2)
        self._connected_widgets += [b_set, b_qry]

        self._slider = ttk.Scale(val_f, from_=0, to=999999, orient="horizontal",
                                 command=self._on_slider)
        self._slider.pack(fill="x", padx=8, pady=(2, 6))
        self._connected_widgets.append(self._slider)

        # ── Output ──
        out_f = ttk.LabelFrame(self.frame, text="Output")
        out_f.pack(fill="x", padx=8, pady=4)

        ttk.Label(out_f, text="Output state:").grid(row=0, column=0, **p)
        self._out_var = tk.StringVar(value="OFF")
        out_cb = ttk.Combobox(out_f, textvariable=self._out_var,
                              values=["ON", "OFF"], state="readonly", width=6)
        out_cb.grid(row=0, column=1, **p)

        b_out_set = ttk.Button(out_f, text="Set", command=self._set_output)
        b_out_set.grid(row=0, column=2, **p)
        b_out_qry = ttk.Button(out_f, text="Query", command=self._query_output)
        b_out_qry.grid(row=0, column=3, **p)
        self._connected_widgets += [out_cb, b_out_set, b_out_qry]

        ttk.Label(out_f, text="(auto-enables REL_EN if OFF when setting ON)",
                  foreground=COL_CMD).grid(row=0, column=4, **p)

        # ── Decade control ──
        dec_f = ttk.LabelFrame(self.frame, text="Decade Control  (RESistance:DECade)")
        dec_f.pack(fill="x", padx=8, pady=4)

        self._decade_vars = [tk.StringVar(value="0") for _ in range(6)]
        for col, (lbl, var) in enumerate(zip(DECADE_LABELS, self._decade_vars)):
            ttk.Label(dec_f, text=lbl, anchor="center").grid(
                row=0, column=col, padx=6, pady=2)
            d_num = 6 - col  # col0→decade6(100kΩ), col5→decade1(1Ω)
            sb = ttk.Spinbox(dec_f, from_=0, to=9, textvariable=var,
                             width=5, justify="center")
            sb.grid(row=1, column=col, padx=4, pady=2)
            sb.bind("<Return>", lambda _, d=d_num: self._decade_set_one(d))
            self._connected_widgets.append(sb)

        btn_dec = ttk.Frame(dec_f)
        btn_dec.grid(row=1, column=6, padx=8)
        b_sa = ttk.Button(btn_dec, text="Set All",   command=self._decade_set_all)
        b_sa.pack(side="left", padx=2)
        b_qa = ttk.Button(btn_dec, text="Query All", command=self._decade_query_all)
        b_qa.pack(side="left", padx=2)
        self._connected_widgets += [b_sa, b_qa]

        ttk.Label(dec_f, text="Spinbox = digit 0–9 per decade.  "
                  "Set All sends 6 × RESistance:DECade.",
                  foreground=COL_CMD).grid(row=2, column=0, columnspan=7,
                                           sticky="w", padx=6, pady=2)

        # ── Live readback ──
        live_f = ttk.LabelFrame(self.frame, text="Live Readback")
        live_f.pack(fill="x", padx=8, pady=(4, 8))

        self._live_var = tk.StringVar(value="—")
        ttk.Label(live_f, textvariable=self._live_var,
                  font=("Courier New", 14, "bold"),
                  foreground=COL_INFO).pack(side="left", padx=12, pady=6)

        b_read = ttk.Button(live_f, text="Read Now",
                            command=self._live_read)
        b_read.pack(side="left", padx=4)
        self._connected_widgets.append(b_read)

    # ── helpers ──

    def _clamp(self, v: int) -> int:
        return max(0, min(999999, v))

    def _step(self, delta: int) -> None:
        if not self.require_connection():
            return
        try:
            cur = int(float(self._res_var.get()))
        except ValueError:
            cur = 0
        new_val = self._clamp(cur + delta)
        self._res_var.set(str(new_val))
        self._slider.set(new_val)
        threading.Thread(target=lambda: self.safe_write(
            f"RESistance:VALue {new_val}"), daemon=True).start()

    def _set_resistance(self) -> None:
        if not self.require_connection():
            return
        try:
            v = self._clamp(int(float(self._res_var.get())))
        except ValueError:
            self._log_pane.log("Invalid resistance value.", "error")
            return
        self._res_var.set(str(v))
        self._slider.set(v)
        threading.Thread(target=lambda: self.safe_write(
            f"RESistance:VALue {v}"), daemon=True).start()

    def _query_resistance(self) -> None:
        if not self.require_connection():
            return
        def _do():
            resp = self.safe_query("RESistance:VALue?")
            if resp:
                try:
                    v = self._clamp(int(float(resp)))
                    self._root.after(0, lambda: self._res_var.set(str(v)))
                    self._root.after(0, lambda: self._slider.set(v))
                    self._root.after(0, lambda r=resp: self._live_var.set(f"{r} Ω"))
                except ValueError:
                    pass
        threading.Thread(target=_do, daemon=True).start()

    def _on_slider(self, val: str) -> None:
        v = int(float(val))
        self._res_var.set(str(v))

    def _set_output(self) -> None:
        if not self.require_connection():
            return
        state = self._out_var.get()
        def _do():
            if state == "ON" and not self._state.rel_en:
                self._log_pane.log("[auto] REL_EN → ON (required for OUTPUT ON)", "warn")
                self.safe_write("OUTPut:RELay:ENable ON")
                self._root.after(0, lambda: setattr(self._state, "rel_en", True))
            self.safe_write(f"OUTPut:STATe {state}")
            self._root.after(0, lambda s=state: setattr(
                self._state, "output_on", s == "ON"))
        threading.Thread(target=_do, daemon=True).start()

    def _query_output(self) -> None:
        if not self.require_connection():
            return
        def _do():
            resp = self.safe_query("OUTPut:STATe?")
            if resp is not None:
                val = "ON" if resp.strip() in ("1", "ON") else "OFF"
                self._root.after(0, lambda v=val: self._out_var.set(v))
        threading.Thread(target=_do, daemon=True).start()

    def _decade_set_one(self, decade_num: int) -> None:
        if not self.require_connection():
            return
        idx = 6 - decade_num
        val = self._decade_vars[idx].get()
        threading.Thread(target=lambda: self.safe_write(
            f"RESistance:DECade {decade_num},{val}"), daemon=True).start()

    def _decade_set_all(self) -> None:
        if not self.require_connection():
            return
        def _do():
            for d in range(6, 0, -1):
                idx = 6 - d
                val = self._decade_vars[idx].get()
                self.safe_write(f"RESistance:DECade {d},{val}")
        threading.Thread(target=_do, daemon=True).start()

    def _decade_query_all(self) -> None:
        if not self.require_connection():
            return
        def _do():
            for d in range(6, 0, -1):
                idx = 6 - d
                resp = self.safe_query(f"RESistance:DECade? {d}")
                if resp and resp.strip().isdigit():
                    i = idx
                    self._root.after(0, lambda v=resp.strip(), i=i:
                                     self._decade_vars[i].set(v))
        threading.Thread(target=_do, daemon=True).start()

    def _live_read(self) -> None:
        if not self.require_connection():
            return
        def _do():
            resp = self.safe_query("RESistance:VALue?")
            if resp:
                self._root.after(0, lambda r=resp: self._live_var.set(f"{r} Ω"))
        threading.Thread(target=_do, daemon=True).start()

    def sync_rel_en_indicator(self) -> None:
        pass  # could add visual indicator later


# ─── RelayTab ─────────────────────────────────────────────────────────────────

class RelayTab(_BaseTab):
    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}

        # ── REL_EN ──
        relen_f = ttk.LabelFrame(self.frame, text="Relay Power Supply (REL_EN / PA4)")
        relen_f.pack(fill="x", padx=8, pady=(8, 4))

        ttk.Label(relen_f, text="REL_EN:").grid(row=0, column=0, **p)
        self._relen_var = tk.StringVar(value="OFF")
        relen_cb = ttk.Combobox(relen_f, textvariable=self._relen_var,
                                values=["ON", "OFF"], state="readonly", width=6)
        relen_cb.grid(row=0, column=1, **p)

        b_set = ttk.Button(relen_f, text="Set",   command=self._relen_set)
        b_set.grid(row=0, column=2, **p)
        b_qry = ttk.Button(relen_f, text="Query", command=self._relen_query)
        b_qry.grid(row=0, column=3, **p)
        self._connected_widgets += [relen_cb, b_set, b_qry]

        ttk.Label(relen_f,
                  text="Enable BEFORE using relays.  Disable when done.",
                  foreground=COL_CMD).grid(row=0, column=4, **p)

        # ── RELay:STATe ──
        state_f = ttk.LabelFrame(self.frame, text="Individual Relay (RELay:STATe)")
        state_f.pack(fill="x", padx=8, pady=4)

        ttk.Label(state_f, text="Decade (1–6):").grid(row=0, column=0, **p)
        self._rs_decade = tk.StringVar(value="1")
        sb_d = ttk.Spinbox(state_f, from_=1, to=6, textvariable=self._rs_decade,
                           width=4, justify="center")
        sb_d.grid(row=0, column=1, **p)

        ttk.Label(state_f, text="Bit (0–5):").grid(row=0, column=2, **p)
        self._rs_bit = tk.StringVar(value="0")
        sb_b = ttk.Spinbox(state_f, from_=0, to=5, textvariable=self._rs_bit,
                           width=4, justify="center")
        sb_b.grid(row=0, column=3, **p)

        b_on  = ttk.Button(state_f, text="ON",    command=lambda: self._relay_state(True))
        b_off = ttk.Button(state_f, text="OFF",   command=lambda: self._relay_state(False))
        b_qr  = ttk.Button(state_f, text="Query", command=self._relay_state_query)
        b_on.grid(row=0, column=4, **p)
        b_off.grid(row=0, column=5, **p)
        b_qr.grid(row=0, column=6, **p)
        self._connected_widgets += [sb_d, sb_b, b_on, b_off, b_qr]

        ttk.Label(state_f,
                  text="decade 1=1Ω…6=100kΩ, bit Q0–Q5.  No protection – marks resistance UNKNOWN.",
                  foreground=COL_CMD).grid(row=1, column=0, columnspan=7,
                                           sticky="w", padx=6, pady=2)

        # ── RELay:RAW ──
        raw_f = ttk.LabelFrame(self.frame, text="Raw Relay (RELay:RAW)")
        raw_f.pack(fill="x", padx=8, pady=4)

        ttk.Label(raw_f, text="xRy list:").grid(row=0, column=0, **p)
        self._raw_var = tk.StringVar(value="103")
        raw_e = ttk.Entry(raw_f, textvariable=self._raw_var, width=24)
        raw_e.grid(row=0, column=1, **p)
        raw_e.bind("<Return>", lambda _: self._relay_raw_set())

        b_rs = ttk.Button(raw_f, text="Set",   command=self._relay_raw_set)
        b_rq = ttk.Button(raw_f, text="Query", command=self._relay_raw_query)
        b_rs.grid(row=0, column=2, **p)
        b_rq.grid(row=0, column=3, **p)
        self._connected_widgets += [raw_e, b_rs, b_rq]

        ttk.Label(raw_f,
                  text="Format: hundreds=decade(1–6), units=relay(0–5), tens ignored.\n"
                       "e.g. 103 = decade 1Ω Q3 | 103,206 = two relays at once",
                  foreground=COL_CMD).grid(row=1, column=0, columnspan=4,
                                           sticky="w", padx=6, pady=2)

    def _relen_set(self) -> None:
        if not self.require_connection():
            return
        val = self._relen_var.get()
        def _do():
            self.safe_write(f"OUTPut:RELay:ENable {val}")
            self._root.after(0, lambda v=val: setattr(
                self._state, "rel_en", v == "ON"))
        threading.Thread(target=_do, daemon=True).start()

    def _relen_query(self) -> None:
        if not self.require_connection():
            return
        def _do():
            resp = self.safe_query("OUTPut:RELay:ENable?")
            if resp is not None:
                v = "ON" if resp.strip() in ("1", "ON") else "OFF"
                self._root.after(0, lambda x=v: self._relen_var.set(x))
                self._root.after(0, lambda x=v: setattr(
                    self._state, "rel_en", x == "ON"))
        threading.Thread(target=_do, daemon=True).start()

    def _relay_state(self, on: bool) -> None:
        if not self.require_connection():
            return
        d, b = self._rs_decade.get(), self._rs_bit.get()
        s = "ON" if on else "OFF"
        threading.Thread(target=lambda: self.safe_write(
            f"RELay:STATe {d},{b},{s}"), daemon=True).start()

    def _relay_state_query(self) -> None:
        if not self.require_connection():
            return
        d, b = self._rs_decade.get(), self._rs_bit.get()
        threading.Thread(target=lambda: self.safe_query(
            f"RELay:STATe? {d},{b}"), daemon=True).start()

    def _relay_raw_set(self) -> None:
        if not self.require_connection():
            return
        raw = self._raw_var.get().strip()
        if not raw:
            return
        tokens = [t.strip() for t in raw.split(",") if t.strip()]
        for tok in tokens:
            try:
                v = int(tok)
                dec, rel = v // 100, v % 10
                if not (1 <= dec <= 6 and 0 <= rel <= 5):
                    self._log_pane.log(f"RAW error: {tok} out of range", "error")
                    return
            except ValueError:
                self._log_pane.log(f"RAW error: '{tok}' not a number", "error")
                return
        threading.Thread(target=lambda: self.safe_write(
            f"RELay:RAW {','.join(tokens)}"), daemon=True).start()

    def _relay_raw_query(self) -> None:
        if not self.require_connection():
            return
        threading.Thread(target=lambda: self.safe_query(
            "RELay:RAW?"), daemon=True).start()


# ─── NetworkTab ───────────────────────────────────────────────────────────────

class NetworkTab(_BaseTab):
    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}

        cfg_f = ttk.LabelFrame(self.frame, text="Network Configuration  (NET:*)")
        cfg_f.pack(fill="x", padx=8, pady=(8, 4))

        fields = [
            ("Static IP:",  "net_ip",   "192.168.1.6",    "NET:IPADdress"),
            ("Netmask:",    "net_mask",  "255.255.255.0",   "NET:SMASk"),
            ("Gateway:",    "net_gw",    "192.168.1.1",     "NET:GATEway"),
        ]
        self._net_vars = {}
        self._net_cmds = {}
        for row, (label, key, default, cmd) in enumerate(fields):
            ttk.Label(cfg_f, text=label).grid(row=row, column=0, **p)
            var = tk.StringVar(value=default)
            e   = ttk.Entry(cfg_f, textvariable=var, width=18)
            e.grid(row=row, column=1, **p)
            b = ttk.Button(cfg_f, text="Set",
                           command=lambda c=cmd, v=var: self._net_set(c, v.get()))
            b.grid(row=row, column=2, **p)
            bq = ttk.Button(cfg_f, text="Query",
                            command=lambda c=cmd+"?": self._net_query(c))
            bq.grid(row=row, column=3, **p)
            self._net_vars[key] = var
            self._connected_widgets += [e, b, bq]

        ttk.Label(cfg_f, text="DHCP:").grid(row=3, column=0, **p)
        self._dhcp_var = tk.BooleanVar(value=True)
        dhcp_cb = ttk.Checkbutton(cfg_f, text="Try DHCP first (fallback to static)",
                                  variable=self._dhcp_var,
                                  command=self._dhcp_set)
        dhcp_cb.grid(row=3, column=1, columnspan=2, sticky="w", **p)
        b_dq = ttk.Button(cfg_f, text="Query", command=lambda: self._net_query("NET:DHCP?"))
        b_dq.grid(row=3, column=3, **p)
        self._connected_widgets += [dhcp_cb, b_dq]

        ttk.Label(cfg_f, text="PHY Speed:").grid(row=4, column=0, **p)
        self._phy_var = tk.StringVar(value="10M")
        phy_cb = ttk.Combobox(cfg_f, textvariable=self._phy_var,
                              values=["AUTO", "10M", "100M"],
                              state="readonly", width=8)
        phy_cb.grid(row=4, column=1, sticky="w", **p)
        b_phy_set = ttk.Button(cfg_f, text="Set",
                               command=lambda: self._phy_set())
        b_phy_set.grid(row=4, column=2, **p)
        b_phy_q = ttk.Button(cfg_f, text="Query", command=self._phy_query)
        b_phy_q.grid(row=4, column=3, **p)
        self._connected_widgets += [phy_cb, b_phy_set, b_phy_q]

        b_apply = ttk.Button(cfg_f, text="NET:APPLy  (save to NVM + restart W5500)",
                             command=self._net_apply)
        b_apply.grid(row=5, column=0, columnspan=3, sticky="w", **p)
        self._connected_widgets.append(b_apply)

        ttk.Label(cfg_f, text="Active IP:").grid(row=5, column=3, **p)
        self._active_ip_var = tk.StringVar(value="—")
        ttk.Label(cfg_f, textvariable=self._active_ip_var,
                  foreground=COL_INFO, width=18).grid(row=5, column=4, **p)
        b_rd = ttk.Button(cfg_f, text="Read", command=self._read_active_ip)
        b_rd.grid(row=5, column=5, **p)
        self._connected_widgets.append(b_rd)

    def _net_set(self, cmd: str, val: str) -> None:
        if not self.require_connection():
            return
        threading.Thread(target=lambda: self.safe_write(f'{cmd} "{val}"'),
                         daemon=True).start()

    def _net_query(self, cmd: str) -> None:
        if not self.require_connection():
            return
        threading.Thread(target=lambda: self.safe_query(cmd), daemon=True).start()

    def _dhcp_set(self) -> None:
        if not self.require_connection():
            return
        val = "ON" if self._dhcp_var.get() else "OFF"
        threading.Thread(target=lambda: self.safe_write(f"NET:DHCP {val}"),
                         daemon=True).start()

    # PHY mode mapping: combobox label ↔ integer sent over SCPI
    _PHY_LABEL_TO_INT = {"AUTO": 0, "10M": 1, "100M": 2}
    _PHY_INT_TO_LABEL = {0: "AUTO", 1: "10M", 2: "100M"}

    def _phy_set(self) -> None:
        if not self.require_connection():
            return
        val = self._phy_var.get()
        mode_int = self._PHY_LABEL_TO_INT.get(val)
        if mode_int is None:
            return
        threading.Thread(target=lambda: self.safe_write(f"NET:PHY:MODE {mode_int}"),
                         daemon=True).start()

    def _phy_query(self) -> None:
        if not self.require_connection():
            return
        def _do():
            resp = self.safe_query("NET:PHY:MODE?")
            if not resp:
                return
            s = resp.strip().strip('"').upper()
            # Firmware returns "AUTO" / "10M" / "100M"; tolerate "0"/"1"/"2" too
            if s in ("AUTO", "10M", "100M"):
                label = s
            elif s.isdigit():
                label = self._PHY_INT_TO_LABEL.get(int(s))
            else:
                label = None
            if label:
                self._root.after(0, lambda v=label: self._phy_var.set(v))
        threading.Thread(target=_do, daemon=True).start()

    def _net_apply(self) -> None:
        if not self.require_connection():
            return
        if messagebox.askyesno(
                "NET:APPLy",
                "This restarts the Ethernet stack.\n"
                "The VXI-11 connection will be dropped.\nProceed?"):
            threading.Thread(target=lambda: self.safe_write("NET:APPLy"),
                             daemon=True).start()
            self._log_pane.log("NOTE: Reconnect after a few seconds.", "warn")

    def _read_active_ip(self) -> None:
        if not self.require_connection():
            return
        def _do():
            resp = self.safe_query("NET:STATe?")
            if resp:
                self._root.after(0, lambda r=resp: self._active_ip_var.set(r))
        threading.Thread(target=_do, daemon=True).start()


# ─── MultimeterConfigWindow ───────────────────────────────────────────────────

class MultimeterConfigWindow(tk.Toplevel):
    def __init__(self, parent: tk.Widget, state: AppState,
                 log_pane: LogPane, root: tk.Tk):
        super().__init__(parent)
        self.title("Multimeter Configuration")
        self.resizable(False, False)
        self.grab_set()
        self._state    = state
        self._log      = log_pane
        self._root_app = root
        self._meter    = state.meter
        self._slot_info: dict = {}
        self._build()

    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=4, pady=4)

        # ── Tab: Connection ──
        conn_f = ttk.Frame(nb)
        nb.add(conn_f, text="Connection")

        ttk.Label(conn_f, text="Model:").grid(row=0, column=0, **p)
        self._model_var = tk.StringVar(value=self._meter.model)
        model_cb = ttk.Combobox(conn_f, textvariable=self._model_var,
                                values=list(METER_MODELS.keys()),
                                state="readonly", width=20)
        model_cb.grid(row=0, column=1, columnspan=2, sticky="w", **p)
        model_cb.bind("<<ComboboxSelected>>", self._on_model_change)

        ttk.Label(conn_f, text="VISA Resource:").grid(row=1, column=0, **p)
        self._visa_var = tk.StringVar()
        self._visa_cb  = ttk.Combobox(conn_f, textvariable=self._visa_var, width=32)
        self._visa_cb.grid(row=1, column=1, **p)
        self._btn_refresh = ttk.Button(conn_f, text="Scan",
                                       command=self._refresh_resources)
        self._btn_refresh.grid(row=1, column=2, **p)

        b_conn = ttk.Button(conn_f, text="Connect",    command=self._connect_meter)
        b_disc = ttk.Button(conn_f, text="Disconnect", command=self._disconnect_meter)
        b_conn.grid(row=2, column=1, sticky="w", **p)
        b_disc.grid(row=2, column=2, sticky="w", **p)

        ttk.Label(conn_f, text="IDN:").grid(row=3, column=0, **p)
        self._idn_var = tk.StringVar(value=self._meter.idn or "—")
        self._idn_lbl = ttk.Label(conn_f, textvariable=self._idn_var,
                                  foreground=COL_INFO, font=("Courier New", 8))
        self._idn_lbl.grid(row=3, column=1, columnspan=3, sticky="w", **p)

        self._refresh_resources()

        # ── Tab: Measurement Settings ──
        meas_f = ttk.Frame(nb)
        nb.add(meas_f, text="Measurement")

        ttk.Label(meas_f, text="Mode:").grid(row=0, column=0, **p)
        self._mode_var = tk.IntVar(value=1 if self._meter.mode_4w else 0)
        ttk.Radiobutton(meas_f, text="2W  (RES)",  variable=self._mode_var,
                        value=0).grid(row=0, column=1, sticky="w", **p)
        ttk.Radiobutton(meas_f, text="4W  (FRES)", variable=self._mode_var,
                        value=1).grid(row=0, column=2, sticky="w", **p)

        ttk.Label(meas_f, text="NPLC:").grid(row=1, column=0, **p)
        self._nplc_var = tk.StringVar(value=str(self._meter.nplc))
        ttk.Combobox(meas_f, textvariable=self._nplc_var, values=NPLC_VALUES,
                     width=8).grid(row=1, column=1, sticky="w", **p)
        ttk.Label(meas_f, text="(higher = more accurate, slower)",
                  foreground=COL_CMD).grid(row=1, column=2, sticky="w", **p)

        ttk.Label(meas_f, text="DIGITS:").grid(row=2, column=0, **p)
        self._digits_var = tk.StringVar(value=str(self._meter.digits))
        self._digits_cb  = ttk.Combobox(meas_f, textvariable=self._digits_var,
                                        values=DIGITS_VALUES, width=8)
        self._digits_cb.grid(row=2, column=1, sticky="w", **p)
        ttk.Label(meas_f, text="(not used by 34970A)",
                  foreground=COL_CMD).grid(row=2, column=2, sticky="w", **p)

        ttk.Label(meas_f, text="Averaging:").grid(row=3, column=0, **p)
        self._avg_en  = tk.BooleanVar(value=self._meter.avg_count > 1)
        self._avg_cnt = tk.StringVar(value=str(max(1, self._meter.avg_count)))
        ttk.Checkbutton(meas_f, text="Mean of", variable=self._avg_en).grid(
            row=3, column=1, sticky="w", **p)
        ttk.Spinbox(meas_f, from_=1, to=20, textvariable=self._avg_cnt,
                    width=5).grid(row=3, column=2, sticky="w", **p)
        ttk.Label(meas_f, text="measurements").grid(row=3, column=3, sticky="w", **p)

        # Quick test
        test_f = ttk.LabelFrame(meas_f, text="Quick Test")
        test_f.grid(row=4, column=0, columnspan=4, sticky="ew", padx=8, pady=8)

        ttk.Button(test_f, text="Measure Now",
                   command=self._quick_measure).pack(side="left", padx=6, pady=4)
        self._meas_lbl = ttk.Label(test_f, text="—",
                                   font=("Courier New", 11, "bold"),
                                   foreground=COL_INFO)
        self._meas_lbl.pack(side="left", padx=6)

        # ── Tab: 34970A MUX (shown only when relevant) ──
        self._mux_frame = ttk.Frame(nb)
        nb.add(self._mux_frame, text="34970A MUX")
        self._nb = nb
        self._build_mux_tab()
        self._on_model_change()

        # ── Bottom buttons ──
        btn_f = ttk.Frame(self)
        btn_f.pack(fill="x", padx=8, pady=6)
        ttk.Button(btn_f, text="Apply & Close",
                   command=self._apply_close).pack(side="right", padx=4)
        ttk.Button(btn_f, text="Cancel",
                   command=self.destroy).pack(side="right", padx=4)

    def _build_mux_tab(self) -> None:
        p = {"padx": 6, "pady": 4}
        f = self._mux_frame
        for w in f.winfo_children():
            w.destroy()

        ttk.Button(f, text="Scan Slots",
                   command=self._scan_slots).grid(row=0, column=0, columnspan=2,
                                                   sticky="w", **p)

        self._slot_labels: dict = {}
        for slot in (1, 2, 3):
            ttk.Label(f, text=f"Slot {slot}:").grid(row=slot, column=0, **p)
            lbl = ttk.Label(f, text="—", width=32)
            lbl.grid(row=slot, column=1, sticky="w", **p)
            self._slot_labels[slot] = lbl

        ttk.Label(f, text="Active slot:").grid(row=4, column=0, **p)
        self._slot_var = tk.StringVar(value=str(self._meter.slot))
        self._slot_cb  = ttk.Combobox(f, textvariable=self._slot_var,
                                      values=["1", "2", "3"], width=4)
        self._slot_cb.grid(row=4, column=1, sticky="w", **p)
        self._slot_cb.bind("<<ComboboxSelected>>", self._update_visa_ch)

        ttk.Label(f, text="Channel:").grid(row=5, column=0, **p)
        self._ch_var = tk.StringVar(value=str(self._meter.channel))
        ch_sb = ttk.Spinbox(f, from_=1, to=20, textvariable=self._ch_var,
                            width=5, command=self._update_visa_ch)
        ch_sb.grid(row=5, column=1, sticky="w", **p)
        ch_sb.bind("<KeyRelease>", lambda _: self._update_visa_ch())

        ttk.Label(f, text="VISA channel:").grid(row=6, column=0, **p)
        self._visa_ch_var = tk.StringVar(value="(@101)")
        ttk.Label(f, textvariable=self._visa_ch_var,
                  font=("Courier New", 10, "bold"),
                  foreground=COL_INFO).grid(row=6, column=1, sticky="w", **p)
        self._update_visa_ch()

    def _on_model_change(self, _=None) -> None:
        model = self._model_var.get()
        is_34970 = model == "Agilent 34970A"
        # show/hide MUX tab
        try:
            idx = list(self._nb.tabs()).index(str(self._mux_frame))
            if is_34970:
                self._nb.tab(idx, state="normal")
            else:
                self._nb.tab(idx, state="hidden")
        except Exception:
            pass
        has_digits = METER_MODELS.get(model, {}).get("digits", False)
        self._digits_cb.config(state="readonly" if has_digits else "disabled")

    def _refresh_resources(self) -> None:
        self._btn_refresh.config(state="disabled", text="Scanning…")
        self._visa_cb["values"] = []
        threading.Thread(target=self._scan_resources_thread, daemon=True).start()

    def _scan_resources_thread(self) -> None:
        try:
            found = list(_get_visa_rm().list_resources())
            addrs = [r for r in found if not r.upper().startswith('TCPIP')]
        except Exception:
            addrs = []
        def _done():
            self._btn_refresh.config(state="normal", text="Scan")
            self._visa_cb["values"] = addrs
            if addrs and (not self._visa_var.get() or self._visa_var.get() not in addrs):
                self._visa_var.set(addrs[0])
        self._root_app.after(0, _done)

    def _connect_meter(self) -> None:
        addr = self._visa_var.get().strip()
        if not addr:
            messagebox.showwarning("No address", "Enter a VISA resource address.")
            return
        threading.Thread(target=self._connect_thread, args=(addr,), daemon=True).start()

    def _connect_thread(self, addr: str) -> None:
        try:
            self._meter.connect(addr)
            self._root_app.after(0, lambda: self._idn_var.set(self._meter.idn))
            self._log.log(f"Meter connected: {self._meter.idn}", "ok")
        except Exception as exc:
            self._root_app.after(0, lambda e=exc: messagebox.showerror(
                "Meter connect error", str(e)))
            self._log.log(f"Meter connect ERROR: {exc}", "error")

    def _disconnect_meter(self) -> None:
        self._meter.disconnect()
        self._idn_var.set("—")
        self._log.log("Meter disconnected.", "warn")

    def _scan_slots(self) -> None:
        if not self._meter.is_connected():
            messagebox.showwarning("Not connected", "Connect multimeter first.")
            return
        threading.Thread(target=self._scan_thread, daemon=True).start()

    def _scan_thread(self) -> None:
        info = self._meter.scan_34970a_slots()
        self._slot_info = info
        self._root_app.after(0, lambda: self._update_slot_labels(info))

    MUX_OK_CARDS   = {"34901A", "34902A"}
    MUX_WARN_CARDS = {"34908A"}
    MUX_BAD_CARDS  = {"34903A", "34904A", "34905A", "34906A", "34907A"}

    def _update_slot_labels(self, info: dict) -> None:
        ok_slots = []
        for slot, card in info.items():
            card_up = card.upper().strip()
            if card_up == "EMPTY" or not card_up:
                icon, fg = "—  EMPTY", COL_CMD
            elif card_up in self.MUX_OK_CARDS:
                icon, fg = f"✓  {card_up}  (MUX compatible)", COL_OK
                ok_slots.append(str(slot))
            elif card_up in self.MUX_WARN_CARDS:
                icon, fg = f"⚠  {card_up}  (single-ended, 2W only)", COL_WARN
                ok_slots.append(str(slot))
            elif card_up in self.MUX_BAD_CARDS:
                icon, fg = f"✗  {card_up}  (RF switch — not usable)", COL_ERR
            else:
                icon, fg = f"?  {card_up}", COL_WARN
                ok_slots.append(str(slot))
            self._slot_labels[slot].config(text=icon, foreground=fg)
        if ok_slots:
            self._slot_cb["values"] = ok_slots
            if self._slot_var.get() not in ok_slots:
                self._slot_var.set(ok_slots[0])
        self._update_visa_ch()

    def _update_visa_ch(self, _=None) -> None:
        try:
            slot = int(self._slot_var.get())
            ch   = int(self._ch_var.get())
            self._visa_ch_var.set(f"(@{slot}{ch:02d})")
        except ValueError:
            self._visa_ch_var.set("(invalid)")

    def _quick_measure(self) -> None:
        if not self._meter.is_connected():
            messagebox.showwarning("Not connected", "Connect multimeter first.")
            return
        self._apply_settings()
        threading.Thread(target=self._measure_thread, daemon=True).start()

    def _measure_thread(self) -> None:
        try:
            val = self._meter.measure()
            self._root_app.after(0, lambda v=val: self._meas_lbl.config(
                text=f"{v:.6g} Ω"))
            self._log.log(f"Meter: {val:.6g} Ω", "ok")
        except Exception as exc:
            self._root_app.after(0, lambda e=exc: self._meas_lbl.config(
                text=f"ERROR: {e}"))
            self._log.log(f"Meter measure ERROR: {exc}", "error")

    def _apply_settings(self) -> None:
        self._meter.model    = self._model_var.get()
        self._meter.mode_4w  = bool(self._mode_var.get())
        try:
            self._meter.nplc = float(self._nplc_var.get())
        except ValueError:
            pass
        try:
            self._meter.digits = float(self._digits_var.get())
        except ValueError:
            pass
        avg_on = self._avg_en.get()
        try:
            cnt = int(self._avg_cnt.get())
        except ValueError:
            cnt = 1
        self._meter.avg_count = cnt if avg_on else 1
        try:
            self._meter.slot    = int(self._slot_var.get())
            self._meter.channel = int(self._ch_var.get())
        except (ValueError, AttributeError):
            pass

    def _apply_close(self) -> None:
        self._apply_settings()
        self.destroy()


# ─── CalibrationTab ───────────────────────────────────────────────────────────

class CalibrationTab(_BaseTab):
    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}

        # ── Controls row ──
        ctrl_f = ttk.Frame(self.frame)
        ctrl_f.pack(fill="x", padx=8, pady=(8, 2))

        b_qa = ttk.Button(ctrl_f, text="Query All from RDE",
                          command=self._query_all_rde)
        b_qa.pack(side="left", padx=2)
        self._connected_widgets.append(b_qa)

        b_sv = ttk.Button(ctrl_f, text="Save Cal",
                          command=self._cal_save)
        b_sv.pack(side="left", padx=2)
        self._connected_widgets.append(b_sv)

        b_ld = ttk.Button(ctrl_f, text="Load Cal",
                          command=self._cal_load)
        b_ld.pack(side="left", padx=2)
        self._connected_widgets.append(b_ld)

        b_rs = ttk.Button(ctrl_f, text="Reset to Nominal",
                          command=self._cal_reset)
        b_rs.pack(side="left", padx=2)
        self._connected_widgets.append(b_rs)

        ttk.Separator(ctrl_f, orient="vertical").pack(
            side="left", fill="y", padx=8, pady=2)

        ttk.Label(ctrl_f, text="Correction:").pack(side="left")
        self._cal_en_var = tk.StringVar(value="OFF")
        cal_en_cb = ttk.Combobox(ctrl_f, textvariable=self._cal_en_var,
                                 values=["ON", "OFF"], state="readonly", width=5)
        cal_en_cb.pack(side="left", padx=2)
        b_ces = ttk.Button(ctrl_f, text="Set",
                           command=self._cal_enable_set)
        b_ceq = ttk.Button(ctrl_f, text="Query",
                           command=self._cal_enable_query)
        b_ces.pack(side="left", padx=2)
        b_ceq.pack(side="left", padx=2)
        self._connected_widgets += [cal_en_cb, b_ces, b_ceq]

        # ── Treeview ──
        tv_f = ttk.LabelFrame(self.frame, text="Calibration Table  [milliohms]")
        tv_f.pack(fill="both", expand=True, padx=8, pady=4)

        digit_cols = [str(d) for d in range(10)]
        self._tv = ttk.Treeview(tv_f, columns=digit_cols, show="headings", height=6)
        for col in digit_cols:
            self._tv.heading(col, text=f"d={col}")
            self._tv.column(col, width=78, anchor="center")

        decade_names = ["d1  1Ω", "d2  10Ω", "d3  100Ω",
                        "d4  1kΩ", "d5  10kΩ", "d6  100kΩ"]
        for row_idx, name in enumerate(decade_names):
            nom = [str(d * DECADE_UNITS_MO[row_idx]) for d in range(10)]
            self._tv.insert("", "end", iid=str(row_idx), text=name, values=nom)
        self._tv.pack(side="left", fill="both", expand=True)
        vsb = ttk.Scrollbar(tv_f, orient="vertical", command=self._tv.yview)
        self._tv.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")

        # ── Manual entry ──
        man_f = ttk.LabelFrame(self.frame, text="Manual Point Entry")
        man_f.pack(fill="x", padx=8, pady=4)

        ttk.Label(man_f, text="Decade (1–6):").grid(row=0, column=0, **p)
        self._m_dec = tk.StringVar(value="1")
        ttk.Spinbox(man_f, from_=1, to=6, textvariable=self._m_dec,
                    width=4).grid(row=0, column=1, **p)

        ttk.Label(man_f, text="Digit (0–9):").grid(row=0, column=2, **p)
        self._m_dig = tk.StringVar(value="1")
        ttk.Spinbox(man_f, from_=0, to=9, textvariable=self._m_dig,
                    width=4).grid(row=0, column=3, **p)

        ttk.Label(man_f, text="Value (mΩ):").grid(row=0, column=4, **p)
        self._m_val = tk.StringVar(value="1000")
        ttk.Entry(man_f, textvariable=self._m_val, width=10).grid(row=0, column=5, **p)

        b_sp = ttk.Button(man_f, text="Set Point",
                          command=self._manual_set)
        b_qp = ttk.Button(man_f, text="Query Point",
                          command=self._manual_query)
        b_sp.grid(row=0, column=6, **p)
        b_qp.grid(row=0, column=7, **p)
        self._connected_widgets += [b_sp, b_qp]

        # ── Auto-calibration ──
        auto_f = ttk.LabelFrame(self.frame, text="Auto-Calibration with Multimeter")
        auto_f.pack(fill="x", padx=8, pady=4)

        b_cfg = ttk.Button(auto_f, text="Multimeter Config…",
                           command=self._open_meter_config)
        b_cfg.pack(side="left", padx=4, pady=4)

        self._meter_status_lbl = ttk.Label(
            auto_f, text="Meter: not connected", foreground=COL_WARN)
        self._meter_status_lbl.pack(side="left", padx=4)

        ttk.Separator(auto_f, orient="vertical").pack(
            side="left", fill="y", padx=8, pady=4)

        self._btn_autocal = ttk.Button(auto_f, text="Auto-Calibrate All…",
                                       command=self._start_autocal,
                                       state="disabled")
        self._btn_autocal.pack(side="left", padx=4, pady=4)
        self._connected_widgets.append(self._btn_autocal)

        self._btn_abort = ttk.Button(auto_f, text="Abort",
                                     command=self._abort_autocal,
                                     state="disabled")
        self._btn_abort.pack(side="left", padx=4)

        self._progress = ttk.Progressbar(auto_f, length=200, mode="determinate")
        self._progress.pack(side="left", padx=8)

        self._prog_lbl = ttk.Label(auto_f, text="")
        self._prog_lbl.pack(side="left")

        # ── Timing settings ──
        timing_f = ttk.Frame(auto_f)
        timing_f.pack(side="left", padx=12, pady=4)

        self._settle_var   = tk.DoubleVar(value=2.0)
        self._intermeas_var = tk.DoubleVar(value=0.5)

        ttk.Label(timing_f, text="Settle (s):").grid(
            row=0, column=0, padx=(0, 2), sticky="e")
        sb_settle = ttk.Spinbox(
            timing_f, from_=0.1, to=30.0, increment=0.1, width=5,
            textvariable=self._settle_var, format="%.1f",
            command=self._update_cal_estimate)
        sb_settle.grid(row=0, column=1, padx=(0, 10))
        sb_settle.bind("<FocusOut>", lambda _: self._update_cal_estimate())
        sb_settle.bind("<Return>",   lambda _: self._update_cal_estimate())

        ttk.Label(timing_f, text="Inter-meas (s):").grid(
            row=0, column=2, padx=(0, 2), sticky="e")
        sb_inter = ttk.Spinbox(
            timing_f, from_=0.0, to=10.0, increment=0.1, width=5,
            textvariable=self._intermeas_var, format="%.1f",
            command=self._update_cal_estimate)
        sb_inter.grid(row=0, column=3, padx=(0, 10))
        sb_inter.bind("<FocusOut>", lambda _: self._update_cal_estimate())
        sb_inter.bind("<Return>",   lambda _: self._update_cal_estimate())

        ttk.Label(timing_f, text="Est. time:").grid(
            row=0, column=4, padx=(0, 2), sticky="e")
        self._est_lbl = ttk.Label(timing_f, text="—", foreground=COL_INFO,
                                  width=10)
        self._est_lbl.grid(row=0, column=5)

        # ── PDF Report ──
        pdf_f = ttk.Frame(self.frame)
        pdf_f.pack(fill="x", padx=8, pady=(2, 8))
        b_pdf = ttk.Button(pdf_f, text="Export PDF Report…",
                           command=self._export_pdf)
        b_pdf.pack(side="left")

        self._cancel_event = threading.Event()
        self._cal_thread:  Optional[threading.Thread] = None
        self._update_cal_estimate()

    def _update_cal_estimate(self, *_) -> None:
        try:
            settle    = float(self._settle_var.get())
            inter     = float(self._intermeas_var.get())
            avg       = max(1, self._state.meter.avg_count)
            nplc      = self._state.meter.nplc
            meas_time = nplc / 50.0          # one reading at 50 Hz line
            per_point = 0.5 + settle + avg * (meas_time + inter) + 0.5
            total_s   = 6 * (6 * 0.1 + 0.5) + 60 * per_point
            mins, secs = divmod(int(total_s), 60)
            self._est_lbl.config(text=f"~{mins}m {secs:02d}s")
        except Exception:
            self._est_lbl.config(text="—")

    # ── RDE calibration commands ──

    def _query_all_rde(self) -> None:
        if not self.require_connection():
            return
        threading.Thread(target=self._query_all_thread, daemon=True).start()

    def _query_all_thread(self) -> None:
        data: dict = {}
        for dec in range(1, 7):
            for dig in range(10):
                resp = self.safe_query(f"CALibration:DECade? {dec},{dig}")
                data[(dec-1, dig)] = resp.strip() if resp else "?"
        def _upd():
            for row in range(6):
                vals = [data.get((row, d), "?") for d in range(10)]
                self._tv.item(str(row), values=vals)
                for dig, v in enumerate(vals):
                    try:
                        self._state.cal_table[row][dig] = int(v)
                    except ValueError:
                        pass
        self._root.after(0, _upd)

    def _cal_save(self) -> None:
        if not self.require_connection():
            return
        def _do():
            if not self.safe_write("CALibration:SAVE"):
                return
            err_count = self.drain_scpi_errors(silent_if_empty=True)
            if err_count == 0:
                self._log_pane.log("Calibration saved to NVM.", "ok")
            else:
                self._log_pane.log(
                    "Calibration save failed. Values remain in RDE RAM only. "
                    "See System tab for NVM/I2C diagnostics.", "error")
                self._root.after(0, lambda: messagebox.showerror(
                    "Save Failed",
                    "CALibration:SAVE failed.\n"
                    "Values are in RDE RAM but NOT stored to NVM.\n\n"
                    "Check System tab → NVM / I2C Diagnostics."))
        threading.Thread(target=_do, daemon=True).start()

    def _cal_load(self) -> None:
        if not self.require_connection():
            return
        def _do():
            self.safe_write("CALibration:LOAD")
            self._query_all_thread()
        threading.Thread(target=_do, daemon=True).start()

    def _cal_reset(self) -> None:
        if not self.require_connection():
            return
        if messagebox.askyesno("Reset calibration",
                               "Reset in-RAM calibration to nominal values?\n"
                               "(Does NOT overwrite NVM — use Save Cal to persist.)"):
            threading.Thread(target=lambda: self.safe_write("CALibration:RESet"),
                             daemon=True).start()

    def _cal_enable_set(self) -> None:
        if not self.require_connection():
            return
        threading.Thread(target=lambda: self.safe_write(
            f"CALibration:ENable {self._cal_en_var.get()}"),
            daemon=True).start()

    def _cal_enable_query(self) -> None:
        if not self.require_connection():
            return
        def _do():
            resp = self.safe_query("CALibration:ENable?")
            if resp is not None:
                v = "ON" if resp.strip() in ("1", "ON") else "OFF"
                self._root.after(0, lambda x=v: self._cal_en_var.set(x))
        threading.Thread(target=_do, daemon=True).start()

    def _manual_set(self) -> None:
        if not self.require_connection():
            return
        dec, dig, val = self._m_dec.get(), self._m_dig.get(), self._m_val.get()
        try:
            ival = int(val)
            if ival < 0:
                raise ValueError
        except ValueError:
            messagebox.showerror("Bad value", "Value must be a non-negative integer (milliohms).")
            return
        try:
            idec, idig = int(dec), int(dig)
        except ValueError:
            messagebox.showerror("Bad value", "Decade and digit must be integers.")
            return
        if not (1 <= idec <= 6) or not (0 <= idig <= 9):
            messagebox.showerror("Bad value", "Decade must be 1–6 and digit 0–9.")
            return
        threading.Thread(target=lambda: self.safe_write(
            f"CALibration:DECade {dec},{dig},{val}"),
            daemon=True).start()

    def _manual_query(self) -> None:
        if not self.require_connection():
            return
        dec, dig = self._m_dec.get(), self._m_dig.get()
        def _do():
            resp = self.safe_query(f"CALibration:DECade? {dec},{dig}")
            if resp:
                self._root.after(0, lambda r=resp: self._m_val.set(r.strip()))
        threading.Thread(target=_do, daemon=True).start()

    # ── Multimeter ──

    def _open_meter_config(self) -> None:
        win = MultimeterConfigWindow(self.frame, self._state,
                                     self._log_pane, self._root)
        self._root.wait_window(win)
        self._refresh_meter_status()
        self._update_cal_estimate()

    def _refresh_meter_status(self) -> None:
        m = self._state.meter
        if m.is_connected():
            model = m.model
            mode  = "4W" if m.mode_4w else "2W"
            self._meter_status_lbl.config(
                text=f"Meter: {model}  {mode}  NPLC={m.nplc}",
                foreground=COL_OK)
            # Enable auto-cal only if RDE also connected
            if self._device.is_connected():
                self._btn_autocal.config(state="normal")
        else:
            self._meter_status_lbl.config(text="Meter: not connected",
                                          foreground=COL_WARN)
            self._btn_autocal.config(state="disabled")

    def _sync_ui_state(self, connected: bool) -> None:
        super()._sync_ui_state(connected)
        self._refresh_meter_status()

    # ── Auto-calibration ──

    def _start_autocal(self) -> None:
        if not self.require_connection():
            return
        if not self._state.meter.is_connected():
            messagebox.showwarning("Meter not connected",
                                   "Open Multimeter Config and connect first.")
            return
        if self._cal_thread and self._cal_thread.is_alive():
            return
        if not messagebox.askyesno(
                "Auto-Calibrate",
                "This will iterate all 60 calibration points (6 decades × 10 digits),\n"
                "measure each with the multimeter, and store results in RDE.\n\n"
                "RDE output relay will be engaged.  Continue?"):
            return
        self._cancel_event.clear()
        self._btn_autocal.config(state="disabled")
        self._btn_abort.config(state="normal")
        self._progress["value"] = 0
        self._cal_thread = threading.Thread(target=self._autocal_thread, daemon=True)
        self._cal_thread.start()

    def _abort_autocal(self) -> None:
        self._cancel_event.set()
        self._log_pane.log("Calibration abort requested…", "warn")

    def _autocal_thread(self) -> None:
        MAX_ERRORS  = 3
        settle_s    = max(0.1, float(self._settle_var.get()))
        intermeas_s = max(0.0, float(self._intermeas_var.get()))
        total  = 6 * 10
        done   = 0
        errors = 0
        meter  = self._state.meter

        # ensure output is on
        self.safe_write("OUTPut:RELay:ENable ON")
        self.safe_write("OUTPut:STATe ON")
        time.sleep(0.2)

        # configure meter once — settings don't change between points
        try:
            meter.configure()
        except Exception as exc:
            self._log_pane.log(f"Meter configure failed: {exc}", "error")
            self._root.after(0, lambda: (
                messagebox.showerror("Meter Error",
                                     "Failed to configure multimeter.\n"
                                     "Check connection and try again."),
                self._btn_autocal.config(state="normal"),
                self._btn_abort.config(state="disabled"),
            ))
            return

        for decade in range(1, 7):
            # Zero all decades before starting each new decade's sweep
            for d in range(1, 7):
                self.safe_write(f"RESistance:DECade {d},0")
            time.sleep(0.5)

            for digit in range(0, 10):
                if self._cancel_event.is_set():
                    break
                if errors >= MAX_ERRORS:
                    self._log_pane.log(
                        f"Stopping: {MAX_ERRORS} consecutive errors.", "error")
                    self._cancel_event.set()
                    break

                time.sleep(0.5)
                self.safe_write(f"RESistance:DECade {decade},{digit}")
                time.sleep(settle_s)

                try:
                    readings = []
                    for i in range(max(1, meter.avg_count)):
                        if i > 0:
                            time.sleep(intermeas_s)
                        readings.append(meter.measure_once())
                    val_ohm = statistics.mean(readings)
                    if not math.isfinite(val_ohm) or val_ohm < 0:
                        raise ValueError(f"Invalid measurement: {val_ohm}")
                    if val_ohm > 1.1e6:
                        raise ValueError(
                            f"Meter overload/overflow: {val_ohm:.3e} Ω")
                    milliohm = round(val_ohm * 1000)
                    errors = 0  # reset on success
                    nominal_mo = digit * DECADE_UNITS_MO[decade - 1]
                    if nominal_mo > 0 and abs(milliohm - nominal_mo) > 0.5 * nominal_mo:
                        self._log_pane.log(
                            f"WARN d{decade}/dig{digit}: {milliohm} mO deviates "
                            f">50% from nominal {nominal_mo} mO", "warn")
                    time.sleep(0.5)
                    self.safe_write(
                        f"CALibration:DECade {decade},{digit},{milliohm}")
                except Exception as exc:
                    self._log_pane.log(
                        f"Measure ERROR d{decade}/dig{digit}: {exc}", "error")
                    milliohm = -1
                    errors  += 1

                done += 1
                pct = done * 100 // total
                row_idx = decade - 1

                d, dg, mo = decade, digit, milliohm
                def _upd(d=d, dg=dg, mo=mo, p=pct, ri=row_idx):
                    self._progress["value"] = p
                    self._prog_lbl.config(text=f"d{d}/dig{dg}  {mo} mΩ")
                    if mo >= 0:
                        vals = list(self._tv.item(str(ri), "values"))
                        vals[dg] = str(mo)
                        self._tv.item(str(ri), values=vals)
                        self._state.cal_table[ri][dg] = mo
                self._root.after(0, _upd)

            if self._cancel_event.is_set():
                break

        if self._cancel_event.is_set():
            if errors >= MAX_ERRORS:
                self._log_pane.log(
                    f"Calibration stopped after {MAX_ERRORS} consecutive errors. "
                    "Values measured so far are in RDE RAM — press 'Save Cal' to persist.",
                    "warn")
            else:
                self._log_pane.log(
                    "Auto-calibration aborted by user. NVM not updated.", "warn")
        else:
            suffix = f"  ({errors} measurement error(s))" if errors else ""
            self._log_pane.log(
                f"Auto-calibration complete.{suffix} "
                "Press 'Save Cal' to persist values.",
                "ok" if errors == 0 else "warn")

        def _finish():
            self._btn_autocal.config(state="normal")
            self._btn_abort.config(state="disabled")
            self._progress["value"] = 0
            self._prog_lbl.config(text="")
        self._root.after(0, _finish)

    # ── PDF export ──

    def _export_pdf(self) -> None:
        if not FPDF_AVAILABLE:
            messagebox.showinfo(
                "fpdf2 not installed",
                "Install fpdf2 to generate PDF reports:\n\n"
                "    pip install fpdf2")
            return
        path = filedialog.asksaveasfilename(
            title="Save Calibration Report",
            defaultextension=".pdf",
            filetypes=[("PDF files", "*.pdf"), ("All files", "*.*")],
            initialfile=f"RDE_cal_{datetime.datetime.now():%Y%m%d_%H%M}.pdf")
        if not path:
            return
        threading.Thread(target=self._generate_pdf, args=(path,), daemon=True).start()

    def _generate_pdf(self, path: str) -> None:
        try:
            pdf = FPDF()
            pdf.add_page()
            pdf.set_font("Helvetica", "B", 16)
            pdf.cell(0, 10, "RDE Calibration Report", new_x="LMARGIN", new_y="NEXT", align="C")
            pdf.set_font("Helvetica", "", 10)
            pdf.ln(4)

            now = datetime.datetime.now().strftime("%Y-%m-%d  %H:%M:%S")
            m   = self._state.meter
            pdf.cell(0, 6, f"Date:      {now}", new_x="LMARGIN", new_y="NEXT")
            pdf.cell(0, 6, f"RDE IDN:   {self._state.rde_idn or 'N/A'}", new_x="LMARGIN", new_y="NEXT")
            pdf.cell(0, 6, f"Meter IDN: {m.idn or 'N/A'}", new_x="LMARGIN", new_y="NEXT")
            mode_str = "4W (FRES)" if m.mode_4w else "2W (RES)"
            pdf.cell(0, 6, f"Mode:      {mode_str},  NPLC={m.nplc},  Avg={m.avg_count}", new_x="LMARGIN", new_y="NEXT")
            pdf.ln(6)

            # Table header
            pdf.set_font("Helvetica", "B", 9)
            col_w = [22] + [17]*10
            hdr = ["Decade"] + [f"d={d}" for d in range(10)]
            for i, h in enumerate(hdr):
                pdf.cell(col_w[i], 7, h, border=1, align="C")
            pdf.ln()

            decade_names = ["1 Ohm  (d1)", "10 Ohm (d2)", "100 Ohm(d3)",
                            "1 kOhm (d4)", "10 kOhm(d5)", "100 kOhm(d6)"]
            pdf.set_font("Helvetica", "", 8)

            for row_idx, dname in enumerate(decade_names):
                pdf.cell(col_w[0], 6, dname, border=1)
                for dig in range(10):
                    mo  = self._state.cal_table[row_idx][dig]
                    nom = dig * DECADE_UNITS_MO[row_idx]
                    pct = abs(mo - nom) / nom * 100 if nom > 0 else 0
                    if pct > 5:
                        pdf.set_fill_color(255, 180, 180)
                        fill = True
                    elif pct > 1:
                        pdf.set_fill_color(255, 240, 160)
                        fill = True
                    else:
                        fill = False
                    pdf.cell(col_w[dig+1], 6, str(mo), border=1,
                             align="C", fill=fill)
                    if fill:
                        pdf.set_fill_color(255, 255, 255)
                pdf.ln()

            # ── Deviation table ──────────────────────────────────────────────
            pdf.ln(6)
            pdf.set_font("Helvetica", "B", 10)
            pdf.cell(0, 6, "Deviation from Nominal:", new_x="LMARGIN", new_y="NEXT")
            pdf.ln(2)

            # Header row — d=0 shows short-circuit offset label
            pdf.set_font("Helvetica", "B", 9)
            pdf.cell(col_w[0], 7, "Decade", border=1, align="C")
            pdf.cell(col_w[1], 7, "d=0 (mO)", border=1, align="C")
            for dig in range(1, 10):
                pdf.cell(col_w[dig + 1], 7, f"d={dig} %", border=1, align="C")
            pdf.ln()

            pdf.set_font("Helvetica", "", 8)
            decade_devs = []   # mean % per decade for summary
            for row_idx, dname in enumerate(decade_names):
                pdf.cell(col_w[0], 6, dname, border=1)
                devs_row = []
                for dig in range(10):
                    mo  = self._state.cal_table[row_idx][dig]
                    nom = dig * DECADE_UNITS_MO[row_idx]
                    if dig == 0:
                        # short-circuit: show absolute mO offset
                        cell_txt = str(mo)
                        pct = 0.0
                        fill = False
                    else:
                        pct = abs(mo - nom) / nom * 100 if nom > 0 else 0.0
                        cell_txt = f"{pct:.2f}"
                        devs_row.append(pct)
                    if dig > 0:
                        if pct > 5:
                            pdf.set_fill_color(255, 180, 180); fill = True
                        elif pct > 1:
                            pdf.set_fill_color(255, 240, 160); fill = True
                        else:
                            fill = False
                    pdf.cell(col_w[dig + 1], 6, cell_txt, border=1,
                             align="C", fill=fill)
                    if fill:
                        pdf.set_fill_color(255, 255, 255)
                pdf.ln()
                decade_devs.append(devs_row)

            # ── Summary table ─────────────────────────────────────────────────
            pdf.ln(6)
            pdf.set_font("Helvetica", "B", 10)
            pdf.cell(0, 6, "Summary:", new_x="LMARGIN", new_y="NEXT")
            pdf.ln(2)

            sw = [col_w[0] + col_w[1], 35, 35, 35]  # Decade | Short | Mean% | Max%
            pdf.set_font("Helvetica", "B", 9)
            for txt, w in zip(["Decade", "Short d=0 (mO)", "Mean dev %", "Max dev %"], sw):
                pdf.cell(w, 7, txt, border=1, align="C")
            pdf.ln()

            pdf.set_font("Helvetica", "", 9)
            all_devs = []
            for row_idx, dname in enumerate(decade_names):
                short_mo = self._state.cal_table[row_idx][0]
                devs_row = decade_devs[row_idx]
                mean_pct = statistics.mean(devs_row) if devs_row else 0.0
                max_pct  = max(devs_row)             if devs_row else 0.0
                all_devs.extend(devs_row)
                for val, w in zip([dname, str(short_mo),
                                   f"{mean_pct:.2f}", f"{max_pct:.2f}"], sw):
                    pdf.cell(w, 6, val, border=1, align="C")
                pdf.ln()

            # Overall row
            overall_mean = statistics.mean(all_devs) if all_devs else 0.0
            overall_max  = max(all_devs)             if all_devs else 0.0
            pdf.set_font("Helvetica", "B", 9)
            for val, w in zip(["ALL DECADES", "-",
                                f"{overall_mean:.2f}", f"{overall_max:.2f}"], sw):
                pdf.cell(w, 6, val, border=1, align="C")
            pdf.ln()

            pdf.output(path)
            self._root.after(0, lambda p=path: messagebox.showinfo(
                "PDF Saved", f"Report saved to:\n{p}"))
            self._log_pane.log(f"PDF report saved: {path}", "ok")
        except Exception as exc:
            self._root.after(0, lambda e=exc: messagebox.showerror(
                "PDF Error", str(e)))
            self._log_pane.log(f"PDF error: {exc}", "error")


# ─── SystemTab ────────────────────────────────────────────────────────────────

class SystemTab(_BaseTab):
    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}

        info_f = ttk.LabelFrame(self.frame, text="Device Information")
        info_f.pack(fill="x", padx=8, pady=(8, 4))

        cmds = [
            ("*IDN?",           "*IDN?"),
            ("SYSTem:ID?",      "SYSTem:ID?"),
            ("SYSTem:ID? LONG", "SYSTem:ID? LONG"),
            ("SYSTem:VERSion?", "SYSTem:VERSion?"),
            ("*TST?",           "*TST?"),
        ]
        for row, (label, cmd) in enumerate(cmds):
            b = ttk.Button(info_f, text=label, width=18,
                           command=lambda c=cmd: self._run(c))
            b.grid(row=row//3, column=(row%3)*2, **p)
            self._connected_widgets.append(b)

        ctrl_f = ttk.LabelFrame(self.frame, text="Control")
        ctrl_f.pack(fill="x", padx=8, pady=4)

        ctrl_cmds = [
            ("*RST",               "*RST"),
            ("*CLS",               "*CLS"),
            ("SYSTem:RST",         "SYSTem:RST"),
            ("SYSTem:BOOTloader",  "SYSTem:BOOTloader:ENter"),
        ]
        for col, (label, cmd) in enumerate(ctrl_cmds):
            b = ttk.Button(ctrl_f, text=label, width=20,
                           command=lambda c=cmd, l=label: self._run_ctrl(c, l))
            b.grid(row=0, column=col, **p)
            self._connected_widgets.append(b)

        err_f = ttk.LabelFrame(self.frame, text="Error Queue  (SYSTem:ERRor)")
        err_f.pack(fill="x", padx=8, pady=4)

        b_en = ttk.Button(err_f, text="Read Next Error",
                          command=self._read_next_error)
        b_da = ttk.Button(err_f, text="Drain All",
                          command=self._drain_all_errors)
        b_ec = ttk.Button(err_f, text="Count?",
                          command=lambda: self._run("SYSTem:ERRor:COUNt?"))
        b_cl = ttk.Button(err_f, text="Clear  (*CLS)",
                          command=lambda: self._run_ctrl("*CLS", "*CLS"))
        b_en.grid(row=0, column=0, **p)
        b_da.grid(row=0, column=1, **p)
        b_ec.grid(row=0, column=2, **p)
        b_cl.grid(row=0, column=3, **p)

        self._err_lbl = ttk.Label(err_f, text="",
                                   foreground=COL_ERR, font=("Courier New", 9))
        self._err_lbl.grid(row=0, column=4, padx=10, sticky="w")

        self._connected_widgets += [b_en, b_da, b_ec, b_cl]

        # ── NVM / I2C Diagnostics ──
        nvm_f = ttk.LabelFrame(self.frame, text="NVM / I2C Diagnostics  (FM24C64B FRAM)")
        nvm_f.pack(fill="x", padx=8, pady=4)

        b_fp = ttk.Button(nvm_f, text="Test FRAM (PING+DIAG)", command=self._fram_test)
        b_fp.grid(row=0, column=0, **p)
        b_sc = ttk.Button(nvm_f, text="I2C Bus Scan", command=self._i2c_scan)
        b_sc.grid(row=0, column=1, **p)
        self._connected_widgets += [b_fp, b_sc]

        ttk.Label(nvm_f,
                  text="Switch NVM backend:  exclude relay_cal.c (flash) or relay_cal_flash.c (FRAM)  in .cproject",
                  foreground=COL_INFO).grid(row=1, column=0, columnspan=3, sticky="w", padx=6, pady=2)

    _I2C_ISR_BITS = [
        (0,  "TXE"),   (1,  "TXIS"),  (2,  "RXNE"),  (3,  "ADDR"),
        (4,  "NACKF"), (5,  "STOPF"), (6,  "TC"),     (7,  "TCR"),
        (15, "BUSY"),  (16, "DIR"),
    ]

    def _decode_i2c_isr(self, isr: int) -> str:
        flags = [name for bit, name in self._I2C_ISR_BITS if isr & (1 << bit)]
        return ",".join(flags) if flags else "none"

    def _fram_test(self) -> None:
        if not self.require_connection():
            return
        def _do():
            diag_str = None
            try:
                diag_str = self._device.query("SYSTem:FRAM:DIAG?").strip()
            except Exception:
                pass
            if diag_str:
                parts = diag_str.split(",")
                ping  = parts[0].strip() == "1" if parts else False
                isr   = int(parts[1].strip(), 16) if len(parts) > 1 else 0
                state = int(parts[2].strip()) if len(parts) > 2 else 0
                flags = self._decode_i2c_isr(isr)
                self._log_pane.log(
                    f"FRAM diag: ping={'OK' if ping else 'FAIL'}  "
                    f"I2C ISR=0x{isr:08X} [{flags}]  HAL_state={state}",
                    "ok" if ping else "error")
                if not ping:
                    busy  = bool(isr & (1 << 15))
                    nackf = bool(isr & (1 << 4))
                    if busy:
                        hint = "I2C bus stuck BUSY — hardware reset or SDA/SCL glitch needed."
                    elif nackf:
                        hint = "NACK — chip not at 0x50. Check A0=A1=A2=GND."
                    else:
                        hint = "No response — check FRAM power, wiring, pull-ups."
                    full = (f"FRAM NOT responding.\n"
                            f"I2C ISR=0x{isr:08X} [{flags}]  HAL state={state}\n\n{hint}")
                    self._root.after(0, lambda m=full: messagebox.showerror("FRAM Test", m))
            else:
                try:
                    resp = self._device.query("SYSTem:FRAM:PING?").strip()
                    ping = resp in ("1", "ON", "TRUE")
                except Exception:
                    ping = None
                if ping is None:
                    self._log_pane.log("FRAM ping: communication error.", "error")
                elif ping:
                    self._log_pane.log("FRAM ping: OK (1).", "ok")
                else:
                    self._log_pane.log("FRAM ping: FAIL (0).", "error")
        threading.Thread(target=_do, daemon=True).start()

    def _i2c_scan(self) -> None:
        if not self.require_connection():
            return
        def _do():
            try:
                result = self._device.query("SYSTem:I2C:SCAN?").strip()
            except Exception as exc:
                self._log_pane.log(f"I2C scan error: {exc}", "error")
                return
            if result == "NONE":
                msg = "I2C scan: NO devices found (bus empty or wiring issue)."
                self._log_pane.log(msg, "error")
                self._root.after(0, lambda: messagebox.showwarning("I2C Scan", msg))
            else:
                notes = []
                for tok in result.split(","):
                    try:
                        addr = int(tok.strip(), 16)
                        if addr == 0x50:
                            notes.append("0x50 = FM24C64B FRAM — OK")
                        elif addr == 0x51:
                            notes.append("0x51 = extra EEPROM (E0=VCC)")
                        else:
                            notes.append(f"0x{addr:02X} = unknown")
                    except ValueError:
                        pass
                detail = "\n".join(notes)
                self._log_pane.log(f"I2C scan: {result}", "ok")
                self._root.after(0, lambda d=detail: messagebox.showinfo("I2C Scan", d))
        threading.Thread(target=_do, daemon=True).start()

    def _read_next_error(self) -> None:
        """Read one error from the SCPI queue and display code + description."""
        if not self.require_connection():
            return
        def _do():
            try:
                resp = self._device.query("SYSTem:ERRor:NEXT?")
            except Exception as exc:
                self._log_pane.log(f"ERROR (error query): {exc}", "error")
                return
            code, desc = parse_scpi_error(resp)
            if code == 0:
                self._log_pane.log("Error queue: empty — no errors", "ok")
                self._root.after(0, lambda: self._err_lbl.config(text=""))
            else:
                self._log_pane.log(f"SCPI error  [{code:+d}]  {desc}", "error")
                self._root.after(0, lambda c=code, d=desc:
                                 self._err_lbl.config(text=f"{c}: {d}"))
        threading.Thread(target=_do, daemon=True).start()

    def _drain_all_errors(self) -> None:
        """Read the entire SCPI error queue and log each entry with description."""
        if not self.require_connection():
            return
        def _do():
            n = self.drain_scpi_errors(silent_if_empty=False)
            self._root.after(0, lambda: self._err_lbl.config(
                text=f"{n} error(s) cleared — see log" if n else ""))
        threading.Thread(target=_do, daemon=True).start()

    def _run(self, cmd: str) -> None:
        if not self.require_connection():
            return
        threading.Thread(target=lambda: self.safe_query(cmd)
                         if cmd.endswith("?") else self.safe_write(cmd),
                         daemon=True).start()

    def _run_ctrl(self, cmd: str, label: str) -> None:
        if not self.require_connection():
            return
        if not messagebox.askyesno("Confirm", f"Send  {label}  to device?"):
            return
        threading.Thread(target=lambda: self.safe_write(cmd), daemon=True).start()


# ─── DFUTab ───────────────────────────────────────────────────────────────────

DFU_TOOL_CANDIDATES = [
    ("STM32_Programmer_CLI",
     ["STM32_Programmer_CLI", "--version"]),
    ("dfu-util",
     ["dfu-util", "--version"]),
]


class DFUTab(_BaseTab):
    def _build(self) -> None:
        p = {"padx": 6, "pady": 4}

        tool_f = ttk.LabelFrame(self.frame, text="DFU Tool")
        tool_f.pack(fill="x", padx=8, pady=(8, 4))

        ttk.Button(tool_f, text="Detect Tool",
                   command=self._detect_tool).grid(row=0, column=0, **p)
        self._tool_lbl = ttk.Label(tool_f, text="—", foreground=COL_WARN,
                                   font=("Courier New", 9))
        self._tool_lbl.grid(row=0, column=1, sticky="w", **p)

        fw_f = ttk.LabelFrame(self.frame, text="Firmware File")
        fw_f.pack(fill="x", padx=8, pady=4)

        self._fw_var = tk.StringVar()
        ttk.Entry(fw_f, textvariable=self._fw_var, width=52).grid(
            row=0, column=0, **p)
        ttk.Button(fw_f, text="Browse…",
                   command=self._browse_fw).grid(row=0, column=1, **p)

        act_f = ttk.LabelFrame(self.frame, text="Actions")
        act_f.pack(fill="x", padx=8, pady=4)

        self._btn_bootloader = ttk.Button(
            act_f, text="Enter Bootloader (via SCPI)",
            command=self._enter_bootloader)
        self._btn_bootloader.grid(row=0, column=0, **p)
        self._connected_widgets.append(self._btn_bootloader)

        self._btn_flash = ttk.Button(act_f, text="Flash Firmware",
                                     command=self._start_flash)
        self._btn_flash.grid(row=0, column=1, **p)

        self._btn_cancel = ttk.Button(act_f, text="Cancel",
                                      command=self._cancel_flash,
                                      state="disabled")
        self._btn_cancel.grid(row=0, column=2, **p)

        self._progress = ttk.Progressbar(act_f, length=200, mode="indeterminate")
        self._progress.grid(row=0, column=3, **p)

        log_f = ttk.LabelFrame(self.frame, text="DFU Log")
        log_f.pack(fill="both", expand=True, padx=8, pady=4)
        self._dfu_log = LogPane(log_f, height=12)

        self._tool_name: Optional[str] = None
        self._tool_path: Optional[str] = None
        self._flash_cancel = threading.Event()
        self._flash_thread: Optional[threading.Thread] = None

        self._detect_tool()

    def _detect_tool(self) -> None:
        for name, probe in DFU_TOOL_CANDIDATES:
            path = shutil.which(probe[0])
            if path:
                try:
                    result = subprocess.run(probe, capture_output=True,
                                            timeout=5)
                    if result.returncode == 0 or name == "dfu-util":
                        self._tool_name = name
                        self._tool_path = path
                        self._tool_lbl.config(
                            text=f"{name}  ({path})", foreground=COL_OK)
                        self._dfu_log.log(f"Found: {name} at {path}", "ok")
                        return
                except Exception:
                    pass
        self._tool_lbl.config(
            text="No DFU tool found (install STM32_Programmer_CLI or dfu-util)",
            foreground=COL_ERR)
        self._dfu_log.log(
            "No DFU tool detected.  Install STM32CubeProgrammer or dfu-util.", "warn")

    def _browse_fw(self) -> None:
        path = filedialog.askopenfilename(
            title="Select Firmware File",
            filetypes=[("Binary/ELF", "*.bin *.elf"), ("All files", "*.*")])
        if path:
            self._fw_var.set(path)

    def _enter_bootloader(self) -> None:
        if not self.require_connection():
            return
        if not messagebox.askyesno(
                "Enter Bootloader",
                "This will send SYSTem:BOOTloader:ENter to the device.\n"
                "The USB connection will drop. Proceed?"):
            return
        def _do():
            self.safe_write("SYSTem:BOOTloader:ENter")
            self._dfu_log.log("Bootloader command sent. Waiting for re-enumeration…", "info")
            time.sleep(2.5)
            self._dfu_log.log("Device should now be in DFU mode.", "ok")
        threading.Thread(target=_do, daemon=True).start()

    def _start_flash(self) -> None:
        fw = self._fw_var.get().strip()
        if not fw or not os.path.isfile(fw):
            messagebox.showerror("File not found",
                                 f"Firmware file not found:\n{fw}")
            return
        if not self._tool_name:
            messagebox.showerror("No tool", "No DFU tool detected.")
            return
        if self._flash_thread and self._flash_thread.is_alive():
            return
        self._flash_cancel.clear()
        self._btn_flash.config(state="disabled")
        self._btn_cancel.config(state="normal")
        self._progress.start(10)
        self._flash_thread = threading.Thread(
            target=self._flash_thread_fn, args=(fw,), daemon=True)
        self._flash_thread.start()

    def _flash_thread_fn(self, fw: str) -> None:
        self._dfu_log.log(f"Flashing: {fw}", "info")
        try:
            if self._tool_name == "STM32_Programmer_CLI":
                cmd = [self._tool_path,
                       "--connect", "port=usb1",
                       "--write", fw, "0x08000000",
                       "--verify", "--go"]
            else:
                cmd = [self._tool_path,
                       "-a", "0",
                       "-D", fw,
                       "--reset"]

            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            for line in proc.stdout:
                if self._flash_cancel.is_set():
                    proc.terminate()
                    break
                l = line.rstrip()
                self._dfu_log.log(l, "ok" if "OK" in l or "success" in l.lower()
                                  else "info" if l else "")
            proc.wait()
            ok = proc.returncode == 0 and not self._flash_cancel.is_set()
            self._dfu_log.log(
                f"Flash {'succeeded' if ok else 'failed/aborted'}  (rc={proc.returncode})",
                "ok" if ok else "error")
        except Exception as exc:
            self._dfu_log.log(f"Flash ERROR: {exc}", "error")
        finally:
            def _done():
                self._btn_flash.config(state="normal")
                self._btn_cancel.config(state="disabled")
                self._progress.stop()
            self._root.after(0, _done)

    def _cancel_flash(self) -> None:
        self._flash_cancel.set()
        self._dfu_log.log("Cancel requested…", "warn")


# ─── ConsoleTab ───────────────────────────────────────────────────────────────

class ConsoleTab(_BaseTab):
    def _build(self) -> None:
        top_f = ttk.Frame(self.frame)
        top_f.pack(fill="x", padx=8, pady=(8, 2))

        ttk.Label(top_f, text="Command:").pack(side="left")
        self._cmd_var = tk.StringVar()
        self._entry   = ttk.Entry(top_f, textvariable=self._cmd_var, width=55,
                                  font=("Courier New", 10))
        self._entry.pack(side="left", padx=6)
        self._entry.bind("<Return>",  self._on_send)
        self._entry.bind("<Up>",      self._history_up)
        self._entry.bind("<Down>",    self._history_down)

        b_send = ttk.Button(top_f, text="Send →", command=self._on_send)
        b_send.pack(side="left", padx=2)
        self._connected_widgets += [self._entry, b_send]

        ttk.Label(top_f, text="↑↓ history",
                  foreground=COL_CMD).pack(side="left", padx=6)

        # Quick command shortcuts
        qf = ttk.LabelFrame(self.frame, text="Quick Commands")
        qf.pack(fill="x", padx=8, pady=4)
        quick = [
            ("*IDN?", "*IDN?"), ("*RST", "*RST"), ("*CLS", "*CLS"),
            ("RES:VAL?", "RESistance:VALue?"),
            ("RES:VAL 0", "RESistance:VALue 0"),
            ("RES:VAL 1000", "RESistance:VALue 1000"),
            ("OUT ON", "OUTPut:STATe ON"), ("OUT OFF", "OUTPut:STATe OFF"),
            ("REL_EN ON", "OUTPut:RELay:ENable ON"),
            ("REL_EN OFF", "OUTPut:RELay:ENable OFF"),
            ("NET:STATe?", "NET:STATe?"),
            ("CAL:ENable ON", "CALibration:ENable ON"),
            ("CAL:ENable OFF", "CALibration:ENable OFF"),
            ("CAL:SAVE", "CALibration:SAVE"),
            ("ERR?", "SYSTem:ERRor[:NEXT]?"),
            ("SYS:ID?", "SYSTem:ID?"),
        ]
        cols = 4
        for idx, (label, cmd) in enumerate(quick):
            r, c = divmod(idx, cols)
            b = ttk.Button(qf, text=label, width=20,
                           command=lambda c2=cmd: self._send_cmd(c2))
            b.grid(row=r, column=c, padx=4, pady=2)
            self._connected_widgets.append(b)

        log_f = ttk.LabelFrame(self.frame, text="Console Log")
        log_f.pack(fill="both", expand=True, padx=8, pady=4)
        self._console_log = LogPane(log_f, height=14)
        # Override _log_pane for this tab so quick buttons log here too
        self._log_pane = self._console_log

        self._history: List[str] = []
        self._history_idx = -1

    def _on_send(self, _=None) -> None:
        cmd = self._cmd_var.get().strip()
        if not cmd:
            return
        if not self.require_connection():
            return
        self._history.append(cmd)
        self._history_idx = -1
        self._cmd_var.set("")
        threading.Thread(target=self._exec_cmd, args=(cmd,), daemon=True).start()

    def _send_cmd(self, cmd: str) -> None:
        if not self.require_connection():
            return
        threading.Thread(target=self._exec_cmd, args=(cmd,), daemon=True).start()

    def _exec_cmd(self, cmd: str) -> None:
        stripped = cmd.strip()
        first = stripped.split()[0] if stripped else ""
        if first.endswith("?"):
            self.safe_query(cmd)
        else:
            self.safe_write(cmd)

    def _history_up(self, event) -> str:
        if not self._history:
            return "break"
        self._history_idx = min(self._history_idx + 1, len(self._history) - 1)
        self._cmd_var.set(self._history[-(self._history_idx + 1)])
        self._entry.icursor(tk.END)
        return "break"

    def _history_down(self, event) -> str:
        if self._history_idx <= 0:
            self._history_idx = -1
            self._cmd_var.set("")
            return "break"
        self._history_idx -= 1
        self._cmd_var.set(self._history[-(self._history_idx + 1)])
        self._entry.icursor(tk.END)
        return "break"


# ─── RdeControlApp ────────────────────────────────────────────────────────────

class RdeControlApp:
    def __init__(self, root: tk.Tk):
        self._root   = root
        self._device = SCPIDevice()
        self._state  = AppState()

        root.title(f"{APP_TITLE}  v{APP_VERSION}")
        root.minsize(WIN_MIN_W, WIN_MIN_H)

        # Status bar (bottom)
        self._status_bar = StatusBar(root)

        # Shared log pane (bottom panel, above status)
        log_outer = ttk.LabelFrame(root, text="Log")
        log_outer.pack(side="bottom", fill="x", padx=6, pady=(0, 2))
        self._log_pane = LogPane(log_outer, height=7)

        # Notebook (main area)
        nb = ttk.Notebook(root)
        nb.pack(fill="both", expand=True, padx=6, pady=(6, 2))

        kw = dict(state=self._state, device=self._device,
                  log_pane=self._log_pane, status_bar=self._status_bar,
                  root=root)

        self.tab_conn  = ConnectionTab(nb, "Connection",  **kw)
        self.tab_res   = ResistanceTab(nb, "Resistance",  **kw)
        self.tab_relay = RelayTab(     nb, "Relay",       **kw)
        self.tab_net   = NetworkTab(   nb, "Network",     **kw)
        self.tab_cal   = CalibrationTab(nb, "Calibration", **kw)
        self.tab_sys   = SystemTab(    nb, "System",      **kw)
        self.tab_dfu   = DFUTab(       nb, "DFU",         **kw)
        self.tab_con   = ConsoleTab(   nb, "Console",     **kw)

        self.tab_conn.on_connect_callback    = self._on_device_connected
        self.tab_conn.on_disconnect_callback = self._on_device_disconnected

        # Initially disable all connected-only widgets
        self._on_device_disconnected()

        root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _on_device_connected(self, idn: str) -> None:
        self._state.rde_idn = idn
        for tab in (self.tab_res, self.tab_relay, self.tab_net,
                    self.tab_cal, self.tab_sys, self.tab_con):
            tab._sync_ui_state(True)

    def _on_device_disconnected(self) -> None:
        for tab in (self.tab_res, self.tab_relay, self.tab_net,
                    self.tab_cal, self.tab_sys, self.tab_con):
            tab._sync_ui_state(False)

    def _on_close(self) -> None:
        try:
            self._device.disconnect()
        except Exception:
            pass
        try:
            m = self._state.meter
            if m.is_connected():
                m.disconnect()
        except Exception:
            pass
        self._root.destroy()


# ─── Entry point ──────────────────────────────────────────────────────────────

def main() -> None:
    if THEMES_AVAILABLE:
        root = ThemedTk(theme="arc")
    else:
        root = tk.Tk()

    if not VISA_AVAILABLE:
        import tkinter.messagebox as mb
        mb.showwarning(
            "pyvisa not installed",
            "pyvisa is not installed — device communication will not work.\n\n"
            "Install with:\n    pip install pyvisa pyvisa-py")

    RdeControlApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
