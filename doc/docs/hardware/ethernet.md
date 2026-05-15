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
| Moc | 3,3 V I/O, wbudowane terminatory magistrali |
| Opakowanie | QFN48 |

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

Adres MAC jest generowany z **UID MCU** (96-bitowy unikalny identyfikator ST):

```c
mac[0] = 0x02;           // bit U/L = 0 (locally administered), bit I/G = 0 (unicast)
mac[1] = uid[0];
mac[2] = uid[4];
mac[3] = uid[8];
mac[4] = uid[10];
mac[5] = uid[11];
```

Dzięki temu każde urządzenie ma unikalny adres MAC bez zewnętrznego układu pamięci.

!!! info
    Prefix `0x02` oznacza locally administered address (LAA) – nie jest przypisany przez IEEE, ale nie koliduje z globalnymi adresami OUI gdy bit U/L = 1.

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

!!! warning "Ograniczenie v0.3 PCB"
    W rewizji v0.3 zaobserwowano tłumienie sygnału RX linii Ethernet, które uniemożliwia prawidłową pracę przy **100 Mbps**. Firmware wymusza tryb **10 Mbps half-duplex** przez rejestr PHY Configuration Register:
    ```c
    // w5500_net_init() – rejestr PHYCFGR
    PHYCFGR = 0x18;  // 10M, half-duplex, forced (nie auto-neg)
    ```
    Naprawa zaplanowana w rewizji v0.4 PCB.

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
| Statyczne IP (fallback) | `192.168.1.50` |
| Maska podsieci | `255.255.255.0` |
| Brama domyślna | `192.168.1.1` |
| Link speed | 10 Mbps half-duplex (v0.3) |

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

Timeout DHCP: ok. **10 sekund** (konfigurowalne w `w5500_net.h`).

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
