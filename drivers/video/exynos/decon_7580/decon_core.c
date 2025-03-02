/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Core file for Samsung EXYNOS DECON driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/clk-provider.h>
#include <linux/dma-buf.h>
#include <linux/exynos_ion.h>
#include <linux/ion.h>
#include <linux/highmem.h>
#include <linux/memblock.h>
#include <linux/exynos_iovmm.h>
#include <linux/bug.h>
#include <linux/of_address.h>
#include <linux/smc.h>
#include <linux/clk-private.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#ifdef CONFIG_POWERSUSPEND
#include <linux/powersuspend.h>
#endif

#include <mach/regs-clock.h>
#include <mach/exynos-pm.h>
#include <media/exynos_mc.h>
#include <video/mipi_display.h>
#include <video/videonode.h>
#include <media/v4l2-subdev.h>

#include "decon.h"
#include "dsim.h"
#include "decon_helper.h"
#include "dpu_common.h"
#include "regs-dpu.h"
#include "panels/dsim_panel.h"

#include "../../../staging/android/sw_sync.h"

#define MIN(x, y) (x < y ? x : y)

#ifdef CONFIG_OF
static const struct of_device_id decon_device_table[] = {
	{ .compatible = "samsung,exynos5-decon_driver" },
	{},
};
MODULE_DEVICE_TABLE(of, decon_device_table);
#endif

int decon_log_level = DECON_LOG_LEVEL_INFO;
module_param(decon_log_level, int, 0644);

struct decon_device *decon_int_drvdata;
EXPORT_SYMBOL(decon_int_drvdata);

static int decon_runtime_resume(struct device *dev);
static int decon_runtime_suspend(struct device *dev);
static void decon_set_protected_content(struct decon_device *decon,
			bool enable);

#ifdef CONFIG_USE_VSYNC_SKIP
static atomic_t extra_vsync_wait;
#endif /* CCONFIG_USE_VSYNC_SKIP */

void decon_dump(struct decon_device *decon)
{
	dev_err(decon->dev, "=== DECON CLK VALUES ===\n");

	dev_err(decon->dev, "mif(%ld), aclk(%ld)\n",
		decon->res.mif_pll->rate / MHZ,
		decon->res.aclk->rate / MHZ);

	dev_err(decon->dev, "=== DECON SFR DUMP ===\n");

	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			decon->regs, 0x718, false);
	dev_err(decon->dev, "=== DECON MIC SFR DUMP ===\n");

	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			decon->regs + 0x2400, 0x20, false);
	dev_err(decon->dev, "=== DECON SHADOW SFR DUMP ===\n");

	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			decon->regs + SHADOW_OFFSET, 0x718, false);

	v4l2_subdev_call(decon->output_sd, core, ioctl, DSIM_IOC_DUMP, NULL);
}

#ifdef CONFIG_USE_VSYNC_SKIP
void decon_extra_vsync_wait_set(int set_count)
{
	atomic_set(&extra_vsync_wait, set_count);
}

int decon_extra_vsync_wait_get(void)
{
	return atomic_read(&extra_vsync_wait);
}

void decon_extra_vsync_wait_add(int skip_count)
{
	atomic_add(skip_count, &extra_vsync_wait);
}
#endif /* CONFIG_USE_VSYNC_SKIP */

/* ---------- CHECK FUNCTIONS ----------- */
static void decon_to_regs_param(struct decon_regs_data *win_regs,
		struct decon_reg_data *regs, int idx)
{
	win_regs->wincon = regs->wincon[idx];
	win_regs->winmap = regs->winmap[idx];
	win_regs->vidosd_a = regs->vidosd_a[idx];
	win_regs->vidosd_b = regs->vidosd_b[idx];
	win_regs->vidosd_c = regs->vidosd_c[idx];
	win_regs->vidosd_d = regs->vidosd_d[idx];
	win_regs->vidw_buf_start = regs->buf_start[idx];
	win_regs->vidw_whole_w = regs->whole_w[idx];
	win_regs->vidw_whole_h = regs->whole_h[idx];
	win_regs->vidw_offset_x = regs->offset_x[idx];
	win_regs->vidw_offset_y = regs->offset_y[idx];
	win_regs->vidw_plane2_buf_start = regs->dma_buf_data[idx][1].dma_addr;
	win_regs->vidw_plane3_buf_start = regs->dma_buf_data[idx][2].dma_addr;

	if (idx)
		win_regs->blendeq = regs->blendeq[idx - 1];

	win_regs->type = regs->win_config[idx].idma_type;

	decon_dbg("decon idma_type(%d)\n", regs->win_config->idma_type);
}

static u16 fb_panstep(u32 res, u32 res_virtual)
{
	return res_virtual > res ? 1 : 0;
}

static u32 vidosd_a(int x, int y)
{
	return VIDOSD_A_TOPLEFT_X(x) |
			VIDOSD_A_TOPLEFT_Y(y);
}

static u32 vidosd_b(int x, int y, u32 xres, u32 yres)
{
	return VIDOSD_B_BOTRIGHT_X(x + xres - 1) |
		VIDOSD_B_BOTRIGHT_Y(y + yres - 1);
}

static u32 vidosd_c(u8 r0, u8 g0, u8 b0)
{
	return VIDOSD_C_ALPHA0_R_F(r0) |
		VIDOSD_C_ALPHA0_G_F(g0) |
		VIDOSD_C_ALPHA0_B_F(b0);
}

static u32 vidosd_d(u8 r1, u8 g1, u8 b1)
{
	return VIDOSD_D_ALPHA1_R_F(r1) |
		VIDOSD_D_ALPHA1_G_F(g1) |
		VIDOSD_D_ALPHA1_B_F(b1);
}

static u32 wincon(u32 bits_per_pixel, u32 transp_length, int format)
{
	u32 data = 0;

	switch (bits_per_pixel) {
	case 12:
		if (format == DECON_PIXEL_FORMAT_NV12 ||
			format == DECON_PIXEL_FORMAT_NV12M)
			data |= WINCON_BPPMODE_NV12;
		else if (format == DECON_PIXEL_FORMAT_NV21 ||
			format == DECON_PIXEL_FORMAT_NV21M ||
			format == DECON_PIXEL_FORMAT_NV21M_FULL)
			data |= WINCON_BPPMODE_NV21;

		data |= WINCON_INTERPOLATION_EN;
		break;
	case 16:
		data |= WINCON_BPPMODE_RGB565;
		break;
	case 24:
	case 32:
		if (transp_length > 0) {
			data |= WINCON_BLD_PIX;
			data |= WINCON_BPPMODE_ARGB8888;
		} else {
			data |= WINCON_BPPMODE_XRGB8888;
		}
		break;
	default:
		pr_err("%d bpp doesn't support\n", bits_per_pixel);
		break;
	}

	if (transp_length != 1)
		data |= WINCON_ALPHA_SEL;

	return data;
}

static inline u32 blendeq(enum decon_blending blending, u8 transp_length,
		int plane_alpha)
{
	u8 a, b;
	int is_plane_alpha = (plane_alpha < 255 && plane_alpha > 0) ? 1 : 0;

	if (transp_length == 1 && blending == DECON_BLENDING_PREMULT)
		blending = DECON_BLENDING_COVERAGE;

	switch (blending) {
	case DECON_BLENDING_NONE:
		a = BLENDE_COEF_ONE;
		b = BLENDE_COEF_ZERO;
		break;

	case DECON_BLENDING_PREMULT:
		if (!is_plane_alpha) {
			a = BLENDE_COEF_ONE;
			b = BLENDE_COEF_ONE_MINUS_ALPHA_A;
		} else {
			a = BLENDE_COEF_ALPHA0;
			b = BLENDE_COEF_ONE_MINUS_ALPHA_A;
		}
		break;

	case DECON_BLENDING_COVERAGE:
		a = BLENDE_COEF_ALPHA_A;
		b = BLENDE_COEF_ONE_MINUS_ALPHA_A;
		break;

	default:
		return 0;
	}

	return BLENDE_A_FUNC(a) |
			BLENDE_B_FUNC(b) |
			BLENDE_P_FUNC(BLENDE_COEF_ZERO) |
			BLENDE_Q_FUNC(BLENDE_COEF_ZERO);
}

