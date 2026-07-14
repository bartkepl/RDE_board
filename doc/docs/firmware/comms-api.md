# API komunikacji

Moduł komunikacji obejmuje trzy warstwy: parser SCPI (libscpi), USB-TMC (TinyUSB), oraz VXI-11/Ethernet. Wszystkie trzy współdzielą jeden bufor odpowiedzi.

---

## Bufor współdzielony

```c
// scpi_def.h
extern uint8_t scpi_reply_buf[512];
extern uint16_t scpi_reply_len;
```

```
                ┌──────────────────────┐
                │    libscpi context   │
                │    SCPI_Main_Init()  │
                └──────────┬───────────┘
                           │ wypełnia po każdej komendzie
                           ▼
                  scpi_reply_buf[512]
                      ┌────┴────┐
                      │         │
               usbtmc_app   vxi11_server
               (USB-TMC)   (VXI-11/TCP)
```

!!! warning "Brak mutexa"
    Bufor nie jest chroniony mutexem. Jednoczesne żądania przez USB i Ethernet (od różnych hostów) mogą powodować wyścig danych. W praktyce jest to mało prawdopodobne przy braku RTOS, ale jest to znane ograniczenie.

---

## Parser SCPI – libscpi

### Inicjalizacja

```c
// scpi_def.c

static scpi_t scpi_context;
static char   scpi_input_buf[256];
static scpi_error_t scpi_error_queue[16];

void SCPI_Main_Init(void) {
    SCPI_Init(&scpi_context,
              scpi_commands,         // tablica komend
              &scpi_interface,       // callbacks I/O
              scpi_units_def,        // definicje jednostek SCPI
              "bartkepl", "RDE", "1.0", serial_str,
              scpi_input_buf, sizeof(scpi_input_buf),
              scpi_error_queue, ARRAY_SIZE(scpi_error_queue),
              scpi_reply_buf, sizeof(scpi_reply_buf));
}
```

### Interfejs I/O libscpi

libscpi wywołuje cztery callbacki przez wskaźnik `scpi_interface`:

| Callback | Kiedy wywoływany | Implementacja |
|---------|-----------------|---------------|
| `write(ctx, data, len)` | Przy każdej odpowiedzi komendy | Wypełnia `scpi_reply_buf` |
| `flush(ctx)` | Po zakończeniu komendy | Ustawia `scpi_reply_len` |
| `error(ctx, err)` | Przy błędzie parsowania | Wpisuje do kolejki błędów |
| `control(ctx, name, val)` | Sygnały statusu IEEE 488 | Opcjonalne |

### Przetwarzanie danych wejściowych

```c
// Wejście od USB-TMC:
SCPI_Input(&scpi_context, (char*)buf, len);

// Wejście od VXI-11:
SCPI_Input(&scpi_context, (char*)buf, len);
// → po zakończeniu, scpi_reply_buf zawiera odpowiedź
```

### Poll (nieblokujące)

```c
void SCPI_Main_Poll(void) {
    SCPI_Input(&scpi_context, NULL, 0);  // brak danych → tylko housekeeping
}
```

---

## USB-TMC – TinyUSB

### Konfiguracja deskryptorów USB

```c
// usb_descriptors.c (generowane przez TinyUSB)

#define USB_VID   0xCAFE
#define USB_PID   0x4000

// Klasa USBTMC: Class=0xFE, SubClass=0x03, Protocol=0x01 (USB488)
// Interfejs 0: USBTMC Bulk-OUT (komendy) + Bulk-IN (odpowiedzi) + Interrupt-IN (status)
```

### Maszyna stanów USB-TMC (`usbtmc_app.c`)

```c
typedef enum {
    USBTMC_STATE_IDLE,
    USBTMC_STATE_PROCESSING,
    USBTMC_STATE_REPLY_READY,
} usbtmc_state_t;
```

```c
void usbtmc_app_task_iter(void) {
    switch (state) {

    case USBTMC_STATE_IDLE:
        if (tud_usbtmc_rx_buf_count()) {         // Czy jest nowy pakiet?
            tud_usbtmc_receive_cb(buf, &len);    // Pobierz dane
            if (is_end_of_message(buf, len)) {
                SCPI_Input(&scpi_context, buf, len);
                state = USBTMC_STATE_REPLY_READY;
            }
        }
        break;

    case USBTMC_STATE_REPLY_READY:
        // Host prosi o dane (REQUEST_DEV_DEP_MSG_IN)?
        if (pending_request_dev_dep_in) {
            tud_usbtmc_transmit(scpi_reply_buf, scpi_reply_len, true /* EOM */);
            state = USBTMC_STATE_IDLE;
        }
        break;
    }
}
```

