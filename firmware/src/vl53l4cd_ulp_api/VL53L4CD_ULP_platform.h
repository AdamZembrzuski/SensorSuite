#ifndef _VL53L4CD_ULP_PLATFORM_H_
#define _VL53L4CD_ULP_PLATFORM_H_
#pragma once

#include <stdint.h>
#include <string.h>
#include <zephyr/drivers/i2c.h>

#define VL53_NODE vl53l4cd


#define VL53_I2C_BUS_NODE DT_NODELABEL(i2c22)
#define VL53_I2C_ADDR 0x29


/**
 * @brief Read 32 bits through I2C.
 */

uint8_t VL53L4CD_ULP_RdDWord(uint16_t dev, uint16_t registerAddr, uint32_t *value);

/**
 * @brief Read 16 bits through I2C.
 */

uint8_t VL53L4CD_ULP_RdWord(uint16_t dev, uint16_t registerAddr, uint16_t *value);

/**
 * @brief Read 8 bits through I2C.
 */

uint8_t VL53L4CD_ULP_RdByte(uint16_t dev, uint16_t registerAddr, uint8_t *value);

/**
 * @brief Write 8 bits through I2C.
 */

uint8_t VL53L4CD_ULP_WrByte(uint16_t dev, uint16_t registerAddr, uint8_t value);

/**
 * @brief Write 16 bits through I2C.
 */

uint8_t VL53L4CD_ULP_WrWord(uint16_t dev, uint16_t RegisterAddress, uint16_t value);

/**
 * @brief Write 32 bits through I2C.
 */

uint8_t VL53L4CD_ULP_WrDWord(uint16_t dev, uint16_t RegisterAddress, uint32_t value);

/**
 * @brief Wait during N milliseconds.
 */

void VL53L4CD_ULP_WaitMs(uint32_t TimeMs);

#endif	// _VL53L4CD_ULP_PLATFORM_H_
