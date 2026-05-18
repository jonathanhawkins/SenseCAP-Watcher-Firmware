/**
 * @file qr_setup.c
 * @brief QR Code Setup Screen Implementation
 *
 * Implements the QR-based device registration flow:
 * 1. Shows QR code with device ID for user to scan
 * 2. Polls server for token after user registers via web
 * 3. Stores token and shows success when claimed
 */

#include "qr_setup.h"
#include "pm.h"
#include "aligned_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lvgl.h"
#include "src/extra/libs/qrcode/lv_qrcode.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "QR_SETUP";

// Screen and UI elements
static lv_obj_t *s_qr_setup_screen = NULL;
static lv_obj_t *s_qr_code = NULL;
static lv_obj_t *s_title_label = NULL;
static lv_obj_t *s_device_id_label = NULL;
static lv_obj_t *s_instruction_label = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_back_btn = NULL;

static bool s_visible = false;
static bool s_polling_enabled = false;
static lv_timer_t *s_poll_timer = NULL;
static char s_hardware_id[32] = {0};

// Screen dimensions (412x412 round display)
#define SCREEN_SIZE 412
#define QR_CODE_SIZE 180
#define POLL_INTERVAL_MS 5000  // Poll every 5 seconds

// Forward declarations
static void qr_setup_create_screen(void);
static void back_btn_event_cb(lv_event_t *e);
static void poll_timer_cb(lv_timer_t *timer);
static void qr_setup_start_polling(void);
static void qr_setup_stop_polling(void);

char *qr_setup_get_mac_address(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 18) {
        return NULL;
    }

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(err));
        return NULL;
    }

    snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

char *qr_setup_get_device_id(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 32) {
        return NULL;
    }

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(err));
        return NULL;
    }

    // Create device ID from MAC: WATCHER_XXXXXXXXXXXX
    snprintf(buf, buf_len, "WATCHER_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

static void qr_setup_create_screen(void)
{
    if (s_qr_setup_screen != NULL) {
        return; // Already created
    }

    ESP_LOGI(TAG, "Creating QR setup screen");

    // Create screen
    s_qr_setup_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_qr_setup_screen, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(s_qr_setup_screen, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_qr_setup_screen, LV_OPA_COVER, 0);

    // Title
    s_title_label = lv_label_create(s_qr_setup_screen);
    lv_label_set_text(s_title_label, "Device Setup");
    lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 45);

    // Get device ID for QR code and store for polling
    if (qr_setup_get_device_id(s_hardware_id, sizeof(s_hardware_id)) == NULL) {
        strcpy(s_hardware_id, "UNKNOWN_DEVICE");
    }

    // Build QR code URL
    char qr_url[128];
    snprintf(qr_url, sizeof(qr_url), "https://aligned.tools/hardware/register?device=%s", s_hardware_id);

    ESP_LOGI(TAG, "QR URL: %s", qr_url);

    // Create QR code (white QR on dark background for better visibility)
    s_qr_code = lv_qrcode_create(s_qr_setup_screen, QR_CODE_SIZE,
                                  lv_color_white(), lv_color_hex(0x1a1a2e));
    lv_qrcode_update(s_qr_code, qr_url, strlen(qr_url));
    lv_obj_align(s_qr_code, LV_ALIGN_CENTER, 0, -20);

    // Add a white border/background around QR code for better scan
    lv_obj_set_style_border_color(s_qr_code, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr_code, 6, 0);

    // Device ID label below QR
    s_device_id_label = lv_label_create(s_qr_setup_screen);
    char mac_str[18];
    if (qr_setup_get_mac_address(mac_str, sizeof(mac_str)) != NULL) {
        lv_label_set_text_fmt(s_device_id_label, "%s", mac_str);
    } else {
        lv_label_set_text(s_device_id_label, s_hardware_id);
    }
    lv_obj_set_style_text_color(s_device_id_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_device_id_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_device_id_label, LV_ALIGN_CENTER, 0, QR_CODE_SIZE/2 + 15);

    // Instruction label
    s_instruction_label = lv_label_create(s_qr_setup_screen);
    lv_label_set_text(s_instruction_label, "Scan with your phone");
    lv_obj_set_style_text_color(s_instruction_label, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(s_instruction_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_instruction_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_instruction_label, LV_ALIGN_CENTER, 0, QR_CODE_SIZE/2 + 40);

    // Status label (shows polling status)
    s_status_label = lv_label_create(s_qr_setup_screen);
    lv_label_set_text(s_status_label, "Waiting for registration...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x6699ff), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -65);

    // Back button (using a simple text button)
    s_back_btn = lv_btn_create(s_qr_setup_screen);
    lv_obj_set_size(s_back_btn, 100, 36);
    lv_obj_align(s_back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x333355), 0);
    lv_obj_set_style_radius(s_back_btn, 18, 0);
    lv_obj_add_event_cb(s_back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(s_back_btn);
    lv_label_set_text(btn_label, "Back");
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
    lv_obj_center(btn_label);

    // Add back button to group for encoder navigation
    if (g_main != NULL) {
        lv_group_add_obj(g_main, s_back_btn);
    }

    ESP_LOGI(TAG, "QR setup screen created");
}

static void back_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Back button clicked");
    qr_setup_hide();
}

/**
 * Timer callback to poll for device token
 */
static void poll_timer_cb(lv_timer_t *timer)
{
    if (!s_visible || !s_polling_enabled) {
        return;
    }

    // Check if we already have a token
    if (aligned_has_token()) {
        ESP_LOGI(TAG, "Token already present, stopping polling");
        qr_setup_stop_polling();
        if (s_status_label) {
            lv_label_set_text(s_status_label, "Already registered!");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x66ff66), 0);
        }
        return;
    }

    ESP_LOGI(TAG, "Polling for token...");
    if (s_status_label) {
        lv_label_set_text(s_status_label, "Checking registration...");
    }

    // Poll the server for our token
    bool claimed = aligned_poll_for_token(s_hardware_id);

    if (claimed) {
        // Token claimed successfully!
        ESP_LOGI(TAG, "Token claimed! Device is registered.");
        qr_setup_stop_polling();

        if (s_status_label) {
            lv_label_set_text(s_status_label, "Registered!");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x66ff66), 0);
        }
        if (s_instruction_label) {
            lv_label_set_text(s_instruction_label, "Press Back to continue");
        }
        if (s_title_label) {
            lv_label_set_text(s_title_label, "Success!");
            lv_obj_set_style_text_color(s_title_label, lv_color_hex(0x66ff66), 0);
        }
    } else {
        // Not registered yet, continue waiting
        if (s_status_label) {
            lv_label_set_text(s_status_label, "Waiting for registration...");
        }
    }
}

