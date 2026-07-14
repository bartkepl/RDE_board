# API przekaźników i kalibracji

## Moduł `relay_ctrl` – sterowanie przekaźnikami

### Typy i stałe

```c
// relay_ctrl.h

// Sentinel zwracany przez relay_get_resistance() gdy stan nieznany
#define RELAY_RESISTANCE_UNKNOWN  (-1)

// Maksymalna rezystancja nominalna
#define RELAY_MAX_RESISTANCE  999999   // Ω
```

### Funkcje

---

#### `relay_init`

```c
void relay_init(void);
```

Inicjalizuje SPI1 i ustawia bezpieczny stan startowy:

- PA4 (REL_EN) = LOW → wyłączone wyjścia STPIC
- Wszystkie bajty `data[6] = 0x00`
- Q6 (CONNECT) = 0, Q7 (SHORT) = 1 → zaciski zwarte
- PA4 = HIGH → wyjścia aktywne

Wywoływana raz przy starcie, przed `SCPI_Main_Init()`.

---

#### `relay_set_resistance`

```c
int relay_set_resistance(int32_t ohms);
```

Ustawia rezystancję z pełną sekwencją ochronną.

**Parametry:**

| Parametr | Typ | Zakres | Opis |
|---------|:---:|:---:|------|
| `ohms` | `int32_t` | 0–999 999 | Żądana rezystancja w omach |

**Zwraca:** `0` sukces, `-1` błąd zakresu.

**Sekwencja wewnętrzna** (gdy wyjście podłączone):

```c
relay_set_bit_raw(6, SHORT_BIT, 1);   // SHORT ON
HAL_Delay(1);
// dekoduj ohms → cyfry 6 dekad
for (int d = 0; d < 6; d++)
    data[d] = digit_to_byte[digit[d]];
relay_latch();                         // prześlij przez SPI
HAL_Delay(1);
relay_set_bit_raw(6, CONNECT_BIT, 1); // CONNECT ON
relay_set_bit_raw(7, SHORT_BIT, 0);   // SHORT OFF
relay_latch();
```

Specjalny przypadek `ohms = 0`:

```c
relay_set_output(false);              // SHORT ON, CONNECT OFF
// Wszystkie dekady = cyfra 0 (bypass)
```

---

#### `relay_get_resistance`

```c
int32_t relay_get_resistance(void);
```

Zwraca aktualną nominalną rezystancję w omach, lub `RELAY_RESISTANCE_UNKNOWN` jeśli stan jest niezdefiniowany (np. po `RELay:STATe`).

---

#### `relay_set_output`

```c
void relay_set_output(bool connected);
```

Steruje przekaźnikami ochronnymi terminali:

| `connected` | Q6 (CONNECT) | Q7 (SHORT) |
|:---:|:---:|:---:|
| `true` | 1 | 0 |
| `false` | 0 | 1 |

Zapis natychmiastowy przez SPI bez zmiany bitów Q0–Q5.

---

#### `relay_get_output`

```c
bool relay_get_output(void);
```

Zwraca aktualny stan wyjścia (`true` = CONNECT, `false` = SHORT).

---

#### `relay_set_decade`

```c
int relay_set_decade(int decade, int digit);
```

Ustawia jedną dekadę z sekwencją ochronną. Pozostałe dekady nie są zmieniane.

**Parametry:**

| Parametr | Zakres | Opis |
|---------|:---:|------|
| `decade` | 1–6 | Numer dekady (1=1Ω, 6=100kΩ) |
| `digit` | 0–9 | Żądana cyfra |

**Zwraca:** `0` sukces, `-1` błąd zakresu.

---

#### `relay_get_decade`

```c
int relay_get_decade(int decade);
```

Zwraca aktualną cyfrę dekady (0–9) lub `-1` gdy stan nieznany.

---

#### `relay_set_bit`

```c
void relay_set_bit(int decade, int bit, bool on);
```

Bezpośrednie ustawienie jednego bitu Q bez sekwencji ochronnej.

!!! warning
    Po wywołaniu tej funkcji `relay_get_resistance()` zwraca `RELAY_RESISTANCE_UNKNOWN`. Stan resetuje się dopiero przez `relay_set_resistance()` lub `relay_set_decade()`.

**Parametry:**

| Parametr | Zakres | Opis |
|---------|:---:|------|
| `decade` | 1–6 | Numer dekady |
| `bit` | 0–5 | Numer bitu Q (Q0–Q5) |
| `on` | `bool` | Stan bitu |

---

#### `relay_get_bit`

```c
bool relay_get_bit(int decade, int bit);
```

Odczytuje stan bitu Q z wewnętrznego bufora (bez odczytu SPI).

---

#### `relay_set_raw`

```c
void relay_set_raw(const uint8_t data[6]);
```

Bezpośredni zapis 6 bajtów do shift-registrów przez SPI. Zachowuje aktualne bity Q6/Q7 (CONNECT/SHORT) – nadpisuje je wartościami z wewnętrznego stanu.

