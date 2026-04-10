// clang-format off
// ─────────────────────────────────────────────────────────────────────────────
// uhf_scene_config.h
//
// Scene registration table.  Each line expands to a scene enum value AND
// three handler function declarations via the ADD_SCENE macro in uhf_scene.h.
//
// Format: ADD_SCENE(prefix, snake_name, PascalId)
// ─────────────────────────────────────────────────────────────────────────────

ADD_SCENE(uhf, verify,             Verify)
ADD_SCENE(uhf, start,              Start)
ADD_SCENE(uhf, read_tag,           ReadTag)
ADD_SCENE(uhf, read_tag_success,   ReadTagSuccess)
ADD_SCENE(uhf, tag_menu,           TagMenu)
ADD_SCENE(uhf, save_name,          SaveName)
ADD_SCENE(uhf, save_success,       SaveSuccess)
ADD_SCENE(uhf, saved_menu,         SavedMenu)
ADD_SCENE(uhf, file_select,        FileSelect)
ADD_SCENE(uhf, device_info,        DeviceInfo)
ADD_SCENE(uhf, delete,             Delete)
ADD_SCENE(uhf, delete_success,     DeleteSuccess)
ADD_SCENE(uhf, write_tag,          WriteTag)
ADD_SCENE(uhf, write_tag_success,  WriteTagSuccess)
ADD_SCENE(uhf, tag_qr,             TagQr)             /* NEW: Show EPC as QR Code      */
ADD_SCENE(uhf, set_access_password, SetAccessPassword) /* NEW: Set/Reset Access Password */
ADD_SCENE(uhf, set_kill_password,   SetKillPassword)   /* NEW: Set Kill Password         */
ADD_SCENE(uhf, kill_tag,            KillTag)           /* NEW: Kill Tag (irreversible)   */
ADD_SCENE(uhf, bulk_scan,           BulkScan)          /* NEW: Bulk Inventory Scan       */
ADD_SCENE(uhf, settings,            Settings)          /* NEW: Settings (baud/pwr/region/write) */
ADD_SCENE(uhf, brute_force,         BruteForce)        /* NEW: Kill password audit / brute force */

// clang-format on
