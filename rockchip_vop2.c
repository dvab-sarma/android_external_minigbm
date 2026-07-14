/*
 * Copyright (C) 2026 Venkata Atchuta Bheemeswara Sarma Darbha (vdarbha0473@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */



 /* Rockchip VOP2 backend for mainline kernel (6.x+).
 *
 * The BSP rockchip.c uses DRM_IOCTL_ROCKCHIP_GEM_CREATE / GEM_MAP_OFFSET,
 * which are vendor ioctls absent from the upstream kernel.
 *
 * This backend runs on the Panthor GPU render node (renderD128) which is
 * accessible from every process context.  Buffers are allocated from the
 * DMA-BUF system heap (/dev/dma_heap/system) and imported via PRIME to
 * obtain a GEM handle.  CPU mapping uses dma-buf re-export + mmap.  The
 * VOP2 KMS driver imports the same dma-buf on card0 for hardware scanout.
 *
 * Kernel 6.14+ removed dumb_create from DRM_GEM_SHMEM_DRIVER_OPS, so
 * DRM_IOCTL_MODE_CREATE_DUMB / MODE_MAP_DUMB / MODE_DESTROY_DUMB are not
 * used here.
 *
 * AFBC: the mainline VOP2 driver (rockchip_vop2_reg.c) lists standard ARM
 * AFBC modifiers (DRM_FORMAT_MOD_ARM_AFBC with BLOCK_SIZE_16x16 variants).
 */

#ifdef DRV_ROCKCHIP_VOP2

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drv_helpers.h"
#include "drv_priv.h"
#include "external/dma-heap.h"
#include "util.h"

void vop2_resolve_format_and_use_flags(struct driver *drv, uint32_t format, uint64_t use_flags,
					   uint32_t *out_format, uint64_t *out_use_flags) 
{

	*out_format = format;
	*out_use_flags = use_flags;
	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* Camera subsystem requires NV12. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE)) {
			*out_format = DRM_FORMAT_NV12;
		} else {
			/*HACK: See b/28671744 */
			*out_format = DRM_FORMAT_XBGR8888;
		}
		break;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		*out_format = DRM_FORMAT_NV12;
		break;
	}

}

/*
 * DRM_FORMAT_MOD_ARM_AFBC(BLOCK_SIZE_16x16 | SPARSE | YTR) – this is the
 * same numeric value as the old DRM_FORMAT_MOD_ROCKCHIP_AFBC.  The VOP2
 * mainline driver lists this modifier in format_modifiers_afbc[].
 */
#define RK_AFBC_MOD \
	DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | \
				AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_YTR)

/*
 * Format tables – aligned with what VOP2 actually advertises to userspace
 * via the DRM plane's format_modifiers list (rockchip_vop2_reg.c).
*/

/* Formats supported for both scanout (VOP2) and GPU rendering. */
static const uint32_t scanout_render_formats[] = {
	DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888, DRM_FORMAT_XRGB8888,
	DRM_FORMAT_BGR888,   DRM_FORMAT_RGB888,
	DRM_FORMAT_RGB565,   DRM_FORMAT_BGR565,
};

/*
 * YUV formats scannable by VOP2 and usable by camera / video codec.
 * NV12 / NV21 / NV16 are semi-planar and very common on Rockchip SoCs.
 */
static const uint32_t scanout_yuv_formats[] = {
	DRM_FORMAT_NV12, DRM_FORMAT_NV21,
	DRM_FORMAT_NV16, DRM_FORMAT_NV61,
};

/* Texture-only (GPU) / camera / codec formats – not scanned out directly. */
static const uint32_t texture_only_formats[] = {
	DRM_FORMAT_YVU420, DRM_FORMAT_YVU420_ANDROID,
};

/* -----------------------------------------------------------------------
 * init
 * ----------------------------------------------------------------------- */

