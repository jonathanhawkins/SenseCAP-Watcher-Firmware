#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "main.h"  // For Aligned functions

static const char *TAG = "cmd";

#define PROMPT_STR "Aligned"  // Changed from SenseCAP
#define STORAGE_NAMESPACE "SenseCAP"
#define OPENAI_API_KEY_STORAGE  "openaikey"
#define ALIGNED_TOKEN_STORAGE   "aligned_token"

char g_openai_api_key_buf[165] = {0,};
static char g_aligned_token_buf[256] = {0,};

static int max(int a, int b) {
    return (a > b) ? a : b;
}

static esp_err_t storage_write(char *p_key, void *p_data, size_t len)
{
    nvs_handle_t my_handle;
    esp_err_t err;
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(my_handle,  p_key, p_data, len);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }
    nvs_close(my_handle);
    return ESP_OK;
}

static esp_err_t storage_read(char *p_key, void *p_data, size_t *p_len)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_get_blob(my_handle, p_key, p_data, p_len);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }
    nvs_close(my_handle);
    return ESP_OK;
}

/** wifi set command **/
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_cfg_args;



static int wifi_cfg_set(int argc, char **argv)
{
    bool have_password = false;
    char ssid[32]= {0};
    char password[64] = {0};
    wifi_config_t wifi_config = { 0 };

    int nerrors = arg_parse(argc, argv, (void **) &wifi_cfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_cfg_args.end, argv[0]);
        return 1;
    }

    if (wifi_cfg_args.ssid->count) {
        int len = strlen( wifi_cfg_args.ssid->sval[0] );
        if( len >  (sizeof(ssid) - 1) ) { 
            ESP_LOGE(TAG,  "out of 31 bytes :%s", wifi_cfg_args.ssid->sval[0]);
            return -1;
        }
        strncpy( ssid, wifi_cfg_args.ssid->sval[0], 31 );
    } else {
        ESP_LOGE(TAG,  "no ssid");
        return -1;
    }

    if (wifi_cfg_args.password->count) {
        int len = strlen(wifi_cfg_args.password->sval[0]);
        if( len > (sizeof(password) - 1) ){ 
            ESP_LOGE(TAG,  "out of 64 bytes :%s", wifi_cfg_args.password->sval[0]);
            return -1;
        }
        have_password = true;
        strncpy( password, wifi_cfg_args.password->sval[0], 63 );
    }
    
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));

    if( have_password ) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else
    {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // CRITICAL FIX: Save WiFi credentials to flash so they persist across reboots
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_ERROR_CHECK(esp_wifi_start());
    // Avoid logging WiFi credentials in plain text
    ESP_LOGI(
        TAG,
        "config wifi, SSID: %s, password_set: %s, password_length: %d",
        wifi_config.sta.ssid,
        (strlen((const char *)wifi_config.sta.password) > 0) ? "true" : "false",
        (int)strlen((const char *)wifi_config.sta.password)
    );
    return 0;
}

//wifi_cfg -s ssid -p password
static void register_cmd_wifi_sta(void)
{
    wifi_cfg_args.ssid =  arg_str0("s", NULL, "<ssid>", "SSID of AP");
    wifi_cfg_args.password =  arg_str0("p", NULL, "<password>", "password of AP");
    wifi_cfg_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "wifi_sta",
        .help = "WiFi is station mode, join specified soft-AP",
        .hint = NULL,
        .func = &wifi_cfg_set,
        .argtable = &wifi_cfg_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}


/************* reboot **************/
static int do_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static void register_cmd_reboot(void)
{
    const esp_console_cmd_t cmd = {
        .command = "reboot",
        .help = "reboot the device",
        .hint = NULL,
        .func = &do_reboot,
        .argtable = NULL
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}



/** openai api key set command **/
static struct {
    struct arg_str *key;
    struct arg_end *end;
} openai_api_key_args;

static int openai_api_key_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &openai_api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, openai_api_key_args.end, argv[0]);
        return 1;
    }
    char key_buf[165] = {0,};
    size_t len = 0;
    if (openai_api_key_args.key->count) {
        len = strlen(openai_api_key_args.key->sval[0]);
        if( len >= sizeof(key_buf)) {
            ESP_LOGE(TAG,  "out of 164 bytes :%s", openai_api_key_args.key->sval[0]);
            return -1;
        }
        strncpy( key_buf, openai_api_key_args.key->sval[0], len );

        // Avoid logging secrets in plain text
        if (len > 8) ESP_LOGI(TAG, "write openai api key: %c%c%c%c...%c%c%c%c (length: %d)", key_buf[0], key_buf[1], key_buf[2], key_buf[3], key_buf[len - 4], key_buf[len - 3], key_buf[len - 2], key_buf[len - 1], (int)len);
        else ESP_LOGI(TAG, "write openai api key (length: %d)", (int)len);
        storage_write(OPENAI_API_KEY_STORAGE, (void *)key_buf, sizeof(key_buf));
    }
   
    len=sizeof(key_buf);
    memset(key_buf, 0, sizeof(key_buf));
    esp_err_t ret = storage_read(OPENAI_API_KEY_STORAGE, (void *)key_buf, &len);
    if (ret == ESP_OK) {
        if (strlen(key_buf) > 8) ESP_LOGI(TAG, "read openai api key: %c%c%c%c...%c%c%c%c", key_buf[0], key_buf[1], key_buf[2], key_buf[3], key_buf[strlen(key_buf) - 4], key_buf[strlen(key_buf) - 3], key_buf[strlen(key_buf) - 2], key_buf[strlen(key_buf) - 1]);
        else ESP_LOGI(TAG, "read openai api key (length: %d)", (int)strlen(key_buf));
	} else {
        ESP_LOGE(TAG, "openai api key read fail!");
	}
    return 0;
}

