/* obdlog.c — implementacja zapisu sesji (JSONL + candump + DTC). */
#include "obdlog.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "obdlog";
static int64_t s_epoch_offset_ms = 0;

int64_t obdlog_now_ms(void)
{
    return s_epoch_offset_ms + esp_timer_get_time() / 1000;
}

void obdlog_set_epoch_ms(int64_t epoch_ms)
{
    s_epoch_offset_ms = epoch_ms - esp_timer_get_time() / 1000;
}

struct obdlog_session {
    char dir[96];
    char ts[32];
    char vehicle[48];
    char adapter[32];
    char pids[160];
    FILE *fjson;
    FILE *fraw;
    size_t written;
    size_t rotate_bytes;
    int part;
};

const char *obdlog_session_name(const obdlog_session_t *s)
{
    return s ? s->ts : "";
}

static void make_ts(char *b, size_t n)
{
    int64_t ms = obdlog_now_ms();
    time_t sec = (time_t)(ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    if (tmv.tm_year + 1900 >= 2020) {
        strftime(b, n, "%Y%m%d_%H%M%S", &tmv);
    } else {
        int64_t up = esp_timer_get_time() / 1000000;
        snprintf(b, n, "boot_%02d%02d%02d", (int)(up / 3600),
                 (int)((up / 60) % 60), (int)(up % 60));
    }
}

static cJSON *make_meta(const obdlog_session_t *s)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "kind", "meta");
    cJSON_AddStringToObject(o, "format", "obd-logger/1");
    cJSON_AddStringToObject(o, "vehicle", s->vehicle);
    cJSON_AddStringToObject(o, "adapter", s->adapter);
    cJSON_AddNumberToObject(o, "created", (double)obdlog_now_ms());
    cJSON_AddStringToObject(o, "tz", "Europe/Warsaw");
    cJSON *pids = cJSON_AddArrayToObject(o, "pids");
    if (pids) {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s", s->pids);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            cJSON_AddItemToArray(pids, cJSON_CreateString(tok));
        }
    }
    return o;
}

static esp_err_t write_json_line(obdlog_session_t *s, cJSON *o)
{
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!line) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = strlen(line);
    if (!s->fjson || fwrite(line, 1, n, s->fjson) != n) {
        cJSON_free(line);
        return ESP_FAIL;
    }
    fputc('\n', s->fjson);
    s->written += n + 1;
    cJSON_free(line);
    return ESP_OK;
}

static esp_err_t rotate_if_needed(obdlog_session_t *s)
{
    if (!s->rotate_bytes || s->written < s->rotate_bytes) {
        return ESP_OK;
    }
    fclose(s->fjson);
    s->fjson = NULL;
    s->part++;
    char path[192];
    snprintf(path, sizeof(path), "%s/obd_%s_p%d.jsonl", s->dir, s->ts, s->part);
    s->fjson = fopen(path, "a");
    if (!s->fjson) {
        ESP_LOGE(TAG, "rotacja: nie moge otworzyc %s", path);
        return ESP_FAIL;
    }
    s->written = 0;
    cJSON *meta = make_meta(s);
    if (meta) {
        return write_json_line(s, meta);
    }
    return ESP_OK;
}

esp_err_t obdlog_session_open(obdlog_session_t **out, const obdlog_config_t *cfg)
{
    if (!out || !cfg || !cfg->dir) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    obdlog_session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s->dir, sizeof(s->dir), "%s", cfg->dir);
    snprintf(s->vehicle, sizeof(s->vehicle), "%s", cfg->vehicle ? cfg->vehicle : "auto");
    snprintf(s->adapter, sizeof(s->adapter), "%s", cfg->adapter ? cfg->adapter : "elm327");
    snprintf(s->pids, sizeof(s->pids), "%s", cfg->pids ? cfg->pids : "");
    s->rotate_bytes = cfg->rotate_bytes;

    mkdir(s->dir, 0777); /* EEXIST ignorujemy */
    make_ts(s->ts, sizeof(s->ts));

    char path[192];
    snprintf(path, sizeof(path), "%s/obd_%s.jsonl", s->dir, s->ts);
    s->fjson = fopen(path, "a");
    if (!s->fjson) {
        ESP_LOGE(TAG, "nie moge otworzyc %s", path);
        free(s);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sesja: %s", path);

    cJSON *meta = make_meta(s);
    if (meta) {
        write_json_line(s, meta);
    }
    *out = s;
    return ESP_OK;
}

