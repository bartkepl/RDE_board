# Procedura kalibracji

Kalibracja polega na zmierzeniu rzeczywistej rezystancji każdego stopnia każdej dekady i zapisaniu tych wartości jako punktów korekcyjnych. Komendy SCPI służące do kalibracji opisane są w [CALibration](scpi/calibration.md).

---

## Zasada działania

- Kalibracja **nie zmienia** nastawianych wartości przekaźników
- Korekta jest stosowana tylko przy odpowiedzi na `RESistance:VALue?`
- Dane przechowywane w FRAM – trwałe przez wyłączenie zasilania
- 60 punktów: **6 dekad × 10 cyfr (0–9)**

Gdy kalibracja włączona (`CALibration:ENable ON`):

```
RESistance:VALue?  →  suma mΩ ze wszystkich aktywnych dekad / 1000
                       format: "XXXX.XXX" (3 miejsca po przecinku)
```

---

## Wymagany sprzęt

| Urządzenie | Wymaganie | Obsługa w rde_control.py |
|-----------|-----------|:---:|
| **HP/Agilent 34401A** | 4,5–6,5 cyfry, 4-przewodowe (Kelvin) | ✓ |
| **Agilent 34970A** z kartą 34901A/34902A | MUX, 4-przewodowe | ✓ |
| **Keithley 2000/2002** | 4,5–6,5 cyfry, 4-przewodowe | ✓ |
| Inny multimetr 4W | SCPI-kompatybilny | Ręczna obsługa |

Kable 4-przewodowe (Kelvin) do podłączenia z zaciskami wyjściowymi RDE są niezbędne dla dokładności poniżej 1 Ω.

---

## Procedura ręczna (przez SCPI)

### Krok 1: Przygotowanie

```python
inst.write('*RST')               # reset do stanu domyślnego
inst.write('*CLS')               # wyczyść kolejkę błędów
inst.write('CALibration:ENable OFF')  # wyłącz korekcję podczas kalibracji
inst.write('OUTPut:STATe ON')    # podłącz wyjście do sieci rezystorów
```

### Krok 2: Pomiar cyfry 0 każdej dekady (rezystancja zacisku)

Cyfra 0 to bypass – dekada zwarta. Zmierz oporność zacisku i przewodów:

```python
for decade in range(1, 7):
    # Ustaw tylko tę dekadę na 0, resztę też na 0
    inst.write('RESistance:VALue 0')
    # Zmierz multimetrem 4-przewodowo (np. 12 mΩ = 0.012 Ω)
    measured_mo = int(input(f'Dekada {decade}, cyfra 0 [mΩ]: '))
    inst.write(f'CALibration:DECade {decade},0,{measured_mo}')
```

### Krok 3: Pomiar cyfr 1–9

Dla każdej dekady, ustaw tylko tę dekadę na daną cyfrę (pozostałe = 0):

```python
multipliers = {1: 1, 2: 10, 3: 100, 4: 1000, 5: 10000, 6: 100000}

for decade in range(1, 7):
    for digit in range(1, 10):
        # Ustaw tylko jedną dekadę
        inst.write('RESistance:VALue 0')
        inst.write(f'RESistance:DECade {decade},{digit}')
        inst.write('OUTPut:STATe ON')

        nominal_mo = digit * multipliers[decade] * 1000
        print(f'Dekada {decade}, cyfra {digit}: nominal = {nominal_mo/1000:.3f} Ω')

        # Zmierz 4-przewodowo i wpisz wynik w mΩ
        measured_mo = int(input('  Zmierzone [mΩ]: '))
        inst.write(f'CALibration:DECade {decade},{digit},{measured_mo}')

        inst.write('OUTPut:STATe OFF')
```

!!! tip "Kolejność pomiarów"
    Zacznij od dekad 6→1 (od największych). Ciepło wydzielane w mniejszych rezystorach przy dużym prądzie testowym zaburza pomiar. Przy dekadach 100 kΩ – 10 kΩ prąd testowy z multimetru jest minimalny.

