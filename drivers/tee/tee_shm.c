/*
 * Copyright (c) 2015-2016, Linaro Limited
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <linux/idr.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include "tee_private.h"

/* extra references appended to shm object for registered shared memory */
struct tee_shm_dmabuf_ref {
	struct tee_shm shm;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
};

static void tee_shm_release(struct tee_shm *shm)
{
	struct tee_device *teedev = shm->teedev;

	mutex_lock(&teedev->mutex);
	idr_remove(&teedev->idr, shm->id);
	if (shm->ctx)
		list_del(&shm->link);
	mutex_unlock(&teedev->mutex);

	if (shm->flags & TEE_SHM_EXT_DMA_BUF) {
		struct tee_shm_dmabuf_ref *ref;

		ref = container_of(shm, struct tee_shm_dmabuf_ref, shm);
		dma_buf_unmap_attachment(ref->attach, ref->sgt,
					 DMA_BIDIRECTIONAL);
		dma_buf_detach(shm->dmabuf, ref->attach);
		dma_buf_put(ref->dmabuf);
	} else if (shm->flags & TEE_SHM_POOL) {
		struct tee_shm_pool_mgr *poolm;

		if (shm->flags & TEE_SHM_DMA_BUF)
			poolm = teedev->pool->dma_buf_mgr;
		else
			poolm = teedev->pool->private_mgr;

		poolm->ops->free(poolm, shm);
	} else if (shm->flags & TEE_SHM_REGISTER) {
		size_t n;
		int rc = teedev->desc->ops->shm_unregister(shm->ctx, shm);

		if (rc)
			dev_err(teedev->dev.parent,
				"unregister shm %p failed: %d", shm, rc);

		for (n = 0; n < shm->num_pages; n++)
			put_page(shm->pages[n]);

		kfree(shm->pages);
	}

	if (shm->ctx)
		teedev_ctx_put(shm->ctx);

	kfree(shm);
	tee_device_put(teedev);
}

static struct sg_table *tee_shm_op_map_dma_buf(struct dma_buf_attachment
			*attach, enum dma_data_direction dir)
{
	return NULL;
}

static void tee_shm_op_unmap_dma_buf(struct dma_buf_attachment *attach,
				     struct sg_table *table,
				     enum dma_data_direction dir)
{
}

static void tee_shm_op_release(struct dma_buf *dmabuf)
{
	struct tee_shm *shm = dmabuf->priv;

	tee_shm_release(shm);
}

static void *tee_shm_op_map(struct dma_buf *dmabuf, unsigned long pgnum)
{
	return NULL;
}

static int tee_shm_op_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct tee_shm *shm = dmabuf->priv;
	size_t size = vma->vm_end - vma->vm_start;

	/* Refuse sharing shared memory provided by application */
	if (shm->flags & TEE_SHM_REGISTER)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start, shm->paddr >> PAGE_SHIFT,
			       size, vma->vm_page_prot);
}

static void *tee_shm_op_map_atomic(struct dma_buf *dmabuf, unsigned long pgnum)
{
	return NULL;
}

static const struct dma_buf_ops tee_shm_dma_buf_ops = {
	.map_dma_buf = tee_shm_op_map_dma_buf,
	.unmap_dma_buf = tee_shm_op_unmap_dma_buf,
	.release = tee_shm_op_release,
	.map_atomic = tee_shm_op_map_atomic,
	.map = tee_shm_op_map,
	.mmap = tee_shm_op_mmap,
};

