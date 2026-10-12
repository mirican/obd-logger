/* main.c — obd-logger v0 (szkielet): sesja logowania PID -> SD (kontrakt obd-logger/1).
 *
 * Schemat: transport (BLE/UART) -> ELM327 -> scheduler PID -> obdlog (JSONL + raw).
 * Reconnect: po serii bledow zamykamy transport i wracamy do petli glownej.
 * TODO (kolejne kroki): esp_hosted (WiFi/BT na C6), eksport HTTP, LVGL UI, SNTP.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "elm327.h"
#include "obd.h"
#include "obdlog.h"
#include "obd_transport.h"

static const char *TAG = "obd-logger";
static sdmmc_card_t *s_card;

/* ---------- SD ---------- */

static esp_err_t mount_sd(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    if (CONFIG_OBDLOG_SD_CLK_PIN >= 0) {
        slot.clk = CONFIG_OBDLOG_SD_CLK_PIN;
    }
    if (CONFIG_OBDLOG_SD_CMD_PIN >= 0) {
        slot.cmd = CONFIG_OBDLOG_SD_CMD_PIN;
    }
    if (CONFIG_OBDLOG_SD_D0_PIN >= 0) {
        slot.d0 = CONFIG_OBDLOG_SD_D0_PIN;
    }
    slot.width = 1;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "TODO: ustaw piny SD w Kconfig (wez z demo Guition JC4880P443)");
    } else {
        ESP_LOGI(TAG, "SD ok (%s)", s_card ? s_card->cid.name : "?");
    }
    return err;
}

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

static obd_transport_t *make_transport(void)
{
#if CONFIG_OBDLOG_USE_BT
    {
        obd_transport_t *t = transport_ble_create(CONFIG_OBDLOG_ADAPTER_NAME,
                                                  CONFIG_OBDLOG_BT_SCAN_MS);
        if (t) {
            return t;
        }
        ESP_LOGW(TAG, "BLE: brak polaczenia z %s", CONFIG_OBDLOG_ADAPTER_NAME);
    }
#endif
#if CONFIG_OBDLOG_USE_UART
    return transport_uart_create(CONFIG_OBDLOG_UART_NUM, CONFIG_OBDLOG_UART_TX_PIN,
                                 CONFIG_OBDLOG_UART_RX_PIN, CONFIG_OBDLOG_UART_BAUD);
#endif
    return NULL;
}

/* ---------- lista PID z Kconfig ---------- */

typedef struct {
    uint8_t pid;
    const char *name;
    const char *unit;
} pid_ent_t;

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
            ESP_LOGW(TAG, "PID '%s' — pomijam", t);
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
    ESP_LOGI(TAG, "VIN: %s", out[0] ? out : "(brak)");
}

/* ---------- sesja ---------- */

static obdlog_session_t *s_sess;

static void open_session_once(elm_handle_t *elm)
{
    if (s_sess) {
        return;
    }
#if CONFIG_OBDLOG_USE_BT
    const char *adapter_name = "vLinker MS (BLE)";
#else
    const char *adapter_name = "ELM327 (UART)";
#endif
    obdlog_config_t lc = {
        .vehicle = CONFIG_OBDLOG_VEHICLE,
        .adapter = adapter_name,
        .pids = CONFIG_OBDLOG_PIDS,
        .dir = CONFIG_OBDLOG_LOG_DIR,
        .rotate_bytes = (size_t)CONFIG_OBDLOG_ROTATE_KB * 1024,
    };
    if (obdlog_session_open(&s_sess, &lc) != ESP_OK) {
        ESP_LOGE(TAG, "nie moge otworzyc sesji w %s", CONFIG_OBDLOG_LOG_DIR);
        return;
    }
    char vin[24];
    read_vin(elm, vin, sizeof(vin));
    if (vin[0]) {
        char extra[64];
        snprintf(extra, sizeof(extra), "{\"vin\":\"%s\"}", vin);
        obdlog_event(s_sess, obdlog_now_ms(), "vin", extra);
    }
#if CONFIG_OBDLOG_DTC_ON_START
    {
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
            ESP_LOGI(TAG, "DTC: %d", (int)cnt);
        }
    }
#endif
}

/* ---------- petla glowna ---------- */

void app_main(void)
{
    ESP_LOGI(TAG, "obd-logger v0 — kontrakt obd-logger/1");
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    mount_sd();

    pid_ent_t pids[16];
    int npids = parse_pids(CONFIG_OBDLOG_PIDS, pids, 16);
    ESP_LOGI(TAG, "PID-ow w planie: %d", npids);

    for (;;) {
        obd_transport_t *tr = make_transport();
        if (!tr) {
            ESP_LOGW(TAG, "transport niedostepny — retry za 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        elm_transport_t et = { .send = elm_send_cb, .recv = elm_recv_cb, .ctx = tr };
        elm_config_t ec = { .cmd_timeout_ms = CONFIG_OBDLOG_CMD_TIMEOUT_MS,
                            .init_timeout_ms = 8000 };
        elm_handle_t *elm = elm_new(&et, &ec);
        if (!elm || elm_init(elm) != ESP_OK) {
            ESP_LOGE(TAG, "ELM init nieudany — retry za 5 s");
            if (elm) {
                elm_free(elm);
            }
            tr->destroy(tr);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(TAG, "ELM gotowy");
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
                        errs = 0;
                    } else {
                        errs++;
                        ESP_LOGW(TAG, "%s: parse (%s)", cmd, esp_err_to_name(e));
                    }
                } else {
                    errs++;
                    ESP_LOGW(TAG, "%s: %s", cmd, esp_err_to_name(e));
                }
                if (errs >= 5) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (errs >= 5 || !tr->is_connected(tr)) {
                break;
            }
            next_round_us += (int64_t)CONFIG_OBDLOG_INTERVAL_MS * 1000;
            int64_t wait_us = next_round_us - esp_timer_get_time();
            if (wait_us > 0) {
                vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
            } else {
                next_round_us = esp_timer_get_time();
            }
        }

        ESP_LOGW(TAG, "link padl (errs=%d) — reconnect za 3 s", errs);
        if (s_sess) {
            obdlog_event(s_sess, obdlog_now_ms(), "link_lost", NULL);
        }
        elm_free(elm);
        tr->destroy(tr);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
