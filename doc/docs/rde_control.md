# Aplikacja rde_control.py

Desktopowa aplikacja GUI w Pythonie do sterowania urządzeniem RDE, konfiguracji sieci i przeprowadzania kalibracji z obsługą popularnych multimetrów laboratoryjnych.

**Plik:** `test_tools/rde_control.py`  
**Wersja:** 1.0

---

## Instalacja

```bash
# Wymagane
pip install pyvisa pyvisa-py ttkthemes fpdf2

# Windows – USB-TMC
pip install pyusb
# + zainstaluj sterownik WinUSB przez Zadig dla VID=0xCAFE, PID=0x4000

# Uruchomienie
python test_tools/rde_control.py
```

| Biblioteka | Cel | Wymagana |
|-----------|-----|:---:|
| `pyvisa` | Interfejs VISA (USB-TMC, VXI-11) | Tak |
| `pyvisa-py` | Backend VISA bez NI-VISA | Tak |
| `ttkthemes` | Motywy Tkinter (wygląd) | Nie |
| `fpdf2` | Generowanie raportu PDF kalibracji | Nie |
| `pyusb` | USB-TMC na Windows | Tak (Windows) |

---

## Architektura kodu

```
rde_control.py
│
├── Stałe (COL_*, DECADE_LABELS, SCPI_ERRORS, NPLC_VALUES, ...)
│
├── Klasy transportu
│   ├── Transport (ABC)            # interfejs bazowy
│   └── VISATransport              # implementacja PyVISA (USB-TMC / VXI-11)
│
├── Klasy multimetrów
│   ├── Multimeter (ABC)           # interfejs bazowy
│   ├── HP34401A                   # HP/Agilent 34401A
│   ├── Agilent34970A              # 34970A z kartą MUX
│   └── Keithley2000               # Keithley 2000/2002
│
├── RDEDevice                      # wrapper komend SCPI dla RDE
│
├── Widżety GUI
│   ├── LogPanel                   # kolorowany panel logów
│   ├── DecadeFrame                # panel sterowania jedną dekadą
│   ├── CalTableFrame              # tabela kalibracyjna 6×10
│   └── NetworkFrame               # panel konfiguracji sieci
│
└── App (tk.Tk / ThemedTk)         # główne okno aplikacji
```

---

## Transport i połączenie

Aplikacja używa PyVISA jako warstwy abstrakcji:

```python
class VISATransport(Transport):
    def __init__(self, resource_name: str, timeout_ms: int = 3000):
        self.rm   = pyvisa.ResourceManager('@py')
        self.inst = self.rm.open_resource(resource_name)
        self.inst.timeout = timeout_ms

    def query(self, cmd: str) -> str:
        return self.inst.query(cmd).strip()

    def write(self, cmd: str) -> None:
        self.inst.write(cmd)
```

### Obsługiwane adresy VISA

| Typ | Format |
|-----|--------|
| USB-TMC | `USB0::0xCAFE::0x4000::<serial>::0::INSTR` |
| VXI-11/Ethernet | `TCPIP::<ip>::INSTR` |
| VXI-11/mDNS | `TCPIP::<name>.local::INSTR` |

---

## Klasa RDEDevice

Wrapper wysokopoziomowy – każda metoda odpowiada jednej lub kilku komendą SCPI:

```python
class RDEDevice:
    def identify(self) -> str:
        return self.transport.query('*IDN?')

    def reset(self) -> None:
        self.transport.write('*RST')

    def set_resistance(self, ohms: int) -> None:
        self.transport.write(f'RESistance:VALue {ohms}')

    def get_resistance(self) -> str:
        return self.transport.query('RESistance:VALue?')

    def set_decade(self, decade: int, digit: int) -> None:
        self.transport.write(f'RESistance:DECade {decade},{digit}')

    def set_output(self, connected: bool) -> None:
        self.transport.write(f'OUTPut:STATe {"ON" if connected else "OFF"}')

    def cal_set(self, decade: int, digit: int, milliohm: int) -> None:
        self.transport.write(f'CALibration:DECade {decade},{digit},{milliohm}')

    def cal_save(self) -> None:
        self.transport.write('CALibration:SAVE')

    def get_errors(self) -> list[tuple[int, str]]:
        errors = []
        while True:
            resp = self.transport.query('SYSTem:ERRor?')
            code, desc = resp.split(',', 1)
            if int(code) == 0:
                break
            errors.append((int(code), desc.strip().strip('"')))
        return errors
```

---

## Sterowniki multimetrów

### HP/Agilent 34401A

