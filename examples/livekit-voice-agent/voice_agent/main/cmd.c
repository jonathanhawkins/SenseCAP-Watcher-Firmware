#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "aligned_client.h"

static const char *TAG = "cmd";

#define PROMPT_STR "Aligned"

/************* WiFi Configuration Command **************/
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_cfg_args;

static int wifi_cfg_set(int argc, char **argv)
{
    bool have_password = false;
    char ssid[32] = {0};
    char password[64] = {0};
    wifi_config_t wifi_config = {0};

    int nerrors = arg_parse(argc, argv, (void **) &wifi_cfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_cfg_args.end, argv[0]);
        return 1;
    }

    if (wifi_cfg_args.ssid->count) {
        int len = strlen(wifi_cfg_args.ssid->sval[0]);
        if (len > (sizeof(ssid) - 1)) {
            ESP_LOGE(TAG, "SSID too long (max 31 bytes): %s", wifi_cfg_args.ssid->sval[0]);
            return -1;
        }
        strncpy(ssid, wifi_cfg_args.ssid->sval[0], 31);
    } else {
        ESP_LOGE(TAG, "No SSID provided");
        return -1;
    }

    if (wifi_cfg_args.password->count) {
        int len = strlen(wifi_cfg_args.password->sval[0]);
        if (len > (sizeof(password) - 1)) {
            ESP_LOGE(TAG, "Password too long (max 63 bytes)");
            return -1;
        }
        have_password = true;
        strncpy(password, wifi_cfg_args.password->sval[0], 63);
    }

    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));

    if (have_password) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    // Initialize WiFi if not already done
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGI(TAG, "Initializing WiFi subsystem...");

        // Initialize network interface first
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to init netif: %s", esp_err_to_name(err));
            return 1;
        }

        // Check if WiFi STA netif already exists before creating
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif == NULL) {
            ESP_LOGI(TAG, "Creating WiFi STA netif...");
            sta_netif = esp_netif_create_default_wifi_sta();
            if (sta_netif == NULL) {
                ESP_LOGE(TAG, "Failed to create WiFi STA netif");
                return 1;
            }
        } else {
            ESP_LOGI(TAG, "Using existing WiFi STA netif");
        }

        wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    } else {
        ESP_LOGI(TAG, "WiFi already initialized");
        esp_wifi_stop();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Save WiFi credentials to flash so they persist across reboots
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    // Set WiFi configuration with error checking
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
        return 1;
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // Avoid logging WiFi credentials in plain text
    ESP_LOGI(TAG, "WiFi configured - SSID: %s, password_set: %s, password_length: %d",
             wifi_config.sta.ssid,
             (strlen((const char *)wifi_config.sta.password) > 0) ? "true" : "false",
             (int)strlen((const char *)wifi_config.sta.password));

    return 0;
}

static void register_cmd_wifi_sta(void)
{
    wifi_cfg_args.ssid = arg_str0("s", NULL, "<ssid>", "SSID of AP");
    wifi_cfg_args.password = arg_str0("p", NULL, "<password>", "password of AP");
    wifi_cfg_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "wifi_sta",
        .help = "Configure WiFi station mode and join AP",
        .hint = NULL,
        .func = &wifi_cfg_set,
        .argtable = &wifi_cfg_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Aligned Device Token Command **************/
static struct {
    struct arg_str *token;
    struct arg_end *end;
} aligned_token_args;

static int aligned_token_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &aligned_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, aligned_token_args.end, argv[0]);
        return 1;
    }

    if (aligned_token_args.token->count) {
        const char *token = aligned_token_args.token->sval[0];
        size_t len = strlen(token);

        if (len >= 256) {
            ESP_LOGE(TAG, "Token too long (max 255 bytes)");
            return -1;
        }

        // Set and save token to NVS
        aligned_set_device_token(token);
        ESP_LOGI(TAG, "✅ Device token set (length: %d)", len);
        ESP_LOGI(TAG, "Token: %c%c%c%c...%c%c%c%c",
                 token[0], token[1], token[2], token[3],
                 token[len-4], token[len-3], token[len-2], token[len-1]);
    } else {
        // Show current token (masked)
        if (aligned_has_token()) {
            const char *token = aligned_get_device_token();
            size_t len = strlen(token);
            ESP_LOGI(TAG, "Current token: %c%c%c%c...%c%c%c%c (length: %d)",
                     token[0], token[1], token[2], token[3],
                     token[len-4], token[len-3], token[len-2], token[len-1], len);
        } else {
            ESP_LOGW(TAG, "No device token configured");
            ESP_LOGI(TAG, "Set token with: aligned_token -t watcher_xxx...");
        }
    }

    return 0;
}

