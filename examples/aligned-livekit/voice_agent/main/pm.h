/**
 * @file pm.h
 * @brief Page Manager - Enhanced with navigation stack
 *
 * Manages screen transitions with encoder group support.
 * Based on factory_firmware/main/view/ui_manager/pm.h
 * Enhanced with 8-level navigation stack for proper back navigation.
 */

#ifndef PM_H
#define PM_H

#include "lvgl.h"

extern lv_group_t *g_main;
extern lv_indev_t *cur_drv;

// Navigation stack configuration
#define PAGE_STACK_DEPTH 8

// Entry in the page navigation stack
typedef struct {
    lv_obj_t *page;           // The page screen object
    lv_obj_t *focused_obj;    // The focused object on this page
} PageStackEntry;

typedef struct
{
  // Legacy fields (kept for compatibility)
  lv_obj_t *g_prepage;
  lv_obj_t *g_curpage;
  lv_obj_t *g_prefocused_obj;
  lv_obj_t *g_curfocused_obj;

  // New: Navigation stack
  PageStackEntry stack[PAGE_STACK_DEPTH];
  uint8_t stack_ptr;          // Points to next free slot (0 = empty stack)
  lv_obj_t *home_page;        // The home/root page to return to
} lv_pm_page_record;

extern lv_pm_page_record g_page_record;

#define MAX_OBJECTS_IN_GROUP 12

typedef struct
{
  lv_obj_t * group[MAX_OBJECTS_IN_GROUP];     // Array of pointers to objects
  uint8_t obj_count;                          // Number of objects in group
} GroupInfo;

typedef enum
{
  PM_ADD_OBJS_TO_GROUP, // 0: change screen, and add all the objs to group
  PM_NO_OPERATION,      // 1: only change screen without operation
  PM_CLEAR_GROUP        // 2: change screen, and clear all the objs of group
} pm_operation_t;

/**
 * @brief Initialize the page manager (PM) system.
 *
 * This function initializes the page manager system by creating the main LVGL group,
 * assigning it to the encoder input device.
 *
 * @param encoder The encoder input device to associate with the group
 */
void lv_pm_init(lv_indev_t *encoder);

/**
 * @brief Open a new page and manage the group of objects.
 *
 * This function handles the transition to a new page, including setting the focused objects
 * and page records, managing the group of objects based on the specified operation, and
 * applying the screen transition animation.
 *
 * @param group Pointer to the LVGL group.
 * @param groupInfo Pointer to the GroupInfo structure containing the objects to be managed.
 * @param operation The operation to be performed on the group (e.g., add objects, clear group).
 * @param target Pointer to the target LVGL object (page) to be opened.
 * @param fademode The screen load animation mode.
 * @param spd The speed of the screen transition animation.
 * @param delay The delay before starting the screen transition animation.
 * @param target_init Pointer to the function that initializes the target page.
 */
void lv_pm_open_page(lv_group_t * group, GroupInfo *groupInfo, pm_operation_t operation, lv_obj_t **target,
                    lv_scr_load_anim_t fademode, int spd, int delay, void (*target_init)(void));

/**
 * @brief Return to the previous page (pops from navigation stack)
 *
 * Pops the top page from the navigation stack and returns to it.
 * If the stack is empty, returns to the home page if set.
 */
void lv_pm_return_to_previous(void);

/**
 * @brief Set the home/root page
 *
 * Sets the page that lv_pm_return_to_home() will navigate to.
 * Should be called after initializing the home screen.
 *
 * @param home_page The home screen object
 */
void lv_pm_set_home_page(lv_obj_t *home_page);

/**
 * @brief Return directly to the home page
 *
 * Clears the navigation stack and returns to the home page.
 * Used when the user wants to go back to the main screen.
 */
void lv_pm_return_to_home(void);

/**
 * @brief Open a page as an overlay (doesn't affect navigation stack)
 *
 * Use this for temporary UI elements like volume control, notifications,
 * etc. that shouldn't be part of the navigation history.
 *
 * @param target Pointer to the target LVGL object (page) to show
 * @param target_init Pointer to the function that initializes the target page
 */
void lv_pm_open_overlay(lv_obj_t **target, void (*target_init)(void));

/**
 * @brief Close an overlay and return to the current page
 *
 * Returns to the page that was active before the overlay was shown.
 */
void lv_pm_close_overlay(void);

/**
 * @brief Get the current stack depth
 *
 * @return The number of pages in the navigation stack
 */
uint8_t lv_pm_get_stack_depth(void);

#endif
