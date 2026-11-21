// AXP2101 glue: XPowersLib drives the PMIC through these two I2C callbacks.
// The panel has no reset GPIO; its supply (ALDO3) is power-cycled instead.

#include "pmic.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "XPowersLib.h"

static const char *TAG = "pmic";

#define PMIC_ADDR      0x34
#define I2C_TIMEOUT_MS 100
#define I2C_TRIES      3

static XPowersPMU s_pmu;
static i2c_master_dev_handle_t s_dev;

static int reg_read(uint8_t, uint8_t reg, uint8_t *data, uint8_t len)
{
    for (int i = 0; i < I2C_TRIES; i++) {
        if (i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS) == ESP_OK)
            return 0;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}

static int reg_write(uint8_t, uint8_t reg, uint8_t *data, uint8_t len)
{
    uint8_t buf[16];
    if (len >= sizeof(buf)) return -1;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    for (int i = 0; i < I2C_TRIES; i++) {
        if (i2c_master_transmit(s_dev, buf, len + 1, I2C_TIMEOUT_MS) == ESP_OK)
            return 0;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}

extern "C" esp_err_t pmic_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = PMIC_ADDR;
    dev.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_dev), TAG, "add device");

    if (!s_pmu.begin(PMIC_ADDR, reg_read, reg_write)) {
        ESP_LOGE(TAG, "AXP2101 not responding");
        return ESP_FAIL;
    }
    s_pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA);

    // The MCU, panel and peripherals all run at 3.3 V. Factory OTP normally
    // sets this already; only correct a rail that came up elsewhere.
    if (s_pmu.getDC1Voltage() != 3300)   s_pmu.setDC1Voltage(3300);
    if (s_pmu.getALDO1Voltage() != 3300) s_pmu.setALDO1Voltage(3300);
    if (s_pmu.getALDO2Voltage() != 3300) s_pmu.setALDO2Voltage(3300);
    if (s_pmu.getALDO3Voltage() != 3300) s_pmu.setALDO3Voltage(3300);
    if (s_pmu.getALDO4Voltage() != 3300) s_pmu.setALDO4Voltage(3300);

    // Only matters if a LiPo is fitted to the battery connector.
    s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
    s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_50MA);
    ESP_LOGI(TAG, "AXP2101 up");
    return ESP_OK;
}

extern "C" void pmic_lcd_power_cycle(void)
{
    s_pmu.disableALDO3();
    vTaskDelay(pdMS_TO_TICKS(100));
    s_pmu.enableALDO3();
    vTaskDelay(pdMS_TO_TICKS(120));      // panel needs ~100 ms after power-up
}
