/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __BCL_H
#define __BCL_H

#if IS_ENABLED(CONFIG_GOOGLE_BCL)
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>
#include <dt-bindings/power/s2mpg1x-power.h>
#include <dt-bindings/soc/google/zuma-bcl.h>

/* consistency checks in google_bcl_register_callback() */
#define bcl_cb_uvlo_read(bcl, m, v) (((bcl)->pmic_ops && (bcl)->intf_pmic_i2c) ? \
	(bcl)->pmic_ops->cb_uvlo_read((bcl)->intf_pmic_i2c, m, v) : -ENODEV)
#define bcl_cb_uvlo_write(bcl, m, v) (((bcl)->pmic_ops && (bcl)->intf_pmic_i2c) ? \
	(bcl)->pmic_ops->cb_uvlo_write((bcl)->intf_pmic_i2c, m, v) : -ENODEV)
#define bcl_cb_batoilo_read(bcl, v) (((bcl)->pmic_ops && (bcl)->intf_pmic_i2c) ? \
	(bcl)->pmic_ops->cb_batoilo_read((bcl)->intf_pmic_i2c, v) : -ENODEV)
#define bcl_cb_batoilo_write(bcl, v) (((bcl)->pmic_ops && (bcl)->intf_pmic_i2c) ? \
	(bcl)->pmic_ops->cb_batoilo_write((bcl)->intf_pmic_i2c, v) : -ENODEV)
#define bcl_cb_vdroop_ok(bcl, v) (((bcl)->pmic_ops && (bcl)->intf_pmic_i2c) ? \
	(bcl)->pmic_ops->cb_get_vdroop_ok((bcl)->intf_pmic_i2c, v) : -ENODEV)

/* helpers for UVLO1 and UVLO2 */
#define bcl_cb_uvlo1_read(bcl, v)	bcl_cb_uvlo_read(bcl, TS_UVLO1, v)
#define bcl_cb_uvlo1_write(bcl, v)	bcl_cb_uvlo_write(bcl, TS_UVLO1, v)
#define bcl_cb_uvlo2_read(bcl, v)	bcl_cb_uvlo_read(bcl, TS_UVLO2, v)
#define bcl_cb_uvlo2_write(bcl, v)	bcl_cb_uvlo_write(bcl, TS_UVLO2, v)

/* This driver determines if HW was throttled due to SMPL/OCP */

#define CPUCL0_BASE (0x29c00000)
#define CPUCL1_BASE (0x29d00000)
#define CPUCL2_BASE (0x29d80000)
#define G3D_BASE (0x1EE00000)
#define TPU_BASE (0x1A300000)
#define SYSREG_CPUCL0_BASE (0x29c20000)
#define CLUSTER0_GENERAL_CTRL_64 (0x1404)
#define CLKDIVSTEP (0x830)
#define VDROOP_FLT (0x838)
#define CPUCL0_CLKDIVSTEP_STAT (0x83c)
#define CPUCL0_CLKDIVSTEP_CON (0x838)
#define CPUCL12_CLKDIVSTEP_STAT (0x848)
#define CPUCL12_CLKDIVSTEP_CON_HEAVY (0x840)
#define CPUCL12_CLKDIVSTEP_CON_LIGHT (0x844)
#define CLKOUT (0x810)
#define G3D_CLKDIVSTEP_STAT (0x854)
#define TPU_CLKDIVSTEP_STAT (0x850)
#define CLUSTER0_MPMM (0x1408)
#define CLUSTER0_PPM (0x140c)
#define MPMMEN_MASK (0xF << 21)
#define PPMEN_MASK (0x3 << 8)
#define PPMCTL_MASK (0xFF)
#define OCP_WARN_MASK (0x1F)
#define SMPL_WARN_MASK (0xE0)
#define ACTIVE_HIGH (0x1)
#define ACTIVE_LOW (0x0)
#define PMU_ALIVE_CPU0_OUT (0x1CA0)
#define PMU_ALIVE_CPU1_OUT (0x1D20)
#define PMU_ALIVE_CPU2_OUT (0x1DA0)
#define PMU_ALIVE_TPU_OUT (0x2920)
#define PMU_ALIVE_GPU_OUT (0x1E20)
#define PMU_CLK_OUT (0x3E80)
#define THRESHOLD_DELAY_MS 50
#define PWRWARN_DELAY_MS 50

