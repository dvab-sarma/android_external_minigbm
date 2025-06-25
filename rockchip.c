/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef DRV_ROCKCHIP

#include <drm_fourcc.h>
#include <errno.h>
#include <inttypes.h>
#include <rockchip_drm.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#if !defined(ANDROID) || (ANDROID_API_LEVEL >= 31 && defined(HAS_DMABUF_SYSTEM_HEAP))
#include <linux/dma-heap.h>
#endif

#include "drv_helpers.h"
#include "drv_priv.h"
#include "util.h"

#define DRM_FORMAT_MOD_ROCKCHIP_AFBC                                                               \
	DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE |        \
				AFBC_FORMAT_MOD_YTR)

struct rockchip_private_map_data {
	void *cached_addr;
	void *gem_addr;
	int prime_fd;
};
struct rockchip_private_drv_data {
	int dma_heap_fd;
};

static const uint32_t scanout_render_formats[] = { DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB8888,
						   DRM_FORMAT_BGR888,	DRM_FORMAT_RGB565,
						   DRM_FORMAT_XBGR8888, DRM_FORMAT_XRGB8888 , DRM_FORMAT_ABGR16161616F};

static const uint32_t texture_only_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_YVU420,
						 DRM_FORMAT_YVU420_ANDROID, DRM_FORMAT_ABGR16161616F };
// Add separate FP16 support only when needed
//static const uint32_t fp16_formats[] = { DRM_FORMAT_ABGR16161616F };



static int afbc_bo_from_format(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			       uint64_t modifier)
{
	/* We've restricted ourselves to four bytes per pixel. */
	const uint32_t pixel_size = 4;

	const uint32_t clump_width = 4;
	const uint32_t clump_height = 4;

#define AFBC_NARROW 1
#if AFBC_NARROW == 1
	const uint32_t block_width = 4 * clump_width;
	const uint32_t block_height = 4 * clump_height;
#else
	const uint32_t block_width = 8 * clump_width;
	const uint32_t block_height = 2 * clump_height;
#endif

	const uint32_t header_block_size = 16;
	const uint32_t body_block_size = block_width * block_height * pixel_size;
	const uint32_t width_in_blocks = DIV_ROUND_UP(width, block_width);
	const uint32_t height_in_blocks = DIV_ROUND_UP(height, block_height);
	const uint32_t total_blocks = width_in_blocks * height_in_blocks;

	const uint32_t header_plane_size = total_blocks * header_block_size;
	const uint32_t body_plane_size = total_blocks * body_block_size;

	/* GPU requires 64 bytes, but EGL import code expects 1024 byte
	 * alignement for the body plane. */
	const uint32_t body_plane_alignment = 1024;

	const uint32_t body_plane_offset = ALIGN(header_plane_size, body_plane_alignment);
	const uint32_t total_size = body_plane_offset + body_plane_size;

	bo->meta.strides[0] = width_in_blocks * block_width * pixel_size;
	bo->meta.sizes[0] = total_size;
	bo->meta.offsets[0] = 0;

	bo->meta.total_size = total_size;

	bo->meta.format_modifier = modifier;

	return 0;
}

