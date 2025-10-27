// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCIP telemetry: logging and tracing.
 *
 * Copyright (C) 2022 Google LLC
 */

#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/eventfd.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <gcip/gcip-memory.h>
#include <gcip/gcip-telemetry.h>

int gcip_telemetry_kci(struct gcip_telemetry *tel,
		       int (*send_kci)(const struct gcip_telemetry_kci_args *),
		       struct gcip_kci *kci)
{
	struct gcip_memory *mem = &tel->memory;
	const struct gcip_telemetry_kci_args args = {
		.kci = kci,
		.addr = mem->dma_addr,
		.size = mem->size,
	};
	int err;

	dev_dbg(tel->dev, "Sending KCI %s", tel->name);
	err = send_kci(&args);

	if (err < 0) {
		dev_err(tel->dev, "KCI %s failed - %d", tel->name, err);
		return err;
	}

	if (err > 0) {
		dev_err(tel->dev, "KCI %s returned %d", tel->name, err);
		return -EBADMSG;
	}

	dev_dbg(tel->dev, "KCI %s Succeeded", tel->name);

	return 0;
}

int gcip_telemetry_set_event(struct gcip_telemetry *tel, u32 eventfd)
{
	struct eventfd_ctx *ctx, *prev_ctx;

	ctx = eventfd_ctx_fdget(eventfd);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	mutex_lock(&tel->state_ctx_lock);
	prev_ctx = tel->ctx;
	tel->ctx = ctx;
	mutex_unlock(&tel->state_ctx_lock);

	if (prev_ctx)
		eventfd_ctx_put(prev_ctx);

	return 0;
}

void gcip_telemetry_unset_event(struct gcip_telemetry *tel)
{
	struct eventfd_ctx *prev_ctx;

	mutex_lock(&tel->state_ctx_lock);
	prev_ctx = tel->ctx;
	tel->ctx = NULL;
	mutex_unlock(&tel->state_ctx_lock);

	if (prev_ctx)
		eventfd_ctx_put(prev_ctx);
}

/**
 * copy_with_wrap() - The helper function to copy data out of the log buffer with wrapping.
 * @header: The telemetry header to read and write the head value.
 * @dest: The buffer to copy the data to.
 * @length: The length of the data to be copied.
 * @size: The size of telemetry buffer.
 * @start: The start address of the telemetry buffer.
 */
static void copy_with_wrap(struct gcip_telemetry_header *header, void *dest, u32 length, u32 size,
			   void *start)
{
	const u32 wrap_bit = size + sizeof(*header);
	u32 remaining = 0;
	u32 head = header->head & (wrap_bit - 1);

	if (head + length < size) {
		memcpy(dest, start + head, length);
		header->head += length;
	} else {
		remaining = size - head;
		memcpy(dest, start + head, remaining);
		memcpy(dest + remaining, start, length - remaining);
		header->head = (header->head & wrap_bit) ^ wrap_bit;
		header->head |= length - remaining;
	}
}

/**
 * gcip_telemetry_fw_log() - The fallback function to consume the log buffer.
 * @log: The log telemetry object.
 *
 * This function will consume the log buffer and print it to dmesg from the host CPU. The logging
 * level depends on the code in the header entry.
 */
static void gcip_telemetry_fw_log(const struct gcip_telemetry *log)
{
	struct device *dev = log->dev;
	struct gcip_telemetry_header *header = log->header;
	struct gcip_log_entry_header entry;
	u8 *start;
	const size_t queue_size = header->size - sizeof(*header);
	const size_t max_length = queue_size - sizeof(entry);
	char *buffer = kvmalloc(max_length + 1, GFP_KERNEL);

	if (!buffer) {
		header->head = header->tail;
		return;
	}
	start = (u8 *)header + sizeof(*header);

	while (header->head != header->tail) {
		copy_with_wrap(header, &entry, sizeof(entry), queue_size, start);
		if (entry.length == 0 || entry.length > max_length) {
			header->head = header->tail;
			dev_err(dev, "log queue is corrupted");
			break;
		}
		copy_with_wrap(header, buffer, entry.length, queue_size, start);
		buffer[entry.length] = 0;

		if (entry.code > GCIP_FW_DMESG_LOG_LEVEL)
			continue;

		switch (entry.code) {
		case GCIP_FW_LOG_LEVEL_VERBOSE:
		case GCIP_FW_LOG_LEVEL_DEBUG:
			dev_dbg(dev, "%s", buffer);
			break;
		case GCIP_FW_LOG_LEVEL_WARN:
			dev_warn(dev, "%s", buffer);
			break;
		case GCIP_FW_LOG_LEVEL_FATAL:
		case GCIP_FW_LOG_LEVEL_ERROR:
			dev_err(dev, "%s", buffer);
			break;
		case GCIP_FW_LOG_LEVEL_INFO:
		default:
			dev_info(dev, "%s", buffer);
			break;
		}
	}
	kvfree(buffer);
}

