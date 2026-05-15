# RESistance & OUTPut

Komendy do ustawiania rezystancji i sterowania terminalami wyjściowymi.

---

## `RESistance:VALue`

Ustawia rezystancję wyjściową.

**Składnia:** `RESistance:VALue <ohms>`

**Parametry:**

| Parametr | Typ | Zakres | Opis |
|---------|:---:|:---:|------|
| `<ohms>` | Integer | 0–999 999 | Żądana rezystancja w omach |

Wartość `0` powoduje:

- SHORT ON, CONNECT OFF (zaciski zwarte)
- Wszystkie dekady ustawione na cyfrę 0

Wartość `> 0` powoduje sekwencję ochronną (SHORT → nastaw dekad → CONNECT):

```python
inst.write('RESistance:VALue 0')       # zwarcie wyjścia
inst.write('RESistance:VALue 4700')    # 4.7 kΩ
inst.write('RESistance:VALue 999999')  # 999.999 kΩ
```

!!! warning "Wartość poza zakresem"
    Wartości < 0 lub > 999 999 generują błąd SCPI `-222,"Data out of range"`.

---

## `RESistance:VALue?`

Odczytuje aktualną rezystancję.

**Składnia:** `RESistance:VALue?`

**Odpowiedź:**

| Tryb kalibracji | Format | Przykład |
|:---:|:---:|:---:|
| Wyłączona (domyślnie) | Integer | `4700` |
| Włączona (`CALibration:ENable ON`) | Float 3 miejsca | `4700.123` |
| Stan nieznany (po `RELay:STATe`) | String | `UNKN` |

```python
# Kalibracja wyłączona
r = inst.query('RESistance:VALue?')
print(int(r))          # 4700

# Kalibracja włączona
r = inst.query('RESistance:VALue?')
print(float(r))        # 4700.123

# Stan nieznany
r = inst.query('RESistance:VALue?')
if r.strip() == 'UNKN':
    print('Rezystancja nieznana – użyj RELay:STATe lub RST')
```

---

## `RESistance:DECade`

Ustawia pojedynczą dekadę.

**Składnia:** `RESistance:DECade <decade>,<digit>`

**Parametry:**

| Parametr | Typ | Zakres | Opis |
|---------|:---:|:---:|------|
| `<decade>` | Integer | 1–6 | Numer dekady (1 = 1 Ω, 6 = 100 kΩ) |
| `<digit>` | Integer | 0–9 | Cyfra |

Wkład dekady do łącznej rezystancji:

| Dekada | Jednostka | Cyfra 1 | Cyfra 5 | Cyfra 9 |
|:---:|:---:|:---:|:---:|:---:|
| 1 | 1 Ω | 1 Ω | 5 Ω | 9 Ω |
| 2 | 10 Ω | 10 Ω | 50 Ω | 90 Ω |
| 3 | 100 Ω | 100 Ω | 500 Ω | 900 Ω |
| 4 | 1 kΩ | 1 kΩ | 5 kΩ | 9 kΩ |
| 5 | 10 kΩ | 10 kΩ | 50 kΩ | 90 kΩ |
| 6 | 100 kΩ | 100 kΩ | 500 kΩ | 900 kΩ |

```python
# Ustaw 4 700 Ω = dekada 4 na 4, dekada 3 na 7, reszta 0
inst.write('RESistance:VALue 0')        # najpierw zerownie
inst.write('RESistance:DECade 4,4')     # +4000 Ω
inst.write('RESistance:DECade 3,7')     # +700 Ω
# Łącznie: 4700 Ω
```

---

## `RESistance:DECade?`

Odczytuje aktualną cyfrę dekady.

**Składnia:** `RESistance:DECade? <decade>`

**Parametry:**

| Parametr | Typ | Zakres |
|---------|:---:|:---:|
| `<decade>` | Integer | 1–6 |

**Odpowiedź:**

| Wartość | Znaczenie |
|:---:|---------|
| `0`–`9` | Aktualna cyfra dekady |
| `UNKN` | Stan nieznany (po komendzie `RELay:STATe`) |

