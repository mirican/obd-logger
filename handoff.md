# HANDOFF — obd-logger (logger OBD), stan 2026-10-07

Przewodnik dla nowego komputera/czatu. Najpierw ten plik, potem `../can-toolkit/handoff.md` (część miriCAN).

## 1. Co to jest

Osobny projekt: **logger OBD** na panelu Guition 4.3" (ESP32-P4 + ESP32-C6), dane z auta przez
**vLinker MS** (Bluetooth). Łańcuch:
`auto > OBD > vLinker MS > BLE > ESP32 (C6 = radio, P4 = logika/logi) > SD > WiFi > miriCAN Studio`.

## 2. Decyzje (zamknięte)

- Firmware: **Arduino** (esp32 core 3.3.11), NIE ESP-IDF (stary szkielet: `firmware-idf/` — parked, referencja).
- Logi na **SD**; zakres **ogólne OBD** (nie tylko BMW); integracja z miriCAN od razu.
- vLinker = wyłącznie interfejs do magistrali; BLE central: P4 przez C6 (ESP-Hosted, wbudowane w core Arduino).
- vLinker MS: tryb przełączany „MFI" → **„BT + BLE"** (VgateFwUpdater) — wymagane, żeby BLE działało.

## 3. Struktura projektu

- `arduino/miri-logger/` — firmware (szkic): `miri-logger.ino` + `src/` (`elm327.c`, `obd_pids.c`, `obd_parse.c`, `obdlog.c`, `transport_ble.cpp`, `transport_uart.cpp`); README w środku.
- `pc/bleak_vlinker_test.py` — test adaptera z PC (skan BLE / connect / `ATZ`/`0100`); skan przetestowany.
- `docs/integracja-mirican.md` — **kontrakt logów**: `obd_*.jsonl` (meta+pid+event), `obd_raw_*.log` (candump -L), `dtc_*.json`; eksport WiFi: `/manifest.json`, `/files/<n>`, mDNS `obd-logger.local`.
- `firmware-idf/` — PARKED (referencja; nie rozwijać).
- Ocena wykonalności całości: `../can-toolkit/docs/11-logger.md`.

## 4. Stan (zrobione / brakuje)

Zrobione:
- analiza wykonalności (docs/11) + kontrakt logów,
- skrypt testowy BLE z PC (`bleak`) — działa skan; pełny test czeka na vLinkera,
- szkic Arduino **kompiluje się** pod `esp32:esp32:esp32p4` (65% flash, 0 ostrzeżeń).

Brakuje (bring-up na sprzęcie):
- **firmware C6 (esp_hosted) ≥ 2.6.0** — bez tego BLE nie ruszy. Update: API w core (`hostedBeginUpdate/WriteUpdate/EndUpdate/ActivateUpdate`) albo esptool,
- piny **SDIO C6↔P4** (`hostedSetPins`) i piny **SD** — do wzięcia z wiki/demo Guition (JC4880P443C_I_W),
- LVGL UI + eksport WiFi (kolejne etapy).

## 5. Nowy komputer — setup

1. **Arduino IDE 2.3.10** lub arduino-cli; rdzeń: `esp32:esp32` **wersja 3.3.11**
   (`arduino-cli core install esp32:esp32@3.3.11`).
2. **Python** (komenda `py`): `py -m pip install bleak` (test vLinkera z PC).
3. Do analizy logów w miriCAN: patrz `../can-toolkit/handoff.md` §3 (python-can, cantools, fastapi…).
4. Build szkicu: `arduino-cli compile --fqbn esp32:esp32:esp32p4 obd-logger/arduino/miri-logger`
5. Test z PC: `py obd-logger/pc/bleak_vlinker_test.py` (MS w trybie BT+BLE, telefon rozłączony).

## 6. Następne kroki (kolejność)

1. vLinker w ręku → przełącz na BT+BLE → test z PC → zanotować GATT (usługi/charakterystyki).
2. Płytka → bring-up: C6 firmware ≥ 2.6.0; piny SDIO/SD; `BLEDevice::init()` → pierwszy `ATZ`/`0100`.
3. Pierwsza sesja na aucie (tylko na postoju!): logi na SD → skopiować do `can-toolkit/logs/` → analiza.
4. LVGL UI (ekran: status linku, PID-y, start/stop sesji).
5. WiFi: SNTP + eksport HTTP wg kontraktu (manifest + pliki).
6. Opcjonalnie: narzędzie `canstudio obdlog` (fetch/parse) po stronie miriCAN.

## 7. Pułapki

- BLE = **jedno połączenie naraz** (jeśli telefon trzyma vLinkera, P4 nie wejdzie). Skan nie może trwać w nieskończoność (bug raportów po ~90 s).
- Na płytce P4 bywają 2× USB (USB-Serial-JTAG + OTG) — driver/wybór portu wg wiki Guition.
- Nazwy plików logów i format — trzymać się kontraktu (`docs/integracja-mirican.md`); nie zmieniać bez podbicia wersji formatu.
- Nie mieszać `python` (AutoClaw) z `py` (użytkownika).
- Piny w `.ino` są domyślne — potwierdzić na płytce; **SDIO C6 to warunek BLE**.

## 8. Sprzęt

- Panel Guition **JC4880P443C_I_W** (ESP32-P4 + C6), microSD,
- **vLinker MS** (BT+BLE),
- do części miriCAN: **MKS CANable 2.0 PRO** (COMx).