//openai_api -k sk-xxxx
static void register_openai_api_key(void)
{
    openai_api_key_args.key =  arg_str0("k", NULL, "<k>", "set key, eg: sk-xxxx..., 51 bytes"); 
    openai_api_key_args.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "openai_api",
        .help = "Set OpenAI API key (legacy / openai-realtime only). Not required for aligned-realtime.",
        .hint = NULL,
        .func = &openai_api_key_set,
        .argtable = &openai_api_key_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&cmd) );
}


/************* Aligned Token Commands **************/
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
        size_t len = strlen(aligned_token_args.token->sval[0]);
        if (len >= sizeof(g_aligned_token_buf)) {
            ESP_LOGE(TAG, "Token too long (max 255 bytes)");
            return -1;
        }
        
        // Use the Aligned client function to set and save token
        aligned_set_device_token(aligned_token_args.token->sval[0]);
        ESP_LOGI(TAG, "✅ Aligned device token set (length: %d)", len);
    } else {
        // Show current token (masked)
        const char *token = aligned_get_device_token();
        if (token && strlen(token) > 0) {
            ESP_LOGI(TAG, "Current token: %s...%s (length: %d)", 
                     token, token + strlen(token) - 4, strlen(token));
        } else {
            ESP_LOGW(TAG, "No device token configured");
            ESP_LOGI(TAG, "Set token with: aligned_token -t watcher_xxx...");
        }
    }
    
    return 0;
}

// aligned_token -t watcher_xxxxx...
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

/************* Aligned API URL Commands **************/
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
        // Reset to production URL
        aligned_set_api_url(NULL);
        ESP_LOGI(TAG, "✅ API URL reset to production: %s", aligned_get_api_base());
    } else if (aligned_api_args.url->count) {
        // Set custom URL
        aligned_set_api_url(aligned_api_args.url->sval[0]);
        ESP_LOGI(TAG, "✅ API URL set to: %s", aligned_get_api_base());
    } else {
        // Show current URL
        ESP_LOGI(TAG, "Current API URL: %s", aligned_get_api_base());
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Usage:");
        ESP_LOGI(TAG, "  aligned_api -u http://192.168.1.x:3000  # Set local dev");
        ESP_LOGI(TAG, "  aligned_api --reset                     # Reset to production");
    }

    return 0;
}

static void register_aligned_api(void)
{
    aligned_api_args.url = arg_str0("u", "url", "<url>", "API base URL (e.g., http://192.168.1.x:3000)");
    aligned_api_args.reset = arg_lit0("r", "reset", "Reset to production URL (https://aligned.tools)");
    aligned_api_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "aligned_api",
        .help = "Set Aligned Tools API URL (for local development)",
        .hint = NULL,
        .func = &aligned_api_set,
        .argtable = &aligned_api_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

// Connect to Aligned and get LiveKit credentials
static int aligned_connect_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "Connecting to Aligned Tools...");

    esp_err_t err = aligned_get_livekit_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get credentials from Aligned");
        return -1;
    }

    ESP_LOGI(TAG, "Connected! Room: %s", aligned_get_room_name());
    ESP_LOGI(TAG, "Voice provider: %s ($0.05/min)", "xai");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "To start voice, run: aligned_voice");

    return 0;
}

