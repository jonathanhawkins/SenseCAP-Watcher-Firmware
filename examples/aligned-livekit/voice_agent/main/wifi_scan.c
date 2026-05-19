/**
 * @file wifi_scan.c
 * @brief WiFi scanning and connection management implementation
 */

#include "wifi_scan.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi_scan";

/* Multi-network credential store.
 *
 * Stored in a dedicated NVS namespace separate from the IDF-provided WiFi
 * config so users can hop between known networks without retyping passwords
 * on the cramped on-screen keyboard. LRU semantics: slot 0 is the most
 * recently used. New successful connections shift the rest down by one and
 * occupy slot 0; if the SSID was already known, its older slot is removed
 * and re-promoted to 0. Storage cost: 8 slots * ~100 bytes ~= 800 B, well
 * within the default 24 KB nvs partition.
 *
 * Keys per slot N (0..WIFI_MAX_SAVED_NETWORKS-1):
 *   ssid_N : null-terminated SSID (empty string = empty slot)
 *   pass_N : null-terminated password (empty allowed for OPEN networks)
 */
#define WIFI_CREDS_NAMESPACE "wifi_creds"
#define WIFI_CREDS_SSID_KEY_FMT "ssid_%d"
#define WIFI_CREDS_PASS_KEY_FMT "pass_%d"

static nvs_handle_t s_creds_nvs = 0;
static bool s_creds_nvs_open = false;
static SemaphoreHandle_t s_creds_mutex = NULL;

/* Pending credentials captured at wifi_connect() time. We only commit them
 * to the multi-network store once GOT_IP confirms the connection actually
 * works — saving an unverified password would let a typo permanently shadow
 * the right credentials for that SSID. */
static char s_pending_ssid[WIFI_SSID_MAX_LEN] = {0};
static char s_pending_password[WIFI_PASSWORD_MAX_LEN] = {0};

/* Forward decls for the multi-network store helpers used by the event handler. */
static void creds_store_open_locked(void);
static bool creds_save_locked(const char *ssid, const char *password);

/* Event bits for connection state */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_SCAN_DONE_BIT  BIT2

/* Static storage for scan results */
static wifi_scan_result_t s_scan_results[WIFI_SCAN_MAX_AP];
static volatile uint16_t s_scan_count = 0;
static volatile bool s_scan_in_progress = false;
static volatile bool s_scan_complete = false;

/* Connection state */
static volatile wifi_connection_state_t s_connection_state = WIFI_STATE_DISCONNECTED;
static char s_current_ssid[WIFI_SSID_MAX_LEN] = {0};
static char s_current_ip[WIFI_IP_MAX_LEN] = {0};

/* Synchronization */
static EventGroupHandle_t s_wifi_event_group = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;
static bool s_initialized = false;
static int s_retry_count = 0;
static bool s_was_connected = false;  /* Track if we were previously connected (for auto-reconnect) */
static volatile int s_last_disconnect_reason = 0;  /* Last WIFI_EVENT_STA_DISCONNECTED reason code */

/*
 * Retry tuning: phone hotspots (esp. iPhone Maximize Compatibility, Android)
 * can take 5-10+ seconds to actually accept a DHCP client. Bumping retries
 * from 10 to 15 gives ~50% more headroom, and a short 200 ms delay between
 * initial-connect retries prevents pathological tight-loop retries against
 * a slow AP. The longer 2 s WIFI_RECONNECT_DELAY_MS is reserved for the
 * post-disconnect reconnect path (give the AP time to come back).
 */
#define WIFI_MAX_RETRIES        15    /* Max retries before giving up */
#define WIFI_INITIAL_RETRY_DELAY_MS 200 /* Delay between retries during initial connect */
#define WIFI_RECONNECT_DELAY_MS 2000  /* Delay before auto-reconnect attempt */

/* Event handler instances */
static esp_event_handler_instance_t s_wifi_handler_instance = NULL;
static esp_event_handler_instance_t s_ip_handler_instance = NULL;

/* Task for processing scan results (to avoid stack overflow on sys_evt task) */
static TaskHandle_t s_scan_process_task = NULL;
static volatile bool s_scan_results_pending = false;

/* Forward declaration */
static void process_scan_results(void);

/**
 * @brief Task that processes scan results off the sys_evt task stack
 *
 * This task waits for notification from the event handler and processes
 * scan results with its own adequate stack allocation.
 */
static void scan_process_task(void *arg)
{
    for (;;)
    {
        /* Wait indefinitely for notification from event handler */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_scan_results_pending)
        {
            s_scan_results_pending = false;
            process_scan_results();

            /* Signal that scan is complete */
            if (s_wifi_event_group)
            {
                xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
            }
        }
    }
}

