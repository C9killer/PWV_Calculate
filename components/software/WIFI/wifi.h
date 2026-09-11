#ifndef  __WIFI_H_
#define  __WIFI_H_

#include "esp_wifi.h"

void wifi_scan(void);
void wifi_sta_init(void);
void wifi_init_softap(void);

#endif // ! 