```python
# Odczyt wszystkich cyfr
for d in range(1, 7):
    digit = inst.query(f'RESistance:DECade? {d}')
    print(f'Dekada {d}: {digit}')
```

---

## `OUTPut:STATe`

Steruje podłączeniem sieci rezystorów do zacisków wyjściowych.

**Składnia:** `OUTPut:STATe <state>`

**Parametry:**

| Parametr | Akceptowane wartości |
|---------|---------------------|
| `<state>` | `ON`, `1`, `OFF`, `0` |

| Stan | Q6 CONNECT | Q7 SHORT | Efekt |
|:---:|:---:|:---:|------|
| `ON` / `1` | 1 | 0 | Sieć rezystorów podłączona do zacisków |
| `OFF` / `0` | 0 | 1 | Zaciski zwarte do masy |

!!! tip "Dobra praktyka"
    Zawsze zwieraj zaciski (`OUTPut:STATe OFF`) przed i po pomiarze. Chroni to przed przepięciami indukcyjnymi podczas przełączania dekad.

```python
# Przełączanie z odpowiednim opóźnieniem
import time

inst.write('RESistance:VALue 10000')
inst.write('OUTPut:STATe ON')
time.sleep(0.05)                    # 50 ms stabilizacja przekaźników
# ... pomiar ...
inst.write('OUTPut:STATe OFF')
```

---

## `OUTPut:STATe?`

Odczytuje stan wyjścia.

**Składnia:** `OUTPut:STATe?`

**Odpowiedź:** `1` (CONNECT) lub `0` (SHORT)

```python
state = int(inst.query('OUTPut:STATe?'))
print('Wyjście: ' + ('podłączone' if state else 'zwarte'))
```

---

## `OUTPut:RELay:ENable`

Włącza lub wyłącza zasilanie przekaźników (pin REL_EN / PA4).

**Składnia:** `OUTPut:RELay:ENable <state>`

**Parametry:** `ON`, `1`, `OFF`, `0`

!!! warning
    Wyłączenie zasilania (`OFF`) powoduje otwarcie **wszystkich** przekaźników (cewki bez prądu). Zaciski wyjściowe stają się otwarte (rezystancja → ∞), niezależnie od nastawy CONNECT/SHORT. Stan shift-registrów jest zachowany w RAM i zostaje przywrócony po ponownym włączeniu.

```python
inst.write('OUTPut:RELay:ENable OFF')   # wyłącz wszystkie przekaźniki
# ... bezpieczna praca mechaniczna ...
inst.write('OUTPut:RELay:ENable ON')    # włącz z powrotem
```

---

## `OUTPut:RELay:ENable?`

Odczytuje stan zasilania przekaźników.

**Składnia:** `OUTPut:RELay:ENable?`

**Odpowiedź:** `1` (włączone) lub `0` (wyłączone)

---

## Tabela podsumowująca

| Komenda | Parametr | Odpowiedź | Opis |
|---------|---------|---------|------|
| `RESistance:VALue <ohms>` | 0–999 999 | – | Ustaw rezystancję z sekwencją ochronną |
| `RESistance:VALue?` | – | int lub float lub `UNKN` | Odczytaj aktualną rezystancję |
| `RESistance:DECade <d>,<n>` | d=1–6, n=0–9 | – | Ustaw cyfrę jednej dekady |
| `RESistance:DECade? <d>` | d=1–6 | 0–9 lub `UNKN` | Odczytaj cyfrę dekady |
| `OUTPut:STATe <s>` | ON/OFF/1/0 | – | Podłącz lub zwórz zaciski |
| `OUTPut:STATe?` | – | 0 lub 1 | Stan zacisków |
| `OUTPut:RELay:ENable <s>` | ON/OFF/1/0 | – | Zasilanie przekaźników |
| `OUTPut:RELay:ENable?` | – | 0 lub 1 | Stan zasilania przekaźników |
