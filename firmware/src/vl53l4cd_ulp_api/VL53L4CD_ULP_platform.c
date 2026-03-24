#include "VL53L4CD_ULP_platform.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>

static const struct i2c_dt_spec vl53_i2c = {
    .bus = DEVICE_DT_GET(VL53_I2C_BUS_NODE),
    .addr = VL53_I2C_ADDR,
};

uint8_t VL53L4CD_ULP_RdDWord(uint16_t dev, uint16_t RegisterAddress, uint32_t *value)
{
	ARG_UNUSED(dev);

	uint8_t addr_buf[2];
    uint8_t data_buf[4];

    sys_put_be16(RegisterAddress, addr_buf);


    int ret = i2c_write_read_dt(&vl53_i2c, addr_buf, 2, data_buf, 4);

    if (ret == 0) {
        *value = sys_get_be32(data_buf);
    }
	else {
		*value = 0;
	}

    return (ret == 0) ? 0 : 1;
}

uint8_t VL53L4CD_ULP_RdWord(uint16_t dev, uint16_t RegisterAddress, uint16_t *value)
{

	ARG_UNUSED(dev);

	uint8_t addr_buf[2];
	uint8_t data_buf[2];

	sys_put_be16(RegisterAddress, addr_buf);

    int ret = i2c_write_read_dt(&vl53_i2c, addr_buf, 2, data_buf, 2);

    if (ret == 0) {
        *value = sys_get_be16(data_buf);
    }
	else {
		*value = 0;
	}

    return (ret == 0) ? 0 : 1;
}

uint8_t VL53L4CD_ULP_RdByte(uint16_t dev, uint16_t RegisterAddress, uint8_t *value)
{
	ARG_UNUSED(dev);

	
	uint8_t addr_buf[2];

    sys_put_be16(RegisterAddress, addr_buf);

    int ret = i2c_write_read_dt(&vl53_i2c, addr_buf, 2, value, 1);
	if (ret != 0) {
		*value = 0;
	}

	return (ret == 0) ? 0 : 1;
}

uint8_t VL53L4CD_ULP_WrByte(uint16_t dev, uint16_t RegisterAddress, uint8_t value)
{

	ARG_UNUSED(dev);
	
	uint8_t tx_buf[3];

    sys_put_be16(RegisterAddress, &tx_buf[0]);
	tx_buf[2] = value;

    int ret = i2c_write_dt(&vl53_i2c, tx_buf, 3);
	return (ret == 0) ? 0 : 1;
}


uint8_t VL53L4CD_ULP_WrWord(uint16_t dev, uint16_t RegisterAddress, uint16_t value)
{
	ARG_UNUSED(dev);
	
	uint8_t tx_buf[4];

    sys_put_be16(RegisterAddress, &tx_buf[0]); 
	sys_put_be16(value, &tx_buf[2]);    

    int ret = i2c_write_dt(&vl53_i2c, tx_buf, 4);
	return (ret == 0) ? 0 : 1;
}

uint8_t VL53L4CD_ULP_WrDWord(uint16_t dev, uint16_t RegisterAddress, uint32_t value)
{
	ARG_UNUSED(dev);

	uint8_t tx_buf[6];

    sys_put_be16(RegisterAddress, &tx_buf[0]);
	sys_put_be32(value, &tx_buf[2]);           

    int ret = i2c_write_dt(&vl53_i2c, tx_buf, 6);
	return (ret == 0) ? 0 : 1;
}


void VL53L4CD_ULP_WaitMs(uint32_t TimeMs)
{
    k_msleep(TimeMs);
}
