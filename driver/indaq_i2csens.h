/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_i2csens.h - AP3216C I2C sensor driver header
 */

#ifndef __INDAQ_I2CSENS_H__
#define __INDAQ_I2CSENS_H__

#include <linux/types.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/workqueue.h>

/* AP3216C register map */
#define AP3216C_REG_SYS_CONFIG  0x00
#define AP3216C_REG_STATUS      0x01
#define AP3216C_REG_IR_DATA_LO  0x0A
#define AP3216C_REG_IR_DATA_HI  0x0B
#define AP3216C_REG_ALS_DATA_LO 0x0C
#define AP3216C_REG_ALS_DATA_HI 0x0D
#define AP3216C_REG_PS_DATA_LO  0x0E
#define AP3216C_REG_PS_DATA_HI  0x0F

/* IR data: 10-bit, IR = (IR_HI << 2) | (IR_LO & 0x03)
 * IR Data Low (0x0A): bit 7 = IR_OF (1 = data invalid)
 *                     bits 1:0 = IR[1:0]
 * IR Data High (0x0B): bits 7:0 = IR[9:2]
 */
#define AP3216C_IR_LO_MASK      0x03
#define AP3216C_IR_HI_SHIFT     2
#define AP3216C_IR_OF           BIT(7)

/* PS data: 10-bit, PS = ((PS_HI & 0x3F) << 4) | (PS_LO & 0x0F)
 * PS Data Low (0x0E): bit 6 = PS_IR_OF (1 = PS data invalid),
 *                      bit 7 = OBJ (1 = object near)
 */
#define AP3216C_PS_LO_MASK      0x0F
#define AP3216C_PS_HI_MASK      0x3F
#define AP3216C_PS_HI_SHIFT     4
#define AP3216C_PS_IR_OF        BIT(6)

/* System config register values */
#define AP3216C_SYS_RESET       0x04
#define AP3216C_SYS_ENABLE      0x03   /* Enable ALS + PS */

/* STATUS register (0x01) bits */
#define AP3216C_STATUS_ALS_READY    BIT(6)  /* ALS data ready */
#define AP3216C_STATUS_PS_READY     BIT(2)  /* PS/IR data ready */

/* AP3216C I2C address */
#define AP3216C_I2C_ADDR       0x1e

/* Default read interval in milliseconds */
#define AP3216C_DEFAULT_INTERVAL_MS  250

struct indaq_i2csens {
	struct i2c_client *client;
	struct regmap *regmap;
	struct delayed_work read_work;
	u32 read_interval_ms;

	/* Latest sensor data */
	u16 als_value;
	u16 ps_value;
	u16 ir_value;
};

int indaq_i2csens_init(struct indaq_device *indev);
void indaq_i2csens_exit(struct indaq_device *indev);

/* Phase 3: get latest ALS, PS, IR sensor readings */
int indaq_i2csens_get_data(u16 *als, u16 *ps, u16 *ir);

/* Phase 5: dynamically change sampling interval (in ms) */
int indaq_i2csens_set_interval(u32 interval_ms);

#endif /* __INDAQ_I2CSENS_H__ */
