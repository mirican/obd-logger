#pragma once
/* transport — wspolny interfejs lacza do adaptera (UART / BLE). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obd_transport obd_transport_t;

struct obd_transport {
    esp_err_t (*send)(obd_transport_t *t, const char *buf, size_t len);
    /* Blokuje <= timeout_ms; zwraca liczbe bajtow lub 0 (timeout/brak), -1 blad. */
    int (*recv)(obd_transport_t *t, char *buf, size_t max, uint32_t timeout_ms);
    bool (*is_connected)(obd_transport_t *t);
    void (*destroy)(obd_transport_t *t);
};

/* UART (bench / klasyczny ELM327 po kablu). Piny < 0 = domyslne. */
obd_transport_t *transport_uart_create(int uart_num, int tx_pin, int rx_pin, int baud);

/* BLE central do adaptera (np. "vLinker"); NULL gdy BT niedostepne/blad.
 * Uwaga: adapter musi byc w trybie BLE (vLinker MS: "BT+BLE" przez VgateFwUpdater). */
obd_transport_t *transport_ble_create(const char *device_name, uint32_t scan_ms);

#ifdef __cplusplus
}
#endif