static u32 decon_red_length(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 8;

	case DECON_PIXEL_FORMAT_RGBA_5551:
		return 5;

	case DECON_PIXEL_FORMAT_RGB_565:
		return 5;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

static u32 decon_red_offset(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_RGBA_5551:
		return 0;

	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
		return 8;

	case DECON_PIXEL_FORMAT_RGB_565:
		return 11;

	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 16;

	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
		return 24;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

static u32 decon_green_length(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 8;

	case DECON_PIXEL_FORMAT_RGBA_5551:
		return 5;

	case DECON_PIXEL_FORMAT_RGB_565:
		return 6;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

static u32 decon_green_offset(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 8;

	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
		return 16;

	case DECON_PIXEL_FORMAT_RGBA_5551:
	case DECON_PIXEL_FORMAT_RGB_565:
		return 5;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

static u32 decon_blue_length(int format)
{
	return decon_red_length(format);
}

static u32 decon_blue_offset(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
		return 16;

	case DECON_PIXEL_FORMAT_RGBA_5551:
		return 10;

	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
		return 8;

	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
		return 24;

	case DECON_PIXEL_FORMAT_RGB_565:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 0;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

static u32 decon_transp_length(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
		return 8;

	case DECON_PIXEL_FORMAT_RGBA_5551:
		return 1;

	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_RGB_565:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 0;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

static u32 decon_transp_offset(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
		return 24;

	case DECON_PIXEL_FORMAT_RGBA_5551:
		return 15;

	case DECON_PIXEL_FORMAT_RGBX_8888:
		return decon_blue_offset(format);

	case DECON_PIXEL_FORMAT_BGRX_8888:
		return decon_red_offset(format);

	case DECON_PIXEL_FORMAT_RGB_565:
		return 0;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

static u32 decon_padding(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 8;

	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_RGBA_5551:
	case DECON_PIXEL_FORMAT_RGB_565:
	case DECON_PIXEL_FORMAT_BGRA_8888:
		return 0;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}

}

/* DECON_PIXEL_FORMAT_RGBA_8888 and WINCON_BPPMODE_ABGR8888 are same format
 * A[31:24] : B[23:16] : G[15:8] : R[7:0] */
static u32 decon_rgborder(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_RGBA_8888:
		return WINCON_BPPMODE_ABGR8888;
	case DECON_PIXEL_FORMAT_RGBX_8888:
		return WINCON_BPPMODE_XBGR8888;
	case DECON_PIXEL_FORMAT_RGB_565:
		return WINCON_BPPMODE_RGB565;
	case DECON_PIXEL_FORMAT_BGRA_8888:
		return WINCON_BPPMODE_ARGB8888;
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return WINCON_BPPMODE_XRGB8888;
	case DECON_PIXEL_FORMAT_ARGB_8888:
		return WINCON_BPPMODE_BGRA8888;
	case DECON_PIXEL_FORMAT_ABGR_8888:
		return WINCON_BPPMODE_RGBA8888;
	case DECON_PIXEL_FORMAT_XRGB_8888:
		return WINCON_BPPMODE_BGRX8888;
	case DECON_PIXEL_FORMAT_XBGR_8888:
		return WINCON_BPPMODE_RGBX8888;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 0;

	default:
		decon_warn("unrecognized pixel format %u\n", format);
		return 0;
	}
}

bool decon_validate_x_alignment(struct decon_device *decon, int x, u32 w,
		u32 bits_per_pixel)
{
	uint8_t pixel_alignment = 32 / bits_per_pixel;

	if (x % pixel_alignment) {
		decon_err("left X coordinate not properly aligned to %u-pixel boundary (bpp = %u, x = %u)\n",
				pixel_alignment, bits_per_pixel, x);
		return 0;
	}
	if ((x + w) % pixel_alignment) {
		decon_err("right X coordinate not properly aligned to %u-pixel boundary (bpp = %u, x = %u, w = %u)\n",
				pixel_alignment, bits_per_pixel, x, w);
		return 0;
	}

	return 1;
}

static unsigned int decon_calc_bandwidth(u32 w, u32 h,
				u32 bytes_per_pixel, int fps)
{
	unsigned int bw = w * h;

	bw *= bytes_per_pixel;
	bw *= fps;
	return bw;
}

#ifdef CONFIG_CPU_IDLE
static int exynos_decon_lpc_event(struct notifier_block *notifier,
		unsigned long pm_event, void *v)
{
	struct decon_device *decon = get_decon_drvdata(0);
	int err = NOTIFY_DONE;

	switch (pm_event) {
	case LPC_PREPARE:
		if (decon->state != DECON_STATE_LPD)
			err = -EBUSY;
		break;
	}

	return notifier_from_errno(err);
}

static struct notifier_block exynos_decon_lpc_nb = {
	.notifier_call = exynos_decon_lpc_event,
};
#endif

/* ---------- OVERLAP COUNT CALCULATION ----------- */
static bool is_decon_rect_differ(struct decon_rect *r1,
		struct decon_rect *r2)
{
	return ((r1->left != r2->left) || (r1->top != r2->top) ||
		(r1->right != r2->right) || (r1->bottom != r2->bottom));
}

static inline bool does_layer_need_scale(struct decon_win_config *config)
{
	return (config->dst.w != config->src.w) || (config->dst.h != config->src.h);
}

static bool decon_intersect(struct decon_rect *r1, struct decon_rect *r2)
{
	return !(r1->left > r2->right || r1->right < r2->left ||
		r1->top > r2->bottom || r1->bottom < r2->top);
}

static int decon_intersection(struct decon_rect *r1,
				struct decon_rect *r2, struct decon_rect *r3)
{
	r3->top = max(r1->top, r2->top);
	r3->bottom = min(r1->bottom, r2->bottom);
	r3->left = max(r1->left, r2->left);
	r3->right = min(r1->right, r2->right);
	return 0;
}

static inline bool is_decon_rgb32(int format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return true;
	default:
		return false;
	}
}

static int decon_set_win_blocking_mode(struct decon_device *decon,
		struct decon_win *win, struct decon_win_config *win_config,
		struct decon_reg_data *regs)
{
	struct decon_rect r1, r2, overlap_rect, block_rect;
	unsigned int overlap_size, blocking_size = 0;
	struct decon_win_config *config = &win_config[win->index];
	int j;
	bool enabled = false;

	if (config->state != DECON_WIN_STATE_BUFFER)
		return 0;

	/* Blocking mode is supported for only RGB32 color formats */
	if (!is_decon_rgb32(config->format))
		return 0;

	r1.left = win_config[win->index].dst.x;
	r1.top = win_config[win->index].dst.y;
	r1.right = r1.left + win_config[win->index].dst.w - 1;
	r1.bottom = r1.top + win_config[win->index].dst.h - 1;

	memset(&block_rect, 0, sizeof(struct decon_rect));
	for (j = win->index + 1; j < decon->pdata->max_win; j++) {
		struct decon_win_config *config = &win_config[j];
		if (config->state != DECON_WIN_STATE_BUFFER)
			continue;

		/* Support only XRGB */
		if ((config->format == DECON_PIXEL_FORMAT_ARGB_8888) ||
				(config->format == DECON_PIXEL_FORMAT_ABGR_8888) ||
				(config->format == DECON_PIXEL_FORMAT_RGBA_8888) ||
				(config->format == DECON_PIXEL_FORMAT_BGRA_8888) ||
				(config->format == DECON_PIXEL_FORMAT_RGBA_5551) ||
				((config->plane_alpha < 255) && (config->plane_alpha > 0)))
			continue;

		r2.left = config->dst.x;
		r2.top = config->dst.y;
		r2.right = r2.left + config->dst.w - 1;
		r2.bottom = r2.top + config->dst.h - 1;
		/* overlaps or not */
		if (decon_intersect(&r1, &r2)) {
			decon_intersection(&r1, &r2, &overlap_rect);
			if (!is_decon_rect_differ(&r1, &overlap_rect)) {
				/* window rect and blocking rect is same. */
				win_config[win->index].state =
					DECON_WIN_STATE_DISABLED;
				return 1;
			}
			if (overlap_rect.right - overlap_rect.left + 1 <
					MIN_BLK_MODE_WIDTH ||
				overlap_rect.bottom - overlap_rect.top + 1 <
					MIN_BLK_MODE_HEIGHT)
				continue;

			overlap_size = (overlap_rect.right - overlap_rect.left) *
					(overlap_rect.bottom - overlap_rect.top);

			if (overlap_size > blocking_size) {
				memcpy(&block_rect, &overlap_rect,
						sizeof(struct decon_rect));
				blocking_size = (block_rect.right - block_rect.left) *
						(block_rect.bottom - block_rect.top);
				enabled = true;
			}
		}
	}

	if (enabled) {
		regs->block_rect[win->index].w = block_rect.right - block_rect.left + 1;
		regs->block_rect[win->index].h = block_rect.bottom - block_rect.top + 1;
		regs->block_rect[win->index].x = block_rect.left -
					win_config[win->index].dst.x;
		regs->block_rect[win->index].y = block_rect.top -
					win_config[win->index].dst.y;

		memcpy(&win_config->block_area, &regs->block_rect[win->index],
				sizeof(struct decon_win_rect));
	}
	return 0;
}

static void decon_enable_blocking_mode(struct decon_device *decon,
		struct decon_reg_data *regs, u32 win_idx)
{
	struct decon_win_rect rect = regs->block_rect[win_idx];
	bool enable = false;

	/* TODO: Check a DECON H/W limitation */
	enable = (rect.w * rect.h) ? true : false;

	if (enable) {
		decon_reg_set_block_mode(DECON_INT, win_idx, rect.x, rect.y,
						rect.w, rect.h, true);
		decon_dbg("win[%d] blocking_mode:(%d,%d,%d,%d)\n", win_idx,
				rect.x, rect.y, rect.w, rect.h);
	} else {
		decon_reg_set_block_mode(DECON_INT, win_idx, 0, 0, 0, 0, false);
	}
}

#ifdef CONFIG_FB_WINDOW_UPDATE
static void decon_wait_for_framedone(struct decon_device *decon)
{
	int ret;
	s64 time_ms = ktime_to_ms(ktime_get()) - ktime_to_ms(decon->trig_mask_timestamp);

	if (time_ms < 17) {
		DISP_SS_EVENT_LOG(DISP_EVT_DECON_FRAMEDONE_WAIT, &decon->sd, ktime_set(0, 0));
		ret = wait_event_interruptible_timeout(decon->wait_frmdone,
			(decon->frame_done_cnt_target <= decon->frame_done_cnt_cur),
		msecs_to_jiffies(17 - time_ms));
	}
}

inline static void decon_win_update_rect_reset(struct decon_device *decon)
{
	decon->update_win.x = 0;
	decon->update_win.y = 0;
	decon->update_win.w = 0;
	decon->update_win.h = 0;
	decon->need_update = true;
}

static int decon_reg_ddi_partial_cmd(struct decon_device *decon, struct decon_win_rect *rect)
{
	struct decon_win_rect win_rect;
	int ret;

	/* Wait for frame done before proceeding */
	decon_wait_for_framedone(decon);

	/* TODO: need to set DSI_IDX */
	decon_reg_wait_linecnt_is_zero_timeout(DECON_INT, 0, 35 * 1000);

	DISP_SS_EVENT_LOG(DISP_EVT_LINECNT_ZERO, &decon->sd, ktime_set(0, 0));

	/* Partial Command */
	win_rect.x = rect->x;
	win_rect.y = rect->y;
	/* w is right & h is bottom */
	win_rect.w = rect->x + rect->w - 1;
	win_rect.h = rect->y + rect->h - 1;
#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	ret = v4l2_subdev_call(decon->output_sd, core, ioctl, DSIM_IOC_PKT_GO_DISABLE, NULL);
	if (ret)
		decon_err("Failed to disable Packet-go in %s\n", __func__);
#endif
	ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
			DSIM_IOC_PARTIAL_CMD, &win_rect);
	if (ret) {
		decon_win_update_rect_reset(decon);
		decon_err("%s: partial_area CMD is failed  %s [%d %d %d %d]\n",
				__func__, decon->output_sd->name, rect->x,
				rect->y, rect->w, rect->h);
	}
#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	v4l2_subdev_call(decon->output_sd, core, ioctl, DSIM_IOC_PKT_GO_ENABLE, NULL); /* Don't care failure or success */
#endif

	return ret;
}

static int decon_win_update_disp_config(struct decon_device *decon,
					struct decon_win_rect *win_rect)
{
	struct decon_lcd lcd_info;
	int ret = 0;

	memcpy(&lcd_info, decon->lcd_info, sizeof(struct decon_lcd));
	lcd_info.xres = win_rect->w;
	lcd_info.yres = win_rect->h;

	lcd_info.hfp = decon->lcd_info->hfp + ((decon->lcd_info->xres - win_rect->w) >> 1);
	lcd_info.vfp = decon->lcd_info->vfp + decon->lcd_info->yres - win_rect->h;

	v4l2_set_subdev_hostdata(decon->output_sd, &lcd_info);
	ret = v4l2_subdev_call(decon->output_sd, core, ioctl, DSIM_IOC_SET_PORCH, NULL);
	if (ret) {
		decon_win_update_rect_reset(decon);
		decon_err("failed to set porch values of DSIM [%d %d %d %d]\n",
				win_rect->x, win_rect->y,
				win_rect->w, win_rect->h);
	}

	if (lcd_info.mic_enabled)
		decon_reg_config_mic(DECON_INT, 0, &lcd_info);
	decon_reg_set_porch(DECON_INT, 0, &lcd_info);
	decon_win_update_dbg("[WIN_UPDATE]%s : vfp %d vbp %d vsa %d hfp %d hbp %d hsa %d w %d h %d\n",
			__func__,
			lcd_info.vfp, lcd_info.vbp, lcd_info.vsa,
			lcd_info.hfp, lcd_info.hbp, lcd_info.hsa,
			win_rect->w, win_rect->h);

	return ret;
}
#endif

static void decon_esd_enable_interrupt(struct decon_device *decon)
{
	struct esd_protect *esd = &decon->esd;

	if (esd) {
		if (esd->pcd_irq) {
			decon_info("%s: pcd(%d) is %s, %d\n", __func__, esd->pcd_gpio, gpio_get_value(esd->pcd_gpio) ? "high" : "low", decon->ignore_vsync);
			enable_irq(esd->pcd_irq);
			if (esd->pcd_pin_active == gpio_get_value(esd->pcd_gpio)) {
				decon_info("%s: Detection panel crack. from now ignore vsync\n", __func__);
				decon->ignore_vsync = true;
			}
		}
		if (esd->err_irq) {
			decon_info("%s: err(%d) is %s\n", __func__, esd->err_gpio, gpio_get_value(esd->err_gpio) ? "high" : "low");
			enable_irq(esd->err_irq);
		}
		if (esd->disp_det_irq) {
			decon_info("%s: det(%d) is %s\n", __func__, esd->disp_det_gpio, gpio_get_value(esd->disp_det_gpio) ? "high" : "low");
			enable_irq(esd->disp_det_irq);
		}
	}
	return;
}

static void decon_esd_disable_interrupt(struct decon_device *decon)
{
	struct esd_protect *esd = &decon->esd;

	if (esd) {
		if (esd->pcd_irq) {
			decon_info("%s: pcd(%d) is %s, %d\n", __func__, esd->pcd_gpio, gpio_get_value(esd->pcd_gpio) ? "high" : "low", decon->ignore_vsync);
			disable_irq(esd->pcd_irq);
		}
		if (esd->err_irq) {
			decon_info("%s: err(%d) is %s\n", __func__, esd->err_gpio, gpio_get_value(esd->err_gpio) ? "high" : "low");
			disable_irq(esd->err_irq);
		}
		if (esd->disp_det_irq) {
			decon_info("%s: det(%d) is %s\n", __func__, esd->disp_det_gpio, gpio_get_value(esd->disp_det_gpio) ? "high" : "low");
			disable_irq(esd->disp_det_irq);
		}
	}
	return;
}

/* ---------- FB_BLANK INTERFACE ----------- */
int decon_enable(struct decon_device *decon)
{
	struct decon_psr_info psr;
	struct decon_init_param p;
	int state = decon->state;
	int ret = 0;

	decon_dbg("enable decon-%s\n", "int");
	exynos_ss_printk("%s:state %d: active %d:+\n", __func__,
				decon->state, pm_runtime_active(decon->dev));

	if (decon->state != DECON_STATE_LPD_EXIT_REQ)
		mutex_lock(&decon->output_lock);

	if ((decon->out_type == DECON_OUT_DSI) && (decon->state == DECON_STATE_INIT)) {
		decon_info("decon in init state\n");
		decon->state = DECON_STATE_ON;
		goto err;
	}

	if (decon->state == DECON_STATE_ON) {
		decon_warn("decon already enabled\n");
		goto err;
	}

	decon->prev_bw = 0;
	/* set bandwidth to default (3 full frame) */
	decon_set_qos(decon, NULL, false, false);


#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_get_sync(decon->dev);
#else
	decon_runtime_resume(decon->dev);
#endif

	if (decon->state == DECON_STATE_LPD_EXIT_REQ) {
		ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
				DSIM_IOC_ENTER_ULPS, (unsigned long *)0);
		if (ret) {
			decon_err("%s: failed to exit ULPS state for %s\n",
					__func__, decon->output_sd->name);
			goto err;
		}
	} else if (decon->out_type == DECON_OUT_DSI) {
		pm_stay_awake(decon->dev);
		dev_warn(decon->dev, "pm_stay_awake");
		ret = v4l2_subdev_call(decon->output_sd, video, s_stream, 1);
		if (ret) {
			decon_err("starting stream failed for %s\n",
					decon->output_sd->name);
			goto err;
		}
	}

	ret = iovmm_activate(decon->dev);
	if (ret < 0) {
		decon_err("failed to reactivate vmm\n");
		goto err;
	}
	ret = 0;

	decon_to_init_param(decon, &p);
	decon_reg_init(DECON_INT, decon->pdata->dsi_mode, &p);
	decon_enable_eclk_idle_gate(DECON_INT, DECON_ECLK_IDLE_GATE_ENABLE);

#if defined(CONFIG_EXYNOS_DECON_DPU)
	dpu_reg_start(decon->lcd_info->xres, decon->lcd_info->yres);
	dpu_reg_restore();
#endif

	decon_to_psr_info(decon, &psr);
	if (decon->state != DECON_STATE_LPD_EXIT_REQ) {
	/* In case of resume*/
		if (decon->out_type == DECON_OUT_DSI)
			decon_reg_start(DECON_INT,
					decon->pdata->dsi_mode, &psr);
#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
		ret = v4l2_subdev_call(decon->output_sd, core,
				ioctl, DSIM_IOC_PKT_GO_ENABLE, NULL);
		if (ret)
			decon_err("Failed to call DSIM packet go enable!\n");
#endif
	}

#ifdef CONFIG_FB_WINDOW_UPDATE
	if (decon->need_update) {
		if (decon->state != DECON_STATE_LPD_EXIT_REQ) {
			decon->need_update = false;
			decon->update_win.x = 0;
			decon->update_win.y = 0;
			decon->update_win.w = decon->lcd_info->xres;
			decon->update_win.h = decon->lcd_info->yres;
		} else {
			decon_win_update_disp_config(decon, &decon->update_win);
		}
	}
#endif
	if (decon->pdata->psr_mode != DECON_VIDEO_MODE) {
		if (!decon->eint_en_status) {
			enable_irq(decon->irq);
			decon->eint_en_status = true;
		}

		decon_reg_set_int(DECON_INT, &psr, DSI_MODE_SINGLE, 1);
	}

	decon->state = DECON_STATE_ON;

	if (state != DECON_STATE_LPD_EXIT_REQ)
		decon_esd_enable_interrupt(decon);
err:
	exynos_ss_printk("%s:state %d: active %d:-\n", __func__,
				decon->state, pm_runtime_active(decon->dev));
	if (state != DECON_STATE_LPD_EXIT_REQ)
		mutex_unlock(&decon->output_lock);
	return ret;
}

int decon_disable(struct decon_device *decon)
{
	struct decon_psr_info psr;
	int ret = 0;
	unsigned long irq_flags;
	int state = decon->state;

	exynos_ss_printk("disable decon-%s, state(%d) cnt %d\n", "int",
				decon->state, pm_runtime_active(decon->dev));
	if (decon->out_type == DECON_OUT_TUI)
		decon_tui_protection(decon, false);

	if (decon->state != DECON_STATE_LPD_ENT_REQ) {
		decon_esd_disable_interrupt(decon);
		if (decon->esd.esd_wq)
			flush_workqueue(decon->esd.esd_wq);
	}

	if (decon->state != DECON_STATE_LPD_ENT_REQ)
		mutex_lock(&decon->output_lock);

	if (decon->state == DECON_STATE_OFF) {
		decon_info("decon already disabled\n");
		goto err;
	} else if (decon->state == DECON_STATE_LPD) {
#ifdef DECON_LPD_OPT
		decon_lcd_off(decon);
		decon_info("decon is LPD state. only lcd is off\n");
#endif
		goto err;
	}

	flush_kthread_worker(&decon->update_regs_worker);


	if (decon->pdata->psr_mode != DECON_VIDEO_MODE) {
		decon_reg_set_int(DECON_INT, &psr, DSI_MODE_SINGLE, 0);

		if ((decon->vsync_info.irq_refcount <= 0) &&
			decon->eint_en_status) {
			disable_irq(decon->irq);
			decon->eint_en_status = false;
		}
	}

	if (decon->out_type == DECON_OUT_DSI && decon->pdata->psr_mode == DECON_VIDEO_MODE) {
		/* stop output device (mipi-dsi) */
		ret = v4l2_subdev_call(decon->output_sd, video, s_stream, 0);
		if (ret)
			decon_err("stopping stream failed for %s\n", decon->output_sd->name);
	}

#if defined(CONFIG_EXYNOS_DECON_DPU)
	decon_reg_enable_apb_clk(DECON_INT, 1);
	dpu_reg_save();
#endif

	decon_to_psr_info(decon, &psr);
	decon_reg_stop(DECON_INT, decon->pdata->dsi_mode, &psr);
	decon_reg_clear_int(DECON_INT);
	decon_set_protected_content(decon, false);
	decon_enable_eclk_idle_gate(DECON_INT, DECON_ECLK_IDLE_GATE_DISABLE);
	iovmm_deactivate(decon->dev);

#if defined(CONFIG_EXYNOS_DECON_DPU)
	decon_reg_enable_apb_clk(DECON_INT, 1);
	dpu_reg_stop();
#endif

	/* Synchronize the decon->state with irq_handler */
	spin_lock_irqsave(&decon->slock, irq_flags);
	if (state == DECON_STATE_LPD_ENT_REQ)
		decon->state = DECON_STATE_LPD;
	spin_unlock_irqrestore(&decon->slock, irq_flags);
#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_put_sync(decon->dev);
#else
	decon_runtime_suspend(decon->dev);
#endif

	if (state == DECON_STATE_LPD_ENT_REQ) {
		ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
				DSIM_IOC_ENTER_ULPS, (unsigned long *)1);
		if (ret) {
			decon_err("%s: failed to enter ULPS state for %s\n",
					__func__, decon->output_sd->name);
			goto err;
		}
		decon->state = DECON_STATE_LPD;
	} else if (decon->out_type == DECON_OUT_DSI) {
		if (decon->pdata->psr_mode != DECON_VIDEO_MODE) {
			/* stop output device (mipi-dsi) */
			ret = v4l2_subdev_call(decon->output_sd, video, s_stream, 0);
			if (ret) {
				decon_err("stopping stream failed for %s\n",
						decon->output_sd->name);
				goto err;
			}
		}

		pm_relax(decon->dev);
		dev_dbg(decon->dev, "pm_relax");

		decon->state = DECON_STATE_OFF;
	}

	decon_set_qos(decon, NULL, true, true);
err:
	exynos_ss_printk("%s:state %d: active%d:-\n", __func__,
				decon->state, pm_runtime_active(decon->dev));
	if (state != DECON_STATE_LPD_ENT_REQ)
		mutex_unlock(&decon->output_lock);
	return ret;
}

static int decon_blank(int blank_mode, struct fb_info *info)
{
	struct decon_win *win = info->par;
	struct decon_device *decon = win->decon;
	int ret = 0;

	decon_info("%s ++ blank_mode : %d \n", __func__, blank_mode);
	decon_info("decon-%s %s mode: %dtype (0: DSI)\n", "int",
			blank_mode == FB_BLANK_UNBLANK ? "UNBLANK" : "POWERDOWN",
			decon->out_type);

	decon_lpd_block_exit(decon);

#ifdef CONFIG_USE_VSYNC_SKIP
	decon_extra_vsync_wait_set(ERANGE);
#endif /* CONFIG_USE_VSYNC_SKIP */

	switch (blank_mode) {
	case FB_BLANK_POWERDOWN:
	case FB_BLANK_NORMAL:
		DISP_SS_EVENT_LOG(DISP_EVT_BLANK, &decon->sd, ktime_set(0, 0));
		ret = decon_disable(decon);
		if (ret) {
			decon_err("failed to disable decon\n");
			goto blank_exit;
		}
#ifdef CONFIG_POWERSUSPEND
		set_power_suspend_state_panel_hook(POWER_SUSPEND_ACTIVE);
#endif
		break;
	case FB_BLANK_UNBLANK:
		DISP_SS_EVENT_LOG(DISP_EVT_UNBLANK, &decon->sd, ktime_set(0, 0));
		ret = decon_enable(decon);
		if (ret) {
			decon_err("failed to enable decon\n");
			goto blank_exit;
		}
#ifdef CONFIG_POWERSUSPEND
		set_power_suspend_state_panel_hook(POWER_SUSPEND_INACTIVE);
#endif
		break;
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	default:
		ret = -EINVAL;
	}

blank_exit:
	decon_lpd_unblock(decon);
	decon_info("%s -- blank_mode : %d, %d\n", __func__, blank_mode, ret);
	return ret;
}

/* ---------- FB_IOCTL INTERFACE ----------- */
static void decon_activate_vsync(struct decon_device *decon)
{
	struct decon_psr_info psr;
	int prev_refcount, ret;

	mutex_lock(&decon->vsync_info.irq_lock);

	prev_refcount = decon->vsync_info.irq_refcount++;
	if (!prev_refcount) {
		if (decon->pdata->psr_mode == DECON_VIDEO_MODE) {
			decon_to_psr_info(decon, &psr);
			decon_reg_set_int(DECON_INT, &psr, DSI_MODE_SINGLE, 1);
			ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
					DSIM_IOC_VSYNC, (unsigned long *)1);
			if (ret)
				decon_err("%s: failed to enable dsim vsync int %s\n",
						__func__, decon->output_sd->name);
		}
		DISP_SS_EVENT_LOG(DISP_EVT_ACT_VSYNC, &decon->sd, ktime_set(0, 0));
	}

	mutex_unlock(&decon->vsync_info.irq_lock);
}

static void decon_deactivate_vsync(struct decon_device *decon)
{
	struct decon_psr_info psr;
	int new_refcount, ret;

	mutex_lock(&decon->vsync_info.irq_lock);

	new_refcount = --decon->vsync_info.irq_refcount;
	WARN_ON(new_refcount < 0);
	if (!new_refcount) {
		if (decon->pdata->psr_mode == DECON_VIDEO_MODE) {
			decon_to_psr_info(decon, &psr);
			decon_reg_set_int(DECON_INT, &psr, DSI_MODE_SINGLE, 0);
			ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
					DSIM_IOC_VSYNC, (unsigned long *)0);
			if (ret)
				decon_err("%s: failed to disable dsim vsync int %s\n",
						__func__, decon->output_sd->name);
		}
		DISP_SS_EVENT_LOG(DISP_EVT_DEACT_VSYNC, &decon->sd, ktime_set(0, 0));
	}

	mutex_unlock(&decon->vsync_info.irq_lock);
}

int decon_wait_for_vsync(struct decon_device *decon, u32 timeout)
{
	ktime_t timestamp;
	int ret;

	if (decon->pdata->psr_mode == DECON_MIPI_COMMAND_MODE) {
		if (decon->ignore_vsync)
			goto wait_exit;
	}

	timestamp = decon->vsync_info.timestamp;
	if (decon->pdata->trig_mode == DECON_SW_TRIG || decon->pdata->psr_mode == DECON_VIDEO_MODE)
		decon_activate_vsync(decon);

	if (timeout) {
		ret = wait_event_interruptible_timeout(decon->vsync_info.wait,
				!ktime_equal(timestamp,
						decon->vsync_info.timestamp),
				msecs_to_jiffies(timeout));
	} else {
		ret = wait_event_interruptible(decon->vsync_info.wait,
				!ktime_equal(timestamp,
						decon->vsync_info.timestamp));
	}

	if (decon->pdata->trig_mode == DECON_SW_TRIG || decon->pdata->psr_mode == DECON_VIDEO_MODE)
		decon_deactivate_vsync(decon);

	if (decon->pdata->psr_mode == DECON_MIPI_COMMAND_MODE) {
		if (decon->ignore_vsync)
			goto wait_exit;
	}

	if (timeout && ret == 0) {
		decon_err("decon wait for vsync timeout");
		return -ETIMEDOUT;
	}

wait_exit:
	return 0;
}

int decon_set_window_position(struct fb_info *info,
				struct decon_user_window user_window)
{
	return 0;
}

int decon_set_plane_alpha_blending(struct fb_info *info,
				struct s3c_fb_user_plane_alpha user_alpha)
{
	return 0;
}

int decon_set_chroma_key(struct fb_info *info,
			struct s3c_fb_user_chroma user_chroma)
{
	return 0;
}

int decon_set_vsync_int(struct fb_info *info, bool active)
{
	struct decon_win *win = info->par;
	struct decon_device *decon = win->decon;
	bool prev_active = decon->vsync_info.active;

	decon->vsync_info.active = active;
	smp_wmb();

	if (active && !prev_active)
		decon_activate_vsync(decon);
	else if (!active && prev_active)
		decon_deactivate_vsync(decon);

	return 0;
}

static unsigned int decon_map_ion_handle(struct decon_device *decon,
		struct device *dev, struct decon_dma_buf_data *dma,
		struct ion_handle *ion_handle, struct dma_buf *buf, int win_no)
{
	dma->fence = NULL;
	dma->dma_buf = buf;

	dma->attachment = dma_buf_attach(dma->dma_buf, dev);
	if (IS_ERR_OR_NULL(dma->attachment)) {
		decon_err("dma_buf_attach() failed: %ld\n",
				PTR_ERR(dma->attachment));
		goto err_buf_map_attach;
	}

