#include "wifi.h"

#include <stdlib.h>
#include "esp_log.h"

void wifi_scan(void)
{
    static const char *TAG = "wifi_scan";

    esp_netif_init();  // 初始化LwIP协议栈，这个协议栈可用来实现TCP/IP协议的应用层、网络层、传输层

    esp_event_loop_create_default(); // 创建默认事件循环，创建一个系统级别的后台任务，用于分发wifi扫描完成、连接成功、IP地址分发等事件

    esp_netif_create_default_wifi_sta(); // 连接LwIP协议与WIFI底层驱动

    //初始化WIFI配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // 采用默认配置
    esp_wifi_init(&cfg); // 初始化wifi
    esp_wifi_set_mode(WIFI_MODE_STA); // 设置为STA模式
    esp_wifi_start(); // 启动WIFI外设

    // 启动wifi扫描，阻塞式
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(ret));
        return;
    }

    // 获取扫描结果
    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count); // 获取扫描的热点数量
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(ret));
        esp_wifi_clear_ap_list();
        return;
    }

    if (ap_count == 0) {
        ESP_LOGI(TAG, "No Wi-Fi access points found");
        esp_wifi_clear_ap_list();
        return;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(*ap_records)); // 开辟用于保存热点信息的结构体存储区域
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for %u AP records",
                 (unsigned)ap_count);
        esp_wifi_clear_ap_list();
        return;
    }

    uint16_t record_count = ap_count;
    ret = esp_wifi_scan_get_ap_records(&record_count, ap_records);   // 获取热点信息
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(ret));
        esp_wifi_clear_ap_list();
        free(ap_records);
        return;
    }

    // 打印热点扫描信息结果
    ESP_LOGI(TAG, "Found %u Wi-Fi access point(s)", (unsigned)record_count);
    for (uint16_t i = 0; i < record_count; i++) {
        const wifi_ap_record_t *ap = &ap_records[i];

        ESP_LOGI(TAG,
                 "[%u] SSID: %-32.32s, RSSI: %d dBm, channel: %u, "
                 "authmode: %d, BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned)(i + 1),
                 ap->ssid[0] != '\0' ? (const char *)ap->ssid : "<hidden>",
                 ap->rssi,
                 (unsigned)ap->primary,
                 (int)ap->authmode,
                 ap->bssid[0], ap->bssid[1], ap->bssid[2],
                 ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    }

    free(ap_records);
}
