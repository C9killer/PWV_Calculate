#include "ACQ.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include "ADS131.h"
#include "wifi.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define ACQ_WINDOW_BUFFER_COUNT 2U
#define ACQ_STREAM_QUEUE_LENGTH 256U

static const char *TAG = "ACQ";
static TaskHandle_t acquisition_task;
static TaskHandle_t window_task;
static TaskHandle_t stream_task;
static QueueHandle_t free_window_queue;
static QueueHandle_t ready_window_queue;
static QueueHandle_t stream_queue;
static ACQ_Window *window_storage[ACQ_WINDOW_BUFFER_COUNT];
static bool started;

typedef struct {
    uint32_t sequence;
    int32_t channel[3];
} stream_sample_t;

static void drdy_isr(void *arg)
{
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(acquisition_task, &wake);
    if (wake == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

__attribute__((weak)) void ACQ_ProcessWindow(const ACQ_Window *window)
{
    (void)window;
}

static void process_windows(void *arg)
{
    ACQ_Window *window;
    for (;;) {
        if (xQueueReceive(ready_window_queue, &window, portMAX_DELAY) == pdTRUE) {
            ACQ_ProcessWindow(window);
            window->sample_count = 0;
            xQueueSend(free_window_queue, &window, portMAX_DELAY);
        }
    }
}

static void stream_samples(void *arg)
{
    stream_sample_t sample;
    char line[48];
    for (;;) {
        if (xQueueReceive(stream_queue, &sample, portMAX_DELAY) == pdTRUE) {
            int length = snprintf(line, sizeof(line),
                                  "%" PRId32 ",%" PRId32 ",%" PRId32 "\n",
                                  sample.channel[0], sample.channel[1],
                                  sample.channel[2]);
            if (length > 0 && (size_t)length < sizeof(line)) {
                /* This task may block on UART; acquisition only performs a
                 * nonblocking queue copy and therefore keeps its 2 ms budget.
                 */
                fwrite(line, 1, (size_t)length, stdout);
                wifi_send_data(line, (size_t)length);
            }
        }
    }
}

static void collect(void *arg)
{
    /* Start gate: GPIO handler and ADC configuration are ready before work. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t sequence = 0;
    uint32_t read_errors = 0, window_drops = 0, stream_drops = 0, resyncs = 0;
    ACQ_Window *active_window = NULL;
    xQueueReceive(free_window_queue, &active_window, 0);
    unsigned settling = 4;
    TickType_t last_report = xTaskGetTickCount();
    for (;;) {
        uint32_t pending = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (pending == 0) {
            ESP_LOGW(TAG, "No DRDY for 1s: check GPIO7, CLKIN and ADC supplies");
        } else if (pending > 1) {
            /* Notifications count edges, not stored samples. Do not invent
             * historical samples by repeatedly reading an overrun FIFO.
             */
            sequence += pending;
            gpio_intr_disable(ACQ_DRDY_PIN);
            ADS131_Synchronize();
            ulTaskNotifyTake(pdTRUE, 0);
            gpio_intr_enable(ACQ_DRDY_PIN);
            settling = 4;
            if (active_window != NULL) {
                active_window->sample_count = 0;
            }
            ++resyncs;
        } else {
            ADS131_Sample sample;
            ++sequence;
            esp_err_t err = ADS131_ReadSample(&sample);
            if (settling != 0) {
                /* Drain initial frames while filters settle after SYNC. */
                --settling;
            } else if (err != ESP_OK) {
                ++read_errors;
                /* SPI/CRC/freshness failures can leave the two-deep FIFO
                 * ambiguous. Resynchronize before accepting another sample.
                 */
                gpio_intr_disable(ACQ_DRDY_PIN);
                ADS131_Synchronize();
                ulTaskNotifyTake(pdTRUE, 0);
                gpio_intr_enable(ACQ_DRDY_PIN);
                settling = 4;
                if (active_window != NULL) {
                    active_window->sample_count = 0;
                }
                ++resyncs;
            } else {
                const stream_sample_t output = {
                    .sequence = sequence,
                    .channel = {sample.channel[0], sample.channel[1],
                                sample.channel[2]},
                };
                if (xQueueSend(stream_queue, &output, 0) != pdTRUE) {
                    ++stream_drops;
                }

                if (active_window == NULL) {
                    xQueueReceive(free_window_queue, &active_window, 0);
                    if (active_window == NULL) {
                        ++window_drops;
                    }
                }
                if (active_window != NULL) {
                    const size_t index = active_window->sample_count;
                    if (index == 0) {
                        active_window->first_sequence = sequence;
                    }
                    active_window->sample[index][0] = sample.channel[0];
                    active_window->sample[index][1] = sample.channel[1];
                    active_window->sample[index][2] = sample.channel[2];
                    active_window->last_sequence = sequence;
                    active_window->sample_count = index + 1;

                    if (active_window->sample_count == ACQ_WINDOW_SAMPLE_COUNT) {
                        if (xQueueSend(ready_window_queue, &active_window, 0) != pdTRUE) {
                            active_window->sample_count = 0;
                            xQueueSend(free_window_queue, &active_window, 0);
                            ++window_drops;
                        }
                        active_window = NULL;
                        xQueueReceive(free_window_queue, &active_window, 0);
                    }
                }
            }
        }
        if ((xTaskGetTickCount() - last_report) >= pdMS_TO_TICKS(1000)) {
            if (read_errors || window_drops || stream_drops || resyncs) {
                ESP_LOGW(TAG, "Last interval: read_errors=%" PRIu32
                         " window_dropped_samples=%" PRIu32
                         " stream_drops=%" PRIu32 " resyncs=%" PRIu32,
                         read_errors, window_drops, stream_drops, resyncs);
                read_errors = window_drops = stream_drops = resyncs = 0;
            }
            last_report = xTaskGetTickCount();
        }
    }
}

