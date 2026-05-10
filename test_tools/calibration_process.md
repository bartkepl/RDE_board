# Proces kalibracji dekady rezystancyjnej RDE_board

## Cel

Kalibracja polega na zmierzeniu rzeczywistej rezystancji dla każdej cyfry (0–9) w każdej z 6 dekad
i wpisaniu tych wartości do pamięci FRAM. Po włączeniu korekcji komenda `RESistance:VALue?` zwraca
rzeczywistą wartość w ohmach (z 3 miejscami po przecinku) zamiast wartości nominalnej.

---

## Wymagany sprzęt

| Sprzęt | Minimalne wymagania |
|--------|---------------------|
| Multimetr wzorcowy | 4,5 cyfry (np. Keithley 2000, HP 34401A, Fluke 8846A) |
| Kable pomiarowe | 4-przewodowe Kelvin (pomiar 4-wire) – eliminuje rezystancję przewodów |
| Ewentualnie | 2-przewodowe + zerowanie (REL / NULL) na multimetrze |

> **Uwaga:** Dla dekady 1Ω (1–9 Ω) rezystancja przewodów (~0,1–0,5 Ω) jest istotna.
> Użyj pomiaru 4-wire lub wyzeruj multimetr przy zwartych zaciskach RDE.

---

## Przygotowanie

1. Podłącz multimetr do zacisków wyjściowych RDE (terminale OUTPUT).
2. Połącz się z urządzeniem (VXI-11 lub USB-TMC) przez `rde_tester.py` lub PyVISA.
3. Włącz zasilanie przekaźników:
   ```
   OUTPut:RELay:ENable ON
   OUTPut:STATe ON
   ```
4. Jeśli używasz pomiaru 2-wire: ustaw `RESistance:VALue 0`, zwróć kable na zaciski
   i wyzeruj (NULL/REL) multimetr.

---

## Procedura kalibracji (dekada po dekadzie)

Dla każdej dekady D (1=1Ω, 2=10Ω, 3=100Ω, 4=1kΩ, 5=10kΩ, 6=100kΩ):

### Krok 1: Zeruj pozostałe dekady

```
RESistance:VALue 0
```

Lub ręcznie przez GUI: ustaw wszystkie spinboxy na 0 → `Set All`.

### Krok 2: Kalibruj cyfry 1–9 tej dekady

Dla cyfry N = 1, 2, ..., 9:

```
RESistance:DECade D,N
```

Odczytaj wskazanie multimetru i wpisz wartość w **miliohmach**:

```
CALibration:DECade D,N,<wartość_mΩ>
```

**Przykład** — dekada 1 (1Ω), cyfra 5, zmierzono 4,983 Ω = 4983 mΩ:
```
RESistance:DECade 1,5
CALibration:DECade 1,5,4983
```

### Krok 3: Cyfra 0 (zawsze 0 mΩ)

Cyfra 0 oznacza zwarcie przez przekaźnik SW1 (0 Ω). Wartość nominalna = 0 mΩ.
Jeśli chcesz uwzględnić resztkową rezystancję stykową, możesz wpisać zmierzoną wartość.
W przeciwnym razie zostaw 0 mΩ (wartość domyślna).

---

## Sekwencja SCPI – pełna kalibracja dekady 1 (przykład)

```
RESistance:VALue 0
RESistance:DECade 1,1
CALibration:DECade 1,1,985
RESistance:DECade 1,2
CALibration:DECade 1,2,1978
RESistance:DECade 1,3
CALibration:DECade 1,3,2963
RESistance:DECade 1,4
CALibration:DECade 1,4,3952
RESistance:DECade 1,5
CALibration:DECade 1,5,4941
RESistance:DECade 1,6
CALibration:DECade 1,6,5928
RESistance:DECade 1,7
CALibration:DECade 1,7,6914
RESistance:DECade 1,8
CALibration:DECade 1,8,7902
RESistance:DECade 1,9
CALibration:DECade 1,9,8888
```

Powtórz dla dekad 2–6 (zmieniaj `D` i wpisuj wartości w mΩ).

---

## Zapis do FRAM

Po skalibraniu wszystkich dekad zapisz dane:

```
CALibration:SAVE
```

Dane są przechowywane z CRC-32 i kopią backup — bezpieczne przy utracie zasilania.

---

## Weryfikacja po kalibracji

1. Włącz korekcję:
   ```
   CALibration:ENable ON
   ```

2. Ustaw wartość i sprawdź odczyt:
   ```
   RESistance:VALue 5000
   RESistance:VALue?
   ```
   Odpowiedź będzie w formacie `XXXX.XXX` (ohmy z 3 miejscami po przecinku), np. `4998.450`.

3. Porównaj z multimetrem – powinna być zgodność w granicach dokładności kalibracji.

---

## Kalibracja przez GUI (rde_tester.py)

Sekcja **Calibration** w GUI zawiera:

- **Decade / Digit / Milliohm** – selektor punktu + przycisk `Set Point` i `Query Point`
- **Save to FRAM** – zapisuje wszystkie punkty do FRAM
- **Load from FRAM** – ładuje punkty z FRAM i odświeża tabelę
- **Reset to nominal** – przywraca wartości nominalne w RAM (bez zapisu do FRAM)
- **Enable correction ON/OFF** – włącza/wyłącza korekcję
- **Query All → Table** – odpytuje wszystkie 60 punktów i wyświetla w tabeli

### Zalecana kolejność w GUI

1. Ustaw dekadę i cyfrę spinboxami
2. Wyślij `RESistance:DECade D,N` przez sekcję Decade Control lub pole Manual Command
3. Odczytaj multimetr
4. Wpisz wartość milliohm w polu **Milliohm**
5. Kliknij **Set Point**
6. Powtórz dla wszystkich punktów
7. Kliknij **Save to FRAM**
8. Kliknij **Query All → Table** żeby sprawdzić wpisane wartości

---

## Notatki

- Kalibracja poprawia jedynie **odczyt** (`RESistance:VALue?`). Nastawiona wartość
  (relay_set_resistance) pozostaje nominalna – dekada zawsze ustawia dokładnie te
  przekaźniki które odpowiadają zadanej cyfrze.
- Suma kalibrowanych wartości = `cal[dec1][digit] + cal[dec2][digit] + ... + cal[dec6][digit]`.
  Zakłada niezależność dekad (typowe dla dekad rezystancyjnych klasy 0,1–1%).
- Zalecana rekalibracja po wymianie przekaźników lub elementów rezystancyjnych.
- Dane kalibracyjne przeżywają restart i wyłączenie zasilania (FRAM, >10^13 cykli zapisu).
