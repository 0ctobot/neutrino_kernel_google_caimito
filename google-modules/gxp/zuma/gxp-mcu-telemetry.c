// SPDX-License-Identifier: GPL-2.0-only
/*
 * GXP MCU telemetry support
 *
 * Copyright (C) 2022 Google LLC
 */

#include <gcip/gcip-memory.h>
#include <gcip/gcip-telemetry.h>

#include "gxp-kci.h"
#include "gxp-mcu-telemetry.h"
#include "gxp-mcu.h"

int gxp_mcu_telemetry_init(struct gxp_mcu *mcu)
{
	struct gcip_telemetry *tel_log = &mcu->telemetry_log;
	struct gcip_telemetry *tel_trace = &mcu->telemetry_trace;
	int ret;

	ret = gxp_mcu_mem_alloc_data(mcu, &tel_log->memory, GXP_MCU_TELEMETRY_LOG_BUFFER_SIZE);
	if (ret)
		return ret;

	ret = gcip_telemetry_init(tel_log, GCIP_TELEMETRY_TYPE_LOG, mcu->gxp->dev);
	if (ret)
		goto free_log_mem;

	ret = gxp_mcu_mem_alloc_data(mcu, &tel_trace->memory, GXP_MCU_TELEMETRY_TRACE_BUFFER_SIZE);
	if (ret)
		goto uninit_log;

	ret = gcip_telemetry_init(tel_trace, GCIP_TELEMETRY_TYPE_TRACE, mcu->gxp->dev);
	if (ret)
		goto free_trace_mem;

	return 0;

free_trace_mem:
	gxp_mcu_mem_free_data(mcu, &tel_trace->memory);

uninit_log:
	gcip_telemetry_exit(&mcu->telemetry_log);

free_log_mem:
	gxp_mcu_mem_free_data(mcu, &tel_log->memory);

	return ret;
}

void gxp_mcu_telemetry_exit(struct gxp_mcu *mcu)
{
	gcip_telemetry_exit(&mcu->telemetry_trace);
	gxp_mcu_mem_free_data(mcu, &mcu->telemetry_trace.memory);
	gcip_telemetry_exit(&mcu->telemetry_log);
	gxp_mcu_mem_free_data(mcu, &mcu->telemetry_log.memory);
}

void gxp_mcu_telemetry_irq_handler(struct gxp_mcu *mcu)
{
	gcip_telemetry_irq_handler(&mcu->telemetry_log);
	gcip_telemetry_irq_handler(&mcu->telemetry_trace);
}

int gxp_mcu_telemetry_kci(struct gxp_mcu *mcu)
{
	int ret;

	ret = gcip_telemetry_kci(&mcu->telemetry_log, gxp_kci_map_mcu_log_buffer,
				 mcu->kci.mbx->mbx_impl.gcip_kci);
	if (ret)
		return ret;

	ret = gcip_telemetry_kci(&mcu->telemetry_trace, gxp_kci_map_mcu_trace_buffer,
				 mcu->kci.mbx->mbx_impl.gcip_kci);
	if (ret)
		return ret;

	return ret;
}
