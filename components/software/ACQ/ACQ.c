#include "ACQ.h"

#include <inttypes.h>
#include <stdbool.h>
#include "ADS131.h"
#include "protocol.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define ACQ_WINDOW_BUFFER_COUNT 2U
/* CPU0 owns the time-critical ADC/SPI acquisition path.  Window consumers
 * are deliberately isolated on CPU1 so their processing cannot preempt it. */
#define ACQ_IO_CORE 0
#define ACQ_PROCESSING_CORE 1

static const char *TAG = "ACQ";
static TaskHandle_t acquisition_task;
static TaskHandle_t window_task;
static QueueHandle_t free_window_queue;
static QueueHandle_t ready_window_queue;
static ACQ_Window *window_storage[ACQ_WINDOW_BUFFER_COUNT];
static bool started;

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

__attribute__((weak)) void ACQ_ProcessSample(const int32_t channels[3])
{
    (void)channels;
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

static void collect(void *arg)
{
    /* Start gate: GPIO handler and ADC configuration are ready before work. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t sequence = 0;
    uint32_t read_errors = 0, window_drops = 0, protocol_drops = 0, resyncs = 0;
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
                if (Protocol_PublishSample(sample.channel) != ESP_OK) {
                    ++protocol_drops;
                }
                ACQ_ProcessSample(sample.channel);

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
            if (read_errors || window_drops || protocol_drops || resyncs) {
                ESP_LOGW(TAG, "Last interval: read_errors=%" PRIu32
                         " window_dropped_samples=%" PRIu32
                         " protocol_drops=%" PRIu32 " resyncs=%" PRIu32,
                         read_errors, window_drops, protocol_drops, resyncs);
                read_errors = window_drops = protocol_drops = resyncs = 0;
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
    if (free_window_queue == NULL || ready_window_queue == NULL) {
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
    if (xTaskCreatePinnedToCore(process_windows, "acq_window", 8192, NULL, 4,
                                &window_task, ACQ_PROCESSING_CORE) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (xTaskCreatePinnedToCore(collect, "acq", 4096, NULL, 10,
                                &acquisition_task, ACQ_IO_CORE) != pdPASS) {
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
    ESP_LOGI(TAG, "Started %d Hz: AIN0..2, %u samples/window, protocol streaming",
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
    if (free_window_queue != NULL) {
        vQueueDelete(free_window_queue);
        free_window_queue = NULL;
    }
    if (ready_window_queue != NULL) {
        vQueueDelete(ready_window_queue);
        ready_window_queue = NULL;
    }
    for (size_t i = 0; i < ACQ_WINDOW_BUFFER_COUNT; ++i) {
        heap_caps_free(window_storage[i]);
        window_storage[i] = NULL;
    }
    return err;
}