	dma->sg_table = dma_buf_map_attachment(dma->attachment,
			DMA_TO_DEVICE);
	if (IS_ERR_OR_NULL(dma->sg_table)) {
		decon_err("dma_buf_map_attachment() failed: %ld\n",
				PTR_ERR(dma->sg_table));
		goto err_buf_map_attachment;
	}

	dma->dma_addr = ion_iovmm_map(dma->attachment, 0,
			dma->dma_buf->size, DMA_TO_DEVICE, win_no);
	if (!dma->dma_addr || IS_ERR_VALUE(dma->dma_addr)) {
		decon_err("iovmm_map() failed: %pa\n", &dma->dma_addr);
		goto err_iovmm_map;
	}

	exynos_ion_sync_dmabuf_for_device(dev, dma->dma_buf, dma->dma_buf->size,
			DMA_TO_DEVICE);

	dma->ion_handle = ion_handle;

	return dma->dma_buf->size;

err_iovmm_map:
	dma_buf_unmap_attachment(dma->attachment, dma->sg_table,
			DMA_TO_DEVICE);
err_buf_map_attachment:
	dma_buf_detach(dma->dma_buf, dma->attachment);
err_buf_map_attach:
	return 0;
}


static void decon_free_dma_buf(struct decon_device *decon,
		struct decon_dma_buf_data *dma)
{
	if (!dma->dma_addr)
		return;

	if (dma->fence)
		sync_fence_put(dma->fence);

	ion_iovmm_unmap(dma->attachment, dma->dma_addr);

	dma_buf_unmap_attachment(dma->attachment, dma->sg_table,
			DMA_TO_DEVICE);

	dma_buf_detach(dma->dma_buf, dma->attachment);
	dma_buf_put(dma->dma_buf);
	ion_free(decon->ion_client, dma->ion_handle);
	memset(dma, 0, sizeof(struct decon_dma_buf_data));
}

static int decon_get_memory_plane_cnt(enum decon_pixel_format format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
	case DECON_PIXEL_FORMAT_RGBA_5551:
	case DECON_PIXEL_FORMAT_RGB_565:
		return 1;

	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV21M_FULL:
		return 2;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
		return 1;

	default:
		return -1;
	}

}

static bool decon_is_plane_offset_calc_required(
				enum decon_pixel_format format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
		return true;
	default:
		return false;
	}
}

static void decon_calc_plane_offset(struct decon_win_config *config,
	struct decon_dma_buf_data *dma_buf_data)
{
	unsigned int stride = config->src.f_w;
	unsigned int vstride = config->src.f_h;

	switch (config->format) {
	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
		dma_buf_data[1].dma_addr = dma_buf_data[0].dma_addr +
				(stride * vstride);
		dma_buf_data[2].dma_addr = 0;
		break;
	default:
		break;
	}

	decon_dbg("decon_calc_plane_offst f:%d, dma:%llu, %llu, %llu\n",
			config->format, dma_buf_data[0].dma_addr,
			dma_buf_data[1].dma_addr, dma_buf_data[2].dma_addr);
}

static void decon_set_protected_content(struct decon_device *decon,
				bool enable)
{
	int ret = 0;
	bool en;
	int retry = 5;

	if (enable)
		en = decon->cur_protection_bitmask ? 1 : 0;
	else
		en = 0;

	if (en == decon->prev_protection_status)
		return;

	if (decon->pdata->psr_mode == DECON_VIDEO_MODE) {
		do {
			decon_reg_update_standalone(DECON_INT);
			decon_reg_per_frame_off(0);
			ret = decon_reg_wait_linecnt_is_zero_timeout(DECON_INT , 0, 20 * 1000);
			if (ret)
				decon_warn("[%s] linecnt_is_zero timeout\n", __func__);
			else
				break;
		} while (retry--);
	} else {
		if (decon_reg_wait_linecnt_is_zero_timeout(DECON_INT, 0, 35 * 1000))
			DISP_SS_EVENT_LOG(DISP_EVT_LINECNT_TIMEOUT,
					&decon->sd, ktime_set(0, 0));
	}

	ret = exynos_smc(SMC_PROTECTION_SET, 0, DRM_DEV_DECON, en);
	if (!ret)
		dev_warn(decon->dev,
			"decon protection Enable failed. ret(%d)\n", ret);
	else
		dev_dbg(decon->dev, "DRM %s\n", en ? "enabled" : "disabled");

	if (en)
		DISP_SS_EVENT_LOG(DISP_EVT_ACT_PROT, &decon->sd, ktime_set(0, 0));
	else
		DISP_SS_EVENT_LOG(DISP_EVT_DEACT_PROT, &decon->sd, ktime_set(0, 0));

	decon->prev_protection_status = en;
}

static inline int decon_set_alpha_blending(struct decon_win_config *win_config,
		struct decon_reg_data *regs, int win_no, int transp_length)
{
	u8 alpha0, alpha1;

	if ((win_config->plane_alpha > 0) && (win_config->plane_alpha < 0xFF)) {
		alpha0 = win_config->plane_alpha;
		alpha1 = 0;
	} else if (transp_length == 1 &&
			win_config->blending == DECON_BLENDING_NONE) {
		alpha0 = 0xff;
		alpha1 = 0xff;
	} else {
		alpha0 = 0;
		alpha1 = 0xff;
	}
	regs->vidosd_c[win_no] = vidosd_c(alpha0, alpha0, alpha0);
	regs->vidosd_d[win_no] = vidosd_d(alpha1, alpha1, alpha1);

	if (win_no) {
		if ((win_config->plane_alpha > 0) && (win_config->plane_alpha < 0xFF)) {
			if (transp_length) {
				if (win_config->blending != DECON_BLENDING_NONE)
					regs->wincon[win_no] |= WINCON_ALPHA_MUL;
			} else {
				regs->wincon[win_no] &= (~WINCON_ALPHA_SEL);
				if (win_config->blending == DECON_BLENDING_PREMULT)
					win_config->blending = DECON_BLENDING_COVERAGE;
			}
		}
		regs->blendeq[win_no - 1] = blendeq(win_config->blending,
					transp_length, win_config->plane_alpha);
	}

	return 0;
}

static int decon_set_win_buffer(struct decon_device *decon, struct decon_win *win,
		struct decon_win_config *win_config, struct decon_reg_data *regs)
{
	struct ion_handle *handle;
	struct fb_var_screeninfo prev_var = win->fbinfo->var;
	struct dma_buf *buf[MAX_BUF_PLANE_CNT];
	struct decon_dma_buf_data dma_buf_data[MAX_BUF_PLANE_CNT];
	unsigned short win_no = win->index;
	int ret, i;
	size_t buf_size = 0, window_size;
	int plane_cnt;
	u32 format;

	if (win_config->format >= DECON_PIXEL_FORMAT_MAX) {
		decon_err("unknown pixel format %u\n", win_config->format);
		return -EINVAL;
	}

	if (win_config->blending >= DECON_BLENDING_MAX) {
		decon_err("unknown blending %u\n", win_config->blending);
		return -EINVAL;
	}

	if (win_no == 0 && win_config->blending != DECON_BLENDING_NONE) {
		decon_err("blending not allowed on window 0\n");
		return -EINVAL;
	}

	if (win_config->dst.w == 0 || win_config->dst.h == 0 ||
			win_config->dst.x < 0 || win_config->dst.y < 0) {
		decon_err("win[%d] size is abnormal (w:%d, h:%d, x:%d, y:%d)\n",
				win_no, win_config->dst.w, win_config->dst.h,
				win_config->dst.x, win_config->dst.y);
		return -EINVAL;
	}

	format = win_config->format;

	win->fbinfo->var.red.length = decon_red_length(format);
	win->fbinfo->var.red.offset = decon_red_offset(format);
	win->fbinfo->var.green.length = decon_green_length(format);
	win->fbinfo->var.green.offset = decon_green_offset(format);
	win->fbinfo->var.blue.length = decon_blue_length(format);
	win->fbinfo->var.blue.offset = decon_blue_offset(format);
	win->fbinfo->var.transp.length =
			decon_transp_length(format);
	win->fbinfo->var.transp.offset =
			decon_transp_offset(format);
	win->fbinfo->var.bits_per_pixel = win->fbinfo->var.red.length +
			win->fbinfo->var.green.length +
			win->fbinfo->var.blue.length +
			win->fbinfo->var.transp.length +
			decon_padding(format);

	if (format <= DECON_PIXEL_FORMAT_RGB_565) {
		win->fbinfo->var.bits_per_pixel = win->fbinfo->var.red.length +
			win->fbinfo->var.green.length +
			win->fbinfo->var.blue.length +
			win->fbinfo->var.transp.length +
			decon_padding(format);
	} else {
		win->fbinfo->var.bits_per_pixel = 12;
	}

	if (win_config->dst.w * win->fbinfo->var.bits_per_pixel / 8 < 128) {
		decon_err("window wide < 128bytes, width = %u, bpp = %u)\n",
				win_config->dst.w,
				win->fbinfo->var.bits_per_pixel);
		ret = -EINVAL;
		goto err_invalid;
	}

	if (win_config->src.f_w < win_config->dst.w) {
		decon_err("f_width(%u) < width(%u),\
			bpp = %u\n", win_config->src.f_w,
			win_config->dst.w,
			win->fbinfo->var.bits_per_pixel);
		ret = -EINVAL;
		goto err_invalid;
	}

	if ((format <= DECON_PIXEL_FORMAT_RGB_565) &&
			(decon_validate_x_alignment(decon, win_config->dst.x,
			win_config->dst.w,
			win->fbinfo->var.bits_per_pixel) == false)) {
		ret = -EINVAL;
		goto err_invalid;
	}

	plane_cnt = decon_get_memory_plane_cnt(win_config->format);
	for (i = 0; i < plane_cnt; ++i) {
		handle = ion_import_dma_buf(decon->ion_client, win_config->fd_idma[i]);
		if (IS_ERR(handle)) {
			decon_err("failed to import fd\n");
			ret = PTR_ERR(handle);
			goto err_invalid;
		}

		buf[i] = dma_buf_get(win_config->fd_idma[i]);
		if (IS_ERR_OR_NULL(buf[i])) {
			decon_err("dma_buf_get() failed: %ld\n", PTR_ERR(buf[i]));
			ret = PTR_ERR(buf[i]);
			goto err_buf_get;
		}
		buf_size = decon_map_ion_handle(decon, decon->dev,
				&dma_buf_data[i], handle, buf[i], win_no);

		if (!buf_size) {
			ret = -ENOMEM;
			goto err_map;
		}
		win_config->vpp_parm.addr[i] = dma_buf_data[i].dma_addr;
		handle = NULL;
		buf[i] = NULL;
	}
	if (win_config->fence_fd >= 0) {
		dma_buf_data[0].fence = sync_fence_fdget(win_config->fence_fd);
		if (!dma_buf_data[0].fence) {
			decon_err("failed to import fence fd\n");
			ret = -EINVAL;
			goto err_offset;
		}
		decon_dbg("%s(%d): fence_fd(%d), fence(%lx)\n", __func__, __LINE__,
				win_config->fence_fd, (ulong)dma_buf_data[0].fence);
	}

	if (format <= DECON_PIXEL_FORMAT_RGB_565) {
		window_size = win_config->dst.w * win_config->dst.h *
			win->fbinfo->var.bits_per_pixel / 8;
		if (window_size > buf_size) {
			decon_err("window size(%zu) > buffer size(%zu)\n",
					window_size, buf_size);
			ret = -EINVAL;
			goto err_offset;
		}
	}

	win->fbinfo->fix.smem_start = dma_buf_data[0].dma_addr;
	win->fbinfo->fix.smem_len = buf_size;
	win->fbinfo->var.xres = win_config->dst.w;
	win->fbinfo->var.xres_virtual = win_config->dst.f_w;
	win->fbinfo->var.yres = win_config->dst.h;
	win->fbinfo->var.yres_virtual = win_config->dst.f_h;
	win->fbinfo->var.xoffset = win_config->src.x;
	win->fbinfo->var.yoffset = win_config->src.y;

	win->fbinfo->fix.line_length = win_config->src.f_w *
			win->fbinfo->var.bits_per_pixel / 8;
	win->fbinfo->fix.xpanstep = fb_panstep(win_config->dst.w,
			win->fbinfo->var.xres_virtual);
	win->fbinfo->fix.ypanstep = fb_panstep(win_config->dst.h,
			win->fbinfo->var.yres_virtual);

	plane_cnt = decon_get_memory_plane_cnt(win_config->format);
	for (i = 0; i < plane_cnt; ++i)
		regs->dma_buf_data[win_no][i] = dma_buf_data[i];
	if (decon_is_plane_offset_calc_required(win_config->format))
		decon_calc_plane_offset(win_config,
		&regs->dma_buf_data[win_no][0]);

	regs->buf_start[win_no] = win->fbinfo->fix.smem_start;

	regs->vidosd_a[win_no] = vidosd_a(win_config->dst.x, win_config->dst.y);
	regs->vidosd_b[win_no] = vidosd_b(win_config->dst.x, win_config->dst.y,

	win_config->dst.w, win_config->dst.h);
	regs->whole_w[win_no] = win_config->src.f_w;
	regs->whole_h[win_no] = win_config->src.f_h;
	regs->offset_x[win_no] = win_config->src.x;
	regs->offset_y[win_no] = win_config->src.y;

	regs->wincon[win_no] = wincon(win->fbinfo->var.bits_per_pixel,
			win->fbinfo->var.transp.length, format);
	regs->wincon[win_no] |= decon_rgborder(format);
	regs->protection[win_no] = win_config->protection;

	decon_set_alpha_blending(win_config, regs, win_no,
				win->fbinfo->var.transp.length);

	decon_dbg("win[%d] SRC:(%d,%d) %dx%d  DST:(%d,%d) %dx%d\n", win_no,
			win_config->src.x, win_config->src.y,
			win_config->src.f_w, win_config->src.f_h,
			win_config->dst.x, win_config->dst.y,
			win_config->dst.w, win_config->dst.h);

	return 0;

err_offset:
	for (i = 0; i < plane_cnt; ++i)
		decon_free_dma_buf(decon, &dma_buf_data[i]);
err_map:
	for (i = 0; i < plane_cnt; ++i)
		dma_buf_put(buf[i]);
err_buf_get:
	if (handle)
		ion_free(decon->ion_client, handle);
err_invalid:
	win->fbinfo->var = prev_var;
	return ret;
}

#ifdef CONFIG_FB_WINDOW_UPDATE
static inline void decon_update_2_full(struct decon_device *decon,
			struct decon_reg_data *regs,
			struct decon_lcd *lcd_info,
			int flag)
{
	if (flag)
		regs->need_update = true;

	decon->need_update = false;
	decon->update_win.x = 0;
	decon->update_win.y = 0;
	decon->update_win.w = lcd_info->xres;
	decon->update_win.h = lcd_info->yres;
	regs->update_win.w = lcd_info->xres;
	regs->update_win.h = lcd_info->yres;
	decon_win_update_dbg("[WIN_UPDATE]update2org: [%d %d %d %d]\n",
			decon->update_win.x, decon->update_win.y, decon->update_win.w, decon->update_win.h);
	return;

}

static void decon_calibrate_win_update_size(struct decon_device *decon,
		struct decon_win_config *win_config,
		struct decon_win_config *update_config)
{
	if (update_config->state != DECON_WIN_STATE_UPDATE)
		return;

	if ((update_config->dst.x < 0) ||
			(update_config->dst.y < 0)) {
		update_config->state = DECON_WIN_STATE_DISABLED;
		return;
	}

	if ((decon->update_win.w == 0) ||
			(decon->update_win.h == 0)) {
		update_config->state = DECON_WIN_STATE_DISABLED;
		return;
	}

	decon_win_update_dbg("[WIN_UPDATE]get_config: [%d %d %d %d]\n",
			update_config->dst.x, update_config->dst.y,
			update_config->dst.w, update_config->dst.h);

	if (update_config->dst.x & 0x7) {
		update_config->dst.w += update_config->dst.x & 0x7;
		update_config->dst.x = update_config->dst.x & (~0x7);
	}
	update_config->dst.w = ((update_config->dst.w + 7) & (~0x7));
	if (update_config->dst.x + update_config->dst.w > decon->lcd_info->xres) {
		update_config->dst.w = decon->lcd_info->xres;
		update_config->dst.x = 0;
	}

	return;
}

static void decon_set_win_update_config(struct decon_device *decon,
		struct decon_win_config *win_config,
		struct decon_reg_data *regs)
{
	int i;
	struct decon_win_config *update_config = &win_config[DECON_WIN_UPDATE_IDX];
	struct decon_win_config temp_config;
	struct decon_rect r1, r2;
	struct decon_lcd *lcd_info = decon->lcd_info;

	decon_calibrate_win_update_size(decon, win_config, update_config);

	/* if the current mode is not WINDOW_UPDATE, set the config as WINDOW_UPDATE */
	if ((update_config->state == DECON_WIN_STATE_UPDATE) &&
			((update_config->dst.x != decon->update_win.x) ||
			 (update_config->dst.y != decon->update_win.y) ||
			 (update_config->dst.w != decon->update_win.w) ||
			 (update_config->dst.h != decon->update_win.h))) {
		decon->update_win.x = update_config->dst.x;
		decon->update_win.y = update_config->dst.y;
		decon->update_win.w = update_config->dst.w;
		decon->update_win.h = update_config->dst.h;
		decon->need_update = true;
		regs->need_update = true;
		regs->update_win.x = update_config->dst.x;
		regs->update_win.y = update_config->dst.y;
		regs->update_win.w = update_config->dst.w;
		regs->update_win.h = update_config->dst.h;

		decon_win_update_dbg("[WIN_UPDATE]need_update_1: [%d %d %d %d]\n",
				update_config->dst.x, update_config->dst.y, update_config->dst.w, update_config->dst.h);
	} else if (decon->need_update &&
			(update_config->state != DECON_WIN_STATE_UPDATE)) {
		/* Platform requested for normal mode, switch to normal mode from WINDOW_UPDATE */
		decon_update_2_full(decon, regs, lcd_info, true);
		return;
	} else if (decon->need_update) {
		/* It is just for debugging info */
		regs->update_win.x = update_config->dst.x;
		regs->update_win.y = update_config->dst.y;
		regs->update_win.w = update_config->dst.w;
		regs->update_win.h = update_config->dst.h;
	}

	if (update_config->state != DECON_WIN_STATE_UPDATE)
		return;

	r1.left = update_config->dst.x;
	r1.top = update_config->dst.y;
	r1.right = r1.left + update_config->dst.w - 1;
	r1.bottom = r1.top + update_config->dst.h - 1;