static struct tee_shm *__tee_shm_alloc(struct tee_context *ctx,
				       struct tee_device *teedev,
				       size_t size, u32 flags)
{
	struct tee_shm_pool_mgr *poolm = NULL;
	struct tee_shm *shm;
	void *ret;
	int rc;

	if (ctx && ctx->teedev != teedev) {
		dev_err(teedev->dev.parent, "ctx and teedev mismatch\n");
		return ERR_PTR(-EINVAL);
	}

	if (!(flags & TEE_SHM_MAPPED)) {
		dev_err(teedev->dev.parent,
			"only mapped allocations supported\n");
		return ERR_PTR(-EINVAL);
	}

	if ((flags & ~(TEE_SHM_MAPPED | TEE_SHM_DMA_BUF))) {
		dev_err(teedev->dev.parent, "invalid shm flags 0x%x", flags);
		return ERR_PTR(-EINVAL);
	}

	if (!tee_device_get(teedev))
		return ERR_PTR(-EINVAL);

	if (!teedev->pool) {
		/* teedev has been detached from driver */
		ret = ERR_PTR(-EINVAL);
		goto err_dev_put;
	}

	shm = kzalloc(sizeof(*shm), GFP_KERNEL);
	if (!shm) {
		ret = ERR_PTR(-ENOMEM);
		goto err_dev_put;
	}

	shm->flags = flags | TEE_SHM_POOL;
	shm->teedev = teedev;
	shm->ctx = ctx;
	if (flags & TEE_SHM_DMA_BUF)
		poolm = teedev->pool->dma_buf_mgr;
	else
		poolm = teedev->pool->private_mgr;

	rc = poolm->ops->alloc(poolm, shm, size);
	if (rc) {
		ret = ERR_PTR(rc);
		goto err_kfree;
	}

	mutex_lock(&teedev->mutex);
	shm->id = idr_alloc(&teedev->idr, shm, 1, 0, GFP_KERNEL);
	mutex_unlock(&teedev->mutex);
	if (shm->id < 0) {
		ret = ERR_PTR(shm->id);
		goto err_pool_free;
	}

	if (flags & TEE_SHM_DMA_BUF) {
		DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

		exp_info.ops = &tee_shm_dma_buf_ops;
		exp_info.size = shm->size;
		exp_info.flags = O_RDWR;
		exp_info.priv = shm;

		shm->dmabuf = dma_buf_export(&exp_info);
		if (IS_ERR(shm->dmabuf)) {
			ret = ERR_CAST(shm->dmabuf);
			goto err_rem;
		}
	}

	if (ctx) {
		teedev_ctx_get(ctx);
		mutex_lock(&teedev->mutex);
		list_add_tail(&shm->link, &ctx->list_shm);
		mutex_unlock(&teedev->mutex);
	}

	return shm;
err_rem:
	mutex_lock(&teedev->mutex);
	idr_remove(&teedev->idr, shm->id);
	mutex_unlock(&teedev->mutex);
err_pool_free:
	poolm->ops->free(poolm, shm);
err_kfree:
	kfree(shm);
err_dev_put:
	tee_device_put(teedev);
	return ret;
}

/**
 * tee_shm_alloc() - Allocate shared memory
 * @ctx:	Context that allocates the shared memory
 * @size:	Requested size of shared memory
 * @flags:	Flags setting properties for the requested shared memory.
 *
 * Memory allocated as global shared memory is automatically freed when the
 * TEE file pointer is closed. The @flags field uses the bits defined by
 * TEE_SHM_* in <linux/tee_drv.h>. TEE_SHM_MAPPED must currently always be
 * set. If TEE_SHM_DMA_BUF global shared memory will be allocated and
 * associated with a dma-buf handle, else driver private memory.
 */
struct tee_shm *tee_shm_alloc(struct tee_context *ctx, size_t size, u32 flags)
{
	return __tee_shm_alloc(ctx, ctx->teedev, size, flags);
}
EXPORT_SYMBOL_GPL(tee_shm_alloc);

struct tee_shm *tee_shm_priv_alloc(struct tee_device *teedev, size_t size)
{
	return __tee_shm_alloc(NULL, teedev, size, TEE_SHM_MAPPED);
}
EXPORT_SYMBOL_GPL(tee_shm_priv_alloc);

