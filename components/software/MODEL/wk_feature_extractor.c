#include "wk_feature_extractor.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "wk_feature_adapter.h"

#define WK_PI 3.14159265358979323846
#define WK_EPS 1e-12

static float clampf(float value, float lower, float upper)
{
    return value < lower ? lower : (value > upper ? upper : value);
}

static double safe_div(double numerator, double denominator)
{
    return fabs(denominator) <= WK_EPS ? 0.0 : numerator / denominator;
}

static void mean_std(const float *values, size_t count, double *mean, double *std)
{
    size_t index;
    double average = 0.0;
    double variance = 0.0;
    for (index = 0u; index < count; ++index) {
        average += values[index];
    }
    average /= count;
    for (index = 0u; index < count; ++index) {
        const double centered = values[index] - average;
        variance += centered * centered;
    }
    *mean = average;
    *std = sqrt(variance / count);
}

static bool normalize(const float *input, float *output, size_t count)
{
    size_t index;
    float minimum = FLT_MAX;
    float maximum = -FLT_MAX;
    for (index = 0u; index < count; ++index) {
        if (!isfinite(input[index])) {
            return false;
        }
        if (input[index] < minimum) minimum = input[index];
        if (input[index] > maximum) maximum = input[index];
    }
    if (maximum - minimum < 1e-8f) {
        return false;
    }
    for (index = 0u; index < count; ++index) {
        output[index] = (input[index] - minimum) / (maximum - minimum);
    }
    return true;
}

static float correlation(const float *left, const float *right, size_t count)
{
    size_t index;
    double left_mean;
    double right_mean;
    double left_std;
    double right_std;
    double covariance = 0.0;
    mean_std(left, count, &left_mean, &left_std);
    mean_std(right, count, &right_mean, &right_std);
    if (left_std < 1e-8 || right_std < 1e-8) return 0.0f;
    for (index = 0u; index < count; ++index) {
        covariance += (left[index] - left_mean) * (right[index] - right_mean);
    }
    return (float)(covariance / (count * left_std * right_std));
}

static size_t detect_peaks(
    const float *signal,
    size_t count,
    size_t minimum_distance,
    float threshold_scale,
    bool invert,
    uint16_t *peaks,
    size_t capacity
)
{
    size_t index;
    size_t found = 0u;
    double mean;
    double std;
    mean_std(signal, count, &mean, &std);
    for (index = 1u; index + 1u < count; ++index) {
        const float value = invert ? -signal[index] : signal[index];
        const float before = invert ? -signal[index - 1u] : signal[index - 1u];
        const float after = invert ? -signal[index + 1u] : signal[index + 1u];
        const float threshold = (float)(invert ? -mean + threshold_scale * std
                                               : mean + threshold_scale * std);
        if (value < threshold || value < before || value <= after) continue;
        if (found > 0u && index - peaks[found - 1u] < minimum_distance) {
            const size_t previous = peaks[found - 1u];
            const float previous_value = invert ? -signal[previous] : signal[previous];
            if (value > previous_value) peaks[found - 1u] = (uint16_t)index;
            continue;
        }
        if (found < capacity) peaks[found++] = (uint16_t)index;
    }
    return found;
}

static float median_interval(const uint16_t *locations, size_t count)
{
    float intervals[WK_MAX_BEATS];
    size_t index;
    size_t j;
    if (count < 2u) return 0.0f;
    for (index = 0u; index + 1u < count; ++index) {
        intervals[index] = (float)(locations[index + 1u] - locations[index]);
    }
    count -= 1u;
    for (index = 1u; index < count; ++index) {
        const float value = intervals[index];
        j = index;
        while (j > 0u && intervals[j - 1u] > value) {
            intervals[j] = intervals[j - 1u];
            --j;
        }
        intervals[j] = value;
    }
    if ((count & 1u) != 0u) return intervals[count / 2u];
    return 0.5f * (intervals[count / 2u - 1u] + intervals[count / 2u]);
}

static float interval_cv(const uint16_t *locations, size_t count)
{
    size_t index;
    double mean = 0.0;
    double variance = 0.0;
    if (count < 3u) return 0.0f;
    for (index = 0u; index + 1u < count; ++index) {
        mean += locations[index + 1u] - locations[index];
    }
    mean /= count - 1u;
    for (index = 0u; index + 1u < count; ++index) {
        const double centered = (locations[index + 1u] - locations[index]) - mean;
        variance += centered * centered;
    }
    return mean <= 0.0 ? 1.0f : (float)(sqrt(variance / (count - 1u)) / mean);
}

static void derivative(const float *input, float *output, size_t count)
{
    size_t index;
    output[0] = 0.0f;
    for (index = 1u; index < count; ++index) {
        output[index] = input[index] - input[index - 1u];
    }
}

static float interpolate(const float *signal, float position)
{
    const size_t lower = (size_t)position;
    const size_t upper = lower + 1u < WK_WINDOW_SAMPLES ? lower + 1u : lower;
    const float fraction = position - (float)lower;
    return signal[lower] + fraction * (signal[upper] - signal[lower]);
}