### Obsługiwane MsgID USBTMC

| MsgID | Nazwa | Kierunek | Opis |
|:---:|------|:---:|------|
| 1 | `DEV_DEP_MSG_OUT` | Host → Urządzenie | Komenda SCPI (ASCII) |
| 2 | `REQUEST_DEV_DEP_MSG_IN` | Host → Urządzenie | Żądanie odpowiedzi |
| 3 | `DEV_DEP_MSG_IN` | Urządzenie → Host | Odpowiedź SCPI |
| 128 | `VENDOR_SPECIFIC_OUT` | Host → Urządzenie | Ignorowany |

### Wykrywanie VBUS

```c
// W głównej pętli main.c:
bool vbus = HAL_GPIO_ReadPin(USB_DETECT_GPIO_Port, USB_DETECT_Pin) == GPIO_PIN_SET;

if (vbus && !tud_connected())  tud_connect();    // PA12 (D+) podciągnięty
if (!vbus && tud_connected())  tud_disconnect(); // PA12 LOW
```

Urządzenie nie jest widoczne dla hosta dopóki nie ma napięcia na VBUS (PA11).

---

## VXI-11 – serwer ONC-RPC

### Protokół

VXI-11 Core to protokół RPC (Remote Procedure Call) zdefiniowany przez VXI-11 rev 1.0:

| Element | Wartość |
|---------|---------|
| Transport | TCP port **703** |
| RPC Program | `0x0607AF` |
| RPC Version | `1` |
| Portmapper | UDP+TCP port 111 |

### Obsługiwane procedury RPC

| Numer RPC | Nazwa | Opis |
|:---:|------|----|
| 10 | `create_link` | Otwiera sesję instrumentu, zwraca `link_id` |
| 11 | `device_write` | Wysyła dane do parsera SCPI |
| 12 | `device_read` | Odczytuje z `scpi_reply_buf` |
| 15 | `device_clear` | Czyści kolejkę błędów (jak `*CLS`) |
| 17 | `device_remote` | Wejście w tryb zdalny (no-op) |
| 18 | `device_local` | Wyjście z trybu zdalnego (no-op) |
| 23 | `destroy_link` | Zamyka sesję instrumentu |

### Ograniczenia

- **Jedno połączenie naraz** – próba `create_link` gdy sesja już otwarta zwraca błąd `ERR_DEVICE_LOCKED`
- **Brak SRQ** (Service Request) – przerwanie instrumentu niezaimplementowane
- **Brak Lock** mechanizmu (VXI-11 `device_lock` ignorowany)

### Przepływ połączenia

```
Klient (PyVISA)                     Serwer RDE
      │                                   │
      │── Portmapper TCP:111 ────────────►│ "Gdzie VXI-11 Core?"
      │◄─────────────────── Port 703 ────-│
      │                                   │
      │── TCP connect :703 ─────────────► │
      │── RPC: create_link ─────────────► │ Otwiera sesję
      │◄─────────── link_id=1, error=0 ──-│
      │                                   │
      │── RPC: device_write "*IDN?\n" ──► │ → SCPI_Input()
      │── RPC: device_read ─────────────► │ ← scpi_reply_buf
      │◄── "bartkepl,RDE,1.0,A3F7B201" ──│
      │                                   │
      │── RPC: destroy_link ────────────► │ Zamknięcie sesji
      │── TCP FIN ──────────────────────► │
```

---

## mDNS (`mdns/`)

Implementacja mDNS rozgłasza urządzenie w sieci lokalnej co **~1 sekundę** (TTL = 120 s):

```
Multicast 224.0.0.251:5353 (UDP, gniazdo W5500 nr 4)
  Rekord A:   RDE-A3F7B201.local → <aktualny IP>
  Rekord SRV: _vxi-11._tcp.local → RDE-A3F7B201.local:703
  Rekord TXT: model=RDE
```

---

## Portmapper (`w5500_net.c`)

Portmapper (RFC 1833) informuje klientów VXI-11 o porcie serwera. RDE implementuje uproszczoną wersję obsługującą tylko jeden program:

```
Program:  0x0607AF (VXI-11 Core)
Version:  1
Protocol: TCP → port 703
```

Gniazda 1 (TCP:111) i 2 (UDP:111) obsługują zapytania portmappera.
