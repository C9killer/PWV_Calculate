/* GB2312: fixed ten-byte protocol sender for the pre-host application. */
#include "protocol.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wifi.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PROTOCOL_QUEUE_LENGTH 384u
#define PROTOCOL_TASK_STACK 4096u
/* CPU0 is reserved for acquisition and network egress. */
#define PROTOCOL_IO_CORE 0

typedef enum {
    PROTOCOL_EVENT_SAMPLE,
    PROTOCOL_EVENT_FLOAT
} protocol_event_kind_t;

typedef struct {
    protocol_event_kind_t kind;
    union {
        int32_t sample[3];
        struct { uint8_t id; float value; } parameter;
    } payload;
} protocol_event_t;

static const char *TAG = "PROTOCOL";
static QueueHandle_t s_protocol_queue;
static bool s_started;
static uint32_t s_queue_drops;
static uint32_t s_send_drops;

static void write_float32_le(uint8_t target[4], float value)
{
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));
    target[0] = (uint8_t)raw;
    target[1] = (uint8_t)(raw >> 8u);
    target[2] = (uint8_t)(raw >> 16u);
    target[3] = (uint8_t)(raw >> 24u);
}

static void pack_float32(uint8_t id, float value, uint8_t frame[PROTOCOL_FRAME_SIZE])
{
    frame[0] = PROTOCOL_FRAME_HEAD;
    frame[PROTOCOL_LENGTH_OFFSET] = PROTOCOL_PACKET_LENGTH;
    frame[PROTOCOL_TYPE_OFFSET] = PROTOCOL_PACKET_TYPE_DATA;
    frame[PROTOCOL_DATA_ID_OFFSET] = id;
    frame[PROTOCOL_DATA_FORMAT_OFFSET] = PROTOCOL_DATA_FORMAT_FLOAT32;
    write_float32_le(&frame[PROTOCOL_PAYLOAD_OFFSET], value);
    frame[PROTOCOL_TAIL_OFFSET] = PROTOCOL_FRAME_TAIL;
}

static void send_frame(uint8_t id, float value)
{
    uint8_t frame[PROTOCOL_FRAME_SIZE];
    pack_float32(id, value, frame);
    /* Send the exact same bytes, including NULs, on enabled transports.
     * Flush each frame because binary payloads need not contain a newline.
     * Only this worker blocks on UART; acquisition enqueues without waiting. */
    flockfile(stdout);
    size_t written = fwrite(frame, 1, sizeof(frame), stdout);
    int flush_result = fflush(stdout);
    funlockfile(stdout);
    if (written != sizeof(frame) || flush_result != 0) {
        ++s_send_drops;
    }
#if ENABLE_WIFI
    /* wifi_send_data copies exact byte length; payload NUL bytes are valid. */
    if (wifi_send_data((const char *)frame, sizeof(frame)) != ESP_OK) {
        ++s_send_drops;
    }
#endif
}

static void protocol_task(void *argument)
{
    protocol_event_t event;
    (void)argument;
    for (;;) {
        if (xQueueReceive(s_protocol_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.kind == PROTOCOL_EVENT_SAMPLE) {
            send_frame(PROTOCOL_DATA_ID_ECG, (float)event.payload.sample[0]);
            send_frame(PROTOCOL_DATA_ID_PPG1, (float)event.payload.sample[1]);
            send_frame(PROTOCOL_DATA_ID_PPG2, (float)event.payload.sample[2]);
        } else if (event.kind == PROTOCOL_EVENT_FLOAT) {
            send_frame(event.payload.parameter.id, event.payload.parameter.value);
        }
    }
}

esp_err_t Protocol_Start(void)
{
    if (s_started) return ESP_OK;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    /* Native USB is a separate console transport from UART0. Its default
     * polling backend only explicitly flushes on LF, unsuitable for binary
     * frames. The interrupt driver flushes short packets and buffers TX. */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_config = {
            .tx_buffer_size = 4096,
            .rx_buffer_size = 256,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&usb_config);
        if (err != ESP_OK) return err;
    }
    flockfile(stdout);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_use_driver();
    funlockfile(stdout);
#endif
    /* Disable LF-to-CRLF conversion: 0x0A is valid binary payload data. */
    if (uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                             ESP_LINE_ENDINGS_LF) != 0) {
        return ESP_FAIL;
    }
    s_protocol_queue = xQueueCreate(PROTOCOL_QUEUE_LENGTH, sizeof(protocol_event_t));
    if (s_protocol_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(protocol_task, "protocol_pack", PROTOCOL_TASK_STACK,
                                NULL, 3u, NULL, PROTOCOL_IO_CORE) != pdPASS) {
        vQueueDelete(s_protocol_queue);
        s_protocol_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
#if ENABLE_WIFI
    ESP_LOGI(TAG, "ready: identical 10-byte float32 frames via UART + Wi-Fi TCP");
#else
    ESP_LOGI(TAG, "ready: 10-byte float32 frames via serial; Wi-Fi disabled");
#endif
    return ESP_OK;
}

esp_err_t Protocol_PublishSample(const int32_t channels[3])
{
    protocol_event_t event;
    if (channels == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_started) return ESP_ERR_INVALID_STATE;
    event.kind = PROTOCOL_EVENT_SAMPLE;
    event.payload.sample[0] = channels[0];
    event.payload.sample[1] = channels[1];
    event.payload.sample[2] = channels[2];
    if (xQueueSend(s_protocol_queue, &event, 0) != pdTRUE) {
        ++s_queue_drops;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t Protocol_PublishBloodPressure(float sbp, float dbp)
{
    protocol_event_t event;
    if (!isfinite(sbp) || !isfinite(dbp)) return ESP_ERR_INVALID_ARG;
    if (!s_started) return ESP_ERR_INVALID_STATE;
    event.kind = PROTOCOL_EVENT_FLOAT;
    event.payload.parameter.id = PROTOCOL_DATA_ID_SBP;
    event.payload.parameter.value = sbp;
    if (xQueueSend(s_protocol_queue, &event, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    event.payload.parameter.id = PROTOCOL_DATA_ID_DBP;
    event.payload.parameter.value = dbp;
    if (xQueueSend(s_protocol_queue, &event, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
