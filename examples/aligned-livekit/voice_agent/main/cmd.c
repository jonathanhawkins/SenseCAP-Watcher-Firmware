#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "linenoise/linenoise.h"
#include "aligned_client.h"
#include "example.h"
#include "wifi_setup.h"
#include "wifi_scan.h"
#include "qr_setup.h"
#include "board.h"

static const char *TAG = "cmd";

#define PROMPT_STR "Aligned"

static const char *authmode_to_str(wifi_auth_mode_t authmode)
{
    switch (authmode)
    {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "UNKNOWN";
    }
}

static esp_err_t ensure_wifi_initialized(bool start)
{
    esp_err_t err;
    wifi_mode_t mode;

    err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT)
    {
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            return err;
        }

        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif == NULL)
        {
            sta_netif = esp_netif_create_default_wifi_sta();
            if (sta_netif == NULL)
            {
                return ESP_FAIL;
            }
        }

        wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wifi_init_cfg);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        return err;
    }

    if (start)
    {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
        {
            return err;
        }
    }

    return ESP_OK;
}

static char *prompt_line(const char *prompt, bool mask)
{
    // Note: linenoiseSetMaskMode was removed in newer ESP-IDF versions
    // Password masking is not available, but functionality still works
    (void)mask;  // Suppress unused parameter warning
    char *line = linenoise(prompt);
    if (line && strlen(line) > 0)
    {
        linenoiseHistoryAdd(line);
    }
    return line;
}

static esp_err_t wifi_apply_config(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    bool have_password = (password != NULL && strlen(password) > 0);

    if (ssid == NULL || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));

    if (have_password)
    {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else
    {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = ensure_wifi_initialized(false);
    if (err != ESP_OK)
    {
        return err;
    }

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
    {
        return err;
    }

    ESP_LOGI(TAG, "WiFi configured - SSID: %s, password_set: %s, password_length: %d",
             wifi_config.sta.ssid,
             have_password ? "true" : "false",
             have_password ? (int)strlen((const char *)wifi_config.sta.password) : 0);

    return ESP_OK;
}

/************* WiFi Configuration Command **************/
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_cfg_args;

static int wifi_cfg_set(int argc, char **argv)
{
    char ssid[32] = {0};
    char password[64] = {0};

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
        strncpy(password, wifi_cfg_args.password->sval[0], 63);
    }

    esp_err_t err = wifi_apply_config(ssid, password);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
        return 1;
    }

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

/************* WiFi Setup Wizard (Graphical UI) **************/
static int wifi_setup_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "Opening WiFi setup screen...");

    // Initialize and show the graphical WiFi setup UI
    wifi_setup_init();
    wifi_setup_show();

    ESP_LOGI(TAG, "WiFi setup screen opened. Use the display to configure WiFi.");
    return 0;
}

static void register_cmd_wifi_setup(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi_setup",
        .help = "Open graphical WiFi setup on display",
        .hint = NULL,
        .func = &wifi_setup_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* WiFi Scan Command **************/
static int wifi_scan_cmd(int argc, char **argv)
{
    esp_err_t err = ensure_wifi_initialized(true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return 1;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    ESP_LOGI(TAG, "Scanning for WiFi networks...");
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return 1;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0)
    {
        ESP_LOGI(TAG, "No networks found");
        return 0;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL)
    {
        ESP_LOGE(TAG, "Out of memory for scan results");
        return 1;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(err));
        free(ap_records);
        return 1;
    }

    ESP_LOGI(TAG, "Found %u networks:", ap_count);
    for (int i = 0; i < ap_count; i++)
    {
        ESP_LOGI(TAG, "  %2d) %-32s RSSI:%4d Auth:%s", i + 1, (char *)ap_records[i].ssid, ap_records[i].rssi, authmode_to_str(ap_records[i].authmode));
    }

    free(ap_records);
    return 0;
}

