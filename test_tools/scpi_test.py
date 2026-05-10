"""
scpi_test.py – SCPI smoke-test for RDE_board

Usage:
    python scpi_test.py                  # default address TCPIP::192.168.1.176::INSTR
    python scpi_test.py 192.168.1.50     # explicit IP  → auto-builds TCPIP::ip::INSTR
    python scpi_test.py TCPIP::...::INSTR  # explicit VISA address
    python scpi_test.py discover         # scan local /24 subnet and connect to first found
"""

import sys
import socket
import ipaddress
import concurrent.futures
import pyvisa


def discover_vxi11(timeout: float = 0.15) -> str:
    """Scan the local /24 subnet for VXI-11 (port 111) and return the first VISA address."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    local_ip = s.getsockname()[0]
    s.close()
    net = ipaddress.ip_network(local_ip + "/24", strict=False)
    print(f"Scanning {net} for VXI-11 (port 111) …")

    def check(ip: str):
        try:
            sock = socket.socket()
            sock.settimeout(timeout)
            sock.connect((ip, 111))
            sock.close()
            return ip
        except Exception:
            return None

    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
        candidates = [ip for ip in ex.map(check, (str(h) for h in net.hosts())) if ip]

    rm = pyvisa.ResourceManager()
    for ip in candidates:
        addr = f"TCPIP::{ip}::INSTR"
        try:
            inst = rm.open_resource(addr)
            inst.timeout = 1000
            idn = inst.query("*IDN?").strip()
            inst.close()
            rm.close()
            print(f"  Found: {addr}  →  {idn}")
            return addr
        except Exception:
            pass
    rm.close()
    raise RuntimeError("No VXI-11 instruments found on the local subnet")


# ── Address resolution ────────────────────────────────────────────────────────

arg = sys.argv[1] if len(sys.argv) > 1 else ""

if arg == "discover":
    addr = discover_vxi11()
elif "::" in arg:
    addr = arg                              # already a full VISA address
elif arg:
    addr = f"TCPIP::{arg}::INSTR"          # bare IP given
else:
    addr = "TCPIP::192.168.1.176::INSTR"   # default

# ── Connect ───────────────────────────────────────────────────────────────────

rm   = pyvisa.ResourceManager()
inst = rm.open_resource(addr)
inst.timeout = 3000
print(f"\nConnected to: {addr}\n")


def q(cmd: str):
    resp = inst.query(cmd).strip()
    print(f"  {cmd:<36} -> {resp!r}")
    return resp


def w(cmd: str):
    inst.write(cmd)
    print(f"  {cmd:<36}    (sent)")


# ── Standard queries ──────────────────────────────────────────────────────────

print("--- IEEE / System ---")
q("*IDN?")
q("SYSTem:VERSion?")
q("SYSTem:ID?")
q("SYSTem:ID? SHORT")
q("SYSTem:ID? LONG")

print("\n--- Resistance / Output ---")
q("RESistance:VALue?")
q("OUTPut:STATe?")
q("OUTPut:RELay:ENable?")
q("NET:STATe?")

# ── RESistance:DECade ─────────────────────────────────────────────────────────

print("\n--- RESistance:DECade ---")
w("RESistance:VALue 0")
q("RESistance:VALue?")                     # → 0

w("RESistance:DECade 3,7")                 # 1 kΩ decade = 7  → +7000 Ω
q("RESistance:VALue?")                     # → 7000
q("RESistance:DECade? 3")                  # → 7

w("RESistance:DECade 1,5")                 # 1 Ω decade = 5   → +5 Ω
q("RESistance:VALue?")                     # → 7005
q("RESistance:DECade? 1")                  # → 5

# Query all decades
print("  [all decades]")
for d in range(1, 7):
    q(f"RESistance:DECade? {d}")

# ── RELay:STATe ───────────────────────────────────────────────────────────────

print("\n--- RELay:STATe ---")
w("RELay:STATe 1,0,ON")                    # Q0 in 1 Ω decade (SHORT = bypass)
q("RELay:STATe? 1,0")                      # → 1
q("RESistance:VALue?")                     # → 4294967295  (RELAY_RESISTANCE_UNKNOWN)

w("RELay:STATe 1,0,OFF")
q("RELay:STATe? 1,0")                      # → 0

w("RELay:STATe 2,2,ON")                    # Q2 in 10 Ω decade
q("RELay:STATe? 2,2")                      # → 1
w("RELay:STATe 2,2,OFF")

# ── Restore clean state ───────────────────────────────────────────────────────

print("\n--- Restore ---")
w("*RST")
q("RESistance:VALue?")                     # → 0

# ── Done ──────────────────────────────────────────────────────────────────────

inst.close()
rm.close()
print("\nDone.")
