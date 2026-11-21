// SPDX-License-Identifier: GPL-2.0
/*
 * dma-buf heap "system" allocated from the MediaTek ION multimedia heap.
 *
 * ion_import_dma_buf() accepts only dma-bufs that carry ION's own
 * dma_buf_ops, and ION_MM_GET_IOVA serves only buffers of heap type
 * ION_HEAP_TYPE_MULTIMEDIA. An allocation therefore goes through ion_alloc()
 * on that heap and leaves as the dma-buf ion_share_dma_buf() exports. ION
 * exports every buffer O_RDWR, so the O_ACCMODE bits of fd_flags do not
 * narrow the file.
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include "ion_drv.h"
#include "mtk_ion.h"

static struct ion_client *heap_client;

static int mtk_ion_system_heap_allocate(struct dma_heap *heap,
					unsigned long len,
					unsigned long fd_flags,
					unsigned long heap_flags)
{
	struct ion_handle *handle;
	struct dma_buf *dmabuf;
	int fd;

	handle = ion_alloc(heap_client, len, 0, ION_HEAP_MULTIMEDIA_MASK,
			   ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC);
	if (IS_ERR_OR_NULL(handle))
		return handle ? PTR_ERR(handle) : -ENOMEM;

	dmabuf = ion_share_dma_buf(heap_client, handle);
	ion_free(heap_client, handle);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, fd_flags);
	if (fd < 0)
		dma_buf_put(dmabuf);

	return fd;
}

static const struct dma_heap_ops mtk_ion_system_heap_ops = {
	.allocate = mtk_ion_system_heap_allocate,
};

static int __init mtk_ion_system_heap_init(void)
{
	struct dma_heap_export_info exp_info = {
		.name = "system",
		.ops = &mtk_ion_system_heap_ops,
	};
	struct dma_heap *heap;

	if (!g_ion_device) {
		pr_err("dma_heap: ION device absent, heap system not registered\n");
		return -ENODEV;
	}

	heap_client = ion_client_create(g_ion_device, "dma_heap_system");
	if (IS_ERR_OR_NULL(heap_client))
		return heap_client ? PTR_ERR(heap_client) : -ENOMEM;

	heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap)) {
		ion_client_destroy(heap_client);
		return PTR_ERR(heap);
	}

	return 0;
}
/*
 * ion_drv_probe() returns -EPROBE_DEFER until its IOMMU domain exists, and the
 * deferred-probe flush runs in a late_initcall, which precedes this one.
 */
late_initcall_sync(mtk_ion_system_heap_init);