static void register_cmd_wifi_scan(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi_scan",
        .help = "Scan for nearby WiFi networks",
        .hint = NULL,
        .func = &wifi_scan_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* WiFi Status Command **************/
static int wifi_status_cmd(int argc, char **argv)
{
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WiFi connected:");
        ESP_LOGI(TAG, "  SSID: %s", (char *)ap_info.ssid);
        ESP_LOGI(TAG, "  RSSI: %d", ap_info.rssi);
        ESP_LOGI(TAG, "  Auth: %s", authmode_to_str(ap_info.authmode));
    }
    else
    {
        ESP_LOGW(TAG, "WiFi not connected (%s)", esp_err_to_name(err));
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL)
    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        {
            ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "  GW: " IPSTR, IP2STR(&ip_info.gw));
        }
    }

    return 0;
}

static void register_cmd_wifi_status(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi_status",
        .help = "Show current WiFi connection status",
        .hint = NULL,
        .func = &wifi_status_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* WiFi Clear Command **************/
static int wifi_clear_cmd(int argc, char **argv)
{
    esp_err_t err = ensure_wifi_initialized(false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return 1;
    }

    wifi_config_t wifi_config = {0};
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to clear WiFi config: %s", esp_err_to_name(err));
        return 1;
    }

    ESP_LOGI(TAG, "WiFi credentials cleared. Reboot to apply.");
    return 0;
}

static void register_cmd_wifi_clear(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi_clear",
        .help = "Clear saved WiFi credentials",
        .hint = NULL,
        .func = &wifi_clear_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* WiFi Reconnect Command **************/
static int wifi_reconnect_cmd(int argc, char **argv)
{
    esp_err_t err = ensure_wifi_initialized(true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return 1;
    }

    /* Reset connection state before reconnecting.
     * This is critical: without resetting, a previous WIFI_STATE_FAILED
     * blocks the disconnect handler from retrying. */
    wifi_reset_connection_state();

    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi reconnect failed: %s", esp_err_to_name(err));
        return 1;
    }

    ESP_LOGI(TAG, "WiFi reconnect requested");
    return 0;
}

static void register_cmd_wifi_reconnect(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi_reconnect",
        .help = "Reconnect using saved WiFi credentials",
        .hint = NULL,
        .func = &wifi_reconnect_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Aligned Pair Wizard **************/
static int aligned_pair_cmd(int argc, char **argv)
{
    char *token = prompt_line("Paste device token (watcher_xxx...): ", false);
    if (token == NULL || strlen(token) == 0)
    {
        if (token)
        {
            linenoiseFree(token);
        }
        ESP_LOGW(TAG, "No token provided");
        return 1;
    }

    if (strlen(token) >= 256)
    {
        ESP_LOGE(TAG, "Token too long (max 255 bytes)");
        linenoiseFree(token);
        return 1;
    }

    aligned_set_device_token(token);
    ESP_LOGI(TAG, "✅ Device token set (length: %d)", (int)strlen(token));

    linenoiseFree(token);

    char *connect_now = prompt_line("Connect now? (y/N): ", false);
    if (connect_now)
    {
        if (connect_now[0] == 'y' || connect_now[0] == 'Y')
        {
            if (!room_is_active())
            {
                join_room();
            }
        }
        linenoiseFree(connect_now);
    }

    return 0;
}

static void register_aligned_pair(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_pair",
        .help = "Interactive token setup (paste token, optional connect)",
        .hint = NULL,
        .func = &aligned_pair_cmd,
        .argtable = NULL
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

/************* Server URL Command **************/
static struct {
    struct arg_str *url;
    struct arg_end *end;
} aligned_server_args;

static int aligned_server_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &aligned_server_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, aligned_server_args.end, argv[0]);
        return 1;
    }

    if (aligned_server_args.url->count) {
        const char *url = aligned_server_args.url->sval[0];
        if (strcmp(url, "default") == 0 || strlen(url) == 0) {
            aligned_set_server_url(NULL);
            ESP_LOGI(TAG, "Server URL reset to default: %s", aligned_get_server_url());
        } else {
            aligned_set_server_url(url);
            ESP_LOGI(TAG, "Server URL set to: %s", url);
        }
    } else {
        // No argument - show current URL
        ESP_LOGI(TAG, "Current server URL: %s", aligned_get_server_url());
        ESP_LOGI(TAG, "Set with: aligned_server -u http://your-server:port");
        ESP_LOGI(TAG, "Reset to default with: aligned_server -u default");
    }

    return 0;
}

