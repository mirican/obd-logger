/* miri-logger — logger OBD (ESP32-P4 + ESP32-C6 przez ESP-Hosted), Arduino.
 *
 * Port z firmware-idf; ten sam kontrakt logow: obd-logger/1
 * (JSONL sesji PID + raw candump + DTC). v0.1:
 *  - BLE central -> vLinker MS (tryb BT+BLE), ELM327, PID-y -> SD,
 *  - reconnect po serii bledow, VIN + migawka DTC na starcie sesji.
 *
 * Uwaga (P4): BLE dziala przez C6 (ESP-Hosted). Firmware C6 musi byc
 * hosted >= 2.6.0; inaczej BLE sie nie uruchomi (szczegoly: README).
 */
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <BLEDevice.h>

#include "src/elm327.h"
#include "src/obd.h"
#include "src/obdlog.h"
#include "src/obd_transport.h"

#define MIRI_VEHICLE        "auto"
#define MIRI_ADAPTER_NAME   "vLinker"
#define MIRI_PIDS           "010C,010D,0105,012F"
#define MIRI_INTERVAL_MS    200
#define MIRI_CMD_TIMEOUT_MS 3000
#define MIRI_LOG_DIR        "/sdcard/logs"
#define MIRI_ROTATE_KB      8192
#define MIRI_BT_SCAN_MS     8000
#define MIRI_DTC_ON_START   1
#define MIRI_VERBOSE        1

typedef struct {
    uint8_t pid;
    const char *name;
    const char *unit;
} pid_ent_t;

static obdlog_session_t *s_sess = NULL;

/* ---------- most elm327 <-> transport ---------- */

static esp_err_t elm_send_cb(void *ctx, const char *buf, size_t len)
{
    obd_transport_t *t = (obd_transport_t *)ctx;
    return t->send(t, buf, len);
}

static int elm_recv_cb(void *ctx, char *buf, size_t max, uint32_t timeout_ms)
{
    obd_transport_t *t = (obd_transport_t *)ctx;
    return t->recv(t, buf, max, timeout_ms);
}

/* ---------- lista PID ---------- */

static int parse_pids(const char *csv, pid_ent_t *out, int max)
{
    int n = 0;
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s", csv ? csv : "");
    char *save = NULL;
    for (char *t = strtok_r(tmp, ",", &save); t && n < max; t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ') {
            t++;
        }
        size_t tl = strlen(t);
        int pid = -1;
        if (tl == 2) {
            pid = (int)strtol(t, NULL, 16);
        } else if (tl == 4 && t[0] == '0' && t[1] == '1') {
            pid = (int)strtol(t + 2, NULL, 16);
        }
        if (pid < 0) {
            Serial.printf("[miri] PID '%s' - pomijam\n", t);
            continue;
        }
        const obd_pid_t *p = obd_find_pid((uint8_t)pid);
        out[n].pid = (uint8_t)pid;
        out[n].name = p ? p->name : "PID_UNKNOWN";
        out[n].unit = p ? p->unit : "";
        n++;
    }
    return n;
}

/* ---------- VIN ---------- */

static void read_vin(elm_handle_t *elm, char *out, size_t cap)
{
    char resp[512];
    char ack[8];
    out[0] = '\0';
    (void)elm_cmd(elm, "ATH0", ack, sizeof(ack), 3000);
    if (elm_cmd(elm, "0902", resp, sizeof(resp), 6000) == ESP_OK) {
        uint8_t bytes[64];
        size_t nb = obd_collect_bytes(resp, bytes, sizeof(bytes));
        for (size_t i = 0; i + 1 < nb; i++) {
            if (bytes[i] == 0x49 && bytes[i + 1] == 0x02) {
                size_t o = 0;
                for (size_t j = i + 2; j < nb && o + 1 < cap; j++) {
                    if (bytes[j] >= 0x20 && bytes[j] < 0x7F) {
                        out[o++] = (char)bytes[j];
                    }
                }
                out[o] = '\0';
                break;
            }
        }
    }
    (void)elm_cmd(elm, "ATH1", ack, sizeof(ack), 3000);
    Serial.printf("[miri] VIN: %s\n", out[0] ? out : "(brak)");
}

/* ---------- sesja ---------- */

