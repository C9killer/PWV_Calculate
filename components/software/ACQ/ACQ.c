#include "ACQ.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include "ADS131.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "ACQ";
static TaskHandle_t acquisition_task;
static TaskHandle_t print_task;
static QueueHandle_t print_queue;
static bool started;

typedef struct {
    uint32_t sequence;
    int32_t channel[3];
} print_sample_t;

static void drdy_isr(void *arg)
{
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(acquisition_task, &wake);
    if (wake == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void printer(void *arg)
{
    print_sample_t sample;
    for (;;) {
        if (xQueueReceive(print_queue, &sample, portMAX_DELAY) == pdTRUE) {
            /* One signed AIN0 raw code per line; UART blocking stays here. */
            printf("%" PRId32 "\n", sample.channel[1]);
        }
    }
}

static void collect(void *arg)
{
    /* Start gate: GPIO handler and ADC configuration are ready before work. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t sequence = 0;
    uint32_t read_errors = 0, queue_drops = 0, resyncs = 0;
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
                ++resyncs;
            } else {
                const print_sample_t output = {
                    .sequence = sequence,
                    .channel = {sample.channel[0], sample.channel[1], sample.channel[2]},
                };
                if (xQueueSend(print_queue, &output, 0) != pdTRUE) {
                    ++queue_drops;
                }
            }
        }
        if ((xTaskGetTickCount() - last_report) >= pdMS_TO_TICKS(1000)) {
            if (read_errors || queue_drops || resyncs) {
                ESP_LOGW(TAG, "Last interval: read_errors=%" PRIu32
                         " print_drops=%" PRIu32 " resyncs=%" PRIu32,
                         read_errors, queue_drops, resyncs);
                read_errors = queue_drops = resyncs = 0;
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
    print_queue = xQueueCreate(64, sizeof(print_sample_t));
    if (print_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(printer, "acq_print", 3072, NULL, 3, &print_task) != pdPASS) {
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
    ESP_LOGI(TAG, "Started %d Hz: AIN0..2, SYNC=GPIO%d, DRDY=GPIO%d falling edge",
             ACQ_SAMPLE_RATE_HZ, ADS131_SYNC_PIN, ACQ_DRDY_PIN);
    return ESP_OK;

fail_handler:
    gpio_intr_disable(ACQ_DRDY_PIN);
    gpio_isr_handler_remove(ACQ_DRDY_PIN);
fail:
    if (acquisition_task != NULL) {
        vTaskDelete(acquisition_task);
        acquisition_task = NULL;
    }
    if (print_task != NULL) {
        vTaskDelete(print_task);
        print_task = NULL;
    }
    vQueueDelete(print_queue);
    print_queue = NULL;
    return err;
}
