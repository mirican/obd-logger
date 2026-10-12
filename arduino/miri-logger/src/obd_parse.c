/* obd_parse.c — parser odpowiedzi ELM dla zadania "01 XX". */
#include "obd.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

/* Zbiera bajty z tekstu odpowiedzi: pomija tokeny ID (3 lub 8 znakow hex),
 * zwraca liczbe zebranych bajtow. */
size_t obd_collect_bytes(const char *resp, uint8_t *out, size_t cap)
{
    int n = 0;
    const char *q = resp;
    while (*q && (size_t)n < cap) {
        while (*q == ' ' || *q == '\n' || *q == '\t' || *q == ',') {
            q++;
        }
        if (!*q) {
            break;
        }
        const char *tok = q;
        while (*q && *q != ' ' && *q != '\n' && *q != '\t' && *q != ',') {
            q++;
        }
        size_t tl = (size_t)(q - tok);
        if (tl == 3 || tl == 8) {
            /* prawdopodobnie ID ramki (7E8, 18DAF110) — pomin */
            bool allhex = true;
            for (size_t i = 0; i < tl; i++) {
                char c = tok[i];
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                      (c >= 'a' && c <= 'f'))) {
                    allhex = false;
                    break;
                }
            }
            if (allhex) {
                continue;
            }
        }
        if (tl == 1 || tl == 2) {
            char tmp[3] = { tok[0], tl == 2 ? tok[1] : '\0', 0 };
            out[n++] = (uint8_t)strtol(tmp, NULL, 16);
        }
    }
    return n;
}

esp_err_t obd_parse_response(const char *resp, uint8_t pid,
                             float *value, uint8_t *raw, size_t raw_cap,
                             size_t *raw_len)
{
    if (raw_len) {
        *raw_len = 0;
    }
    if (!resp) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strstr(resp, "NO DATA") || strstr(resp, "UNABLE TO CONNECT") ||
        strstr(resp, "CAN ERROR")) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t bytes[64];
    size_t n = obd_collect_bytes(resp, bytes, sizeof(bytes));
    /* szukaj pary 0x41,0xXX */
    for (size_t i = 0; i + 1 < n; i++) {
        if (bytes[i] == 0x41 && bytes[i + 1] == pid) {
            const uint8_t *data = &bytes[i + 2];
            int avail = (int)n - (int)(i + 2);
            const obd_pid_t *p = obd_find_pid(pid);
            size_t want = p ? p->nbytes : 0;
            if (want == 0 || avail < (int)want) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (raw && raw_cap >= want) {
                memcpy(raw, data, want);
            }
            if (raw_len) {
                *raw_len = want;
            }
            if (value) {
                *value = p->decode(data);
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
