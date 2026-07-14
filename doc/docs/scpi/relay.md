# RELay – surowy dostęp do przekaźników

!!! warning "Tryb serwisowy"
    Komendy `RELay` omijają sekwencję ochronną i mechanizm śledzenia rezystancji. Po ich użyciu `RESistance:VALue?` i `RESistance:DECade?` zwracają `UNKN`. Stan resetuje się dopiero przez `RESistance:VALue` lub `*RST`.

    Używaj wyłącznie do testowania sprzętu i serwisowania.

---

## `RELay:RAW`

Ustawia przekaźniki bezpośrednio przez format numeryczny kodujący dekadę i bit.

**Składnia:** `RELay:RAW <xRy>[,<xRy>,...]`

**Format parametru `xRy`:**

Trzy cyfry dziesiętne tworzą jeden adres bitu:

| Pozycja | Cyfra | Znaczenie |
|:---:|:---:|---------|
| Setki (`x`) | 1–6 | Numer dekady |
| Dziesiątki | dowolne | Ignorowane (separator) |
| Jedności (`y`) | 0–5 | Numer bitu Q |

Można podać wiele adresów oddzielonych przecinkami – wszystkie bity zostaną ustawione jednocześnie.

**Uwaga:** `RELay:RAW` **ustawia** tylko podane bity; pozostałe bity zachowują poprzedni stan. Bity Q6 i Q7 (CONNECT/SHORT) są zawsze zachowywane z aktualnego stanu wyjścia – nie można ich zmienić przez `RELay:RAW`.

**Przykłady:**

```
RELay:RAW 103        ← dekada 1 (setki=1), bit Q3 (jedności=3) = ON
RELay:RAW 206,405    ← dekada 2 bit Q6 (nieważne – ale Q6 jest zarezerwowane)
                        i dekada 4 bit Q5 = ON
RELay:RAW 100        ← dekada 1, bit Q0 (bypass) = ON
```

```python
# Ustawienie bypass (cyfra 0) dla dekady 1
inst.write('RELay:RAW 100')

# Ustawienie cyfry 5 dla dekady 3 (bity Q1 i Q4)
# Cyfra 5 = Q1=1, Q4=1 → adresy: 301 i 304
inst.write('RELay:RAW 301,304')

# Sprawdzenie stanu
raw = inst.query('RELay:RAW?')
print(raw)   # np. "1,0,18,0,0,0"  (6 bajtów dziesiętnie)
```

---

## `RELay:RAW?`

Odczytuje aktualny stan wszystkich 6 bajtów shift-registrów.

**Składnia:** `RELay:RAW?`

**Odpowiedź:** `<b0>,<b1>,<b2>,<b3>,<b4>,<b5>` – 6 liczb dziesiętnych

Kolejność bajtów:

| Pozycja | Bajt | Odpowiada |
|:---:|:---:|---------|
| `b0` | `data[0]` | Dekada 6 (100 kΩ) |
| `b1` | `data[1]` | Dekada 5 (10 kΩ) |
| `b2` | `data[2]` | Dekada 4 (1 kΩ) |
| `b3` | `data[3]` | Dekada 3 (100 Ω) |
| `b4` | `data[4]` | Dekada 2 (10 Ω) |
| `b5` | `data[5]` | Dekada 1 (1 Ω) + bity Q6/Q7 |

```python
raw = inst.query('RELay:RAW?')
bytes_val = [int(x) for x in raw.split(',')]

# Dekodowanie bajtu dekady 1 (b5)
b5 = bytes_val[5]
connect = (b5 >> 6) & 1
short   = (b5 >> 7) & 1
print(f'CONNECT={connect}, SHORT={short}')
print(f'Bity Q0-Q5 dekady 1: {b5 & 0x3F:06b}')
```

---

## `RELay:STATe`

Ustawia pojedynczy bit Q bez sekwencji ochronnej.

**Składnia:** `RELay:STATe <decade>,<bit>,<state>`

**Parametry:**

| Parametr | Typ | Zakres | Opis |
|---------|:---:|:---:|------|
| `<decade>` | Integer | 1–6 | Numer dekady |
| `<bit>` | Integer | 0–5 | Numer bitu Q |
| `<state>` | Bool | ON/OFF/1/0 | Żądany stan bitu |

Po tej komendzie wszystkie odczyty `RESistance:DECade?` dla tej dekady zwrócą `UNKN`.

```python
# Włącz bit Q3 w dekadzie 2 (bez sekwencji ochronnej!)
inst.write('RELay:STATe 2,3,ON')
inst.write('RELay:STATe 2,3,1')    # równoważne

# Wyłącz bit Q0 (bypass) w dekadzie 4
inst.write('RELay:STATe 4,0,OFF')
```

---

## `RELay:STATe?`

Odczytuje stan bitu Q.

**Składnia:** `RELay:STATe? <decade>,<bit>`

**Parametry:**

| Parametr | Zakres |
|---------|:---:|
| `<decade>` | 1–6 |
| `<bit>` | 0–5 |

**Odpowiedź:** `0` lub `1`

```python
# Sprawdzenie stanu bitu Q0 (bypass) dekady 1
bit = int(inst.query('RELay:STATe? 1,0'))
print('Bypass' if bit else 'Normalny')
```

---

## Typowy przypadek użycia: testowanie sprzętu

```python
# Sekwencja testu: czy każdy przekaźnik reaguje?
import time

inst.write('OUTPut:RELay:ENable ON')
inst.write('OUTPut:STATe OFF')   # SHORT dla bezpieczeństwa

for decade in range(1, 7):
    for bit in range(6):
        inst.write(f'RELay:STATe {decade},{bit},ON')
        time.sleep(0.02)   # 20 ms stabilizacja
        state = int(inst.query(f'RELay:STATe? {decade},{bit}'))
        print(f'd{decade} Q{bit}: {"OK" if state == 1 else "BŁĄD"}')
        inst.write(f'RELay:STATe {decade},{bit},OFF')

# Powrót do normalnego stanu
inst.write('*RST')
```

---

## Tabela podsumowująca

| Komenda | Parametry | Odpowiedź | Opis |
|---------|---------|---------|------|
| `RELay:RAW <xRy>[,...]` | Format `xRy` (dekada×100 + bit) | – | Ustaw bity przez adres |
| `RELay:RAW?` | – | `b0,b1,...,b5` | Odczyt 6 bajtów shift-registrów |
| `RELay:STATe <d>,<b>,<s>` | d=1–6, b=0–5, s=ON/OFF | – | Ustaw pojedynczy bit Q |
| `RELay:STATe? <d>,<b>` | d=1–6, b=0–5 | 0 lub 1 | Odczyt stanu bitu Q |