static int rockchip_init(struct driver *drv)
{
	struct format_metadata metadata;
	struct rockchip_private_drv_data *priv;

	priv = calloc(1, sizeof(*priv));
	if(!priv){
		drv_loge("failed calloc private data, errno=%d\n", -errno);
		return -errno;
	}
	priv->dma_heap_fd = -1;
	drv->priv = priv;

	metadata.tiling = 0;
	metadata.priority = 1;
	metadata.modifier = DRM_FORMAT_MOD_LINEAR;

	drv_add_combinations(drv, scanout_render_formats, ARRAY_SIZE(scanout_render_formats),
			     &metadata, BO_USE_RENDER_MASK | BO_USE_SCANOUT |BO_USE_PROTECTED);

	drv_add_combinations(drv, texture_only_formats, ARRAY_SIZE(texture_only_formats), &metadata,
			     BO_USE_TEXTURE_MASK | BO_USE_PROTECTED);
	// // In rockchip_init():
	// drv_add_combinations(drv, fp16_formats, ARRAY_SIZE(fp16_formats), &metadata,
    //                 BO_USE_RENDER_MASK | BO_USE_TEXTURE_MASK);

	// drv_modify_combination(drv, DRM_FORMAT_ABGR16161616F, &metadata,
    //                 BO_USE_RENDER_MASK | BO_USE_TEXTURE_MASK | BO_USE_GPU_DATA_BUFFER);
	/* NV12 format for camera, display, decoding and encoding. */
	/* Camera ISP supports only NV12 output. */
	drv_modify_combination(drv, DRM_FORMAT_NV12, &metadata,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SCANOUT |
				   BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER | BO_USE_PROTECTED);

	drv_modify_linear_combinations(drv);
	/*
	 * R8 format is used for Android's HAL_PIXEL_FORMAT_BLOB and is used for JPEG snapshots
	 * from camera and input/output from hardware decoder/encoder.
	 */
	drv_add_combination(drv, DRM_FORMAT_R8, &metadata,
			    BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SW_MASK |
				BO_USE_LINEAR | BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER |
				BO_USE_GPU_DATA_BUFFER | BO_USE_SENSOR_DIRECT_DATA | BO_USE_PROTECTED);
	// drv_add_combination(drv, DRM_FORMAT_ABGR16161616F, &metadata,
	// 		    BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_SW_MASK |
	// 			BO_USE_LINEAR | BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER |
	// 			BO_USE_GPU_DATA_BUFFER | BO_USE_SENSOR_DIRECT_DATA | BO_USE_PROTECTED);

	return 0;
}

static void rockchip_close(struct driver *drv){
	struct rockchip_private_drv_data *priv = (struct rockchip_private_drv_data *)drv->priv;
	if (priv->dma_heap_fd >= 0)
		close(priv->dma_heap_fd);	

	free(priv);
	drv->priv = NULL;
}

