#ifndef PMU_H
#define PMU_H

#include "../types.h"

/* NXP PCF50635 PMU on I2C bus 1, including its BCD real-time clock. */

#define PMU_I2C_ADDR    0x73

void pmu_i2c_start(void);     /* START or repeated START to 0x73 */
void pmu_i2c_send(u8 byte);
u8   pmu_i2c_recv(void);

/* Hold/Home on the PMU's EXTON1 input (wake source); diagnostics. */
void pmu_button_event(bool pressed);
u32  pmu_int2_state(void);
extern u32 pmu_irq_raises, pmu_int_reads;

#endif /* PMU_H */