struct tee_shm *tee_shm_register(struct tee_context *ctx, unsigned long addr,
				 size_t length, u32 flags)
{
	struct tee_device *teedev = ctx->teedev;
	const u32 req_flags = TEE_SHM_DMA_BUF | TEE_SHM_USER_MAPPED;
	struct tee_shm *shm;
	void *ret;
	int rc;
	int num_pages;
	unsigned long start;

	if (flags != req_flags)
		return ERR_PTR(-ENOTSUPP);

	if (!tee_device_get(teedev))
		return ERR_PTR(-EINVAL);

	if (!teedev->desc->ops->shm_register ||
	    !teedev->desc->ops->shm_unregister) {
		tee_device_put(teedev);
		return ERR_PTR(-ENOTSUPP);
	}

	teedev_ctx_get(ctx);

	shm = kzalloc(sizeof(*shm), GFP_KERNEL);
	if (!shm) {
		ret = ERR_PTR(-ENOMEM);
		goto err;
	}

	shm->flags = flags | TEE_SHM_REGISTER;
	shm->teedev = teedev;
	shm->ctx = ctx;
	shm->id = -1;
	start = rounddown(addr, PAGE_SIZE);
	shm->offset = addr - start;
	shm->size = length;
	num_pages = (roundup(addr + length, PAGE_SIZE) - start) / PAGE_SIZE;
	shm->pages = kcalloc(num_pages, sizeof(*shm->pages), GFP_KERNEL);
	if (!shm->pages) {
		ret = ERR_PTR(-ENOMEM);
		goto err;
	}

	rc = get_user_pages_fast(start, num_pages, 1, shm->pages);
	if (rc > 0)
		shm->num_pages = rc;
	if (rc != num_pages) {
		if (rc >= 0)
			rc = -ENOMEM;
		ret = ERR_PTR(rc);
		goto err;
	}

	mutex_lock(&teedev->mutex);
	shm->id = idr_alloc(&teedev->idr, shm, 1, 0, GFP_KERNEL);
	mutex_unlock(&teedev->mutex);

	if (shm->id < 0) {
		ret = ERR_PTR(shm->id);
		goto err;
	}

	rc = teedev->desc->ops->shm_register(ctx, shm, shm->pages,
					     shm->num_pages, start);
	if (rc) {
		ret = ERR_PTR(rc);
		goto err;
	}

	if (flags & TEE_SHM_DMA_BUF) {
		DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

		exp_info.ops = &tee_shm_dma_buf_ops;
		exp_info.size = shm->size;
		exp_info.flags = O_RDWR;
		exp_info.priv = shm;

		shm->dmabuf = dma_buf_export(&exp_info);
		if (IS_ERR(shm->dmabuf)) {
			ret = ERR_CAST(shm->dmabuf);
			teedev->desc->ops->shm_unregister(ctx, shm);
			goto err;
		}
	}

	mutex_lock(&teedev->mutex);
	list_add_tail(&shm->link, &ctx->list_shm);
	mutex_unlock(&teedev->mutex);

	return shm;
err:
	if (shm) {
		size_t n;

		if (shm->id >= 0) {
			mutex_lock(&teedev->mutex);
			idr_remove(&teedev->idr, shm->id);
			mutex_unlock(&teedev->mutex);
		}
		if (shm->pages) {
			for (n = 0; n < shm->num_pages; n++)
				put_page(shm->pages[n]);
			kfree(shm->pages);
		}
	}
	kfree(shm);
	teedev_ctx_put(ctx);
	tee_device_put(teedev);
	return ret;
}
EXPORT_SYMBOL_GPL(tee_shm_register);

struct tee_shm *tee_shm_register_fd(struct tee_context *ctx, int fd)
{
	struct tee_shm_dmabuf_ref *ref;
	void *rc;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (!tee_device_get(ctx->teedev))
		return ERR_PTR(-EINVAL);

