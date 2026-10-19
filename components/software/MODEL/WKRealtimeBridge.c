#include "WKRealtimeBridge.h"

#include <inttypes.h>
#include <math.h>

#include "ACQ.h"
#include "protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wk_realtime_pipeline.h"

/* Hardware channel routing. Change only if the AFE wiring differs. */
#define WK_ACQ_ECG_CHANNEL 0u
#define WK_ACQ_FINGER_PPG_CHANNEL 1u
#define WK_ACQ_WRIST_PPG_CHANNEL 2u
#define WK_INPUT_RATE_HZ WK_SAMPLE_RATE_HZ
#define WK_DECIMATION_FACTOR (ACQ_SAMPLE_RATE_HZ / WK_INPUT_RATE_HZ)
#define WK_QUEUE_LENGTH 512u
/* Keep all signal processing and inference off the acquisition/network core. */
#define WK_MODEL_CORE 1
#define WK_TASK_STACK_BYTES 12288u
#define WK_INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define WK_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char *TAG = "WK_RT";

typedef struct {
    float ecg;
    float finger_ppg;
    float wrist_ppg;
    int64_t captured_us;
    TickType_t captured_tick;
} wk_sample_t;

static QueueHandle_t s_queue;
static wk_realtime_state_t *s_pipeline;

/* Optional fourth-order Butterworth anti-alias filter with a 50 Hz cutoff.
 * Bypassed when acquisition and model both run at 250 Hz. All three
 * channels use identical filters when integer decimation is needed. */
static struct {
    float b0, b1, b2, a1, a2;
    float z1[3], z2[3];
} s_decimator[2];
static unsigned s_decimation_phase;

static void init_decimator(void)
{
    const float q[2] = {0.5411961001f, 1.3065629649f};
    const float omega = 2.0f * 3.14159265358979323846f
        * (0.2f * WK_INPUT_RATE_HZ) / ACQ_SAMPLE_RATE_HZ;
    const float cosine = cosf(omega);
    for (unsigned stage = 0; stage < 2; ++stage) {
        float alpha = sinf(omega) / (2.0f * q[stage]);
        float inverse_a0 = 1.0f / (1.0f + alpha);
        s_decimator[stage].b0 = 0.5f * (1.0f - cosine) * inverse_a0;
        s_decimator[stage].b1 = (1.0f - cosine) * inverse_a0;
        s_decimator[stage].b2 = s_decimator[stage].b0;
        s_decimator[stage].a1 = -2.0f * cosine * inverse_a0;
        s_decimator[stage].a2 = (1.0f - alpha) * inverse_a0;
        for (unsigned channel = 0; channel < 3; ++channel) {
            s_decimator[stage].z1[channel] = 0.0f;
            s_decimator[stage].z2[channel] = 0.0f;
        }
    }
    s_decimation_phase = 0;
}

static float decimator_filter(unsigned channel, float value)
{
    for (unsigned stage = 0; stage < 2; ++stage) {
        float output = s_decimator[stage].b0 * value
            + s_decimator[stage].z1[channel];
        s_decimator[stage].z1[channel] = s_decimator[stage].b1 * value
            - s_decimator[stage].a1 * output + s_decimator[stage].z2[channel];
        s_decimator[stage].z2[channel] = s_decimator[stage].b2 * value
            - s_decimator[stage].a2 * output;
        value = output;
    }
    return value;
}

/* Per-status statistics prevent rejected windows from lowering the BP average. */
typedef struct {
    uint64_t count;
    int64_t total_us;
    int64_t max_us;
    uint64_t over_hop;
} wk_timing_stats_t;
static wk_timing_stats_t s_timing[WK_RT_PHYSIOLOGY_ERROR + 1];