enum SUBSYSTEM_SOURCE {
	CPU0,
	CPU1,
	CPU2,
	TPU,
	GPU,
	SUBSYSTEM_SOURCE_MAX,
};

extern const unsigned int subsystem_pmu[];
extern const unsigned int clk_stats_offset[];
extern const char * const clk_stats_source[];

enum BCL_THERMAL_SOURCE {
	TS_UVLO2,
	TS_UVLO1,
};

enum PMIC_REG {
	S2MPG14,
	S2MPG15
};

#define MAIN 			S2MPG14
#define SUB 			S2MPG15

struct ocpsmpl_stats {
	ktime_t _time;
	int capacity;
	int voltage;
};

enum RATIO_SOURCE {
	CPU0_CON,
	CPU1_HEAVY,
	CPU2_HEAVY,
	TPU_HEAVY,
	GPU_HEAVY,
	CPU1_LIGHT,
	CPU2_LIGHT,
	TPU_LIGHT,
	GPU_LIGHT
};

typedef int (*pmic_set_uvlo_lvl_fn)(struct i2c_client *client, uint8_t mode, unsigned int lvl);
typedef int (*pmic_get_uvlo_lvl_fn)(struct i2c_client *client, uint8_t mode, unsigned int *lvl);
typedef int (*pmic_set_batoilo_lvl_fn)(struct i2c_client *client, unsigned int lvl);
typedef int (*pmic_get_batoilo_lvl_fn)(struct i2c_client *client, unsigned int *lvl);
typedef int (*pmic_get_vdroop_ok_fn)(struct i2c_client *client, bool *state);

struct bcl_ifpmic_ops {
	pmic_get_vdroop_ok_fn	cb_get_vdroop_ok;
	pmic_set_uvlo_lvl_fn	cb_uvlo_write;
	pmic_get_uvlo_lvl_fn	cb_uvlo_read;
	pmic_set_batoilo_lvl_fn	cb_batoilo_write;
	pmic_get_batoilo_lvl_fn cb_batoilo_read;
};

struct bcl_device {
	struct device *device;
	struct device *main_dev;
	struct device *sub_dev;
	struct device *mitigation_dev;
	struct odpm_info *main_odpm;
	struct odpm_info *sub_odpm;
	void __iomem *base_mem[5];
	void __iomem *sysreg_cpucl0;
	struct power_supply *batt_psy;
	const struct bcl_ifpmic_ops *pmic_ops;

	struct notifier_block psy_nb;
	struct delayed_work init_work;
	struct delayed_work bcl_intf_work[TRIGGERED_SOURCE_MAX];
	unsigned int bcl_lvl[TRIGGERED_SOURCE_MAX];
	atomic_t bcl_cnt[TRIGGERED_SOURCE_MAX];
	int bcl_prev_lvl[TRIGGERED_SOURCE_MAX];

	int trip_high_temp;
	int trip_low_temp;
	int trip_val;
	struct mutex state_trans_lock;
	struct thermal_zone_of_device_ops bcl_ops[TRIGGERED_SOURCE_MAX];
	struct mutex bcl_irq_lock[TRIGGERED_SOURCE_MAX];
	struct delayed_work bcl_irq_work[TRIGGERED_SOURCE_MAX];
	struct thermal_zone_device *bcl_tz[TRIGGERED_SOURCE_MAX];

	unsigned int bcl_irq[TRIGGERED_SOURCE_MAX];
	int bcl_tz_cnt[TRIGGERED_SOURCE_MAX];
	int bcl_pin[TRIGGERED_SOURCE_MAX];
	struct ocpsmpl_stats bcl_stats[TRIGGERED_SOURCE_MAX];

	struct i2c_client *main_pmic_i2c;
	struct i2c_client *sub_pmic_i2c;
	struct i2c_client *intf_pmic_i2c;

	struct mutex ratio_lock;
	unsigned int tpu_con_heavy;
	unsigned int tpu_con_light;
	unsigned int gpu_con_heavy;
	unsigned int gpu_con_light;
	unsigned int tpu_clkdivstep;
	unsigned int gpu_clkdivstep;
	unsigned int cpu2_clkdivstep;
	unsigned int cpu1_clkdivstep;
	unsigned int cpu0_clkdivstep;
	unsigned int gpu_clk_stats;
	unsigned int tpu_clk_stats;
	unsigned int tpu_vdroop_flt;
	unsigned int gpu_vdroop_flt;
	unsigned int odpm_ratio;

