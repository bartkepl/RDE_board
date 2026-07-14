# Komendy SCPI – Przegląd

RDE implementuje protokół **SCPI-1999** (Standard Commands for Programmable Instruments) nad IEEE 488.2. Komendy dostępne są przez USB-TMC i VXI-11/Ethernet – te same komendy, ten sam wynik.

---

## Grupy komend

| Prefix | Plik | Opis |
|--------|------|------|
| `*` | [IEEE 488.2](ieee488.md) | Komendy obowiązkowe (IDN, RST, CLS, TST, ...) |
| `RESistance`, `OUTPut` | [RESistance & OUTPut](resistance.md) | Ustawianie rezystancji, sterowanie wyjściem |
| `RELay` | [RELay](relay.md) | Surowy dostęp do bitów shift-registrów |
| `CALibration` | [CALibration](calibration.md) | Kalibracja 60-punktowa |
| `NET` | [NET](network.md) | Konfiguracja sieci Ethernet |
| `SYSTem` | [SYSTem](system.md) | Błędy, wersja, numer seryjny, reset, bootloader |

---

## Składnia SCPI

### Forma długa i krótka

Duże litery w nazwie komendy wyznaczają minimalny skrót:

```
RESistance:VALue 1000   →  RES:VAL 1000   ✓
OUTPut:STATe ON         →  OUTP:STAT ON   ✓
SYSTem:ERRor:NEXT?      →  SYST:ERR:NEXT? ✓
```

### Zapytania (query)

Znak `?` na końcu tworzy zapytanie zamiast komendy:

```
RESistance:VALue 4700      ← ustawia
RESistance:VALue?          ← odpytuje → "4700"
```

### Łączenie komend

Średnik `;` łączy komendy w jednej linii (dzielą prefiks):

```
RESistance:VALue 1000; OUTPut:STATe ON
```

### Typy wartości

| Typ | Przykłady |
|-----|---------|
| Liczba całkowita | `0`, `4700`, `999999` |
| Liczba zmiennoprzecinkowa | `4.983`, `1.5e3` |
| Bool | `ON`, `OFF`, `1`, `0` |
| String | `"tekst"` |
| Wybór | `SHORT`, `LONG`, `HIGH`, `MEDIUM`, `LOW` |

---

## Kolejka błędów

RDE utrzymuje kolejkę błędów SCPI (max 16 pozycji, FIFO).

- Błąd pojawia się przy: nieprawidłowej komendzie, złych parametrach, braku FRAM
- **LED_R** świeci gdy kolejka niepusta
- Odczyt i usunięcie błędu: `SYSTem:ERRor[:NEXT]?`
- Wyczyszczenie całej kolejki: `*CLS`

Format odpowiedzi:
```
SYSTem:ERRor?  →  -222,"Data out of range"
SYSTem:ERRor?  →  0,"No error"
```

### Wzorzec obsługi błędów w Pythonie

```python
def check_errors(inst):
    while True:
        resp = inst.query('SYSTem:ERRor?')
        code, desc = resp.split(',', 1)
        if int(code) == 0:
            break
        print(f'RDE Error {code}: {desc.strip()}')

inst.write('RESistance:VALue 9999999')  # błąd: poza zakresem
check_errors(inst)
# RDE Error -222: "Data out of range"
```

---

## Tabela dostępności komend

| Komenda | Dostępna | Uwagi |
|---------|:---:|-------|
| Wszystkie `*` (IEEE 488.2) | ✓ | Zawsze dostępne |
| `RESistance:*` | ✓ | – |
| `OUTPut:*` | ✓ | – |
| `RELay:*` | ✓ | Tryb serwisowy |
| `CALibration:*` | ✓ | Wymaga FRAM dla `CAL:SAVE/LOAD` |
| `NET:*` | ✓ | `NET:APPLy` restartuje stos |
| `SYSTem:*` | ✓ | `SYSTem:BOOTloader:ENter` nieodwracalne |