	for (i = 0; i < decon->pdata->max_win; i++) {
		struct decon_win_config *config = &win_config[i];
		if (config->state == DECON_WIN_STATE_DISABLED)
			continue;
		r2.left = config->dst.x;
		r2.top = config->dst.y;
		r2.right = r2.left + config->dst.w - 1;
		r2.bottom = r2.top + config->dst.h - 1;
		if (!decon_intersect(&r1, &r2)) {
			config->state = DECON_WIN_STATE_DISABLED;
			continue;
		}
		memcpy(&temp_config, config, sizeof(struct decon_win_config));
		if (update_config->dst.x > config->dst.x)
			config->dst.w = min(update_config->dst.w,
					config->dst.x + config->dst.w - update_config->dst.x);
		else if(update_config->dst.x + update_config->dst.w < config->dst.x + config->dst.w)
			config->dst.w = min(config->dst.w,
					update_config->dst.w + update_config->dst.x - config->dst.x);

		if (update_config->dst.y > config->dst.y)
			config->dst.h = min(update_config->dst.h,
					config->dst.y + config->dst.h - update_config->dst.y);
		else if(update_config->dst.y + update_config->dst.h < config->dst.y + config->dst.h)
			config->dst.h = min(config->dst.h,
					update_config->dst.h + update_config->dst.y - config->dst.y);

		config->dst.x = max(config->dst.x - update_config->dst.x, 0);
		config->dst.y = max(config->dst.y - update_config->dst.y, 0);

		if (update_config->dst.y > temp_config.dst.y) {
			config->src.y += (update_config->dst.y - temp_config.dst.y);
		}

		if (update_config->dst.x > temp_config.dst.x) {
			config->src.x += (update_config->dst.x - temp_config.dst.x);
		}
		config->src.w = config->dst.w;
		config->src.h = config->dst.h;

		if (regs->need_update == true)
			decon_win_update_dbg("[WIN_UPDATE]win_idx %d: idma_type %d:,"
			"dst[%d %d %d %d] -> [%d %d %d %d], src[%d %d %d %d] -> [%d %d %d %d]\n",
				i, temp_config.idma_type,
				temp_config.dst.x, temp_config.dst.y, temp_config.dst.w, temp_config.dst.h,
				config->dst.x, config->dst.y, config->dst.w, config->dst.h,
				temp_config.src.x, temp_config.src.y, temp_config.src.w, temp_config.src.h,
				config->src.x, config->src.y, config->src.w, config->src.h);
	}

	return;
}
#endif

void decon_reg_chmap_validate(struct decon_device *decon, struct decon_reg_data *regs)
{
	unsigned short i, bitmap = 0;

	for (i = 0; i < decon->pdata->max_win; i++) {
		if ((regs->wincon[i] & WINCON_ENWIN) &&
			!(regs->winmap[i] & WIN_MAP_MAP)) {
			if (bitmap & (1 << regs->win_config[i].idma_type)) {
				decon_warn("Channel-%d is mapped to multiple windows\n",
					regs->win_config[i].idma_type);
				regs->wincon[i] &= (~WINCON_ENWIN);
			}
			bitmap |= 1 << regs->win_config[i].idma_type;
		}
	}
}

#ifdef CONFIG_FB_WINDOW_UPDATE
static int decon_reg_set_win_update_config(struct decon_device *decon, struct decon_reg_data *regs)
{
	int ret = 0;

	if (regs->need_update) {
		decon_reg_ddi_partial_cmd(decon, &regs->update_win);
		ret = decon_win_update_disp_config(decon, &regs->update_win);
	}
	return ret;
}
#endif

static void __decon_update_regs(struct decon_device *decon, struct decon_reg_data *regs)
{
	unsigned short i, j;
	struct decon_regs_data win_regs;
	struct decon_psr_info psr;
	int plane_cnt;
#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	int ret;
#endif

	memset(&win_regs, 0, sizeof(struct decon_regs_data));

	decon->cur_protection_bitmask = 0;

	if (decon->pdata->trig_mode == DECON_HW_TRIG)
		decon_reg_set_trigger(DECON_INT, decon->pdata->dsi_mode,
				decon->pdata->trig_mode, DECON_TRIG_DISABLE);

	for (i = 0; i < decon->pdata->max_win; i++)
		decon_reg_shadow_protect_win(DECON_INT,
				decon->windows[i]->index, 1);

	decon_reg_chmap_validate(decon, regs);

#ifdef CONFIG_FB_WINDOW_UPDATE
	if (decon->out_type == DECON_OUT_DSI)
		decon_reg_set_win_update_config(decon, regs);
#endif

	for (i = 0; i < decon->pdata->max_win; i++) {
		decon_to_regs_param(&win_regs, regs, i);
		decon_reg_set_regs_data(DECON_INT, i, &win_regs);
		decon->cur_protection_bitmask |=
			regs->protection[i] << regs->win_config[i].idma_type;
		plane_cnt = decon_get_memory_plane_cnt(regs->win_config[i].format);
		for (j = 0; j < MAX_BUF_PLANE_CNT; ++j) {
			if (j < plane_cnt)
				decon->windows[i]->dma_buf_data[j] =
					regs->dma_buf_data[i][j];
			else
				memset(&decon->windows[i]->dma_buf_data[j], 0,
				sizeof(struct decon_dma_buf_data));
		}
		if (IS_ENABLED(CONFIG_DECON_BLOCKING_MODE))
			decon_enable_blocking_mode(decon, regs, i);
	}

	for (i = 0; i < decon->pdata->max_win; i++)
		decon_reg_shadow_protect_win(DECON_INT,
				decon->windows[i]->index, 0);

	decon_set_protected_content(decon, true);

	decon_to_psr_info(decon, &psr);
	decon_reg_start(DECON_INT, decon->pdata->dsi_mode, &psr);
#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
				DSIM_IOC_PKT_GO_ENABLE, NULL);
	if (ret)
		decon_err("Failed to call DSIM packet go enable in %s!\n",
				__func__);
#endif
}

static void decon_fence_wait(struct sync_fence *fence)
{
	int err = sync_fence_wait(fence, 900);
	if (err >= 0)
		return;

	if (err < 0)
		decon_warn("error waiting on acquire fence: %d\n", err);
}

#ifdef CONFIG_DECON_DEVFREQ
void decon_set_qos(struct decon_device *decon, struct decon_reg_data *regs,
			bool is_after, bool is_default_qos)
{
	u64 req_bandwidth;

	req_bandwidth = regs ? (is_default_qos ? 0 : regs->bandwidth) :
			(decon->max_win_bw * 3);

	if (decon->prev_bw == req_bandwidth)
		return;

	if ((is_after && (decon->prev_bw > req_bandwidth)) ||
	    (!is_after && (decon->prev_bw < req_bandwidth))) {
		exynos7_update_media_scenario(TYPE_DECON_INT, req_bandwidth, 0);
		decon->prev_bw = req_bandwidth;
	}

	decon_dbg("decon bandwidth(%llu)\n", req_bandwidth);
}
#else
void decon_set_qos(struct decon_device *decon, struct decon_reg_data *regs,
			bool is_after, bool is_default_qos)
{
	return;
}
#endif

static int decon_prevent_size_mismatch
	(struct decon_device *decon, int dsi_idx, unsigned long timeout)
{
	unsigned long delay_time = 100;
	unsigned long cnt = timeout / delay_time;
	u32 decon_line, dsim_line;
	u32 decon_hoz, dsim_hoz;
	u32 need_save = true;
	struct disp_ss_size_info info;

	if (decon->pdata->psr_mode == DECON_VIDEO_MODE)
		return 0;

	while (decon_reg_get_vstatus(DECON_INT, dsi_idx) ==
			VIDCON1_VSTATUS_IDLE && --cnt) {
		/* Check a DECON and DSIM size mismatch */
		decon_line = decon_reg_get_lineval(DECON_INT, dsi_idx, decon->lcd_info);
		dsim_line = dsim_reg_get_lineval(dsi_idx);

		decon_hoz = decon_reg_get_hozval(DECON_INT, dsi_idx, decon->lcd_info);
		dsim_hoz = dsim_reg_get_hozval(dsi_idx);

		if (decon_line == dsim_line && decon_hoz == dsim_hoz)
			goto wait_done;

		if (need_save) {
			/* TODO: Save a err data */
			info.w_in = decon_hoz;
			info.h_in = decon_line;
			info.w_out = dsim_hoz;
			info.h_out = dsim_line;
			DISP_SS_EVENT_SIZE_ERR_LOG(&decon->sd, &info);
			need_save = false;
		}

		udelay(delay_time);
	}

	if (!cnt) {
		decon_err("size mis-match, TRIGCON:0x%x decon_line:%d,	\
				dsim_line:%d, decon_hoz:%d, dsim_hoz:%d\n",
				decon_read(DECON_INT, TRIGCON),
				decon_line, dsim_line, decon_hoz, dsim_hoz);
	}
wait_done:
	return 0;
}

static void decon_update_regs(struct decon_device *decon, struct decon_reg_data *regs)
{
	struct decon_dma_buf_data old_dma_bufs[decon->pdata->max_win][MAX_BUF_PLANE_CNT];
	int i, j;

#ifdef CONFIG_USE_VSYNC_SKIP
	int vsync_wait_cnt = 0;
#endif /* CONFIG_USE_VSYNC_SKIP */

#ifdef CONFIG_LCD_HMT
	struct dsim_device *dsim = NULL;
	if (decon->out_type == DECON_OUT_DSI)
		dsim = container_of(decon->output_sd, struct dsim_device, sd);
#endif


	if (decon->state == DECON_STATE_LPD)
		decon_exit_lpd(decon);

	memset(old_dma_bufs, 0, sizeof(struct decon_dma_buf_data) *
			decon->pdata->max_win *
			MAX_BUF_PLANE_CNT);

	for (i = 0; i < decon->pdata->max_win; i++) {
		for (j = 0; j < MAX_BUF_PLANE_CNT; ++j)
			old_dma_bufs[i][j] = decon->windows[i]->dma_buf_data[j];

		if (regs->dma_buf_data[i][0].fence) {
			decon_fence_wait(regs->dma_buf_data[i][0].fence);
		}
	}

	if (decon->prev_bw != regs->bandwidth)
		decon_set_qos(decon, regs, false, false);

#ifdef CONFIG_USE_VSYNC_SKIP
	vsync_wait_cnt = decon_extra_vsync_wait_get();
	decon_extra_vsync_wait_set(0);

	if (vsync_wait_cnt < ERANGE && regs->num_of_window <= 2) {
		while ((vsync_wait_cnt--) > 0) {
			if((decon_extra_vsync_wait_get() >= ERANGE)) {
				decon_extra_vsync_wait_set(0);
				break;
			}

			decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
		}
	}
#endif /* CONFIG_USE_VSYNC_SKIP */
	__decon_update_regs(decon, regs);

	if ((decon->disp_ss_log_unmask & EVT_TYPE_WININFO))
		DISP_SS_EVENT_LOG_UPDATE_PARAMS(&decon->sd, regs);
	else
		DISP_SS_EVENT_LOG_WINCON(&decon->sd, regs);

	decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
	if (decon_reg_wait_for_update_timeout(DECON_INT, 300 * 1000) < 0) {
		DISP_SS_EVENT_LOG(DISP_EVT_UPDATE_TIMEOUT, &decon->sd,
				ktime_set(0, 0));
		decon_dump(decon);
	}

	/* prevent size mis-matching after decon update clear */
	decon_prevent_size_mismatch(decon, 0, 50 * 1000); /* 50ms */

	/* clear I80 Framedone pending interrupt */
	decon_write_mask(DECON_INT, VIDINTCON1, ~0, VIDINTCON1_INT_I80);
	decon->frame_done_cnt_target = decon->frame_done_cnt_cur + 1;
#ifdef CONFIG_LCD_HMT
	if (decon->out_type == DECON_OUT_DSI && dsim->priv.hmt_on == 0) {
		if (decon->pdata->trig_mode == DECON_HW_TRIG)
			decon_reg_set_trigger(DECON_INT, decon->pdata->dsi_mode,
					decon->pdata->trig_mode, DECON_TRIG_DISABLE);
	}
#else
	if (decon->pdata->trig_mode == DECON_HW_TRIG)
		decon_reg_set_trigger(DECON_INT, decon->pdata->dsi_mode,
				decon->pdata->trig_mode, DECON_TRIG_DISABLE);
#endif

	DISP_SS_EVENT_LOG(DISP_EVT_TRIG_MASK, &decon->sd, ktime_set(0, 0));
	decon->trig_mask_timestamp =  ktime_get();

	for (i = 0; i < decon->pdata->max_win; i++) {
		for (j = 0; j < MAX_BUF_PLANE_CNT; ++j)
			decon_free_dma_buf(decon, &old_dma_bufs[i][j]);
	}

	sw_sync_timeline_inc(decon->timeline, 1);

	if (decon->prev_bw != regs->bandwidth)
		decon_set_qos(decon, regs, true, false);
}

static void decon_update_regs_handler(struct kthread_work *work)
{
	struct decon_device *decon =
			container_of(work, struct decon_device, update_regs_work);
	struct decon_reg_data *data, *next;
	struct list_head saved_list;

	if (decon->state == DECON_STATE_LPD)
		decon_warn("%s: LPD state: %d\n", __func__, decon_get_lpd_block_cnt(decon));

	mutex_lock(&decon->update_regs_list_lock);
	saved_list = decon->update_regs_list;
	list_replace_init(&decon->update_regs_list, &saved_list);
	mutex_unlock(&decon->update_regs_list_lock);

	list_for_each_entry_safe(data, next, &saved_list, list) {
		decon_update_regs(decon, data);
		decon_lpd_unblock(decon);
		list_del(&data->list);
		kfree(data);
	}
}

static int decon_set_win_config(struct decon_device *decon,
		struct decon_win_config_data *win_data)
{
	struct decon_win_config *win_config = win_data->config;
	int ret = 0;
	unsigned short i, j;
	struct decon_reg_data *regs;
	struct sync_fence *fence;
	struct sync_pt *pt;
	int fd, unused_fd[3] = {0}, fd_idx = 0;
	int plane_cnt = 0;
	unsigned int bw = 0;

	mutex_lock(&decon->output_lock);
	fd = get_unused_fd();
	if (fd < 0) {
		mutex_unlock(&decon->output_lock);
		return -EINVAL;
	}

	if (fd < 3) {
		/* If fd from get_unused_fd() has value between 0 and 2,
		 * fd is tried to get value again using dup() except current fd vlaue.
		 */
		while (fd < 3) {
			unused_fd[fd_idx++] = fd;
			fd = get_unused_fd();
		}

		while (fd_idx-- > 0)
			put_unused_fd(unused_fd[fd_idx]);
	}

	if ((decon->state == DECON_STATE_OFF) || (decon->ignore_vsync == true)
		|| (decon->out_type == DECON_OUT_TUI)) {
		decon->timeline_max++;
		pt = sw_sync_pt_create(decon->timeline, decon->timeline_max);
		fence = sync_fence_create("display", pt);
		sync_fence_install(fence, fd);
		win_data->fence = fd;

		sw_sync_timeline_inc(decon->timeline, 1);
		goto err;
	}

	regs = kzalloc(sizeof(struct decon_reg_data), GFP_KERNEL);
	if (!regs) {
		decon_err("could not allocate decon_reg_data\n");
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < decon->pdata->max_win; i++) {
		decon->windows[i]->prev_fix =
			decon->windows[i]->fbinfo->fix;
		decon->windows[i]->prev_var =
			decon->windows[i]->fbinfo->var;

	}

#ifdef CONFIG_FB_WINDOW_UPDATE
	if (decon->out_type == DECON_OUT_DSI)
		decon_set_win_update_config(decon, win_config, regs);
#endif

	for (i = 0; i < decon->pdata->max_win && !ret; i++) {
		struct decon_win_config *config = &win_config[i];
		struct decon_win *win = decon->windows[i];

		bool enabled = 0;
		u32 color_map = WIN_MAP_MAP | WIN_MAP_MAP_COLOUR(0);

		if (does_layer_need_scale(config)) {
			decon_err("ERROR: layer(%d) needs scaling"
				"(%d,%d) -> (%d,%d)\n", i,
				config->src.w, config->dst.w,
				config->src.h, config->dst.h);
			config->state = DECON_WIN_STATE_DISABLED;
		}

		if (decon_get_memory_plane_cnt(config->format) < 0) {
			WARN(1, "Unsupported Format: (%d)\n", config->format);
			config->state = DECON_WIN_STATE_DISABLED;
		}

		switch (config->state) {
		case DECON_WIN_STATE_DISABLED:
			break;
		case DECON_WIN_STATE_COLOR:
			enabled = 1;
			color_map |= WIN_MAP_MAP_COLOUR(config->color);
			regs->vidosd_a[win->index] = vidosd_a(config->dst.x, config->dst.y);
			regs->vidosd_b[win->index] = vidosd_b(config->dst.x, config->dst.y,
					config->dst.w, config->dst.h);
			decon_set_alpha_blending(config, regs, win->index, 0);
			break;
		case DECON_WIN_STATE_BUFFER:
			if (IS_ENABLED(CONFIG_DECON_BLOCKING_MODE))
				if (decon_set_win_blocking_mode(decon,
						win, win_config, regs))
					break;

			ret = decon_set_win_buffer(decon, win, config, regs);
			if (!ret) {
				enabled = 1;
				color_map = 0;
			}
			break;
		default:
			decon_warn("unrecognized window state %u",
					config->state);
			ret = -EINVAL;
			break;
		}
		if (enabled)
			regs->wincon[i] |= WINCON_ENWIN;
		else
			regs->wincon[i] &= ~WINCON_ENWIN;

		/*
		 * Because BURSTLEN field does not have shadow register,
		 * this bit field should be retain always.
		 * exynos7580 must be set 16 burst
		 */
		regs->wincon[i] |= WINCON_BURSTLEN_16WORD;

		regs->winmap[i] = color_map;

		if (enabled && config->state == DECON_WIN_STATE_BUFFER) {
			/* Actual width, height are used in calculation of bw */
			bw += decon_calc_bandwidth(config->dst.w, config->dst.h,
				DIV_ROUND_UP(win->fbinfo->var.bits_per_pixel, 8),
				win->fps);
			regs->num_of_window++;
		}
	}

	for (i = 0; i < decon->pdata->max_win; i++)
		memcpy(&regs->win_config[i], &win_config[i],
				sizeof(struct decon_win_config));

	regs->bandwidth = bw;
	decon_dbg("Total BW = %llu Mbits, Max BW per window = %llu Mbits\n",
			regs->bandwidth >> 20, decon->max_win_bw >> 20);

	if (ret) {
		for (i = 0; i < decon->pdata->max_win; i++) {
			decon->windows[i]->fbinfo->fix = decon->windows[i]->prev_fix;
			decon->windows[i]->fbinfo->var = decon->windows[i]->prev_var;

			plane_cnt = decon_get_memory_plane_cnt(regs->win_config[i].format);
			for (j = 0; j < plane_cnt; ++j)
				decon_free_dma_buf(decon, &regs->dma_buf_data[i][j]);
		}
		put_unused_fd(fd);
		kfree(regs);
	} else if (decon->out_type == DECON_OUT_DSI) {
		decon_lpd_block(decon);
		mutex_lock(&decon->update_regs_list_lock);
		decon->timeline_max++;
		pt = sw_sync_pt_create(decon->timeline, decon->timeline_max);
		fence = sync_fence_create("display", pt);
		sync_fence_install(fence, fd);
		win_data->fence = fd;

		list_add_tail(&regs->list, &decon->update_regs_list);
		mutex_unlock(&decon->update_regs_list_lock);
		queue_kthread_work(&decon->update_regs_worker,
				&decon->update_regs_work);
	}
err:
	mutex_unlock(&decon->output_lock);
	return ret;
}

