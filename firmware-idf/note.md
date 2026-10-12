# PARKED — nie rozwijamy tego katalogu

Decyzja użytkownika (2026-10-07): firmware płytki piszemy w **Arduino** (esp32 core 3.3.11).

- Aktywny kod: `../arduino/miri-logger/` (ten sam kontrakt logów `obd-logger/1`).
- Ten katalog (`firmware-idf/`) zostaje wyłącznie jako **referencja** — logika
  (elm327/obd/obdlog) została przeniesiona 1:1 do szkicu Arduino.
