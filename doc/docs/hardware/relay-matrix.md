# Macierz przekaźników rezystancyjnych

## Sterownik STPIC6C595

Każda z 6 dekad jest sterowana przez jeden układ **STPIC6C595TTR** – 8-bitowy shift-register z wyjściami open-drain 50 V / 150 mA (zdolny do bezpośredniego sterowania cewkami przekaźników).

| Parametr | Wartość |
|---------|---------|
| Napięcie zasilania | 5 V |
| Prąd wyjścia (sink) | 150 mA per kanał, 500 mA suma |
| Napięcie wyjściowe (max) | 50 V |
| Interfejs wejściowy | SPI (MOSI/SCK) + latch (RCLK) |
| Czas propagacji | < 50 ns |
| Liczba układów | 6 (połączone szeregowo) |

### Podłączenie do MCU (SPI1)

```
MCU PA7 (MOSI) ─────────────────────────────────────── SER IN
MCU PA5 (SCK) ──────────────────────────────────────── SRCLK
MCU PA6 (REL_RCK) ──────────────────────────────────── RCLK (latch)
MCU PA4 (REL_EN) ──────── OE\ ─── wszystkie 6 układów  (active LOW!)
                  ┌──────────────────────────────┐
         QH' ─────┤STPIC1 (d1, 1Ω)  data[5]     ├── Q0..Q7
                  ├──────────────────────────────┤
         QH' ─────┤STPIC2 (d2, 10Ω) data[4]     ├── Q0..Q7
                  ├──────────────────────────────┤
         ...  ────┤STPIC3..6                     ├── Q0..Q7
                  └──────────────────────────────┘
```

!!! note "Polaryzacja OE"
    REL_EN (PA4) jest aktywny **HIGH** w firmware (`relay_init()` ustawia PA4=HIGH żeby włączyć wyjścia), ale pin OE układu STPIC6C595 jest aktywny LOW. Między PA4 a OE\ jest inwerter na płycie lub sygnał jest podłączony bezpośrednio (sprawdź schemat v0.3).

### Kolejność danych w łańcuchu

SPI wysyła **6 bajtów** – pierwszy bajt trafia do ostatniego układu w łańcuchu:

| Bajt `data[]` | Układ STPIC | Dekada | Jednostka |
|:---:|:---:|:---:|:---:|
| `data[0]` | STPIC6 | 6 | 100 kΩ |
| `data[1]` | STPIC5 | 5 | 10 kΩ |
| `data[2]` | STPIC4 | 4 | 1 kΩ |
| `data[3]` | STPIC3 | 3 | 100 Ω |
| `data[4]` | STPIC2 | 2 | 10 Ω |
| `data[5]` | STPIC1 | 1 | 1 Ω + Q6/Q7 |

Po wysłaniu 6 bajtów dany jest sygnał RCLK (PA6) – dane przechodzą z shift register do output register.

---

## Topologia sieci rezystorowej

Każda dekada składa się z **5 rezystorów** w topologii drabinkowej 1-2-2-2-2:

```
         Q0  Q1  Q2         Q3         Q4         Q5
(wejście) ●───┤  ├───R1───●───R2───●───R2───●───R2───●───R2───● (wyjście)
               │          │        │        │        │
              (0Ω)       tap1    tap2    tap3    tap4
```

| Przekaźnik | Typ | Rezystancja |
|:---:|:---:|:---:|
| Q0 (SW1) | Bypass | Zwarcie całej dekady → 0 × jednostka |
| Q1 (SW2) | Bypass R1 | Pomija R1 (umożliwia parzyste kroki) |
| Q2 (SW3) | Tap po R1 | Wybór wartości 1× |
| Q3 (SW4) | Tap po R2 | Wybór wartości 3× |
| Q4 (SW5) | Tap po R3 | Wybór wartości 5× |
| Q5 (SW6) | Tap po R4 | Wybór wartości 7× |

---

## Kodowanie cyfr 0–9

Poniższa tabela pokazuje, które bity Q są aktywne dla każdej cyfry każdej dekady. Kodowanie jest identyczne dla wszystkich 6 dekad (tylko wartości jednostkowe różnią się):