esp_err_t ACQ_Start(void)
{
    if (started) {
        return ESP_OK;
    }
    esp_err_t err = ADS131_Init();
    if (err != ESP_OK) {
        return err;
    }
    err = ADS131_ConfigureAcquisition();
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t drdy = {
        .pin_bit_mask = 1ULL << ACQ_DRDY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&drdy);
    if (err != ESP_OK) {
        return err;
    }
    /* This driver installs a non-IRAM ISR service; no SPI/stdio in the ISR. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    free_window_queue = xQueueCreate(ACQ_WINDOW_BUFFER_COUNT,
                                     sizeof(ACQ_Window *));
    ready_window_queue = xQueueCreate(ACQ_WINDOW_BUFFER_COUNT,
                                      sizeof(ACQ_Window *));
    stream_queue = xQueueCreate(ACQ_STREAM_QUEUE_LENGTH,
                                sizeof(stream_sample_t));
    if (free_window_queue == NULL || ready_window_queue == NULL ||
        stream_queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    for (size_t i = 0; i < ACQ_WINDOW_BUFFER_COUNT; ++i) {
        window_storage[i] = heap_caps_calloc(1, sizeof(ACQ_Window),
                                             MALLOC_CAP_SPIRAM |
                                             MALLOC_CAP_8BIT);
        if (window_storage[i] == NULL) {
            ESP_LOGE(TAG, "Unable to allocate five-second window %u in PSRAM",
                     (unsigned)i);
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        xQueueSend(free_window_queue, &window_storage[i], 0);
    }
    if (xTaskCreate(process_windows, "acq_window", 8192, NULL, 4,
                    &window_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (xTaskCreate(stream_samples, "acq_stream", 4096, NULL, 3,
                    &stream_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (xTaskCreate(collect, "acq", 4096, NULL, 10, &acquisition_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = gpio_isr_handler_add(ACQ_DRDY_PIN, drdy_isr, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    /* handler_add enables this GPIO; keep it masked until the start gate. */
    gpio_intr_disable(ACQ_DRDY_PIN);
    /* Setting the edge type does not enable it until gpio_intr_enable. */
    err = gpio_set_intr_type(ACQ_DRDY_PIN, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) {
        goto fail_handler;
    }
    ADS131_Synchronize();
    /* Wake the start gate before enabling the real DRDY interrupt. */
    xTaskNotifyGive(acquisition_task);
    err = gpio_intr_enable(ACQ_DRDY_PIN);
    if (err != ESP_OK) {
        goto fail_handler;
    }
    started = true;
    ESP_LOGI(TAG, "Started %d Hz: AIN0..2, %u samples/window, UART + Wi-Fi streaming",
             ACQ_SAMPLE_RATE_HZ, (unsigned)ACQ_WINDOW_SAMPLE_COUNT);
    return ESP_OK;

fail_handler:
    gpio_intr_disable(ACQ_DRDY_PIN);
    gpio_isr_handler_remove(ACQ_DRDY_PIN);
fail:
    if (acquisition_task != NULL) {
        vTaskDelete(acquisition_task);
        acquisition_task = NULL;
    }
    if (window_task != NULL) {
        vTaskDelete(window_task);
        window_task = NULL;
    }
    if (stream_task != NULL) {
        vTaskDelete(stream_task);
        stream_task = NULL;
    }
    if (free_window_queue != NULL) {
        vQueueDelete(free_window_queue);
        free_window_queue = NULL;
    }
    if (ready_window_queue != NULL) {
        vQueueDelete(ready_window_queue);
        ready_window_queue = NULL;
    }
    if (stream_queue != NULL) {
        vQueueDelete(stream_queue);
        stream_queue = NULL;
    }
    for (size_t i = 0; i < ACQ_WINDOW_BUFFER_COUNT; ++i) {
        heap_caps_free(window_storage[i]);
        window_storage[i] = NULL;
    }
    return err;
}
