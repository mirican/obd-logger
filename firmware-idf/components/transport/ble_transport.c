/* ble_transport.c — BLE central (NimBLE) do adaptera OBD (vLinker MS w trybie BT+BLE).
 *
 * Strategia: skan po prefiksie nazwy -> connect -> discovery uslug ->
 * wybor charakterystyki write (preferuj 0xFFF1) i notify (preferuj 0xFFF2),
 * fallback: pierwsza lepsza para write+notify w dowolnej usludze (klony).
 * Subskrypcja przez zapis CCCD (0x2902). RX do pierscienia; send = write bez odpowiedzi.
 */
#include "obd_transport.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

static const char *TAG = "tr_ble";

#define RXBUF 2048
#define MAX_SVCS 8
#define MAX_CHRS 32

typedef struct {
    uint16_t def;
    uint16_t val;
    uint8_t props;
    uint16_t uuid16;
    uint16_t end;
} chr_rec_t;

typedef struct {
    obd_transport_t base;

    SemaphoreHandle_t rx_mtx;
    volatile size_t rx_w, rx_r;
    uint8_t rx[RXBUF];

    volatile bool ready;
    volatile bool abort;
    uint16_t conn;
    uint16_t write_handle;
    uint16_t notify_handle;
    uint16_t cccd_handle;

    char prefix[32];

    SemaphoreHandle_t setup_sem;   /* setup zakonczony */
    SemaphoreHandle_t disc_sem;    /* discovery: operacja zakonczona */
    SemaphoreHandle_t conn_sem;    /* connect: wynik */
    int conn_status;

    ble_addr_t found;
    volatile bool found_flag;

    struct { uint16_t start, end; } svcs[MAX_SVCS];
    int nsvc;
    chr_rec_t chrs[MAX_CHRS];
    int nchr;
    uint16_t cccd_found;
} ble_t;

static ble_t s_ble;
static bool s_nimble_inited;
static SemaphoreHandle_t s_sync_sem;
static uint8_t s_own_addr_type;

/* ---------- RX ring ---------- */

static void push_rx(const uint8_t *d, size_t n)
{
    if (!s_ble.rx_mtx) {
        return;
    }
    if (xSemaphoreTake(s_ble.rx_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        size_t nw = (s_ble.rx_w + 1) % RXBUF;
        if (nw == s_ble.rx_r) {
            break; /* pelny */
        }
        s_ble.rx[s_ble.rx_w] = d[i];
        s_ble.rx_w = nw;
    }
    xSemaphoreGive(s_ble.rx_mtx);
}

/* ---------- NimBLE init ---------- */

static void on_sync(void)
{
    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}

static bool ensure_nimble(void)
{
    if (s_nimble_inited) {
        return true;
    }
    s_sync_sem = xSemaphoreCreateBinary();
    if (!s_sync_sem) {
        return false;
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init blad");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "brak sync NimBLE");
        return false;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "nie moge ustalic adresu wlasnego");
        return false;
    }
    s_nimble_inited = true;
    return true;
}

/* ---------- callbacki GAP ---------- */

static bool name_matches(const uint8_t *data, uint8_t len)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    if (ble_hs_adv_parse_fields(&f, data, len) != 0) {
        return false;
    }
    if (!f.name || f.name_len == 0) {
        return false;
    }
    size_t pl = strlen(s_ble.prefix);
    if (f.name_len < pl) {
        return false;
    }
    return strncasecmp((const char *)f.name, s_ble.prefix, pl) == 0;
}

