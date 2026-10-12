#pragma once
/* obdlog — zapis sesji wg kontraktu obd-logger/1:
 *  - obd_<ts>.jsonl      (meta + pid + event)
 *  - obd_raw_<ts>.log    (candump -L, best effort)
 *  - dtc_<ts>.json       (migawka DTC)
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obdlog_session obdlog_session_t;

typedef struct {
    const char *vehicle;
    const char *adapter;
    const char *pids;          /* "010C,010D,..." (do meta) */
    const char *dir;           /* katalog docelowy (np. /sdcard/logs) */
    size_t rotate_bytes;       /* 0 = bez rotacji */
} obdlog_config_t;

esp_err_t obdlog_session_open(obdlog_session_t **out, const obdlog_config_t *cfg);
void obdlog_session_close(obdlog_session_t *s);
const char *obdlog_session_name(const obdlog_session_t *s);

esp_err_t obdlog_pid(obdlog_session_t *s, int64_t t_ms, const char *pid,
                     const char *name, float value, const char *unit,
                     const char *raw_hex /* nullable */);
esp_err_t obdlog_event(obdlog_session_t *s, int64_t t_ms, const char *event,
                       const char *extra_json /* nullable, obiekt */);
esp_err_t obdlog_raw_frame(obdlog_session_t *s, uint32_t can_id, bool extended,
                           const uint8_t *data, size_t len);
esp_err_t obdlog_dtc_file(obdlog_session_t *s, const char *const *codes,
                          size_t count);

/* Czas: epoch ms; bez synchronizacji NTP cofa sie do czasu od startu. */
int64_t obdlog_now_ms(void);
void obdlog_set_epoch_ms(int64_t epoch_ms);

#ifdef __cplusplus
}
#endif
