#include "wifi.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

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

// 用于测试连接手机热点
#define PHONE_SSID "REDMI_K80"
#define PHONE_PWD  "lyx030129"  

//WIFI连接事件标志组,官方推荐使用事件标志组（任务同步） + 系统事件（连接管理）的方式来进行联网管理
//注意：WIFI_EVENT系统事件是ESP-IDF的WIFI驱动里面的系统机制，在开启了事件循环之后，会由底层驱动和网络协议栈自动发布WIFI相关的状态，而事件标志组是Freertos的机制
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static uint8_t retry_count;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)     // 扫描到需要开始连接
    {

        esp_wifi_connect();

    } 
    else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED)                // 连接失败，自动重连
    {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;

        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        if (retry_count < 5) {
            retry_count++;
            ESP_LOGW("wifi_sta", "Disconnected (reason=%d), retry %u/5",
                     (int)event->reason, (unsigned)retry_count);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);    // 重连超过过次数 ，失败
            ESP_LOGE("wifi_sta", "Failed to connect after 5 retries (reason=%d)",
                     (int)event->reason);
        }

    } 
    else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP)                 // 获取IP事件，需要获取IP
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        retry_count = 0;
        xEventGroupClearBits(wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI("wifi_sta", "Connected, IP address: " IPSTR,
                 IP2STR(&event->ip_info.ip));
    }

}

void wifi_sta_init(void)
{
    static const char *TAG = "wifi_sta_init";

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return;
    }

    retry_count = 0;

    ESP_ERROR_CHECK(esp_netif_init());  // 初始化LwIP协议栈，这个协议栈可用来实现TCP/IP协议的应用层、网络层、传输层

    ESP_ERROR_CHECK(esp_event_loop_create_default()); // 创建默认事件循环，创建一个系统级别的后台任务，用于分发wifi扫描完成、连接成功、IP地址分发等事件

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta(); // 连接LwIP协议与WIFI底层驱动
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi STA interface");
        return;
    }

    //初始化WIFI配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // 采用默认配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); // 初始化wifi

    // 注册WIFI事件处理函数，用于进行所需要的连接、重连、获取IP与置位连接成功标志位的操作(ESP-IDF中的机制)
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                wifi_event_handler,
                                                NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler,
                                                NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = PHONE_SSID,
            .password = PHONE_PWD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // 设置为STA模式
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start()); // 启动WIFI外设

    ESP_LOGI(TAG, "Connecting to SSID: %s", PHONE_SSID);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            portMAX_DELAY);

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        ESP_LOGI(TAG, "Wi-Fi connection established");
    } else if ((bits & WIFI_FAIL_BIT) != 0) {
        ESP_LOGE(TAG, "Unable to connect to SSID: %s", PHONE_SSID);
    }

}

// 用于测试ESP32作为热点
#define ESP_WIFI_SSID  "ESP32S3LYX"
#define ESP_WIFI_PASSWORD "lyx030129"
#define ESP_WIFI_CONN 2
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"

//wifi热点回调函数
static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)//需要使用日志打印出连接设备的MAC地址(通过上面的宏定义转换)与AID
    {
        const wifi_event_ap_staconnected_t *event =
            (const wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI("wifi_ap", "Station " MACSTR " connected, AID=%u",
                 MAC2STR(event->mac), (unsigned)event->aid);
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)//需要通过日志输出设备断开提示
    {
        const wifi_event_ap_stadisconnected_t *event =
            (const wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI("wifi_ap", "Station " MACSTR
                 " disconnected, AID=%u, reason=%u",
                 MAC2STR(event->mac), (unsigned)event->aid,
                 (unsigned)event->reason);
    }
}

void wifi_init_softap(void)//初始化esp为热点
{
    static const char *TAG = "wifi_ap_init";
  
    retry_count = 0;

    ESP_ERROR_CHECK(esp_netif_init());  // 初始化LwIP协议栈，这个协议栈可用来实现TCP/IP协议的应用层、网络层、传输层

    ESP_ERROR_CHECK(esp_event_loop_create_default()); // 创建默认事件循环，创建一个系统级别的后台任务，用于分发wifi扫描完成、连接成功、IP地址分发等事件

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap(); // 连接LwIP协议与WIFI底层驱动
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi AP interface");
        return;
    }

    //初始化WIFI配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // 采用默认配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); // 初始化wifi

    // 注册WIFI热点连接事件处理函数，用于进行WiFi热点接入设备和设备断开时候的操作(ESP-IDF中的机制)
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                wifi_ap_event_handler,
                                                NULL));
                                        
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .password = ESP_WIFI_PASSWORD,
            .max_connection = ESP_WIFI_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if(strlen(ESP_WIFI_PASSWORD) == 0) // 无密码
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));//设置为AP模式
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start()); // 启动WIFI外设

    //打印出本ESP32的MAC地址
    uint8_t ap_mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, ap_mac));
    ESP_LOGI(TAG, "SoftAP started, SSID=%s, MAC=" MACSTR,
             ESP_WIFI_SSID, MAC2STR(ap_mac));
}