static void register_aligned_connect(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_connect",
        .help = "Connect to Aligned Tools and get LiveKit credentials",
        .hint = NULL,
        .func = &aligned_connect_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

// Start voice conversation (requires aligned_connect first)
static int aligned_voice_cmd(int argc, char **argv)
{
    if (!aligned_is_connected()) {
        ESP_LOGE(TAG, "Not connected to Aligned. Run 'aligned_connect' first.");
        return -1;
    }

    if (livekit_is_active()) {
        ESP_LOGW(TAG, "Voice is already active");
        return 0;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting Aligned Voice with xAI Grok");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Room: %s", aligned_get_room_name());
    ESP_LOGI(TAG, "Cost: $0.05/minute (63 credits/min)");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Speak into the microphone to talk with AI...");
    ESP_LOGI(TAG, "");

    // Join LiveKit room using SDK signaling
    join_room();

    return 0;
}

static void register_aligned_voice(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_voice",
        .help = "Start voice conversation with xAI Grok (requires aligned_connect first)",
        .hint = NULL,
        .func = &aligned_voice_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

// Quick command: connect + voice in one step
static int aligned_start_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "Starting Aligned Voice Assistant...");

    // Step 1: Get credentials
    esp_err_t err = aligned_get_livekit_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get credentials from Aligned");
        return -1;
    }

    ESP_LOGI(TAG, "Connected! Room: %s", aligned_get_room_name());

    // Step 2: Start voice
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Aligned Voice with xAI Grok Realtime");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Cost: $0.05/minute");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Speak into the microphone...");
    ESP_LOGI(TAG, "");

    join_room();

    return 0;
}

static void register_aligned_start(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_start",
        .help = "Quick start: connect to Aligned and start voice in one command",
        .hint = NULL,
        .func = &aligned_start_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Stop Voice Command **************/
static int aligned_stop_cmd(int argc, char **argv)
{
    if (!livekit_is_active()) {
        ESP_LOGI(TAG, "Voice is not active");
        return 0;
    }

    ESP_LOGI(TAG, "Stopping voice connection...");
    livekit_stop();
    ESP_LOGI(TAG, "Voice disconnected. Use 'aligned_start' to reconnect.");
    return 0;
}

static void register_aligned_stop(void)
{
    const esp_console_cmd_t cmd = {
        .command = "aligned_stop",
        .help = "Stop voice connection cleanly (allows reconnection without reboot)",
        .hint = NULL,
        .func = &aligned_stop_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* Sleep Command **************/
// Defined in main.cpp as extern "C"
extern "C" void device_enter_sleep(void);

static int sleep_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "Entering sleep mode...");
    device_enter_sleep();
    return 0;
}

static void register_sleep(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sleep",
        .help = "Enter low-power sleep mode (wakes up periodically)",
        .hint = NULL,
        .func = &sleep_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/************* cmd register **************/
int cmd_init(void)
{
#if CONFIG_ENABLE_FACTORY_FW_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    /* Prompt to be printed before each line.
     * This can be customized, made dynamic, etc.
     */
    repl_config.prompt = PROMPT_STR ">";
    repl_config.max_cmdline_length = 1024;
    // Increase stack size to prevent overflow during voice connection logging
    repl_config.task_stack_size = 8192;

    register_cmd_wifi_sta();
    register_openai_api_key();
    register_cmd_reboot();

    // Aligned Tools commands
    register_aligned_token();
    register_aligned_api();
    register_aligned_connect();
    register_aligned_voice();
    register_aligned_start();
    register_aligned_stop();
    register_sleep();

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
    // Since we have SD card access in console cmd, it might trigger the SPI core-conflict issue
    // we can't control the core on which the console runs, so 
    // TODO: narrow the SD card access code into another task which runs on Core 1.
    ESP_ERROR_CHECK(esp_console_start_repl(repl));


    // NOTE: aligned-realtime does NOT require an OpenAI API key.
    // Keep OpenAI key support for backwards compatibility, but don't block boot.
#ifndef OPENAI_API_KEY
    size_t len=sizeof(g_openai_api_key_buf);
    memset(g_openai_api_key_buf, 0, sizeof(g_openai_api_key_buf));
    esp_err_t ret = storage_read(OPENAI_API_KEY_STORAGE, (void *)g_openai_api_key_buf, &len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,"read openai api key");
	} else {
        ESP_LOGW(TAG, "OpenAI API key not set (OK for aligned-realtime).");
        ESP_LOGI(TAG, "If you are using legacy OpenAI mode, set with: openai_api -k sk-... then reboot");
	}
#else
    ESP_LOGI(TAG,"read OPENAI_API_KEY");
    memset(g_openai_api_key_buf, 0, sizeof(g_openai_api_key_buf));
    memcpy(g_openai_api_key_buf, OPENAI_API_KEY, strlen(OPENAI_API_KEY));
#endif

    // Auto-set device token for testing (hardcoded)
    if (!aligned_has_token()) {
        ESP_LOGW(TAG, "No device token found, using hardcoded token for testing...");
        aligned_set_device_token("watcher_p0cwCmTU5ED3_6ChqTzg6NE-oUOsJ1jthn8Q6KtjUvs");
        ESP_LOGI(TAG, "✅ Hardcoded device token set!");
    }
    ESP_LOGI(TAG, "Set WiFi with: wifi_sta -s <ssid> -p <password>");
    ESP_LOGI(TAG, "Or use: aligned_connect to connect with saved credentials");



    return 0;
}