### Krok 4: Zapis i aktywacja

```python
inst.write('CALibration:SAVE')       # zapis do FRAM (PRIMARY + BACKUP + CRC32)
inst.write('CALibration:ENable ON')  # włącz korekcję

# Weryfikacja
inst.write('RESistance:VALue 4700')
r = inst.query('RESistance:VALue?')
print(f'Skalibrowane: {r} Ω')       # np. 4700.012
```

---

## Procedura automatyczna (rde_control.py)

Aplikacja `rde_control.py` realizuje pełną automatyczną kalibrację z obsługą multimetru:

1. Uruchom `python test_tools/rde_control.py`
2. Połącz się z RDE (zakładka *Połączenie*)
3. Połącz multimetr przez VISA (zakładka *Kalibracja*)
4. Podłącz kable 4-przewodowe z multimetru do zacisków RDE
5. Kliknij **Start kalibracji automatycznej**

Aplikacja:
- Iteruje przez 60 punktów (dekady 6→1, cyfry 0→9)
- Ustawia dekadę przez `RESistance:DECade`
- Odpytuje multimetr N razy i uśrednia (domyślnie 3 próbki)
- Zapisuje wynik przez `CALibration:DECade`
- Po zakończeniu wywołuje `CALibration:SAVE` i `CALibration:ENable ON`
- Generuje **raport PDF** z wynikami i odchyłkami w ppm

Czas kalibracji: ok. **15–30 minut** (zależy od NPLC multimetru i liczby uśrednień).

---

## Weryfikacja kalibracji

Po kalibracji porównaj wartości nominalne ze skalibrowanymi:

```python
print(f'{"Dekada":>8} {"Cyfra":>6} {"Nominal [Ω]":>14} {"Skalibrowane [Ω]":>18} {"Błąd [ppm]":>12}')

multipliers = {1: 1, 2: 10, 3: 100, 4: 1000, 5: 10000, 6: 100000}
for decade in range(1, 7):
    for digit in range(1, 10):
        inst.write(f'RESistance:DECade {decade},{digit}')
        nominal = digit * multipliers[decade]
        cal_mo  = int(inst.query(f'CALibration:DECade? {decade},{digit}'))
        cal_ohm = cal_mo / 1000.0
        ppm     = (cal_ohm - nominal) / nominal * 1e6
        print(f'{multipliers[decade]:>8} {digit:>6} {nominal:>14.3f} {cal_ohm:>18.3f} {ppm:>+12.0f}')
```

---

## Typowe wartości odchyłek

Dla rezystorów metalowych (MFR 0,1% tolerancja):

| Dekada | Typowy błąd |
|:---:|:---:|
| 1 Ω | ±5 000 ppm (5 mΩ absolutne) |
| 10 Ω | ±2 000 ppm |
| 100 Ω | ±1 000 ppm |
| 1 kΩ | ±500 ppm |
| 10 kΩ | ±300 ppm |
| 100 kΩ | ±200 ppm |

Kalibracja eliminuje błąd systematyczny rezystorów. Błąd powtarzalności (stabilność termiczna, kontakty przekaźników) pozostaje na poziomie ok. 50–200 ppm.

---

## Mapa danych w FRAM

| Adres FRAM | Zawartość |
|:---:|---------|
| `0x0040` | `cal_data_t` PRIMARY (240 B: `uint32_t milliohm[6][10]`) |
| `0x0130` | CRC32 PRIMARY (4 B) |
| `0x0138` | `cal_config_t` PRIMARY (8 B: magic + enabled) |
| `0x0148` | CRC32 cal_config PRIMARY (4 B) |
| `0x0150` | `cal_data_t` BACKUP (240 B) |
| `0x0248` | `cal_config_t` BACKUP (12 B) |

Szczegółowa mapa pamięci: [Pamięć – FRAM i Flash](hardware/memory.md).