/**
 * @brief Compare function for sorting networks by RSSI (descending)
 */
static int compare_rssi(const void *a, const void *b)
{
    const wifi_scan_result_t *ap_a = (const wifi_scan_result_t *)a;
    const wifi_scan_result_t *ap_b = (const wifi_scan_result_t *)b;
    /* Sort descending (stronger signal first) */
    return (int)ap_b->rssi - (int)ap_a->rssi;
}

/**
 * @brief Convert ESP-IDF auth mode to simplified auth type
 */
static wifi_scan_auth_t convert_authmode(wifi_auth_mode_t authmode)
{
    switch (authmode)
    {
        case WIFI_AUTH_OPEN:
            return WIFI_SCAN_AUTH_OPEN;
        case WIFI_AUTH_WEP:
            return WIFI_SCAN_AUTH_WEP;
        default:
            /* WPA, WPA2, WPA3, WPA2_ENTERPRISE, etc. */
            return WIFI_SCAN_AUTH_WPA;
    }
}

/**
 * @brief Process scan results into our format
 */
static void process_scan_results(void)
{
    esp_err_t err;
    wifi_ap_record_t ap_records[WIFI_SCAN_MAX_AP];
    uint16_t ap_count = WIFI_SCAN_MAX_AP;

    /* Get the scan results */
    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get scan records: %s", esp_err_to_name(err));
        s_scan_count = 0;
        return;
    }

    ESP_LOGI(TAG, "Scan found %d networks", ap_count);

    /* Take the mutex to update results */
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take scan mutex");
        return;
    }

    /* Copy results to our format */
    s_scan_count = 0;
    for (uint16_t i = 0; i < ap_count && s_scan_count < WIFI_SCAN_MAX_AP; i++)
    {
        /* Skip networks with empty SSID (hidden networks) */
        if (ap_records[i].ssid[0] == '\0')
        {
            continue;
        }

        /* Copy SSID */
        strncpy(s_scan_results[s_scan_count].ssid, (char *)ap_records[i].ssid, WIFI_SSID_MAX_LEN - 1);
        s_scan_results[s_scan_count].ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

        /* Copy RSSI */
        s_scan_results[s_scan_count].rssi = ap_records[i].rssi;

        /* Convert auth mode */
        s_scan_results[s_scan_count].authmode = convert_authmode(ap_records[i].authmode);

        /* Check if this is the currently connected network */
        s_scan_results[s_scan_count].is_connected =
            (s_connection_state == WIFI_STATE_CONNECTED) &&
            (strcmp(s_scan_results[s_scan_count].ssid, s_current_ssid) == 0);

        s_scan_count++;
    }

    /* Sort by RSSI (strongest first) */
    if (s_scan_count > 1)
    {
        qsort(s_scan_results, s_scan_count, sizeof(wifi_scan_result_t), compare_rssi);
    }

    xSemaphoreGive(s_scan_mutex);

    /* Log the results */
    for (uint16_t i = 0; i < s_scan_count && i < 5; i++)
    {
        ESP_LOGI(TAG, "  [%d] %s (%d dBm) %s%s",
                 i + 1,
                 s_scan_results[i].ssid,
                 s_scan_results[i].rssi,
                 s_scan_results[i].authmode == WIFI_SCAN_AUTH_OPEN ? "Open" : "Secured",
                 s_scan_results[i].is_connected ? " *connected*" : "");
    }
    if (s_scan_count > 5)
    {
        ESP_LOGI(TAG, "  ... and %d more networks", s_scan_count - 5);
    }
}

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "WiFi scan completed");
                s_scan_in_progress = false;
                s_scan_complete = true;
                /* Defer processing to dedicated task to avoid stack overflow */
                s_scan_results_pending = true;
                if (s_scan_process_task)
                {
                    xTaskNotifyGive(s_scan_process_task);
                }
                break;

            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;

            case WIFI_EVENT_STA_CONNECTED:
            {
                wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
                strncpy(s_current_ssid, (char *)event->ssid, WIFI_SSID_MAX_LEN - 1);
                s_current_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
                ESP_LOGI(TAG, "Connected to %s", s_current_ssid);
                s_connection_state = WIFI_STATE_CONNECTING; /* Wait for IP */
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED:
            {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGI(TAG, "Disconnected from WiFi (reason: %d)", event->reason);
                s_last_disconnect_reason = event->reason;

                s_current_ssid[0] = '\0';
                s_current_ip[0] = '\0';

                if (s_connection_state == WIFI_STATE_CONNECTING)
                {
                    /* Retry connection during initial connect */
                    if (s_retry_count < WIFI_MAX_RETRIES)
                    {
                        s_retry_count++;
                        ESP_LOGI(TAG, "Retry connection (%d/%d)", s_retry_count, WIFI_MAX_RETRIES);
                        /* Small delay so we don't hammer a slow AP (e.g. phone
                         * hotspot still negotiating). Reserved 2 s delay is
                         * only used for the auto-reconnect path below. */
                        vTaskDelay(pdMS_TO_TICKS(WIFI_INITIAL_RETRY_DELAY_MS));
                        esp_wifi_connect();
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Connection failed after %d retries", WIFI_MAX_RETRIES);
                        s_connection_state = WIFI_STATE_FAILED;
                        if (s_wifi_event_group)
                        {
                            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                        }
                    }
                }
                else if (s_was_connected)
                {
                    /* We were previously connected - auto-reconnect */
                    ESP_LOGI(TAG, "Lost connection, auto-reconnecting in %dms...", WIFI_RECONNECT_DELAY_MS);
                    s_connection_state = WIFI_STATE_CONNECTING;
                    s_retry_count = 0;
                    /* Small delay to let the AP recover if it rebooted */
                    vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));
                    esp_wifi_connect();
                }
                else
                {
                    s_connection_state = WIFI_STATE_DISCONNECTED;
                }
                break;
            }

            default:
                break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_current_ip, WIFI_IP_MAX_LEN, IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s", s_current_ip);
            s_connection_state = WIFI_STATE_CONNECTED;
            s_retry_count = 0;
            s_was_connected = true;  /* Remember we connected successfully for auto-reconnect */
            if (s_wifi_event_group)
            {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }

            /* The connection actually worked — persist the credentials to
             * the multi-network store. We deliberately wait for GOT_IP rather
             * than STA_CONNECTED so a bad password that only fails during
             * the 4-way handshake doesn't overwrite the user's last-good
             * password for this SSID. */
            if (s_pending_ssid[0] != '\0')
            {
                if (s_creds_mutex && xSemaphoreTake(s_creds_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                {
                    creds_store_open_locked();
                    (void)creds_save_locked(s_pending_ssid, s_pending_password);
                    xSemaphoreGive(s_creds_mutex);
                }
                /* Clear pending buffers so a passive reconnect (handled by
                 * the IDF supplicant) doesn't re-save stale data. */
                memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
                memset(s_pending_password, 0, sizeof(s_pending_password));
            }
        }
    }
}

