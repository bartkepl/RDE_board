# Status projektu & TODO

## Co zostało zrealizowane

### Elektronika / PCB

- [x] Projekt schematu elektrycznego (13 arkuszy KiCAD)
- [x] Projekt PCB dwuwarstwowy, format 3U Eurocard 160×100 mm
- [x] Złącze tylne DIN 41612 (montaż w rack 19")
- [x] Sprzętowy stos TCP/IP (W5500 na SPI2)
- [x] Sterowniki shift-register STPIC6C595 (6 układów, łańcuch SPI1)
- [x] Pamięć FRAM FM24C64B (I2C1)
- [x] Interfejs USB 2.0 FS (STM32G4 wbudowany, CRS)
- [x] UART1 (debug), UART2 (RS485 half-duplex)
- [x] Wskaźniki LED (heartbeat + błąd SCPI)
- [x] Zamówienie i składanie prototypu PCB v0.3

### Firmware

- [x] Inicjalizacja STM32G431CBT6 (HAL, zegary, peryferia)
- [x] Obsługa STPIC6C595 – sterowanie 6 dekadami rezystancji
- [x] Sekwencja ochronna przekaźników (zapobiega przepięciom przy przełączaniu)
- [x] Przekaźniki ochronne terminali Q6 (CONNECT) i Q7 (SHORT)
- [x] Parser SCPI (libscpi) – pełna zgodność z IEEE 488.2
- [x] Komendy SCPI: `RESistance`, `OUTPut`, `RELay`, `NET`, `CALibration`, `SYSTem`, IEEE 488.2
- [x] USB-TMC (TinyUSB) z detekcją VBUS
- [x] VXI-11 serwer (ONC-RPC, port 703)
- [x] Portmapper RPC (port 111 TCP+UDP)
- [x] Klient DHCP (WIZnet ioLibrary)
- [x] mDNS/zeroconf discovery
- [x] Sterownik FRAM FM24C64B (I2C)
- [x] Kalibracja 60-punktowa z zapisem do FRAM (PRIMARY + BACKUP + CRC32)
- [x] Konfiguracja sieci w FRAM + fallback na flash STM32G4 (strona 63)
- [x] Numer seryjny z UID MCU (FNV-1a hash)
- [x] Heartbeat LED_G, błąd LED_R
- [x] Komenda `SYSTem:BOOTloader:ENter` (DFU przez USB)

### Oprogramowanie PC

- [x] `rde_control.py` – GUI Tkinter, sterowanie dekadami, kalibracja, config sieci
- [x] Obsługa multimetrów: HP 34401A, Agilent 34970A, Keithley 2000/2002
- [x] Automatyczna kalibracja z raportem PDF
- [x] Skaner VXI-11 w podsieci
- [x] Katalog błędów SCPI z opisami

---

## TODO – do zrobienia

### Krytyczne / Hardware

- [ ] **PCB v0.4**: naprawa problemu tłumienia RX (umożliwi 100 Mbps Ethernet)
- [ ] Weryfikacja wartości rezystorów sieci dekad na płycie v0.3 pod kątem liniowości
- [ ] Testy termiczne (dryf rezystancji w zakresie -20°C do +70°C)

### Firmware

- [ ] **RS485**: zaimplementować obsługę UART2 z half-duplex transceiverem
- [ ] Pełna obsługa SCPI przez UART1 (aktualnie tylko debug)
- [ ] Mutex na buforze `scpi_reply_buf` (zabezpieczenie przed jednoczesnym USB+ETH)
- [ ] Komenda `NET:MACaddress?` – odczyt adresu MAC
- [ ] `CALibration:DATE?` – data ostatniej kalibracji (wymaga RTC lub NTP)
- [ ] Weryfikacja sekwencji ochronnej w ekstremalnych przypadkach (R = 999 999 → 0)
- [ ] Aktualizacja firmware OTA przez Ethernet (prosty HTTP bootloader)

### Oprogramowanie PC

- [ ] Testy `rde_control.py` na docelowym sprzęcie z wszystkimi multimetrami
- [ ] Auto-reconnect po utracie połączenia VISA
- [ ] Eksport/import tabeli kalibracyjnej do CSV
- [ ] Testy regresji dla `scpi_test.py` – automatyczny smoke-test po wgraniu firmware
- [ ] Obsługa RS485 w `rde_control.py` (gdy firmware gotowe)
- [ ] Pakiet instalacyjny (PyInstaller exe dla Windows)

### Dokumentacja

- [ ] Schemat elektryczny jako PDF w dokumentacji (link do `prod/`)
- [ ] Interaktywny BoM (link do `prod/`)
- [ ] Tutorial wideo: unboxing → podłączenie → pierwsza kalibracja
- [ ] Application Note: integracja z LabVIEW / MATLAB / pytest

### Testy produkcyjne

- [ ] Test fixture (jig) do zautomatyzowanego testu każdej płyty
- [ ] Procedura `Go/No-Go` dla wszystkich 60 punktów kalibracj
- [ ] Skrypt Python do automatycznego testu produkcyjnego

---

## Znane błędy / ograniczenia

| ID | Opis | Severity | Status |
|----|------|:---:|:---:|
| HW-01 | PCB v0.3: tłumienie RX Ethernet – tylko 10 Mbps half-duplex | Medium | Obejście w FW |
| FW-01 | Brak mutexa na `scpi_reply_buf` – ryzyko przy jednoczesnym USB+ETH | Low | Otwarte |
| FW-02 | RS485 (UART2) niezaimplementowane | Medium | Otwarte |
| SW-01 | `rde_control.py` brak auto-reconnect | Low | Otwarte |

---

## Historia zmian (git)

| Commit | Opis |
|--------|------|
| `8cf27d9` | Added FRAM param storage and calibration |
| `c4bb4f2` | Done core functionality – needs polishing |
| `50a9998` | Firmware development – USBTMC done, ETH almost with hardware and soft modifications |
| `19b9de2` | Edited Resistor array to be included in BoM |
| `6d148f0` | Updated revision: v0.3 |

---

## Środowisko pracy

| Narzędzie | Wersja / Uwagi |
|---------|--------------|
| STM32CubeIDE | 1.14+ |
| arm-none-eabi-gcc | 12.x (wbudowany w CubeIDE) |
| KiCAD | 7.x / 8.x |
| Python | 3.9+ |
| ST-Link | V2/V3 (debug i flash) |
| PyVISA | 1.13+ |