static ssize_t decon_fb_read(struct fb_info *info, char __user *buf,
		size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t decon_fb_write(struct fb_info *info, const char __user *buf,
		size_t count, loff_t *ppos)
{
	return 0;
}

static int decon_ioctl(struct fb_info *info, unsigned int cmd,
			unsigned long arg)
{
	struct decon_win *win = info->par;
	struct decon_device *decon = win->decon;
	struct decon_win_config_data win_data = { 0 };
	int ret = 0;
	u32 crtc;

	/* enable lpd only when system is ready to interact with driver */
	decon_lpd_enable();

	decon_lpd_block_exit(decon);

	switch (cmd) {
	case FBIO_WAITFORVSYNC:
		if (get_user(crtc, (u32 __user *)arg)) {
			ret = -EFAULT;
			break;
		}

		if (crtc == 0)
			ret = decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
		else
			ret = -ENODEV;

		break;

	case S3CFB_WIN_POSITION:
		if (copy_from_user(&decon->ioctl_data.user_window,
				(struct decon_user_window __user *)arg,
				sizeof(decon->ioctl_data.user_window))) {
			ret = -EFAULT;
			break;
		}

		if (decon->ioctl_data.user_window.x < 0)
			decon->ioctl_data.user_window.x = 0;
		if (decon->ioctl_data.user_window.y < 0)
			decon->ioctl_data.user_window.y = 0;

		ret = decon_set_window_position(info, decon->ioctl_data.user_window);
		break;

	case S3CFB_WIN_SET_PLANE_ALPHA:
		if (copy_from_user(&decon->ioctl_data.user_alpha,
				(struct s3c_fb_user_plane_alpha __user *)arg,
				sizeof(decon->ioctl_data.user_alpha))) {
			ret = -EFAULT;
			break;
		}

		ret = decon_set_plane_alpha_blending(info, decon->ioctl_data.user_alpha);
		break;

	case S3CFB_WIN_SET_CHROMA:
		if (copy_from_user(&decon->ioctl_data.user_chroma,
				   (struct s3c_fb_user_chroma __user *)arg,
				   sizeof(decon->ioctl_data.user_chroma))) {
			ret = -EFAULT;
			break;
		}

		ret = decon_set_chroma_key(info, decon->ioctl_data.user_chroma);
		break;

	case S3CFB_SET_VSYNC_INT:
		if (get_user(decon->ioctl_data.vsync, (int __user *)arg)) {
			ret = -EFAULT;
			break;
		}

		ret = decon_set_vsync_int(info, decon->ioctl_data.vsync);
		break;

	case S3CFB_WIN_CONFIG:
		if (copy_from_user(&win_data,
				   (struct decon_win_config_data __user *)arg,
				   sizeof(struct decon_win_config_data))) {
			ret = -EFAULT;
			break;
		}

		if ((decon->disp_ss_log_unmask & EVT_TYPE_WININFO))
			DISP_SS_EVENT_LOG_WIN_CONFIG(&decon->sd, &decon->ioctl_data.win_data);
		else
			DISP_SS_EVENT_LOG(DISP_EVT_WIN_CONFIG, &decon->sd, ktime_set(0, 0));

		ret = decon_set_win_config(decon, &win_data);
		if (ret)
			break;

		if (copy_to_user(&((struct decon_win_config_data __user *)arg)->fence,
				 &win_data.fence, sizeof(int))) {
			ret = -EFAULT;
			break;
		}
		break;

	default:
		ret = -ENOTTY;
	}

	decon_lpd_unblock(decon);
	return ret;
}

int decon_release(struct fb_info *info, int user)
{
	return 0;
}

static int decon_compat_ioctl(struct fb_info *info, unsigned int cmd,
				unsigned long arg)
{
	arg = (unsigned long) compat_ptr(arg);
	return decon_ioctl(info, cmd, arg);
}

extern int decon_set_par(struct fb_info *info);
extern int decon_pan_display(struct fb_var_screeninfo *var, struct fb_info *info);
extern int decon_setcolreg(unsigned regno,
			    unsigned red, unsigned green, unsigned blue,
			    unsigned transp, struct fb_info *info);
extern int decon_mmap(struct fb_info *info, struct vm_area_struct *vma);

/* ---------- FREAMBUFFER INTERFACE ----------- */
static struct fb_ops decon_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= decon_check_var,
	.fb_set_par	= decon_set_par,
	.fb_blank	= decon_blank,
	.fb_setcolreg	= decon_setcolreg,
	.fb_fillrect    = cfb_fillrect,
	.fb_copyarea    = cfb_copyarea,
	.fb_imageblit   = cfb_imageblit,
	.fb_ioctl	= decon_ioctl,
	.fb_compat_ioctl = decon_compat_ioctl,
	.fb_read	= decon_fb_read,
	.fb_write	= decon_fb_write,
	.fb_pan_display	= decon_pan_display,
	.fb_mmap	= decon_mmap,
	.fb_release	= decon_release,
};

/* ---------- POWER MANAGEMENT ----------- */
void decon_clocks_info(struct decon_device *decon)
{
	decon_warn("%s: %ld Mhz\n", __clk_get_name(decon->res.pclk),
				clk_get_rate(decon->res.pclk) / MHZ);
	decon_warn("%s: %ld Mhz\n", __clk_get_name(decon->res.aclk),
				clk_get_rate(decon->res.aclk) / MHZ);
	decon_warn("%s: %ld Mhz\n", __clk_get_name(decon->res.eclk),
				clk_get_rate(decon->res.eclk) / MHZ);
	decon_warn("%s: %ld Mhz\n", __clk_get_name(decon->res.vclk),
				clk_get_rate(decon->res.vclk) / MHZ);
	decon_warn("%s: %ld Mhz\n", __clk_get_name(decon->res.aclk_disp),
				clk_get_rate(decon->res.aclk_disp) / MHZ);
}

void decon_put_clocks(struct decon_device *decon)
{
	clk_put(decon->res.pclk);
	clk_put(decon->res.aclk);
	clk_put(decon->res.eclk);
	clk_put(decon->res.vclk);
	clk_put(decon->res.aclk_disp);
	clk_put(decon->res.mif_pll);
}

static int decon_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct decon_device *decon = platform_get_drvdata(pdev);

	DISP_SS_EVENT_LOG(DISP_EVT_DECON_RESUME, &decon->sd, ktime_set(0, 0));
	decon_dbg("decon %s +\n", __func__);
	mutex_lock(&decon->mutex);

	decon_int_set_clocks(decon);

	clk_prepare_enable(decon->res.pclk);
	clk_prepare_enable(decon->res.aclk);
	clk_prepare_enable(decon->res.eclk);
	clk_prepare_enable(decon->res.vclk);
	clk_prepare_enable(decon->res.aclk_disp);

	if (decon->state == DECON_STATE_INIT)
		decon_clocks_info(decon);

	mutex_unlock(&decon->mutex);
	decon_dbg("decon %s -\n", __func__);

	return 0;
}

static int decon_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct decon_device *decon = platform_get_drvdata(pdev);

	DISP_SS_EVENT_LOG(DISP_EVT_DECON_SUSPEND, &decon->sd, ktime_set(0, 0));
	decon_dbg("decon %s +\n", __func__);
	mutex_lock(&decon->mutex);

	clk_disable_unprepare(decon->res.pclk);
	clk_disable_unprepare(decon->res.aclk);
	clk_disable_unprepare(decon->res.eclk);
	clk_disable_unprepare(decon->res.vclk);
	clk_disable_unprepare(decon->res.aclk_disp);

	mutex_unlock(&decon->mutex);
	decon_dbg("decon %s -\n", __func__);

	return 0;
}

static const struct dev_pm_ops decon_pm_ops = {
	.runtime_suspend = decon_runtime_suspend,
	.runtime_resume	 = decon_runtime_resume,
};

/* ---------- MEDIA CONTROLLER MANAGEMENT ----------- */
static long decon_sd_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct decon_device *decon = container_of(sd, struct decon_device, sd);
	long ret = 0;

	switch (cmd) {
	case DECON_IOC_LPD_EXIT_LOCK:
		decon_lpd_block_exit(decon);
		break;
	case DECON_IOC_LPD_UNLOCK:
		decon_lpd_unblock(decon);
		break;
	default:
		dev_err(decon->dev, "unsupported ioctl");
		ret = -EINVAL;
	}
	return ret;
}

static int decon_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

static int decon_s_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
		       struct v4l2_subdev_format *format)
{
	decon_err("unsupported ioctl");
	return -EINVAL;
}

static const struct v4l2_subdev_core_ops decon_sd_core_ops = {
	.ioctl = decon_sd_ioctl,
};

static const struct v4l2_subdev_video_ops decon_sd_video_ops = {
	.s_stream = decon_s_stream,
};

static const struct v4l2_subdev_pad_ops	decon_sd_pad_ops = {
	.set_fmt = decon_s_fmt,
};

static const struct v4l2_subdev_ops decon_sd_ops = {
	.video = &decon_sd_video_ops,
	.core = &decon_sd_core_ops,
	.pad = &decon_sd_pad_ops,
};

static int decon_link_setup(struct media_entity *entity,
			      const struct media_pad *local,
			      const struct media_pad *remote, u32 flags)
{
	return 0;
}

static const struct media_entity_operations decon_entity_ops = {
	.link_setup = decon_link_setup,
};

static int decon_register_subdev_nodes(struct decon_device *decon,
					struct exynos_md *md)
{
	int ret = v4l2_device_register_subdev_nodes(&md->v4l2_dev);
	if (ret) {
		decon_err("failed to make nodes for subdev\n");
		return ret;
	}

	decon_info("Register V4L2 subdev nodes for DECON\n");

	return 0;

}

static int decon_create_links(struct decon_device *decon,
					struct exynos_md *md)
{
	char err[80];

	decon_info("decon create links\n");
	memset(err, 0, sizeof(err));

	/* link creation: decon <-> output */
	return create_link_mipi(decon);
}

static void decon_unregister_entity(struct decon_device *decon)
{
	v4l2_device_unregister_subdev(&decon->sd);
}

static int decon_register_entity(struct decon_device *decon)
{
	struct v4l2_subdev *sd = &decon->sd;
	struct media_pad *pads = decon->pads;
	struct media_entity *me = &sd->entity;
	struct exynos_md *md;
	int i, n_pad, ret = 0;

	/* init DECON sub-device */
	v4l2_subdev_init(sd, &decon_sd_ops);
	sd->owner = THIS_MODULE;
	snprintf(sd->name, sizeof(sd->name), "exynos-decon%d", DECON_INT);

	/* DECON sub-device can be opened in user space */
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	/* init DECON sub-device as entity */
	n_pad = decon->n_sink_pad + decon->n_src_pad;
	for (i = 0; i < decon->n_sink_pad; i++)
		pads[i].flags = MEDIA_PAD_FL_SINK;
	for (i = decon->n_sink_pad; i < n_pad; i++)
		pads[i].flags = MEDIA_PAD_FL_SOURCE;

	me->ops = &decon_entity_ops;
	ret = media_entity_init(me, n_pad, pads, 0);
	if (ret) {
		decon_err("failed to initialize media entity\n");
		return ret;
	}

	md = (struct exynos_md *)module_name_to_driver_data(MDEV_MODULE_NAME);
	if (!md) {
		decon_err("failed to get output media device\n");
		return -ENODEV;
	}

	ret = v4l2_device_register_subdev(&md->v4l2_dev, sd);
	if (ret) {
		decon_err("failed to register DECON subdev\n");
		return ret;
	}
	decon_info("%s entity init\n", sd->name);

	return find_subdev_mipi(decon);
}

static void decon_release_windows(struct decon_win *win)
{
	if (win->fbinfo)
		framebuffer_release(win->fbinfo);
}

static int decon_fb_alloc_memory(struct decon_device *decon, struct decon_win *win)
{
	struct decon_fb_pd_win *windata = &win->windata;
	unsigned int real_size, virt_size, size;
	struct fb_info *fbi = win->fbinfo;
	struct ion_handle *handle;
	dma_addr_t map_dma;
	struct dma_buf *buf;
	void *vaddr;
	unsigned int ret;

	dev_info(decon->dev, "allocating memory for display\n");

	real_size = windata->win_mode.videomode.xres * windata->win_mode.videomode.yres;
	virt_size = windata->virtual_x * windata->virtual_y;

	dev_info(decon->dev, "real_size=%u (%u.%u), virt_size=%u (%u.%u)\n",
		real_size, windata->win_mode.videomode.xres, windata->win_mode.videomode.yres,
		virt_size, windata->virtual_x, windata->virtual_y);

	size = (real_size > virt_size) ? real_size : virt_size;
	size *= (windata->max_bpp > 16) ? 32 : windata->max_bpp;
	size /= 8;

	fbi->fix.smem_len = size;
	size = PAGE_ALIGN(size);

	dev_info(decon->dev, "want %u bytes for window[%d]\n", size, win->index);

#if defined(CONFIG_ION_EXYNOS)
	handle = ion_alloc(decon->ion_client, (size_t)size, 0,
					EXYNOS_ION_HEAP_SYSTEM_MASK, 0);
	if (IS_ERR(handle)) {
		dev_err(decon->dev, "failed to ion_alloc\n");
		return -ENOMEM;
	}

	buf = ion_share_dma_buf(decon->ion_client, handle);
	if (IS_ERR_OR_NULL(buf)) {
		dev_err(decon->dev, "ion_share_dma_buf() failed\n");
		goto err_share_dma_buf;
	}

	vaddr = ion_map_kernel(decon->ion_client, handle);

	fbi->screen_base = vaddr;

	win->dma_buf_data[1].fence = NULL;
	win->dma_buf_data[2].fence = NULL;
	ret = decon_map_ion_handle(decon, decon->dev, &win->dma_buf_data[0],
			handle, buf, win->index);
	if (!ret)
		goto err_map;
	map_dma = win->dma_buf_data[0].dma_addr;

	dev_info(decon->dev, "alloated memory\n");
#else
	fbi->screen_base = dma_alloc_writecombine(decon->dev, size,
						  &map_dma, GFP_KERNEL);
	if (!fbi->screen_base)
		return -ENOMEM;

	dev_dbg(decon->dev, "mapped %x to %p\n",
		(unsigned int)map_dma, fbi->screen_base);

	memset(fbi->screen_base, 0x0, size);
#endif
	fbi->fix.smem_start = map_dma;

	dev_info(decon->dev, "fb start addr = 0x%x\n", (u32)fbi->fix.smem_start);

	return 0;

#ifdef CONFIG_ION_EXYNOS
err_map:
	dma_buf_put(buf);
err_share_dma_buf:
	ion_free(decon->ion_client, handle);
	return -ENOMEM;
#endif
}

static void decon_missing_pixclock(struct decon_fb_videomode *win_mode)
{
	u64 pixclk = 1000000000000ULL;
	u32 div;
	u32 width, height;

	width = win_mode->videomode.xres;
	height = win_mode->videomode.yres;

	div = width * height * (win_mode->videomode.refresh ? : 60);

	do_div(pixclk, div);
	win_mode->videomode.pixclock = pixclk;
}

static int decon_acquire_windows(struct decon_device *decon, int idx)
{
	struct decon_win *win;
	struct fb_info *fbinfo;
	struct fb_var_screeninfo *var;
	struct decon_lcd *lcd_info = NULL;
	int ret, i;

	decon_dbg("acquire DECON window%d\n", idx);

	fbinfo = framebuffer_alloc(sizeof(struct decon_win), decon->dev);
	if (!fbinfo) {
		decon_err("failed to allocate framebuffer\n");
		return -ENOENT;
	}

	win = fbinfo->par;
	decon->windows[idx] = win;
	var = &fbinfo->var;
	win->fbinfo = fbinfo;
	/* fbinfo->fbops = &decon_fb_ops; */
	/* fbinfo->flags = FBINFO_FLAG_DEFAULT; */
	win->decon = decon;
	win->index = idx;

	win->windata.default_bpp = 32;
	win->windata.max_bpp = 32;

	lcd_info = decon->lcd_info;
	win->windata.virtual_x = lcd_info->xres;
	win->windata.virtual_y = lcd_info->yres * 2;
	win->windata.width = lcd_info->xres;
	win->windata.height = lcd_info->yres;
	win->windata.win_mode.videomode.left_margin = lcd_info->hbp;
	win->windata.win_mode.videomode.right_margin = lcd_info->hfp;
	win->windata.win_mode.videomode.upper_margin = lcd_info->vbp;
	win->windata.win_mode.videomode.lower_margin = lcd_info->vfp;
	win->windata.win_mode.videomode.hsync_len = lcd_info->hsa;
	win->windata.win_mode.videomode.vsync_len = lcd_info->vsa;
	win->windata.win_mode.videomode.xres = lcd_info->xres;
	win->windata.win_mode.videomode.yres = lcd_info->yres;
	decon_missing_pixclock(&win->windata.win_mode);

	for (i = 0; i < MAX_BUF_PLANE_CNT; ++i)
		memset(&win->dma_buf_data[i], 0, sizeof(win->dma_buf_data));

	if (win->index == decon->pdata->default_win) {
		ret = decon_fb_alloc_memory(decon, win);
		if (ret) {
			dev_err(decon->dev, "failed to allocate display memory\n");
			return ret;
		}
	}

	fb_videomode_to_var(&fbinfo->var, &win->windata.win_mode.videomode);

	fbinfo->fix.type	= FB_TYPE_PACKED_PIXELS;
	fbinfo->fix.accel	= FB_ACCEL_NONE;
	fbinfo->var.activate	= FB_ACTIVATE_NOW;
	fbinfo->var.vmode	= FB_VMODE_NONINTERLACED;
	fbinfo->var.bits_per_pixel = win->windata.default_bpp;
	fbinfo->var.width	= win->windata.width;
	fbinfo->var.height	= win->windata.height;
	fbinfo->fbops		= &decon_fb_ops;
	fbinfo->flags		= FBINFO_FLAG_DEFAULT;
	fbinfo->pseudo_palette  = &win->pseudo_palette;

	ret = decon_check_var(&fbinfo->var, fbinfo);
	if (ret < 0) {
		dev_err(decon->dev, "check_var failed on initial video params\n");
		return ret;
	}

	ret = fb_alloc_cmap(&fbinfo->cmap, 256 /* palette size */, 1);
	if (ret == 0)
		fb_set_cmap(&fbinfo->cmap, fbinfo);
	else
		dev_err(decon->dev, "failed to allocate fb cmap\n");

	decon_info("decon window[%d] create\n", idx);
	return 0;
}

static int decon_acquire_window(struct decon_device *decon)
{
	int i, ret;

	for (i = 0; i < decon->n_sink_pad; i++) {
		ret = decon_acquire_windows(decon, i);
		if (ret < 0) {
			decon_err("failed to create decon-int window[%d]\n", i);
			for (; i >= 0; i--)
				decon_release_windows(decon->windows[i]);
			return ret;
		}
	}

	return 0;
}

