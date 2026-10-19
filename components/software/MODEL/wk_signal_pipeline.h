#ifndef WK_SIGNAL_PIPELINE_H
#define WK_SIGNAL_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>

#define WK_SAMPLE_RATE_HZ 250u
#define WK_WINDOW_SECONDS 5u
#define WK_WINDOW_SAMPLES (WK_SAMPLE_RATE_HZ * WK_WINDOW_SECONDS)
#define WK_HOP_SECONDS 1u
#define WK_HOP_SAMPLES (WK_SAMPLE_RATE_HZ * WK_HOP_SECONDS)

typedef struct {
    float x_previous;
    float high_previous;
    float low_stage_1;
    float low_stage_2;
    float high_alpha;
    float low_alpha;
} wk_bandpass_state_t;

typedef struct {
    float ecg[WK_WINDOW_SAMPLES];
    float finger_ppg[WK_WINDOW_SAMPLES];
    float wrist_ppg[WK_WINDOW_SAMPLES];
    size_t write_index;
    size_t samples_seen;
    size_t samples_since_window;
    float finger_polarity;
    float wrist_polarity;
    wk_bandpass_state_t ecg_filter;
    wk_bandpass_state_t finger_filter;
    wk_bandpass_state_t wrist_filter;
} wk_signal_stream_t;

/** Configure 250 Hz causal filters and clear the rolling 5 s window. */
void wk_signal_stream_init(
    wk_signal_stream_t *stream,
    float finger_polarity,
    float wrist_polarity
);

/**
 * Push one synchronized sample. Returns true once per 1 s hop after the
 * initial 5 s window has filled.
 */
bool wk_signal_stream_push(
    wk_signal_stream_t *stream,
    float ecg,
    float finger_ppg,
    float wrist_ppg
);

/** Copy the latest 5 s window into chronological order. */
bool wk_signal_stream_snapshot(
    const wk_signal_stream_t *stream,
    float ecg[WK_WINDOW_SAMPLES],
    float finger_ppg[WK_WINDOW_SAMPLES],
    float wrist_ppg[WK_WINDOW_SAMPLES]
);

#endif