esp_err_t obdlog_pid(obdlog_session_t *s, int64_t t_ms, const char *pid,
                     const char *name, float value, const char *unit,
                     const char *raw_hex)
{
    if (!s || !s->fjson) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(o, "kind", "pid");
    cJSON_AddNumberToObject(o, "t", (double)t_ms);
    cJSON_AddStringToObject(o, "pid", pid ? pid : "?");
    cJSON_AddStringToObject(o, "name", name ? name : "?");
    cJSON_AddNumberToObject(o, "value", (double)value);
    cJSON_AddStringToObject(o, "unit", unit ? unit : "");
    if (raw_hex && raw_hex[0]) {
        cJSON_AddStringToObject(o, "raw", raw_hex);
    }
    esp_err_t err = write_json_line(s, o);
    if (err == ESP_OK) {
        err = rotate_if_needed(s);
    }
    return err;
}

esp_err_t obdlog_event(obdlog_session_t *s, int64_t t_ms, const char *event,
                       const char *extra_json)
{
    if (!s || !s->fjson) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(o, "kind", "event");
    cJSON_AddNumberToObject(o, "t", (double)t_ms);
    cJSON_AddStringToObject(o, "e", event ? event : "?");
    if (extra_json && extra_json[0] == '{') {
        cJSON *data = cJSON_Parse(extra_json);
        if (data) {
            cJSON_AddItemToObject(o, "data", data);
        }
    }
    return write_json_line(s, o);
}

esp_err_t obdlog_raw_frame(obdlog_session_t *s, uint32_t can_id, bool extended,
                           const uint8_t *data, size_t len)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s->fraw) {
        char path[192];
        snprintf(path, sizeof(path), "%s/obd_raw_%s.log", s->dir, s->ts);
        s->fraw = fopen(path, "a");
        if (!s->fraw) {
            return ESP_FAIL;
        }
        fputs("# obd-logger raw (candump -L)\n", s->fraw);
    }
    if (len > 8) {
        len = 8;
    }
    int64_t ms = obdlog_now_ms();
    char line[96];
    int n = snprintf(line, sizeof(line), "(%lld.%06ld) can0 %0*X#",
                     (long long)(ms / 1000), (long)((ms % 1000) * 1000),
                     extended ? 8 : 3, (unsigned)can_id);
    for (size_t i = 0; i < len; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%02X", data[i]);
    }
    fwrite(line, 1, (size_t)n, s->fraw);
    fputc('\n', s->fraw);
    return ESP_OK;
}

esp_err_t obdlog_dtc_file(obdlog_session_t *s, const char *const *codes,
                          size_t count)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(o, "format", "obd-logger/1");
    cJSON_AddNumberToObject(o, "t", (double)obdlog_now_ms());
    cJSON_AddStringToObject(o, "vehicle", s->vehicle);
    cJSON *arr = cJSON_AddArrayToObject(o, "dtcs");
    for (size_t i = 0; i < count && arr; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "code", codes[i]);
        cJSON_AddItemToArray(arr, d);
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/dtc_%s.json", s->dir, s->ts);
    FILE *f = fopen(path, "w");
    esp_err_t err = ESP_FAIL;
    if (f) {
        fputs(txt, f);
        fputc('\n', f);
        fclose(f);
        err = ESP_OK;
    }
    cJSON_free(txt);
    return err;
}

void obdlog_session_close(obdlog_session_t *s)
{
    if (!s) {
        return;
    }
    if (s->fjson) {
        fclose(s->fjson);
    }
    if (s->fraw) {
        fclose(s->fraw);
    }
    free(s);
}
