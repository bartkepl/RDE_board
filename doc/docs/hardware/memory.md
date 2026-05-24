# Pamięć – FRAM i Flash

## FM24C64B – FRAM

**FM24C64B** to ferreoelektryczna pamięć nieulotna (FRAM) 64 Kb (8192 bajtów) firmy Cypress/Infineon, dostępna przez magistralę I2C1.

| Parametr | Wartość |
|---------|---------|
| Pojemność | 8192 bajtów (64 Kb) |
| Interfejs | I2C, do 1 MHz (Fast Plus) |
| Adres I2C | `0x50` (A0=A1=A2=GND) |
| Czas zapisu | Natychmiastowy (brak opóźnienia Write Cycle jak w EEPROM) |
| Wytrzymałość zapisu | 10^12 cykli (praktycznie nieograniczona) |
| Retencja danych | 151 lat @ 55 °C |
| Napięcie zasilania | 2.7–3.65 V |
| Prąd aktywny | < 1 mA |
| Opakowanie | SOP-8 |

### Przewaga nad Flash / EEPROM

| Cecha | FRAM FM24C64B | EEPROM | STM32 Flash |
|-------|:---:|:---:|:---:|
| Czas zapisu | < 1 µs | 5–10 ms | 1–2 ms (po erase) |
| Kasowanie przed zapisem | Nie | Tak | Tak (cała strona) |
| Cykle zapisu | 10^12 | ~10^6 | ~10^4 |
| Granularność | 1 bajt | 1 bajt | 1 strona (2 KB) |

---

## Mapa pamięci FRAM

```
Adres       Rozmiar  Zawartość
──────────────────────────────────────────────────────────────
0x0000      24 B     net_config_t PRIMARY (IP, maska, GW, DHCP, magic)
0x0018       4 B     CRC32 net_config PRIMARY
0x001C       4 B     padding
0x0020      24 B     net_config_t BACKUP
0x0038       4 B     CRC32 net_config BACKUP
0x003C       4 B     padding
──────────────────────────────────────────────────────────────
0x0040     240 B     cal_data_t PRIMARY  [uint32_t milliohm[6][10]]
0x0130       4 B     CRC32 cal_data PRIMARY
0x0134       8 B     cal_config_t PRIMARY  {magic, enabled, pad[3]}
0x013C       4 B     CRC32 cal_config PRIMARY
──────────────────────────────────────────────────────────────
0x0140     240 B     cal_data_t BACKUP
0x0230       4 B     CRC32 cal_data BACKUP
0x0234       8 B     cal_config_t BACKUP
0x023C       4 B     CRC32 cal_config BACKUP
──────────────────────────────────────────────────────────────
0x0240    ~7872 B    WOLNE (do 0x1FFF)
```

### Struktury danych

**`net_config_t`** (24 bajty):
```c
typedef struct {
    uint32_t magic;        // NET_CONFIG_MAGIC = 0xDE1AC0DF
    uint8_t  ip[4];        // Statyczne IP
    uint8_t  sn[4];        // Maska podsieci
    uint8_t  gw[4];        // Brama domyślna
    uint8_t  use_dhcp;     // 0 = statyczne, 1 = DHCP
    uint8_t  phy_mode;     // 0=AUTO, 1=10M HD, 2=100M FD
    uint8_t  _pad[6];
} net_config_t;            // 24 bajty = 3 × DWORD
```

Tryb PHY konfigurowany komendą `NET:PHY:MODE`; patrz [NET – konfiguracja sieci](../scpi/network.md).

**`cal_data_t`** (240 bajtów):
```c
typedef struct {
    uint32_t milliohm[6][10];  // [dekada 0..5][cyfra 0..9]
} cal_data_t;
```

Całkowity rozmiar: 6 × 10 × 4 = **240 bajtów**.

**`cal_config_t`** (8 bajtów):
```c
typedef struct {
    uint32_t magic;    // CAL_MAGIC = 0xCA11B001
    uint8_t  enabled;  // 0 = korekcja wyłączona, 1 = włączona
    uint8_t  _pad[3];
} cal_config_t;
```

---

## Schemat CRC i redundancji

Każdy blok danych ma towarzyszący **CRC32** (IEEE 802.3). Strategia odczytu:

```
Odczyt PRIMARY
    │
    ├─ CRC OK? ──YES──► Użyj PRIMARY
    │
    └─ CRC FAIL──► Odczyt BACKUP
                       │
                       ├─ CRC OK? ──YES──► Użyj BACKUP, przepisz PRIMARY
                       │
                       └─ CRC FAIL──► (dla net_config) spróbuj Flash STM32
                                       │
                                       └─ też błąd → wartości domyślne
```

Przy każdym zapisie dane są najpierw zapisywane do PRIMARY, a następnie do BACKUP (dwa osobne zapisy FRAM z CRC).

---

## Inicjalizacja FRAM w firmware

```c
// Sprawdzenie obecności FRAM przez I2C ping
HAL_StatusTypeDef st = fm24_ping();
if (st != HAL_OK) {
    SCPI_ErrorPush(ctx, SCPI_ERROR_SYSTEM_ERROR);
    // Firmware kontynuuje, ale bez FRAM
    // Konfiguracja sieci: wczytana z Flash STM32
    // Kalibracja: tylko wartości nominalne
}
```

---

## Flash STM32G4 – zapasowa konfiguracja sieci

Gdy FRAM jest niedostępny, konfiguracja sieci jest przechowywana w **ostatniej stronie Flash STM32G4**:

| Parametr | Wartość |
|---------|---------|
| Adres | `0x0801F800` (strona 63, 2 KB) |
| Rozmiar strony | 2048 bajtów |
| Wytrzymałość | ~10 000 cykli kasowanie/zapis |
| Czas kasowania strony | ~10 ms |
| Format | `net_config_t` + CRC32 (28 bajtów) |

!!! warning
    Flash STM32G4 wymaga skasowania całej strony 2 KB przed zapisem. Ta operacja jest destruktywna i trwa ~10 ms podczas której CPU jest zablokowane (brak ICACHE w STM32G4). Unikaj częstego zapisu do Flash – używaj FRAM gdy dostępny.

### Procedura zapisu do Flash

```c
void net_config_flash_save(const net_config_t *cfg) {
    HAL_FLASH_Unlock();
    
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Page      = 63,
        .NbPages   = 1
    };
    uint32_t error;
    HAL_FLASHEx_Erase(&erase, &error);
    
    // Zapis double-word (64-bit) – wymóg STM32G4
    for (int i = 0; i < sizeof(net_config_t); i += 8)
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          FLASH_CONFIG_ADDR + i,
                          *((uint64_t*)(buf + i)));
    
    HAL_FLASH_Lock();
}
```

---

## Sterownik I2C FRAM (`fm24c64b.c`)

| Funkcja | Opis |
|---------|------|
| `fm24_ping()` | Sprawdza obecność przez HAL_I2C_IsDeviceReady |
| `fm24_read(addr, buf, len)` | Odczyt `len` bajtów z adresu `addr` |
| `fm24_write(addr, buf, len)` | Zapis `len` bajtów pod adres `addr` |

Bajty adresu wysyłane są big-endian (MSB first), zgodnie ze specyfikacją I2C FRAM.

Prędkość I2C1: **400 kHz** (Fast Mode). FRAM obsługuje do 1 MHz – ograniczenie po stronie konfiguracji STM32 CubeMX.
