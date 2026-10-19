#include "wk_signal_pipeline.h"

#include <math.h>
#include <string.h>

static void configure_filter(
    wk_bandpass_state_t *filter,
    float highpass_hz,
    float lowpass_hz
)
{
    const float pi = 3.14159265358979323846f;
    const float dt = 1.0f / (float)WK_SAMPLE_RATE_HZ;
    const float high_rc = 1.0f / (2.0f * pi * highpass_hz);
    const float low_rc = 1.0f / (2.0f * pi * lowpass_hz);
    memset(filter, 0, sizeof(*filter));
    filter->high_alpha = high_rc / (high_rc + dt);
    filter->low_alpha = dt / (low_rc + dt);
}

static float filter_sample(wk_bandpass_state_t *filter, float value)
{
    const float high = filter->high_alpha
        * (filter->high_previous + value - filter->x_previous);
    filter->x_previous = value;
    filter->high_previous = high;
    filter->low_stage_1 += filter->low_alpha * (high - filter->low_stage_1);
    filter->low_stage_2 += filter->low_alpha
        * (filter->low_stage_1 - filter->low_stage_2);
    return filter->low_stage_2;
}

void wk_signal_stream_init(
    wk_signal_stream_t *stream,
    float finger_polarity,
    float wrist_polarity
)
{
    memset(stream, 0, sizeof(*stream));
    stream->finger_polarity = finger_polarity >= 0.0f ? 1.0f : -1.0f;
    stream->wrist_polarity = wrist_polarity >= 0.0f ? 1.0f : -1.0f;
    configure_filter(&stream->ecg_filter, 5.0f, 25.0f);
    configure_filter(&stream->finger_filter, 0.5f, 8.0f);
    configure_filter(&stream->wrist_filter, 0.5f, 8.0f);
}

bool wk_signal_stream_push(
    wk_signal_stream_t *stream,
    float ecg,
    float finger_ppg,
    float wrist_ppg
)
{
    const size_t index = stream->write_index;
    stream->ecg[index] = filter_sample(&stream->ecg_filter, ecg);
    stream->finger_ppg[index] = filter_sample(
        &stream->finger_filter, stream->finger_polarity * finger_ppg
    );
    stream->wrist_ppg[index] = filter_sample(
        &stream->wrist_filter, stream->wrist_polarity * wrist_ppg
    );
    stream->write_index = (index + 1u) % WK_WINDOW_SAMPLES;
    if (stream->samples_seen < WK_WINDOW_SAMPLES) {
        ++stream->samples_seen;
    }
    ++stream->samples_since_window;
    if (stream->samples_seen < WK_WINDOW_SAMPLES) {
        return false;
    }
    if (stream->samples_since_window >= WK_HOP_SAMPLES) {
        stream->samples_since_window = 0u;
        return true;
    }
    return false;
}

bool wk_signal_stream_snapshot(
    const wk_signal_stream_t *stream,
    float ecg[WK_WINDOW_SAMPLES],
    float finger_ppg[WK_WINDOW_SAMPLES],
    float wrist_ppg[WK_WINDOW_SAMPLES]
)
{
    size_t output_index;
    if (stream == NULL || ecg == NULL || finger_ppg == NULL || wrist_ppg == NULL
        || stream->samples_seen < WK_WINDOW_SAMPLES) {
        return false;
    }
    for (output_index = 0u; output_index < WK_WINDOW_SAMPLES; ++output_index) {
        const size_t source = (stream->write_index + output_index) % WK_WINDOW_SAMPLES;
        ecg[output_index] = stream->ecg[source];
        finger_ppg[output_index] = stream->finger_ppg[source];
        wrist_ppg[output_index] = stream->wrist_ppg[source];
    }
    return true;
}
