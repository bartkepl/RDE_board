# IEEE 488.2 – Komendy obowiązkowe

Komendy zaczynające się od `*` są obowiązkowe w standardzie IEEE 488.2 i obecne w każdym urządzeniu SCPI.

---

## `*IDN?`

Identyfikacja urządzenia.

**Składnia:** `*IDN?`

**Odpowiedź:**
```
bartkepl,RDE,1.0,<serial>
```

Pola oddzielone przecinkami:

| Pole | Wartość | Opis |
|------|---------|------|
| Manufacturer | `bartkepl` | Identyfikator producenta |
| Model | `RDE` | Model urządzenia |
| Serial | `<8 znaków HEX>` | FNV-1a hash z UID MCU |
| Firmware | `1.0` | Wersja firmware |

**Przykład Python:**

```python
idn = inst.query('*IDN?')
manufacturer, model, firmware, serial = idn.split(',')
print(f'Połączono z: {model} ({serial})')
```

---

## `*RST`

Reset urządzenia do stanu domyślnego.

**Składnia:** `*RST`

**Efekt:**

- Rezystancja → 0 Ω (zwarcie wszystkich dekad)
- Wyjście → SHORT ON, CONNECT OFF (zaciski zwarte)
- Kolejka błędów → **nie** czyszczona (użyj `*CLS`)
- Konfiguracja sieci → **nie** zmieniana
- Kalibracja → **nie** zmieniana

!!! note
    `*RST` jest równoważny `SYSTem:RST` w zakresie stanu przekaźników. Nie powoduje twardego restartu MCU.

**Przykład Python:**

```python
inst.write('*RST')
print(inst.query('RESistance:VALue?'))   # 0
print(inst.query('OUTPut:STATe?'))       # 0
```

---

## `*CLS`

Czyszczenie rejestrów statusu i kolejki błędów.

**Składnia:** `*CLS`

**Efekt:**

- Kolejka błędów SCPI → wyczyszczona
- Event Status Register (ESR) → 0
- Status Byte (STB) → 0
- LED_R → gaśnie

```python
inst.write('*CLS')
```

---

## `*TST?`

Self-test urządzenia.

**Składnia:** `*TST?`

**Odpowiedź:**

| Wartość | Znaczenie |
|:---:|---------|
| `0` | Test przeszedł pomyślnie |
| `1` | Błąd: FRAM niedostępny (bit 0) |

Self-test sprawdza:
- Obecność FRAM FM24C64B na I2C (przez `fm24_ping()`)

!!! info
    Self-test nie testuje stanu przekaźników ani sieci Ethernet. Wartość `1` oznacza że kalibracja i konfiguracja sieci nie będą zapisywane trwale.

**Przykład Python:**

```python
result = inst.query('*TST?')
if int(result) != 0:
    print('Uwaga: FRAM niedostępny – kalibracja nie będzie zapisana')
```

---

## `*OPC` / `*OPC?`

Operation Complete – synchronizacja.

**Składnia:** `*OPC` / `*OPC?`

Ponieważ wszystkie operacje RDE są synchroniczne (przekaźniki przełączają się w callbacku komendy), `*OPC?` zawsze zwraca `1` natychmiast.

**Odpowiedź `*OPC?`:** `1`

```python
inst.write('RESistance:VALue 100000')
opc = inst.query('*OPC?')   # → "1" (operacja już zakończona)
```

---

## `*WAI`

Czekaj na zakończenie operacji.

**Składnia:** `*WAI`

No-op dla RDE – wszystkie operacje są synchroniczne.

---

## `*ESE <val>` / `*ESE?`

Event Status Enable – rejestr maski ESR.

**Składnia:**
- `*ESE <val>` – ustaw maskę (0–255)
- `*ESE?` – odczytaj maskę

**Bity ESR (IEEE 488.2):**

| Bit | Waga | Znaczenie |
|:---:|:---:|---------|
| 0 | 1 | Operation Complete (OPC) |
| 2 | 4 | Query Error |
| 3 | 8 | Device Dependent Error |
| 4 | 16 | Execution Error |
| 5 | 32 | Command Error |
| 6 | 64 | User Request |
| 7 | 128 | Power On |

```python
inst.write('*ESE 255')   # włącz wszystkie bity
print(inst.query('*ESE?'))  # 255
```

---

## `*ESR?`

Event Status Register – odczyt i wyczyszczenie.

**Składnia:** `*ESR?`

**Odpowiedź:** liczba 0–255 (bity jak w `*ESE`)

Odczyt ESR automatycznie go zeruje.

```python
esr = int(inst.query('*ESR?'))
if esr & 0x20:
    print('Błąd komendy w ESR')
```

---

## `*SRE <val>` / `*SRE?`

Service Request Enable – rejestr maski SRQ.

**Składnia:**
- `*SRE <val>` – ustaw maskę (0–255)
- `*SRE?` – odczytaj maskę

Bit 6 (64) zarezerwowany (zawsze 0 w SRE, nie można włączyć własnego bitu MSS).

---

## `*STB?`

Status Byte – odczyt rejestru statusu.

**Składnia:** `*STB?`

**Odpowiedź:** liczba 0–255

| Bit | Waga | Znaczenie |
|:---:|:---:|---------|
| 2 | 4 | Error/Event Queue not empty (MAV dla kolejki błędów) |
| 4 | 16 | Message Available (MAV) |
| 5 | 32 | Event Status Summary (ESB) |
| 6 | 64 | Request Service (MSS/RQS) |

---

## Tabela podsumowująca

| Komenda | Parametr | Odpowiedź | Opis |
|---------|---------|---------|------|
| `*IDN?` | – | `bartkepl,RDE,1.0,<serial>` | Identyfikacja |
| `*RST` | – | – | Reset do stanu domyślnego |
| `*CLS` | – | – | Czyszczenie statusu i błędów |
| `*TST?` | – | `0` lub `1` | Self-test |
| `*OPC` | – | – | Operation Complete (set bit) |
| `*OPC?` | – | `1` | Czy wszystkie operacje zakończone |
| `*WAI` | – | – | Czekaj (no-op) |
| `*ESE <n>` | 0–255 | – | Ustaw maskę ESR |
| `*ESE?` | – | 0–255 | Odczyt maski ESR |
| `*ESR?` | – | 0–255 | Odczyt i zerowanie ESR |
| `*SRE <n>` | 0–255 | – | Ustaw maskę SRQ |
| `*SRE?` | – | 0–255 | Odczyt maski SRQ |
| `*STB?` | – | 0–255 | Odczyt Status Byte |