static int gap_event(struct ble_gap_event *event, void *arg);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!s_ble.found_flag && name_matches(event->disc.data, event->disc.length_data)) {
            s_ble.found = event->disc.addr;
            s_ble.found_flag = true;
            ESP_LOGI(TAG, "znaleziono: %02X:%02X:%02X:%02X:%02X:%02X",
                     s_ble.found.val[5], s_ble.found.val[4], s_ble.found.val[3],
                     s_ble.found.val[2], s_ble.found.val[1], s_ble.found.val[0]);
            ble_gap_disc_cancel();
        }
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_ble.disc_sem) {
            xSemaphoreGive(s_ble.disc_sem);
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        s_ble.conn_status = event->connect.status;
        if (event->connect.status == 0) {
            s_ble.conn = event->connect.conn_handle;
        }
        if (s_ble.conn_sem) {
            xSemaphoreGive(s_ble.conn_sem);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "rozlaczony (reason=%d)", event->disconnect.reason);
        s_ble.ready = false;
        s_ble.conn = BLE_HS_CONN_HANDLE_NONE;
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle == s_ble.notify_handle) {
            uint16_t om_len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t tmp[256];
            uint16_t cl = om_len > sizeof(tmp) ? (uint16_t)sizeof(tmp) : om_len;
            os_mbuf_copydata(event->notify_rx.om, 0, cl, tmp);
            push_rx(tmp, cl);
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* ---------- callbacki discovery ---------- */

static void clear_sem(SemaphoreHandle_t sem)
{
    while (sem && xSemaphoreTake(sem, 0) == pdTRUE) {
        /* drain */
    }
}

static int svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && svc && s_ble.nsvc < MAX_SVCS) {
        s_ble.svcs[s_ble.nsvc].start = svc->start_handle;
        s_ble.svcs[s_ble.nsvc].end = svc->end_handle;
        s_ble.nsvc++;
    } else if (error->status == BLE_HS_EDONE) {
        xSemaphoreGive(s_ble.disc_sem);
    }
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && chr && s_ble.nchr < MAX_CHRS) {
        chr_rec_t *r = &s_ble.chrs[s_ble.nchr];
        r->def = chr->def_handle;
        r->val = chr->val_handle;
        r->props = chr->properties;
        r->uuid16 = 0;
        if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            r->uuid16 = chr->uuid.u16.value;
        }
        s_ble.nchr++;
    } else if (error->status == BLE_HS_EDONE) {
        xSemaphoreGive(s_ble.disc_sem);
    }
    return 0;
}

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;
    if (error->status == 0 && dsc) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
            s_ble.cccd_found = dsc->handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        xSemaphoreGive(s_ble.disc_sem);
    }
    return 0;
}

/* ---------- setup task ---------- */

#define WRITE_PROPS (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)

static bool scan_for_device(uint32_t scan_ms)
{
    struct ble_gap_disc_params dp;
    memset(&dp, 0, sizeof(dp));
    dp.filter_duplicates = 1;
    dp.passive = 0;

    for (int attempt = 0; attempt < 3 && !s_ble.abort; attempt++) {
        s_ble.found_flag = false;
        clear_sem(s_ble.disc_sem);
        int rc = ble_gap_disc(s_own_addr_type, (int32_t)scan_ms, &dp, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
            return false;
        }
        int64_t deadline = esp_timer_get_time() + (int64_t)(scan_ms + 1500) * 1000;
        while (!s_ble.found_flag && esp_timer_get_time() < deadline && !s_ble.abort) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_ble.found_flag) {
            vTaskDelay(pdMS_TO_TICKS(200)); /* daj chwile na DISC_COMPLETE/cancel */
            return true;
        }
        ESP_LOGW(TAG, "skan %d: nie znaleziono '%s'", attempt + 1, s_ble.prefix);
    }
    return false;
}

static bool connect_device(void)
{
    clear_sem(s_ble.conn_sem);
    s_ble.conn_status = -1;
    int rc = ble_gap_connect(s_own_addr_type, &s_ble.found, 10000, NULL,
                             gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
        return false;
    }
    if (xSemaphoreTake(s_ble.conn_sem, pdMS_TO_TICKS(12000)) != pdTRUE) {
        ESP_LOGW(TAG, "connect timeout");
        return false;
    }
    if (s_ble.conn_status != 0) {
        ESP_LOGW(TAG, "connect status=%d", s_ble.conn_status);
        return false;
    }
    return true;
}