void wifi_scan_init(void)
{
    if (s_initialized)
    {
        return;
    }

    ESP_LOGI(TAG, "Initializing WiFi scan module");

    /* Create mutex for thread-safe access to scan results */
    s_scan_mutex = xSemaphoreCreateMutex();
    if (s_scan_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create scan mutex");
        return;
    }

    /* Mutex protecting the multi-network credential store. Independent from
     * the scan mutex so a long credential read can't block a scan. */
    if (s_creds_mutex == NULL)
    {
        s_creds_mutex = xSemaphoreCreateMutex();
        if (s_creds_mutex == NULL)
        {
            ESP_LOGE(TAG, "Failed to create credentials mutex");
            vSemaphoreDelete(s_scan_mutex);
            s_scan_mutex = NULL;
            return;
        }
    }

    /* Create event group */
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(s_scan_mutex);
        s_scan_mutex = NULL;
        return;
    }

    /* Create scan processing task with adequate stack for wifi_ap_record_t array */
    BaseType_t task_created = xTaskCreate(
        scan_process_task,
        "scan_proc",
        8192,  /* 8KB stack - room for 40 x wifi_ap_record_t (~3800 bytes) plus qsort */
        NULL,
        3,     /* Priority below main tasks */
        &s_scan_process_task);
    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create scan processing task");
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        vSemaphoreDelete(s_scan_mutex);
        s_scan_mutex = NULL;
        return;
    }

    /* Check if WiFi is already initialized */
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);

    if (err == ESP_ERR_WIFI_NOT_INIT)
    {
        ESP_LOGI(TAG, "Initializing WiFi driver");

        /* Initialize network interface if not already */
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Failed to init netif: %s", esp_err_to_name(err));
            return;
        }

        /* Create WiFi STA interface if not exists */
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif == NULL)
        {
            sta_netif = esp_netif_create_default_wifi_sta();
            if (sta_netif == NULL)
            {
                ESP_LOGE(TAG, "Failed to create WiFi STA interface");
                return;
            }
        }

        /* Initialize WiFi with default config */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
            return;
        }

        /* Set station mode */
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(err));
            return;
        }

        /* Start WiFi */
        err = esp_wifi_start();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
            return;
        }
    }

    /* Register event handlers */
    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL,
        &s_wifi_handler_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL,
        &s_ip_handler_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        return;
    }

    /* Check current connection state */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        strncpy(s_current_ssid, (char *)ap_info.ssid, WIFI_SSID_MAX_LEN - 1);
        s_current_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

        /* Get current IP */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif)
        {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
            {
                snprintf(s_current_ip, WIFI_IP_MAX_LEN, IPSTR, IP2STR(&ip_info.ip));
                s_connection_state = WIFI_STATE_CONNECTED;
                ESP_LOGI(TAG, "Already connected to %s (%s)", s_current_ssid, s_current_ip);
            }
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi scan module initialized");

    /* Auto-connect using saved credentials from NVS flash.
     * esp_wifi_start() loads saved config but does NOT initiate connection.
     * We must explicitly call esp_wifi_connect() to use the saved creds.
     * CRITICAL: Reset retry state before connecting to prevent stale WIFI_STATE_FAILED
     * from blocking the connection attempt after reboot. */
    if (s_connection_state != WIFI_STATE_CONNECTED && wifi_has_saved_credentials())
    {
        ESP_LOGI(TAG, "Saved WiFi credentials found, initiating connection...");
        s_retry_count = 0;
        s_connection_state = WIFI_STATE_CONNECTING;
        esp_wifi_connect();
    }
}