Używana przez komendę `RELay:RAW`.

---

#### `relay_get_raw`

```c
void relay_get_raw(uint8_t data[6]);
```

Kopiuje aktualny wewnętrzny bufor 6 bajtów do `data`.

---

### Przykłady użycia

```c
// Inicjalizacja
relay_init();

// Ustaw 4.7 kΩ, podłącz wyjście
relay_set_resistance(4700);
relay_set_output(true);

// Odczyt stanu
int32_t r = relay_get_resistance();   // 4700
int d4 = relay_get_decade(4);         // 4 (bo 4000 Ω w dekadzie 4)

// Zmień dekadę 2 na cyfrę 7 (70 Ω)
relay_set_decade(2, 7);
// r_total = 4700 - 0 + 70 = 4770 Ω (dekada 2 zmieniona z 0 na 7)

// Zwieranie wyjścia
relay_set_output(false);
```

---

## Moduł `relay_cal` – kalibracja

### Typy i stałe

```c
// relay_cal.h

#define CAL_MAGIC    0xCA11B001u   // Magic word w FRAM dla kalibracji

typedef struct {
    uint32_t milliohm[6][10];     // [dekada_idx 0..5][cyfra 0..9]
} cal_data_t;

typedef struct {
    uint32_t magic;               // CAL_MAGIC
    uint8_t  enabled;             // 0 = korekcja wyłączona, 1 = włączona
    uint8_t  _pad[3];
} cal_config_t;
```

Indeks dekady w tablicy: 0 = dekada 1 (1 Ω), 5 = dekada 6 (100 kΩ).

### Funkcje

---

#### `relay_cal_init`

```c
void relay_cal_init(void);
```

Wczytuje kalibrację z FRAM (PRIMARY → BACKUP → nominały). Wywoływana przy starcie po `fm24_ping()`.

---

#### `relay_cal_set`

```c
void relay_cal_set(int decade, int digit, uint32_t milliohm);
```

Zapisuje punkt kalibracyjny do RAM (nie do FRAM). Wymaga oddzielnego `relay_cal_save()`.

**Parametry:**

| Parametr | Zakres | Opis |
|---------|:---:|------|
| `decade` | 1–6 | Numer dekady |
| `digit` | 0–9 | Cyfra |
| `milliohm` | 0–900 000 000 | Zmierzona rezystancja w mΩ |

---

#### `relay_cal_get`

```c
uint32_t relay_cal_get(int decade, int digit);
```

Zwraca zapisany punkt kalibracyjny w mΩ.

---

#### `relay_cal_save`

```c
int relay_cal_save(void);
```

Zapisuje całą tablicę kalibracji do FRAM (PRIMARY + BACKUP z CRC32).

**Zwraca:** `0` sukces, `-1` błąd zapisu FRAM.

---

#### `relay_cal_load`

```c
int relay_cal_load(void);
```

Przeładowuje kalibrację z FRAM (jak przy starcie). Przydatne po ręcznej zmianie `CALibration:DECade` bez `CALibration:SAVE`.

**Zwraca:** `0` sukces, `-1` błąd odczytu lub CRC.

---

#### `relay_cal_reset`

```c
void relay_cal_reset(void);
```

Resetuje wszystkie punkty do wartości nominalnych:

```c
milliohm[d][digit] = (uint32_t)digit * multiplier[d] * 1000;
// multiplier[] = {1, 10, 100, 1000, 10000, 100000}
```

Nie zapisuje do FRAM – wymagane oddzielne `relay_cal_save()`.

---

#### `relay_cal_set_enabled`

```c
void relay_cal_set_enabled(bool enable);
```

Włącza lub wyłącza stosowanie korekcji kalibracyjnej.

---

#### `relay_cal_get_enabled`

```c
bool relay_cal_get_enabled(void);
```

Zwraca stan flagi korekcji.

---

#### `relay_cal_get_resistance_mo`

```c
uint64_t relay_cal_get_resistance_mo(void);
```

Sumuje miliohmowe wartości kalibracyjne dla aktualnie nastawionych cyfr każdej dekady.

```c
uint64_t total = 0;
for (int d = 0; d < 6; d++)
    total += cal_data.milliohm[d][current_digit[d]];
return total;
```

Wywoływana przez handler `RESistance:VALue?` gdy kalibracja jest włączona.

---

### Przykłady użycia

```c
// Inicjalizacja (przy starcie)
relay_cal_init();

// Załaduj nowy punkt kalibracyjny (dekada 1, cyfra 5, zmierzono 4983 mΩ)
relay_cal_set(1, 5, 4983);

// Zapisz do FRAM
relay_cal_save();

// Włącz korekcję
relay_cal_set_enabled(true);

// Odczyt skalibrowanej rezystancji
uint64_t mo = relay_cal_get_resistance_mo();
printf("%.3f Ω\n", mo / 1000.0);
```