	teedev_ctx_get(ctx);

	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref) {
		rc = ERR_PTR(-ENOMEM);
		goto err;
	}

	ref->shm.ctx = ctx;
	ref->shm.teedev = ctx->teedev;
	ref->shm.id = -1;

	ref->dmabuf = dma_buf_get(fd);
	if (!ref->dmabuf) {
		rc = ERR_PTR(-EINVAL);
		goto err;
	}

	ref->attach = dma_buf_attach(ref->dmabuf, &ref->shm.teedev->dev);
	if (IS_ERR_OR_NULL(ref->attach)) {
		rc = ERR_PTR(-EINVAL);
		goto err;
	}

	ref->sgt = dma_buf_map_attachment(ref->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(ref->sgt)) {
		rc = ERR_PTR(-EINVAL);
		goto err;
	}

	if (sg_nents(ref->sgt->sgl) != 1) {
		rc = ERR_PTR(-EINVAL);
		goto err;
	}

	ref->shm.paddr = sg_dma_address(ref->sgt->sgl);
	ref->shm.size = sg_dma_len(ref->sgt->sgl);
	ref->shm.flags = TEE_SHM_DMA_BUF | TEE_SHM_EXT_DMA_BUF;

	mutex_lock(&ref->shm.teedev->mutex);
	ref->shm.id = idr_alloc(&ref->shm.teedev->idr, &ref->shm,
				1, 0, GFP_KERNEL);
	mutex_unlock(&ref->shm.teedev->mutex);
	if (ref->shm.id < 0) {
		rc = ERR_PTR(ref->shm.id);
		goto err;
	}

	/* export a dmabuf to later get a userland ref */
	exp_info.ops = &tee_shm_dma_buf_ops;
	exp_info.size = ref->shm.size;
	exp_info.flags = O_RDWR;
	exp_info.priv = &ref->shm;

	ref->shm.dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(ref->shm.dmabuf)) {
		rc = ERR_PTR(-EINVAL);
		goto err;
	}

	mutex_lock(&ref->shm.teedev->mutex);
	list_add_tail(&ref->shm.link, &ctx->list_shm);
	mutex_unlock(&ref->shm.teedev->mutex);

	return &ref->shm;

err:
	if (ref) {
		if (ref->shm.id >= 0) {
			mutex_lock(&ctx->teedev->mutex);
			idr_remove(&ctx->teedev->idr, ref->shm.id);
			mutex_unlock(&ctx->teedev->mutex);
		}
		if (ref->sgt)
			dma_buf_unmap_attachment(ref->attach, ref->sgt,
						 DMA_BIDIRECTIONAL);
		if (ref->attach)
			dma_buf_detach(ref->dmabuf, ref->attach);
		if (ref->dmabuf)
			dma_buf_put(ref->dmabuf);
	}
	kfree(ref);
	teedev_ctx_put(ctx);
	tee_device_put(ctx->teedev);
	return rc;
}
EXPORT_SYMBOL_GPL(tee_shm_register_fd);

/**
 * tee_shm_get_fd() - Increase reference count and return file descriptor
 * @shm:	Shared memory handle
 * @returns user space file descriptor to shared memory
 */