void wifi_scan_start(void)
{
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "WiFi scan not initialized, initializing now");
        wifi_scan_init();
        if (!s_initialized)
        {
            return;
        }
    }

    if (s_scan_in_progress)
    {
        ESP_LOGW(TAG, "Scan already in progress");
        return;
    }

    ESP_LOGI(TAG, "Starting WiFi scan");

    /* Clear previous results */
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        s_scan_count = 0;
        s_scan_complete = false;
        xSemaphoreGive(s_scan_mutex);
    }

    /* Clear scan done bit */
    if (s_wifi_event_group)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    }

    /* Configure scan */
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,        /* Scan all channels */
        .show_hidden = true, /* Include hidden APs in raw results (empty-SSID
                              * entries are still filtered downstream until a
                              * proper hidden-network UX lands). */
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            }
        }
    };

    /* Start async scan */
    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start scan: %s", esp_err_to_name(err));
        return;
    }

    s_scan_in_progress = true;
}

bool wifi_scan_is_complete(void)
{
    return s_scan_complete && !s_scan_in_progress;
}

uint16_t wifi_scan_get_count(void)
{
    return s_scan_count;
}

const wifi_scan_result_t* wifi_scan_get_results(void)
{
    /* Note: Caller should check wifi_scan_is_complete() first and
     * not start a new scan while accessing these results.
     * For thread-safe access, use wifi_scan_copy_results() instead.
     */
    if (s_scan_count == 0)
    {
        return NULL;
    }
    return s_scan_results;
}

uint16_t wifi_scan_copy_results(wifi_scan_result_t *out_results, uint16_t max_count)
{
    if (out_results == NULL || max_count == 0)
    {
        return 0;
    }

    if (s_scan_mutex == NULL || xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        return 0;
    }

    uint16_t copy_count = (s_scan_count < max_count) ? s_scan_count : max_count;
    if (copy_count > 0)
    {
        memcpy(out_results, s_scan_results, copy_count * sizeof(wifi_scan_result_t));
    }

    xSemaphoreGive(s_scan_mutex);
    return copy_count;
}

bool wifi_is_connected(void)
{
    return s_connection_state == WIFI_STATE_CONNECTED;
}

wifi_connection_state_t wifi_get_state(void)
{
    return s_connection_state;
}

const char* wifi_get_current_ssid(void)
{
    return s_current_ssid;
}

const char* wifi_get_current_ip(void)
{
    return s_current_ip;
}

int wifi_get_last_disconnect_reason(void)
{
    return s_last_disconnect_reason;
}