static void decon_parse_pdata(struct decon_device *decon, struct device *dev)
{
#ifdef CONFIG_DECON_USE_BOOTLOADER_FB
	u32 res[6];
#endif

	if (dev->of_node) {
		of_property_read_u32(dev->of_node, "ip_ver",
					&decon->pdata->ip_ver);
		of_property_read_u32(dev->of_node, "n_sink_pad",
					&decon->n_sink_pad);
		of_property_read_u32(dev->of_node, "n_src_pad",
					&decon->n_src_pad);
		of_property_read_u32(dev->of_node, "max_win",
					&decon->pdata->max_win);
		of_property_read_u32(dev->of_node, "default_win",
					&decon->pdata->default_win);
		/* video mode: 0, dp: 1 mipi command mode: 2 */
		of_property_read_u32(dev->of_node, "psr_mode",
					&decon->pdata->psr_mode);
		/* H/W trigger: 0, S/W trigger: 1 */
		of_property_read_u32(dev->of_node, "trig_mode",
					&decon->pdata->trig_mode);
		decon_info("decon-%s: ver%d, max win%d, %s mode, %s trigger\n",
			"int", decon->pdata->ip_ver,
			decon->pdata->max_win,
			decon->pdata->psr_mode ? "command" : "video",
			decon->pdata->trig_mode ? "sw" : "hw");

		/* single DSI: 0, dual DSI: 1 */
		of_property_read_u32(dev->of_node, "dsi_mode",
				&decon->pdata->dsi_mode);
		/* disp_pll */
		of_property_read_u32(dev->of_node, "disp-pll-clk",
				&decon->pdata->disp_pll_clk);
		decon_info("dsi mode(%d). 0: single 1: dual dsi 2: dual display\n",
				decon->pdata->dsi_mode);

#ifdef CONFIG_DECON_USE_BOOTLOADER_FB
		if (!of_property_read_u32_array(dev->of_node, "bootloader_fb",
					res, 6)) {
			decon->bl_fb_info.phy_addr	= res[0];
			decon->bl_fb_info.l		= res[1];
			decon->bl_fb_info.t		= res[2];
			decon->bl_fb_info.r		= res[3];
			decon->bl_fb_info.b		= res[4];
			decon->bl_fb_info.format	= res[5];
			decon->bl_fb_info.size		= 4 *
				(decon->bl_fb_info.b - decon->bl_fb_info.t) *
				(decon->bl_fb_info.r - decon->bl_fb_info.l);
			decon_warn("bl_fb_info: 0x%x, (%d, %d, %d, %d) f=%d\n",
				decon->bl_fb_info.phy_addr,
				decon->bl_fb_info.l,
				decon->bl_fb_info.t,
				decon->bl_fb_info.r,
				decon->bl_fb_info.b,
				decon->bl_fb_info.format);
		}
#endif
	} else {
		decon_warn("no device tree information\n");
	}
}

#ifdef CONFIG_DECON_EVENT_LOG
static int decon_debug_event_show(struct seq_file *s, void *unused)
{
	struct decon_device *decon = s->private;
	DISP_SS_EVENT_SHOW(s, decon, 0, false);
	return 0;
}

static int decon_debug_event_open(struct inode *inode, struct file *file)
{
	return single_open(file, decon_debug_event_show, inode->i_private);
}

static struct file_operations decon_event_fops = {
	.open = decon_debug_event_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

#if defined(CONFIG_EXYNOS_DECON_DPU)
static int decon_dpu_scr_set(struct seq_file *s, void *unused)
{
	struct dpu *dpu = &decon_int_drvdata->dpu_save;

	seq_printf(s, "scr_onoff: %x\n", dpu->scr_onoff);
	seq_printf(s, "scr_red: %x\n", dpu->scr_red);
	seq_printf(s, "scr_green: %x\n", dpu->scr_green);
	seq_printf(s, "scr_blue: %x\n", dpu->scr_blue);
	seq_printf(s, "scr_cyan: %x\n", dpu->scr_cyan);
	seq_printf(s, "scr_magenta: %x\n", dpu->scr_magenta);
	seq_printf(s, "scr_yellow: %x\n", dpu->scr_yellow);
	seq_printf(s, "scr_white: %x\n", dpu->scr_white);
	seq_printf(s, "scr_black: %x\n", dpu->scr_black);

	return 0;
}

static int decon_dpu_gamma_set(struct seq_file *s, void *unused)
{
	struct dpu *dpu = &decon_int_drvdata->dpu_save;

	seq_printf(s, "gamma_onoff: %d\n", dpu->gamma_onoff);
	seq_printf(s, "gamma_set: %x\n", dpu->gamma_set);

	return 0;
}

static int decon_dpu_gamma_chunk_set(struct seq_file *s, void *unused)
{
	/* TODO : Need to define what shell We do? */
	return 0;
}

static int decon_dpu_sat_set(struct seq_file *s, void *unused)
{
	struct dpu *dpu = &decon_int_drvdata->dpu_save;

	seq_printf(s, "saturation_onoff: %d\n", dpu->saturation_onoff);
	seq_printf(s, "saturation_red: %d\n", dpu->saturation_red);
	seq_printf(s, "saturation_green: %d\n", dpu->saturation_green);
	seq_printf(s, "saturation_blue: %d\n", dpu->saturation_blue);
	seq_printf(s, "saturation_magenta: %d\n", dpu->saturation_magenta);
	seq_printf(s, "saturation_yellow: %d\n", dpu->saturation_yellow);
	seq_printf(s, "saturation_shift: %d\n", dpu->saturation_shift);
	seq_printf(s, "saturation_scale: %d\n", dpu->saturation_scale);
	seq_printf(s, "saturation_total: %d\n", dpu->saturation_total);

	return 0;
}

static int decon_dpu_hue_set(struct seq_file *s, void *unused)
{
	struct dpu *dpu = &decon_int_drvdata->dpu_save;

	seq_printf(s, "hue_onoff: %d\n", dpu->hue_onoff);
	seq_printf(s, "hue_red: %d\n", dpu->hue_red);
	seq_printf(s, "hue_green: %d\n", dpu->hue_green);
	seq_printf(s, "hue_blue: %d\n", dpu->hue_blue);
	seq_printf(s, "hue_cyan: %d\n", dpu->hue_cyan);
	seq_printf(s, "hue_magenta: %d\n", dpu->hue_magenta);
	seq_printf(s, "hue_yellow: %d\n", dpu->hue_yellow);

	return 0;
}

static int decon_dpu_preset_set(struct seq_file *s, void *unused)
{
	return 0;
}

static ssize_t decon_dpu_scr_write(struct file *file, const char __user *buffer,
			size_t count, loff_t *ppos)
{
	int val, ret = 0;
	u32 mask, length = 0;
	const char *str_fomat = NULL;
	const char *str_num = NULL;

	if (count > 32 || count == 0)
		return -1;

	if (decon_int_drvdata) {
		decon_lpd_block_exit(decon_int_drvdata);
		if (decon_int_drvdata->state != DECON_STATE_ON) {
			printk(KERN_ERR " decon is not enabled!\n");
			ret = -1;
			goto scr_err;
		}
	} else {
		printk(KERN_ERR " decon_int_drvdata is NULL!\n");
		ret = -1;
		goto scr_err;
	}

	if (buffer) {
		str_fomat = buffer;
		str_num = buffer + 2;
		length = count - 2;

		switch (str_fomat[0]) {
		case 'R':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_r(mask, DPU_SCR_RED(val));
			decon_int_drvdata->dpu_save.scr_red = val;
			break;
		case 'G':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_g(mask, DPU_SCR_GREEN(val));
			decon_int_drvdata->dpu_save.scr_green = val;
			break;
		case 'B':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_b(mask, DPU_SCR_BLUE(val));
			decon_int_drvdata->dpu_save.scr_blue = val;
			break;
		case 'C':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_c(mask, DPU_SCR_CYAN(val));
			decon_int_drvdata->dpu_save.scr_cyan = val;
			break;
		case 'M':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_m(mask, DPU_SCR_MAGENTA(val));
			decon_int_drvdata->dpu_save.scr_magenta = val;
			break;
		case 'Y':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_y(mask, DPU_SCR_YELLOW(val));
			decon_int_drvdata->dpu_save.scr_yellow = val;
			break;
		case 'W':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_w(mask, DPU_SCR_WHITE(val));
			decon_int_drvdata->dpu_save.scr_white = val;
			break;
		case 'K':
			mask = DPU_SCR_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_k(mask, DPU_SCR_BLACK(val));
			decon_int_drvdata->dpu_save.scr_black = val;
			break;
		case 'O':
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto scr_err;
			}
			dpu_reg_set_scr_onoff(val);
			decon_int_drvdata->dpu_save.scr_onoff = val;
			break;
		default:
			ret = -1;
			goto scr_err;
		}
	} else {
		ret = -1;
		goto scr_err;
	}

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return count;
scr_err:

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return ret;
}

static int gamma_lut_reg_set(u32 (*gamma_lut)[65])
{
	int i, j;
	u32 mask = 0, offset_in = 0, offset_ex = 0, gamma = 0;

	for (j = 0; j < 3; j++) {
		for (i = 0; i < 65; i++) {
			if ((i % 2) == 0) {
				if (i >= 2)
					offset_in += 4;
				mask = DPU_GAMMA_LUT_Y_MASK;
				gamma = DPU_GAMMA_LUT_Y(gamma_lut[j][i]);
			} else {
				mask = DPU_GAMMA_LUT_X_MASK;
				gamma = DPU_GAMMA_LUT_X(gamma_lut[j][i]);
			}
			dpu_reg_set_gamma(offset_in + offset_ex, mask, gamma);
			mask = 0;
			gamma = 0;
			if (i == 64)
				offset_in = 0;
		}
		offset_ex += DPU_GAMMA_OFFSET;
	}
	return 0;
}

static ssize_t decon_dpu_gamma_chunk_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *ppos)
{
	int ret = 0;
	u32 gamma_lut[3][65];

	if (count <= 0) {
		printk(KERN_ERR "gamma chunk write count error\n");
		ret = -1;
		goto gamma_err;
	}

	if (decon_int_drvdata) {
		decon_lpd_block_exit(decon_int_drvdata);
		if (decon_int_drvdata->state != DECON_STATE_ON) {
			printk(KERN_ERR " decon is not enabled!\n");
			ret = -1;
			goto gamma_err;
		}
	} else {
		printk(KERN_ERR " decon_int_drvdata is NULL!\n");
		ret = -1;
		goto gamma_err;
	}

	if (copy_from_user(gamma_lut, buffer, sizeof(gamma_lut))) {
		printk(KERN_ERR "copy_from_user(gamma_lut) failed\n");
		ret = -EFAULT;
		goto gamma_err;
	}

	gamma_lut_reg_set(gamma_lut);

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);
	return count;

gamma_err:
	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);
	return ret;
}

static ssize_t decon_dpu_gamma_write(struct file *file, const char __user *buffer,
			size_t count, loff_t *ppos)
{
	int i, j, val, ret = 0;
	u32 offset_in = 0, offset_ex = 0, gamma = 0, mask = 0, length = 0;
	const char *str_fomat = NULL;
	const char *str_num = NULL;

	if (count > 32 || count == 0)
		return -1;

	if (decon_int_drvdata) {
		decon_lpd_block_exit(decon_int_drvdata);
		if (decon_int_drvdata->state != DECON_STATE_ON) {
			printk(KERN_ERR " decon is not enabled!\n");
			ret = -1;
			goto gamma_err;
		}
	} else {
		printk(KERN_ERR " decon_int_drvdata is NULL!\n");
		ret = -1;
		goto gamma_err;
	}

	if (buffer) {
		str_fomat = buffer;
		str_num = buffer + 2;
		length = count - 2;

		switch (str_fomat[0]) {
		case 'O':
			mask = TSC_ON;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				return -EFAULT;
			}
			dpu_reg_set_gamma_onoff(val);
			decon_int_drvdata->dpu_save.gamma_onoff = val;
			ret = count;
			goto gamma_done;
		default:
			break;
		}
	} else {
		ret = -1;
		goto gamma_err;
	}

	if (kstrtoint_from_user(buffer, count, 16, &val)) {
		printk(KERN_ERR " copy_from_user() failed!\n");
		ret = -EFAULT;
		goto gamma_err;
	}

	switch (val) {
	case 0:
		for (j = 0; j < 3; j++) {
			for (i = 0; i < 65; i++) {
				if ((i % 2) == 0) {
					if (i >= 2)
						offset_in += 4;
					mask = DPU_GAMMA_LUT_Y_MASK;
					gamma = DPU_GAMMA_LUT_Y(gamma_table1[j][i]);
				} else {
					mask = DPU_GAMMA_LUT_X_MASK;
					gamma = DPU_GAMMA_LUT_X(gamma_table1[j][i]);
				}
				dpu_reg_set_gamma(offset_in + offset_ex, mask, gamma);
				mask = 0;
				gamma = 0;
				if (i == 64)
					offset_in = 0;
			}
			offset_ex += DPU_GAMMA_OFFSET;
		}
		break;
	case 1:
		for (j = 0; j < 3; j++) {
			for (i = 0; i < 65; i++) {
				if ((i % 2) == 0) {
					if (i >= 2)
						offset_in += 4;
					mask = DPU_GAMMA_LUT_Y_MASK;
					gamma = DPU_GAMMA_LUT_Y(gamma_table2[j][i]);
				} else {
					mask = DPU_GAMMA_LUT_X_MASK;
					gamma = DPU_GAMMA_LUT_X(gamma_table2[j][i]);
				}
				dpu_reg_set_gamma(offset_in + offset_ex, mask, gamma);
				mask = 0;
				gamma = 0;
				if (i == 64)
					offset_in = 0;
			}
			offset_ex += DPU_GAMMA_OFFSET;
		}
		break;
	case 2:
		for (j = 0; j < 3; j++) {
			for (i = 0; i < 65; i++) {
				if ((i % 2) == 0) {
					if (i >= 2)
						offset_in += 4;
					mask = DPU_GAMMA_LUT_Y_MASK;
					gamma = DPU_GAMMA_LUT_Y(gamma_table3[j][i]);
				} else {
					mask = DPU_GAMMA_LUT_X_MASK;
					gamma = DPU_GAMMA_LUT_X(gamma_table3[j][i]);
				}
				dpu_reg_set_gamma(offset_in + offset_ex, mask, gamma);
				mask = 0;
				gamma = 0;
				if (i == 64)
					offset_in = 0;
			}
			offset_ex += DPU_GAMMA_OFFSET;
		}
		break;
	default:
		ret = -1;
		goto gamma_err;
		break;
	}
	decon_int_drvdata->dpu_save.gamma_set = val;

gamma_done:
	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return count;

gamma_err:

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return ret;
}

static ssize_t decon_dpu_sat_write(struct file *file, const char __user *buffer,
			size_t count, loff_t *ppos)
{
	int val, ret = 0;
	u32 mask, length = 0;
	const char *str_fomat = NULL;
	const char *str_num = NULL;


	if (count > 32 || count == 0)
		return -1;

	if (decon_int_drvdata) {
		decon_lpd_block_exit(decon_int_drvdata);
		if (decon_int_drvdata->state != DECON_STATE_ON) {
			printk(KERN_ERR " decon is not enabled!\n");
			ret = -1;
			goto sat_err;
		}
	} else {
		printk(KERN_ERR " decon_int_drvdata is NULL!\n");
		ret = -1;
		goto sat_err;
	}

	if (buffer) {
		str_fomat = buffer;
		str_num = buffer+2;
		length = count - 2;

		switch (str_fomat[0]) {
		case 'R':
			mask = PAIM_GAIN0;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_rgb(mask, DPU_TSC_RED(val));
			decon_int_drvdata->dpu_save.saturation_red = val;
			break;
		case 'G':
			mask = PAIM_GAIN1;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_rgb(mask, DPU_TSC_GREEN(val));
			decon_int_drvdata->dpu_save.saturation_green = val;
			break;
		case 'B':
			mask = PAIM_GAIN2;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_rgb(mask, DPU_TSC_BLUE(val));
			decon_int_drvdata->dpu_save.saturation_blue = val;
			break;
		case 'C':
			mask = PAIM_GAIN3;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_cmy(mask, DPU_TSC_CYAN(val));
			decon_int_drvdata->dpu_save.saturation_magenta = val;
			break;

		case 'M':
			mask = PAIM_GAIN3;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_cmy(mask, DPU_TSC_MAGENTA(val));
			decon_int_drvdata->dpu_save.saturation_magenta = val;
			break;
		case 'Y':
			mask = PAIM_GAIN4;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_cmy(mask, DPU_TSC_YELLOW(val));
			decon_int_drvdata->dpu_save.saturation_yellow = val;
			break;
		case 'O':
			mask = TSC_ON;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_onoff(val);
			decon_int_drvdata->dpu_save.saturation_onoff = val;
			break;
		case 'T':
			mask = TSC_GAIN;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_tscgain(DPU_TSC_GAIN(val));
			decon_int_drvdata->dpu_save.saturation_total = val;
			break;
		case 'S':
			mask = PAIM_SHIFT;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_shift(mask, DPU_TSC_SHIFT(val));
			decon_int_drvdata->dpu_save.saturation_shift = val;
			break;
		case 'A':
			mask = PAIM_SCALE;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto sat_err;
			}
			dpu_reg_set_saturation_shift(mask, DPU_TSC_SCALE(val));
			decon_int_drvdata->dpu_save.saturation_scale = val;
			break;
		default:
			ret = -1;
			goto sat_err;
		}
	} else {
		ret = -1;
		goto sat_err;
	}

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return count;
sat_err:

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return ret;
}

static ssize_t decon_dpu_hue_write(struct file *file, const char __user *buffer,
			size_t count, loff_t *ppos)
{
	int val, ret = 0;
	u32 mask, length = 0;
	const char *str_fomat = NULL;
	const char *str_num = NULL;

	if (count > 32 || count == 0)
		return -1;

	if (decon_int_drvdata) {
		decon_lpd_block_exit(decon_int_drvdata);
		if (decon_int_drvdata->state != DECON_STATE_ON) {
			printk(KERN_ERR " decon is not enabled!\n");
			ret = -1;
			goto hue_err;
		}
	} else {
		printk(KERN_ERR " decon_int_drvdata is NULL!\n");
		ret = -1;
		goto hue_err;
	}

	if (buffer) {
		str_fomat = buffer;
		str_num = buffer+2;
		length = count - 2;

		switch (str_fomat[0]) {
		case 'R':
			mask = DPU_PPHC_GAIN0_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto hue_err;
			}
			dpu_reg_set_hue_rgb(mask, DPU_HUE_RED(val));
			decon_int_drvdata->dpu_save.hue_red = val;
			break;
		case 'G':
			mask = DPU_PPHC_GAIN1_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto hue_err;
			}
			dpu_reg_set_hue_rgb(mask, DPU_HUE_GREEN(val));
			decon_int_drvdata->dpu_save.hue_green = val;
			break;
		case 'B':
			mask = DPU_PPHC_GAIN2_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto hue_err;
			}
			dpu_reg_set_hue_rgb(mask, DPU_HUE_BLUE(val));
			decon_int_drvdata->dpu_save.hue_blue = val;
			break;
		case 'C':
			mask = DPU_PPHC_GAIN3_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto hue_err;
			}
			dpu_reg_set_hue_cmy(mask, DPU_HUE_CYAN(val));
			decon_int_drvdata->dpu_save.hue_cyan = val;
			break;
		case 'M':
			mask = DPU_PPHC_GAIN4_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto hue_err;
			}
			dpu_reg_set_hue_cmy(mask, DPU_HUE_MAGENTA(val));
			decon_int_drvdata->dpu_save.hue_magenta = val;
			break;
		case 'Y':
			mask = DPU_PPHC_GAIN5_MASK;
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto hue_err;
			}
			dpu_reg_set_hue_cmy(mask, DPU_HUE_YELLOW(val));
			decon_int_drvdata->dpu_save.hue_yellow = val;
			break;
		case 'O':
			if (kstrtoint_from_user(str_num, length, 16, &val)) {
				printk(KERN_ERR " copy_from_user() failed!\n");
				ret = -EFAULT;
				goto hue_err;
			}
			dpu_reg_set_hue_onoff(val);
			decon_int_drvdata->dpu_save.hue_onoff = val;
			break;
		default:
			ret = -1;
			goto hue_err;
		}
	} else {
		ret = -1;
		goto hue_err;
	}

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return count;
hue_err:

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return ret;
}

