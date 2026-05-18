/**
 * @file pm.c
 * @brief Page Manager - Enhanced with navigation stack
 *
 * Based on factory_firmware/main/view/ui_manager/pm.c
 * Enhanced with 8-level navigation stack for proper back navigation.
 */

#include "pm.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "PM";

lv_pm_page_record g_page_record;
lv_group_t *g_main;
lv_indev_t *cur_drv;

// Track if we're currently showing an overlay
static bool s_overlay_active = false;
static lv_obj_t *s_page_before_overlay = NULL;

static void lv_pm_obj_group(lv_group_t * group, GroupInfo *groupInfo);
static void lv_pm_push_page(lv_obj_t *page, lv_obj_t *focused_obj);
static bool lv_pm_pop_page(lv_obj_t **page, lv_obj_t **focused_obj);

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

// Push current page onto the navigation stack
static void lv_pm_push_page(lv_obj_t *page, lv_obj_t *focused_obj)
{
    if (page == NULL) {
        ESP_LOGW(TAG, "Cannot push NULL page to stack");
        return;
    }

    if (g_page_record.stack_ptr >= PAGE_STACK_DEPTH) {
        ESP_LOGW(TAG, "Navigation stack full (depth=%d), shifting oldest entry", PAGE_STACK_DEPTH);
        // Shift stack to make room (drop oldest entry)
        memmove(&g_page_record.stack[0], &g_page_record.stack[1],
                sizeof(PageStackEntry) * (PAGE_STACK_DEPTH - 1));
        g_page_record.stack_ptr = PAGE_STACK_DEPTH - 1;
    }

    g_page_record.stack[g_page_record.stack_ptr].page = page;
    g_page_record.stack[g_page_record.stack_ptr].focused_obj = focused_obj;
    g_page_record.stack_ptr++;

    ESP_LOGI(TAG, "Pushed page %p to stack (depth now %d)", page, g_page_record.stack_ptr);
}

// Pop a page from the navigation stack
static bool lv_pm_pop_page(lv_obj_t **page, lv_obj_t **focused_obj)
{
    if (g_page_record.stack_ptr == 0) {
        ESP_LOGW(TAG, "Navigation stack empty");
        return false;
    }

    g_page_record.stack_ptr--;
    *page = g_page_record.stack[g_page_record.stack_ptr].page;
    *focused_obj = g_page_record.stack[g_page_record.stack_ptr].focused_obj;

    // Clear the slot
    g_page_record.stack[g_page_record.stack_ptr].page = NULL;
    g_page_record.stack[g_page_record.stack_ptr].focused_obj = NULL;

    ESP_LOGI(TAG, "Popped page %p from stack (depth now %d)", *page, g_page_record.stack_ptr);
    return true;
}

// Function to open page and add group
void lv_pm_open_page(lv_group_t * group, GroupInfo *groupInfo, pm_operation_t operation, lv_obj_t **target, lv_scr_load_anim_t fademode, int spd, int delay, void (*target_init)(void))
{
    ESP_LOGI(TAG, "lv_pm_open_page: target=%p, *target=%p, g_curpage=%p, stack_depth=%d",
             target, target ? *target : NULL, g_page_record.g_curpage, g_page_record.stack_ptr);

    // Get the currently focused object before transition
    lv_obj_t *current_focused = lv_group_get_focused(g_main);

    // Push current page onto the navigation stack (if we have a current page)
    if (g_page_record.g_curpage != NULL) {
        lv_pm_push_page(g_page_record.g_curpage, current_focused);
        // Also update legacy fields for compatibility
        g_page_record.g_prepage = g_page_record.g_curpage;
        g_page_record.g_prefocused_obj = current_focused;
    }

    // Save focused object (legacy)
    g_page_record.g_curfocused_obj = current_focused;

    // Initialize target if NULL
    if (*target == NULL && target_init != NULL) {
        ESP_LOGI(TAG, "Initializing target page via init function");
        target_init();
    }

    // Update current page
    g_page_record.g_curpage = *target;

    ESP_LOGI(TAG, "Page transition: prev=%p, cur=%p, stack_depth=%d",
             g_page_record.g_prepage, g_page_record.g_curpage, g_page_record.stack_ptr);

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
    ESP_LOGI(TAG, "Initializing Page Manager with navigation stack (depth=%d)", PAGE_STACK_DEPTH);

    // Clear the entire page record structure
    memset(&g_page_record, 0, sizeof(g_page_record));

    // CRITICAL: Initialize current page with the active screen
    // This ensures the first page transition has a valid "previous page" to return to
    g_page_record.g_curpage = lv_scr_act();
    g_page_record.home_page = g_page_record.g_curpage;  // Set initial screen as home
    ESP_LOGI(TAG, "Initial active screen (home): %p", g_page_record.g_curpage);

    // Reset overlay state
    s_overlay_active = false;
    s_page_before_overlay = NULL;

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

    ESP_LOGI(TAG, "Page Manager initialized, g_main=%p, cur_drv=%p, stack_ptr=%d",
             g_main, cur_drv, g_page_record.stack_ptr);
}

