lwis-objs := lwis_device.o
lwis-objs += lwis_device_i2c.o
lwis-objs += lwis_device_ioreg.o
lwis-objs += lwis_device_top.o
lwis-objs += lwis_clock.o
lwis-objs += lwis_gpio.o
lwis-objs += lwis_i2c.o
lwis-objs += lwis_interrupt.o
lwis-objs += lwis_ioctl.o
lwis-objs += lwis_ioreg.o
lwis-objs += lwis_phy.o
lwis-objs += lwis_pinctrl.o
lwis-objs += lwis_regulator.o
lwis-objs += lwis_event.o
lwis-objs += lwis_buffer.o
lwis-objs += lwis_util.o

# Exynos specific files
ifeq ($(CONFIG_SOC_EXYNOS9810), y)
lwis-objs += platform/exynos/lwis_platform_exynos.o
lwis-objs += platform/exynos/lwis_platform_exynos_dma.o
endif

# Device tree specific file
ifeq ($(CONFIG_OF), y)
lwis-objs += lwis_dt.o
endif

obj-$(CONFIG_LWIS) += lwis.o

ccflags-y = -I$(abspath $(KBUILD_SRC)/$(KBUILD_EXTMOD))
