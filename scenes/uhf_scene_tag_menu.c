/**
 * uhf_scene_tag_menu.c
 *
 * Scene: Tag Menu
 *
 * Shown after a tag has been read or loaded from storage.
 * Provides access to all per-tag operations.
 *
 * Menu order:
 *   Save                        → UHFSceneSaveName
 *   Show EPC as QR Code         → UHFSceneTagQr
 *   Set/Reset Access Password   → UHFSceneSetAccessPassword
 *   Set Kill Password           → UHFSceneSetKillPassword
 *   Kill Tag                    → UHFSceneKillTag
 */

#include "../uhf_app_i.h"

typedef enum {
    SubmenuIndexSave,
    SubmenuIndexShowQR,              /* Show EPC as QR Code           */
    SubmenuIndexSetAccessPassword,   /* Set/Reset Access Password     */
    SubmenuIndexSetKillPassword,     /* Set Kill Password             */
    SubmenuIndexKillTag,             /* Kill Tag (irreversible)       */
    SubmenuIndexChangeKey,           /* Reserved for future key mgmt */
} SubmenuIndex;

/* ── Submenu callback ────────────────────────────────────────────────────────
 * Forwards the selected index to the scene as a custom event.
 * ────────────────────────────────────────────────────────────────────────── */
void uhf_scene_tag_menu_submenu_callback(void* ctx, uint32_t index) {
    UHFApp* uhf_app = ctx;
    view_dispatcher_send_custom_event(uhf_app->view_dispatcher, index);
}

/* ── on_enter ────────────────────────────────────────────────────────────── */
void uhf_scene_tag_menu_on_enter(void* ctx) {
    UHFApp* uhf_app = ctx;
    Submenu* submenu = uhf_app->submenu;

    submenu_add_item(
        submenu, "Save", SubmenuIndexSave, uhf_scene_tag_menu_submenu_callback, uhf_app);

    submenu_add_item(
        submenu,
        "Show EPC as QR Code",
        SubmenuIndexShowQR,
        uhf_scene_tag_menu_submenu_callback,
        uhf_app);

    submenu_add_item(
        submenu,
        "Set/Reset Access Password",
        SubmenuIndexSetAccessPassword,
        uhf_scene_tag_menu_submenu_callback,
        uhf_app);

    submenu_add_item(
        submenu,
        "Set Kill Password",
        SubmenuIndexSetKillPassword,
        uhf_scene_tag_menu_submenu_callback,
        uhf_app);

    submenu_add_item(
        submenu,
        "Kill Tag",
        SubmenuIndexKillTag,
        uhf_scene_tag_menu_submenu_callback,
        uhf_app);

    /* Restore cursor to the previously selected item */
    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(uhf_app->scene_manager, UHFSceneTagMenu));

    view_dispatcher_switch_to_view(uhf_app->view_dispatcher, UHFViewMenu);
}

/* ── on_event ────────────────────────────────────────────────────────────── */
bool uhf_scene_tag_menu_on_event(void* ctx, SceneManagerEvent event) {
    UHFApp* uhf_app = ctx;
    bool consumed   = false;

    if(event.type == SceneManagerEventTypeCustom) {

        /* Save cursor position so the correct item is re-selected on return */
        scene_manager_set_scene_state(uhf_app->scene_manager, UHFSceneTagMenu, event.event);

        if(event.event == SubmenuIndexSave) {
            scene_manager_next_scene(uhf_app->scene_manager, UHFSceneSaveName);
            consumed = true;

        } else if(event.event == SubmenuIndexShowQR) {
            scene_manager_next_scene(uhf_app->scene_manager, UHFSceneTagQr);
            consumed = true;

        } else if(event.event == SubmenuIndexSetAccessPassword) {
            scene_manager_next_scene(uhf_app->scene_manager, UHFSceneSetAccessPassword);
            consumed = true;

        } else if(event.event == SubmenuIndexSetKillPassword) {
            scene_manager_next_scene(uhf_app->scene_manager, UHFSceneSetKillPassword);
            consumed = true;

        } else if(event.event == SubmenuIndexKillTag) {
            scene_manager_next_scene(uhf_app->scene_manager, UHFSceneKillTag);
            consumed = true;
        }
        /* SubmenuIndexChangeKey: reserved for future implementation */

    } else if(event.type == SceneManagerEventTypeBack) {
        consumed = scene_manager_search_and_switch_to_previous_scene(
            uhf_app->scene_manager, UHFSceneStart);
    }

    return consumed;
}

/* ── on_exit ─────────────────────────────────────────────────────────────── */
void uhf_scene_tag_menu_on_exit(void* ctx) {
    UHFApp* uhf_app = ctx;
    submenu_reset(uhf_app->submenu);
}