/**
 * gcip_telemetry_fw_trace() - The fallback function to consume the trace buffer.
 * @trace: The trace telemetry object.
 *
 * This function will do nothing but update the value of the head in the header.
 */
static void gcip_telemetry_fw_trace(const struct gcip_telemetry *trace)
{
	struct gcip_telemetry_header *header = trace->header;

	header->head = header->tail;
}

void gcip_telemetry_irq_handler(struct gcip_telemetry *tel)
{
	/*
	 * Safe to access tel->state without state_ctx_lock because it would just schedule a
	 * redundant worker which is fine.
	 */
	if (tel->state != GCIP_TELEMETRY_ENABLED)
		return;

	/* Early return if we know there is no pending data. */
	if (tel->header && (tel->header->head == tel->header->tail))
		return;

	schedule_work(&tel->work);
}

/**
 * gcip_telemetry_inc_mmap_count() - Increases the telemetry mmap count.
 * @tel: The telemetry to add the mmapped_count.
 * @dif: The number to add the mmapped_count.
 */
static void gcip_telemetry_inc_mmap_count(struct gcip_telemetry *tel, int dif)
{
	mutex_lock(&tel->mmap_lock);
	tel->mmapped_count += dif;
	mutex_unlock(&tel->mmap_lock);
}

/**
 * gcip_telemetry_vma_ops_open() - The callback function to trigger when VMA is being mapped.
 * @vma: The VM area to be opened.
 *
 * Increses the mmap count of the retrieved telemetry.
 */
static void gcip_telemetry_vma_ops_open(struct vm_area_struct *vma)
{
	struct gcip_telemetry *tel = vma->vm_private_data;

	gcip_telemetry_inc_mmap_count(tel, 1);
}

/**
 * gcip_telemetry_vma_ops_close() - The callback function to trigger when VMA is being unmapped.
 * @vma: The VM area to be closed.
 *
 * Decreses the mmap count of the retrieved telemetry.
 */
static void gcip_telemetry_vma_ops_close(struct vm_area_struct *vma)
{
	struct gcip_telemetry *tel = vma->vm_private_data;

	gcip_telemetry_inc_mmap_count(tel, -1);
}

static const struct vm_operations_struct gcip_telemetry_vma_ops = {
	.open = gcip_telemetry_vma_ops_open,
	.close = gcip_telemetry_vma_ops_close,
};

int gcip_telemetry_mmap(struct gcip_telemetry *tel, struct vm_area_struct *vma)
{
	struct gcip_memory *mem = &tel->memory;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long orig_pgoff = vma->vm_pgoff;
	int ret;

	size = min(size, mem->size);
	if (!size) {
		dev_err(tel->dev, "The size of the telemetry buffer to be mapped cannot be 0");
		return -EINVAL;
	}

	dev_dbg(tel->dev, "%s: virt = %pK phys = %pap\n", __func__, mem->virt_addr,
		&mem->phys_addr);

	mutex_lock(&tel->mmap_lock);

	if (tel->mmapped_count) {
		ret = -EBUSY;
		dev_warn(tel->dev, "%s is already mmapped %ld times", tel->name,
			 tel->mmapped_count);
		goto err_unlock;
	}

	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vma->vm_pgoff = 0;
	ret = remap_pfn_range(vma, vma->vm_start, mem->phys_addr >> PAGE_SHIFT, size,
			      vma->vm_page_prot);
	vma->vm_pgoff = orig_pgoff;
	if (ret)
		goto err_unlock;

	vma->vm_ops = &gcip_telemetry_vma_ops;
	vma->vm_private_data = tel;
	tel->mmapped_count = 1;
	mem->host_addr = vma->vm_start;

	mutex_unlock(&tel->mmap_lock);

	return 0;

err_unlock:
	mutex_unlock(&tel->mmap_lock);
	return ret;
}