static int rockchip_bo_create_with_modifiers(struct bo *bo, uint32_t width, uint32_t height,
					     uint32_t format, const uint64_t *modifiers,
					     uint32_t count)
{
	 drv_logi("Creating BO: %dx%d fmt:%4.4s mod:%llx flags:%llx\n",
             width, height, (char*)&format, 
             modifiers ? modifiers[0] : 0, count);
	int ret;
	struct drm_rockchip_gem_create gem_create = { 0 };
	uint64_t afbc_modifier;

	const bool is_protected = bo->meta.use_flags & BO_USE_PROTECTED;

	if (drv_has_modifier(modifiers, count, DRM_FORMAT_MOD_ROCKCHIP_AFBC))
		afbc_modifier = DRM_FORMAT_MOD_ROCKCHIP_AFBC;
	/*else if (drv_has_modifier(modifiers, count, DRM_FORMAT_MOD_CHROMEOS_ROCKCHIP_AFBC))
		afbc_modifier = DRM_FORMAT_MOD_CHROMEOS_ROCKCHIP_AFBC;*/
	else
		afbc_modifier = 0;

	if (format == DRM_FORMAT_NV12) {
		uint32_t stride = ALIGN(width, 128); // 128-byte alignment for Rockchip VPU
        drv_bo_from_format(bo, stride, 1, height, format);
        bo->meta.total_size = stride * height * 3/2; // Y+UV planes
        /* Add extra space for motion vectors */
		uint32_t w_mbs = DIV_ROUND_UP(width, 16);
		uint32_t h_mbs = DIV_ROUND_UP(height, 16);

		uint32_t aligned_width = w_mbs * 16;
		uint32_t aligned_height = h_mbs * 16;

		drv_bo_from_format(bo, aligned_width, 1, aligned_height, format);
		/*
		 * drv_bo_from_format updates total_size. Add an extra data space for rockchip video
		 * driver to store motion vectors.
		 */
		bo->meta.total_size += w_mbs * h_mbs * 128;
	} else if (width <= 2560 && afbc_modifier && bo->drv->compression) {
		/* If the caller has decided they can use AFBC, always
		 * pick that */
		afbc_bo_from_format(bo, width, height, format, afbc_modifier);
	} else {
		if (!drv_has_modifier(modifiers, count, DRM_FORMAT_MOD_LINEAR)) {
			errno = EINVAL;
			drv_loge("no usable modifier found\n");
			return -errno;
		}

		uint32_t stride;
		/*
		 * Since the ARM L1 cache line size is 64 bytes, align to that
		 * as a performance optimization. For YV12, the Mali cmem allocator
		 * requires that chroma planes are aligned to 64-bytes, so align the
		 * luma plane to 128 bytes.
		 */
		stride = drv_stride_from_format(format, width, 0);
		if (format == DRM_FORMAT_YVU420 || format == DRM_FORMAT_YVU420_ANDROID)
			stride = ALIGN(stride, 128);
		else
			stride = ALIGN(stride, 64);
		// if (format == DRM_FORMAT_YVU420 || format == DRM_FORMAT_YVU420_ANDROID)
        //     stride = ALIGN(drv_stride_from_format(format, width, 0), 128);
        // else
        //     stride = ALIGN(drv_stride_from_format(format, width, 0), 64);

		drv_bo_from_format(bo, stride, 1, height, format);
	}

	if(is_protected){
#if !defined(ANDROID) || (ANDROID_API_LEVEL >= 31 && defined(HAS_DMABUF_SYSTEM_HEAP))
		int ret;
		struct rockchip_private_drv_data *priv = (struct rockchip_private_drv_data *)bo->drv->priv;
		struct dma_heap_allocation_data heap_data = {
			.len = bo->meta.total_size,
			.fd_flags = O_RDWR | O_CLOEXEC,
		};
		if (priv->dma_heap_fd < 0) {
			priv->dma_heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
			if (priv->dma_heap_fd < 0) {
				drv_loge("Failed opening secure CMA heap errno=%d\n", -errno);
				return -errno;
			}
		}
		ret = ioctl(priv->dma_heap_fd, DMA_HEAP_IOCTL_ALLOC, &heap_data);
		if (ret < 0) {
			drv_loge("Failed allocating CMA buffer ret=%d\n", ret);
			return ret;
		}
		ret = drmPrimeFDToHandle(bo->drv->fd, heap_data.fd, &bo->handle.u32);
		close(heap_data.fd);
		if (ret) {
			drv_loge("Failed drmPrimeFDToHandle(fd:%d) ret=%d\n", heap_data.fd, ret);
			return ret;
		}
#else
		drv_loge("Protected allocation not supported\n");
		return -1;
#endif
		return 0;
	}


	gem_create.size = bo->meta.total_size;
	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_ROCKCHIP_GEM_CREATE, &gem_create);

	if (ret) {
		drv_loge("DRM_IOCTL_ROCKCHIP_GEM_CREATE failed (size=%" PRIu64 ")\n",
			 gem_create.size);
		return -errno;
	}

	bo->handle.u32 = gem_create.handle;

	return 0;
}

static int rockchip_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
			      uint64_t use_flags)

{
	// Force LINEAR for scanout buffers
    if (use_flags & BO_USE_SCANOUT) {
        uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR };
        return rockchip_bo_create_with_modifiers(bo, width, height, format, 
                                               modifiers, ARRAY_SIZE(modifiers));
    }
	uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR };
	return rockchip_bo_create_with_modifiers(bo, width, height, format, modifiers,
						 ARRAY_SIZE(modifiers));
}

