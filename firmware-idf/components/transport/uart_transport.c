/* uart_transport.c — transport UART (testy na biurku / klasyczny ELM327). */
#include "obd_transport.h"

#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "tr_uart";

typedef struct {
    obd_transport_t base;
    int num;
} uart_t;

static uart_t s_uart;

static esp_err_t u_send(obd_transport_t *t, const char *buf, size_t len)
{
    uart_t *u = (uart_t *)t;
    int w = uart_write_bytes(u->num, buf, len);
    return w == (int)len ? ESP_OK : ESP_FAIL;
}

static int u_recv(obd_transport_t *t, char *buf, size_t max, uint32_t timeout_ms)
{
    uart_t *u = (uart_t *)t;
    int r = uart_read_bytes(u->num, buf, max, pdMS_TO_TICKS(timeout_ms));
    return r; /* 0 = brak danych w oknie czasowym */
}

static bool u_connected(obd_transport_t *t)
{
    (void)t;
    return true; /* brak stanu lacza na UART */
}

static void u_destroy(obd_transport_t *t)
{
    (void)t;
    /* v0: nie zamykamy sterownika (pojedyncze uzycie) */
}

obd_transport_t *transport_uart_create(int uart_num, int tx_pin, int rx_pin, int baud)
{
    uart_t *u = &s_uart;
    memset(u, 0, sizeof(*u));
    u->num = uart_num;
    u->base.send = u_send;
    u->base.recv = u_recv;
    u->base.is_connected = u_connected;
    u->base.destroy = u_destroy;

    uart_config_t cfg = {
        .baud_rate = baud > 0 ? baud : 38400,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(uart_num, 4096, 0, 0, NULL, 0) != ESP_OK) {
        return NULL;
    }
    if (uart_param_config(uart_num, &cfg) != ESP_OK) {
        return NULL;
    }
    if (tx_pin >= 0 || rx_pin >= 0) {
        uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    ESP_LOGI(TAG, "UART%d @ %d baud (tx=%d rx=%d)", uart_num, cfg.baud_rate, tx_pin, rx_pin);
    return &u->base;
}