/**
 * gcip_telemetry_worker() - The worker for processing the log/trace buffers.
 * @work: The work_struct of the telemetry.
 */
static void gcip_telemetry_worker(struct work_struct *work)
{
	struct gcip_telemetry *tel = container_of(work, struct gcip_telemetry, work);
	struct gcip_telemetry_header *header = tel->header;
	u32 prev_head;

	/*
	 * Loops while following conditions are all true:
	 * 1. The telemetry is enabled.
	 * 2. The header is visible(not NULL).
	 * 3. There is data to be consumed, and the previous iteration made progress.
	 */
	do {
		mutex_lock(&tel->state_ctx_lock);
		if (tel->state != GCIP_TELEMETRY_ENABLED) {
			mutex_unlock(&tel->state_ctx_lock);
			return;
		}

		if (header)
			prev_head = header->head;

		/*
		 * The runtime side handler and the fallback function should consider the case that
		 * head != tail.
		 */
		if (tel->ctx)
			eventfd_signal(tel->ctx, 1);
		else if (tel->fallback_fn)
			tel->fallback_fn(tel);
		else
			dev_warn(tel->dev, "Failed to consume the telemetry buffer");

		mutex_unlock(&tel->state_ctx_lock);
		msleep(GCIP_TELEMETRY_TYPE_LOG_RECHECK_DELAY);
	} while (header && (header->head != header->tail) && (header->head != prev_head));

	/* If another IRQ arrives after the header check, we should schedule another worker. */
}

int gcip_telemetry_init(struct gcip_telemetry *tel, enum gcip_telemetry_type type,
			struct device *dev)
{
	struct gcip_memory *mem = &tel->memory;
	const char *name;
	void (*fallback_fn)(const struct gcip_telemetry *tel);
	struct gcip_telemetry_header *header;

	/* The memory have to be set before telemetry init. */
	if (!mem->virt_addr || !mem->size) {
		dev_err(dev, "The telemetry memory should be set before initializing");
		return -EINVAL;
	}

	if (!is_power_of_2(mem->size) || mem->size <= sizeof(struct gcip_telemetry_header)) {
		dev_err(dev,
			"Size of GCIP telemetry buffer must be a power of 2 and greater than %zu.",
			sizeof(struct gcip_telemetry_header));
		return -EINVAL;
	}

	switch (type) {
	case GCIP_TELEMETRY_TYPE_LOG:
		name = GCIP_TELEMETRY_NAME_LOG;
		fallback_fn = gcip_telemetry_fw_log;
		header = mem->virt_addr;
		break;
	case GCIP_TELEMETRY_TYPE_TRACE:
		name = GCIP_TELEMETRY_NAME_TRACE;
		fallback_fn = gcip_telemetry_fw_trace;
		header = mem->virt_addr;
		break;
	case GCIP_TELEMETRY_TYPE_OPAQUE:
		name = GCIP_TELEMETRY_NAME_OPAQUE;
		fallback_fn = NULL;
		header = NULL;
		break;
	default:
		dev_err(dev, "Unrecognized GCIP telemetry type: %d", type);
		return -EINVAL;
	}

	tel->name = name;
	tel->dev = dev;
	tel->type = type;
	tel->fallback_fn = fallback_fn;
	tel->header = header;
	tel->ctx = NULL;
	tel->state = GCIP_TELEMETRY_ENABLED;
	tel->mmapped_count = 0;

	if (tel->header) {
		tel->header->head = 0;
		tel->header->tail = 0;
		tel->header->size = mem->size;
		tel->header->entries_dropped = 0;
	}

	INIT_WORK(&tel->work, gcip_telemetry_worker);
	mutex_init(&tel->mmap_lock);
	mutex_init(&tel->state_ctx_lock);

	return 0;
}

void gcip_telemetry_exit(struct gcip_telemetry *tel)
{
	mutex_lock(&tel->state_ctx_lock);
	if (tel->ctx)
		eventfd_ctx_put(tel->ctx);
	tel->ctx = NULL;
	/* Prevents racing with the worker. */
	tel->state = GCIP_TELEMETRY_INVALID;
	mutex_unlock(&tel->state_ctx_lock);

	cancel_work_sync(&tel->work);
}