static void register_aligned_server(void)
{
    aligned_server_args.url = arg_str0("u", NULL, "<url>", "Server URL (or 'default' to reset)");
    aligned_server_args.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "aligned_server",
        .help = "Set/show Aligned backend server URL",
        .hint = NULL,
        .func = &aligned_server_set,
        .argtable = &aligned_server_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* LiveKit Control Commands **************/
static int aligned_start_cmd(int argc, char **argv)
{
    if (!aligned_has_token())
    {
        ESP_LOGE(TAG, "No device token configured. Use: aligned_token -t watcher_xxx...");
        return 1;
    }

    if (room_is_active())
    {
        ESP_LOGW(TAG, "Already connected to LiveKit room");
        return 0;
    }

    join_room();
    return 0;
}

static int aligned_stop_cmd(int argc, char **argv)
{
    if (!room_is_active())
    {
        ESP_LOGW(TAG, "No active LiveKit room to leave");
        return 0;
    }

    leave_room();
    return 0;
}

static void register_aligned_start(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_start",
        .help = "Join the LiveKit room using the configured device token",
        .hint = NULL,
        .func = &aligned_start_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void register_aligned_stop(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_stop",
        .help = "Leave the current LiveKit room",
        .hint = NULL,
        .func = &aligned_stop_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Aligned Status Command **************/
static int aligned_status_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "Aligned status:");
    ESP_LOGI(TAG, "  Token set: %s", aligned_has_token() ? "yes" : "no");
    ESP_LOGI(TAG, "  Credentials: %s", aligned_is_connected() ? "yes" : "no");
    ESP_LOGI(TAG, "  Room active: %s", room_is_active() ? "yes" : "no");

    const char *room = aligned_get_room_name();
    if (room && strlen(room) > 0)
    {
        ESP_LOGI(TAG, "  Room: %s", room);
    }

    return 0;
}

static void register_aligned_status(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_status",
        .help = "Show token/room connection status",
        .hint = NULL,
        .func = &aligned_status_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Device Status Command **************/
static int device_status_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "Device status:");

    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "  WiFi: connected");
        ESP_LOGI(TAG, "  SSID: %s", (char *)ap_info.ssid);
        ESP_LOGI(TAG, "  RSSI: %d", ap_info.rssi);
        ESP_LOGI(TAG, "  Auth: %s", authmode_to_str(ap_info.authmode));
    }
    else
    {
        ESP_LOGI(TAG, "  WiFi: not connected (%s)", esp_err_to_name(err));
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL)
    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        {
            ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "  GW: " IPSTR, IP2STR(&ip_info.gw));
        }
    }

    ESP_LOGI(TAG, "  Token set: %s", aligned_has_token() ? "yes" : "no");
    ESP_LOGI(TAG, "  Credentials: %s", aligned_is_connected() ? "yes" : "no");
    ESP_LOGI(TAG, "  Room active: %s", room_is_active() ? "yes" : "no");

    const char *room = aligned_get_room_name();
    if (room && strlen(room) > 0)
    {
        ESP_LOGI(TAG, "  Room: %s", room);
    }

    return 0;
}

