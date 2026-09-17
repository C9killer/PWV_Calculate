/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "LED.h"
#include "PWR.h"
#include "ACQ.h"
#include "wifi.h"
#include "nvs_flash.h"      
#include "esp_log.h"   
#include "W25Q64.h"
#include "ModelStorage.h"
#include "wk_edge_inference.h"

/* Deliberately static: this 7.4 KiB inference state must not consume app_main's stack. */
static wk_edge_state_t s_wk_models;

void app_main(void)
{
    // 初始化NVS,用于存储的自动修复
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    printf("Hello world!\n");
    W25Q64_Init();
    ESP_ERROR_CHECK(ModelStorage_Init());
    ret = wk_edge_state_init(&s_wk_models);
    if (ret == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGW("main", "WK model partition is empty/invalid; flash mimic_iii_wk_mk_f64.bin before inference");
    } else if (ret != ESP_OK) {
        ESP_LOGE("main", "WK model initialization failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI("main", "WK model runtime ready (185 compact features; flash-mapped on demand)");
    }
    // LED_Init();
    wifi_init_softap();

    
    // while (1)
    // {
    //     /* code */
    //     LED_Toggle();
    //     vTaskDelay(500);
    // }

    /* 用于PWV的采集 不要动这些注释的部分*/
    esp_err_t err = PWR_Init();
    if (err != ESP_OK) {
        /* Stop peripheral startup without entering a reset loop via ESP32 EN. */
        ESP_LOGE("main", "Power initialization failed: %s", esp_err_to_name(err));
        return;
    }

    // err = ACQ_Start();
    // if (err != ESP_OK) {
    //     ESP_LOGE("main", "Acquisition startup failed: %s", esp_err_to_name(err));
    //     return;
    // }

    /* ACQ owns SPI and prints samples from its worker tasks. */
    
}
