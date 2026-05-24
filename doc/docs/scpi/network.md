# NET – konfiguracja sieci

Komendy do konfiguracji Ethernet. Zmiany NET:IPADdress/SMASk/GATEway/DHCP są tymczasowe (RAM) do czasu wywołania `NET:APPLy`, które zapisuje je do FRAM i restartuje stos W5500.

---

## `NET:IPADdress`

Ustawia statyczny adres IP.

**Składnia:** `NET:IPADdress <a.b.c.d>`

**Parametry:**

| Parametr | Format | Przykład |
|---------|:---:|:---:|
| `<a.b.c.d>` | IPv4 dziesiętny z kropkami | `192.168.10.25` |

!!! note
    Zmiana jest tymczasowa (tylko RAM). Wywołaj `NET:APPLy` aby zapisać i zastosować.

```python
inst.write('NET:IPADdress 192.168.10.25')
```

---

## `NET:IPADdress?`

Odczytuje skonfigurowany statyczny adres IP.

**Składnia:** `NET:IPADdress?`

**Odpowiedź:** `<a.b.c.d>` – aktualnie skonfigurowany statyczny IP (może być inny niż operacyjny gdy DHCP przydzieliło inny adres)

```python
print(inst.query('NET:IPADdress?'))   # 192.168.1.6
```

---

## `NET:SMASk`

Ustawia maskę podsieci.

**Składnia:** `NET:SMASk <a.b.c.d>`

```python
inst.write('NET:SMASk 255.255.255.0')
```

---

## `NET:SMASk?`

Odczytuje maskę podsieci.

**Składnia:** `NET:SMASk?`

**Odpowiedź:** `<a.b.c.d>`

---

## `NET:GATEway`

Ustawia adres bramy domyślnej.

**Składnia:** `NET:GATEway <a.b.c.d>`

```python
inst.write('NET:GATEway 192.168.10.1')
```

---

## `NET:GATEway?`

Odczytuje adres bramy domyślnej.

**Składnia:** `NET:GATEway?`

**Odpowiedź:** `<a.b.c.d>`

---

## `NET:DHCP`

Włącza lub wyłącza klienta DHCP.

**Składnia:** `NET:DHCP <state>`

**Parametry:** `ON`, `1`, `OFF`, `0`

| Stan | Działanie |
|:---:|---------|
| `ON` | Urządzenie próbuje uzyskać IP przez DHCP. Po timeout (~10 s) przełącza się na statyczne IP |
| `OFF` | Zawsze używa statycznego IP (NET:IPADdress) |

```python
inst.write('NET:DHCP OFF')           # wyłącz DHCP
inst.write('NET:IPADdress 10.0.0.5')
inst.write('NET:SMASk 255.0.0.0')
inst.write('NET:GATEway 10.0.0.1')
inst.write('NET:APPLy')              # zapisz i zastosuj
```

---

## `NET:DHCP?`

Odczytuje stan flagi DHCP.

**Składnia:** `NET:DHCP?`

**Odpowiedź:** `1` (DHCP włączone) lub `0` (DHCP wyłączone)

---

## `NET:PHY:MODE`

Ustawia tryb prędkości PHY. Zmiana jest tymczasowa (RAM) do czasu wywołania `NET:APPLy`.

**Składnia:** `NET:PHY:MODE <code>`

**Parametry (integer):**

| Kod | Tryb | Opis |
|:---:|:---:|---------|
| `0` | `AUTO` | Auto-negocjacja przez piny PMODE (wymaga dobrego sygnału 100M RX) |
| `1` | `10M`  | Wymuś 10 Mbps half-duplex (działa przy osłabionym sygnale RX)     |
| `2` | `100M` | Wymuś 100 Mbps full-duplex (wymaga dobrego sygnału RX)            |

!!! info "Dlaczego integer?"
    Parser libscpi traktuje `10M`/`100M` jako liczbę z sufiksem mega (M), a nie mnemonik. Integer 0/1/2 jest najbardziej niezawodną formą.

!!! note
    Zmiana jest tymczasowa (tylko RAM). Wywołaj `NET:APPLy` aby zapisać do FRAM i zastosować (wymaga restartu stosu W5500).

```python
inst.write('NET:PHY:MODE 0')   # AUTO (auto-negocjacja)
inst.write('NET:APPLy')

inst.write('NET:PHY:MODE 2')   # 100M FD
inst.write('NET:APPLy')
```

---

## `NET:PHY:MODE?`

Odczytuje skonfigurowany tryb PHY.