static void wk_report(const wk_realtime_output_t *output, int64_t elapsed,
                      int64_t latency, bool monitored,
                      size_t internal_before, size_t psram_before)
{
    size_t internal_min = 0, psram_min = 0;
    if (monitored) {
        internal_min = heap_caps_get_minimum_free_size(WK_INTERNAL_CAPS);
        psram_min = heap_caps_get_minimum_free_size(WK_PSRAM_CAPS);
        heap_caps_monitor_local_minimum_free_size_stop();
    }
    wk_timing_stats_t *stats = &s_timing[output->status];
    ++stats->count;
    stats->total_us += elapsed;
    if (elapsed > stats->max_us) stats->max_us = elapsed;
    if (elapsed > (int64_t)WK_HOP_SECONDS * 1000000) ++stats->over_hop;
    ESP_LOGI(TAG, "PROFILE status=%s n=%" PRIu64 " sample=%" PRIu64
             " time_us=%" PRId64 " avg_us=%" PRId64 " max_us=%" PRId64
             " over_hop=%" PRIu64 " latency_us=%" PRId64 " queue=%u",
             wk_realtime_status_name(output->status), stats->count, output->sample_index,
             elapsed, stats->total_us / (int64_t)stats->count, stats->max_us,
             stats->over_hop, latency, (unsigned)uxQueueMessagesWaiting(s_queue));
    /* IDF returns stack high-water marks in bytes, unlike vanilla FreeRTOS. */
    size_t stack_free = (size_t)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "RAM stack_peak_since_start_B=%u stack_reserved_B=%u",
             (unsigned)(WK_TASK_STACK_BYTES - stack_free), WK_TASK_STACK_BYTES);
    if (monitored) {
        ESP_LOGI(TAG, "RAM system_window internal_used_peak_bound_B=%u psram_used_peak_bound_B=%u",
                 (unsigned)(heap_caps_get_total_size(WK_INTERNAL_CAPS) - internal_min),
                 (unsigned)(heap_caps_get_total_size(WK_PSRAM_CAPS) - psram_min));
        ESP_LOGI(TAG, "RAM system_window internal_min_free_B=%u psram_min_free_B=%u"
                 " internal_extra_peak_bound_B=%u psram_extra_peak_bound_B=%u",
                 (unsigned)internal_min, (unsigned)psram_min,
                 (unsigned)(internal_before > internal_min ? internal_before - internal_min : 0),
                 (unsigned)(psram_before > psram_min ? psram_before - psram_min : 0));
    } else {
        ESP_LOGW(TAG, "RAM window monitoring unavailable (monitor already active)");
    }
}

static void wk_publish(const wk_realtime_output_t *result, int64_t processing_us,
                       int64_t end_to_end_us, TickType_t end_to_end_ticks)
{
    if (result->status == WK_RT_OUTPUT_READY) {
        (void)Protocol_PublishBloodPressure((float)result->sbp,
                                            (float)result->dbp);
        ESP_LOGI(TAG, "BP SBP=%.2f DBP=%.2f Q=%.2f process=%" PRId64
                 "us end_to_end=%" PRId64 "us (%" PRIu32 " ticks, %" PRIu32 "ms) ref=%d",
                 result->sbp, result->dbp, result->signal_quality.quality_score,
                 processing_us, end_to_end_us, (uint32_t)end_to_end_ticks,
                 (uint32_t)(end_to_end_ticks * portTICK_PERIOD_MS), result->used_reference);
    } else if (result->status != WK_RT_NO_NEW_WINDOW) {
        ESP_LOGW(TAG, "window rejected: %s (%s), process=%" PRId64 "us",
                 wk_realtime_status_name(result->status),
                 wk_signal_status_name(result->signal_quality.status), processing_us);
    }
}

static void wk_task(void *arg)
{
    wk_sample_t sample;
    (void)arg;
    for (;;) {
        if (xQueueReceive(s_queue, &sample, portMAX_DELAY) == pdTRUE) {
            wk_realtime_output_t output;
            uint64_t next_sample = s_pipeline->sample_index + 1;
            bool window_due = next_sample >= WK_WINDOW_SAMPLES &&
                (next_sample - WK_WINDOW_SAMPLES) % WK_HOP_SAMPLES == 0;
            size_t internal_before = 0, psram_before = 0;
            bool monitored = false;
            if (window_due) {
                internal_before = heap_caps_get_free_size(WK_INTERNAL_CAPS);
                psram_before = heap_caps_get_free_size(WK_PSRAM_CAPS);
                monitored = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
            }
            int64_t started = esp_timer_get_time();
            esp_err_t err = wk_realtime_push_sample(s_pipeline, sample.ecg,
                sample.finger_ppg, sample.wrist_ppg, &output);
            int64_t finished = esp_timer_get_time();
            int64_t elapsed = finished - started;
            TickType_t end_to_end_ticks = xTaskGetTickCount() - sample.captured_tick;
            if (window_due) {
                wk_report(&output, elapsed, finished - sample.captured_us,
                          monitored, internal_before, psram_before);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "pipeline error: %s", esp_err_to_name(err));
            }
            if (output.status != WK_RT_NO_NEW_WINDOW) {
                wk_publish(&output, elapsed, finished - sample.captured_us,
                           end_to_end_ticks);
            }
        }
    }
}