| Cyfra | Q0 | Q1 | Q2 | Q3 | Q4 | Q5 | Bajt HEX | Rezystancja |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| 0 | 1 | 0 | 0 | 0 | 0 | 0 | `0x01` | 0 × unit |
| 1 | 0 | 1 | 1 | 0 | 0 | 0 | `0x06` | 1 × unit |
| 2 | 0 | 0 | 1 | 0 | 0 | 0 | `0x04` | 2 × unit |
| 3 | 0 | 1 | 0 | 1 | 0 | 0 | `0x0A` | 3 × unit |
| 4 | 0 | 0 | 0 | 1 | 0 | 0 | `0x08` | 4 × unit |
| 5 | 0 | 1 | 0 | 0 | 1 | 0 | `0x12` | 5 × unit |
| 6 | 0 | 0 | 0 | 0 | 1 | 0 | `0x10` | 6 × unit |
| 7 | 0 | 1 | 0 | 0 | 0 | 1 | `0x22` | 7 × unit |
| 8 | 0 | 0 | 0 | 0 | 0 | 1 | `0x20` | 8 × unit |
| 9 | 0 | 0 | 1 | 1 | 0 | 1 | `0x24` | 9 × unit |

!!! note
    Bajt HEX dotyczy **tylko bitów Q0–Q5** (bity 0–5 bajtu). Bity 6 i 7 są używane tylko w bajcie `data[5]` (STPIC1) dla Q6 i Q7.

---

## Przekaźniki ochronne terminali wyjściowych

W bajcie `data[5]` (STPIC1, dekada 1 Ω) bity 6 i 7 sterują **przekaźnikami ochronnymi**:

| Bit | Nazwa | Stan = 1 | Stan = 0 |
|:---:|:---:|---------|---------|
| 6 | Q6 – CONNECT | Sieć rezystorów podłączona do zacisków | Zaciski odłączone |
| 7 | Q7 – SHORT | Zaciski wyjściowe zwarte do masy | Zaciski otwarte |

### Dozwolone stany Q6/Q7

| Stan wyjścia | Q6 | Q7 | Efekt |
|:---:|:---:|:---:|-------|
| **Zwarte** (`OUTPut:STATe OFF`) | 0 | 1 | Zaciski zwarte – bezpieczny stan przy braku pomiaru |
| **Podłączone** (`OUTPut:STATe ON`) | 1 | 0 | Dekady podłączone do zacisków – pomiar możliwy |
| ~~Q6=1, Q7=1~~ | ~~1~~ | ~~1~~ | **Zabroniony** – zwarcie dekad przez SHORT |
| Q6=0, Q7=0 | 0 | 0 | Wyjście otwarte (niezdefiniowane – unikać) |

---

## Sekwencja ochronna

Przy zmianie rezystancji firmware realizuje sekwencję chroniącą przed przepięciami indukcyjnymi:

### Ustawianie R > 0 przy wyjściu podłączonym (CONNECT ON)

```
1. SHORT ON  (Q7=1)          ← zwiera wyjście zanim cokolwiek zmieni się w dekadach
2. Czekaj > 1 ms             ← prąd przez cewki opada
3. Nastawiaj każdą dekadę    ← możliwa dowolna zmiana bitu
4. CONNECT ON, SHORT OFF      ← odblokowanie wyjścia
```

### Przejście do R = 0

```
1. CONNECT OFF               ← odłącza dekady od zacisków
2. SHORT ON                  ← zwiera zaciski
3. Nastawiaj dekadę 1 na cyfrę 0  ← opcjonalnie
```

### Zmiana pojedynczej dekady (`RESistance:DECade`)

```
1. SHORT ON                  ← bezpieczne zwieranie
2. Zmień cyfrę wybranej dekady
3. SHORT OFF (jeśli CONNECT był ON)
```

!!! warning "Pominięcie sekwencji"
    Komendy `RELay:RAW` i `RELay:STATe` **pomijają** sekwencję ochronną. Mogą powodować przepięcia indukcyjne jeśli zmiana dotyczy aktywnego przekaźnika przy podłączonym wyjściu. Używaj tylko w trybie serwisowym.

---

## Prędkość SPI i czas latch

- Prędkość SPI1: 10 MHz (wewnętrzny APB2 / prescaler)
- Czas transferu 6 bajtów: ~5 µs
- Opóźnienie po RCLK: ≥ 50 ns (już gwarantowane przez HAL)
- Czas ustabilizowania przekaźnika: 5–10 ms (typowo dla małych przekaźników sygnałowych)

Firmware czeka minimalnie **1 ms** po każdym przełączeniu gdy wyjście jest aktywne.