static void normalize_cycle(float cycle[WK_CYCLE_POINTS])
{
    size_t index;
    float minimum = cycle[0];
    float maximum = cycle[0];
    for (index = 1u; index < WK_CYCLE_POINTS; ++index) {
        if (cycle[index] < minimum) minimum = cycle[index];
        if (cycle[index] > maximum) maximum = cycle[index];
    }
    if (maximum - minimum < 1e-8f) {
        memset(cycle, 0, sizeof(float) * WK_CYCLE_POINTS);
        return;
    }
    for (index = 0u; index < WK_CYCLE_POINTS; ++index) {
        cycle[index] = (cycle[index] - minimum) / (maximum - minimum);
    }
}

static bool average_resampled_segments(
    const float *signal,
    const uint16_t *starts,
    const uint16_t *ends,
    size_t segment_count,
    float output[WK_CYCLE_POINTS]
)
{
    size_t segment;
    size_t point;
    size_t used = 0u;
    memset(output, 0, sizeof(float) * WK_CYCLE_POINTS);
    for (segment = 0u; segment < segment_count; ++segment) {
        float cycle[WK_CYCLE_POINTS];
        const size_t start = starts[segment];
        const size_t end = ends[segment];
        if (end <= start + 8u || end >= WK_WINDOW_SAMPLES) continue;
        for (point = 0u; point < WK_CYCLE_POINTS; ++point) {
            const float position = (float)start
                + (float)(end - start) * point / (float)(WK_CYCLE_POINTS - 1u);
            cycle[point] = interpolate(signal, position);
        }
        normalize_cycle(cycle);
        for (point = 0u; point < WK_CYCLE_POINTS; ++point) output[point] += cycle[point];
        ++used;
    }
    if (used == 0u) return false;
    for (point = 0u; point < WK_CYCLE_POINTS; ++point) output[point] /= used;
    return true;
}

static size_t valley_segments(
    const uint16_t *valleys,
    size_t valley_count,
    uint16_t *starts,
    uint16_t *ends
)
{
    size_t index;
    size_t count = 0u;
    for (index = 0u; index + 1u < valley_count && count < WK_MAX_BEATS; ++index) {
        if (valleys[index + 1u] > valleys[index] + 8u) {
            starts[count] = valleys[index];
            ends[count] = valleys[index + 1u];
            ++count;
        }
    }
    return count;
}

static size_t peak_segments(
    const uint16_t *peaks,
    size_t peak_count,
    float p2p,
    uint16_t *starts,
    uint16_t *ends
)
{
    size_t index;
    size_t count = 0u;
    for (index = 1u; index + 1u < peak_count && count < WK_MAX_BEATS; ++index) {
        const int start = (int)lroundf(peaks[index] - 0.25f * p2p);
        const int end = (int)lroundf(peaks[index] + 0.75f * p2p);
        if (start >= 0 && end < (int)WK_WINDOW_SAMPLES && end > start + 8) {
            starts[count] = (uint16_t)start;
            ends[count] = (uint16_t)end;
            ++count;
        }
    }
    return count;
}

static size_t argmax(const float *values, size_t count)
{
    size_t index;
    size_t result = 0u;
    for (index = 1u; index < count; ++index) {
        if (values[index] > values[result]) result = index;
    }
    return result;
}

static size_t argmin(const float *values, size_t count)
{
    size_t index;
    size_t result = 0u;
    for (index = 1u; index < count; ++index) {
        if (values[index] < values[result]) result = index;
    }
    return result;
}

static double neighbor_max_mean(const float *cycle)
{
    const size_t maximum = argmax(cycle, WK_CYCLE_POINTS);
    const size_t start = maximum > 5u ? maximum - 5u : 0u;
    const size_t end = maximum + 5u < WK_CYCLE_POINTS
        ? maximum + 5u : WK_CYCLE_POINTS;
    size_t index;
    double sum = 0.0;
    for (index = start; index < end; ++index) sum += cycle[index];
    return sum / (end - start);
}

static void histogram(
    const float *cycle,
    size_t start,
    size_t end,
    size_t bins,
    double *output
)
{
    size_t index;
    const double scale = end > start ? (double)bins / (end - start) : 0.0;
    memset(output, 0, sizeof(double) * bins);
    for (index = start; index < end; ++index) {
        size_t bin = (size_t)floor(clampf(cycle[index], 0.0f, 0.999999f) * bins);
        if (bin >= bins) bin = bins - 1u;
        output[bin] += scale;
    }
}

