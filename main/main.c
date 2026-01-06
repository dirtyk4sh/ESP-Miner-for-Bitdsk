
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

// #include "protocol_examples_common.h"
#include "main.h"

#include "asic_result_task.h"
#include "asic_task.h"
#include "create_jobs_task.h"
#include "esp_netif.h"
#include "global_state.h"
#include "http_server.h"
#include "nvs_config.h"
#include "serial.h"
#include "stratum_task.h"
#include "user_input_task.h"

static GlobalState GLOBAL_STATE = {.extranonce1 = NULL, .extranonce2_size = 0, .abandon_work = 0, .version_mask = 0,.board_ip=NULL,.board_gw=NULL, .last_mining_notify_time = 0};

static const char * TAG = "miner";

void app_main(void)
{

    GLOBAL_STATE.board_ip =malloc(20);
    GLOBAL_STATE.board_gw =malloc(20);
    // gpio_set_intr_type
    // gpio_set_direction(GPIO_NUM_17, GPIO_MODE_OUTPUT);
    // gpio_set_level(GPIO_NUM_17, 1);

    // gpio_set_direction(GPIO_NUM_18, GPIO_MODE_OUTPUT);
    // gpio_set_level(GPIO_NUM_18, 1);

    // gpio_set_direction(GPIO_NUM_1, GPIO_MODE_OUTPUT);
    // gpio_set_level(GPIO_NUM_1, 1);

    gpio_reset_pin(GPIO_NUM_17);
    gpio_reset_pin(GPIO_NUM_18);
    gpio_reset_pin(GPIO_NUM_1);
    ////
    ESP_ERROR_CHECK(nvs_flash_init());
    /////////////////////////////////



    // ESP_LOGI(TAG, "NVS_CONFIG_ASIC_FREQ %f", (float) nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, 300));
    //  start freq  以及  asic freq
    GLOBAL_STATE.POWER_MANAGEMENT_MODULE.frequency_start_value = nvs_config_get_u16(NVS_CONFIG_ASIC_STARTFREQ, 200);
    GLOBAL_STATE.POWER_MANAGEMENT_MODULE.frequency_value = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, 300);


    GLOBAL_STATE.staticipopen = nvs_config_get_u16(NVS_CONFIG_STATIC_IP_OPEN, 0);
    GLOBAL_STATE.staticip = nvs_config_get_string(NVS_CONFIG_STATIC_IP, "");
    GLOBAL_STATE.staticnetmask = nvs_config_get_string(NVS_CONFIG_STATIC_NETMASK, "");
    GLOBAL_STATE.staticopengw = nvs_config_get_string(NVS_CONFIG_STATIC_GW, "");

    // 得到asic_model  bm1397  bm1366

    char * caches_str=nvs_config_get_string(NVS_CONFIG_ASIC_MODEL, "BM1397");
    GLOBAL_STATE.asic_model =malloc(strlen(caches_str)+1);
    memcpy(GLOBAL_STATE.asic_model,caches_str,strlen(caches_str)+1);
    free(caches_str);

    //GLOBAL_STATE.asic_model = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL, "BM1397");
    if (strcmp(GLOBAL_STATE.asic_model, "BM1366") == 0) {
        // ESP_LOGI(TAG, "ASIC: BM1366");
        AsicFunctions ASIC_functions = {.init_fn = BM1366_init,
                                        .receive_result_fn = BM1366_proccess_work,
                                        .set_max_baud_fn = BM1366_set_max_baud,
                                        .set_difficulty_mask_fn = BM1366_set_job_difficulty_mask,
                                        .send_work_fn = BM1366_send_work};
        GLOBAL_STATE.asic_job_frequency_ms = ASIC_SCAN_INTERVAL_MS;

        GLOBAL_STATE.ASIC_functions = ASIC_functions;
    } else if (strcmp(GLOBAL_STATE.asic_model, "BM1397") == 0) {
        // ESP_LOGI(TAG, "ASIC: BM1397");
        AsicFunctions ASIC_functions = {.init_fn = BM1397_init,
                                        .receive_result_fn = BM1397_proccess_work,
                                        //.set_max_baud_fn = BM1397_set_max_baud,
                                        .set_max_baud_fn = BM1397_set_default_baud,
                                        .set_difficulty_mask_fn = BM1397_set_job_difficulty_mask,
                                        .send_work_fn = BM1397_send_work};

        uint64_t bm1397_hashrate = GLOBAL_STATE.POWER_MANAGEMENT_MODULE.frequency_value * BM1397_CORE_COUNT * 1000000;
        // ESP_LOGI(TAG, "bm1397_hashrate:::::::::::::::::::::::::::%f", (float) bm1397_hashrate);

        GLOBAL_STATE.asic_job_frequency_ms = ((double) NONCE_SPACE / (double) bm1397_hashrate) * 1000;

        GLOBAL_STATE.ASIC_functions = ASIC_functions;
    } else {
        ESP_LOGE(TAG, "Invalid ASIC model");
        AsicFunctions ASIC_functions = {.init_fn = NULL,
                                        .receive_result_fn = NULL,
                                        .set_max_baud_fn = NULL,
                                        .set_difficulty_mask_fn = NULL,
                                        .send_work_fn = NULL};
        GLOBAL_STATE.ASIC_functions = ASIC_functions;
    }

    xTaskCreate(SYSTEM_task, "SYSTEM_task", 4096, (void *) &GLOBAL_STATE, 3, NULL);
     
    ESP_EARLY_LOGI(TAG, "Bitdsk Running");

    // pull the wifi credentials out of NVS
    // char * wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, WIFI_SSID);
    // char * wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS, WIFI_PASS);
    char * wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, "");
    char * wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS, "");
    char * hostname  = nvs_config_get_string(NVS_CONFIG_HOSTNAME, "bitdsk");

    char * dnsmain = nvs_config_get_string(NVS_CONFIG_DNS_MAIN, "");
    char * dnsbackup = nvs_config_get_string(NVS_CONFIG_DNS_BACKUP, "");
    char * dnsfallback  = nvs_config_get_string(NVS_CONFIG_DNS_FALLBACK, "");

    // copy the wifi ssid to the global state
    strncpy(GLOBAL_STATE.SYSTEM_MODULE.ssid, wifi_ssid, 20);
    

    // init and connect to wifi
    wifi_init(wifi_ssid, wifi_pass,hostname,
            GLOBAL_STATE.board_ip,GLOBAL_STATE.board_gw,
            GLOBAL_STATE.staticipopen,GLOBAL_STATE.staticip,GLOBAL_STATE.staticnetmask,GLOBAL_STATE.staticopengw,
            dnsmain,dnsbackup,dnsfallback
            );
    start_rest_server((void *) &GLOBAL_STATE);
    EventBits_t result_bits = wifi_connect();

    if (result_bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID: %s", wifi_ssid);
        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "Connected!", 20);
    } else if (result_bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", wifi_ssid);

        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "Failed to connect", 20);
        // User might be trying to configure with AP, just chill here
        ESP_EARLY_LOGI(TAG, "Finished, waiting for user input.");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "unexpected error", 20);
        // User might be trying to configure with AP, just chill here
        ESP_EARLY_LOGI(TAG, "Finished, waiting for user input.");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    free(wifi_ssid);
    free(wifi_pass);
    free(hostname);

    free(dnsmain);
    free(dnsbackup);
    free(dnsfallback);
    // set the startup_done flag
    GLOBAL_STATE.SYSTEM_MODULE.startup_done = true;

    xTaskCreate(USER_INPUT_task, "user input", 8192, (void *) &GLOBAL_STATE, 5, NULL);

    if (GLOBAL_STATE.ASIC_functions.init_fn != NULL) {
        wifi_softap_off();
        // gpio_hold_dis(GPIO_NUM_17);
        // gpio_hold_dis(GPIO_NUM_18);
        // gpio_hold_dis(GPIO_NUM_1);

        queue_init(&GLOBAL_STATE.stratum_queue);
        queue_init(&GLOBAL_STATE.ASIC_jobs_queue);

        //
        SERIAL_init();
        //
        // 修改 frequency_value 为 frequency_start_value
        // (*GLOBAL_STATE.ASIC_functions.init_fn)(GLOBAL_STATE.POWER_MANAGEMENT_MODULE.frequency_value);
        (*GLOBAL_STATE.ASIC_functions.init_fn)(GLOBAL_STATE.POWER_MANAGEMENT_MODULE.frequency_start_value);
        //
        xTaskCreate(POWER_MANAGEMENT_task, "power mangement", 8192, (void *) &GLOBAL_STATE, 10, NULL);


        xTaskCreate(stratum_task, "stratum task", 8192, (void *) &GLOBAL_STATE, 5, NULL);
        xTaskCreate(create_jobs_task, "creat jobs", 8192, (void *) &GLOBAL_STATE, 10, NULL);

        //

        xTaskCreate(ASIC_task, "asic task", 8192, (void *) &GLOBAL_STATE, 10, NULL);
        xTaskCreate(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL);

        // while (1)
        // {
        //     if(GLOBAL_STATE.ASIC_jobs_queue.count>0){
        //         xTaskCreate(POWER_MANAGEMENT_task, "power mangement", 8192, (void *) &GLOBAL_STATE, 10, NULL);
        //         break;
        //     }else{
        //         vTaskDelay(5000);
        //     }


        //     vTaskDelay(1);
        // }


    }

    



}

void MINER_set_wifi_status(wifi_status_t status, uint16_t retry_count)
{
    if (status == WIFI_RETRYING) {
        snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Retrying: %d/%d", retry_count, CONFIG_ESP_MAXIMUM_RETRY);
        return;
    } else if (status == WIFI_CONNECT_FAILED) {
        snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Connect Failed!");
        return;
    }
}
