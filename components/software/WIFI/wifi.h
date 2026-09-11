#ifndef  __WIFI_H_
#define  __WIFI_H_

#include <stddef.h>
#include "esp_wifi.h"

#define WIFI_DATA_PORT 3333

void wifi_scan(void);
void wifi_sta_init(void);
void wifi_init_softap(void);
esp_err_t wifi_send_data(const char *data, size_t length);

#endif // ! 