int tee_shm_get_fd(struct tee_shm *shm)
{
	int fd;

	if (!(shm->flags & TEE_SHM_DMA_BUF))
		return -EINVAL;

	get_dma_buf(shm->dmabuf);
	fd = dma_buf_fd(shm->dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(shm->dmabuf);
	return fd;
}

/**
 * tee_shm_free() - Free shared memory
 * @shm:	Handle to shared memory to free
 */
void tee_shm_free(struct tee_shm *shm)
{
	/*
	 * dma_buf_put() decreases the dmabuf reference counter and will
	 * call tee_shm_release() when the last reference is gone.
	 *
	 * In the case of driver private memory we call tee_shm_release
	 * directly instead as it doesn't have a reference counter.
	 */
	if (shm->flags & TEE_SHM_DMA_BUF)
		dma_buf_put(shm->dmabuf);
	else
		tee_shm_release(shm);
}
EXPORT_SYMBOL_GPL(tee_shm_free);

/**
 * tee_shm_va2pa() - Get physical address of a virtual address
 * @shm:	Shared memory handle
 * @va:		Virtual address to tranlsate
 * @pa:		Returned physical address
 * @returns 0 on success and < 0 on failure
 */
int tee_shm_va2pa(struct tee_shm *shm, void *va, phys_addr_t *pa)
{
	if (!(shm->flags & TEE_SHM_MAPPED))
		return -EINVAL;
	/* Check that we're in the range of the shm */
	if ((char *)va < (char *)shm->kaddr)
		return -EINVAL;
	if ((char *)va >= ((char *)shm->kaddr + shm->size))
		return -EINVAL;

	return tee_shm_get_pa(
			shm, (unsigned long)va - (unsigned long)shm->kaddr, pa);
}
EXPORT_SYMBOL_GPL(tee_shm_va2pa);

/**
 * tee_shm_pa2va() - Get virtual address of a physical address
 * @shm:	Shared memory handle
 * @pa:		Physical address to tranlsate
 * @va:		Returned virtual address
 * @returns 0 on success and < 0 on failure
 */
int tee_shm_pa2va(struct tee_shm *shm, phys_addr_t pa, void **va)
{
	if (!(shm->flags & TEE_SHM_MAPPED))
		return -EINVAL;
	/* Check that we're in the range of the shm */
	if (pa < shm->paddr)
		return -EINVAL;
	if (pa >= (shm->paddr + shm->size))
		return -EINVAL;

	if (va) {
		void *v = tee_shm_get_va(shm, pa - shm->paddr);

		if (IS_ERR(v))
			return PTR_ERR(v);
		*va = v;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(tee_shm_pa2va);

/**
 * tee_shm_get_va() - Get virtual address of a shared memory plus an offset
 * @shm:	Shared memory handle
 * @offs:	Offset from start of this shared memory
 * @returns virtual address of the shared memory + offs if offs is within
 *	the bounds of this shared memory, else an ERR_PTR
 */
void *tee_shm_get_va(struct tee_shm *shm, size_t offs)
{
	if (!(shm->flags & TEE_SHM_MAPPED))
		return ERR_PTR(-EINVAL);
	if (offs >= shm->size)
		return ERR_PTR(-EINVAL);
	return (char *)shm->kaddr + offs;
}
EXPORT_SYMBOL_GPL(tee_shm_get_va);

/**
 * tee_shm_get_pa() - Get physical address of a shared memory plus an offset
 * @shm:	Shared memory handle
 * @offs:	Offset from start of this shared memory
 * @pa:		Physical address to return
 * @returns 0 if offs is within the bounds of this shared memory, else an
 *	error code.
 */
int tee_shm_get_pa(struct tee_shm *shm, size_t offs, phys_addr_t *pa)
{
	if (offs >= shm->size)
		return -EINVAL;
	if (pa)
		*pa = shm->paddr + offs;
	return 0;
}
EXPORT_SYMBOL_GPL(tee_shm_get_pa);

/**
 * tee_shm_get_from_id() - Find shared memory object and increase reference
 * count
 * @ctx:	Context owning the shared memory
 * @id:		Id of shared memory object
 * @returns a pointer to 'struct tee_shm' on success or an ERR_PTR on failure
 */
struct tee_shm *tee_shm_get_from_id(struct tee_context *ctx, int id)
{
	struct tee_device *teedev;
	struct tee_shm *shm;

	if (!ctx)
		return ERR_PTR(-EINVAL);

	teedev = ctx->teedev;
	mutex_lock(&teedev->mutex);
	shm = idr_find(&teedev->idr, id);
	if (!shm || shm->ctx != ctx)
		shm = ERR_PTR(-EINVAL);
	else if (shm->flags & TEE_SHM_DMA_BUF)
		get_dma_buf(shm->dmabuf);
	mutex_unlock(&teedev->mutex);
	return shm;
}
EXPORT_SYMBOL_GPL(tee_shm_get_from_id);

/**
 * tee_shm_put() - Decrease reference count on a shared memory handle
 * @shm:	Shared memory handle
 */
void tee_shm_put(struct tee_shm *shm)
{
	if (shm->flags & TEE_SHM_DMA_BUF)
		dma_buf_put(shm->dmabuf);
}
EXPORT_SYMBOL_GPL(tee_shm_put);