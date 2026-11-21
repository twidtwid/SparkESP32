# The board contract (board.h, display.h): header-only. Each board component under
# firmware/boards/<id>/board implements it and provides include/board_config.h.
idf_component_register(INCLUDE_DIRS "include" REQUIRES esp_common)