static void register_device_status(void)
{
    const esp_console_cmd_t cmd = {
        .command = "device_status",
        .help = "Show combined WiFi + Aligned status",
        .hint = NULL,
        .func = &device_status_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Aligned Restart Command **************/
static int aligned_restart_cmd(int argc, char **argv)
{
    if (room_is_active())
    {
        ESP_LOGI(TAG, "Leaving current room...");
        leave_room();
    }
    ESP_LOGI(TAG, "Rejoining LiveKit room...");
    return aligned_start_cmd(argc, argv);
}

static void register_aligned_restart(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_restart",
        .help = "Leave and rejoin the LiveKit room",
        .hint = NULL,
        .func = &aligned_restart_cmd,
        .argtable = NULL
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

/************* Touch Debug Command **************/
static int touch_debug_cmd(int argc, char **argv)
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0)
        {
            bsp_touch_debug_enable(true);
            return 0;
        }
        else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0)
        {
            bsp_touch_debug_enable(false);
            return 0;
        }
    }

    // Toggle or show status
    if (bsp_touch_debug_is_enabled())
    {
        bsp_touch_debug_enable(false);
    }
    else
    {
        bsp_touch_debug_enable(true);
    }
    return 0;
}

static void register_cmd_touch_debug(void)
{
    const esp_console_cmd_t cmd = {
        .command = "touch_debug",
        .help = "Toggle touch coordinate debug logging (on/off/toggle)",
        .hint = "[on|off]",
        .func = &touch_debug_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* QR Setup Command **************/
static int qr_setup_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "Opening QR setup screen...");

    // Initialize and show the QR setup screen
    qr_setup_init();
    qr_setup_show();

    // Show device ID in console for reference
    char device_id[32];
    if (qr_setup_get_device_id(device_id, sizeof(device_id)) != NULL) {
        ESP_LOGI(TAG, "Device ID: %s", device_id);
    }

    char mac_str[18];
    if (qr_setup_get_mac_address(mac_str, sizeof(mac_str)) != NULL) {
        ESP_LOGI(TAG, "MAC Address: %s", mac_str);
    }

    ESP_LOGI(TAG, "Scan the QR code with your phone to register this device.");
    ESP_LOGI(TAG, "URL: https://getaligned.ai/hardware/register?device=%s", device_id);
    return 0;
}

static void register_cmd_qr_setup(void)
{
    const esp_console_cmd_t cmd = {
        .command = "qr_setup",
        .help = "Show QR code for device registration",
        .hint = NULL,
        .func = &qr_setup_cmd,
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
    register_cmd_wifi_setup();
    register_cmd_wifi_scan();
    register_cmd_wifi_status();
    register_cmd_wifi_clear();
    register_cmd_wifi_reconnect();
    register_aligned_token();
    register_aligned_server();
    register_aligned_pair();
    register_aligned_start();
    register_aligned_stop();
    register_aligned_status();
    register_device_status();
    register_aligned_restart();
    register_cmd_reboot();
    register_cmd_touch_debug();
    register_cmd_qr_setup();

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
    ESP_LOGI(TAG, "  wifi_setup");
    ESP_LOGI(TAG, "  wifi_scan");
    ESP_LOGI(TAG, "  wifi_status");
    ESP_LOGI(TAG, "  wifi_clear");
    ESP_LOGI(TAG, "  wifi_reconnect");
    ESP_LOGI(TAG, "  aligned_token -t watcher_xxx...");
    ESP_LOGI(TAG, "  aligned_pair");
    ESP_LOGI(TAG, "  aligned_start");
    ESP_LOGI(TAG, "  aligned_stop");
    ESP_LOGI(TAG, "  aligned_status");
    ESP_LOGI(TAG, "  device_status");
    ESP_LOGI(TAG, "  aligned_restart");
    ESP_LOGI(TAG, "  qr_setup              - Show QR code for device registration");
    ESP_LOGI(TAG, "  touch_debug [on|off]  - Debug touch coordinates");
    ESP_LOGI(TAG, "  reboot");
    ESP_LOGI(TAG, "");

    if (!aligned_has_token()) {
        ESP_LOGW(TAG, "⚠️  No device token configured!");
        ESP_LOGI(TAG, "Set token with: aligned_token -t watcher_xxx...");
        ESP_LOGI(TAG, "");
    }

    return 0;
}