/**
 * Start polling for device token
 */
static void qr_setup_start_polling(void)
{
    if (s_polling_enabled) {
        return;
    }

    ESP_LOGI(TAG, "Starting token polling (every %d ms)", POLL_INTERVAL_MS);
    s_polling_enabled = true;

    // Create a timer to poll periodically
    if (s_poll_timer == NULL) {
        s_poll_timer = lv_timer_create(poll_timer_cb, POLL_INTERVAL_MS, NULL);
    } else {
        lv_timer_resume(s_poll_timer);
    }

    // Do an immediate poll
    poll_timer_cb(NULL);
}

/**
 * Stop polling for device token
 */
static void qr_setup_stop_polling(void)
{
    if (!s_polling_enabled) {
        return;
    }

    ESP_LOGI(TAG, "Stopping token polling");
    s_polling_enabled = false;

    if (s_poll_timer != NULL) {
        lv_timer_pause(s_poll_timer);
    }
}

void qr_setup_init(void)
{
    ESP_LOGI(TAG, "Initializing QR setup module");
    // Screen will be created lazily on first show
}

void qr_setup_show(void)
{
    ESP_LOGI(TAG, "Showing QR setup screen");

    // Create screen if needed
    if (s_qr_setup_screen == NULL) {
        qr_setup_create_screen();
    }

    // Reset status label if we're showing again
    if (s_status_label) {
        lv_label_set_text(s_status_label, "Waiting for registration...");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x6699ff), 0);
    }
    if (s_title_label) {
        lv_label_set_text(s_title_label, "Device Setup");
        lv_obj_set_style_text_color(s_title_label, lv_color_white(), 0);
    }
    if (s_instruction_label) {
        lv_label_set_text(s_instruction_label, "Scan with your phone");
    }

    // Show as overlay (doesn't affect navigation stack)
    lv_pm_open_overlay(&s_qr_setup_screen, NULL);

    // Focus the back button for encoder navigation
    if (g_main != NULL && s_back_btn != NULL) {
        lv_group_focus_obj(s_back_btn);
    }

    s_visible = true;

    // Start polling for token if we don't have one
    if (!aligned_has_token()) {
        qr_setup_start_polling();
    } else {
        // Already registered
        if (s_status_label) {
            lv_label_set_text(s_status_label, "Already registered!");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x66ff66), 0);
        }
    }
}

void qr_setup_hide(void)
{
    if (!s_visible) {
        return;
    }

    ESP_LOGI(TAG, "Hiding QR setup screen");

    // Stop polling
    qr_setup_stop_polling();

    lv_pm_close_overlay();
    s_visible = false;
}

bool qr_setup_is_visible(void)
{
    return s_visible;
}

bool qr_setup_process_scanned_qr(const char *qr_data)
{
    if (qr_data == NULL) {
        return false;
    }

    ESP_LOGI(TAG, "Processing scanned QR: %.50s...", qr_data);

    // Expected format: aligned://token/watcher_XXXX...
    // or: https://getaligned.ai/token/watcher_XXXX...
    const char *token_prefix1 = "aligned://token/";
    const char *token_prefix2 = "watcher_";

    const char *token_start = NULL;

    // Check for aligned:// scheme
    if (strncmp(qr_data, token_prefix1, strlen(token_prefix1)) == 0) {
        token_start = qr_data + strlen(token_prefix1);
    }
    // Check if the QR data starts directly with watcher_
    else if (strncmp(qr_data, token_prefix2, strlen(token_prefix2)) == 0) {
        token_start = qr_data;
    }
    // Check for token anywhere in the string
    else {
        token_start = strstr(qr_data, token_prefix2);
    }

    if (token_start == NULL) {
        ESP_LOGW(TAG, "QR code does not contain a valid device token");
        return false;
    }

    // Extract the token (it should start with watcher_ and be around 50 chars)
    char token[128];
    int i = 0;
    while (token_start[i] != '\0' && token_start[i] != '&' &&
           token_start[i] != ' ' && token_start[i] != '\n' &&
           i < sizeof(token) - 1) {
        token[i] = token_start[i];
        i++;
    }
    token[i] = '\0';

    // Validate token format
    if (strlen(token) < 20 || strncmp(token, "watcher_", 8) != 0) {
        ESP_LOGW(TAG, "Invalid token format: %s", token);
        return false;
    }

    ESP_LOGI(TAG, "Extracted token: %s", token);

    // Store the token using aligned_client
    aligned_set_device_token(token);

    // Verify it was stored
    if (!aligned_has_token()) {
        ESP_LOGE(TAG, "Failed to store token");
        return false;
    }

    ESP_LOGI(TAG, "Token successfully stored! Device is now registered.");
    return true;
}