static ssize_t decon_dpu_preset_write(struct file *file, const char __user *buffer,
			size_t count, loff_t *ppos)
{
	int val, ret = 0;

	if (count > 32 || count == 0)
		return -1;

	if (decon_int_drvdata) {
		decon_lpd_block_exit(decon_int_drvdata);
		if (decon_int_drvdata->state != DECON_STATE_ON) {
			printk(KERN_ERR " decon is not enabled!\n");
			ret = -1;
			goto preset_err;
		}
	} else {
		printk(KERN_ERR " decon_int_drvdata is NULL!\n");
		ret = -1;
		goto preset_err;
	}

	if (kstrtoint_from_user(buffer, count, 16, &val)) {
		printk(KERN_ERR " copy_from_user() failed!\n");
		ret = -EFAULT;
		goto preset_err;
	}

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return count;
preset_err:

	if (decon_int_drvdata)
		decon_lpd_unblock(decon_int_drvdata);

	return ret;
}

static int decon_dpu_scr_open(struct inode *inode, struct file *file)
{
	return single_open(file, decon_dpu_scr_set, inode->i_private);
}

static int decon_dpu_gamma_open(struct inode *inode, struct file *file)
{
	return single_open(file, decon_dpu_gamma_set, inode->i_private);
}

static int decon_dpu_gamma_chunk_open(struct inode *inode, struct file *file)
{
	return single_open(file, decon_dpu_gamma_chunk_set, inode->i_private);
}

static int decon_dpu_sat_open(struct inode *inode, struct file *file)
{
	return single_open(file, decon_dpu_sat_set, inode->i_private);
}

static int decon_dpu_hue_open(struct inode *inode, struct file *file)
{
	return single_open(file, decon_dpu_hue_set, inode->i_private);
}

static int decon_dpu_preset_open(struct inode *inode, struct file *file)
{
	return single_open(file, decon_dpu_preset_set, inode->i_private);
}

static struct file_operations decon_dpu_scr_ops = {
	.open = decon_dpu_scr_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write   = decon_dpu_scr_write,
};

static struct file_operations decon_dpu_gamma_chunk_ops = {
	.open = decon_dpu_gamma_chunk_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write   = decon_dpu_gamma_chunk_write,
};

static struct file_operations decon_dpu_gamma_ops = {
	.open = decon_dpu_gamma_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write   = decon_dpu_gamma_write,
};

static struct file_operations decon_dpu_sat_ops = {
	.open = decon_dpu_sat_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write   = decon_dpu_sat_write,
};

static struct file_operations decon_dpu_hue_ops = {
	.open = decon_dpu_hue_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write   = decon_dpu_hue_write,
};

static struct file_operations decon_dpu_preset_ops = {
	.open = decon_dpu_preset_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write   = decon_dpu_preset_write,
};

#endif

