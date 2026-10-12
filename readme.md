# obd-logger (projekt osobny)

Logger OBD na panelu **Guition 4.3" (ESP32-P4 + ESP32-C6)**:

```
auto > OBD > vLinker MS > Bluetooth (BLE) > ESP32 (C6: radio, P4: UI/logi) > SD > WiFi > miriCAN
```

## Struktura

- `arduino/miri-logger/` — **firmware (szkic Arduino)** — cel: ESP32-P4+C6; kompiluje się
  (`arduino-cli compile --fqbn esp32:esp32:esp32p4`),
- `pc/bleak_vlinker_test.py` — test adaptera vLinker MS z PC (BLE),
- `docs/integracja-mirican.md` — kontrakt logów (JSONL + candump + eksport WiFi),
- `firmware-idf/` — wcześniejszy szkielet ESP-IDF (**parked**, referencja; nie rozwijamy).

## Status (2026-10-07)

- Decyzja: firmware w **Arduino** (esp32 core 3.3.11). P4 + BLE przez C6 (ESP-Hosted)
  są wspierane przez core (`hostedInitBLE()` wołane automatycznie przez bibliotekę BLE).
- Szkielet skompilowany pod `esp32p4` (65% flash, 0 ostrzeżeń).
- **BLE wymaga C6 firmware hosted >= 2.6.0** (fabryczny bywa starszy; update z API core).
- Do zrobienia przy bring-upie: piny SDIO C6 (`hostedSetPins`) i SD wg wiki Guition,
  LVGL UI, eksport WiFi.
- Czekamy też na: przełączenie MS na tryb **BT+BLE** + test z PC.
