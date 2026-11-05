#ifndef I2C_H
#define I2C_H

#include "../types.h"

/*
 * S5L8900 I2C controllers: bus 0 at 0x3C600000, bus 1 at 0x3C900000.
 * Bus 1 carries the PCF50635 PMU at address 0x73.
 */

void i2c_init(void);
u32  i2c_read(int bus, u32 offset);
void i2c_write(int bus, u32 offset, u32 value);

void i2c_status_line(char *buf);

#endif /* I2C_H */