esp_err_t WKRealtimeBridge_Start(void)
{
    esp_err_t err;
    if (s_pipeline != NULL) return ESP_OK;
    if (ACQ_SAMPLE_RATE_HZ < WK_INPUT_RATE_HZ ||
        ACQ_SAMPLE_RATE_HZ % WK_INPUT_RATE_HZ != 0) {
        ESP_LOGE(TAG, "requires acquisition at a multiple of %u Hz, got %u Hz", WK_INPUT_RATE_HZ,
                 (unsigned)ACQ_SAMPLE_RATE_HZ);
        return ESP_ERR_NOT_SUPPORTED;
    }
    init_decimator();
    s_queue = xQueueCreate(WK_QUEUE_LENGTH, sizeof(wk_sample_t));
    s_pipeline = heap_caps_calloc(1, sizeof(*s_pipeline),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue == NULL || s_pipeline == NULL) return ESP_ERR_NO_MEM;
    err = wk_realtime_init(s_pipeline, 1.0f, 1.0f);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "RESOURCE model_flash_B=%" PRIu32 " partition_reserved_B=%" PRIu32,
             s_pipeline->edge.models.package_bytes, s_pipeline->edge.models.partition->size);
    ESP_LOGI(TAG, "RESOURCE pipeline_psram_B=%u edge_state_included_B=%u"
             " queue_payload_B=%u task_stack_reserved_B=%u",
             (unsigned)sizeof(*s_pipeline), (unsigned)sizeof(s_pipeline->edge),
             (unsigned)(WK_QUEUE_LENGTH * sizeof(wk_sample_t)), WK_TASK_STACK_BYTES);
    if (xTaskCreatePinnedToCore(wk_task, "wk_realtime", WK_TASK_STACK_BYTES, NULL, 6u,
                                NULL, WK_MODEL_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready: AIN%u=ECG AIN%u=finger PPG AIN%u=wrist PPG; %u Hz acquisition -> %u Hz model",
             WK_ACQ_ECG_CHANNEL, WK_ACQ_FINGER_PPG_CHANNEL, WK_ACQ_WRIST_PPG_CHANNEL,
             (unsigned)ACQ_SAMPLE_RATE_HZ, (unsigned)WK_INPUT_RATE_HZ);
    return ESP_OK;
}

esp_err_t WKRealtimeBridge_SubmitReference(double sbp, double dbp)
{
    return s_pipeline == NULL ? ESP_ERR_INVALID_STATE :
        wk_realtime_set_reference(s_pipeline, sbp, dbp);
}

/* Runs in acquisition: bounded filtering and nonblocking queue submission.
 * Raw protocol publishing separately retains the full acquisition rate. */
void ACQ_ProcessSample(const int32_t channels[3])
{
    wk_sample_t output;
    if (channels == NULL || s_queue == NULL) return;
    float filtered[3];
    for (unsigned channel = 0; channel < 3; ++channel) {
        filtered[channel] = WK_DECIMATION_FACTOR > 1
            ? decimator_filter(channel, (float)channels[channel])
            : (float)channels[channel];
    }
    if (++s_decimation_phase < WK_DECIMATION_FACTOR) return;
    s_decimation_phase = 0;
    output.ecg = filtered[WK_ACQ_ECG_CHANNEL];
    output.finger_ppg = filtered[WK_ACQ_FINGER_PPG_CHANNEL];
    output.wrist_ppg = filtered[WK_ACQ_WRIST_PPG_CHANNEL];
    output.captured_us = esp_timer_get_time();
    output.captured_tick = xTaskGetTickCount();
    if (xQueueSend(s_queue, &output, 0) != pdTRUE) {
        ESP_EARLY_LOGW(TAG, "sample queue full; dropping 250 Hz model sample");
    }
}