static void open_session_once(elm_handle_t *elm)
{
    if (s_sess) {
        return;
    }
    obdlog_config_t lc;
    lc.vehicle = MIRI_VEHICLE;
    lc.adapter = "vLinker MS (BLE)";
    lc.pids = MIRI_PIDS;
    lc.dir = MIRI_LOG_DIR;
    lc.rotate_bytes = (size_t)MIRI_ROTATE_KB * 1024;
    if (obdlog_session_open(&s_sess, &lc) != ESP_OK) {
        Serial.printf("[miri] nie moge otworzyc sesji w %s (SD zamontowane?)\n", MIRI_LOG_DIR);
        return;
    }
    char vin[24];
    read_vin(elm, vin, sizeof(vin));
    if (vin[0]) {
        char extra[64];
        snprintf(extra, sizeof(extra), "{\"vin\":\"%s\"}", vin);
        obdlog_event(s_sess, obdlog_now_ms(), "vin", extra);
    }
#if MIRI_DTC_ON_START
    static char codes[16][8];
    size_t cnt = 0;
    if (elm_read_dtc(elm, codes, &cnt, 16) == ESP_OK) {
        const char *ptrs[16];
        for (size_t i = 0; i < cnt; i++) {
            ptrs[i] = codes[i];
        }
        obdlog_dtc_file(s_sess, ptrs, cnt);
        char extra[64];
        snprintf(extra, sizeof(extra), "{\"count\":%d}", (int)cnt);
        obdlog_event(s_sess, obdlog_now_ms(), "dtc_read", extra);
        Serial.printf("[miri] DTC: %d\n", (int)cnt);
    }
#endif
}

/* ---------- petla glowna ---------- */

static void logger_task(void *arg)
{
    (void)arg;
    pid_ent_t pids[16];
    int npids = parse_pids(MIRI_PIDS, pids, 16);
    Serial.printf("[miri] PID-ow w planie: %d\n", npids);

    for (;;) {
        obd_transport_t *tr = transport_ble_create(MIRI_ADAPTER_NAME, MIRI_BT_SCAN_MS);
        if (!tr) {
            Serial.println("[miri] BLE: brak polaczenia - retry za 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        elm_transport_t et;
        et.send = elm_send_cb;
        et.recv = elm_recv_cb;
        et.ctx = tr;
        elm_config_t ec;
        ec.cmd_timeout_ms = MIRI_CMD_TIMEOUT_MS;
        ec.init_timeout_ms = 8000;

        elm_handle_t *elm = elm_new(&et, &ec);
        if (!elm || elm_init(elm) != ESP_OK) {
            Serial.println("[miri] ELM init nieudany - retry za 5 s");
            if (elm) {
                elm_free(elm);
            }
            tr->destroy(tr);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        Serial.println("[miri] ELM gotowy");
        open_session_once(elm);
        if (s_sess) {
            obdlog_event(s_sess, obdlog_now_ms(), "link_up", NULL);
        }

        char resp[512];
        int errs = 0;
        int64_t next_round_us = esp_timer_get_time();

        while (errs < 5) {
            for (int i = 0; i < npids; i++) {
                char cmd[8];
                snprintf(cmd, sizeof(cmd), "01%02X", pids[i].pid);
                esp_err_t e = elm_obd_request(elm, cmd, resp, sizeof(resp));
                if (e == ESP_OK) {
                    uint8_t raw[8];
                    size_t rl = 0;
                    float v = 0;
                    e = obd_parse_response(resp, pids[i].pid, &v, raw, sizeof(raw), &rl);
                    if (e == ESP_OK) {
                        char rawh[24] = "";
                        for (size_t k = 0; k < rl; k++) {
                            snprintf(rawh + k * 2, 3, "%02X", raw[k]);
                        }
                        if (s_sess) {
                            obdlog_pid(s_sess, obdlog_now_ms(), cmd, pids[i].name,
                                       v, pids[i].unit, rawh);
                        }
#if MIRI_VERBOSE
                        Serial.printf("[pid] %s %s=%.2f %s\n", cmd, pids[i].name, v,
                                      pids[i].unit);
#endif
                        errs = 0;
                    } else {
                        errs++;
                        Serial.printf("[miri] %s: parse (%s)\n", cmd, esp_err_to_name(e));
                    }
                } else {
                    errs++;
                    Serial.printf("[miri] %s: %s\n", cmd, esp_err_to_name(e));
                }
                if (errs >= 5) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (errs >= 5 || !tr->is_connected(tr)) {
                break;
            }
            next_round_us += (int64_t)MIRI_INTERVAL_MS * 1000;
            int64_t wait_us = next_round_us - esp_timer_get_time();
            if (wait_us > 0) {
                vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
            } else {
                next_round_us = esp_timer_get_time();
            }
        }

        Serial.printf("[miri] link padl (errs=%d) - reconnect za 3 s\n", errs);
        if (s_sess) {
            obdlog_event(s_sess, obdlog_now_ms(), "link_lost", NULL);
        }
        elm_free(elm);
        tr->destroy(tr);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("=== miri-logger (Arduino / ESP32-P4+C6) ===");
    Serial.printf("SDK: %s\n", ESP.getSdkVersion());

    /* TODO(bring-up): hostedSetPins(...) dla Guition JC4880P443C_I_W (piny SDIO C6)
     * wywolac PRZED pierwsza inicjalizacja BLE; domyslnie jak w sdkconfig. */

    if (!BLEDevice::init("miri-logger")) {
        Serial.println("[miri] BLEDevice::init FAILED (C6 firmware hosted >= 2.6.0?)");
    }
    xTaskCreatePinnedToCore(logger_task, "miri_logger", 8192, NULL, 5, NULL, 0);
}

void loop()
{
    delay(2000);
}