static int rockchip_vop2_init(struct driver *drv)
{
	drv_logi("rockchip_vop2_init: backend loaded\n");

	struct format_metadata linear = {
		.tiling   = 0,
		.priority = 1,
		.modifier = DRM_FORMAT_MOD_LINEAR,
	};

	/* RGB scanout + render */
	drv_add_combinations(drv, scanout_render_formats,
			     ARRAY_SIZE(scanout_render_formats),
			     &linear, BO_USE_RENDER_MASK | BO_USE_SCANOUT);

	/*
	 * YUV scanout + video.  BO_USE_SW_MASK is required because Codec2
	 * (ffmpeg_codec2, MediaCodec) fetches graphic blocks with
	 * CPU_READ | CPU_WRITE regardless of whether the actual data path
	 * is a CPU memcpy or a zero-copy DMA-BUF (RGA) operation.
	 * Without SW flags, drv_get_combination() returns nullptr and the
	 * block allocation fails with C2_CORRUPTED.
	 */
	drv_add_combinations(drv, scanout_yuv_formats,
			     ARRAY_SIZE(scanout_yuv_formats), &linear,
			     BO_USE_SCANOUT | BO_USE_HW_VIDEO_DECODER |
			     BO_USE_HW_VIDEO_ENCODER |
			     BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
			     BO_USE_TEXTURE_MASK);

	/* YUV texture / SW only */
	drv_add_combinations(drv, texture_only_formats,
			     ARRAY_SIZE(texture_only_formats),
			     &linear, BO_USE_TEXTURE_MASK);


	drv_modify_combination(drv, DRM_FORMAT_NV12, &linear,
			       BO_USE_HW_VIDEO_ENCODER | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE);

	drv_modify_linear_combinations(drv);

	{
		struct combination *c = drv_get_combination(drv, DRM_FORMAT_NV12,
							    BO_USE_SW_READ_OFTEN);
		drv_logi("NV12+SW_READ combo: %s (flags=0x%llx)\n",
			 c ? "FOUND" : "MISSING",
			 (unsigned long long)(c ? c->use_flags : 0));
	}

	/*
	 * R8 – JPEG / codec blob buffers.  Keep SW access flags so the
	 * CPU can read/write them directly.
	 */
	drv_add_combination(drv, DRM_FORMAT_R8, &linear,
			    BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
			    BO_USE_SW_MASK | BO_USE_LINEAR |
			    BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER |
			    BO_USE_GPU_DATA_BUFFER | BO_USE_SENSOR_DIRECT_DATA);

	return 0;
}

/* 
 * GEM allocation
 */

static int rockchip_vop2_create_gem(struct bo *bo)
{
	drv_logi("create_gem: fmt=0x%x size=%zu\n", bo->meta.format, bo->meta.total_size);

	int heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
	if (heap_fd < 0) {
		drv_loge("Failed to open /dev/dma_heap/system: %s\n", strerror(errno));
		return -errno;
	}

	struct dma_heap_allocation_data alloc = {
		.len      = bo->meta.total_size,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	int ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
	close(heap_fd);
	if (ret) {
		drv_loge("DMA_HEAP_IOCTL_ALLOC failed (size=%zu): %s\n",
			 bo->meta.total_size, strerror(errno));
		return -errno;
	}

	drv_logi("create_gem: dma_buf fd=%d\n", (int)alloc.fd);

	struct drm_prime_handle prime = { .fd = (int)alloc.fd };
	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
	close((int)alloc.fd);
	if (ret) {
		drv_loge("DRM_IOCTL_PRIME_FD_TO_HANDLE failed: %s\n", strerror(errno));
		return -errno;
	}

	drv_logi("create_gem: GEM handle=%u\n", prime.handle);
	bo->handle.u32 = prime.handle;
	return 0;
}

/*
 * AFBC layout helper (unchanged from rockchip.c)
 */

static int afbc_bo_from_format(struct bo *bo, uint32_t width, uint32_t height)
{
	const uint32_t pixel_size   = 4; /* AFBC is restricted to 32-bpp */
	const uint32_t block_width  = 16;
	const uint32_t block_height = 16;
	const uint32_t header_block = 16;
	const uint32_t body_block   = block_width * block_height * pixel_size;

	const uint32_t w_blocks = DIV_ROUND_UP(width,  block_width);
	const uint32_t h_blocks = DIV_ROUND_UP(height, block_height);
	const uint32_t total    = w_blocks * h_blocks;

	const uint32_t header_size = total * header_block;
	const uint32_t body_size   = total * body_block;
	/* EGL import requires 1024-byte body alignment */
	const uint32_t body_off    = ALIGN(header_size, 1024);

	bo->meta.strides[0]       = w_blocks * block_width * pixel_size;
	bo->meta.sizes[0]         = body_off + body_size;
	bo->meta.offsets[0]       = 0;
	bo->meta.total_size       = bo->meta.sizes[0];
	bo->meta.format_modifier  = RK_AFBC_MOD;

	return 0;
}

/* 
 * bo_create_with_modifiers
*/

static int rockchip_vop2_bo_create_with_modifiers(struct bo *bo,
						   uint32_t width,
						   uint32_t height,
						   uint32_t format,
						   const uint64_t *modifiers,
						   uint32_t count)
{
	/* AFBC path – only for 32-bpp RGB formats, width ≤ 2560 */
	if (drv_has_modifier(modifiers, count, RK_AFBC_MOD) &&
	    bo->drv->compression &&
	    width <= 2560 &&
	    format != DRM_FORMAT_NV12 && format != DRM_FORMAT_NV21 &&
	    format != DRM_FORMAT_NV16 && format != DRM_FORMAT_NV61) {
		afbc_bo_from_format(bo, width, height);
		return rockchip_vop2_create_gem(bo);
	}

	/* All other paths use a linear layout. */
	if (!drv_has_modifier(modifiers, count, DRM_FORMAT_MOD_LINEAR)) {
		errno = EINVAL;
		drv_loge("no usable modifier found\n");
		return -errno;
	}

	if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21) {
		/*
		 * Video decoders / encoders expect 16×16 macroblock alignment
		 * plus extra space at the end for motion vectors (128 bytes per
		 * macroblock, same as the BSP driver).
		 */
		uint32_t w_mbs = DIV_ROUND_UP(width,  16);
		uint32_t h_mbs = DIV_ROUND_UP(height, 16);
		uint32_t aw    = w_mbs * 16;
		uint32_t ah    = h_mbs * 16;

		drv_bo_from_format(bo, aw, 1, ah, format);
		bo->meta.total_size += w_mbs * h_mbs * 128;
	} else {
		/*
		 * ARM L1 cache line = 64 bytes.  Align stride to that for
		 * performance; YVU420 chroma planes need 64-byte alignment
		 * in the Mali CMEM allocator so luma gets 128.
		 */
		uint32_t stride = drv_stride_from_format(format, width, 0);
		if (format == DRM_FORMAT_YVU420 ||
		    format == DRM_FORMAT_YVU420_ANDROID)
			stride = ALIGN(stride, 128);
		else
			stride = ALIGN(stride, 64);

		drv_bo_from_format(bo, stride, 1, height, format);
	}

	return rockchip_vop2_create_gem(bo);
}

