#ifndef  __WIFI_H_
#define  __WIFI_H_

#include <stddef.h>
#include "esp_wifi.h"

/* Keep disabled for power measurements without radio activity.
 * Set to 1 to restore SoftAP startup and protocol forwarding over Wi-Fi. */
#define ENABLE_WIFI 1

#define WIFI_DATA_PORT 3333

void wifi_scan(void);
void wifi_sta_init(void);
void wifi_init_softap(void);
esp_err_t wifi_send_data(const char *data, size_t length);

#endif // ! 
