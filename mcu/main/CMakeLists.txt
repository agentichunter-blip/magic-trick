cmake_minimum_required(VERSION 3.22)

if(CONFIG_CHROMAGIC_WIFI_FILES)
    set(WIFI_SOURCES wifi_files.c wifi_file_store.c wifi_network.c wifi_credentials.c)
    set(WIFI_ASSETS wifi_files.html)
endif()

idf_component_register(
    SRCS ${WIFI_SOURCES}
        "main.c" "gfx.c" "fpga_tx.c" "fpga_rx.c" "fpga_common.c" "pwrmgr.c"
        "sd_card.c" "sd_publish.c" "cart_link.c" "cart_bulk.c" "cart_mapper.c" "cart_rtc.c"
        "native_state_control.c" "native_state_shortcuts.c"
        "cart_backup.c" "virtual_cart.c" "state_save_branch.c" "state_file.c" "rom_browser.c" "pc_backup.c" "pc_sd.c"
        "rom_browser_worker.c" "box_art.c" "video_preview.c" "last_game.c" "recent_games.c" "boot_resume.c" "intro_video.c" "intro_video_data.c" "core_switch.c" "cheats.c" "gg_cheats.c" "cheat_patch.c"
    EMBED_TXTFILES ${WIFI_ASSETS}
    INCLUDE_DIRS "."
    REQUIRES
        images esp_driver_uart esp_driver_spi esp_driver_gpio esp_driver_sdmmc
        nvs_flash fatfs sdmmc
        common battery button osd menu_mgr settings mutex
        console crc mbedtls json esp_wifi esp_netif esp_event esp_http_server
)