static int rockchip_vop2_bo_create(struct bo *bo, uint32_t width,
				    uint32_t height, uint32_t format,
				    uint64_t use_flags)
{
	const uint64_t mods[] = { DRM_FORMAT_MOD_LINEAR };
	return rockchip_vop2_bo_create_with_modifiers(bo, width, height,
						       format, mods,
						       ARRAY_SIZE(mods));
}

/* 
 * bo_map / bo_unmap
*/

static void *rockchip_vop2_bo_map(struct bo *bo, struct vma *vma,
				   uint32_t map_flags)
{
	if (bo->meta.format_modifier == RK_AFBC_MOD)
		return MAP_FAILED;

	/*
	 * Buffers are allocated via DMA-BUF system heap and imported as GEM
	 * objects.  DRM_IOCTL_MODE_MAP_DUMB does not work on imported GEM
	 * objects, so export back to a dma-buf fd and mmap that directly.
	 * The exported fd is closed after mmap; the mapping keeps a reference.
	 */
	int fd;
	int ret = drmPrimeHandleToFD(bo->drv->fd, bo->handle.u32,
				     DRM_CLOEXEC | DRM_RDWR, &fd);
	if (ret)
		ret = drmPrimeHandleToFD(bo->drv->fd, bo->handle.u32,
					 DRM_CLOEXEC, &fd);
	if (ret) {
		drv_loge("drmPrimeHandleToFD failed for mmap: %s\n", strerror(errno));
		return MAP_FAILED;
	}

	for (size_t i = 0; i < bo->meta.num_planes; i++)
		vma->length += bo->meta.sizes[i];

	void *addr = mmap(NULL, vma->length, drv_get_prot(map_flags),
			  MAP_SHARED, fd, 0);
	close(fd);
	return addr;
}

/* 
 * Backend descriptor
 */

const struct backend backend_rockchip = {
	/*
	 * Match the Panthor GPU render node (renderD*) instead of the VOP2
	 * card node.  
	 * Buffers are allocated from /dev/dma_heap/system and imported via
	 * PRIME — no dependency on dumb_create (which was removed from
	 * DRM_GEM_SHMEM_DRIVER_OPS in kernel 6.14+).  CPU mapping uses
	 * dma-buf re-export + mmap.  The VOP2 HWC opens card0 independently
	 * and imports the dma-buf there for atomic scanout.
	 */
	.name                      = "panthor",
	.init                      = rockchip_vop2_init,
	.bo_create                 = rockchip_vop2_bo_create,
	.bo_create_with_modifiers  = rockchip_vop2_bo_create_with_modifiers,
	.bo_destroy                = drv_gem_bo_destroy,
	.bo_import                 = drv_prime_bo_import,
	.bo_export                 = drv_prime_bo_export,
	.bo_map                    = rockchip_vop2_bo_map,
	.bo_unmap                  = drv_bo_munmap,
	.resolve_format_and_use_flags = vop2_resolve_format_and_use_flags,
};

#endif /* DRV_ROCKCHIP_VOP2 */
