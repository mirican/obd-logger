/* transport_ble.cpp — BLE central (biblioteka BLE/NimBLE) do adaptera vLinker MS.
 *
 * ESP32-P4 nie ma radia: BLE dostarcza C6 przez ESP-Hosted — core Arduino
 * sam wywoluje hostedInitBLE() przy inicjalizacji BLE.
 * Wymagania: tryb adaptera "BT+BLE" (VgateFwUpdater) i firmware C6 hosted >= 2.6.0.
 */
#include "obd_transport.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define RXBUF 2048
static const char *TAG = "tr_ble";

typedef struct {
    obd_transport_t base;
    SemaphoreHandle_t mtx;
    size_t wr, rd;
    uint8_t rx[RXBUF];
    BLEClient *client;
    BLERemoteCharacteristic *wch;
    BLERemoteCharacteristic *nch;
    volatile bool ready;
    char prefix[32];
} bridge_t;

static bridge_t s_b;

static void notify_cb(BLERemoteCharacteristic *ch, uint8_t *data, size_t len, bool isNotify)
{
    (void)ch;
    (void)isNotify;
    if (!s_b.mtx) {
        return;
    }
    if (xSemaphoreTake(s_b.mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        size_t nw = (s_b.wr + 1) % RXBUF;
        if (nw == s_b.rd) {
            break;
        }
        s_b.rx[s_b.wr] = data[i];
        s_b.wr = nw;
    }
    xSemaphoreGive(s_b.mtx);
}

static esp_err_t b_send(obd_transport_t *t, const char *buf, size_t len)
{
    (void)t;
    if (!s_b.ready || !s_b.wch) {
        return ESP_ERR_INVALID_STATE;
    }
    bool ok = s_b.wch->writeValue((uint8_t *)buf, len, false);
    return ok ? ESP_OK : ESP_FAIL;
}

static int b_recv(obd_transport_t *t, char *buf, size_t max, uint32_t timeout_ms)
{
    (void)t;
    if (!s_b.mtx) {
        return -1;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t got = 0;
    for (;;) {
        if (xSemaphoreTake(s_b.mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
            while (got < max && s_b.rd != s_b.wr) {
                buf[got++] = (char)s_b.rx[s_b.rd];
                s_b.rd = (s_b.rd + 1) % RXBUF;
            }
            xSemaphoreGive(s_b.mtx);
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
    (void)t;
    return s_b.ready && s_b.client && s_b.client->isConnected();
}

static void b_destroy(obd_transport_t *t)
{
    (void)t;
    s_b.ready = false;
    if (s_b.client && s_b.client->isConnected()) {
        s_b.client->disconnect();
    }
    s_b.wch = NULL;
    s_b.nch = NULL;
}

/* dopasowanie UUID po fragmencie (np. "fff1", "fff2") */
static bool uuid_has(BLEUUID u, const char *needle)
{
    String s = u.toString();
    s.toLowerCase();
    return s.indexOf(needle) >= 0;
}

static bool scan_find(const char *prefix, BLEAddress *addr, uint8_t *atype, uint32_t scan_ms)
{
    BLEScan *scan = BLEDevice::getScan();
    if (!scan) {
        return false;
    }
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);

    uint32_t secs = scan_ms / 1000 ? scan_ms / 1000 : 1;
    String pl(prefix);
    pl.toLowerCase();

    for (int attempt = 0; attempt < 3; attempt++) {
        BLEScanResults *res = scan->start(secs, false);
        if (res) {
            int cnt = res->getCount();
            for (int i = 0; i < cnt; i++) {
                BLEAdvertisedDevice d = res->getDevice((uint32_t)i);
                if (!d.haveName()) {
                    continue;
                }
                String nl = d.getName();
                nl.toLowerCase();
                if (nl.startsWith(pl)) {
                    *addr = d.getAddress();
                    *atype = d.getAddressType();
                    scan->clearResults();
                    return true;
                }
            }
            scan->clearResults();
        }
        delay(300);
    }
    return false;
}

obd_transport_t *transport_ble_create(const char *device_name, uint32_t scan_ms)
{
    if (!s_b.mtx) {
        s_b.mtx = xSemaphoreCreateMutex();
        if (!s_b.mtx) {
            return NULL;
        }
    }
    s_b.rd = s_b.wr = 0;
    s_b.ready = false;
    s_b.wch = s_b.nch = NULL;
    snprintf(s_b.prefix, sizeof(s_b.prefix), "%s", device_name ? device_name : "vLinker");
    s_b.base.send = b_send;
    s_b.base.recv = b_recv;
    s_b.base.is_connected = b_connected;
    s_b.base.destroy = b_destroy;

    if (!BLEDevice::getInitialized()) {
        Serial.printf("[%s] BLEDevice nie zainicjowany (zrob to w setup())\n", TAG);
        return NULL;
    }

    BLEAddress addr;
    uint8_t atype = 0;
    if (!scan_find(s_b.prefix, &addr, &atype, scan_ms)) {
        Serial.printf("[%s] nie znaleziono '%s'\n", TAG, s_b.prefix);
        return NULL;
    }
    Serial.printf("[%s] znaleziono %s\n", TAG, addr.toString().c_str());

    s_b.client = BLEDevice::createClient();
    if (!s_b.client) {
        return NULL;
    }
    if (!s_b.client->connect(addr, atype, 10000)) {
        Serial.printf("[%s] connect nieudany\n", TAG);
        return NULL;
    }
    s_b.client->setMTU(185); /* vLinker i tak dzieli ramki; wiekszy MTU pomaga dlugim odpowiedziom */

    BLERemoteCharacteristic *wc = NULL;
    BLERemoteCharacteristic *nc = NULL;
    std::map<std::string, BLERemoteService *> *svcs = s_b.client->getServices();
    if (svcs) {
        for (auto &kv : *svcs) {
            BLERemoteService *svc = kv.second;
            std::map<std::string, BLERemoteCharacteristic *> *chs = svc->getCharacteristics();
            if (!chs) {
                continue;
            }
            for (auto &kc : *chs) {
                BLERemoteCharacteristic *ch = kc.second;
                if ((ch->canWrite() || ch->canWriteNoResponse()) &&
                    (!wc || uuid_has(ch->getUUID(), "fff1"))) {
                    wc = ch;
                }
                if (ch->canNotify() && (!nc || uuid_has(ch->getUUID(), "fff2"))) {
                    nc = ch;
                }
            }
        }
    }
    /* klony czesto maja jedna charakterystyke do obu rol */
    if (wc && !nc && wc->canNotify()) {
        nc = wc;
    }
    if (nc && !wc && (nc->canWrite() || nc->canWriteNoResponse())) {
        wc = nc;
    }
    if (!wc || !nc) {
        Serial.printf("[%s] brak charakterystyki write/notify\n", TAG);
        s_b.client->disconnect();
        return NULL;
    }
    s_b.wch = wc;
    s_b.nch = nc;
    nc->registerForNotify(notify_cb, true, true);
    delay(100);
    s_b.ready = true;
    Serial.printf("[%s] gotowe (write=%s notify=%s)\n", TAG,
                  wc->getUUID().toString().c_str(), nc->getUUID().toString().c_str());
    return &s_b.base;
}
