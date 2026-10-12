# miri-logger (Arduino — szkic)

Logger OBD na **ESP32-P4 + ESP32-C6** (Guition JC4880P443C_I_W); docelowo: LVGL UI + SD + eksport WiFi.

## Budowa

Arduino IDE 2.3.10:
- płytka: **ESP32P4 Dev Module** (esp32 core 3.3.11), PSRAM wg modułu (32 MB),
- po podłączeniu: sprawdzić flash/PSRAM/piny wg wiki Guition.

arduino-cli:
```
arduino-cli compile --fqbn esp32:esp32:esp32p4 .
```

## Co juz dziala (v0.1)

- `src/elm327.c` — sterownik ELM327 (transport-agnostyczny),
- `src/obd_*.c` — tabela PID + parser odpowiedzi,
- `src/obdlog.c` — logi w kontrakcie `obd-logger/1` (JSONL + candump + DTC),
- `src/transport_ble.cpp` — BLE central -> vLinker MS (preferuje FFF1/FFF2, fallback: pierwsza para write+notify),
- `src/transport_uart.cpp` — UART (testy na biurku z klasycznym ELM327),
- `miri-logger.ino` — pętla: polacz -> elm_init -> VIN/DTC -> PID-y -> reconnect.

## Do zrobienia przy bring-upie (na plytce)

1. **Firmware C6 (esp_hosted) >= 2.6.0** — wtedy `hostedInitBLE()` uruchomi kontroler BT.
   Starszy firmware (np. 2.3.0 fabryczny) -> BLE milczy; aktualizacja przez API
   `hostedBeginUpdate()/hostedWriteUpdate()/...` (arduino core 3.3.x) albo esptool.
2. **Piny SDIO C6↔P4** — jesli inne niz domyslne: `hostedSetPins(clk,cmd,d0..d3,rst)`
   przed `BLEDevice::init`.
3. **SD**: SD_MMC + katalog `/sdcard/logs` (piny z wiki Guition).
4. **LVGL UI** + eksport WiFi (kolejne kroki).

## Kontrakt logow

Patrz: `../docs/integracja-mirican.md` (dokladnie te same formaty co firmware IDF).
