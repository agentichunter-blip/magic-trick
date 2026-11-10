cmake_minimum_required(VERSION 3.22)
project(chromatic_firmware_host_tests C)
set(CMAKE_C_STANDARD 11)
if(APPLE)
  set(DEAD_STRIP -Wl,-dead_strip)
else()
  set(DEAD_STRIP -Wl,--gc-sections)
endif()
set(FW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(LV_ROOT "${FW_ROOT}/managed_components/lvgl__lvgl")
file(GLOB_RECURSE LV_SOURCES "${LV_ROOT}/src/*.c")
add_library(lvgl_host STATIC ${LV_SOURCES})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_SKIP LV_COLOR_DEPTH=16 LV_FONT_MONTSERRAT_28=1 LV_MEM_SIZE=32768 LV_LVGL_H_INCLUDE_SIMPLE "CONFIG_CHROMATIC_FW_VER_STR=\"1.0.0 (4.2)\"")
target_include_directories(lvgl_host PUBLIC "${LV_ROOT}" "stubs")
set(FW_INCLUDES main components/common components/dlist components/button components/battery components/settings components/mutex components/osd components/osd/system components/osd/status components/menu_mgr components/tab)
foreach(DIR ${FW_INCLUDES})
  target_include_directories(lvgl_host PUBLIC "${FW_ROOT}/${DIR}")
endforeach()
add_executable(osd_test osd_test.c
  "${FW_ROOT}/components/tab/tab_list/tab_list.c"
  "${FW_ROOT}/components/tab/tab_shared.c"
  "${FW_ROOT}/components/battery/battery.c"
  "${FW_ROOT}/components/images/icon_bat_large_g.c"
  "${FW_ROOT}/components/images/icon_bat_large_y.c"
  "${FW_ROOT}/components/images/icon_bat_large_r.c"
  "${FW_ROOT}/components/images/icon_charging.c"
  "${FW_ROOT}/components/osd/system/rom_browser_ui.c"
  "${FW_ROOT}/components/osd/system/rom_browser_scroll.c"
  "${FW_ROOT}/components/osd/system/banner_font.c"
  "${FW_ROOT}/components/osd/system/progress_font.c"
  "${FW_ROOT}/components/common/rom_title.c"
  "${FW_ROOT}/components/osd/system/cart_backup_ui.c"
  "${FW_ROOT}/components/osd/system/native_state_ui.c"
  "${FW_ROOT}/components/osd/status/fw.c"
  "${FW_ROOT}/components/menu_mgr/menu_mgr.c"
  "${FW_ROOT}/components/common/osd_shared.c"
  "${FW_ROOT}/components/fonts/fingfai.c"
  "${FW_ROOT}/components/fonts/jf_dot_k14.c"
  "${FW_ROOT}/components/images/menu_system.c"
  "${FW_ROOT}/components/images/menu_backups_accent.c"
  "${FW_ROOT}/components/images/menu_status.c"
  "${FW_ROOT}/components/images/img_chromatic.c"
  "${FW_ROOT}/components/images/img_magician_hat.c"
  "${FW_ROOT}/components/images/img_magician_wand.c")