bool wifi_connect(const char* ssid, const char* password)
{
    if (!s_initialized)
    {
        wifi_scan_init();
        if (!s_initialized)
        {
            return false;
        }
    }

    if (ssid == NULL || ssid[0] == '\0')
    {
        ESP_LOGE(TAG, "Invalid SSID");
        return false;
    }

    if (strlen(ssid) >= WIFI_SSID_MAX_LEN)
    {
        ESP_LOGE(TAG, "SSID too long");
        return false;
    }

    if (password && strlen(password) >= WIFI_PASSWORD_MAX_LEN)
    {
        ESP_LOGE(TAG, "Password too long");
        return false;
    }

    ESP_LOGI(TAG, "Connecting to %s", ssid);

    /* Remember what we're attempting; the credentials are only persisted to
     * the multi-network store once GOT_IP fires, so this buffer lives across
     * the supplicant's retry / handshake window. */
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    if (password) {
        strncpy(s_pending_password, password, sizeof(s_pending_password) - 1);
        s_pending_password[sizeof(s_pending_password) - 1] = '\0';
    } else {
        s_pending_password[0] = '\0';
    }

    /* Stop any ongoing scan */
    if (s_scan_in_progress)
    {
        esp_wifi_scan_stop();
        s_scan_in_progress = false;
    }

    /* Disconnect if currently connected */
    if (s_connection_state == WIFI_STATE_CONNECTED ||
        s_connection_state == WIFI_STATE_CONNECTING)
    {
        esp_wifi_disconnect();
    }

    /* Configure WiFi */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password)
    {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    // Accept WPA/WPA2/WPA3 — some phone hotspots (Android, iPhone with
    // Maximize Compatibility off) negotiate WPA3-PSK by default. The
    // supplicant will negotiate the highest mode the AP advertises.
    wifi_config.sta.threshold.authmode = password ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    /* Set configuration (will be saved to flash) */
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
        return false;
    }

    /* Clear event bits and reset retry counter */
    if (s_wifi_event_group)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    s_retry_count = 0;
    s_connection_state = WIFI_STATE_CONNECTING;

    /* Start connection */
    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start connection: %s", esp_err_to_name(err));
        s_connection_state = WIFI_STATE_FAILED;
        return false;
    }

    return true;
}

void wifi_disconnect(void)
{
    if (!s_initialized)
    {
        return;
    }

    ESP_LOGI(TAG, "Disconnecting from WiFi");
    esp_wifi_disconnect();
    s_connection_state = WIFI_STATE_DISCONNECTED;
    s_current_ssid[0] = '\0';
    s_current_ip[0] = '\0';
}

void wifi_reset_connection_state(void)
{
    s_retry_count = 0;
    s_connection_state = WIFI_STATE_CONNECTING;
    ESP_LOGI(TAG, "Connection state reset (retries cleared)");
}

void wifi_clear_credentials(void)
{
    ESP_LOGI(TAG, "Clearing saved WiFi credentials");

    /* Set empty config to clear saved credentials */
    wifi_config_t wifi_config = {0};
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

bool wifi_has_saved_credentials(void)
{
    wifi_config_t wifi_config;
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);

    if (err != ESP_OK)
    {
        return false;
    }

    return wifi_config.sta.ssid[0] != '\0';
}

/* =====================================================================
 * Multi-network credential store
 * =====================================================================
 *
 * All `_locked` helpers assume the caller holds s_creds_mutex.
 *
 * The store is intentionally tiny and synchronous: 8 slots, two short string
 * keys each, a single commit after a multi-key write. NVS handles wear
 * levelling for us. No background tasks, no eviction queue — the call sites
 * (connect-on-got-ip, password-screen lookup) are infrequent enough that the
 * extra ~10 ms of NVS work per call is invisible.
 */

