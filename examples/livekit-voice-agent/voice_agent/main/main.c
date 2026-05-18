#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "board.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "example.h"
#include "livekit_example_utils.h"
#include "media.h"
#include "cmd.h"
#include "aligned_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "livekit.h"

static const char *TAG = "main";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;
static esp_event_handler_instance_t s_wifi_event_instance = NULL;
static esp_event_handler_instance_t s_ip_event_instance = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < 5)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/5)", s_retry_num);
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Failed to connect to WiFi");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool connect_wifi_from_flash(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "=== WiFi Connection Start ===");

    // Reset retry counter for fresh connection attempt
    s_retry_num = 0;

    // Step 1: Check if WiFi is already initialized
    wifi_mode_t mode;
    err = esp_wifi_get_mode(&mode);

    if (err == ESP_ERR_WIFI_NOT_INIT)
    {
        ESP_LOGI(TAG, "[1/7] WiFi not initialized, initializing...");

        // Step 1a: Initialize network interface
        ESP_LOGI(TAG, "[2/7] Initializing network interface...");
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Failed to init netif: %s", esp_err_to_name(err));
            return false;
        }

        // Step 1b: Get or create WiFi station interface
        ESP_LOGI(TAG, "[3/7] Checking WiFi STA interface...");
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif == NULL)
        {
            ESP_LOGI(TAG, "Creating new WiFi STA interface...");
            sta_netif = esp_netif_create_default_wifi_sta();
            if (sta_netif == NULL)
            {
                ESP_LOGE(TAG, "Failed to create WiFi STA interface");
                return false;
            }
        }
        else
        {
            ESP_LOGI(TAG, "Using existing WiFi STA interface");
        }

        // Step 1c: Initialize WiFi driver
        ESP_LOGI(TAG, "[4/7] Initializing WiFi driver...");
        wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wifi_init_cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
            return false;
        }

        // Step 1d: Set WiFi mode to station
        ESP_LOGI(TAG, "[5/7] Setting WiFi mode to STA...");
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Set mode failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    else
    {
        ESP_LOGI(TAG, "[1/7] WiFi already initialized");
    }

    // Step 2: Try to get saved WiFi config from flash
    ESP_LOGI(TAG, "[6/7] Checking for saved WiFi credentials...");
    wifi_config_t wifi_config;
    err = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get WiFi config: %s", esp_err_to_name(err));
        return false;
    }

    if (strlen((char *)wifi_config.sta.ssid) == 0)
    {
        ESP_LOGW(TAG, "No WiFi credentials saved in flash");
        ESP_LOGW(TAG, "Please configure WiFi using: wifi_sta -s \"YOUR_SSID\" -p \"YOUR_PASSWORD\"");
        // NOTE: Default WiFi credentials removed for security
        // Configure your network credentials via serial console using wifi_sta command
        // Example: wifi_sta -s "YourNetworkName" -p "YourPassword"
    }

    ESP_LOGI(TAG, "Found saved credentials - SSID: %s", wifi_config.sta.ssid);

    // Step 3: Create event group for WiFi status
    // Free old event group if it exists to prevent memory leak
    if (s_wifi_event_group != NULL)
    {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    // Clear any stale event bits from previous connection attempts
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Step 4: Register event handlers (BEFORE starting WiFi)
    ESP_LOGI(TAG, "Registering WiFi event handlers...");

    // Unregister old handlers if they exist to prevent duplicate registration
    if (s_wifi_event_instance != NULL)
    {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }
    if (s_ip_event_instance != NULL)
    {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }

    // Register new handlers and store instances
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_event_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_event_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        return false;
    }

    // Step 5: Start WiFi
    ESP_LOGI(TAG, "[7/7] Starting WiFi and connecting to: %s", wifi_config.sta.ssid);
    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return false;
    }

    // Step 6: Wait for connection (event handler will trigger)
    ESP_LOGI(TAG, "Waiting for connection (timeout: 20s)...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    // Step 7: Check result
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "✅ WiFi connected successfully!");
        ESP_LOGI(TAG, "=== WiFi Connection Complete ===");
        return true;
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGE(TAG, "❌ WiFi connection failed after retries");
        return false;
    }
    else
    {
        ESP_LOGE(TAG, "❌ WiFi connection timeout (20s)");
        return false;
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    // CRITICAL: Initialize NVS flash before anything else
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // CRITICAL: Create default event loop before any WiFi/network operations
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    livekit_system_init();
    board_init();
    media_init();

    // Initialize SNTP for time synchronization
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);

    // Connect to WiFi BEFORE starting REPL (so logs are visible)
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting WiFi connection...");
    ESP_LOGI(TAG, "========================================");
    fflush(stdout);

    // Try to connect to WiFi using credentials saved to flash
    // If no credentials, user can configure via console: wifi_sta -s "SSID" -p "password"
    if (connect_wifi_from_flash())
    {
        ESP_LOGI(TAG, "✅ WiFi connected!");
        ESP_LOGI(TAG, "");

        // If device token is configured, auto-join room
        if (aligned_has_token())
        {
            ESP_LOGI(TAG, "Device token found, connecting to LiveKit room...");
            join_room(); // See example.c
        }
        else
        {
            ESP_LOGW(TAG, "⚠️  Device token not configured!");
            ESP_LOGI(TAG, "Set token with: aligned_token -t watcher_xxx...");
        }
    }
    else
    {
        ESP_LOGW(TAG, "❌ WiFi not connected");
        ESP_LOGI(TAG, "Configure WiFi with:");
        ESP_LOGI(TAG, "  wifi_sta -s \"YourSSID\" -p \"YourPassword\"");
        ESP_LOGI(TAG, "  reboot");
    }

    // Now start the console REPL (this will capture stdout)
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Starting console REPL...");
    fflush(stdout);
    cmd_init();

    // Console REPL continues running in background
}
