#include "esp_log.h"
// #include "addr_from_stdin.h"
#include "bm1397.h"
#include "connect.h"
#include "global_state.h"
#include "lwip/dns.h"
#include "nvs_config.h"
#include "stratum_task.h"
#include "work_queue.h"
#include "esp_wifi.h"
#include <esp_sntp.h>
#include "esp_timer.h"
#include <time.h>

static const char * TAG = "stratum_task";
static ip_addr_t ip_Addr;
static bool bDNSFound = false;
static bool bDNSInvalid = false;

static StratumApiV1Message stratum_api_v1_message = {};

static stratumlinshidiff stratum_linshi_diffMODULE = {.stratum_difficulty = 256};

void dns_found_cb(const char * name, const ip_addr_t * ipaddr, void * callback_arg)
{
    bDNSFound = true;
    if (ipaddr != NULL) {
        ip_Addr = *ipaddr;
    } else {
        bDNSInvalid = true;
    }
}

bool is_wifi_connected() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return true;
    } else {
        return false;
    }
}

void stratum_task(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    STRATUM_V1_initialize_buffer();
    char host_ip[20];
    int addr_family = 0;
    int ip_protocol = 0;

    char * stratum_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, "");
    uint16_t stratum_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, "");

    // check to see if the STRATUM_URL is an ip address already
    if (inet_pton(AF_INET, stratum_url, &ip_Addr) == 1) {
        bDNSFound = true;
    } else {
        ESP_EARLY_LOGI(TAG, "Get IP for URL: %s\n", stratum_url);
        dns_gethostbyname(stratum_url, &ip_Addr, dns_found_cb, NULL);
        while (!bDNSFound)
            ;

        if (bDNSInvalid) {
            ESP_LOGE(TAG, "DNS lookup failed for URL: %s\n", stratum_url);
            // set ip_Addr to 0.0.0.0 so that connect() will fail
            IP_ADDR4(&ip_Addr, 0, 0, 0, 0);
        }
    }

    // make IP address string from ip_Addr
    snprintf(host_ip, sizeof(host_ip), "%d.%d.%d.%d", ip4_addr1(&ip_Addr.u_addr.ip4), ip4_addr2(&ip_Addr.u_addr.ip4),
             ip4_addr3(&ip_Addr.u_addr.ip4), ip4_addr4(&ip_Addr.u_addr.ip4));
    ESP_LOGI(TAG, "Connecting to: stratum+tcp://%s:%d (%s)\n", stratum_url, stratum_port, host_ip);

    while (1) {

            if (!is_wifi_connected()) {
                ESP_LOGI(TAG, "WiFi disconnected, attempting to reconnect...");
                esp_wifi_connect();
                vTaskDelay(10000 / portTICK_PERIOD_MS);
                //delay_ms *= 2; // Increase delay exponentially
                continue;
            }


        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(host_ip);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(stratum_port);

        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;

        GLOBAL_STATE->sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (GLOBAL_STATE->sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            //esp_restart();
            continue;
        }
        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, stratum_port);

        int err = connect(GLOBAL_STATE->sock, (struct sockaddr *) &dest_addr, sizeof(struct sockaddr_in));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect to %s:%d (errno %d)", stratum_url, stratum_port, errno);
            // close the socket
            shutdown(GLOBAL_STATE->sock, SHUT_RDWR);
            close(GLOBAL_STATE->sock);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "Socket connect success,so sendding...subscribe");
        // send mining.subscribe  get extranonce1  extranonce2_size
        STRATUM_V1_subscribe(GLOBAL_STATE->sock, "bitdsk_d12");

        // mining.configure
        STRATUM_V1_configure_version_rolling(GLOBAL_STATE->sock, &GLOBAL_STATE->version_mask);

        // This should come before the final step of authenticate so the first job is sent with the proper difficulty set
        // mining.suggest_difficulty
        // STRATUM_V1_suggest_difficulty(GLOBAL_STATE->sock, );
        STRATUM_V1_suggest_difficulty(GLOBAL_STATE->sock, 256);

        /// auth
        STRATUM_V1_authenticate(GLOBAL_STATE->sock, nvs_config_get_string(NVS_CONFIG_STRATUM_USER, ""),
                                nvs_config_get_string(NVS_CONFIG_STRATUM_PASS, ""));

        // ESP_LOGI(TAG, "Extranonce: %s", GLOBAL_STATE->extranonce1);
        // ESP_LOGI(TAG, "Extranonce 2 length: %d", GLOBAL_STATE->extranonce2_size);
    
        while (1) {
            char * line = STRATUM_V1_receive_jsonrpc_line(GLOBAL_STATE->sock);

            ///////////////
            ESP_EARLY_LOGI(TAG, "rx: %s", line); // debug incoming stratum messages
           ////////////////////

            STRATUM_V1_parse(&stratum_api_v1_message, line);
            free(line);

            if (stratum_api_v1_message.method == MINING_NOTIFY) {
                ESP_EARLY_LOGI(TAG, "New MINING_NOTIFY ");
                // SYSTEM_notify_new_ntime(&GLOBAL_STATE->SYSTEM_MODULE, stratum_api_v1_message.mining_notification->ntime);

                if (stratum_api_v1_message.mining_notification->should_abandon_work &&
                    (GLOBAL_STATE->stratum_queue.count > 0 || GLOBAL_STATE->ASIC_jobs_queue.count > 0)) {
                    //ESP_LOGI(TAG, "abandoning work");

                    GLOBAL_STATE->abandon_work = 1;

                    queue_clear(&GLOBAL_STATE->stratum_queue);
                    // ESP_LOGI(TAG, "queue_clear end");
                    ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
                    // ESP_LOGI(TAG, "ASIC_jobs_queue_clear end");

                    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
                    for (int i = 0; i < 128; i++) {
                        if (GLOBAL_STATE->valid_jobs[i] == 1){
                            GLOBAL_STATE->valid_jobs[i] = 0;
                        }
                        
                    }
                    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
                    // ESP_LOGI(TAG, "GLOBAL_STATE->valid_jobs end");

                    GLOBAL_STATE->abandon_work = 0;
                }else if (GLOBAL_STATE->stratum_queue.count > 0)
                {
                    GLOBAL_STATE->abandon_work = 1;
                    queue_clear(&GLOBAL_STATE->stratum_queue);
                    GLOBAL_STATE->abandon_work = 0;
                }
                

                stratum_api_v1_message.mining_notification->difficulty = stratum_linshi_diffMODULE.stratum_difficulty;
                queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification);

                /////////////get networkdiff
                SYSTEM_update_net_diff(&GLOBAL_STATE->SYSTEM_MODULE, stratum_api_v1_message.mining_notification->target);

                //update last_mining_notify_time time
                GLOBAL_STATE->last_mining_notify_time = esp_timer_get_time();

            } else if (stratum_api_v1_message.method == MINING_SET_DIFFICULTY) {

                if ((int) stratum_api_v1_message.new_difficulty > 0) {
                    ESP_LOGI(TAG, "Set new difficulty %ld", stratum_api_v1_message.new_difficulty);
                    (*GLOBAL_STATE->ASIC_functions.set_difficulty_mask_fn)((int)stratum_api_v1_message.new_difficulty);

                    if (stratum_api_v1_message.new_difficulty != stratum_linshi_diffMODULE.stratum_difficulty) {
                        stratum_linshi_diffMODULE.stratum_difficulty = stratum_api_v1_message.new_difficulty;
                       // ESP_LOGI(TAG, "Set stratum difficulty: %ld", stratum_linshi_diffMODULE.stratum_difficulty);
                    }

                    if (stratum_api_v1_message.new_difficulty != GLOBAL_STATE->stratum_difficulty) {
                        GLOBAL_STATE->stratum_difficulty = stratum_api_v1_message.new_difficulty;
                       // ESP_LOGI(TAG, "Set GLOBAL_STATE->stratum_difficulty: %ld", GLOBAL_STATE->stratum_difficulty);
                    }
                }

            } else if (stratum_api_v1_message.method == MINING_SET_VERSION_MASK ||
                       stratum_api_v1_message.method == STRATUM_RESULT_VERSION_MASK) {
                // 1fffe000
                ESP_LOGI(TAG, "Set version mask: %08lx", stratum_api_v1_message.version_mask);
                GLOBAL_STATE->version_mask = stratum_api_v1_message.version_mask;

            } else if (stratum_api_v1_message.method == STRATUM_RESULT) {
                if (stratum_api_v1_message.response_success) {
                    ESP_EARLY_LOGI(TAG, "POOL Result -> Accepted");
                    SYSTEM_notify_accepted_share(&GLOBAL_STATE->SYSTEM_MODULE);
                } else {
                    ESP_EARLY_LOGW(TAG, "POOL Result -> Rejected");
                    SYSTEM_notify_rejected_share(&GLOBAL_STATE->SYSTEM_MODULE);
                }
            }else{

                
                // Calculate the uptime in seconds
                // double uptime_in_seconds = (esp_timer_get_time() - module->start_time) / 1000000;
                // int uptime_in_days = uptime_in_seconds / (3600 * 24);
                // int remaining_seconds = (int) uptime_in_seconds % (3600 * 24);
                // int uptime_in_hours = remaining_seconds / 3600;
                // remaining_seconds %= 3600;
                // int uptime_in_minutes = remaining_seconds / 60;

                
                if ( GLOBAL_STATE->last_mining_notify_time != 0 )
                {
                    int nouptime_in_seconds =(int)(esp_timer_get_time() - GLOBAL_STATE->last_mining_notify_time) / 1000000;
                    
                    if (nouptime_in_seconds >= 120)
                    {
                        ESP_LOGE(TAG, "Sock Link is down,relink... Please check wifi....");
                        GLOBAL_STATE->last_mining_notify_time=0;


                        //

                        GLOBAL_STATE->abandon_work = 1;

                        queue_clear(&GLOBAL_STATE->stratum_queue);
                        ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);

                        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
                        for (int i = 0; i < 128; i++) {
                            if (GLOBAL_STATE->valid_jobs[i] == 1){
                                GLOBAL_STATE->valid_jobs[i] = 0;
                            }
                            
                        }
                        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);


                        GLOBAL_STATE->abandon_work = 0;





                        break;
                    }
                    
                }


            }



            



            vTaskDelay(1);
        }

        if (GLOBAL_STATE->sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(GLOBAL_STATE->sock, 0);
            close(GLOBAL_STATE->sock);
        }
    }




    vTaskDelete(NULL);
    

}
