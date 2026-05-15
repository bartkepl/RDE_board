# SYSTem – system i diagnostyka

---

## `SYSTem:ERRor[:NEXT]?`

Pobiera i usuwa pierwszy błąd z kolejki SCPI.

**Składnia:** `SYSTem:ERRor?` lub `SYSTem:ERRor:NEXT?`

**Odpowiedź:** `<code>,"<description>"`

| Przykład odpowiedzi | Znaczenie |
|---------------------|---------|
| `0,"No error"` | Kolejka pusta |
| `-222,"Data out of range"` | Parametr poza zakresem |
| `-113,"Undefined header"` | Nieznana komenda |

!!! tip "Wzorzec obsługi błędów"
    Zawsze opróżniaj kolejkę po sekwencji komend:
    ```python
    def drain_errors(inst):
        errors = []
        while True:
            resp = inst.query('SYSTem:ERRor?')
            code, desc = resp.split(',', 1)
            if int(code) == 0:
                break
            errors.append((int(code), desc.strip().strip('"')))
        return errors
    ```

---

## `SYSTem:ERRor:COUNt?`

Zwraca liczbę błędów w kolejce bez ich usuwania.

**Składnia:** `SYSTem:ERRor:COUNt?`

**Odpowiedź:** Integer (0–16)

```python
count = int(inst.query('SYSTem:ERRor:COUNt?'))
if count > 0:
    print(f'{count} błędów w kolejce')
```

---

## `SYSTem:VERSion?`

Wersja standardu SCPI zaimplementowanego w urządzeniu.

**Składnia:** `SYSTem:VERSion?`

**Odpowiedź:** `1999.0`

---

## `SYSTem:ID?`

Numer seryjny urządzenia z UID mikrokontrolera STM32G431.

**Składnia:** `SYSTem:ID? [SHORT|LONG]`

**Parametry (opcjonalne):**

| Wartość | Opis |
|:---:|---------|
| `SHORT` (domyślne) | 8 znaków HEX – FNV-1a hash 96-bitowego UID MCU |
| `LONG` | 24 znaki HEX – surowe 96 bitów UID MCU |

**Przykłady odpowiedzi:**

```
SYSTem:ID?        → A3F7B201
SYSTem:ID? SHORT  → A3F7B201
SYSTem:ID? LONG   → 0034002B3438510A00360035
```

!!! info "Skąd pochodzi numer seryjny"
    UID STM32G4 to trzy słowa 32-bitowe zapisane w flash at adresie `0x1FFF7590`. Hash FNV-1a (32-bit) zapewnia równomierne rozkładanie wartości – nie ma ryzyka kolizji przy małych wolumenach urządzeń.

```python
serial_short = inst.query('SYSTem:ID?')
serial_long  = inst.query('SYSTem:ID? LONG')
print(f'S/N: {serial_short}')
print(f'UID: {serial_long}')
```

---

## `SYSTem:BOOTloader:ENter`

Wchodzi w tryb bootloadera DFU (Device Firmware Update) przez USB.

**Składnia:** `SYSTem:BOOTloader:ENter`

Sekwencja:

1. Urządzenie ustawia magiczny znacznik w RAM (`0xDEADBEEF` na początku SRAM)
2. Wywołuje `NVIC_SystemReset()`
3. Startup code wykrywa znacznik i przeskakuje do systemowego bootloadera ST (`0x1FFF0000`)
4. Urządzenie pojawia się jako `STM32 BOOTLOADER` w menedżerze urządzeń

!!! warning "Nieodwracalne (do restartu)"
    Po wejściu w bootloader nie można wysyłać komend SCPI. Wymagane fizyczne odłączenie i ponowne podłączenie USB (lub power cycle) po zakończeniu aktualizacji.

**Procedura aktualizacji firmware:**

```bash
# Wejście w bootloader przez SCPI (jeśli połączony)
python -c "import pyvisa; rm=pyvisa.ResourceManager('@py'); \
           inst=rm.open_resource('USB0::0xCAFE::0x4000::...::INSTR'); \
           inst.write('SYSTem:BOOTloader:ENter')"

# Wgranie firmware przez dfu-util (Linux/macOS)
dfu-util -a 0 -s 0x08000000:leave -D RDE_soft.bin

# Lub przez DfuSe (Windows) – GUI aplikacja STMicroelectronics
```

---

## `SYSTem:RST`

Miękki reset mikrokontrolera.

**Składnia:** `SYSTem:RST`

Wywołuje `NVIC_SystemReset()` – pełny restart CPU, peryferiów i stosu aplikacji. Urządzenie wraca do normalnej pracy po ~1 sekundzie.

Różnica względem `*RST`:

| Komenda | Efekt |
|---------|-------|
| `*RST` | Reset stanu przekaźników do domyślnego (bez restartu CPU) |
| `SYSTem:RST` | Twardy restart CPU – jak power cycle |

!!! note
    `SYSTem:RST` przeładowuje konfigurację z FRAM – przydatne po ręcznej modyfikacji FRAM.

---

## Kody błędów SCPI

| Kod | Kategoria | Opis |
|:---:|:---:|------|
| 0 | – | Brak błędu |
| **Błędy komend** | | |
| -100 | Command | Ogólny błąd komendy |
| -101 | Command | Nieprawidłowy znak |
| -102 | Command | Błąd składni |
| -103 | Command | Nieprawidłowy separator |
| -104 | Command | Typ danych niezgodny |
| -108 | Command | Błąd delimitera parametru |
| -109 | Command | Brakujący parametr |
| -113 | Command | Nieznana komenda (Undefined header) |
| **Błędy wykonania** | | |
| -200 | Execution | Ogólny błąd wykonania |
| -220 | Execution | Błąd parametru |
| -221 | Execution | Ustawienie sprzeczne |
| -222 | Execution | Wartość poza zakresem |
| -223 | Execution | Zbyt dużo danych |
| -224 | Execution | Niedozwolona wartość parametru |
| **Błędy zapytań** | | |
| -400 | Query | Ogólny błąd zapytania |
| -410 | Query | Przerwa zapytania (INTERRUPTED) |
| -420 | Query | Brak danych (UNTERMINATED) |
| **Błędy systemowe** | | |
| -300 | System | Błąd systemowy (np. brak FRAM) |
| -350 | System | Przepełnienie kolejki błędów |

---

## Tabela podsumowująca

| Komenda | Parametry | Odpowiedź | Opis |
|---------|---------|---------|------|
| `SYSTem:ERRor[:NEXT]?` | – | `<code>,"<desc>"` | Pobierz i usuń błąd z kolejki |
| `SYSTem:ERRor:COUNt?` | – | Integer | Liczba błędów w kolejce |
| `SYSTem:VERSion?` | – | `1999.0` | Wersja standardu SCPI |
| `SYSTem:ID? [SHORT\|LONG]` | SHORT/LONG | HEX string | Numer seryjny z UID MCU |
| `SYSTem:BOOTloader:ENter` | – | – | Wejdź w DFU bootloader |
| `SYSTem:RST` | – | – | Miękki reset CPU |