static void creds_store_open_locked(void)
{
    if (s_creds_nvs_open) {
        return;
    }
    esp_err_t err = nvs_open(WIFI_CREDS_NAMESPACE, NVS_READWRITE, &s_creds_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_creds: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    s_creds_nvs_open = true;
}

/* Read a single slot's SSID; out_ssid is WIFI_SSID_MAX_LEN bytes. Returns
 * true if a non-empty SSID is present. */
static bool creds_read_ssid_locked(int slot, char *out_ssid)
{
    if (!s_creds_nvs_open || slot < 0 || slot >= WIFI_MAX_SAVED_NETWORKS) {
        out_ssid[0] = '\0';
        return false;
    }
    char key[16];
    snprintf(key, sizeof(key), WIFI_CREDS_SSID_KEY_FMT, slot);
    size_t len = WIFI_SSID_MAX_LEN;
    esp_err_t err = nvs_get_str(s_creds_nvs, key, out_ssid, &len);
    if (err != ESP_OK) {
        out_ssid[0] = '\0';
        return false;
    }
    return out_ssid[0] != '\0';
}

static bool creds_read_pass_locked(int slot, char *out_pass, size_t out_len)
{
    if (!s_creds_nvs_open || slot < 0 || slot >= WIFI_MAX_SAVED_NETWORKS) {
        if (out_len > 0) out_pass[0] = '\0';
        return false;
    }
    char key[16];
    snprintf(key, sizeof(key), WIFI_CREDS_PASS_KEY_FMT, slot);
    esp_err_t err = nvs_get_str(s_creds_nvs, key, out_pass, &out_len);
    if (err != ESP_OK) {
        if (out_len > 0) out_pass[0] = '\0';
        return false;
    }
    return true;
}

static esp_err_t creds_write_slot_locked(int slot, const char *ssid, const char *password)
{
    char key[16];
    esp_err_t err;

    snprintf(key, sizeof(key), WIFI_CREDS_SSID_KEY_FMT, slot);
    err = nvs_set_str(s_creds_nvs, key, ssid ? ssid : "");
    if (err != ESP_OK) {
        return err;
    }

    snprintf(key, sizeof(key), WIFI_CREDS_PASS_KEY_FMT, slot);
    err = nvs_set_str(s_creds_nvs, key, password ? password : "");
    return err;
}

static esp_err_t creds_clear_slot_locked(int slot)
{
    return creds_write_slot_locked(slot, "", "");
}

/* Find the slot index containing the given SSID, or -1 if not found. */
static int creds_find_slot_locked(const char *ssid)
{
    char buf[WIFI_SSID_MAX_LEN];
    for (int i = 0; i < WIFI_MAX_SAVED_NETWORKS; i++) {
        if (creds_read_ssid_locked(i, buf) && strcmp(buf, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

/* Save the SSID/password at slot 0 with LRU shift. */
static bool creds_save_locked(const char *ssid, const char *password)
{
    if (!s_creds_nvs_open || ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    int existing = creds_find_slot_locked(ssid);

    if (existing == 0) {
        /* Already at slot 0 — just overwrite password (it may have rotated). */
        esp_err_t err = creds_write_slot_locked(0, ssid, password);
        if (err == ESP_OK) {
            err = nvs_commit(s_creds_nvs);
        }
        return err == ESP_OK;
    }

    /* Decide how far down to shift. If the SSID already exists at K>0, the
     * shift covers slots [0..K-1] -> [1..K]. If it's new, shift everything
     * down by one (evict slot N-1). */
    int shift_top;
    if (existing > 0) {
        shift_top = existing - 1;       /* shift slots 0..existing-1 down one */
    } else {
        shift_top = WIFI_MAX_SAVED_NETWORKS - 2; /* shift slots 0..N-2 down one */
    }

    /* Move from the bottom up so we don't overwrite source before reading. */
    for (int i = shift_top; i >= 0; i--) {
        char src_ssid[WIFI_SSID_MAX_LEN];
        char src_pass[WIFI_PASSWORD_MAX_LEN];
        bool have_ssid = creds_read_ssid_locked(i, src_ssid);
        bool have_pass = creds_read_pass_locked(i, src_pass, sizeof(src_pass));
        if (!have_ssid) {
            /* Empty source slot — write empties into the destination to keep
             * compaction tidy. */
            (void)creds_clear_slot_locked(i + 1);
        } else {
            (void)creds_write_slot_locked(i + 1,
                                          src_ssid,
                                          have_pass ? src_pass : "");
        }
    }

    /* Write the new entry at slot 0. */
    esp_err_t err = creds_write_slot_locked(0, ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_creds: write slot 0 failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_commit(s_creds_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_creds: commit failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "wifi_creds: saved '%s' at slot 0 (had %s%d)",
             ssid,
             existing >= 0 ? "existing slot " : "no prior entry",
             existing >= 0 ? existing : 0);
    return true;
}

bool wifi_get_saved_password(const char *ssid, char *out_pass, size_t out_len)
{
    if (ssid == NULL || ssid[0] == '\0' || out_pass == NULL || out_len == 0) {
        return false;
    }

    if (s_creds_mutex == NULL || xSemaphoreTake(s_creds_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    creds_store_open_locked();

    bool found = false;
    int slot = creds_find_slot_locked(ssid);
    if (slot >= 0) {
        found = creds_read_pass_locked(slot, out_pass, out_len);
    } else {
        out_pass[0] = '\0';
    }

    xSemaphoreGive(s_creds_mutex);
    return found;
}

bool wifi_has_saved_for_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    if (s_creds_mutex == NULL || xSemaphoreTake(s_creds_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    creds_store_open_locked();
    bool found = creds_find_slot_locked(ssid) >= 0;
    xSemaphoreGive(s_creds_mutex);
    return found;
}

bool wifi_save_credentials_for_ssid(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    if (strlen(ssid) >= WIFI_SSID_MAX_LEN) {
        return false;
    }
    if (password && strlen(password) >= WIFI_PASSWORD_MAX_LEN) {
        return false;
    }

    if (s_creds_mutex == NULL || xSemaphoreTake(s_creds_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    creds_store_open_locked();
    bool ok = creds_save_locked(ssid, password);

    xSemaphoreGive(s_creds_mutex);
    return ok;
}

bool wifi_forget_credentials_for_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    if (s_creds_mutex == NULL || xSemaphoreTake(s_creds_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    creds_store_open_locked();

    bool removed = false;
    int slot = creds_find_slot_locked(ssid);
    if (slot >= 0 && s_creds_nvs_open) {
        /* Shift everything below `slot` up by one so the list stays compact. */
        for (int i = slot; i < WIFI_MAX_SAVED_NETWORKS - 1; i++) {
            char src_ssid[WIFI_SSID_MAX_LEN];
            char src_pass[WIFI_PASSWORD_MAX_LEN];
            bool have_ssid = creds_read_ssid_locked(i + 1, src_ssid);
            bool have_pass = creds_read_pass_locked(i + 1, src_pass, sizeof(src_pass));
            if (have_ssid) {
                (void)creds_write_slot_locked(i, src_ssid, have_pass ? src_pass : "");
            } else {
                (void)creds_clear_slot_locked(i);
            }
        }
        /* Empty the now-vacated tail slot. */
        (void)creds_clear_slot_locked(WIFI_MAX_SAVED_NETWORKS - 1);

        if (nvs_commit(s_creds_nvs) == ESP_OK) {
            removed = true;
            ESP_LOGI(TAG, "wifi_creds: forgot '%s' (was slot %d)", ssid, slot);
        }
    }

    xSemaphoreGive(s_creds_mutex);
    return removed;
}

/* =====================================================================
 * Boot-time + auto-reconnect: scan-then-pick-best-saved
 * =====================================================================
 *
 * The naive "esp_wifi_connect() against the IDF builtin slot" path retries
 * the same SSID 15 times before giving up, which leaves the user staring at
 * the boot screen if the most-recently-used AP happens to be out of range.
 *
 * Instead, scan first, then walk the multi-network credential list (slot 0
 * is LRU-newest) and connect to the FIRST saved SSID that is actually
 * visible. Fall back to the WiFi setup UI only if nothing matches.
 */

/* Synchronously run a scan and wait for completion. Returns true on success. */
static bool scan_and_wait(uint32_t timeout_ms)
{
    if (!s_initialized || s_wifi_event_group == NULL) {
        return false;
    }

    /* If a connection is currently in-flight, esp_wifi_scan_start can return
     * ESP_ERR_WIFI_STATE. The caller is expected to ensure we're idle. */
    xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    wifi_scan_start();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_SCAN_DONE_BIT,
        pdTRUE,   /* clear on exit */
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if ((bits & WIFI_SCAN_DONE_BIT) == 0) {
        ESP_LOGW(TAG, "Scan timed out after %u ms", (unsigned)timeout_ms);
        /* Stop the scan so the radio is idle for a subsequent connect. */
        esp_wifi_scan_stop();
        s_scan_in_progress = false;
        return false;
    }
    return true;
}

/* Returns true if `ssid` is present in the most-recent scan results. */
static bool ssid_visible_in_scan(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    bool found = false;
    for (uint16_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_results[i].ssid, ssid) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_scan_mutex);
    return found;
}

/* Wait for either CONNECTED or FAILED to settle. Returns true on success. */
static bool wait_for_connect(uint32_t timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,    /* clear on exit */
        pdFALSE,   /* either bit */
        pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_try_connect_best_saved(uint32_t scan_timeout_ms, uint32_t connect_timeout_ms)
{
    if (!s_initialized) {
        wifi_scan_init();
        if (!s_initialized) {
            return false;
        }
    }

    /* Already connected — nothing to do. */
    if (s_connection_state == WIFI_STATE_CONNECTED) {
        return true;
    }

    /* Gather the saved SSID list (LRU order). */
    char saved[WIFI_MAX_SAVED_NETWORKS][WIFI_SSID_MAX_LEN];
    int n_saved = wifi_list_saved_ssids(saved, WIFI_MAX_SAVED_NETWORKS);
    if (n_saved <= 0) {
        ESP_LOGI(TAG, "No saved networks in the multi-network store");
        return false;
    }

    ESP_LOGI(TAG, "Scanning for %d saved network(s)...", n_saved);

    /* If a previous failed-connect attempt left the supplicant in a retry
     * loop, calm it down so the scan can run cleanly. */
    if (s_connection_state == WIFI_STATE_CONNECTING) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!scan_and_wait(scan_timeout_ms)) {
        ESP_LOGW(TAG, "Scan failed — cannot match saved networks");
        return false;
    }

    /* Walk saved networks in LRU order; connect to the first visible one. */
    for (int i = 0; i < n_saved; i++) {
        if (!ssid_visible_in_scan(saved[i])) {
            ESP_LOGI(TAG, "  saved[%d] '%s' not visible — skip", i, saved[i]);
            continue;
        }

        char password[WIFI_PASSWORD_MAX_LEN] = {0};
        bool have_pass = wifi_get_saved_password(saved[i], password, sizeof(password));
        (void)have_pass; /* password may be empty for open networks */

        ESP_LOGI(TAG, "  saved[%d] '%s' visible — attempting connect", i, saved[i]);

        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group,
                                 WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }

        if (!wifi_connect(saved[i], password)) {
            ESP_LOGW(TAG, "  wifi_connect('%s') failed to start", saved[i]);
            continue;
        }

        if (wait_for_connect(connect_timeout_ms)) {
            ESP_LOGI(TAG, "✅ Connected to '%s' (%s)",
                     wifi_get_current_ssid(), wifi_get_current_ip());
            return true;
        }

        ESP_LOGW(TAG, "  '%s' failed to connect within %u ms — trying next",
                 saved[i], (unsigned)connect_timeout_ms);
        /* Make sure the supplicant isn't still chewing on this SSID before
         * we try the next one. wifi_connect() resets retry state anyway. */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGW(TAG, "No saved network was reachable");
    /* Avoid leaving the state machine stuck in CONNECTING — that would make
     * the auto-reconnect task think a connect is in flight. */
    s_connection_state = WIFI_STATE_DISCONNECTED;
    s_was_connected = false;
    return false;
}

/* ---------- Background auto-reconnect task ---------- */

static TaskHandle_t s_auto_reconnect_task = NULL;
static volatile uint32_t s_auto_reconnect_interval_ms = 30000;

static void auto_reconnect_task(void *arg)
{
    (void)arg;
    /* Brief settle before the first scan attempt — gives the boot-time
     * connect call a chance to succeed first. */
    vTaskDelay(pdMS_TO_TICKS(5000));

    for (;;) {
        /* Sleep first; main.c already runs the initial scan/connect. */
        vTaskDelay(pdMS_TO_TICKS(s_auto_reconnect_interval_ms));

        /* If we're connected, just keep idling. */
        if (s_connection_state == WIFI_STATE_CONNECTED) {
            continue;
        }

        /* Don't fight an in-progress connect attempt (manual user action or
         * the supplicant's own retry). */
        if (s_connection_state == WIFI_STATE_CONNECTING) {
            continue;
        }

        /* Quick check that we actually have saved creds to try. */
        char saved[1][WIFI_SSID_MAX_LEN];
        if (wifi_list_saved_ssids(saved, 1) <= 0) {
            continue;
        }

        ESP_LOGI(TAG, "auto-reconnect: scanning for saved networks");
        if (wifi_try_connect_best_saved(6000, 12000)) {
            ESP_LOGI(TAG, "auto-reconnect: re-acquired WiFi");
        }
    }
}

void wifi_start_auto_reconnect_task(uint32_t interval_ms)
{
    if (s_auto_reconnect_task != NULL) {
        s_auto_reconnect_interval_ms = interval_ms;
        return;
    }
    s_auto_reconnect_interval_ms = (interval_ms < 5000) ? 5000 : interval_ms;

    /* 4 KB stack — task only does small NVS reads + wifi_connect/scan calls.
     * The scan-result processing runs on its own task (scan_process_task). */
    BaseType_t ok = xTaskCreate(
        auto_reconnect_task,
        "wifi_reconn",
        4096,
        NULL,
        3,  /* below user-facing tasks */
        &s_auto_reconnect_task);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto-reconnect task");
        s_auto_reconnect_task = NULL;
    } else {
        ESP_LOGI(TAG, "Auto-reconnect task started (interval=%u ms)",
                 (unsigned)s_auto_reconnect_interval_ms);
    }
}

int wifi_list_saved_ssids(char ssids[][WIFI_SSID_MAX_LEN], int max)
{
    if (ssids == NULL || max <= 0) {
        return 0;
    }
    if (s_creds_mutex == NULL || xSemaphoreTake(s_creds_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return 0;
    }
    creds_store_open_locked();

    int count = 0;
    char buf[WIFI_SSID_MAX_LEN];
    for (int i = 0; i < WIFI_MAX_SAVED_NETWORKS && count < max; i++) {
        if (creds_read_ssid_locked(i, buf)) {
            strncpy(ssids[count], buf, WIFI_SSID_MAX_LEN - 1);
            ssids[count][WIFI_SSID_MAX_LEN - 1] = '\0';
            count++;
        }
    }

    xSemaphoreGive(s_creds_mutex);
    return count;
}
