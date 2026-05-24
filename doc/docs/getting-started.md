# Pierwsze kroki

## Wymagania

| Element | Minimalne wymaganie |
|---------|---------------------|
| Python | 3.9+ |
| PyVISA | ≥ 1.13 |
| pyvisa-py | ≥ 0.7 (bez NI-VISA) lub NI-VISA ≥ 21 |
| System | Windows 10/11, Linux, macOS |
| Kabel | USB Micro-B lub RJ-45 Ethernet |

---

## Podłączenie przez USB

### 1. Instalacja sterownika (Windows)

USBTMC wymaga sterownika libusb na Windows. Bez niego urządzenie nie pojawi się w PyVISA.

1. Pobierz **[Zadig](https://zadig.akeo.ie/)** i uruchom jako Administrator.
2. Podłącz urządzenie kablem USB – powinno się pojawić jako `RDE` lub `USB Serial Device`.
3. W menu *Options* zaznacz **List All Devices**.
4. Wybierz urządzenie z VID = `0xCAFE`, PID = `0x4000`.
5. Wybierz sterownik **WinUSB** i kliknij **Install Driver**.

!!! info "Linux / macOS"
    Sterownik `usbtmc` jest wbudowany w jądro Linux. Na macOS działa przez `pyvisa-py`. Dodaj regułę udev na Linuksie:
    ```
    echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="cafe", MODE="0666"' | sudo tee /etc/udev/rules.d/99-rde.rules
    sudo udevadm control --reload-rules
    ```

### 2. Instalacja bibliotek Python

```bash
pip install pyvisa pyvisa-py
# Windows USB:
pip install pyusb
```

### 3. Pierwsze połączenie USB

```python
import pyvisa

rm = pyvisa.ResourceManager('@py')      # pyvisa-py bez NI-VISA
resources = rm.list_resources()
print(resources)
# ('USB0::0xCAFE::0x4000::A3F7B201::0::INSTR',)

inst = rm.open_resource('USB0::0xCAFE::0x4000::A3F7B201::0::INSTR')
inst.timeout = 3000
print(inst.query('*IDN?'))
# bartkepl,RDE,1.0,A3F7B201
```

---

## Podłączenie przez Ethernet

### 1. Połączenie fizyczne

Podłącz kabel RJ-45 do tej samej podsieci co komputer. Urządzenie automatycznie pobierze adres przez DHCP.

!!! tip "Wykrywanie adresu IP"
    Sprawdź DHCP lease w routerze lub użyj skanera mDNS:
    ```bash
    # Linux/macOS
    avahi-browse -t _vxi-11._tcp
    # Windows (z Bonjour)
    dns-sd -B _vxi-11._tcp
    ```
    Hostname mDNS: `RDE-<serial>.local`

Domyślny fallback jeśli brak DHCP: **`192.168.1.6`**

### 2. Pierwsze połączenie VXI-11

```python
import pyvisa

rm = pyvisa.ResourceManager('@py')
inst = rm.open_resource('TCPIP::192.168.1.6::INSTR')
inst.timeout = 5000
print(inst.query('*IDN?'))
# bartkepl,RDE,1.0,A3F7B201
```

Lub przez mDNS (Python ≥ 3.13 lub z biblioteką `zeroconf`):

```python
inst = rm.open_resource('TCPIP::RDE-A3F7B201.local::INSTR')
```

---

## Podstawowe komendy SCPI

Po nawiązaniu połączenia możesz od razu sterować urządzeniem:

```python
# Identyfikacja
print(inst.query('*IDN?'))
# bartkepl,RDE,1.0,A3F7B201

# Reset do stanu domyślnego (R = 0 Ω, wyjście zwarte)
inst.write('*RST')

# Ustawienie rezystancji
inst.write('RESistance:VALue 4700')
print(inst.query('RESistance:VALue?'))
# 4700

# Podłączenie wyjścia (CONNECT)
inst.write('OUTPut:STATe ON')
print(inst.query('OUTPut:STATe?'))
# 1

# Zwieranie wyjścia (SHORT – bezpieczny stan pomiarowy)
inst.write('OUTPut:STATe OFF')
```

---

## Kompletny przykład – pomiar 4-przewodowy

```python
import pyvisa
import time

def check_errors(inst):
    """Odczytaj i wydrukuj błędy z kolejki SCPI."""
    while True:
        err = inst.query('SYSTem:ERRor?')
        code, desc = err.split(',', 1)
        if int(code) == 0:
            break
        print(f'  SCPI Error {code}: {desc.strip()}')

rm = pyvisa.ResourceManager('@py')
inst = rm.open_resource('TCPIP::192.168.1.6::INSTR')
inst.timeout = 5000

# Zresetuj i ustaw rezystancję
inst.write('*RST')
inst.write('*CLS')
inst.write('RESistance:VALue 10000')   # 10 kΩ

# Podłącz wyjście – teraz możesz mierzyć
inst.write('OUTPut:STATe ON')
time.sleep(0.1)    # stabilizacja przekaźników

# Odczyt (jeśli kalibracja włączona – wartość ze skrótem mΩ)
r = inst.query('RESistance:VALue?')
print(f'Nastawa: {r} Ω')

check_errors(inst)
inst.write('OUTPut:STATe OFF')   # bezpieczne zwieranie po pomiarze
inst.close()
```

---

## Ustawianie pojedynczej dekady

```python
# Dekada 4 (1 kΩ) = cyfra 7 → wkład 7000 Ω
inst.write('RESistance:DECade 4,7')

# Odczyt cyfry danej dekady
d = inst.query('RESistance:DECade? 4')
print(d)   # 7
```

---

## Sprawdzenie konfiguracji sieci

```python
print(inst.query('NET:DHCP?'))        # 1
print(inst.query('NET:IPADdress?'))   # 192.168.1.6
print(inst.query('NET:STATe?'))       # 192.168.1.67  (adres DHCP)
```

---

## Wejście w bootloader DFU (aktualizacja firmware)

```python
inst.write('SYSTem:BOOTloader:ENter')
# Urządzenie pojawi się jako "STM32 BOOTLOADER" w menedżerze urządzeń
# Wgraj nowy firmware przez DfuSe lub dfu-util
```

!!! warning
    Po wejściu w bootloader urządzenie nie odpowiada na SCPI. Wymagane fizyczne odłączenie i ponowne podłączenie USB po aktualizacji.

---

## Rozwiązywanie problemów

| Objaw | Przyczyna | Rozwiązanie |
|-------|-----------|-------------|
| Urządzenie niewidoczne w `list_resources()` | Brak sterownika WinUSB | Ponownie uruchom Zadig jako Admin |
| `VISA ERROR -1073807339` | Błędny VID/PID lub brak sterownika | Sprawdź VID=0xCAFE, PID=0x4000 w Zadig |
| Timeout po `*IDN?` | Zbyt krótki timeout | Ustaw `inst.timeout = 5000` |
| LED_R świeci | Błędy w kolejce SCPI | Wywołaj `SYSTem:ERRor?` do wyczyszczenia |
| Brak odpowiedzi przez Ethernet | Urządzenie nie dostało IP | Sprawdź DHCP, użyj statycznego IP |
| `RESistance:VALue?` zwraca `UNKN` | Po komendzie `RELay:STATe` | Ustaw ponownie przez `RESistance:VALue` |
