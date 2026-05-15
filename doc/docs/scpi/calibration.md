# CALibration – komendy kalibracyjne

Komendy do zarządzania 60-punktową kalibracją rezystancji. Szczegółowa procedura kalibracji z multimetrem opisana jest na stronie [Procedura kalibracji](../calibration.md).

---

## `CALibration:DECade`

Zapisuje punkt kalibracyjny do RAM.

**Składnia:** `CALibration:DECade <decade>,<digit>,<milliohm>`

**Parametry:**

| Parametr | Typ | Zakres | Opis |
|---------|:---:|:---:|------|
| `<decade>` | Integer | 1–6 | Numer dekady (1=1Ω, 6=100kΩ) |
| `<digit>` | Integer | 0–9 | Cyfra |
| `<milliohm>` | Integer | 0–900 000 000 | Zmierzona rezystancja w miliohmach |

!!! note "Przelicznik"
    1 Ω = 1 000 mΩ, 1 kΩ = 1 000 000 mΩ.
    Przykład: zmierzono 4.983 Ω → `<milliohm>` = 4983.

**Przykłady:**

```python
# Dekada 1 (1Ω), cyfra 5: zmierzono 4.983 Ω = 4983 mΩ
inst.write('CALibration:DECade 1,5,4983')

# Dekada 4 (1kΩ), cyfra 3: zmierzono 2998.7 Ω = 2998700 mΩ
inst.write('CALibration:DECade 4,3,2998700')

# Cyfra 0 każdej dekady: oporność zacisku i przewodów (np. 12 mΩ)
inst.write('CALibration:DECade 1,0,12')
```

---

## `CALibration:DECade?`

Odczytuje zapisany punkt kalibracyjny.

**Składnia:** `CALibration:DECade? <decade>,<digit>`

**Odpowiedź:** Wartość w miliohmach (Integer)

```python
mo = int(inst.query('CALibration:DECade? 1,5'))
print(f'{mo / 1000:.3f} Ω')    # 4.983 Ω

# Odczyt całej tabeli kalibracyjnej
print(f'{"":8}', end='')
for digit in range(10):
    print(f'{digit:>10}', end='')
print()
for decade in range(1, 7):
    unit = 10**(decade-1)
    print(f'{unit:>6} Ω:', end='')
    for digit in range(10):
        mo = int(inst.query(f'CALibration:DECade? {decade},{digit}'))
        print(f'{mo:>10}', end='')
    print()
```

---

## `CALibration:SAVE`

Zapisuje wszystkie 60 punktów kalibracyjnych do FRAM.

**Składnia:** `CALibration:SAVE`

Operacja zapisuje PRIMARY i BACKUP z CRC32. Dane są trwałe po wyłączeniu zasilania.

!!! warning "Wymaga FRAM"
    Jeśli FRAM jest niedostępny (`*TST?` zwraca 1), komenda zakończy się błędem SCPI. Dane w RAM zostają zachowane do restartu urządzenia.

```python
# Po zmodyfikowaniu punktów kalibracyjnych:
inst.write('CALibration:SAVE')

# Sprawdzenie czy nie było błędu:
err = inst.query('SYSTem:ERRor?')
code = int(err.split(',')[0])
if code == 0:
    print('Kalibracja zapisana do FRAM')
else:
    print(f'Błąd zapisu: {err}')
```

---

## `CALibration:LOAD`

Przeładowuje kalibrację z FRAM do RAM.

**Składnia:** `CALibration:LOAD`

Przydatne po ręcznych zmianach `CALibration:DECade` które chcesz cofnąć (bez restartu urządzenia). Efekt jest taki sam jak restart, ale bez przerwy w pracy.

```python
inst.write('CALibration:DECade 1,5,9999')  # tymczasowa zmiana
# ... testowanie ...
inst.write('CALibration:LOAD')              # cofnij do zapisanej wersji
```

---

## `CALibration:RESet`

Resetuje wszystkie 60 punktów do wartości nominalnych.

**Składnia:** `CALibration:RESet`

Wartości nominalne: `digit × multiplier × 1000` mΩ, gdzie multiplier to 1, 10, 100, ... 100 000.

!!! note
    `CALibration:RESet` **nie zapisuje** do FRAM. Aby utrwalić nominały, wywołaj następnie `CALibration:SAVE`.

```python
# Reset i zapis nominałów
inst.write('CALibration:RESet')
inst.write('CALibration:SAVE')

# Weryfikacja – cyfra 7 dekady 3 (700 Ω nominalnie)
mo = int(inst.query('CALibration:DECade? 3,7'))
print(mo)   # 700000 (700.000 Ω)
```

---

## `CALibration:ENable`

Włącza lub wyłącza stosowanie korekcji kalibracyjnej.

**Składnia:** `CALibration:ENable <state>`

**Parametry:** `ON`, `1`, `OFF`, `0`

| Stan | Efekt na `RESistance:VALue?` |
|:---:|------|
| `OFF` (domyślnie) | Zwraca nominalną wartość całkowitą (np. `4700`) |
| `ON` | Zwraca sumę mΩ ze skalibrowanych punktów podzieloną przez 1000, 3 miejsca po przecinku (np. `4700.123`) |

```python
inst.write('CALibration:ENable ON')
r = inst.query('RESistance:VALue?')
print(float(r))    # 4700.123

inst.write('CALibration:ENable OFF')
r = inst.query('RESistance:VALue?')
print(int(r))      # 4700
```

!!! tip
    Flaga `CALibration:ENable` jest zapisywana do FRAM razem z danymi przez `CALibration:SAVE`. Po restarcie urządzenia stan jest odtwarzany.

---

## `CALibration:ENable?`

Odczytuje stan flagi korekcji.

**Składnia:** `CALibration:ENable?`

**Odpowiedź:** `1` (włączona) lub `0` (wyłączona)

---

## Tabela podsumowująca

| Komenda | Parametry | Odpowiedź | Opis |
|---------|---------|---------|------|
| `CALibration:DECade <d>,<n>,<mΩ>` | d=1–6, n=0–9, mΩ=0..9×10^8 | – | Zapisz punkt kalibracyjny do RAM |
| `CALibration:DECade? <d>,<n>` | d=1–6, n=0–9 | Integer [mΩ] | Odczytaj punkt kalibracyjny |
| `CALibration:SAVE` | – | – | Zapisz 60 punktów do FRAM (PRIMARY + BACKUP + CRC32) |
| `CALibration:LOAD` | – | – | Przeładuj kalibrację z FRAM do RAM |
| `CALibration:RESet` | – | – | Przywróć nominały (bez zapisu do FRAM) |
| `CALibration:ENable <s>` | ON/OFF/1/0 | – | Włącz/wyłącz korekcję kalibracyjną |
| `CALibration:ENable?` | – | 0 lub 1 | Stan korekcji |