target_link_libraries(osd_test PRIVATE lvgl_host m)
add_executable(pc_mode_test pc_mode_test.c "${FW_ROOT}/components/osd/system/pc_backup_mode.c")
target_compile_options(pc_mode_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(pc_mode_test PRIVATE ${DEAD_STRIP})
target_link_libraries(pc_mode_test PRIVATE lvgl_host m)
enable_testing()
add_executable(gfx_start_test gfx_start_test.c "${FW_ROOT}/main/gfx.c")
target_link_libraries(gfx_start_test PRIVATE lvgl_host m)
add_test(NAME menu_startup_ready COMMAND gfx_start_test)
add_test(NAME backups_menu COMMAND osd_test)
add_executable(rom_browser_test rom_browser_test.c "${FW_ROOT}/main/rom_browser.c" "${FW_ROOT}/components/common/rom_title.c")
target_compile_definitions(rom_browser_test PRIVATE ROM_BROWSER_ROOT="${CMAKE_CURRENT_BINARY_DIR}/rom-browser-fixture")
target_link_libraries(rom_browser_test PRIVATE lvgl_host m)
add_test(NAME backup_folders COMMAND rom_browser_test)
add_test(NAME pc_mode_lifecycle COMMAND pc_mode_test)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME qspi_input_timing COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/qspi_timing_test.py" --cc "${CMAKE_C_COMPILER}")

find_path(CJSON_SOURCE_DIR cJSON.c PATHS "$ENV{IDF_PATH}/components/json/cJSON" "$ENV{HOME}/esp-idf/components/json/cJSON" REQUIRED)
add_executable(sd_files_test sd_files_test.c "${CJSON_SOURCE_DIR}/cJSON.c" "${FW_ROOT}/main/sd_publish.c")
target_include_directories(sd_files_test PRIVATE "${CJSON_SOURCE_DIR}")
target_compile_options(sd_files_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(sd_files_test PRIVATE ${DEAD_STRIP})
target_link_libraries(sd_files_test PRIVATE lvgl_host m)
add_test(NAME sd_files COMMAND sd_files_test)
add_executable(sd_mount_test sd_mount_test.c)
target_link_libraries(sd_mount_test PRIVATE lvgl_host m)
add_test(NAME sd_mount COMMAND sd_mount_test)

add_executable(virtual_geometry_test virtual_geometry_test.c "${FW_ROOT}/main/cart_mapper.c")
target_compile_options(virtual_geometry_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(virtual_geometry_test PRIVATE ${DEAD_STRIP})
target_link_libraries(virtual_geometry_test PRIVATE lvgl_host m)
add_test(NAME virtual_small_ram COMMAND virtual_geometry_test ram)
add_test(NAME virtual_rom_extent COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/checksum_diagnostic_test.py" "$<TARGET_FILE:virtual_geometry_test>")
add_test(NAME virtual_folder_paths COMMAND virtual_geometry_test paths)

add_executable(virtual_transition_test virtual_transition_test.c "${FW_ROOT}/main/cart_rtc.c")
target_compile_options(virtual_transition_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(virtual_transition_test PRIVATE ${DEAD_STRIP})
target_link_libraries(virtual_transition_test PRIVATE lvgl_host m)
if(DEFINED VIRTUAL_CART_TEST_SOURCE)
  target_compile_definitions(virtual_transition_test PRIVATE CM_VIRTUAL_CART_SOURCE="${VIRTUAL_CART_TEST_SOURCE}")
endif()
add_test(NAME virtual_final_save COMMAND virtual_transition_test)

add_executable(virtual_load_test virtual_load_test.c
  "${FW_ROOT}/main/cart_mapper.c" "${FW_ROOT}/main/cart_rtc.c")
target_compile_options(virtual_load_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(virtual_load_test PRIVATE ${DEAD_STRIP})
target_link_libraries(virtual_load_test PRIVATE lvgl_host m)
add_test(NAME virtual_load_cleanup COMMAND virtual_load_test)

add_executable(browser_scroll_test browser_scroll_test.c
  "${FW_ROOT}/components/osd/system/rom_browser_scroll.c"
  "${FW_ROOT}/components/common/rom_title.c")
target_link_libraries(browser_scroll_test PRIVATE lvgl_host m)
add_test(NAME browser_acceleration COMMAND browser_scroll_test)
add_executable(box_art_test box_art_test.c "${FW_ROOT}/main/box_art.c")
target_link_libraries(box_art_test PRIVATE lvgl_host m)
add_test(NAME box_art_format COMMAND box_art_test)
add_executable(browser_worker_test browser_worker_test.c "${FW_ROOT}/main/box_art.c" "${FW_ROOT}/main/video_preview.c")
target_link_libraries(browser_worker_test PRIVATE lvgl_host m)
add_test(NAME browser_background_jobs COMMAND browser_worker_test)
add_test(NAME box_art_converter COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/box_art_converter_test.py")

# Read-only batch checks for supplied ROM collections and converted cover assets.
add_executable(library_preflight "${FW_ROOT}/tools/library_preflight.c"
  "${FW_ROOT}/main/cart_mapper.c" "${FW_ROOT}/main/box_art.c")
target_compile_options(library_preflight PRIVATE -ffunction-sections -fdata-sections)
target_link_options(library_preflight PRIVATE ${DEAD_STRIP})
target_link_libraries(library_preflight PRIVATE lvgl_host m)
add_test(NAME library_repairs COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/library_repair_test.py")

add_executable(video_preview_test video_preview_test.c "${FW_ROOT}/main/video_preview.c" "${FW_ROOT}/main/box_art.c")
target_link_libraries(video_preview_test PRIVATE lvgl_host m)
add_test(NAME video_preview_format COMMAND video_preview_test)
add_test(NAME gameplay_converter COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/video_converter_test.py")

add_executable(boot_resume_test boot_resume_test.c "${FW_ROOT}/main/boot_resume.c" "${FW_ROOT}/main/last_game.c" "${FW_ROOT}/main/intro_video.c" "${FW_ROOT}/main/intro_video_data.c")
target_link_libraries(boot_resume_test PRIVATE lvgl_host m)
add_test(NAME boot_resume COMMAND boot_resume_test)

foreach(TARGET virtual_geometry_test virtual_transition_test virtual_load_test)
  target_sources(${TARGET} PRIVATE "${FW_ROOT}/main/cheat_patch.c")
endforeach()
add_executable(cheats_test cheats_test.c "${FW_ROOT}/main/cheats.c" "${FW_ROOT}/main/cheat_patch.c")
target_compile_definitions(cheats_test PRIVATE CHEAT_ROM_ROOT="${CMAKE_CURRENT_BINARY_DIR}/cheat-fixture/BACKUPS/" CHEAT_PACK_ROOT="${CMAKE_CURRENT_BINARY_DIR}/cheat-fixture/CHEATS/")
target_link_libraries(cheats_test PRIVATE lvgl_host m)
add_test(NAME cheat_patches_and_settings COMMAND cheats_test)
add_test(NAME cheat_database_converter COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/cheat_converter_test.py")

add_executable(button_input_test button_input_test.c "${FW_ROOT}/components/button/button.c")
target_compile_options(button_input_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(button_input_test PRIVATE ${DEAD_STRIP})
target_link_libraries(button_input_test PRIVATE lvgl_host m)
add_test(NAME physical_select_button COMMAND button_input_test)

target_sources(osd_test PRIVATE "${FW_ROOT}/components/button/button.c")
target_compile_options(osd_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(osd_test PRIVATE ${DEAD_STRIP})

add_executable(recent_games_test recent_games_test.c "${FW_ROOT}/main/recent_games.c")
target_link_libraries(recent_games_test PRIVATE lvgl_host m)
add_test(NAME recent_game_history COMMAND recent_games_test)

target_sources(boot_resume_test PRIVATE "${FW_ROOT}/components/common/rom_title.c")
target_sources(recent_games_test PRIVATE "${FW_ROOT}/components/common/rom_title.c")

add_executable(wifi_file_store_test wifi_file_store_test.c "${FW_ROOT}/main/wifi_file_store.c")
target_include_directories(wifi_file_store_test PRIVATE "${FW_ROOT}/main")
add_test(NAME wifi_file_store COMMAND wifi_file_store_test)
add_executable(wifi_http_test wifi_http_test.c "${FW_ROOT}/main/wifi_file_store.c" "${CJSON_SOURCE_DIR}/cJSON.c")
target_include_directories(wifi_http_test BEFORE PRIVATE "stubs/wifi" "${CJSON_SOURCE_DIR}")
target_compile_options(wifi_http_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(wifi_http_test PRIVATE ${DEAD_STRIP})
target_link_libraries(wifi_http_test PRIVATE lvgl_host m)
add_test(NAME wifi_http COMMAND wifi_http_test)
target_sources(osd_test PRIVATE "${FW_ROOT}/components/osd/system/wifi_files_ui.c")

target_compile_definitions(osd_test PRIVATE CONFIG_CHROMAGIC_WIFI_FILES=1)
add_executable(wifi_credentials_test wifi_credentials_test.c)
target_link_libraries(wifi_credentials_test PRIVATE lvgl_host m)
add_test(NAME wifi_credentials COMMAND wifi_credentials_test)
target_sources(wifi_http_test PRIVATE "${FW_ROOT}/main/wifi_credentials.c")
add_executable(wifi_network_test wifi_network_test.c)
target_include_directories(wifi_network_test BEFORE PRIVATE "stubs/wifi")
target_link_libraries(wifi_network_test PRIVATE lvgl_host m)
add_test(NAME wifi_router_lifecycle COMMAND wifi_network_test)
add_test(NAME wifi_agent_client COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/wifi_agent_test.py")
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(NAME wifi_browser_events COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/wifi_page_test.js")
endif()
add_test(NAME cart_backup_buffer_lifetime COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cart_backup_memory_test.py)

add_executable(pc_mode_wifi_test pc_mode_test.c "${FW_ROOT}/components/osd/system/pc_backup_mode.c")
target_compile_definitions(pc_mode_wifi_test PRIVATE CONFIG_CHROMAGIC_WIFI_FILES=1)
target_compile_options(pc_mode_wifi_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(pc_mode_wifi_test PRIVATE ${DEAD_STRIP})
target_link_libraries(pc_mode_wifi_test PRIVATE lvgl_host m)
add_test(NAME wifi_usb_mode_exclusion COMMAND pc_mode_wifi_test)

add_executable(native_state_test native_state_test.c "${FW_ROOT}/main/state_save_branch.c" "${FW_ROOT}/main/cart_rtc.c")
target_compile_options(native_state_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(native_state_test PRIVATE ${DEAD_STRIP})
target_link_libraries(native_state_test PRIVATE lvgl_host m)
add_test(NAME native_state_battery_protection COMMAND native_state_test)

# Invoked with real-game exports after the full-board gameplay simulation.
add_executable(native_state_game_save_test native_state_game_save_test.c "${FW_ROOT}/main/state_save_branch.c" "${FW_ROOT}/main/cart_rtc.c")
target_compile_options(native_state_game_save_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(native_state_game_save_test PRIVATE ${DEAD_STRIP})
target_link_libraries(native_state_game_save_test PRIVATE lvgl_host m)

find_package(Threads REQUIRED)
add_executable(native_state_autosave_test native_state_autosave_test.c "${FW_ROOT}/main/state_save_branch.c" "${FW_ROOT}/main/cart_rtc.c")
target_compile_definitions(native_state_autosave_test PRIVATE HOST_FREERTOS_PTHREAD_LOCKS)
target_compile_options(native_state_autosave_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(native_state_autosave_test PRIVATE ${DEAD_STRIP})
target_link_libraries(native_state_autosave_test PRIVATE lvgl_host m Threads::Threads)
add_test(NAME native_state_autosave_overlap COMMAND native_state_autosave_test)
set_tests_properties(native_state_autosave_overlap PROPERTIES TIMEOUT 15)

add_executable(native_shortcuts_test native_shortcuts_test.c "${FW_ROOT}/main/native_state_shortcuts.c")
target_include_directories(native_shortcuts_test PRIVATE "${FW_ROOT}/main")
add_test(NAME native_shortcut_gestures COMMAND native_shortcuts_test)
add_executable(native_control_test native_control_test.c "${FW_ROOT}/main/native_state_shortcuts.c")
target_link_libraries(native_control_test PRIVATE lvgl_host m)
add_test(NAME native_state_menu_controller COMMAND native_control_test)

add_executable(gg_load_test gg_load_test.c "${FW_ROOT}/main/cart_mapper.c" "${FW_ROOT}/main/cart_rtc.c" "${FW_ROOT}/main/cheat_patch.c")
target_compile_options(gg_load_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(gg_load_test PRIVATE ${DEAD_STRIP})
target_link_libraries(gg_load_test PRIVATE lvgl_host m)
add_test(NAME gamegear_load COMMAND gg_load_test)

add_executable(core_switch_test core_switch_test.c "${FW_ROOT}/main/core_switch.c" "${FW_ROOT}/main/last_game.c" "${FW_ROOT}/components/common/rom_title.c")
target_link_libraries(core_switch_test PRIVATE lvgl_host m)
add_test(NAME core_switch_handoff COMMAND core_switch_test)

add_executable(gg_cheats_test gg_cheats_test.c "${FW_ROOT}/main/gg_cheats.c")
target_link_libraries(gg_cheats_test PRIVATE lvgl_host m)
add_test(NAME gamegear_cheat_transport COMMAND gg_cheats_test)
add_executable(intro_video_test intro_video_test.c "${FW_ROOT}/main/intro_video.c" "${FW_ROOT}/main/intro_video_data.c")
target_include_directories(intro_video_test PRIVATE "${FW_ROOT}/main")
add_test(NAME boot_intro_video COMMAND intro_video_test)
