#pragma once
/* obd — tabela standardowych PID (tryb 01) + parser odpowiedzi ELM. */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t pid;
    const char *name;
    const char *unit;
    uint8_t nbytes;                 /* liczba bajtow danych (>=1) */
    float (*decode)(const uint8_t *d);
} obd_pid_t;

const obd_pid_t *obd_find_pid(uint8_t pid);
const obd_pid_t *obd_pid_table(size_t *count);

/* Zbiera bajty z tekstu odpowiedzi ELM (pomija tokeny ID: 3/8 znakow hex,
 * prefiksy "0:", itp.). Zwraca liczbe zebranych bajtow. */
size_t obd_collect_bytes(const char *resp, uint8_t *out, size_t cap);

/* Parsuje odpowiedz na zadanie "01 XX": szuka bajtow 0x41 0xXX w (wielu) liniach,
 * dekoduje wartosc wg tabeli. raw/raw_len: surowe bajty danych (do logu).
 * Zwraca ESP_ERR_NOT_FOUND gdy "NO DATA" / brak odpowiedzi. */
esp_err_t obd_parse_response(const char *resp, uint8_t pid,
                             float *value, uint8_t *raw, size_t raw_cap,
                             size_t *raw_len);
#ifdef __cplusplus
}
#endif
