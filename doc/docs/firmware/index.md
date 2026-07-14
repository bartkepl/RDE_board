# Architektura firmware

## Środowisko i zależności

| Element | Wersja / Opis |
|---------|--------------|
| IDE | STM32CubeIDE 1.14+ |
| Kompilator | arm-none-eabi-gcc 12.x (wbudowany w CubeIDE) |
| HAL | STM32Cube HAL + CMSIS (generowane przez CubeMX) |
| USB stack | TinyUSB (vendored w `App/tinyusb/`) |
| Ethernet stack | WIZnet ioLibrary Driver (vendored w `App/wiznet/`) |
| SCPI parser | libscpi (vendored w `App/libscpi/`) |
| Język | C11 |

---

## Struktura katalogów

```
firmware/RDE_soft/
├── Core/
│   ├── Inc/                        # Nagłówki CubeMX (GPIO, SPI, I2C, UART, USB)
│   ├── Src/
│   │   └── main.c                  # Punkt wejścia, inicjalizacja, główna pętla
│   └── Startup/                    # Plik startowy STM32 (assembly)
├── Drivers/
│   ├── CMSIS/                      # ARM CMSIS headers
│   └── STM32G4xx_HAL_Driver/       # STM32 HAL
└── App/                            # Kod aplikacji
    ├── scpi_def.h / scpi_def.c     # Tabela komend SCPI i handlery
    ├── net_config.h / net_config.c # Konfiguracja sieci (FRAM + flash)
    ├── w5500_net.h / w5500_net.c   # Zarządzanie W5500, DHCP, sockety
    ├── usbtmc_app.h / usbtmc_app.c # Warstwa aplikacji USB-TMC
    ├── relay/
    │   ├── relay_ctrl.h / .c       # Sterowanie STPIC6C595
    │   └── relay_cal.h  / .c       # Kalibracja + FRAM
    ├── fram/
    │   └── fm24c64b.h / .c         # Sterownik I2C FRAM
    ├── vxi11/
    │   └── vxi11_server.h / .c     # Serwer VXI-11 RPC
    ├── utils/
    │   └── utils.c                 # Numer seryjny (UID + FNV-1a)
    ├── libscpi/                    # Parser SCPI (zewnętrzny)
    ├── tinyusb/                    # Stack USB (zewnętrzny)
    ├── wiznet/                     # WIZnet driver (zewnętrzny)
    └── mdns/                       # mDNS (zewnętrzny)
```

---

## Przepływ danych

```
Klient VISA (PyVISA)
        │
   ┌────┴─────┐
   │          │
USB-TMC    VXI-11/TCP
(TinyUSB)  (port 703)
   │          │
   └────┬─────┘
        │ SCPI tekst ASCII
        ▼
   libscpi parser
   (scpi_def.c – tabela komend)
        │
   ┌────┴────────────┐
   │                 │
relay_ctrl.c     net_config.c
relay_cal.c      vxi11_server.c
   │                 │
SPI1 (STPIC)     SPI2 (W5500)
   │                 │
Przekaźniki      Ethernet PHY
```

---

## Inicjalizacja – kolejność

```c
// main.c

// 1. HAL i zegar systemowy
HAL_Init();
SystemClock_Config();          // 170 MHz PLL z HSI16, HSI48 dla USB

// 2. Peryferia (CubeMX)
MX_GPIO_Init();
MX_SPI1_Init();                // Shift-registery (przekaźniki)
MX_SPI2_Init();                // W5500 Ethernet
MX_I2C1_Init();                // FRAM FM24C64B
MX_USART1_UART_Init();         // Debug / SCPI przez UART
MX_USART2_UART_Init();         // RS485
MX_USB_Init();                 // USB FS

// 3. CRS – automatyczne trymianie HSI48 synchronizowane przez SOF USB
// (eliminuje potrzebę zewnętrznego kwarcu USB)

// 4. Moduły aplikacji
relay_init();                  // STPIC do bezpiecznego stanu (SHORT ON)
SCPI_Main_Init();              // Kontekst libscpi, rejestracja komend
fm24_ping();                   // Weryfikacja FRAM – push SCPI error jeśli brak
net_config_init();             // Wczytaj konfigurację sieci z FRAM lub flash
relay_cal_init();              // Wczytaj kalibrację z FRAM (lub nominały)

// 5. USB – czeka na VBUS zanim podciągnie D+
tud_init(0);
tud_disconnect();              // D+ LOW, nie widoczny dla hosta

// 6. Ethernet
w5500_net_init();              // Reset W5500, konfiguracja IP, DHCP start
```