static void fft_features(
    const float *signal,
    float *magnitudes,
    double *peak_index,
    double *peak_height,
    double *neighbor_average
)
{
    size_t sample;
    size_t bin;
    size_t first_peak = 0u;
    const size_t count = WK_WINDOW_SAMPLES;
    const size_t bins = WK_MAX_FFT_BINS;
    const double orthogonal_scale = sqrt((double)count);
    for (bin = 0u; bin < bins; ++bin) {
        const size_t k = bin + 1u;
        const double omega = 2.0 * WK_PI * k / count;
        const double coefficient = 2.0 * cos(omega);
        double previous = 0.0;
        double previous_2 = 0.0;
        for (sample = 0u; sample < count; ++sample) {
            const double current = signal[sample]
                + coefficient * previous - previous_2;
            previous_2 = previous;
            previous = current;
        }
        {
            double power = previous_2 * previous_2
                + previous * previous
                - coefficient * previous * previous_2;
            if (power < 0.0) power = 0.0;
            /* Matches np.fft.fft(..., norm="ortho") magnitude. */
            magnitudes[bin] = (float)(sqrt(power) / orthogonal_scale);
        }
    }
    for (bin = 29u; bin + 1u < bins; ++bin) {
        if (magnitudes[bin] > magnitudes[bin - 1u]
            && magnitudes[bin] >= magnitudes[bin + 1u]) {
            first_peak = bin;
            break;
        }
    }
    if (first_peak == 0u) {
        *peak_index = *peak_height = *neighbor_average = 0.0;
        return;
    }
    *peak_index = first_peak;
    *peak_height = magnitudes[first_peak];
    {
        const size_t start = first_peak > 6u ? first_peak - 6u : 0u;
        const size_t end = first_peak + 6u < bins ? first_peak + 6u : bins;
        double sum = 0.0;
        for (bin = start; bin < end; ++bin) sum += magnitudes[bin];
        *neighbor_average = sum / (end - start);
    }
}

static double trapezoid(const float *values, size_t start, size_t end)
{
    size_t index;
    double area = 0.0;
    if (end <= start + 1u) return 0.0;
    for (index = start; index + 1u < end; ++index) {
        area += 0.5 * (values[index] + values[index + 1u]);
    }
    return area;
}

static void width_at_fraction(
    const float *cycle,
    size_t peak,
    float fraction,
    double *sw,
    double *dw
)
{
    const float minimum = cycle[argmin(cycle, WK_CYCLE_POINTS)];
    const float level = minimum + fraction * (cycle[peak] - minimum);
    size_t left = peak;
    size_t right = peak;
    while (left > 0u && cycle[left] > level) --left;
    while (right + 1u < WK_CYCLE_POINTS && cycle[right] > level) ++right;
    *sw = peak - left;
    *dw = right - peak;
}

static void skew_kurtosis(const float *values, size_t count, double *skew, double *kurt)
{
    size_t index;
    double mean;
    double std;
    double third = 0.0;
    double fourth = 0.0;
    mean_std(values, count, &mean, &std);
    if (std < 1e-8) {
        *skew = *kurt = 0.0;
        return;
    }
    for (index = 0u; index < count; ++index) {
        const double z = (values[index] - mean) / std;
        third += z * z * z;
        fourth += z * z * z * z;
    }
    *skew = third / count;
    *kurt = fourth / count;
}

static void local_extrema_apg(
    const float *apg,
    const float *vpg,
    size_t peak,
    size_t *a,
    size_t *b,
    size_t *c,
    size_t *d,
    size_t *e
)
{
    size_t index;
    size_t y = peak;
    *a = argmax(apg, peak > 2u ? peak : 2u);
    *b = *a + 1u;
    for (index = *a + 1u; index < peak; ++index) {
        if (apg[index] < apg[*b]) *b = index;
    }
    for (index = peak + 1u; index < WK_CYCLE_POINTS - 1u; ++index) {
        if (vpg[index] < vpg[y]) y = index;
    }
    *e = y + 1u < WK_CYCLE_POINTS ? y + 1u : y;
    for (index = y + 1u; index + 1u < WK_CYCLE_POINTS; ++index) {
        if (apg[index] > apg[*e]) *e = index;
    }
    if (*e <= peak + 1u) *e = WK_CYCLE_POINTS - 2u;
    *d = peak;
    for (index = peak; index <= *e; ++index) if (apg[index] < apg[*d]) *d = index;
    *c = *b;
    if (*d <= *b + 1u) *d = *b + 2u;
    for (index = *b; index <= *d; ++index) if (apg[index] > apg[*c]) *c = index;
}

static double harmonic_ratio(const float *signal, double fundamental_hz)
{
    unsigned int harmonic;
    size_t sample;
    double total = 0.0;
    double fundamental_power = 0.0;
    if (fundamental_hz <= 0.0) return 0.0;
    for (harmonic = 1u; harmonic < 20u; ++harmonic) {
        const double frequency = harmonic * fundamental_hz;
        double previous = 0.0;
        double previous_2 = 0.0;
        double coefficient;
        double power;
        if (frequency >= 0.5 * WK_SAMPLE_RATE_HZ) break;
        coefficient = 2.0 * cos(
            2.0 * WK_PI * frequency / WK_SAMPLE_RATE_HZ
        );
        for (sample = 0u; sample < WK_WINDOW_SAMPLES; ++sample) {
            const double current = signal[sample]
                + coefficient * previous - previous_2;
            previous_2 = previous;
            previous = current;
        }
        power = previous_2 * previous_2 + previous * previous
            - coefficient * previous * previous_2;
        if (power < 0.0) power = 0.0;
        if (harmonic == 1u) fundamental_power = power;
        total += power;
    }
    return total <= WK_EPS ? 0.0 : (total - fundamental_power) / total;
}

