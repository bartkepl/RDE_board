"""
rde_tester.py – SCPI tester for RDE_board (Resistance DEcade)
Supports USB-TMC and VXI-11/Ethernet via PyVISA.

Requirements:
    pip install pyvisa pyvisa-py
For USB-TMC on Windows also install:
    pip install pyusb  (+ libusb driver via Zadig)
Or use NI-VISA (installs with NI-MAX).
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
import threading
import datetime

try:
    import pyvisa
    VISA_AVAILABLE = True
except ImportError:
    VISA_AVAILABLE = False


class RdeTester:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("RDE_board SCPI Tester")
        self.root.resizable(True, True)

        self.rm = None
        self.instrument = None
        self._build_ui()

    # ─── UI construction ──────────────────────────────────────────────────────

    def _build_ui(self):
        pad = {"padx": 6, "pady": 4}

        # ── Connection frame ──
        conn_frame = ttk.LabelFrame(self.root, text="Connection")
        conn_frame.grid(row=0, column=0, columnspan=2, sticky="ew", **pad)

        ttk.Label(conn_frame, text="Interface:").grid(row=0, column=0, **pad)
        self.iface_var = tk.StringVar(value="VXI-11/Ethernet")
        iface_cb = ttk.Combobox(conn_frame, textvariable=self.iface_var, width=18,
                                 values=["VXI-11/Ethernet", "USB-TMC"], state="readonly")
        iface_cb.grid(row=0, column=1, **pad)
        iface_cb.bind("<<ComboboxSelected>>", self._on_iface_change)

        ttk.Label(conn_frame, text="Address:").grid(row=0, column=2, **pad)
        self.addr_var = tk.StringVar(value="TCPIP::192.168.1.100::INSTR")
        self.addr_entry = ttk.Entry(conn_frame, textvariable=self.addr_var, width=34)
        self.addr_entry.grid(row=0, column=3, **pad)

        self.btn_connect = ttk.Button(conn_frame, text="Connect",    command=self._connect)
        self.btn_connect.grid(row=0, column=4, **pad)
        self.btn_disconnect = ttk.Button(conn_frame, text="Disconnect", command=self._disconnect,
                                          state="disabled")
        self.btn_disconnect.grid(row=0, column=5, **pad)

        self.status_lbl = ttk.Label(conn_frame, text="Disconnected", foreground="red")
        self.status_lbl.grid(row=0, column=6, **pad)

        # ── Resistance frame ──
        res_frame = ttk.LabelFrame(self.root, text="Resistance Control")
        res_frame.grid(row=1, column=0, sticky="ew", **pad)

        ttk.Label(res_frame, text="Value (Ω):").grid(row=0, column=0, **pad)
        self.res_var = tk.StringVar(value="1000")
        res_entry = ttk.Entry(res_frame, textvariable=self.res_var, width=10)
        res_entry.grid(row=0, column=1, **pad)
        res_entry.bind("<Return>", lambda _: self._res_set())
        ttk.Button(res_frame, text="Set",   command=self._res_set).grid(row=0, column=2, **pad)
        ttk.Button(res_frame, text="Query", command=self._res_query).grid(row=0, column=3, **pad)

        # Resistance slider (0–999 999 Ω, logarithmic feel via step buttons)
        self.res_slider = ttk.Scale(res_frame, from_=0, to=999999, orient="horizontal",
                                    length=300, command=self._on_slider)
        self.res_slider.grid(row=0, column=4, padx=10)

        # Output terminal relay controls
        ttk.Label(res_frame, text="Output:").grid(row=0, column=5, **pad)
        self.out_var = tk.StringVar(value="ON")
        out_cb = ttk.Combobox(res_frame, textvariable=self.out_var, width=5,
                               values=["ON", "OFF"], state="readonly")
        out_cb.grid(row=0, column=6, **pad)
        ttk.Button(res_frame, text="Set Output",  command=self._out_set).grid(row=0, column=7, **pad)
        ttk.Button(res_frame, text="Query Output", command=self._out_query).grid(row=0, column=8, **pad)

        # ── Quick commands ──
        qcmd_frame = ttk.LabelFrame(self.root, text="Quick Commands")
        qcmd_frame.grid(row=2, column=0, sticky="ew", **pad)

        quick_cmds = [
            ("*IDN?",               "*IDN?"),
            ("*RST",                "*RST"),
            ("*TST?",               "*TST?"),
            ("*CLS",                "*CLS"),
            ("RES:VAL 0",           "RESistance:VALue 0"),
            ("RES:VAL 1000",        "RESistance:VALue 1000"),
            ("RES:VAL 10000",       "RESistance:VALue 10000"),
            ("RES:VAL?",            "RESistance:VALue?"),
            ("OUT ON",              "OUTPut:STATe ON"),
            ("OUT OFF",             "OUTPut:STATe OFF"),
            ("OUT?",                "OUTPut:STATe?"),
            ("SYSTem:ID?",          "SYSTem:ID?"),
            ("SYSTem:RST",          "SYSTem:RST"),
            ("SYSTem:BOOTloader",   "SYSTem:BOOTloader:ENter"),
        ]
        cols = 4
        for idx, (label, cmd) in enumerate(quick_cmds):
            r, c = divmod(idx, cols)
            ttk.Button(qcmd_frame, text=label, width=22,
                       command=lambda c=cmd: self._send(c)).grid(row=r, column=c, **pad)

        # ── Manual command ──
        man_frame = ttk.LabelFrame(self.root, text="Manual Command")
        man_frame.grid(row=3, column=0, sticky="ew", **pad)

        self.cmd_var = tk.StringVar()
        cmd_entry = ttk.Entry(man_frame, textvariable=self.cmd_var, width=48)
        cmd_entry.grid(row=0, column=0, **pad)
        cmd_entry.bind("<Return>", lambda _: self._send(self.cmd_var.get()))
        ttk.Button(man_frame, text="Send →", command=lambda: self._send(self.cmd_var.get())).grid(
            row=0, column=1, **pad)

        # ── Response log ──
        log_frame = ttk.LabelFrame(self.root, text="Response Log")
        log_frame.grid(row=4, column=0, sticky="nsew", **pad)
        self.root.rowconfigure(4, weight=1)
        self.root.columnconfigure(0, weight=1)

        self.log = scrolledtext.ScrolledText(log_frame, height=14, wrap=tk.WORD,
                                              state="disabled", font=("Courier New", 9))
        self.log.grid(row=0, column=0, sticky="nsew")
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        ttk.Button(log_frame, text="Clear Log",
                   command=self._clear_log).grid(row=1, column=0, sticky="e", padx=4, pady=2)

    # ─── Interface helpers ────────────────────────────────────────────────────

    def _on_iface_change(self, _event=None):
        iface = self.iface_var.get()
        if iface == "USB-TMC":
            self.addr_var.set("USB::0xCAFE::0x4000::INSTR")
        else:
            self.addr_var.set("TCPIP::192.168.1.100::INSTR")

    def _connect(self):
        if not VISA_AVAILABLE:
            self._log("ERROR: pyvisa not installed. Run: pip install pyvisa pyvisa-py")
            return
        addr = self.addr_var.get().strip()
        self._log(f"Connecting to {addr} …")
        threading.Thread(target=self._connect_thread, args=(addr,), daemon=True).start()

    def _connect_thread(self, addr):
        try:
            self.rm = pyvisa.ResourceManager()
            self.instrument = self.rm.open_resource(addr)
            self.instrument.timeout = 3000
            self.root.after(0, self._on_connected)
        except Exception as exc:
            self.root.after(0, lambda: self._log(f"Connect ERROR: {exc}"))

    def _on_connected(self):
        self.status_lbl.config(text="Connected", foreground="green")
        self.btn_connect.config(state="disabled")
        self.btn_disconnect.config(state="normal")
        self._log("Connected.")
        # Auto-identify
        threading.Thread(target=lambda: self._send("*IDN?"), daemon=True).start()

    def _disconnect(self):
        if self.instrument:
            try:
                self.instrument.close()
            except Exception:
                pass
            self.instrument = None
        if self.rm:
            self.rm.close()
            self.rm = None
        self.status_lbl.config(text="Disconnected", foreground="red")
        self.btn_connect.config(state="normal")
        self.btn_disconnect.config(state="disabled")
        self._log("Disconnected.")

    # ─── SCPI commands ────────────────────────────────────────────────────────

    def _send(self, cmd: str):
        if not cmd.strip():
            return
        if not self.instrument:
            self._log("Not connected.")
            return
        threading.Thread(target=self._send_thread, args=(cmd.strip(),), daemon=True).start()

    def _send_thread(self, cmd: str):
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self._log(f"[{ts}] >> {cmd}")
        try:
            if cmd.endswith("?"):
                resp = self.instrument.query(cmd).strip()
                self._log(f"       << {resp}")
            else:
                self.instrument.write(cmd)
                self._log(f"       (sent)")
        except Exception as exc:
            self._log(f"       ERROR: {exc}")

    def _res_set(self):
        val = self.res_var.get().strip()
        try:
            v = int(float(val))
            v = max(0, min(999999, v))
            self.res_var.set(str(v))
            self.res_slider.set(v)
        except ValueError:
            pass
        self._send(f"RESistance:VALue {self.res_var.get()}")

    def _res_query(self):
        self._send("RESistance:VALue?")

    def _on_slider(self, val):
        v = int(float(val))
        self.res_var.set(str(v))

    def _out_set(self):
        self._send(f"OUTPut:STATe {self.out_var.get()}")

    def _out_query(self):
        self._send("OUTPut:STATe?")

    # ─── Log ─────────────────────────────────────────────────────────────────

    def _log(self, msg: str):
        def _append():
            self.log.config(state="normal")
            self.log.insert(tk.END, msg + "\n")
            self.log.see(tk.END)
            self.log.config(state="disabled")
        self.root.after(0, _append)

    def _clear_log(self):
        self.log.config(state="normal")
        self.log.delete("1.0", tk.END)
        self.log.config(state="disabled")


if __name__ == "__main__":
    root = tk.Tk()
    app = RdeTester(root)
    root.mainloop()