```python
class HP34401A(Multimeter):
    def measure_resistance_4w(self) -> float:
        """Pomiar 4-przewodowy (Kelvin)."""
        self.inst.write(f'SENSe:RESistance:NPLCycles {self.nplc}')
        return float(self.inst.query('MEASure:FRESistance?'))
```

Konfiguracja:
- NPLC: 0.02 / 0.2 / 1 / 2 / 10 / 100 (domyślnie 10)
- Rozdzielczość: 4.5 / 5.5 / 6.5 cyfry

### Agilent 34970A

```python
class Agilent34970A(Multimeter):
    def __init__(self, ...):
        # Auto-detekcja kart MUX przez SYSTem:CTYPE?
        cards = self.inst.query('SYSTem:CTYPE? 100').split(',')
        # 34901A / 34902A → OK
        # 34908A → ostrzeżenie (mniejsza dokładność)
        # inne → odrzucone
```

Obsługiwane karty:

| Karta | Obsługa | Opis |
|:---:|:---:|------|
| 34901A | ✓ | 20-kanałowy MUX, 4-przewodowy |
| 34902A | ✓ | 16-kanałowy MUX, 4-przewodowy |
| 34908A | ⚠ | 40-kanałowy, brak 4W – ostrzeżenie |
| Inne | ✗ | Nieobsługiwane |

### Keithley 2000/2002

```python
class Keithley2000(Multimeter):
    def measure_resistance_4w(self) -> float:
        self.inst.write(f':SENSe:FRESistance:NPLCycles {self.nplc}')
        return float(self.inst.query(':MEASure:FRESistance?'))
```

---

## Zakładki GUI

### Sterowanie

```
┌──────────────────────────────────────────┐
│  Rezystancja: [________4700__________]   │
│  [Ustaw]  [0 Ω]  [999999 Ω]  [OFF]      │
│                                          │
│  100 kΩ (d6): [▼][ 0 ][▲]  contrib: 0   │
│   10 kΩ (d5): [▼][ 0 ][▲]  contrib: 0   │
│    1 kΩ (d4): [▼][ 4 ][▲]  contrib: 4000│
│   100 Ω (d3): [▼][ 7 ][▲]  contrib: 700 │
│    10 Ω (d2): [▼][ 0 ][▲]  contrib: 0   │
│     1 Ω (d1): [▼][ 0 ][▲]  contrib: 0   │
│                                          │
│  Łącznie: 4700 Ω                         │
│  Wyjście: [ON]  [OFF]                    │
└──────────────────────────────────────────┘
```

Każda dekada ma przyciski `-`/`+` oraz pole spinbox (0–9). Łączna rezystancja aktualizuje się na żywo (bez wysyłania do urządzenia aż do naciśnięcia [Ustaw]).

### Kalibracja

```
┌────────────────────────────────────────────────┐
│  Multimetr: [HP 34401A ▼]  [Połącz]            │
│  NPLC: [10 ▼]  Próbki: [3]                     │
│                                                 │
│  ┌──────────────────────────────────────────┐   │
│  │      0      1      2  ...      9         │   │
│  │ 1Ω   0    983   1981  ...   8972         │   │
│  │10Ω   0   9990  19988  ...  89910         │   │
│  │ ...                                      │   │
│  └──────────────────────────────────────────┘   │
│                                                 │
│  [Odczyt z RDE]  [Start auto-kalibracji]        │
│  [Zapisz do FRAM]  [Reset nominały]  [PDF]      │
└────────────────────────────────────────────────┘
```

### Konfiguracja sieci

```
┌──────────────────────────────────┐
│  DHCP:  [☑] Włączony             │
│  IP:    [192.168.1.6           ] │
│  Maska: [255.255.255.0         ] │
│  Brama: [192.168.1.1           ] │
│  Aktywny adres: 192.168.1.67    │
│                                  │
│  [Odczyt z RDE]  [Zastosuj]      │
└──────────────────────────────────┘
```

[Zastosuj] wysyła `NET:APPLy` – urządzenie restartuje stos Ethernet.

### Terminal SCPI

Pole tekstowe do ręcznego wpisywania komend z historią i kolorowaniem:

```
[    Wyślij komendę SCPI:  _____________________  ] [Wyślij]

▶ *IDN?
◀ bartkepl,RDE,1.0,A3F7B201
▶ RESistance:VALue 1000
▶ RESistance:VALue?
◀ 1000
⚠ SYSTem:ERRor? → 0,"No error"
```

Kolory logów:

| Kolor | Znaczenie |
|:---:|---------|
| Szary | Komendy wysłane (`▶`) |
| Niebieski | Odpowiedzi RDE (`◀`) |
| Czerwony | Błędy SCPI |
| Zielony | Komunikaty systemowe (połączenie, zapis) |

---

## Automatyczna kalibracja – algorytm

