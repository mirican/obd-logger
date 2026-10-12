#pragma once
/* elm327 — prosty sterownik adaptera ELM327 (vLinker i klony).
 * Transport-agnostyczny: dostajesz send/recv, reszta to tekst + prompt '>'.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct elm_handle elm_handle_t;

typedef struct {
    /* Wysyla bajty do adaptera. 0 = ok. */
    esp_err_t (*send)(void *ctx, const char *buf, size_t len);
    /* Blokuje do timeout_ms; zwraca liczbe bajtow (>0), 0 = brak, -1 = blad. */
    int (*recv)(void *ctx, char *buf, size_t max, uint32_t timeout_ms);
    void *ctx;
} elm_transport_t;

typedef struct {
    uint32_t cmd_timeout_ms;   /* domyslnie 3000 */
    uint32_t init_timeout_ms;  /* ATZ — domyslnie 8000 */
} elm_config_t;

elm_handle_t *elm_new(const elm_transport_t *transport, const elm_config_t *config);
void elm_free(elm_handle_t *h);

/* Pojedyncze polecenie: dopisuje CR, czeka na '>', normalizuje CR->LF.
 * out: tekst odpowiedzi bez promptu (NUL-terminowany). */
esp_err_t elm_cmd(elm_handle_t *h, const char *cmd, char *out, size_t out_len,
                  uint32_t timeout_ms);

/* Sekwencja startowa: ATZ, ATE0, ATL0, ATS0, ATH1, ATAT1, ATSP0, potem probe 0100. */
esp_err_t elm_init(elm_handle_t *h);

/* Zadanie OBD (np. "010C"); zwraca surowy tekst odpowiedzi. */
esp_err_t elm_obd_request(elm_handle_t *h, const char *svc_pid, char *out,
                          size_t out_len);

/* Odczyt DTC (tryb 03): wpisuje kody hex "XXXXXX" (bez prefiksu P/C/B/U). */
esp_err_t elm_read_dtc(elm_handle_t *h, char codes[][8], size_t *count,
                       size_t max_codes);

/* Ostatnia odpowiedz — do logow/diagnostyki. */
const char *elm_last_response(const elm_handle_t *h);

#ifdef __cplusplus
}
#endif