	bool batt_psy_initialized;
	bool enabled;
	bool ready;

	unsigned int main_offsrc1;
	unsigned int main_offsrc2;
	unsigned int sub_offsrc1;
	unsigned int sub_offsrc2;
	unsigned int pwronsrc;

	unsigned int vdroop1_pin;
	unsigned int vdroop2_pin;
	unsigned int modem_gpio1_pin;
	unsigned int modem_gpio2_pin;

	/* debug */
	struct dentry *debug_entry;
	unsigned int gpu_clk_out;
	unsigned int tpu_clk_out;

	int main_irq_base, sub_irq_base;
	u64 main_limit[METER_CHANNEL_MAX];
	u64 sub_limit[METER_CHANNEL_MAX];
	int main_pwr_warn_irq[METER_CHANNEL_MAX];
	int sub_pwr_warn_irq[METER_CHANNEL_MAX];
	bool main_pwr_warn_triggered[METER_CHANNEL_MAX];
	bool sub_pwr_warn_triggered[METER_CHANNEL_MAX];
	struct delayed_work main_pwr_irq_work;
	struct delayed_work sub_pwr_irq_work;
};

extern void google_bcl_irq_update_lvl(struct bcl_device *bcl_dev, int index, unsigned int lvl);
extern void google_bcl_irq_changed(struct bcl_device *bcl_dev, int index);
extern int google_set_mpmm(struct bcl_device *data, unsigned int value);
extern int google_set_ppm(struct bcl_device *data, unsigned int value);
extern unsigned int google_get_mpmm(struct bcl_device *data);
extern unsigned int google_get_ppm(struct bcl_device *data);
extern struct bcl_device *google_retrieve_bcl_handle(void);
extern int google_bcl_register_ifpmic(struct bcl_device *bcl_dev,
				      const struct bcl_ifpmic_ops *pmic_ops);
extern int google_init_gpu_ratio(struct bcl_device *data);
extern int google_init_tpu_ratio(struct bcl_device *data);
bool bcl_is_subsystem_on(unsigned int addr);
void bcl_disable_power(void);
void bcl_enable_power(void);
void __iomem *get_addr_by_subsystem(void *dev, const char *subsystem);
int pmic_write(int pmic, struct bcl_device *bcl_dev, u8 reg, u8 value);
int pmic_read(int pmic, struct bcl_device *bcl_dev, u8 reg, u8 *value);
#else
struct bcl_device;

static inline int pmic_write(int pmic, struct bcl_device *bcl_dev, u8 reg, u8 value)
{
	return 0;
}

int pmic_read(int pmic, struct bcl_device *bcl_dev, u8 reg, u8 *value)
{
	return 0;
}

static inline bool bcl_is_subsystem_on(unsigned int addr)
{
	return true;
}
static inline void bcl_disable_power(void)
{
}
static inline void bcl_enable_power(void)
{
}
static inline void __iomem *get_addr_by_subsystem(void *dev, const char *subsystem)
{
	return NULL;
}
static inline void google_bcl_irq_update_lvl(struct bcl_device *bcl_dev, int index,
					     unsigned int lvl)
{
}
static inline unsigned int google_get_mpmm(struct bcl_device *data)
{
	return 0;
}
static inline unsigned int google_get_ppm(struct bcl_device *data)
{
	return 0;
}
static inline int google_set_ppm(struct bcl_device *data, unsigned int value)
{
	return 0;
}
static inline int google_set_mpmm(struct bcl_device *data, unsigned int value)
{
	return 0;
}
static inline struct bcl_device *google_retrieve_bcl_handle(void)
{
	return NULL;
}
static void google_bcl_irq_changed(struct bcl_device *bcl_dev, int index)
{
}
static int google_bcl_register_ifpmic(struct bcl_device *bcl_dev,
				      const struct bcl_ifpmic_ops *pmic_ops)
{
	return 0;
}
static inline int google_init_gpu_ratio(struct bcl_device *data)
{
	return 0;
}
static inline int google_init_tpu_ratio(struct bcl_device *data)
{
	return 0;
}
#endif /* IS_ENABLED(CONFIG_GOOGLE_BCL) */

#endif /* __BCL_H */