```python
def run_auto_calibration(self):
    self.rde.write('CALibration:ENable OFF')
    self.rde.write('OUTPut:STATe ON')

    for decade in range(6, 0, -1):    # od 100kΩ do 1Ω
        for digit in range(10):
            self.rde.set_decade_isolated(decade, digit)
            time.sleep(0.2)            # stabilizacja przekaźnika

            readings = []
            for _ in range(self.num_samples):
                readings.append(self.dmm.measure_resistance_4w())
                time.sleep(0.05)

            avg_ohm = statistics.mean(readings)
            avg_mo  = round(avg_ohm * 1000)
            self.rde.cal_set(decade, digit, avg_mo)

            self.update_table(decade, digit, avg_mo)
            self.update_progress()

    self.rde.cal_save()
    self.rde.write('CALibration:ENable ON')
    self.generate_pdf_report()
```

---

## Raport PDF kalibracji

Generowany przez `fpdf2` w wątku tła (nie blokuje GUI). Uruchamiany przyciskiem **[PDF]** lub automatycznie po zakończeniu auto-kalibracji.

**Nazwa pliku:** `RDE_cal_YYYYMMDD_HHMM.pdf`  
Przykład: `RDE_cal_20260524_1445.pdf`

Raport składa się z czterech części:

**1. Nagłówek**

| Pole | Opis |
|------|------|
| Data i czas | Timestamp kalibracji |
| RDE IDN | Odpowiedź na `*IDN?` |
| Meter IDN | Identyfikacja multimetru |
| Tryb | `4W FRES` lub `2W RES`, NPLC, liczba uśrednień |

**2. Tabela kalibracyjna [mΩ]**

Siatka 6 dekad × 10 cyfr ze zmierzonymi wartościami w miliohmach. Komórki kolorowane wg odchyłki od wartości nominalnej:

| Kolor | Odchyłka |
|:-----:|----------|
| Biały | ≤ 1% |
| Żółty | 1–5% |
| Czerwony | > 5% |

**3. Tabela odchyłek od nominału**

- Kolumna `d=0`: bezwzględna wartość zwarcia w mΩ (brak wartości nominalnej)
- Kolumny `d=1..9`: odchyłka procentowa `|zmierzone - nominalne| / nominalne × 100%`
- Ta sama kolorystyka co tabela kalibracyjna

**4. Tabela podsumowująca**

Dla każdej dekady i wiersz zbiorczy `ALL DECADES`:

| Kolumna | Opis |
|---------|------|
| Zwarcie d=0 [mΩ] | Offset zwarciowy dekady |
| Średnia odchyłka [%] | Średnia z cyfr 1–9 |
| Maks. odchyłka [%] | Najgorsza cyfra w dekadzie |

---

## Skaner VXI-11

Menu **Narzędzia → Skanuj sieć** skanuje podsieć w poszukiwaniu urządzeń VXI-11:

```python
def scan_vxi11(subnet: str, timeout: float = 0.5):
    for ip in ipaddress.ip_network(subnet, strict=False).hosts():
        try:
            with socket.create_connection((str(ip), 703), timeout=timeout):
                # Spróbuj portmapper query
                idn = probe_vxi11_idn(str(ip))
                if 'RDE' in idn:
                    yield str(ip), idn
        except (socket.timeout, ConnectionRefusedError, OSError):
            pass
```

---

## Katalog błędów SCPI (`SCPI_ERRORS`)

Lokalny słownik opisów dla 30+ kodów błędów SCPI – używany do czytelniejszego logowania:

```python
SCPI_ERRORS = {
      0: "No error",
   -100: "Command error",
   -113: "Undefined header",
   -222: "Data out of range",
   -350: "Queue overflow",
   # ...
}
```

---

## Wymagania systemowe

| Element | Wymaganie |
|---------|---------|
| Python | 3.9+ |
| pyvisa + pyvisa-py | ≥ 1.13 + ≥ 0.7 |
| ttkthemes | ≥ 3.2 (opcjonalne) |
| fpdf2 | ≥ 2.7 (opcjonalne) |
| pyusb | ≥ 1.2 (Windows, USB-TMC) |
| OS | Windows 10/11, Linux, macOS |

---

## Znane ograniczenia

- Brak auto-reconnect po utracie połączenia (wymaga ręcznego ponownego połączenia)
- Eksport/import tabeli kalibracyjnej do CSV nie zaimplementowany
- Jednoczesna praca multimetru i RDE na tym samym interfejsie USB wymaga dwóch oddzielnych VISA resource managers
- Na Linuksie wymagana reguła udev: `SUBSYSTEM=="usb", ATTRS{idVendor}=="cafe", MODE="0666"`
