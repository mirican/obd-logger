/* elm327.c — implementacja sterownika ELM327. */
#include "elm327.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "elm327";

#define ELM_BUF 1024

struct elm_handle {
    elm_transport_t t;
    elm_config_t cfg;
    char last[ELM_BUF];
};

elm_handle_t *elm_new(const elm_transport_t *transport, const elm_config_t *config)
{
    if (!transport || !transport->send || !transport->recv) {
        return NULL;
    }
    elm_handle_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return NULL;
    }
    h->t = *transport;
    h->cfg.cmd_timeout_ms = (config && config->cmd_timeout_ms) ? config->cmd_timeout_ms : 3000;
    h->cfg.init_timeout_ms = (config && config->init_timeout_ms) ? config->init_timeout_ms : 8000;
    return h;
}

void elm_free(elm_handle_t *h)
{
    free(h);
}

const char *elm_last_response(const elm_handle_t *h)
{
    return h ? h->last : "";
}

static void drain(elm_handle_t *h)
{
    char tmp[64];
    for (int i = 0; i < 64; i++) {
        int r = h->t.recv(h->t.ctx, tmp, sizeof(tmp), 0);
        if (r <= 0) {
            break;
        }
    }
}

esp_err_t elm_cmd(elm_handle_t *h, const char *cmd, char *out, size_t out_len,
                  uint32_t timeout_ms)
{
    if (!h || !cmd || !out || out_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    drain(h);

    char line[192];
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    if (n <= 0 || n >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = h->t.send(h->t.ctx, line, (size_t)n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send '%s' blad %d", cmd, err);
        return err;
    }

    /* Zbieraj az do promptu '>' (albo timeout). CR -> LF. */
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t o = 0;
    bool prompt = false;
    char chunk[128];
    while (!prompt) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            break;
        }
        uint32_t left_ms = (uint32_t)(left_us / 1000);
        int r = h->t.recv(h->t.ctx, chunk, sizeof(chunk), left_ms);
        if (r < 0) {
            return ESP_FAIL;
        }
        if (r == 0) {
            continue;
        }
        for (int i = 0; i < r; i++) {
            char c = chunk[i];
            if (c == '>') {
                prompt = true;
                break;
            }
            if (c == '\r') {
                c = '\n';
            }
            if (o + 1 < out_len) {
                out[o++] = c;
            }
        }
    }
    out[o] = '\0';
    /* przytnij wiodace/kocowe biale znaki */
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == ' ')) {
        out[--len] = '\0';
    }
    size_t start = 0;
    while (out[start] == '\n' || out[start] == ' ') {
        start++;
    }
    if (start > 0) {
        memmove(out, out + start, len - start + 1);
    }
    snprintf(h->last, sizeof(h->last), "%s", out);

    if (!prompt) {
        ESP_LOGW(TAG, "'%s' -> timeout (brak '>'), czesc: %.60s", cmd, out);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t elm_init(elm_handle_t *h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    char resp[ELM_BUF];

    /* ATZ: reset (niektore adaptery wysylaja smieci/echo przed promptem) */
    (void)elm_cmd(h, "ATZ", resp, sizeof(resp), h->cfg.init_timeout_ms);
    /* Echo off, LF off, spacje off, naglowki on, adaptive timing, auto-protokol */
    const char *seq[] = { "ATE0", "ATL0", "ATS0", "ATH1", "ATAT1", "ATSP0" };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        esp_err_t err = elm_cmd(h, seq[i], resp, sizeof(resp), h->cfg.cmd_timeout_ms);
        bool ack = (strstr(resp, "OK") != NULL) || (strstr(resp, "?") == NULL && err == ESP_OK);
        ESP_LOGI(TAG, "init %s -> %s", seq[i], ack ? "OK" : resp);
        if (!ack) {
            ESP_LOGW(TAG, "init: brak OK dla %s", seq[i]);
        }
    }

    /* Probe: 0100 (supported PIDs) — moze trwac dlugo (SEARCHING...) */
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_err_t err = elm_cmd(h, "0100", resp, sizeof(resp), h->cfg.init_timeout_ms);
        if (err == ESP_OK && strstr(resp, "41 00") != NULL) {
            ESP_LOGI(TAG, "probe 0100 OK");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "probe 0100 (proba %d): %s", attempt + 1, resp);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t elm_obd_request(elm_handle_t *h, const char *svc_pid, char *out,
                          size_t out_len)
{
    return elm_cmd(h, svc_pid, out, out_len, h->cfg.cmd_timeout_ms);
}

esp_err_t elm_read_dtc(elm_handle_t *h, char codes[][8], size_t *count,
                       size_t max_codes)
{
    char resp[ELM_BUF];
    char ack[8];
    *count = 0;

    /* Czytamy z wylaczonymi naglowkami (ATH0) — odpowiedz to czyste bajty:
     * "43 01 23 45 00 ...". Potem przywracamy ATH1. */
    (void)elm_cmd(h, "ATH0", ack, sizeof(ack), h->cfg.cmd_timeout_ms);
    esp_err_t err = elm_cmd(h, "03", resp, sizeof(resp), h->cfg.cmd_timeout_ms + 2000);
    (void)elm_cmd(h, "ATH1", ack, sizeof(ack), h->cfg.cmd_timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (strstr(resp, "NO DATA") != NULL) {
        return ESP_OK; /* brak DTC = sukces */
    }

    int vals[32];
    int nv = 0;
    const char *q = resp;
    while (*q && nv < 32) {
        while (*q == ' ' || *q == '\n' || *q == '\t' || *q == ',') {
            q++;
        }
        if (!*q) {
            break;
        }
        char tok[3] = { 0, 0, 0 };
        int t = 0;
        while (*q && *q != ' ' && *q != '\n' && *q != '\t' && *q != ',' && t < 2) {
            tok[t++] = *q++;
        }
        while (*q && *q != ' ' && *q != '\n' && *q != '\t' && *q != ',') {
            q++; /* pomin ogon dluzszego tokenu */
        }
        if (t == 2) {
            vals[nv++] = (int)strtol(tok, NULL, 16);
        }
    }
    if (nv < 3 || vals[0] != 0x43) {
        return ESP_OK;
    }
    for (int i = 1; i + 1 < nv && *count < max_codes; i += 2) {
        snprintf(codes[*count], 8, "%02X%02X", vals[i], vals[i + 1]);
        (*count)++;
    }
    return ESP_OK;
}
