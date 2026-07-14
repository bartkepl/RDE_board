# Sprzęt – Przegląd

## Mikrokontroler STM32G431CBT6

| Parametr | Wartość |
|---------|---------|
| Rdzeń | ARM Cortex-M33, 170 MHz |
| Flash | 128 KB |
| SRAM | 32 KB |
| Opakowanie | LQFP48 |
| USB | USB 2.0 Full Speed z CRS (bez zewnętrznego kwarcu) |
| Interfejsy | 2× SPI, 1× I2C, 2× UART, USB FS |
| ADC | 12-bit, 5 Msps |
| DAC | 12-bit |

### Przypisanie peryferiów

| Peryferium | Sygnały | Przeznaczenie |
|-----------|---------|---------------|
| SPI1 | PA5/PA6/PA7, PA4 (REL_EN), PA6 (REL_RCK) | Sterowniki STPIC6C595 (przekaźniki) |
| SPI2 | PB13/PB14/PB15, PB12 (CS) | W5500 Ethernet |
| I2C1 | PB6/PB7 | FRAM FM24C64B |
| UART1 | PA9/PA10 | Debug / SCPI przez UART |
| UART2 | PA2/PA3 | RS485 (half-duplex przez transceiver) |
| USB FS | PA11/PA12, PA11 (VBUS det.) | USB-TMC (TinyUSB) |
| CRS | – | Automatyczne przycinanie HSI48 na SOF USB |

### Mapowanie pinów GPIO

| Pin | Kierunek | Sygnał | Opis |
|-----|:---:|-------|------|
| PA2 | OUT | UART2_TX | RS485 TX / RS485 DE |
| PA3 | IN | UART2_RX | RS485 RX |
| PA4 | OUT | REL_EN | Zasilanie przekaźników (STPIC6C595 OE, active HIGH) |
| PA5 | OUT | SPI1_SCK | Zegar SPI dla shift-registrów |
| PA6 | OUT | REL_RCK | Latch shift-registrów (aktywny zboczem narastającym) |
| PA7 | OUT | SPI1_MOSI | Dane do shift-registrów |
| PA9 | OUT | UART1_TX | Debug TX |
| PA10 | IN | UART1_RX | Debug RX |
| PA11 | IN | USB_DETECT | Detekcja VBUS (5V obecne) |
| PA11/12 | IO | USB_DM/DP | Magistrala USB D−/D+ |
| PB6 | OUT | I2C1_SCL | Zegar I2C (FRAM) |
| PB7 | IO | I2C1_SDA | Dane I2C (FRAM) |
| PB12 | OUT | W5500_CS | Chip Select dla W5500 (active LOW) |
| PB13 | OUT | SPI2_SCK | Zegar SPI dla W5500 |
| PB14 | IN | SPI2_MISO | Dane z W5500 |
| PB15 | OUT | SPI2_MOSI | Dane do W5500 |
| PC13 | OUT | LED_R | LED czerwony (błąd SCPI) |
| PC14 | OUT | LED_G | LED zielony (heartbeat) |
| Pxx | OUT | W5500_RST | Reset W5500 (active LOW, 10 ms) |

---

## Schemat blokowy systemu

```
                    ┌──────────────────────────────────────┐
                    │         STM32G431CBT6 @ 170 MHz      │
                    │                                      │
 USB Host ─────────── USB FS (PA11/PA12)                  │
                    │   └─ TinyUSB (USB-TMC Class)         │
                    │                                      │
 RJ-45 ─── PHY ─── │ SPI2 (PB12-15) ── W5500             │
                    │   └─ VXI-11 / DHCP / mDNS            │
                    │                                      │
                    │ SPI1 (PA4-7) ─── STPIC6C595 ×6      │
                    │   └─ REL_EN (PA4)                    │── 48 przekaźników
                    │   └─ REL_RCK (PA6)                   │   (6 dekad + Q6/Q7)
                    │                                      │
 FRAM ─────────────── I2C1 (PB6/PB7) ── FM24C64B         │
 (kalibracja,       │                                      │
  config sieci)     │ UART1 (PA9/PA10) ── Debug           │
                    │ UART2 (PA2/PA3)  ── RS485            │
                    │                                      │
                    │ LED_G (PC14) 1 Hz heartbeat          │
                    │ LED_R (PC13) błąd SCPI               │
                    └──────────────────────────────────────┘
```

---

## Zasilanie

| Szyna | Napięcie | Źródło | Odbiorniki |
|-------|:---:|--------|-----------|
| VBUS | 5 V | Złącze USB lub zewnętrzne | Przetwornica 3.3 V, zasilanie przekaźników |
| VDD | 3.3 V | Przetwornica LDO | STM32, W5500, FRAM, STPIC6C595 |
| VREL | 5 V | VBUS | Cewki przekaźników przez STPIC6C595 |

---

## Format płyty

| Parametr | Wartość |
|---------|---------|
| Standard | 3U Eurocard (IEEE 1101.1-1998) |
| Wymiary | 160 × 100 mm |
| Warstwy PCB | 2 |
| Złącze tylne | DIN 41612 (typ C, 96 pin) |
| Rack | 19" 3U |
| Rewizja | v0.3 |

---

## Sekcje szczegółowe

- **[Macierz przekaźników](relay-matrix.md)** – STPIC6C595, kodowanie cyfr, sekwencja ochronna
- **[Ethernet – W5500](ethernet.md)** – W5500, SPI, gniazda, DHCP, VXI-11
- **[Pamięć – FRAM i Flash](memory.md)** – FM24C64B, mapa pamięci, CRC32, flash fallback