static void *rockchip_bo_map(struct bo *bo, struct vma *vma, uint32_t map_flags)
{
	int ret ,prime_fd;
	struct rockchip_private_map_data *priv;
	struct drm_rockchip_gem_map_off gem_map = { 0 };
	void *addr = NULL;

	/* We can only map buffers created with SW access flags, which should
	 * have no modifiers (ie, not AFBC). */
	// if (bo->meta.format_modifier == DRM_FORMAT_MOD_CHROMEOS_ROCKCHIP_AFBC ||
	//     bo->meta.format_modifier == DRM_FORMAT_MOD_ROCKCHIP_AFBC)
	// 	return MAP_FAILED;
	// if (bo->meta.format_modifier == DRM_FORMAT_MOD_ROCKCHIP_AFBC)
	// 	return MAP_FAILED;	

	gem_map.handle = bo->handle.u32;
	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_ROCKCHIP_GEM_MAP_OFFSET, &gem_map);
	if (ret) {
		drv_loge("DRM_IOCTL_ROCKCHIP_GEM_MAP_OFFSET failed\n");
		return MAP_FAILED;
	}

	prime_fd = drv_bo_get_plane_fd(bo,0);
	if (prime_fd < 0){
		drv_loge("Failed to get a prime fd\n");
		return MAP_FAILED;	
	}

	addr = mmap(0, bo->meta.total_size, drv_get_prot(map_flags), MAP_SHARED, bo->drv->fd,
		    gem_map.offset);
	if (addr == MAP_FAILED)
		//return MAP_FAILED;
		goto out_close_prime_fd;

	vma->length = bo->meta.total_size;
	priv = calloc(1, sizeof(*priv));
	if (!priv)
		goto out_unmap_addr;

	if (bo->meta.use_flags & BO_USE_RENDERSCRIPT) {
		// priv = calloc(1, sizeof(*priv));
		// if (!priv)
		// 	goto out_unmap_addr;

		priv->cached_addr = calloc(1, bo->meta.total_size);
		if (!priv->cached_addr)
			goto out_free_priv;

		priv->gem_addr = addr;
		//vma->priv = priv;
		addr = priv->cached_addr;
	}

	priv->prime_fd = prime_fd;
	vma->priv = priv;

	return addr;

out_free_priv:
	free(priv);
out_unmap_addr:
	munmap(addr, bo->meta.total_size);
	return MAP_FAILED;
out_close_prime_fd:
	close(prime_fd);
	return MAP_FAILED;
}

static int rockchip_bo_unmap(struct bo *bo, struct vma *vma)
{
	if (vma->priv) {
		struct rockchip_private_map_data *priv = vma->priv;
		vma->addr = priv->gem_addr;
		free(priv->cached_addr);
		free(priv);
		vma->priv = NULL;
	}

	return munmap(vma->addr, vma->length);
}

static int rockchip_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	struct rockchip_private_map_data *priv = mapping->vma->priv;
	struct pollfd fds = {
		.fd = priv->prime_fd,
	};
	if (mapping->vma->map_flags & BO_MAP_WRITE)
		fds.events |= POLLOUT;

	if (mapping->vma->map_flags & BO_MAP_READ)
		fds.events |= POLLIN;
	
	poll(&fds, 1 ,-1);
	if (fds.revents != fds.events)
		drv_loge("poll prime_fd failed\n");
	if (priv) {
		//struct rockchip_private_map_data *priv = mapping->vma->priv;
		memcpy(priv->cached_addr, priv->gem_addr, bo->meta.total_size);
	}

	return 0;
}

static int rockchip_bo_flush(struct bo *bo, struct mapping *mapping)
{
	struct rockchip_private_map_data *priv = mapping->vma->priv;
	if (priv && priv->cached_addr && (mapping->vma->map_flags & BO_MAP_WRITE))
		memcpy(priv->gem_addr, priv->cached_addr, bo->meta.total_size);

	return 0;
}

static void rockchip_resolve_format_and_use_flags(
	struct driver *drv, uint32_t format,
						  uint64_t use_flags, uint32_t *out_format,
						  uint64_t *out_use_flags
){
	*out_format = format;
	*out_use_flags = use_flags;
	*out_format = DRM_FORMAT_ABGR16161616F;
	*out_use_flags &= ~BO_USE_SCANOUT;
}

const struct backend backend_rockchip = {
	.name = "rockchip",
	.init = rockchip_init,
	.close = rockchip_close,
	.bo_create = rockchip_bo_create,
	.bo_create_with_modifiers = rockchip_bo_create_with_modifiers,
	.bo_destroy = drv_gem_bo_destroy,
	.bo_import = drv_prime_bo_import,
	.bo_map = rockchip_bo_map,
	.bo_unmap = rockchip_bo_unmap,
	.bo_invalidate = rockchip_bo_invalidate,
	.bo_flush = rockchip_bo_flush,
	//.resolve_format_and_use_flags = drv_resolve_format_and_use_flags_helper,
	.resolve_format_and_use_flags = rockchip_resolve_format_and_use_flags,
};

#endif
