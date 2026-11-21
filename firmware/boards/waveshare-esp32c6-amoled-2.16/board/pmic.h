#pragma once
// AXP2101 power management (private to the board component).
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pmic_init(i2c_master_bus_handle_t bus);   // rails to 3.3 V, VBUS limit, charger
void      pmic_lcd_power_cycle(void);               // panel reset: ALDO3 off -> on

#ifdef __cplusplus
}
#endif