**Składnia:** `NET:PHY:MODE?`

**Odpowiedź (human-readable):** `AUTO`, `10M` lub `100M`

```python
print(inst.query('NET:PHY:MODE?'))   # 100M
```

---

## `NET:APPLy`

Zapisuje konfigurację sieci do FRAM i restartuje stos Ethernet.

**Składnia:** `NET:APPLy`

Sekwencja po wywołaniu:

1. Zapisuje `net_config_t` do FRAM (PRIMARY + BACKUP + CRC32)
2. Jeśli FRAM niedostępny: zapisuje do flash STM32G4 (strona 63)
3. Zamyka wszystkie otwarte gniazda W5500
4. Restartuje W5500 z nową konfiguracją
5. Jeśli DHCP=ON: uruchamia klienta DHCP

!!! warning "Chwilowa przerwa w komunikacji"
    Po `NET:APPLy` połączenie TCP/VXI-11 zostaje zerwane (restart stosu). Jeśli zmieniasz IP, musisz nawiązać nowe połączenie pod nowym adresem.

```python
# Kompletna zmiana adresacji
inst.write('NET:DHCP OFF')
inst.write('NET:IPADdress 192.168.20.100')
inst.write('NET:SMASk 255.255.255.0')
inst.write('NET:GATEway 192.168.20.1')
inst.write('NET:APPLy')

# Poczekaj na restart stosu
import time
time.sleep(3)

# Nawiąż połączenie pod nowym adresem
inst.close()
inst = rm.open_resource('TCPIP::192.168.20.100::INSTR')
print(inst.query('*IDN?'))
```

---

## `NET:STATe?`

Odczytuje aktualny operacyjny adres IP.

**Składnia:** `NET:STATe?`

**Odpowiedź:** `<a.b.c.d>` – aktualny adres IP (DHCP lub statyczny)

Różni się od `NET:IPADdress?` gdy DHCP przydzieliło inny adres niż skonfigurowany statyczny.

```python
static_ip = inst.query('NET:IPADdress?')   # 192.168.1.6 (skonfigurowany)
actual_ip  = inst.query('NET:STATe?')       # 192.168.1.100 (przydzielony przez DHCP)
```

---

## Przykład: odczyt pełnej konfiguracji sieci

```python
print(f"DHCP:     {inst.query('NET:DHCP?')}")
print(f"IP:       {inst.query('NET:IPADdress?')}")
print(f"Maska:    {inst.query('NET:SMASk?')}")
print(f"Brama:    {inst.query('NET:GATEway?')}")
print(f"PHY:      {inst.query('NET:PHY:MODE?')}")
print(f"Aktywne:  {inst.query('NET:STATe?')}")
```

Przykładowy wynik:
```
DHCP:     1
IP:       192.168.1.6
Maska:    255.255.255.0
Brama:    192.168.1.1
PHY:      AUTO
Aktywne:  192.168.1.100
```

---

## Wartości domyślne

| Parametr | Wartość domyślna |
|---------|:---:|
| DHCP | `ON` |
| Statyczne IP | `192.168.1.6` |
| Maska | `255.255.255.0` |
| Brama | `192.168.1.1` |
| PHY Speed | `AUTO` |

---

## Tabela podsumowująca

| Komenda | Parametry | Odpowiedź | Opis |
|---------|---------|---------|------|
| `NET:IPADdress <ip>` | IPv4 | – | Ustaw statyczny IP (RAM) |
| `NET:IPADdress?` | – | IPv4 | Odczytaj statyczny IP |
| `NET:SMASk <mask>` | IPv4 | – | Ustaw maskę podsieci (RAM) |
| `NET:SMASk?` | – | IPv4 | Odczytaj maskę |
| `NET:GATEway <gw>` | IPv4 | – | Ustaw bramę (RAM) |
| `NET:GATEway?` | – | IPv4 | Odczytaj bramę |
| `NET:DHCP <s>` | ON/OFF/1/0 | – | Włącz/wyłącz DHCP (RAM) |
| `NET:DHCP?` | – | 0 lub 1 | Stan DHCP |
| `NET:PHY:MODE <n>` | 0/1/2 | – | Ustaw tryb PHY (RAM): 0=AUTO, 1=10M, 2=100M |
| `NET:PHY:MODE?` | – | AUTO/10M/100M | Odczytaj tryb PHY |
| `NET:APPLy` | – | – | Zapisz do FRAM i restartuj stos W5500 |
| `NET:STATe?` | – | IPv4 | Aktualny operacyjny adres IP |