// Return to previous page (pops from stack)
void lv_pm_return_to_previous(void)
{
    lv_obj_t *prev_page = NULL;
    lv_obj_t *prev_focused = NULL;

    ESP_LOGI(TAG, "lv_pm_return_to_previous: stack_depth=%d, home=%p",
             g_page_record.stack_ptr, g_page_record.home_page);

    // Try to pop from the stack
    if (lv_pm_pop_page(&prev_page, &prev_focused)) {
        // Successfully popped a page from stack
        ESP_LOGI(TAG, "Returning to stacked page: %p", prev_page);

        // Update legacy fields for compatibility
        g_page_record.g_prepage = g_page_record.g_curpage;
        g_page_record.g_curpage = prev_page;

        // Load the previous screen
        lv_scr_load_anim(prev_page, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);

        // Restore focus if available
        if (prev_focused != NULL) {
            lv_group_focus_obj(prev_focused);
        }
    } else if (g_page_record.home_page != NULL) {
        // Stack is empty, go to home page
        ESP_LOGI(TAG, "Stack empty, returning to home page: %p", g_page_record.home_page);
        lv_pm_return_to_home();
    } else {
        ESP_LOGW(TAG, "No previous page or home page to return to");
    }
}

// Set the home page
void lv_pm_set_home_page(lv_obj_t *home_page)
{
    ESP_LOGI(TAG, "Setting home page: %p", home_page);
    g_page_record.home_page = home_page;
}

// Return directly to home page (clears stack)
void lv_pm_return_to_home(void)
{
    if (g_page_record.home_page == NULL) {
        ESP_LOGW(TAG, "No home page set");
        return;
    }

    ESP_LOGI(TAG, "Returning to home page: %p (clearing stack of %d entries)",
             g_page_record.home_page, g_page_record.stack_ptr);

    // Clear the navigation stack
    memset(g_page_record.stack, 0, sizeof(g_page_record.stack));
    g_page_record.stack_ptr = 0;

    // Update current page
    g_page_record.g_prepage = g_page_record.g_curpage;
    g_page_record.g_curpage = g_page_record.home_page;

    // Clear overlay state
    s_overlay_active = false;
    s_page_before_overlay = NULL;

    // Load home screen
    lv_scr_load_anim(g_page_record.home_page, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    lv_refr_now(NULL);
}

// Open a page as an overlay (doesn't affect navigation stack)
void lv_pm_open_overlay(lv_obj_t **target, void (*target_init)(void))
{
    ESP_LOGI(TAG, "Opening overlay: target=%p, *target=%p",
             target, target ? *target : NULL);

    // Save current page before showing overlay
    if (!s_overlay_active) {
        s_page_before_overlay = g_page_record.g_curpage;
        s_overlay_active = true;
        ESP_LOGI(TAG, "Saved page before overlay: %p", s_page_before_overlay);
    }

    // Initialize target if NULL
    if (*target == NULL && target_init != NULL) {
        target_init();
    }

    // Show the overlay screen (don't touch navigation stack or current page record)
    lv_scr_load_anim(*target, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// Close overlay and return to the page that was active
void lv_pm_close_overlay(void)
{
    if (!s_overlay_active || s_page_before_overlay == NULL) {
        ESP_LOGW(TAG, "No overlay to close");
        return;
    }

    ESP_LOGI(TAG, "Closing overlay, returning to: %p", s_page_before_overlay);

    // Return to the page that was showing before the overlay
    lv_scr_load_anim(s_page_before_overlay, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);

    // Clear overlay state
    s_overlay_active = false;
    s_page_before_overlay = NULL;
}

// Get current stack depth
uint8_t lv_pm_get_stack_depth(void)
{
    return g_page_record.stack_ptr;
}