static double inverse_pat(
    const uint16_t *ecg_peaks,
    size_t ecg_count,
    const uint16_t *ppg_peaks,
    size_t ppg_count
)
{
    size_t ecg_index;
    size_t ppg_index = 0u;
    size_t matched = 0u;
    double sum = 0.0;
    for (ecg_index = 0u; ecg_index < ecg_count; ++ecg_index) {
        while (ppg_index < ppg_count && ppg_peaks[ppg_index] <= ecg_peaks[ecg_index]) {
            ++ppg_index;
        }
        if (ppg_index >= ppg_count) break;
        {
            const double delay = (double)(ppg_peaks[ppg_index] - ecg_peaks[ecg_index])
                / WK_SAMPLE_RATE_HZ;
            if (delay >= 0.04 && delay <= 0.80) {
                sum += delay;
                ++matched;
            }
        }
    }
    return matched < 2u ? 0.0 : 1.0 / (sum / matched);
}

static void usdc_dsdc(const float *cycle, double *usdc, double *dsdc)
{
    float rising[20];
    float falling[60];
    float deviation[60];
    const size_t peak = argmax(cycle, WK_CYCLE_POINTS);
    size_t index;
    for (index = 0u; index < 20u; ++index) {
        const float position = peak * index / 19.0f;
        const size_t lower = (size_t)position;
        const size_t upper = lower + 1u <= peak ? lower + 1u : lower;
        rising[index] = cycle[lower] + (position - lower) * (cycle[upper] - cycle[lower]);
    }
    for (index = 0u; index < 60u; ++index) {
        const float position = peak + (WK_CYCLE_POINTS - 1u - peak) * index / 59.0f;
        const size_t lower = (size_t)position;
        const size_t upper = lower + 1u < WK_CYCLE_POINTS ? lower + 1u : lower;
        falling[index] = cycle[lower] + (position - lower) * (cycle[upper] - cycle[lower]);
    }
    {
        const double denominator = hypot(rising[19] - rising[0], 19.0);
        for (index = 0u; index < 20u; ++index) {
            deviation[index] = (float)((
                rising[index] * (rising[19] - rising[0]) * index
                - rising[index] * 19.0 + rising[0] * 19.0
            ) / (denominator + WK_EPS));
        }
        for (index = 0u; index < 6u; ++index) {
            const size_t start = 3u * index;
            usdc[index] = (deviation[start] + deviation[start + 1u]
                           + deviation[start + 2u]) / 3.0;
        }
    }
    {
        const double denominator = hypot(falling[59] - falling[0], 59.0);
        for (index = 0u; index < 60u; ++index) {
            deviation[index] = (float)((
                falling[index] * (falling[59] - falling[0]) * index
                - 59.0 * falling[index] + falling[0] * 59.0
            ) / (denominator + WK_EPS));
        }
        for (index = 0u; index < 19u; ++index) {
            const size_t start = 3u * index;
            dsdc[index] = (deviation[start] + deviation[start + 1u]
                           + deviation[start + 2u]) / 3.0;
        }
    }
}

