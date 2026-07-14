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
from tkinter import ttk, scrolledtext, messagebox
import threading
import datetime
import socket
import ipaddress
import concurrent.futures

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

        # ── Row 0: Connection ──
        conn_frame = ttk.LabelFrame(self.root, text="Connection")
        conn_frame.grid(row=0, column=0, columnspan=2, sticky="ew", **pad)

        ttk.Label(conn_frame, text="Interface:").grid(row=0, column=0, **pad)
        self.iface_var = tk.StringVar(value="VXI-11/Ethernet")
        iface_cb = ttk.Combobox(conn_frame, textvariable=self.iface_var, width=18,
                                 values=["VXI-11/Ethernet", "USB-TMC"], state="readonly")
        iface_cb.grid(row=0, column=1, **pad)
        iface_cb.bind("<<ComboboxSelected>>", self._on_iface_change)

        ttk.Label(conn_frame, text="Address:").grid(row=0, column=2, **pad)
        self.addr_var = tk.StringVar(value="TCPIP::192.168.1.176::INSTR")
        self.addr_entry = ttk.Combobox(conn_frame, textvariable=self.addr_var, width=34)
        self.addr_entry.grid(row=0, column=3, **pad)

        self.btn_scan = ttk.Button(conn_frame, text="Scan", command=self._scan_vxi11)
        self.btn_scan.grid(row=0, column=4, **pad)

        self.btn_connect = ttk.Button(conn_frame, text="Connect",    command=self._connect)
        self.btn_connect.grid(row=0, column=5, **pad)
        self.btn_disconnect = ttk.Button(conn_frame, text="Disconnect", command=self._disconnect,
                                          state="disabled")
        self.btn_disconnect.grid(row=0, column=6, **pad)

        self.status_lbl = ttk.Label(conn_frame, text="Disconnected", foreground="red")
        self.status_lbl.grid(row=0, column=7, **pad)

        # ── Row 1: Resistance Control ──
        res_frame = ttk.LabelFrame(self.root, text="Resistance Control")
        res_frame.grid(row=1, column=0, columnspan=2, sticky="ew", **pad)

        ttk.Label(res_frame, text="Value (Ω):").grid(row=0, column=0, **pad)
        self.res_var = tk.StringVar(value="1000")
        res_entry = ttk.Entry(res_frame, textvariable=self.res_var, width=10)
        res_entry.grid(row=0, column=1, **pad)
        res_entry.bind("<Return>", lambda _: self._res_set())
        ttk.Button(res_frame, text="Set",   command=self._res_set).grid(row=0, column=2, **pad)
        ttk.Button(res_frame, text="Query", command=self._res_query).grid(row=0, column=3, **pad)

        self.res_slider = ttk.Scale(res_frame, from_=0, to=999999, orient="horizontal",
                                    length=300, command=self._on_slider)
        self.res_slider.grid(row=0, column=4, padx=10)

        ttk.Label(res_frame, text="Output:").grid(row=0, column=5, **pad)
        self.out_var = tk.StringVar(value="ON")
        out_cb = ttk.Combobox(res_frame, textvariable=self.out_var, width=5,
                               values=["ON", "OFF"], state="readonly")
        out_cb.grid(row=0, column=6, **pad)
        ttk.Button(res_frame, text="Set Output",   command=self._out_set).grid(row=0, column=7, **pad)
        ttk.Button(res_frame, text="Query Output", command=self._out_query).grid(row=0, column=8, **pad)

        # ── Row 2: Decade Control ──
        dec_frame = ttk.LabelFrame(self.root, text="Decade Control  (RESistance:DECade)")
        dec_frame.grid(row=2, column=0, columnspan=2, sticky="ew", **pad)

        decade_labels = ["100kΩ", "10kΩ", "1kΩ", "100Ω", "10Ω", "1Ω"]
        # decade_vars[0] = decade 6 (100kΩ), ..., decade_vars[5] = decade 1 (1Ω)
        self.decade_vars = [tk.StringVar(value="0") for _ in range(6)]

        for col, (lbl, var) in enumerate(zip(decade_labels, self.decade_vars)):
            ttk.Label(dec_frame, text=lbl, anchor="center", width=7).grid(row=0, column=col, padx=4)
            decade_num = 6 - col  # col 0 → decade 6 (100kΩ), col 5 → decade 1 (1Ω)
            sb = ttk.Spinbox(dec_frame, from_=0, to=9, textvariable=var, width=4,
                             justify="center")
            sb.grid(row=1, column=col, padx=4, pady=2)
            sb.bind("<Return>", lambda _, d=decade_num: self._decade_set_one(d))

        btn_row = ttk.Frame(dec_frame)
        btn_row.grid(row=1, column=6, padx=8)
        ttk.Button(btn_row, text="Set All",   command=self._decade_set_all).pack(side="left", padx=2)
        ttk.Button(btn_row, text="Query All", command=self._decade_query_all).pack(side="left", padx=2)

        ttk.Label(dec_frame,
                  text="Each spinbox = digit 0–9 for that decade.  "
                       "Set All sends 6 × RESistance:DECade (with protection sequence).",
                  foreground="gray").grid(row=2, column=0, columnspan=7, sticky="w", padx=4, pady=2)

        # ── Row 3: Relay Power + Individual Relay + Raw Relay ──
        relay_outer = ttk.Frame(self.root)
        relay_outer.grid(row=3, column=0, columnspan=2, sticky="ew", **pad)
        relay_outer.columnconfigure(0, weight=1)
        relay_outer.columnconfigure(1, weight=2)

        # Relay power (REL_EN)
        relen_frame = ttk.LabelFrame(relay_outer, text="Relay Power Supply (REL_EN / PA4)")
        relen_frame.grid(row=0, column=0, sticky="ew", padx=(0, 4))

        ttk.Label(relen_frame, text="REL_EN:").grid(row=0, column=0, **pad)
        self.relen_var = tk.StringVar(value="OFF")
        relen_cb = ttk.Combobox(relen_frame, textvariable=self.relen_var, width=5,
                                 values=["ON", "OFF"], state="readonly")
        relen_cb.grid(row=0, column=1, **pad)
        ttk.Button(relen_frame, text="Set", command=self._relen_set).grid(row=0, column=2, **pad)
        ttk.Button(relen_frame, text="Query", command=self._relen_query).grid(row=0, column=3, **pad)
        ttk.Label(relen_frame,
                  text="Enable BEFORE using relays.\nDisable when done.",
                  foreground="gray", justify="left").grid(row=1, column=0, columnspan=4, padx=6)

        # Individual relay (RELay:STATe) + Raw relay (RELay:RAW) in one frame
        relay_ctrl_frame = ttk.LabelFrame(relay_outer, text="Relay Control")
        relay_ctrl_frame.grid(row=0, column=1, sticky="ew", padx=(4, 0))
        relay_ctrl_frame.columnconfigure(0, weight=1)
        relay_ctrl_frame.columnconfigure(1, weight=1)

        # Left: RELay:STATe
        state_frame = ttk.LabelFrame(relay_ctrl_frame, text="Individual Relay (RELay:STATe)")
        state_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 4), pady=2)

        ttk.Label(state_frame, text="Decade:").grid(row=0, column=0, **pad)
        self.rstate_decade_var = tk.StringVar(value="1")
        ttk.Spinbox(state_frame, from_=1, to=6, textvariable=self.rstate_decade_var,
                    width=4, justify="center").grid(row=0, column=1, **pad)

        ttk.Label(state_frame, text="Bit:").grid(row=0, column=2, **pad)
        self.rstate_bit_var = tk.StringVar(value="0")
        ttk.Spinbox(state_frame, from_=0, to=5, textvariable=self.rstate_bit_var,
                    width=4, justify="center").grid(row=0, column=3, **pad)

        ttk.Button(state_frame, text="ON",    command=lambda: self._relay_state_set(True)).grid(row=0, column=4, **pad)
        ttk.Button(state_frame, text="OFF",   command=lambda: self._relay_state_set(False)).grid(row=0, column=5, **pad)
        ttk.Button(state_frame, text="Query", command=self._relay_state_query).grid(row=0, column=6, **pad)

        ttk.Label(state_frame,
                  text="decade=1–6 (1=1Ω … 6=100kΩ), bit=Q0–Q5\n"
                       "No protection sequence – marks resistance UNKNOWN",
                  foreground="gray", justify="left").grid(row=1, column=0, columnspan=7, padx=6, pady=2)

        # Right: RELay:RAW
        raw_frame = ttk.LabelFrame(relay_ctrl_frame, text="Raw Relay Test (RELay:RAW)")
        raw_frame.grid(row=0, column=1, sticky="nsew", padx=(4, 0), pady=2)

        ttk.Label(raw_frame, text="xRy list:").grid(row=0, column=0, **pad)
        self.raw_var = tk.StringVar(value="103")
        raw_entry = ttk.Entry(raw_frame, textvariable=self.raw_var, width=20)
        raw_entry.grid(row=0, column=1, **pad)
        raw_entry.bind("<Return>", lambda _: self._relay_raw_set())
        ttk.Button(raw_frame, text="Set",   command=self._relay_raw_set).grid(row=0, column=2, **pad)
        ttk.Button(raw_frame, text="Query", command=self._relay_raw_query).grid(row=0, column=3, **pad)
        ttk.Label(raw_frame,
                  text="Format: hundreds=decade(1–6), units=relay(0–5), tens ignored\n"
                       "e.g. 103 = decade 1Ω Q3 | 103,206 = two relays simultaneously",
                  foreground="gray", justify="left").grid(row=1, column=0, columnspan=4, padx=6)

        # ── Row 4: Network Configuration ──
        net_frame = ttk.LabelFrame(self.root, text="Network Configuration  (NET:*)")
        net_frame.grid(row=4, column=0, columnspan=2, sticky="ew", **pad)

        ttk.Label(net_frame, text="Static IP:").grid(row=0, column=0, **pad)
        self.net_ip_var = tk.StringVar(value="192.168.1.6")
        ttk.Entry(net_frame, textvariable=self.net_ip_var, width=16).grid(row=0, column=1, **pad)
        ttk.Button(net_frame, text="Set", command=lambda: self._net_set_ip()).grid(row=0, column=2, **pad)

        ttk.Label(net_frame, text="Mask:").grid(row=0, column=3, **pad)
        self.net_mask_var = tk.StringVar(value="255.255.255.0")
        ttk.Entry(net_frame, textvariable=self.net_mask_var, width=16).grid(row=0, column=4, **pad)
        ttk.Button(net_frame, text="Set", command=lambda: self._net_set_mask()).grid(row=0, column=5, **pad)

        ttk.Label(net_frame, text="Gateway:").grid(row=0, column=6, **pad)
        self.net_gw_var = tk.StringVar(value="192.168.1.1")
        ttk.Entry(net_frame, textvariable=self.net_gw_var, width=16).grid(row=0, column=7, **pad)
        ttk.Button(net_frame, text="Set", command=lambda: self._net_set_gw()).grid(row=0, column=8, **pad)

        ttk.Label(net_frame, text="DHCP:").grid(row=1, column=0, **pad)
        self.net_dhcp_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(net_frame, text="Try DHCP first (fallback static)",
                        variable=self.net_dhcp_var,
                        command=self._net_set_dhcp).grid(row=1, column=1, columnspan=3, sticky="w", **pad)

        ttk.Button(net_frame, text="NET:APPLy  (save flash + restart)",
                   command=self._net_apply).grid(row=1, column=4, columnspan=2, **pad)

        ttk.Label(net_frame, text="Active IP:").grid(row=1, column=6, **pad)
        self.net_state_var = tk.StringVar(value="—")
        ttk.Label(net_frame, textvariable=self.net_state_var,
                  foreground="blue", width=16).grid(row=1, column=7, **pad)
        ttk.Button(net_frame, text="Read", command=self._net_read_state).grid(row=1, column=8, **pad)

        btn_row2 = ttk.Frame(net_frame)
        btn_row2.grid(row=2, column=0, columnspan=9, sticky="w", padx=4, pady=2)
        ttk.Button(btn_row2, text="Query IP",   command=lambda: self._send_and_show("NET:IPADdress?")).pack(side="left", padx=2)
        ttk.Button(btn_row2, text="Query Mask", command=lambda: self._send_and_show("NET:SMASk?")).pack(side="left", padx=2)
        ttk.Button(btn_row2, text="Query GW",   command=lambda: self._send_and_show("NET:GATEway?")).pack(side="left", padx=2)
        ttk.Button(btn_row2, text="Query DHCP", command=lambda: self._send_and_show("NET:DHCP?")).pack(side="left", padx=2)
        ttk.Label(btn_row2,
                  text="  NOTE: Set fields → NET:APPLy restarts Ethernet stack (reconnect needed)",
                  foreground="gray").pack(side="left", padx=6)

        # ── Row 5: Calibration ──
        cal_frame = ttk.LabelFrame(self.root, text="Calibration  (CALibration:*)")
        cal_frame.grid(row=5, column=0, columnspan=2, sticky="ew", **pad)

        # Row 0: point selector + set/query
        ttk.Label(cal_frame, text="Decade:").grid(row=0, column=0, **pad)
        self.cal_decade_var = tk.StringVar(value="1")
        ttk.Spinbox(cal_frame, from_=1, to=6, textvariable=self.cal_decade_var,
                    width=4, justify="center").grid(row=0, column=1, **pad)

        ttk.Label(cal_frame, text="Digit:").grid(row=0, column=2, **pad)
        self.cal_digit_var = tk.StringVar(value="1")
        ttk.Spinbox(cal_frame, from_=0, to=9, textvariable=self.cal_digit_var,
                    width=4, justify="center").grid(row=0, column=3, **pad)

        ttk.Label(cal_frame, text="Milliohm:").grid(row=0, column=4, **pad)
        self.cal_milli_var = tk.StringVar(value="1000")
        ttk.Entry(cal_frame, textvariable=self.cal_milli_var, width=10).grid(row=0, column=5, **pad)

        ttk.Button(cal_frame, text="Set Point",   command=self._cal_set).grid(row=0, column=6, **pad)
        ttk.Button(cal_frame, text="Query Point", command=self._cal_query).grid(row=0, column=7, **pad)

        # Row 1: global actions
        btn_cal = ttk.Frame(cal_frame)
        btn_cal.grid(row=1, column=0, columnspan=8, sticky="w", padx=4, pady=2)
        ttk.Button(btn_cal, text="Save to FRAM",      command=self._cal_save).pack(side="left", padx=2)
        ttk.Button(btn_cal, text="Load from FRAM",    command=self._cal_load).pack(side="left", padx=2)
        ttk.Button(btn_cal, text="Reset to nominal",  command=self._cal_reset).pack(side="left", padx=2)

        ttk.Label(btn_cal, text="  Enable correction:").pack(side="left", padx=(10, 2))
        self.cal_en_var = tk.StringVar(value="OFF")
        ttk.Combobox(btn_cal, textvariable=self.cal_en_var, values=["ON", "OFF"],
                     state="readonly", width=5).pack(side="left", padx=2)
        ttk.Button(btn_cal, text="Set",   command=self._cal_enable_set).pack(side="left", padx=2)
        ttk.Button(btn_cal, text="Query", command=self._cal_enable_query).pack(side="left", padx=2)

        # Row 2: query all → populate treeview
        ttk.Button(cal_frame, text="Query All → Table",
                   command=self._cal_query_all).grid(row=2, column=0, columnspan=2, sticky="w", padx=6, pady=2)
        ttk.Label(cal_frame,
                  text="Values in milliohms. Digit 0 is always 0 mΩ (short).",
                  foreground="gray").grid(row=2, column=2, columnspan=6, sticky="w", padx=4)

        # Calibration treeview
        cal_tv_frame = ttk.Frame(cal_frame)
        cal_tv_frame.grid(row=3, column=0, columnspan=8, sticky="ew", padx=4, pady=(0, 4))

        decade_names = ["1 (1Ω)", "2 (10Ω)", "3 (100Ω)", "4 (1kΩ)", "5 (10kΩ)", "6 (100kΩ)"]
        digit_cols   = [str(d) for d in range(10)]
        self.cal_tv = ttk.Treeview(cal_tv_frame,
                                   columns=digit_cols,
                                   show="headings",
                                   height=6)
        for col in digit_cols:
            self.cal_tv.heading(col, text=f"d={col}")
            self.cal_tv.column(col, width=80, anchor="center")
        for row_idx, name in enumerate(decade_names):
            nominal = [str(int(d) * (10 ** row_idx) * 1000) for d in range(10)]
            self.cal_tv.insert("", "end", iid=str(row_idx), values=nominal, tags=(name,))
        self.cal_tv.pack(side="left", fill="x", expand=True)

        cal_vsb = ttk.Scrollbar(cal_tv_frame, orient="vertical", command=self.cal_tv.yview)
        self.cal_tv.configure(yscrollcommand=cal_vsb.set)
        cal_vsb.pack(side="right", fill="y")

        # ── Row 6: Quick Commands ──
        qcmd_frame = ttk.LabelFrame(self.root, text="Quick Commands")
        qcmd_frame.grid(row=6, column=0, columnspan=2, sticky="ew", **pad)

        quick_cmds = [
            ("*IDN?",               "*IDN?"),
            ("*RST",                "*RST"),
            ("*TST?",               "*TST?"),
            ("*CLS",                "*CLS"),
            ("SYSTem:ID?",          "SYSTem:ID?"),
            ("SYSTem:ID? LONG",     "SYSTem:ID? LONG"),
            ("SYSTem:RST",          "SYSTem:RST"),
            ("SYSTem:BOOTloader",   "SYSTem:BOOTloader:ENter"),
            ("RES:VAL 0",           "RESistance:VALue 0"),
            ("RES:VAL 1000",        "RESistance:VALue 1000"),
            ("RES:VAL 10000",       "RESistance:VALue 10000"),
            ("RES:VAL?",            "RESistance:VALue?"),
            ("OUT ON",              "OUTPut:STATe ON"),
            ("OUT OFF",             "OUTPut:STATe OFF"),
            ("OUT?",                "OUTPut:STATe?"),
            ("REL_EN ON",           "OUTPut:RELay:ENable ON"),
            ("REL_EN OFF",          "OUTPut:RELay:ENable OFF"),
            ("REL_EN?",             "OUTPut:RELay:ENable?"),
            ("DECade? All",         None),   # special: calls _decade_query_all
            ("DEC 3→7",             "RESistance:DECade 3,7"),
            ("RELay:RAW?",          "RELay:RAW?"),
            ("RSTAT? 1,0",          "RELay:STATe? 1,0"),
            ("RSTAT 1,0,ON",        "RELay:STATe 1,0,ON"),
            ("RSTAT 1,0,OFF",       "RELay:STATe 1,0,OFF"),
            ("NET:STATe?",          "NET:STATe?"),
            ("CAL:ENable ON",       "CALibration:ENable ON"),
            ("CAL:ENable OFF",      "CALibration:ENable OFF"),
            ("CAL:ENable?",         "CALibration:ENable?"),
            ("CAL:SAVE",            "CALibration:SAVE"),
            ("CAL:LOAD",            "CALibration:LOAD"),
            ("CAL:RESet",           "CALibration:RESet"),
        ]
        cols = 4
        for idx, (label, cmd) in enumerate(quick_cmds):
            r, c = divmod(idx, cols)
            if cmd is None:
                ttk.Button(qcmd_frame, text=label, width=22,
                           command=self._decade_query_all).grid(row=r, column=c, **pad)
            else:
                ttk.Button(qcmd_frame, text=label, width=22,
                           command=lambda c=cmd: self._send(c)).grid(row=r, column=c, **pad)

        # ── Row 7: Manual Command ──
        man_frame = ttk.LabelFrame(self.root, text="Manual Command")
        man_frame.grid(row=7, column=0, columnspan=2, sticky="ew", **pad)

        self.cmd_var = tk.StringVar()
        cmd_entry = ttk.Entry(man_frame, textvariable=self.cmd_var, width=60)
        cmd_entry.grid(row=0, column=0, **pad)
        cmd_entry.bind("<Return>", lambda _: self._send(self.cmd_var.get()))
        ttk.Button(man_frame, text="Send →", command=lambda: self._send(self.cmd_var.get())).grid(
            row=0, column=1, **pad)

        # ── Row 8: Response Log ──
        log_frame = ttk.LabelFrame(self.root, text="Response Log")
        log_frame.grid(row=8, column=0, columnspan=2, sticky="nsew", **pad)
        self.root.rowconfigure(8, weight=1)
        self.root.columnconfigure(0, weight=1)

        self.log = scrolledtext.ScrolledText(log_frame, height=14, wrap=tk.WORD,
                                              state="disabled", font=("Courier New", 9))
        self.log.grid(row=0, column=0, sticky="nsew")
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        ttk.Button(log_frame, text="Clear Log",
                   command=self._clear_log).grid(row=1, column=0, sticky="e", padx=4, pady=2)

    # ─── VXI-11 autodiscovery ─────────────────────────────────────────────────

    def _scan_vxi11(self):
        if not VISA_AVAILABLE:
            self._log("ERROR: pyvisa not installed.")
            return
        self._log("Scanning local /24 subnet for VXI-11 instruments (port 111) …")
        self.btn_scan.config(state="disabled")
        threading.Thread(target=self._scan_thread, daemon=True).start()

    def _scan_thread(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
        except Exception:
            self.root.after(0, lambda: self._log("Scan ERROR: cannot determine local IP"))
            self.root.after(0, lambda: self.btn_scan.config(state="normal"))
            return

        net = ipaddress.ip_network(local_ip + "/24", strict=False)
        hosts = [str(h) for h in net.hosts()]
        self._log(f"Scan: checking {len(hosts)} hosts on {net} …")

        def check_port(ip):
            try:
                sock = socket.socket()
                sock.settimeout(0.15)
                sock.connect((ip, 111))
                sock.close()
                return ip
            except Exception:
                return None

        with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
            candidates = [ip for ip in ex.map(check_port, hosts) if ip]

        results = []
        for ip in candidates:
            addr = f"TCPIP::{ip}::INSTR"
            try:
                rm   = pyvisa.ResourceManager()
                inst = rm.open_resource(addr)
                inst.timeout = 1000
                idn  = inst.query("*IDN?").strip()
                inst.close()
                rm.close()
                results.append((addr, idn))
                self._log(f"  Found: {addr}  →  {idn}")
            except Exception:
                pass

        if not results:
            self._log("Scan: no VXI-11 instruments found")
        else:
            addrs = [r[0] for r in results]
            self.root.after(0, lambda a=addrs: self._set_scan_results(a))
        self.root.after(0, lambda: self.btn_scan.config(state="normal"))

    def _set_scan_results(self, addrs):
        self.addr_entry["values"] = addrs
        if addrs:
            self.addr_var.set(addrs[0])
            self._log(f"Scan complete – {len(addrs)} device(s) found. Select and Connect.")

    # ─── Interface helpers ────────────────────────────────────────────────────

    def _on_iface_change(self, _event=None):
        iface = self.iface_var.get()
        if iface == "USB-TMC":
            self.addr_var.set("USB::0xCAFE::0x4000::INSTR")
            self.btn_scan.config(state="disabled")
        else:
            self.addr_var.set("TCPIP::192.168.1.176::INSTR")
            self.btn_scan.config(state="normal")

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

    # ─── SCPI send helpers ────────────────────────────────────────────────────

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
            first_token = cmd.strip().split()[0] if cmd.strip() else ""
            if first_token.endswith("?"):
                resp = self.instrument.query(cmd).strip()
                self._log(f"       << {resp}")
                return resp
            else:
                self.instrument.write(cmd)
                self._log(f"       (sent)")
        except Exception as exc:
            self._log(f"       ERROR: {exc}")
        return None

    def _send_and_get(self, cmd: str):
        """Send a query synchronously (call from a worker thread), return response string."""
        if not self.instrument:
            return None
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self._log(f"[{ts}] >> {cmd}")
        try:
            resp = self.instrument.query(cmd).strip()
            self._log(f"       << {resp}")
            return resp
        except Exception as exc:
            self._log(f"       ERROR: {exc}")
            return None

    def _send_and_show(self, cmd: str):
        self._send(cmd)

    # ─── Resistance ──────────────────────────────────────────────────────────

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

    # ─── Decade control ───────────────────────────────────────────────────────

    def _decade_set_one(self, decade_num: int):
        """decade_num = 1..6  (1=1Ω, 6=100kΩ)"""
        idx = 6 - decade_num  # decade 6 → var index 0, decade 1 → var index 5
        val = self.decade_vars[idx].get()
        self._send(f"RESistance:DECade {decade_num},{val}")

    def _decade_set_all(self):
        for d in range(6, 0, -1):
            self._decade_set_one(d)

    def _decade_query_all(self):
        def _do():
            for d in range(6, 0, -1):
                idx = 6 - d
                resp = self._send_and_get(f"RESistance:DECade? {d}")
                if resp and resp.isdigit():
                    i = idx  # capture
                    self.root.after(0, lambda v=resp, i=i: self.decade_vars[i].set(v))
        threading.Thread(target=_do, daemon=True).start()

    # ─── REL_EN ──────────────────────────────────────────────────────────────

    def _relen_set(self):
        self._send(f"OUTPut:RELay:ENable {self.relen_var.get()}")

    def _relen_query(self):
        self._send("OUTPut:RELay:ENable?")

    # ─── Individual relay (RELay:STATe) ──────────────────────────────────────

    def _relay_state_set(self, on: bool):
        decade = self.rstate_decade_var.get()
        bit    = self.rstate_bit_var.get()
        state  = "ON" if on else "OFF"
        self._send(f"RELay:STATe {decade},{bit},{state}")

    def _relay_state_query(self):
        decade = self.rstate_decade_var.get()
        bit    = self.rstate_bit_var.get()
        self._send(f"RELay:STATe? {decade},{bit}")

    # ─── Raw relay ───────────────────────────────────────────────────────────

    def _relay_raw_set(self):
        raw = self.raw_var.get().strip()
        if not raw:
            self._log("Raw relay: enter value(s) like 103 or 103,206")
            return
        tokens = [t.strip() for t in raw.split(",") if t.strip()]
        for tok in tokens:
            try:
                v = int(tok)
                decade = v // 100
                relay  = v % 10
                if not (1 <= decade <= 6 and 0 <= relay <= 5):
                    self._log(f"Raw relay ERROR: {tok} out of range (decade 1-6, relay 0-5)")
                    return
            except ValueError:
                self._log(f"Raw relay ERROR: '{tok}' is not a number")
                return
        self._send(f"RELay:RAW {','.join(tokens)}")

    def _relay_raw_query(self):
        self._send("RELay:RAW?")

    # ─── Network config ───────────────────────────────────────────────────────

    def _net_set_ip(self):
        ip = self.net_ip_var.get().strip()
        self._send(f'NET:IPADdress "{ip}"')

    def _net_set_mask(self):
        mask = self.net_mask_var.get().strip()
        self._send(f'NET:SMASk "{mask}"')

    def _net_set_gw(self):
        gw = self.net_gw_var.get().strip()
        self._send(f'NET:GATEway "{gw}"')

    def _net_set_dhcp(self):
        val = "ON" if self.net_dhcp_var.get() else "OFF"
        self._send(f"NET:DHCP {val}")

    def _net_apply(self):
        if messagebox.askyesno("NET:APPLy",
                               "This restarts the Ethernet stack.\n"
                               "The VXI-11/USB connection may need to be re-established.\n\nProceed?"):
            self._send("NET:APPLy")
            self._log("NOTE: Reconnect after a few seconds if using VXI-11.")

    def _net_read_state(self):
        if not self.instrument:
            self._log("Not connected.")
            return
        def _do():
            resp = self._send_and_get("NET:STATe?")
            if resp:
                self.root.after(0, lambda: self.net_state_var.set(resp))
        threading.Thread(target=_do, daemon=True).start()

    # ─── Calibration ──────────────────────────────────────────────────────────

    def _cal_set(self):
        decade  = self.cal_decade_var.get().strip()
        digit   = self.cal_digit_var.get().strip()
        milliohm = self.cal_milli_var.get().strip()
        try:
            int(milliohm)
        except ValueError:
            self._log("Calibration ERROR: milliohm value must be an integer")
            return
        self._send(f"CALibration:DECade {decade},{digit},{milliohm}")

    def _cal_query(self):
        decade = self.cal_decade_var.get().strip()
        digit  = self.cal_digit_var.get().strip()
        def _do():
            resp = self._send_and_get(f"CALibration:DECade? {decade},{digit}")
            if resp is not None:
                self.root.after(0, lambda: self.cal_milli_var.set(resp))
        threading.Thread(target=_do, daemon=True).start()

    def _cal_save(self):
        self._send("CALibration:SAVE")

    def _cal_load(self):
        def _do():
            self._send_and_get("CALibration:LOAD")
            self._cal_query_all_impl()
        threading.Thread(target=_do, daemon=True).start()

    def _cal_reset(self):
        if messagebox.askyesno("Reset calibration",
                               "Reset in-RAM calibration to nominal values?\n"
                               "(Does NOT overwrite FRAM — use Save to persist.)"):
            self._send("CALibration:RESet")

    def _cal_enable_set(self):
        self._send(f"CALibration:ENable {self.cal_en_var.get()}")

    def _cal_enable_query(self):
        def _do():
            resp = self._send_and_get("CALibration:ENable?")
            if resp is not None:
                val = "ON" if resp.strip() in ("1", "ON") else "OFF"
                self.root.after(0, lambda: self.cal_en_var.set(val))
        threading.Thread(target=_do, daemon=True).start()

    def _cal_query_all(self):
        threading.Thread(target=self._cal_query_all_impl, daemon=True).start()

    def _cal_query_all_impl(self):
        """Query all 60 calibration points and update the treeview (call from worker thread)."""
        data = {}  # {(decade_idx, digit): milliohm_str}
        for dec in range(1, 7):
            for dig in range(10):
                resp = self._send_and_get(f"CALibration:DECade? {dec},{dig}")
                data[(dec - 1, dig)] = resp if resp else "?"

        def _update():
            for row_idx in range(6):
                values = [data.get((row_idx, d), "?") for d in range(10)]
                self.cal_tv.item(str(row_idx), values=values)
        self.root.after(0, _update)

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
