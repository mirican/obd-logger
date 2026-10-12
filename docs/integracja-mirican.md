# Kontrakt integracji: obd-logger ↔ miriCAN Studio (szkic, 2026-10-07)

Cel: logi z loggera (ESP32-P4) są **od razu czytelne** dla miriCAN i agenta —
bez ręcznej obróbki. Trzy formaty plików + eksport przez WiFi.

## 1. Log sesji PID — `obd_<YYYYmmdd_HHMMSS>.jsonl` (główny)

Jedna linia = jeden rekord (UTF-8, `\n`), epoch ms:

```json
{"kind":"meta","format":"obd-logger/1","vehicle":"BMW F30","adapter":"vLinker MS",
 "created":1791403000000,"tz":"Europe/Warsaw","pids":["010C","010D","012F"]}
{"kind":"pid","t":1791403000123,"pid":"010C","name":"EngineRPM","value":812.5,"unit":"rpm"}
{"kind":"event","t":1791403005000,"e":"dtc_read","data":{"count":2,"dtcs":[["123456","24"],["ABC001","2F"]]}}
{"kind":"event","t":1791403010000,"e":"mark","label":"przycisk"}
```

- `t` = epoch w ms (NTP gdy WiFi; bez NTP — czas lokalny od startu + offset).
- `kind`: `meta` (pierwsza linia sesji), `pid`, `event` (`dtc_read`, `mark`, `note`, `error`).
- Nazwy/ jednostki z profilu PID (ogólne OBD + rozszerzalne per pojazd).
- Podział pliku przy ~8 MB: kolejne `obd_<ts>_p2.jsonl` z własnym `meta`.

## 2. Surowy zapis magistrali — `obd_raw_<ts>.log` (opcjonalny, „candump -L")

Jeśli uda się wyciągnąć ramki z trybu monitor (ATMA / tryb sniffera vLinkera):

```
(1791403000.123456) can0 130#034B107780EF7232
(1791403000.143210) can0 1A0#114B14E440000000
```

- **Celowo format candump** — nasze istniejące narzędzia (stats / discover /
  correlate / autobc / diff) czytają go od razu, bez zmian.
- Uwaga: przez ELM327 monitor ma ograniczenia (straty przy dużym obciążeniu);
  to log „best effort", nie pełny sniff (do pełnego — Canable).

## 3. Migawka DTC — `dtc_<ts>.json`

```json
{"format":"obd-logger/1","t":1791403005000,"vehicle":"BMW F30",
 "dtcs":[{"code":"123456","status":"24","text":"..."}]}
```

## 4. Eksport przez WiFi (HTTP + mDNS)

ESP32 serwuje:

- `GET /manifest.json` → `{"files":[{"name":"obd_....jsonl","size":12345,"mtime":...}]}`
- `GET /files/<name>` → treść pliku
- (opcjonalnie) `POST /files/<name>?delete=1` — kasowanie po pobraniu

mDNS: `obd-logger.local` (fallback: IP z LCD).

## 5. Ścieżka do Studia / agenta

1. Użytkownik: przeglądarka → pobierz plik do `can-toolkit/logs/`.
2. Agent/CLI (przyszłość, małe): `canstudio obdlog fetch --url http://obd-logger.local`
   i `obdlog parse` (statystyki sesji PID, wykresy przez agenta; raw → narzędzia istniejące).
3. Bez terminala: logger może po WiFi wypchnąć pliki na wskazany endpoint (opcja).

## 6. Ustalenia techniczne

- Czas: epoch ms (spójne z GUI/Studio); przy braku NTP — offset aktualizowany, gdy WiFi.
- Nazwy plików: tylko ASCII `[a-z0-9_.-]`.
- Kompatybilność: nie zmieniamy `format` bez podniesienia wersji (`obd-logger/2`).
- Raw w candump = **most do miriCAN od pierwszego dnia**; JSONL = warstwa analityczna.

## 7. Do decyzji przy firmware

- Czy zapisywać PID-y „ciągle" (np. 5–10 Hz wybrane) czy tylko na żądanie z UI?
- Czy dołączać surowe odpowiedzi ELM (`"raw":"41 0C 1A F8"`) w liniach PID? (proponuję: tak, pole `raw`)
- Czy logger ma umieć wykonać „replay żądań" (lista PID z pliku)? (nice-to-have)