bool wk_extract_features_250hz(
    const float ecg[WK_WINDOW_SAMPLES],
    const float finger_ppg[WK_WINDOW_SAMPLES],
    const float wrist_ppg[WK_WINDOW_SAMPLES],
    wk_feature_workspace_t *work,
    double features[WK_COMPACT_BASE_FEATURE_COUNT],
    wk_signal_quality_t *quality
)
{
    size_t ppg_count;
    size_t valley_count;
    size_t ecg_count;
    size_t wrist_count;
    float p2p;
    float hr;
    uint16_t starts[WK_MAX_BEATS];
    uint16_t ends[WK_MAX_BEATS];
    size_t segments;
    size_t waveform;
    size_t split;
    size_t index;
    double up_dummy[5];
    double down_dummy[10];
    double usdc[6];
    double dsdc[19];
    float *cycle;
    float vpg[WK_CYCLE_POINTS];
    float apg[WK_CYCLE_POINTS];
    size_t peak;
    size_t w;
    size_t y;
    size_t z;
    size_t a, b, c, d, e;
    double tc;
    double sample_seconds;
    double sample_equivalent_125;
    double sw25, dw25, sw50, dw50, sw75, dw75;
    double minimum;
    double s1, s2, s3, s4, aucsys, aucdia;
    double skew, kurt;
    double pwv;
    double nha;
    double ipa;
    double ihar;
    double sv;

    if (ecg == NULL || finger_ppg == NULL || wrist_ppg == NULL
        || work == NULL || features == NULL || quality == NULL) return false;
    memset(features, 0, sizeof(double) * WK_COMPACT_BASE_FEATURE_COUNT);
    memset(quality, 0, sizeof(*quality));
    quality->status = WK_SIGNAL_OK;
    if (!normalize(finger_ppg, work->ppg_normalized, WK_WINDOW_SAMPLES)
        || !normalize(wrist_ppg, work->wrist_normalized, WK_WINDOW_SAMPLES)
        || !normalize(ecg, work->ecg_normalized, WK_WINDOW_SAMPLES)) {
        quality->status = WK_SIGNAL_FLAT;
        return false;
    }

    ppg_count = detect_peaks(work->ppg_normalized, WK_WINDOW_SAMPLES,
                             (size_t)(0.35f * WK_SAMPLE_RATE_HZ), 0.15f, false,
                             work->ppg_peaks, WK_MAX_BEATS);
    valley_count = detect_peaks(work->ppg_normalized, WK_WINDOW_SAMPLES,
                                (size_t)(0.35f * WK_SAMPLE_RATE_HZ), 0.10f, true,
                                work->ppg_valleys, WK_MAX_BEATS + 1u);
    ecg_count = detect_peaks(work->ecg_normalized, WK_WINDOW_SAMPLES,
                             (size_t)(0.30f * WK_SAMPLE_RATE_HZ), 0.35f, false,
                             work->ecg_peaks, WK_MAX_BEATS);
    wrist_count = detect_peaks(work->wrist_normalized, WK_WINDOW_SAMPLES,
                               (size_t)(0.35f * WK_SAMPLE_RATE_HZ), 0.15f, false,
                               work->wrist_peaks, WK_MAX_BEATS);
    quality->ppg_peak_count = (uint16_t)ppg_count;
    quality->ecg_peak_count = (uint16_t)ecg_count;
    quality->finger_wrist_correlation = correlation(
        work->ppg_normalized, work->wrist_normalized, WK_WINDOW_SAMPLES
    );
    quality->rr_cv = interval_cv(work->ecg_peaks, ecg_count);
    if (ppg_count < 3u || valley_count < 3u) {
        quality->status = WK_SIGNAL_TOO_FEW_PPG_BEATS;
        return false;
    }
    if (ecg_count < 3u) {
        quality->status = WK_SIGNAL_TOO_FEW_ECG_BEATS;
        return false;
    }
    p2p = median_interval(work->ppg_peaks, ppg_count);
    hr = p2p > 0.0f ? WK_SAMPLE_RATE_HZ * 60.0f / p2p : 0.0f;
    quality->heart_rate_bpm = hr;
    if (hr < 35.0f || hr > 180.0f) {
        quality->status = WK_SIGNAL_RATE_OUT_OF_RANGE;
        return false;
    }
    if (quality->rr_cv > 0.35f) {
        quality->status = WK_SIGNAL_IRREGULAR_RHYTHM;
        return false;
    }
    quality->quality_score = clampf(
        0.55f * (1.0f - quality->rr_cv / 0.35f)
        + 0.25f * clampf((float)ppg_count / 8.0f, 0.0f, 1.0f)
        + 0.10f * clampf(fabsf(quality->finger_wrist_correlation), 0.0f, 1.0f)
        + 0.10f * clampf((float)wrist_count / 8.0f, 0.0f, 1.0f),
        0.0f, 1.0f
    );

    work->derivatives[0][0] = work->ppg_normalized[0];
    memcpy(work->derivatives[0], work->ppg_normalized,
           sizeof(float) * WK_WINDOW_SAMPLES);
    for (waveform = 1u; waveform < 5u; ++waveform) {
        derivative(work->derivatives[waveform - 1u], work->derivatives[waveform],
                   WK_WINDOW_SAMPLES);
        normalize(work->derivatives[waveform], work->derivatives[waveform],
                  WK_WINDOW_SAMPLES);
    }

    segments = valley_segments(work->ppg_valleys, valley_count, starts, ends);
    quality->usable_cycle_count = (uint16_t)segments;
    if (segments < 2u) {
        quality->status = WK_SIGNAL_FEATURE_FAILURE;
        return false;
    }
    for (waveform = 0u; waveform < 5u; ++waveform) {
        if (!average_resampled_segments(work->derivatives[waveform], starts, ends,
                                        segments, work->valley_cycle[waveform])) {
            quality->status = WK_SIGNAL_FEATURE_FAILURE;
            return false;
        }
    }
    segments = peak_segments(work->ppg_peaks, ppg_count, p2p, starts, ends);
    for (waveform = 0u; waveform < 5u; ++waveform) {
        if (!average_resampled_segments(work->derivatives[waveform], starts, ends,
                                        segments, work->peak_cycle[waveform])) {
            memcpy(work->peak_cycle[waveform], work->valley_cycle[waveform],
                   sizeof(float) * WK_CYCLE_POINTS);
        }
    }

    fft_features(work->ppg_normalized, work->fft_magnitude,
                 &features[WK_F_PPG_FFT_PEAKS_0],
                 &features[WK_F_PPG_FFT_PEAKS_HEIGHTS_0],
                 &features[WK_F_PPG_FFT_PEAKS_NEIGHBOR_AVGS_0]);
    features[WK_F_HR_0] = hr;
    /* The frozen model learned p2p in 125 Hz samples. */
    features[WK_F_P2P_0] = p2p * 125.0 / WK_SAMPLE_RATE_HZ;

    features[WK_F_PPG_MAX_NEIGHBOR_MEAN_0] = neighbor_max_mean(work->peak_cycle[0]);
    features[WK_F_PPG_MIN_0] = argmin(work->peak_cycle[0], WK_CYCLE_POINTS);
    features[WK_F_VPG_MAX_NEIGHBOR_MEAN_0] = neighbor_max_mean(work->peak_cycle[1]);
    features[WK_F_VPG_MIN_0] = argmin(work->peak_cycle[1], WK_CYCLE_POINTS);
    features[WK_F_APG_MAX_NEIGHBOR_MEAN_0] = neighbor_max_mean(work->peak_cycle[2]);
    features[WK_F_APG_MIN_0] = argmin(work->peak_cycle[2], WK_CYCLE_POINTS);
    features[WK_F_PPG3_MAX_NEIGHBOR_MEAN_0] = neighbor_max_mean(work->peak_cycle[3]);
    features[WK_F_PPG3_MIN_0] = argmin(work->peak_cycle[3], WK_CYCLE_POINTS);
    features[WK_F_PPG4_MAX_NEIGHBOR_MEAN_0] = neighbor_max_mean(work->peak_cycle[4]);
    features[WK_F_PPG4_MIN_0] = argmin(work->peak_cycle[4], WK_CYCLE_POINTS);

    split = argmax(work->valley_cycle[0], WK_CYCLE_POINTS);
    for (waveform = 0u; waveform < 5u; ++waveform) {
        histogram(work->valley_cycle[waveform], 0u, split, 5u, up_dummy);
        histogram(work->valley_cycle[waveform], split, WK_CYCLE_POINTS, 10u, down_dummy);
        if (waveform == 0u) {
            for (index = 0u; index < 5u; ++index) features[WK_F_PPG_HISTOGRAM_UP_0 + index] = up_dummy[index];
            for (index = 0u; index < 10u; ++index) features[WK_F_PPG_HISTOGRAM_DOWN_0 + index] = down_dummy[index];
            features[WK_F_PPG_MAX_0] = argmax(work->valley_cycle[0], WK_CYCLE_POINTS);
        } else if (waveform == 1u) {
            for (index = 0u; index < 5u; ++index) features[WK_F_VPG_HISTOGRAM_UP_0 + index] = up_dummy[index];
            for (index = 0u; index < 7u; ++index) features[WK_F_VPG_HISTOGRAM_DOWN_0 + index] = down_dummy[index];
            features[WK_F_VPG_MAX_0] = argmax(work->valley_cycle[1], WK_CYCLE_POINTS);
        } else if (waveform == 2u) {
            for (index = 0u; index < 5u; ++index) features[WK_F_APG_HISTOGRAM_UP_0 + index] = up_dummy[index];
            for (index = 0u; index < 10u; ++index) features[WK_F_APG_HISTOGRAM_DOWN_0 + index] = down_dummy[index];
            features[WK_F_APG_MAX_0] = argmax(work->valley_cycle[2], WK_CYCLE_POINTS);
        } else if (waveform == 3u) {
            for (index = 0u; index < 2u; ++index) features[WK_F_PPG3_HISTOGRAM_UP_0 + index] = up_dummy[index];
            features[WK_F_PPG3_HISTOGRAM_UP_2] = up_dummy[2];
            features[WK_F_PPG3_HISTOGRAM_UP_3] = up_dummy[3];
            features[WK_F_PPG3_HISTOGRAM_UP_4] = up_dummy[4];
            features[WK_F_PPG3_HISTOGRAM_DOWN_0] = down_dummy[0];
            features[WK_F_PPG3_HISTOGRAM_DOWN_1] = down_dummy[1];
            for (index = 3u; index < 10u; ++index) {
                features[WK_F_PPG3_HISTOGRAM_DOWN_3 + index - 3u] = down_dummy[index];
            }
            features[WK_F_PPG3_MAX_0] = argmax(work->valley_cycle[3], WK_CYCLE_POINTS);
        } else {
            for (index = 0u; index < 5u; ++index) features[WK_F_PPG4_HISTOGRAM_UP_0 + index] = up_dummy[index];
            for (index = 1u; index < 10u; ++index) {
                features[WK_F_PPG4_HISTOGRAM_DOWN_1 + index - 1u] = down_dummy[index];
            }
            features[WK_F_PPG4_MAX_0] = argmax(work->valley_cycle[4], WK_CYCLE_POINTS);
        }
    }

    usdc_dsdc(work->valley_cycle[0], usdc, dsdc);
    features[WK_F_USDC_0] = usdc[0];
    features[WK_F_USDC_1] = usdc[1];
    features[WK_F_USDC_2] = usdc[2];
    features[WK_F_USDC_3] = usdc[3];
    features[WK_F_USDC_5] = usdc[5];
    for (index = 0u; index < 19u; ++index) features[WK_F_DSDC_0 + index] = dsdc[index];
    features[WK_F_PPG_MEAN_CYCLES_MATCH_PEAK_59] = work->peak_cycle[0][59];

    cycle = work->valley_cycle[0];
    derivative(cycle, vpg, WK_CYCLE_POINTS);
    derivative(vpg, apg, WK_CYCLE_POINTS);
    peak = argmax(cycle, WK_CYCLE_POINTS);
    w = argmax(vpg, peak > 1u ? peak : 2u);
    y = peak;
    for (index = peak; index < WK_CYCLE_POINTS; ++index) if (vpg[index] < vpg[y]) y = index;
    z = y;
    for (index = y; index < WK_CYCLE_POINTS; ++index) if (vpg[index] > vpg[z]) z = index;
    local_extrema_apg(apg, vpg, peak, &a, &b, &c, &d, &e);

    tc = p2p / WK_SAMPLE_RATE_HZ;
    sample_seconds = tc / WK_CYCLE_POINTS;
    sample_equivalent_125 = tc * 125.0 / WK_CYCLE_POINTS;
    features[WK_F_TC] = tc;
    features[WK_F_TS] = peak * sample_seconds;
    features[WK_F_TD] = (WK_CYCLE_POINTS - peak) * sample_seconds;
    features[WK_F_NEGSTEEPEST] = vpg[y] / (sample_equivalent_125 + WK_EPS);
    features[WK_F_TDIARISE] = z * sample_seconds;
    features[WK_F_DIARISE] = cycle[z];
    features[WK_F_STEEPDIARISE] = vpg[z] / (sample_equivalent_125 + WK_EPS);
    features[WK_F_TSYSTODIARISE] = ((double)z - peak) * sample_seconds;
    features[WK_F_TDIATOEND] = (WK_CYCLE_POINTS - z) * sample_seconds;
    features[WK_F_RATIO] = safe_div(cycle[peak], cycle[z]);
    features[WK_F_TNEGSTEEPEST_NORM] = (double)y / WK_CYCLE_POINTS;
    features[WK_F_TSYSTODIARISE_NORM] = ((double)z - peak) / WK_CYCLE_POINTS;

    width_at_fraction(cycle, peak, 0.25f, &sw25, &dw25);
    width_at_fraction(cycle, peak, 0.50f, &sw50, &dw50);
    width_at_fraction(cycle, peak, 0.75f, &sw75, &dw75);
    features[WK_F_SW25] = sw25 * sample_seconds;
    features[WK_F_DW25] = dw25 * sample_seconds;
    features[WK_F_DW25_NORM] = dw25 / WK_CYCLE_POINTS;
    features[WK_F_SWADDDW25_NORM] = (sw25 + dw25) / WK_CYCLE_POINTS;
    features[WK_F_DWDIVSW25] = safe_div(dw25, sw25);
    features[WK_F_SW50_NORM] = sw50 / WK_CYCLE_POINTS;
    features[WK_F_DW50] = dw50 * sample_seconds;
    features[WK_F_DW50_NORM] = dw50 / WK_CYCLE_POINTS;
    features[WK_F_DWDIVSW50] = safe_div(dw50, sw50);
    features[WK_F_SW75] = sw75 * sample_seconds;
    features[WK_F_SW75_NORM] = sw75 / WK_CYCLE_POINTS;
    features[WK_F_DW75] = dw75 * sample_seconds;
    features[WK_F_DWDIVSW75] = safe_div(dw75, sw75);

    minimum = cycle[argmin(cycle, WK_CYCLE_POINTS)];
    for (index = 0u; index < WK_CYCLE_POINTS; ++index) work->fft_magnitude[index] = cycle[index] - (float)minimum;
    s1 = trapezoid(work->fft_magnitude, 0u, w) * (tc * 125.0 / WK_CYCLE_POINTS);
    s2 = trapezoid(work->fft_magnitude, w, peak) * (tc * 125.0 / WK_CYCLE_POINTS);
    s3 = trapezoid(work->fft_magnitude, peak, z) * (tc * 125.0 / WK_CYCLE_POINTS);
    s4 = trapezoid(work->fft_magnitude, z, WK_CYCLE_POINTS) * (tc * 125.0 / WK_CYCLE_POINTS);
    aucsys = s1 + s2;
    aucdia = s3 + s4;
    features[WK_F_S1] = s1;
    features[WK_F_S2] = s2;
    features[WK_F_S3] = s3;
    features[WK_F_S4] = s4;
    features[WK_F_AUCDIA] = aucdia;
    features[WK_F_S1_NORM] = safe_div(s1, aucsys);
    features[WK_F_S2_NORM] = safe_div(s2, aucsys);
    features[WK_F_S3_NORM] = safe_div(s3, aucdia);
    features[WK_F_S4_NORM] = safe_div(s4, aucdia);
    skew_kurtosis(cycle, WK_CYCLE_POINTS, &skew, &kurt);
    features[WK_F_SQI_SKEW] = skew;
    features[WK_F_SQI_KURTOSIS] = kurt;

    features[WK_F_APG_A] = apg[a] / ((sample_equivalent_125 * sample_equivalent_125) + WK_EPS);
    features[WK_F_APG_B] = apg[b] / ((sample_equivalent_125 * sample_equivalent_125) + WK_EPS);
    features[WK_F_APG_C] = apg[c] / ((sample_equivalent_125 * sample_equivalent_125) + WK_EPS);
    features[WK_F_APG_D] = apg[d] / ((sample_equivalent_125 * sample_equivalent_125) + WK_EPS);
    features[WK_F_APG_E] = apg[e] / ((sample_equivalent_125 * sample_equivalent_125) + WK_EPS);
    features[WK_F_PPG_A] = cycle[a];
    features[WK_F_PPG_B] = cycle[b];
    features[WK_F_PPG_C] = cycle[c];
    features[WK_F_PPG_D] = cycle[d];
    features[WK_F_PPG_E] = cycle[e];
    features[WK_F_RATIO_APG_D] = safe_div(apg[d], apg[a]);
    features[WK_F_RATIO_APG_E] = safe_div(apg[e], apg[a]);
    features[WK_F_RATIO_PPG_B] = safe_div(cycle[b], cycle[a]);
    features[WK_F_T_A] = a * sample_seconds;
    features[WK_F_T_B] = ((double)b - a) * sample_seconds;
    features[WK_F_T_C] = ((double)c - b) * sample_seconds;
    features[WK_F_T_D] = ((double)d - c) * sample_seconds;
    features[WK_F_T_E] = ((double)e - d) * sample_seconds;
    features[WK_F_T_A_NORM] = (double)a / WK_CYCLE_POINTS;
    features[WK_F_T_D_NORM] = ((double)d - c) / WK_CYCLE_POINTS;
    features[WK_F_T_E_NORM] = ((double)e - d) / WK_CYCLE_POINTS;
    features[WK_F_T_PEAK_D] = ((double)d - peak) * sample_seconds;
    features[WK_F_T_PEAK_E] = ((double)e - peak) * sample_seconds;
    features[WK_F_T_PEAK_A_NORM] = ((double)peak - a) / WK_CYCLE_POINTS;
    features[WK_F_T_PEAK_B_NORM] = ((double)peak - b) / WK_CYCLE_POINTS;
    features[WK_F_T_PEAK_C_NORM] = ((double)c - peak) / WK_CYCLE_POINTS;
    features[WK_F_T_PEAK_D_NORM] = ((double)d - peak) / WK_CYCLE_POINTS;
    features[WK_F_T_PEAK_E_NORM] = ((double)e - peak) / WK_CYCLE_POINTS;
    features[WK_F_AI] = safe_div(apg[b] - apg[c] - apg[d] - apg[e], apg[a]);
    features[WK_F_BD] = safe_div(vpg[d] - vpg[b], ((double)d - b) * sample_equivalent_125);
    {
        double numerator = 0.0;
        double denominator = 0.0;
        for (index = peak; index <= d && index < WK_CYCLE_POINTS; ++index) numerator += vpg[index] * vpg[index];
        for (index = 0u; index < WK_CYCLE_POINTS; ++index) denominator += vpg[index] * vpg[index];
        features[WK_F_SDOO] = safe_div(numerator, denominator);
    }

    nha = harmonic_ratio(work->ppg_normalized, hr / 60.0);
    {
        size_t notch = peak + (WK_CYCLE_POINTS - peak) / 2u;
        for (index = peak + 2u; index + 1u < WK_CYCLE_POINTS; ++index) {
            if (vpg[index - 1u] <= 0.0f && vpg[index] > 0.0f) {
                notch = index;
                break;
            }
        }
        ipa = safe_div(trapezoid(cycle, notch, WK_CYCLE_POINTS),
                       trapezoid(cycle, 0u, WK_CYCLE_POINTS));
    }
    ihar = ipa > WK_EPS ? (1.0 - nha) / ipa : 0.0;
    sv = features[WK_F_TS] > WK_EPS ? 4.0 * ihar / features[WK_F_TS] : 0.0;
    pwv = inverse_pat(work->ecg_peaks, ecg_count, work->ppg_peaks, ppg_count);
    if (!isfinite(sv) || sv <= 0.0 || !isfinite(pwv) || pwv <= 0.0) {
        quality->status = WK_SIGNAL_FEATURE_FAILURE;
        return false;
    }
    features[WK_F_SV] = sv;
    features[WK_F_IHAR] = ihar;
    features[WK_F_PWV] = pwv;
    for (index = 0u; index < WK_COMPACT_BASE_FEATURE_COUNT; ++index) {
        if (!isfinite(features[index])) features[index] = 0.0;
    }
#ifndef WK_DISABLE_FEATURE_ADAPTER
    wk_apply_feature_adapter(features);
#endif
    return true;
}

const char *wk_signal_status_name(wk_signal_status_t status)
{
    switch (status) {
        case WK_SIGNAL_OK: return "ok";
        case WK_SIGNAL_NONFINITE: return "nonfinite";
        case WK_SIGNAL_FLAT: return "flat";
        case WK_SIGNAL_TOO_FEW_PPG_BEATS: return "too_few_ppg_beats";
        case WK_SIGNAL_TOO_FEW_ECG_BEATS: return "too_few_ecg_beats";
        case WK_SIGNAL_RATE_OUT_OF_RANGE: return "rate_out_of_range";
        case WK_SIGNAL_IRREGULAR_RHYTHM: return "irregular_rhythm";
        case WK_SIGNAL_POOR_WRIST_CONTACT: return "poor_wrist_contact";
        default: return "feature_failure";
    }
}