static bool discover_and_subscribe(void)
{
    /* 1) uslugi */
    s_ble.nsvc = 0;
    clear_sem(s_ble.disc_sem);
    if (ble_gattc_disc_all_svcs(s_ble.conn, svc_cb, NULL) != 0 ||
        xSemaphoreTake(s_ble.disc_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "discovery uslug nieudane");
        return false;
    }
    ESP_LOGI(TAG, "uslug: %d", s_ble.nsvc);

    /* 2) szukamy pary write+notify (preferencja FFF1/FFF2) */
    int iw = -1, in = -1;
    for (int s = 0; s < s_ble.nsvc; s++) {
        s_ble.nchr = 0;
        clear_sem(s_ble.disc_sem);
        if (ble_gattc_disc_all_chrs(s_ble.conn, s_ble.svcs[s].start,
                                    s_ble.svcs[s].end, chr_cb, NULL) != 0) {
            continue;
        }
        if (xSemaphoreTake(s_ble.disc_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            continue;
        }
        for (int i = 0; i < s_ble.nchr; i++) {
            chr_rec_t *r = &s_ble.chrs[i];
            r->end = (i + 1 < s_ble.nchr) ? (uint16_t)(s_ble.chrs[i + 1].def - 1)
                                          : s_ble.svcs[s].end;
        }
        /* preferencje */
        for (int i = 0; i < s_ble.nchr; i++) {
            chr_rec_t *r = &s_ble.chrs[i];
            if ((r->props & WRITE_PROPS) && r->uuid16 == 0xFFF1) {
                iw = i;
                break;
            }
        }
        if (iw < 0) {
            for (int i = 0; i < s_ble.nchr; i++) {
                if (s_ble.chrs[i].props & WRITE_PROPS) {
                    iw = i;
                    break;
                }
            }
        }
        for (int i = 0; i < s_ble.nchr; i++) {
            chr_rec_t *r = &s_ble.chrs[i];
            if ((r->props & BLE_GATT_CHR_PROP_NOTIFY) && r->uuid16 == 0xFFF2) {
                in = i;
                break;
            }
        }
        if (in < 0) {
            for (int i = 0; i < s_ble.nchr; i++) {
                if (s_ble.chrs[i].props & BLE_GATT_CHR_PROP_NOTIFY) {
                    in = i;
                    break;
                }
            }
        }
        if (iw >= 0 && in >= 0) {
            break;
        }
        /* jesli mamy tylko jedno, sprawdz czy ta sama char sluzy obu rolom */
        iw = in = -1;
    }
    /* pojedyncza char obslugujaca obie role (czeste w klonach) */
    if (iw < 0 || in < 0) {
        for (int i = 0; i < s_ble.nchr; i++) {
            chr_rec_t *r = &s_ble.chrs[i];
            if ((r->props & WRITE_PROPS) && (r->props & BLE_GATT_CHR_PROP_NOTIFY)) {
                iw = in = i;
                break;
            }
        }
    }
    if (iw < 0 || in < 0) {
        ESP_LOGW(TAG, "nie znaleziono char write=%d notify=%d", iw, in);
        for (int i = 0; i < s_ble.nchr; i++) {
            ESP_LOGW(TAG, "  chr val=0x%04X props=0x%02X uuid16=0x%04X",
                     s_ble.chrs[i].val, s_ble.chrs[i].props, s_ble.chrs[i].uuid16);
        }
        return false;
    }
    s_ble.write_handle = s_ble.chrs[iw].val;
    s_ble.notify_handle = s_ble.chrs[in].val;
    ESP_LOGI(TAG, "write=0x%04X notify=0x%04X", s_ble.write_handle, s_ble.notify_handle);

    /* 3) CCCD charakterystyki notify */
    s_ble.cccd_found = 0;
    clear_sem(s_ble.disc_sem);
    if (ble_gattc_disc_all_dscs(s_ble.conn, s_ble.chrs[in].val, s_ble.chrs[in].end,
                                dsc_cb, NULL) == 0) {
        xSemaphoreTake(s_ble.disc_sem, pdMS_TO_TICKS(5000));
    }
    if (!s_ble.cccd_found) {
        ESP_LOGW(TAG, "brak CCCD (0x2902) — subskrypcja niemozliwa");
        return false;
    }
    /* 4) subskrypcja */
    const uint8_t cccd_val[2] = { 0x01, 0x00 }; /* notifications on */
    if (ble_gattc_write_flat(s_ble.conn, s_ble.cccd_found, cccd_val, 2, NULL, NULL) != 0) {
        ESP_LOGW(TAG, "zapis CCCD nieudany");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return true;
}

static void setup_task(void *arg)
{
    (void)arg;
    bool ok = false;
    if (ensure_nimble() && scan_for_device(CONFIG_OBDLOG_BT_SCAN_MS) &&
        connect_device() && discover_and_subscribe()) {
        s_ble.ready = true;
        ok = true;
        ESP_LOGI(TAG, "gotowe: podlaczony i zasubskrybowany");
    }
    if (!ok) {
        ESP_LOGW(TAG, "setup nieudany");
    }
    if (s_ble.setup_sem) {
        xSemaphoreGive(s_ble.setup_sem);
    }
    vTaskDelete(NULL);
}

/* ---------- API transportu ---------- */

static esp_err_t b_send(obd_transport_t *t, const char *buf, size_t len)
{
    ble_t *b = (ble_t *)t;
    if (!b->ready || b->conn == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = ble_gattc_write_flat(b->conn, b->write_handle, buf, (uint16_t)len,
                                  NULL, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static int b_recv(obd_transport_t *t, char *buf, size_t max, uint32_t timeout_ms)
{
    ble_t *b = (ble_t *)t;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t got = 0;
    for (;;) {
        if (xSemaphoreTake(b->rx_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
            while (got < max && b->rx_r != b->rx_w) {
                buf[got++] = (char)b->rx[b->rx_r];
                b->rx_r = (b->rx_r + 1) % RXBUF;
            }
            xSemaphoreGive(b->rx_mtx);
        }
        if (got > 0) {
            return (int)got;
        }
        if (esp_timer_get_time() >= deadline) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static bool b_connected(obd_transport_t *t)
{
    ble_t *b = (ble_t *)t;
    return b->ready;
}

static void b_destroy(obd_transport_t *t)
{
    ble_t *b = (ble_t *)t;
    b->abort = true;
    b->ready = false;
    /* semafory zostawiamy do re-uzycia przy kolejnym create (single instance) */
}

obd_transport_t *transport_ble_create(const char *device_name, uint32_t scan_ms)
{
    (void)scan_ms; /* czas skanu z Kconfig */
    if (!ensure_nimble()) {
        return NULL;
    }
    ble_t *b = &s_ble;
    b->base.send = b_send;
    b->base.recv = b_recv;
    b->base.is_connected = b_connected;
    b->base.destroy = b_destroy;
    b->ready = false;
    b->abort = false;
    b->conn = BLE_HS_CONN_HANDLE_NONE;
    b->rx_r = b->rx_w = 0;
    snprintf(b->prefix, sizeof(b->prefix), "%s", device_name ? device_name : "vLinker");
    if (!b->rx_mtx) {
        b->rx_mtx = xSemaphoreCreateMutex();
    }
    if (!b->disc_sem) {
        b->disc_sem = xSemaphoreCreateBinary();
    }
    if (!b->conn_sem) {
        b->conn_sem = xSemaphoreCreateBinary();
    }
    if (!b->setup_sem) {
        b->setup_sem = xSemaphoreCreateBinary();
    }
    if (!b->rx_mtx || !b->disc_sem || !b->conn_sem || !b->setup_sem) {
        return NULL;
    }
    clear_sem(b->setup_sem);
    if (xTaskCreate(setup_task, "ble_setup", 6144, NULL, 5, NULL) != pdPASS) {
        return NULL;
    }
    /* czekaj na wynik setupu (skan + connect + discovery ~ do 25 s) */
    if (xSemaphoreTake(b->setup_sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
        b->abort = true;
        ESP_LOGW(TAG, "create: timeout setupu");
        return NULL;
    }
    if (!b->ready) {
        return NULL;
    }
    return &b->base;
}

#else /* !CONFIG_BT_NIMBLE_ENABLED */

obd_transport_t *transport_ble_create(const char *device_name, uint32_t scan_ms)
{
    (void)device_name;
    (void)scan_ms;
    return NULL; /* BT wylaczone w tej konfiguracji */
}

#endif
