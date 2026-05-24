# Ethernet – W5500

## Układ WIZnet W5500

**W5500** to jednoukładowy sterownik Ethernet z wbudowanym, sprzętowym stosem TCP/IP. Podłączony przez SPI2 do STM32G431.

| Parametr | Wartość |
|---------|---------|
| Prędkość | 10/100 Mbps |
| Interfejs do MCU | SPI (tryb 0, do 80 MHz) |
| Gniazda sprzętowe | 8 niezależnych (TCP/UDP/MACRAW) |
| Protokoły wbudowane | TCP, UDP, ICMP, IPv4, ARP, IGMP, PPPoE |
| DHCP | Przez bibliotekę ioLibrary (klient) |
| Zasilanie | 3,3 V I/O, wbudowane terminatory magistrali |
| Obudowa | QFN48 |

### Podłączenie SPI2

| Pin MCU | Sygnał | Pin W5500 | Opis |
|:---:|:---:|:---:|------|
| PB12 | SPI2_CS | /SCS | Chip Select (active LOW) |
| PB13 | SPI2_SCK | SCLK | Zegar SPI |
| PB14 | SPI2_MISO | MISO | Dane z W5500 |
| PB15 | SPI2_MOSI | MOSI | Dane do W5500 |
| Pxx | W5500_RST | /RSTn | Reset (active LOW, min 2 µs) |
| Pxx | W5500_INT | INTn | Przerwanie (opcjonalne, polling w v0.3) |

Prędkość SPI2: **10 MHz** (przez prescaler APB1, konfiguracja w `MX_SPI2_Init()`).

---

## Adres MAC

Adres MAC jest generowany z prefiksu **WIZnet OUI** i 3 bajtów z unikalnego numeru seryjnego MCU (`serial_get_full()`):

```c
mac[0] = 0x00;   /* WIZnet OUI – globally administered unicast */
mac[1] = 0x08;
mac[2] = 0xDC;
/* Ostatnie 3 bajty z numeru seryjnego (hex ASCII → binary) */
mac[3] = (serial[0] << 4) | serial[1];
mac[4] = (serial[2] << 4) | serial[3];
mac[5] = (serial[4] << 4) | serial[5];
```

Dzięki temu każde urządzenie ma unikalny adres MAC oparty na zarejestrowanym OUI producenta układu Ethernet (WIZnet Co., Ltd).

!!! info
    Prefix `00:08:DC` to zarejestrowany OUI firmy WIZnet (globally administered). Trzy ostatnie bajty są wyznaczane ze skróconego numeru seryjnego MCU.

---

## Alokacja gniazd

W5500 ma 8 niezależnych gniazd sprzętowych. RDE używa:

| Gniazdo | Protokół | Port | Cel |
|:---:|:---:|:---:|------|
| 0 | UDP | 68 (DHCP) | Klient DHCP |
| 1 | TCP | 111 | Portmapper ONC-RPC |
| 2 | UDP | 111 | Portmapper ONC-RPC |
| 3 | TCP | 703 | VXI-11 Core serwer |
| 4 | UDP | 5353 | mDNS (multicast 224.0.0.251) |
| 5–7 | – | – | Wolne |

---

## PHY i interfejs fizyczny

W5500 ma wbudowany PHY 10/100BASE-T z auto-negocjacją. Podłączony bezpośrednio do złącza RJ-45 z transformatorem (magnetic module lub integrowany w gnieździe).

### Konfiguracja prędkości PHY przez SCPI

Tryb PHY jest konfigurowany w czasie rzeczywistym komendą `NET:PHY:MODE` (integer 0/1/2) i przechowywany w FRAM. Zmiana wchodzi w życie po `NET:APPLy` (restart stosu W5500).

| Kod | Tryb | Komenda SCPI | PHYCFGR (reset/run) | OPMDC | Uwagi |
|:---:|:---:|:---:|:---:|:---:|------|
| `0` | Auto-negocjacja      | `NET:PHY:MODE 0` | (domyślne z PMODE) | – | Wymaga dobrego sygnału 100M RX |
| `1` | 10 Mbps half-duplex  | `NET:PHY:MODE 1` | `0x40` / `0xC0`    | `000` | Działa przy osłabionym RX |
| `2` | 100 Mbps full-duplex | `NET:PHY:MODE 2` | `0x58` / `0xD8`    | `011` | Wymaga dobrego sygnału RX |

!!! danger "Uwaga – nie używaj 0x70/0xF0!"
    Te wartości kodują **OPMDC=110 (PHY Power Down)**, nie 100M FD. Poprawne dla 100BT FD bez auto-neg to **OPMDC=011 → 0x58/0xD8**.

```python
inst.write('NET:PHY:MODE 2')   # 100M FD
inst.write('NET:APPLy')
```

---

## Topologia sieci

```
                 STM32G431
                 ┌────────┐
     SPI2 ───── │ W5500  │ ── MAG ── RJ-45
                │        │
                └────────┘
                     │
               (DHCP klient)
                     │
              ┌──────┴──────┐
              │             │
           Router        Komputer
           (DHCP serwer) (PyVISA)
```

---

## Konfiguracja sieci – wartości domyślne

| Parametr | Wartość domyślna |
|---------|:---:|
| DHCP | Włączony |
| Statyczne IP (fallback) | `192.168.1.6` |
| Maska podsieci | `255.255.255.0` |
| Brama domyślna | `192.168.1.1` |
| Link speed | Konfigurowalny przez SCPI `NET:PHY:MODE` (domyślnie: AUTO) |

Zmiana przez SCPI: patrz [NET – konfiguracja sieci](../scpi/network.md).

---

## DHCP – mechanizm

```
w5500_net_init()
   │
   ├── DHCP enabled? ──YES──► DHCP_init() → DHCP_run() (nieblokujące)
   │                              │
   │                              ├── Odpowiedź w < 10s → używaj adresu DHCP
   │                              └── Timeout → fallback na statyczne IP
   │
   └── DHCP disabled? ──YES──► Użyj natychmiast statycznego IP
```

Timeout DHCP: **12 sekund** (`DHCP_TIMEOUT_MS` w `w5500_net.c`).

Po przyznaniu adresu przez DHCP, aktualny adres można odczytać komendą `NET:STATe?`.

---

## mDNS

Urządzenie rozgłasza obecność przez **Multicast DNS** (RFC 6762) na gnieździe 4 (UDP 5353):

- **Hostname**: `RDE-<serial_short>.local` (np. `RDE-A3F7B201.local`)
- **Rekord A**: wskazuje na aktualny adres IP urządzenia
- **Rekord SRV**: usługa `_vxi-11._tcp`, port 703
- **Rekord TXT**: `model=RDE`

Dzięki mDNS można połączyć się z urządzeniem bez znajomości adresu IP:
```
TCPIP::RDE-A3F7B201.local::INSTR
```

---

## VXI-11 – protokół sieciowy

VXI-11 Core działa przez ONC-RPC (Remote Procedure Call) na TCP port **703**. Portmapper (port 111) informuje klienta o tym porcie.

Szczegółowy opis implementacji VXI-11: [API komunikacji](../firmware/comms-api.md).

Konfiguracja sieci przez SCPI: [NET – konfiguracja sieci](../scpi/network.md).
