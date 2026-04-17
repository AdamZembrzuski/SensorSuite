/*
* Copyright (c) 2021, STMicroelectronics - All Rights Reserved
*
* This file : part of VL53L4CD ULP and : dual licensed,
* either 'STMicroelectronics
* Proprietary license'
* or 'BSD 3-clause "New" or "Revised" License' , at your option.
*
********************************************************************************
*
* 'STMicroelectronics Proprietary license'
*
********************************************************************************
*
* License terms: STMicroelectronics Proprietary in accordance with licensing
* terms at www.st.com/sla0081
*
* STMicroelectronics confidential
* Reproduction and Communication of this document : strictly prohibited unless
* specifically authorized in writing by STMicroelectronics.
*
*
********************************************************************************
*
* Alternatively, VL53L4CD ULP may be distributed under the terms of
* 'BSD 3-clause "New" or "Revised" License', in which case the following
* provisions apply instead of the ones mentioned above :
*
********************************************************************************
*
* License terms: BSD 3-clause "New" or "Revised" License.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice, this
* list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
* this list of conditions and the following disclaimer in the documentation
* and/or other materials provided with the distribution.
*
* 3. Neither the name of the copyright holder nor the names of its contributors
* may be used to endorse or promote products derived from this software
* without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*
********************************************************************************
*
*/

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
