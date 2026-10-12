/* transport_uart.cpp — transport UART (bench: klasyczny ELM327 po kablu). */
#include "obd_transport.h"

#include <Arduino.h>

static HardwareSerial *s_ser = NULL;

static esp_err_t u_send(obd_transport_t *t, const char *buf, size_t len)
{
    (void)t;
    if (!s_ser) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t w = s_ser->write((const uint8_t *)buf, len);
    return w == len ? ESP_OK : ESP_FAIL;
}

static int u_recv(obd_transport_t *t, char *buf, size_t max, uint32_t timeout_ms)
{
    (void)t;
    if (!s_ser) {
        return -1;
    }
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < max && (millis() - t0) < timeout_ms) {
        int av = s_ser->available();
        if (av > 0) {
            size_t take = (max - got < (size_t)av) ? (max - got) : (size_t)av;
            got += s_ser->readBytes((char *)(buf + got), take);
        } else {
            delay(2);
        }
    }
    return (int)got;
}

static bool u_connected(obd_transport_t *t)
{
    (void)t;
    return s_ser != NULL;
}

static void u_destroy(obd_transport_t *t)
{
    (void)t;
}

obd_transport_t *transport_uart_create(int uart_num, int tx_pin, int rx_pin, int baud)
{
    static obd_transport_t base;
    s_ser = (uart_num == 2) ? &Serial2 : &Serial1;
    s_ser->begin(baud > 0 ? baud : 38400, SERIAL_8N1, rx_pin, tx_pin);
    base.send = u_send;
    base.recv = u_recv;
    base.is_connected = u_connected;
    base.destroy = u_destroy;
    Serial.printf("[tr_uart] UART%d @ %d (tx=%d rx=%d)\n", uart_num,
                  baud > 0 ? baud : 38400, tx_pin, rx_pin);
    return &base;
}