static int decon_debug_event_open_sync(struct inode *inode, struct file *file)
{
	struct seq_file *p = NULL;

	if (!p) {
		p = kmalloc(sizeof(*p), GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		file->private_data = p;
	}
	memset(p, 0, sizeof(*p));

	p->private = inode->i_private;
	p->buf = kmalloc(PAGE_SIZE<<2, GFP_KERNEL | __GFP_NOWARN);
	p->size = PAGE_SIZE<<2;

	file->f_version = 0;
	file->f_mode &= ~FMODE_PWRITE;

	return 0;
}

static ssize_t decon_debug_event_read_sync(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct decon_device *decon = s->private;
	static int base_idx = DEFAULT_BASE_IDX;
	int n, ret;

	s->count = 0;
	s->from = 0;
	s->index = 0;
	s->read_pos = 0;

	if (base_idx == atomic_read(&decon->disp_ss_log_idx)) {
		wait_event_interruptible(decon->event_wait,
		(base_idx != atomic_read(&decon->disp_ss_log_idx)));
	}

	DISP_SS_EVENT_SHOW(s, decon, base_idx, true);
	base_idx = atomic_read(&decon->disp_ss_log_idx);

	n = min(s->count, size);

	ret = copy_to_user(buf, s->buf + (s->count - n), n);
	if (ret)
		return ret;

	return n;
}

static loff_t decon_debug_event_lseek_sync(struct file *file, loff_t offset, int whence)
{
	return 0;
}

static int decon_debug_event_release_sync(struct inode *inode, struct file *file)
{
	struct seq_file *s = file->private_data;
	struct decon_device *decon = s->private;

	wake_up_interruptible_all(&decon->event_wait);
	mdelay(100);

	kfree(s->buf);
	kfree(s);
	return 0;
}

static struct file_operations decon_event_sync_fops = {
	.open = decon_debug_event_open_sync,
	.read = decon_debug_event_read_sync,
	.llseek = decon_debug_event_lseek_sync,
	.release = decon_debug_event_release_sync,
};
#endif

#ifdef CONFIG_DECON_USE_BOOTLOADER_FB
static int decon_copy_bootloader_fb(struct platform_device *pdev,
		struct dma_buf *dest_buf)
{
	struct decon_device *decon = platform_get_drvdata(pdev);
	struct resource res;
	int ret = 0;
	size_t i;
	u32 offset;
	void *screen_base;

	res.start = decon->bl_fb_info.phy_addr;
	res.end = decon->bl_fb_info.phy_addr + decon->bl_fb_info.size - 1;

	ret = dma_buf_begin_cpu_access(dest_buf, 0, resource_size(&res),
			DMA_TO_DEVICE);
	if (ret < 0) {
		decon_err("failed to get framebuffer: %d\n", ret);
		goto err;
	}

	screen_base = decon->windows[decon->pdata->default_win]->fbinfo->screen_base;
	offset = ((decon->bl_fb_info.t * decon->lcd_info->xres) +
			decon->bl_fb_info.l)<<2;

	for (i = 0; i < resource_size(&res); i += PAGE_SIZE) {
		void *page = phys_to_page(res.start + i);
		void *from_virt = kmap(page);
		memcpy(screen_base + offset + i, from_virt, MIN((u32)resource_size(&res) - (u32)i, PAGE_SIZE));
		kunmap(page);
	}

	dma_buf_end_cpu_access(dest_buf, 0, resource_size(&res), DMA_TO_DEVICE);

err:
	if (memblock_free(res.start, resource_size(&res)))
		decon_err("failed to free bootloader FB memblock\n");

	return ret;
}

static int decon_display_bootloader_fb(struct decon_device *decon,
		int idx, dma_addr_t dma_addr)
{
	int ret = 0;
	struct decon_psr_info psr;
	int retry = 3;

	decon_to_psr_info(decon, &psr);

	do {
		decon_reg_per_frame_off(0);
		decon_reg_update_standalone(DECON_INT);
		ret = decon_reg_wait_stop_status_timeout(DECON_INT, 20 * 1000);
		if (ret)
			decon_warn("[%s] linecnt_is_zero timeout\n", __func__);
		else
			break;
	} while (retry--);

	if (retry == 0 && ret) {
		decon_warn("linecnt_is_zero timeout reached max retries.\n");

		decon_set_par(decon->windows[decon->pdata->default_win]->fbinfo);
		decon_reg_shadow_protect_win(DECON_INT, idx, 1);
		decon_write_mask(DECON_INT, WINCON(idx), 0, WINCON_ENWIN);
		decon_reg_shadow_protect_win(DECON_INT, idx, 0);
		decon_reg_update_standalone(DECON_INT);
		decon_reg_wait_for_update_timeout(DECON_INT, 300 * 1000);
		ret = iovmm_activate(decon->dev);

		decon_reg_shadow_protect_win(DECON_INT, idx, 1);
		decon_write_mask(DECON_INT, WINCON(idx), ~0, WINCON_ENWIN);
		decon_reg_shadow_protect_win(DECON_INT, idx, 0);
		decon_reg_update_standalone(DECON_INT);
		return 0;
	}

	decon_reg_shadow_protect_win(DECON_INT, idx, 1);
	decon_set_par(decon->windows[decon->pdata->default_win]->fbinfo);
	decon_reg_set_regs_data(DECON_INT, idx, &decon->win_regs);
	decon_reg_shadow_protect_win(DECON_INT, idx, 0);
	decon_reg_update_standalone(DECON_INT);

	iovmm_activate(decon->dev);
	decon_reg_start(DECON_INT, decon->pdata->dsi_mode, &psr);
	return 0;
}
#endif

static int decon_esd_panel_reset(struct decon_device *decon)
{
	int ret = 0;
	struct esd_protect *esd = &decon->esd;

	decon_info("++ %s\n", __func__);

	if (decon->state == DECON_STATE_OFF) {
		decon_warn("decon status is inactive\n");
		return ret;
	}

	flush_workqueue(decon->lpd_wq);

	decon_lpd_block_exit(decon);

	mutex_lock(&decon->output_lock);

	if (decon->pdata->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon->ignore_vsync = true;

	flush_kthread_worker(&decon->update_regs_worker);

	/* stop output device (mipi-dsi or hdmi) */
	ret = v4l2_subdev_call(decon->output_sd, video, s_stream, 0);
	if (ret) {
		decon_err("stopping stream failed for %s\n",
				decon->output_sd->name);
		goto reset_fail;
	}

	msleep(200);

	ret = v4l2_subdev_call(decon->output_sd, video, s_stream, 1);
	if (ret) {
		decon_err("starting stream failed for %s\n",
				decon->output_sd->name);
		goto reset_fail;
	}

	esd->queuework_pending = 0;

	if (decon->pdata->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon->ignore_vsync = false;

#ifdef CONFIG_FB_WINDOW_UPDATE
	decon->need_update = false;
	decon->update_win.x = 0;
	decon->update_win.y = 0;
	decon->update_win.w = decon->lcd_info->xres;
	decon->update_win.h = decon->lcd_info->yres;
#endif
#if 0
	if (decon->pdata->trig_mode == DECON_HW_TRIG)
		decon_reg_set_trigger(decon->id, decon->pdata->dsi_mode,
			decon->pdata->trig_mode, DECON_TRIG_ENABLE);

	ret = decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
	if (ret) {
		decon_err("%s:vsync time over\n", __func__);
		goto reset_exit;
	}

	if (decon->pdata->trig_mode == DECON_HW_TRIG)
		decon_reg_set_trigger(decon->id, decon->pdata->dsi_mode,
			decon->pdata->trig_mode, DECON_TRIG_DISABLE);
reset_exit:
#endif
	mutex_unlock(&decon->output_lock);

	decon_lpd_unblock(decon);

	decon_info("-- %s\n", __func__);

	return ret;

reset_fail:
	if (decon->pdata->psr_mode == DECON_MIPI_COMMAND_MODE)
		decon->ignore_vsync = false;

	mutex_unlock(&decon->output_lock);

	decon_lpd_unblock(decon);

	decon_info("--(e) %s\n", __func__);

	return ret;
}

static void decon_esd_handler(struct work_struct *work)
{
	int ret;
	struct esd_protect *esd;
	struct decon_device *decon;

	decon_info("esd : handler was called\n");

	esd = container_of(work, struct esd_protect, esd_work);
	decon = container_of(esd, struct decon_device, esd);

	if (decon->out_type == DECON_OUT_DSI) {
		ret = decon_esd_panel_reset(decon);
		if (ret)
			decon_err("%s : failed to panel reset", __func__);
	}

	return;
}

irqreturn_t decon_esd_pcd_handler(int irq, void *dev_id)
{
	struct esd_protect *esd;
	struct decon_device *decon = (struct decon_device *)dev_id;

	int level = 0;

	if (decon == NULL)
		goto handler_exit;

	esd = &decon->esd;
	level = gpio_get_value(esd->pcd_gpio);

	decon_info("%s: level: %d, state: %d\n",  __func__, level, decon->state);

	if (esd->pcd_pin_active != level)
		goto handler_exit;

	decon_info("%s: Detection panel crack. from now ignore vsync\n", __func__);

	decon->ignore_vsync = true;

handler_exit:
	return IRQ_HANDLED;
}


irqreturn_t decon_esd_err_handler(int irq, void *dev_id)
{
	struct esd_protect *esd;
	struct decon_device *decon = (struct decon_device *)dev_id;

	int level = 0;

	if (decon == NULL)
		goto handler_exit;

	esd = &decon->esd;
	level = gpio_get_value(esd->err_gpio);

	decon_info("%s: level: %d, state: %d, count: %d\n",  __func__, level, decon->state, decon->esd.err_count);

	if (esd->err_pin_active != level)
		goto handler_exit;

	if (decon->state == DECON_STATE_OFF)
		goto handler_exit;

	if ((esd->esd_wq) && (esd->queuework_pending == 0)) {
		esd->queuework_pending = 1;
		decon->esd.err_count++;
		queue_work(esd->esd_wq, &esd->esd_work);
	}
handler_exit:
	return IRQ_HANDLED;
}


irqreturn_t decon_disp_det_handler(int irq, void *dev_id)
{
	struct esd_protect *esd;
	struct decon_device *decon = (struct decon_device *)dev_id;

	int level = 0;

	if (decon == NULL)
		goto handler_exit;

	esd = &decon->esd;
	level = gpio_get_value(esd->disp_det_gpio);

	decon_info("%s: level: %d, state: %d, count: %d\n",  __func__, level, decon->state, decon->esd.det_count);

	if (esd->det_pin_active != level)
		goto handler_exit;

	if (decon->state == DECON_STATE_OFF)
		goto handler_exit;

	if ((esd->esd_wq) && (esd->queuework_pending == 0)) {
		esd->queuework_pending = 1;
		decon->esd.det_count++;
		queue_work(esd->esd_wq, &esd->esd_work);
	}
handler_exit:
	return IRQ_HANDLED;
}


static int decon_register_esd_funcion(struct decon_device *decon)
{
	int gpio;
	int ret = 0;
	struct esd_protect *esd;
	struct device *dev = decon->dev;
	enum of_gpio_flags flags;
	unsigned int pcd_irqf_type = IRQF_TRIGGER_RISING;
	unsigned int err_irqf_type = IRQF_TRIGGER_RISING;
	unsigned int det_irqf_type = IRQF_TRIGGER_RISING;

	esd = &decon->esd;

	decon_info("%s +\n", __func__);

	esd->pcd_irq = 0;
	esd->err_irq = 0;
	esd->pcd_gpio = 0;
	esd->disp_det_gpio = 0;

	gpio = of_get_named_gpio_flags(dev->of_node, "gpio_pcd", 0, &flags);
	if (gpio_is_valid(gpio)) {
		decon_info("%s: found gpio_pcd(%d) success\n", __func__, gpio);
		esd->pcd_irq = gpio_to_irq(gpio);
		esd->pcd_gpio = gpio;
		esd->pcd_pin_active = !(flags & OF_GPIO_ACTIVE_LOW);
		pcd_irqf_type = (flags & OF_GPIO_ACTIVE_LOW) ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
		decon_info("%s: pcd_active is %s, %s\n", __func__,  esd->pcd_pin_active ? "high" : "low",
			(pcd_irqf_type == IRQF_TRIGGER_RISING) ? "rising" : "falling");
		ret++;

		if (esd->pcd_pin_active == gpio_get_value(esd->pcd_gpio)) {
			decon_info("%s: pcd(%d) is already %s(%d)\n", __func__, esd->pcd_gpio,
				(esd->pcd_pin_active) ? "high" : "low", gpio_get_value(esd->pcd_gpio));
		}
	}

	gpio = of_get_named_gpio_flags(dev->of_node, "gpio_err", 0, &flags);
	if (gpio_is_valid(gpio)) {
		decon_info("%s: found gpio_err(%d) success\n", __func__, gpio);
		esd->err_irq = gpio_to_irq(gpio);
		esd->err_gpio = gpio;
		esd->err_pin_active = !(flags & OF_GPIO_ACTIVE_LOW);
		err_irqf_type = (flags & OF_GPIO_ACTIVE_LOW) ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
		decon_info("%s: err_active is %s, %s\n", __func__, esd->err_pin_active ? "high" : "low",
			(err_irqf_type == IRQF_TRIGGER_RISING) ? "rising" : "falling");
		ret++;

		if (esd->err_pin_active == gpio_get_value(esd->err_gpio)) {
			decon_info("%s: err(%d) is already %s(%d)\n", __func__, esd->err_gpio,
				(esd->err_pin_active) ? "high" : "low", gpio_get_value(esd->err_gpio));
		}
	}

	gpio = of_get_named_gpio_flags(dev->of_node, "gpio_det", 0, &flags);
	if (gpio_is_valid(gpio)) {
		decon_info("%s: found display_det(%d) sueccess\n", __func__, gpio);
		esd->disp_det_irq = gpio_to_irq(gpio);
		esd->disp_det_gpio = gpio;
		esd->det_pin_active = !(flags & OF_GPIO_ACTIVE_LOW);
		det_irqf_type = (flags & OF_GPIO_ACTIVE_LOW) ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
		decon_info("%s: det_active is %s, %s\n", __func__, esd->det_pin_active ? "high" : "low",
			(det_irqf_type == IRQF_TRIGGER_RISING) ? "rising" : "falling");
		ret++;

		if (esd->det_pin_active == gpio_get_value(esd->disp_det_gpio)) {
			decon_info("%s: det(%d) is already %s(%d)\n", __func__, esd->disp_det_gpio,
				(esd->det_pin_active) ? "high" : "low", gpio_get_value(esd->disp_det_gpio));
		}
	}

	if (ret == 0)
		goto register_exit;

	if (esd->err_irq || esd->disp_det_irq) {
		esd->esd_wq = create_singlethread_workqueue("decon_esd");

		if (esd->esd_wq)
			INIT_WORK(&esd->esd_work, decon_esd_handler);
	}

	if (esd->pcd_irq) {
		if (devm_request_irq(dev, esd->pcd_irq, decon_esd_pcd_handler,
				pcd_irqf_type, "pcd-irq", decon)) {
			dsim_err("%s : failed to request irq for pcd\n", __func__);
			esd->pcd_irq = 0;
			ret--;
		}
		disable_irq_nosync(esd->pcd_irq);
	}
	if (esd->err_irq) {
		if (devm_request_irq(dev, esd->err_irq, decon_esd_err_handler,
				err_irqf_type, "err-irq", decon)) {
			dsim_err("%s : failed to request irq for err_fg\n", __func__);
			esd->err_irq = 0;
			ret--;
		}
		disable_irq_nosync(esd->err_irq);
	}
	if (esd->disp_det_irq) {
		if (devm_request_irq(dev, esd->disp_det_irq, decon_disp_det_handler,
				det_irqf_type, "display-det", decon)) {
			dsim_err("%s : failed to request irq for display det\n", __func__);
			esd->disp_det_irq = 0;
			ret--;
		}
		disable_irq_nosync(esd->disp_det_irq);
	}

	esd->queuework_pending = 0;

register_exit:
	decon_info("%s -\n", __func__);
	return ret;

}

/* ---------- TUI INTERFACE ----------- */
int decon_tui_protection(struct decon_device *decon, bool tui_en)
{
	int ret = 0;
	int i;
	struct decon_psr_info psr;

	decon_warn("%s:state %d: out_type %d:+\n", __func__,
		tui_en, decon->out_type);
	mutex_lock(&decon->output_lock);
	if (decon->state == DECON_STATE_OFF) {
		decon_warn("%s: decon is already disabled(tui=%d)\n", __func__, tui_en);
		decon->out_type = DECON_OUT_DSI;
		mutex_unlock(&decon->output_lock);
		/* UnBlocking LPD */
		decon_lpd_unblock(decon);
		return -EBUSY;
	}

	mutex_unlock(&decon->output_lock);

	if (tui_en) {
		/* Blocking LPD */
		decon_lpd_block_exit(decon);
		mutex_lock(&decon->output_lock);
		flush_kthread_worker(&decon->update_regs_worker);

		decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
		/* disable all the windows */
		for (i = 0; i < decon->pdata->max_win; i++)
			decon_write(DECON_INT, WINCON(i), 0);
		#ifdef CONFIG_FB_WINDOW_UPDATE
		/* Restore window_partial_update */
		if (decon->need_update) {
			decon->update_win.x = 0;
			decon->update_win.y = 0;
			decon->update_win.w = decon->lcd_info->xres;
			decon->update_win.h = decon->lcd_info->yres;
			decon_reg_ddi_partial_cmd(decon, &decon->update_win);
			decon_win_update_disp_config(decon, &decon->update_win);
			decon->need_update = false;
			}
		#endif
		decon_to_psr_info(decon, &psr);
		if (decon->pdata->trig_mode == DECON_HW_TRIG)
			decon_reg_set_trigger(DECON_INT, decon->pdata->dsi_mode,
				decon->pdata->trig_mode, DECON_TRIG_DISABLE);
		decon_reg_per_frame_off(DECON_INT);
		decon_reg_update_standalone(DECON_INT);
		decon->out_type = DECON_OUT_TUI;
		decon->prev_bw = 0;
		/* set bandwidth to default (3 full frame) */
		decon_set_qos(decon, NULL, false, false);
		mutex_unlock(&decon->output_lock);
	}else {
		mutex_lock(&decon->output_lock);
		decon->out_type = DECON_OUT_DSI;
		mutex_unlock(&decon->output_lock);
		/* UnBlocking LPD */
		decon_lpd_unblock(decon);
	}

	decon_warn("%s:state %d: out_type %d:-\n", __func__,
		tui_en, decon->out_type);

	return ret;
}


/* --------- DRIVER INITIALIZATION ---------- */
static int decon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct decon_device *decon;
	struct resource *res;
	struct fb_info *fbinfo;
	int ret = 0;
	char device_name[MAX_NAME_SIZE], debug_name[MAX_NAME_SIZE];
	struct decon_psr_info psr;
	struct decon_init_param p;
	struct decon_regs_data win_regs;
	struct dsim_device *dsim = NULL;
	struct panel_private *priv = NULL;
	struct exynos_md *md;
	struct device_node *cam_stat;
	int win_idx = 0;
	int retry = 5;

	dev_info(dev, "%s start\n", __func__);

	decon = devm_kzalloc(dev, sizeof(struct decon_device), GFP_KERNEL);
	if (!decon) {
		decon_err("no memory for decon device\n");
		return -ENOMEM;
	}

	/* setup pointer to master device */
	decon->dev = dev;
	decon->pdata = devm_kzalloc(dev, sizeof(struct exynos_decon_platdata),
						GFP_KERNEL);
	if (!decon->pdata) {
		decon_err("no memory for DECON platdata\n");
		kfree(decon);
		return -ENOMEM;
	}

	/* store platform data ptr to decon_tv context */
	decon_parse_pdata(decon, dev);
	win_idx = decon->pdata->default_win;

	/* init clock setting for decon */
	decon_int_drvdata = decon;
	decon_int_get_clocks(decon);

	/* Get memory resource and map SFR region. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	decon->regs = devm_request_and_ioremap(dev, res);
	if (decon->regs == NULL) {
		decon_err("failed to claim register region\n");
		goto fail_kfree;
	}

	spin_lock_init(&decon->slock);
	init_waitqueue_head(&decon->vsync_info.wait);
	init_waitqueue_head(&decon->wait_frmdone);
	mutex_init(&decon->vsync_info.irq_lock);

	snprintf(device_name, MAX_NAME_SIZE, "decon%d", DECON_INT);
	decon->timeline = sw_sync_timeline_create(device_name);
	decon->timeline_max = 1;

	/* Get IRQ resource and register IRQ, create thread */
	ret = decon_int_register_irq(pdev, decon);
	if (ret)
			goto fail_irq_mutex;
	ret = decon_int_create_vsync_thread(decon);
	if (ret)
			goto fail_irq_mutex;
	ret = decon_int_create_psr_thread(decon);
	if (ret)
		goto fail_vsync_thread;
	ret = decon_fb_config_eint_for_te(pdev, decon);
	if (ret)
		goto fail_psr_thread;
	ret = decon_int_register_lpd_work(decon);
	if (ret)
		goto fail_psr_thread;

	snprintf(debug_name, MAX_NAME_SIZE, "decon");
	decon->debug_root = debugfs_create_dir(debug_name, NULL);
	if (!decon->debug_root) {
		decon_err("failed to create debugfs root directory.\n");
		goto fail_lpd_work;
	}

	decon->ion_client = ion_client_create(ion_exynos, device_name);
	if (IS_ERR(decon->ion_client)) {
		decon_err("failed to ion_client_create\n");
		goto fail_lpd_work;
	}

#ifdef CONFIG_DECON_EVENT_LOG
	snprintf(debug_name, MAX_NAME_SIZE, "event%d", DECON_INT);
	atomic_set(&decon->disp_ss_log_idx, -1);
	decon->debug_event = debugfs_create_file(debug_name, 0444,
						decon->debug_root,
						decon, &decon_event_fops);

	init_waitqueue_head(&decon->event_wait);

	snprintf(debug_name, MAX_NAME_SIZE, "event%d_sync", DECON_INT);
	atomic_set(&decon->disp_ss_log_idx, -1);
	decon->debug_event = debugfs_create_file(debug_name, 0444,
						decon->debug_root,
						decon, &decon_event_sync_fops);

	decon->disp_ss_log_unmask = EVT_TYPE_INT | EVT_TYPE_IOCTL |
		EVT_TYPE_ASYNC_EVT | EVT_TYPE_PM;

	decon->mask = debugfs_create_u32("unmask", 0644, decon->debug_root,
			(u32 *)&decon->disp_ss_log_unmask);
#endif

#if defined(CONFIG_EXYNOS_DECON_DPU)
	snprintf(debug_name, MAX_NAME_SIZE, "scr");
	decon->dpu_set = debugfs_create_file(debug_name, 0644,
						decon->debug_root,
						decon, &decon_dpu_scr_ops);


	snprintf(debug_name, MAX_NAME_SIZE, "gamma");
	decon->dpu_set = debugfs_create_file(debug_name, 0644,
						decon->debug_root,
						decon, &decon_dpu_gamma_ops);

	snprintf(debug_name, MAX_NAME_SIZE, "gamma_chunk");
	decon->dpu_set = debugfs_create_file(debug_name, 0444,
						decon->debug_root,
						decon, &decon_dpu_gamma_chunk_ops);

	snprintf(debug_name, MAX_NAME_SIZE, "saturation");
	decon->dpu_set = debugfs_create_file(debug_name, 0644,
						decon->debug_root,
						decon, &decon_dpu_sat_ops);

	snprintf(debug_name, MAX_NAME_SIZE, "hue");
	decon->dpu_set = debugfs_create_file(debug_name, 0644,
						decon->debug_root,
						decon, &decon_dpu_hue_ops);

	snprintf(debug_name, MAX_NAME_SIZE, "preset_normal");
	decon->dpu_set = debugfs_create_file(debug_name, 0444,
						decon->debug_root,
						decon, &decon_dpu_preset_ops);
#endif

	/* register internal and external DECON as entity */
	ret = decon_register_entity(decon);
	if (ret)
		goto fail_ion_create;

	decon_to_psr_info(decon, &psr);
	decon_to_init_param(decon, &p);

	/* if decon already running in video mode but no bootloader fb info, stop decon */
	if (decon_reg_get_stop_status(DECON_INT) &&
			psr.psr_mode == DECON_VIDEO_MODE &&
			decon_is_no_bootloader_fb(decon)) {
		decon_reg_init_probe(DECON_INT, decon->pdata->dsi_mode, &p);

		do {
			decon_reg_update_standalone(DECON_INT);
			decon_reg_per_frame_off(0);
			ret = decon_reg_wait_linecnt_is_zero_timeout(0 , 0, 20000);
			if (ret)
				decon_warn("[%s] linecnt_is_zero timeout\n", __func__);
			else
				break;
		} while (retry--);
	}

	/* if command mode or video mode without bootloader framebuffer, enable iovmm */
	if (decon->pdata->psr_mode == DECON_MIPI_COMMAND_MODE ||
		decon_is_no_bootloader_fb(decon)) {
		ret = iovmm_activate(decon->dev);
		if (ret < 0) {
			decon_err("failed to reactivate vmm\n");
			goto fail_entity;
		}
	}

	/* configure windows */
	ret = decon_acquire_window(decon);
	if (ret)
		goto fail_iovmm;

	md = (struct exynos_md *)module_name_to_driver_data(MDEV_MODULE_NAME);
	if (!md) {
		decon_err("failed to get output media device\n");
		goto fail_iovmm;
	}

	decon->mdev = md;

	/* link creation: vpp <-> decon / decon <-> output */
	ret = decon_create_links(decon, md);
	if (ret)
		goto fail_iovmm;

	ret = decon_register_subdev_nodes(decon, md);
	if (ret)
		goto fail_iovmm;


	/* register framebuffer */
	fbinfo = decon->windows[decon->pdata->default_win]->fbinfo;
	ret = register_framebuffer(fbinfo);
	if (ret < 0) {
		decon_err("failed to register framebuffer\n");
		goto fail_iovmm;
	}

	/* mutex mechanism */
	mutex_init(&decon->output_lock);
	mutex_init(&decon->mutex);

	/* init work thread for update registers */
	INIT_LIST_HEAD(&decon->update_regs_list);
	mutex_init(&decon->update_regs_list_lock);
	init_kthread_worker(&decon->update_regs_worker);

	decon->update_regs_thread = kthread_run(kthread_worker_fn,
			&decon->update_regs_worker, device_name);
	if (IS_ERR(decon->update_regs_thread)) {
		ret = PTR_ERR(decon->update_regs_thread);
		decon->update_regs_thread = NULL;
		decon_err("failed to run update_regs thread\n");
		goto fail_output_lock;
	}
	init_kthread_work(&decon->update_regs_work, decon_update_regs_handler);

	ret = decon_int_set_lcd_config(decon);
	if (ret) {
		decon_err("failed to set lcd information\n");
		goto fail_thread;
	}
	platform_set_drvdata(pdev, decon);
	pm_runtime_enable(dev);


	decon->max_win_bw = decon_calc_bandwidth(decon->lcd_info->xres,
			decon->lcd_info->yres, 4,
			decon->lcd_info->fps);

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_get_sync(decon->dev);
#else
	decon_runtime_resume(decon->dev);
#endif

	/* DECON does not need to start, if DECON is already
	 * running(enabled in LCD_ON_UBOOT) */
	if (decon_reg_get_stop_status(DECON_INT)) {
#ifdef CONFIG_DECON_USE_BOOTLOADER_FB
		if (decon->pdata->psr_mode == DECON_VIDEO_MODE &&
				!decon_is_no_bootloader_fb(decon)) {
			/* video mode with bootloader framebuffer, show bootloader fb */
			int w = decon->pdata->default_win;
			ret = decon_copy_bootloader_fb(pdev,
				decon->windows[w]->dma_buf_data[0].dma_buf);
			if (!ret) {
				/* copied successfully, now display */
				ret = decon_display_bootloader_fb(decon, win_idx,
				decon->windows[w]->dma_buf_data[0].dma_addr);

				if (ret < 0) {
					decon_err("failed to reactivate vmm\n");
					goto fail_thread;
				}
				goto decon_init_done;
			}

			/* copy failed, activate iommu and skip */
			decon_err("failed to copy bootloader fb\n");
			decon_reg_init_probe(DECON_INT, decon->pdata->dsi_mode, &p);
			if (decon->pdata->trig_mode == DECON_HW_TRIG)
				decon_reg_set_trigger(DECON_INT, decon->pdata->dsi_mode,
					decon->pdata->trig_mode, DECON_TRIG_DISABLE);

			decon_reg_per_frame_off(DECON_INT);
			decon_reg_wait_linecnt_is_zero_timeout(0 , 0, 20000);

			ret = iovmm_activate(decon->dev);
			if (ret < 0) {
				decon_err("failed to reactivate vmm\n");
				goto fail_thread;
			}
			goto decon_init_done;
		}
#endif
		decon_reg_init_probe(DECON_INT, decon->pdata->dsi_mode, &p);
		if (decon->pdata->trig_mode == DECON_HW_TRIG)
			decon_reg_set_trigger(DECON_INT, decon->pdata->dsi_mode,
				decon->pdata->trig_mode, DECON_TRIG_DISABLE);
		goto decon_init_done;
	}

	decon_reg_shadow_protect_win(DECON_INT, win_idx, 1);

	decon_reg_init(DECON_INT, decon->pdata->dsi_mode, &p);

	win_regs.wincon = WINCON_BPPMODE_ARGB8888;
	win_regs.winmap = 0x0;
	win_regs.vidosd_a = vidosd_a(0, 0);
	win_regs.vidosd_b = vidosd_b(0, 0, fbinfo->var.xres, fbinfo->var.yres);
	win_regs.vidosd_c = vidosd_c(0x0, 0x0, 0x0);
	win_regs.vidosd_d = vidosd_d(0xff, 0xff, 0xff);
	win_regs.vidw_buf_start = fbinfo->fix.smem_start;
	win_regs.vidw_whole_w = fbinfo->var.xres_virtual;
	win_regs.vidw_whole_h = fbinfo->var.yres_virtual;
	win_regs.vidw_offset_x = fbinfo->var.xoffset;
	win_regs.vidw_offset_y = fbinfo->var.yoffset;
	win_regs.type = IDMA_G0;

	decon_reg_set_regs_data(DECON_INT, win_idx, &win_regs);

	decon_reg_shadow_protect_win(DECON_INT, win_idx, 0);

	decon_reg_start(DECON_INT, decon->pdata->dsi_mode, &psr);

	decon_reg_activate_window(DECON_INT, win_idx);

	decon_reg_set_winmap(DECON_INT, win_idx, 0x000000 /* black */, 1);

	if (decon->pdata->trig_mode == DECON_HW_TRIG)
		decon_reg_set_trigger(DECON_INT, decon->pdata->dsi_mode,
				decon->pdata->trig_mode, DECON_TRIG_ENABLE);

	dsim = container_of(decon->output_sd, struct dsim_device, sd);
	call_panel_ops(dsim, displayon, dsim);

decon_init_done:
	decon->ignore_vsync = false;

	ret = decon_register_esd_funcion(decon);
	decon_info("esd: %d entity was registered\n", ret);

	if (dsim == NULL)
		dsim = container_of(decon->output_sd, struct dsim_device, sd);

	if (dsim) {
		priv = &dsim->priv;
		if ((priv) && (!priv->lcdConnected)) {
			decon_info("decon does not found panel\n");
			decon->ignore_vsync = true;
		}
		decon_info("panel id : %x\n", lcdtype);
	}

#ifdef CONFIG_DECON_MIPI_DSI_PKTGO
	ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
				DSIM_IOC_PKT_GO_ENABLE, NULL);
	if (ret)
		decon_err("Failed to call DSIM packet go enable\n");
#endif
	decon->state = DECON_STATE_INIT;

	/* [W/A] prevent sleep enter during LCD on */
	ret = device_init_wakeup(decon->dev, true);
	if (ret) {
		dev_err(decon->dev, "failed to init wakeup device\n");
		goto fail_thread;
	}

	pm_stay_awake(decon->dev);
	dev_warn(decon->dev, "pm_stay_awake");
	cam_stat = of_get_child_by_name(decon->dev->of_node, "cam-stat");
	if (!cam_stat) {
		decon_info("No DT node for cam-stat\n");
	} else {
		decon->cam_status[0] = of_iomap(cam_stat, 0);
		if (!decon->cam_status[0])
			decon_info("Failed to get CAM0-STAT Reg\n");
	}

#ifdef CONFIG_CPU_IDLE
	decon->lpc_nb = exynos_decon_lpc_nb;
	exynos_pm_register_notifier(&decon->lpc_nb);
#endif

#if defined(CONFIG_EXYNOS_DECON_DPU)
	dpu_reg_start(decon->lcd_info->xres, decon->lcd_info->yres);
	dpu_reg_gamma_init();
#endif
	decon_esd_enable_interrupt(decon);

	decon_info("decon registered successfully\n");

	return 0;

fail_thread:
	if (decon->update_regs_thread)
		kthread_stop(decon->update_regs_thread);

fail_output_lock:
	mutex_destroy(&decon->output_lock);
	mutex_destroy(&decon->mutex);

fail_iovmm:
	iovmm_deactivate(dev);

fail_entity:
	decon_unregister_entity(decon);

fail_ion_create:
	ion_client_destroy(decon->ion_client);

fail_lpd_work:
	if (decon->lpd_wq)
		destroy_workqueue(decon->lpd_wq);

	mutex_destroy(&decon->lpd_lock);

fail_psr_thread:
	decon_int_destroy_psr_thread(decon);

fail_vsync_thread:
	if (decon->vsync_info.thread)
		kthread_stop(decon->vsync_info.thread);

	decon_int_destroy_vsync_thread(decon);

fail_irq_mutex:
	mutex_destroy(&decon->vsync_info.irq_lock);

fail_kfree:
	kfree(decon->pdata);
	kfree(decon);

	decon_err("decon probe fail");
	return ret;
}

static int decon_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct decon_device *decon = platform_get_drvdata(pdev);
	int i;

	pm_runtime_disable(dev);
	decon_put_clocks(decon);

	iovmm_deactivate(dev);
	unregister_framebuffer(decon->windows[0]->fbinfo);

	if (decon->update_regs_thread)
		kthread_stop(decon->update_regs_thread);

	for (i = 0; i < decon->pdata->max_win; i++)
		decon_release_windows(decon->windows[i]);

	debugfs_remove_recursive(decon->debug_root);
	kfree(decon);

	decon_info("remove sucessful\n");
	return 0;
}

static void decon_shutdown(struct platform_device *pdev)
{
	int ret;
	//struct device *dev = &pdev->dev;
	struct decon_device *decon = platform_get_drvdata(pdev);

	decon_info("+ %s \n", __func__);


	decon_lpd_block_exit(decon);

	if (decon->state != DECON_STATE_OFF) {
		disable_irq(decon->irq);
		ret = decon_disable(decon);
		if (ret) {
			dsim_err("%s : failed to decon_disable\n", __func__);
		}
	}

	decon_info("+ %s\n", __func__);
	return;
}

static struct platform_driver decon_driver __refdata = {
	.probe		= decon_probe,
	.remove		= decon_remove,
	.shutdown 	= decon_shutdown,
	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.pm	= &decon_pm_ops,
		.of_match_table = of_match_ptr(decon_device_table),
	}
};

static int exynos_decon_register(void)
{
	platform_driver_register(&decon_driver);

	return 0;
}

static void exynos_decon_unregister(void)
{
	platform_driver_unregister(&decon_driver);
}
late_initcall(exynos_decon_register);
module_exit(exynos_decon_unregister);

MODULE_AUTHOR("Ayoung Sim <a.sim@samsung.com>");
MODULE_DESCRIPTION("Samsung EXYNOS Soc DECON driver");
MODULE_LICENSE("GPL");
