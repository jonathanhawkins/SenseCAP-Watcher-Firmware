/**
 * @file pm.c
 * @brief Page Manager - copied from factory firmware
 *
 * Based on factory_firmware/main/view/ui_manager/pm.c
 */

#include "pm.h"
#include "esp_log.h"

static const char *TAG = "PM";

lv_pm_page_record g_page_record;
lv_group_t *g_main;
lv_indev_t *cur_drv;

static void lv_pm_obj_group(lv_group_t * group, GroupInfo *groupInfo);

// Function to add objects to the group
void addObjToGroup(GroupInfo *groupInfo, lv_obj_t *objects[], int count) {
    if (count > MAX_OBJECTS_IN_GROUP) {
        ESP_LOGE(TAG, "Error: Object count exceeds maximum limit");
        return;
    }

    // Copy objects into the group
    for (int i = 0; i < count; i++) {
        groupInfo->group[i] = objects[i];
    }
    // Update object count
    groupInfo->obj_count = count;
}

// Function to open page and add group
void lv_pm_open_page(lv_group_t * group, GroupInfo *groupInfo, pm_operation_t operation, lv_obj_t **target, lv_scr_load_anim_t fademode, int spd, int delay, void (*target_init)(void))
{
    ESP_LOGI(TAG, "lv_pm_open_page: target=%p, *target=%p, g_curpage=%p",
             target, target ? *target : NULL, g_page_record.g_curpage);

    // Save focused object
    if (g_page_record.g_curfocused_obj != NULL)
    {
        g_page_record.g_prefocused_obj = g_page_record.g_curfocused_obj;
    }
    g_page_record.g_curfocused_obj = lv_group_get_focused(g_main);

    // Save current page as previous page BEFORE updating curpage
    if (g_page_record.g_curpage != NULL)
    {
        g_page_record.g_prepage = g_page_record.g_curpage;
        ESP_LOGI(TAG, "Saved previous page: %p", g_page_record.g_prepage);
    } else {
        ESP_LOGW(TAG, "g_curpage is NULL, cannot save previous page!");
    }
    g_page_record.g_curpage = *target;

    // Initialize target if NULL
    if (*target == NULL && target_init != NULL) {
        ESP_LOGI(TAG, "Initializing target page via init function");
        target_init();
    }

    ESP_LOGI(TAG, "Page transition: prev=%p, cur=%p", g_page_record.g_prepage, g_page_record.g_curpage);

    switch (operation)
    {
        case PM_ADD_OBJS_TO_GROUP:
            if ((group != NULL) && (groupInfo != NULL)){
                lv_pm_obj_group(group, groupInfo);
            }
            break;
        case PM_NO_OPERATION:
            break;
        case PM_CLEAR_GROUP:
            if (group != NULL) {
                lv_group_remove_all_objs(group);
            }
            break;
    }

    // Focus previous object
    if (g_page_record.g_prefocused_obj != NULL) {
        lv_group_focus_obj(g_page_record.g_prefocused_obj);
    }

    // Load the screen with animation - EXACTLY like factory firmware pm.c line 108
    lv_scr_load_anim(*target, fademode, spd, delay, false);

    // Force immediate refresh to ensure screen is drawn
    lv_refr_now(NULL);
    ESP_LOGI(TAG, "Screen loaded and refresh forced");
}

static void lv_pm_obj_group(lv_group_t * group, GroupInfo *groupInfo)
{
    lv_group_remove_all_objs(group);
    for (uint8_t index = 0; index < groupInfo->obj_count; index++)
    {
        lv_group_add_obj(group, groupInfo->group[index]);
    }
}

// Function to init pm components
void lv_pm_init(lv_indev_t *encoder)
{
    ESP_LOGI(TAG, "Initializing Page Manager");

    // Clear page record
    g_page_record.g_prepage = NULL;
    g_page_record.g_prefocused_obj = NULL;
    g_page_record.g_curfocused_obj = NULL;

    // CRITICAL: Initialize current page with the active screen
    // This ensures the first page transition has a valid "previous page" to return to
    g_page_record.g_curpage = lv_scr_act();
    ESP_LOGI(TAG, "Initial active screen: %p", g_page_record.g_curpage);

    // Create main group
    g_main = lv_group_create();

    // Associate encoder with the group
    if (encoder != NULL) {
        lv_indev_set_group(encoder, g_main);
        cur_drv = encoder;
        ESP_LOGI(TAG, "Encoder associated with main group");
    } else {
        // Find encoder input device
        cur_drv = NULL;
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL)
        {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER)
            {
                lv_indev_set_group(indev, g_main);
                cur_drv = indev;
                ESP_LOGI(TAG, "Found and associated encoder device: %p", indev);
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Page Manager initialized, g_main=%p, cur_drv=%p", g_main, cur_drv);
}

// Return to previous page
void lv_pm_return_to_previous(void)
{
    if (g_page_record.g_prepage == NULL) {
        ESP_LOGW(TAG, "No previous page to return to");
        return;
    }

    ESP_LOGI(TAG, "Returning to previous page: %p", g_page_record.g_prepage);

    // Swap current and previous
    lv_obj_t *temp = g_page_record.g_curpage;
    g_page_record.g_curpage = g_page_record.g_prepage;
    g_page_record.g_prepage = temp;

    // Load the previous screen
    lv_scr_load_anim(g_page_record.g_curpage, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