static void register_aligned_token(void)
{
    aligned_token_args.token = arg_str0("t", NULL, "<token>", "Aligned device token (watcher_xxx...)");
    aligned_token_args.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "aligned_token",
        .help = "Set Aligned Tools device token for voice assistant",
        .hint = NULL,
        .func = &aligned_token_set,
        .argtable = &aligned_token_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Aligned API URL Command **************/
static struct {
    struct arg_str *url;
    struct arg_lit *reset;
    struct arg_end *end;
} aligned_api_args;

static int aligned_api_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &aligned_api_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, aligned_api_args.end, argv[0]);
        return 1;
    }

    if (aligned_api_args.reset->count) {
        // Reset to production
        aligned_set_api_url(NULL);
        ESP_LOGI(TAG, "✅ API URL reset to production");
    } else if (aligned_api_args.url->count) {
        const char *url = aligned_api_args.url->sval[0];
        size_t len = strlen(url);

        if (len >= 256) {
            ESP_LOGE(TAG, "URL too long (max 255 bytes)");
            return -1;
        }

        // Set and save URL to NVS
        aligned_set_api_url(url);
        ESP_LOGI(TAG, "✅ API URL set to: %s", url);
    } else {
        // Show current URL
        const char *url = aligned_get_api_base();
        ESP_LOGI(TAG, "Current API URL: %s", url);
    }

    return 0;
}

static void register_aligned_api(void)
{
    aligned_api_args.url = arg_str0("u", NULL, "<url>", "API base URL (e.g., http://192.168.1.71:3000)");
    aligned_api_args.reset = arg_lit0("r", "reset", "Reset to production (https://aligned.tools)");
    aligned_api_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "aligned_api",
        .help = "Set Aligned API URL (for local development)",
        .hint = NULL,
        .func = &aligned_api_set,
        .argtable = &aligned_api_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Reboot Command **************/
static int do_reboot(int argc, char **argv)
{
    ESP_LOGI(TAG, "Rebooting device...");
    esp_restart();
    return 0;
}

static void register_cmd_reboot(void)
{
    const esp_console_cmd_t cmd = {
        .command = "reboot",
        .help = "Reboot the device",
        .hint = NULL,
        .func = &do_reboot,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Console Initialization **************/
int cmd_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = PROMPT_STR ">";
    repl_config.max_cmdline_length = 1024;

    // Register commands
    register_cmd_wifi_sta();
    register_aligned_token();
    register_aligned_api();
    register_cmd_reboot();

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
#error Unsupported console type
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "  Aligned Tools - LiveKit Voice Assistant");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Console commands:");
    ESP_LOGI(TAG, "  wifi_sta -s \"SSID\" -p \"password\"");
    ESP_LOGI(TAG, "  aligned_token -t watcher_xxx...");
    ESP_LOGI(TAG, "  aligned_api -u http://192.168.1.71:3000");
    ESP_LOGI(TAG, "  aligned_api -r  (reset to production)");
    ESP_LOGI(TAG, "  reboot");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Current API: %s", aligned_get_api_base());

    if (!aligned_has_token()) {
        ESP_LOGW(TAG, "⚠️  No device token configured!");
        ESP_LOGI(TAG, "Set token with: aligned_token -t watcher_xxx...");
        ESP_LOGI(TAG, "");
    }

    return 0;
}
