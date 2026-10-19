#ifndef WK_REALTIME_BRIDGE_H
#define WK_REALTIME_BRIDGE_H

#include "esp_err.h"

/* Start the 250 Hz causal WK-MK pipeline. Call once after NVS/PSRAM startup. */
esp_err_t WKRealtimeBridge_Start(void);

/* Optional cuff/reference BP observation, applied to the next 1 Hz window. */
esp_err_t WKRealtimeBridge_SubmitReference(double sbp, double dbp);

#endif