---

## Główna pętla (`while(1)`)

Firmware realizuje **kooperatywne planowanie zadań** bez RTOS – wszystkie zadania są nieblokujące:

```c
while (1) {

    /* === Heartbeat i status LED === */
    if (HAL_GetTick() - led_tick > 500) {
        led_tick = HAL_GetTick();
        HAL_GPIO_TogglePin(LED_G_GPIO_Port, LED_G_Pin);  // 1 Hz blink
    }
    // LED_R – świeci gdy kolejka błędów SCPI niepusta
    HAL_GPIO_WritePin(LED_R_GPIO_Port, LED_R_Pin,
                      SCPI_ErrorCount(ctx) > 0 ? GPIO_PIN_SET : GPIO_PIN_RESET);

    /* === USB === */
    bool vbus = HAL_GPIO_ReadPin(USB_DETECT_GPIO_Port, USB_DETECT_Pin);
    if (vbus && !tud_connected()) tud_connect();
    if (!vbus && tud_connected())  tud_disconnect();
    if (vbus) {
        tud_task();               // obsługa zdarzeń USB (SOF, SETUP, IN/OUT)
        usbtmc_app_task_iter();   // parsowanie ramek USB-TMC
    }

    /* === SCPI poll (wymagane przez libscpi) === */
    SCPI_Main_Poll();

    /* === Ethernet: DHCP, VXI-11, portmapper, mDNS === */
    w5500_net_task();
}
```

### Czasy zadań

| Zadanie | Typ | Typowy czas |
|---------|:---:|:---:|
| LED toggle | Co 500 ms | < 1 µs |
| `tud_task()` | Co iterację (polling) | 2–50 µs |
| `usbtmc_app_task_iter()` | Co iterację | 1–200 µs |
| `w5500_net_task()` | Co iterację | 50–500 µs |
| `SCPI_Main_Poll()` | Co iterację | < 1 µs |

---

## Moduły – opis

### `scpi_def.c` / `scpi_def.h`

- Tablica `scpi_commands[]` – mapuje ciągi SCPI na wskaźniki do funkcji handlerów
- Współdzielony bufor odpowiedzi `scpi_reply_buf[512]` (używany przez USB-TMC i VXI-11)
- Funkcja `SCPI_Main_Init()` – inicjalizuje kontekst libscpi
- Funkcja `SCPI_Main_Poll()` – nieblokujące wywołanie libscpi

### `net_config.c`

- `net_config_init()` – wczytuje konfigurację z FRAM (PRIMARY → BACKUP → Flash → domyślne)
- `net_config_save()` – zapisuje do FRAM (PRIMARY + BACKUP z CRC32)
- `net_config_flash_save()` – zapisuje do flash STM32G4 strona 63
- `net_config_apply()` – stosuje konfigurację: restartuje stos W5500

### `utils.c`

Generuje numery seryjne z **UID MCU** (96-bitowy unikalny numer STM32):

```c
// Krótki: 32-bitowy FNV-1a hash UID → 8 znaków HEX
void serial_get(char out[9]);

// Długi: surowe 96 bitów UID → 24 znaki HEX  
void serial_get_full(char out[25]);
```

---

## Obsługa błędów

- libscpi utrzymuje kolejkę błędów SCPI (max 16 pozycji)
- LED_R świeci gdy `SCPI_ErrorCount() > 0`
- Błędy kasowane przez `*CLS` lub `SYSTem:ERRor:NEXT?`
- Błąd braku FRAM przy starcie: `SCPI_ERROR_SYSTEM_ERROR` (kod -300)

---

## Sekcje szczegółowe

- **[API przekaźników i kalibracji](relay-api.md)** – `relay_ctrl.c`, `relay_cal.c`, typy, funkcje
- **[API komunikacji](comms-api.md)** – USB-TMC, VXI-11, SCPI, bufor współdzielony
