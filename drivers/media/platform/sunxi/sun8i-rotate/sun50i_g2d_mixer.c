// SPDX-License-Identifier: GPL-2.0-only
/* A133 G2D mixer. Filter coefficients copyright (C) 2016 Allwinner. */
#include <linux/io.h>
#include <linux/math64.h>

#include "sun8i-rotate.h"
#include "sun8i-formats.h"

#define G2D_MIXER_CTL	0x0100
#define G2D_MIXER_INT	0x0104
#define G2D_BLD_EN_CTL	0x0400
#define G2D_BLD_CH_ISIZE0	0x0420
#define G2D_BLD_SIZE	0x0448
#define G2D_BLD_OUT_COLOR	0x0460
#define G2D_ROP_CTL	0x0480
#define G2D_CSC_CTL	0x0500
#define G2D_CSC2_COEFF	0x0570
#define G2D_V0_ATTCTL	0x0800
#define G2D_V0_MBSIZE	0x0804
#define G2D_V0_PITCH0	0x080c
#define G2D_V0_LADD0	0x0818
#define G2D_V0_HADD	0x0828
#define G2D_V0_SIZE	0x082c
#define G2D_V0_HDS_CTL0	0x0830
#define G2D_V0_HDS_CTL1	0x0834
#define G2D_V0_VDS_CTL0	0x0838
#define G2D_V0_VDS_CTL1	0x083c
#define G2D_WB_ATT	0x3000
#define G2D_WB_SIZE	0x3004
#define G2D_WB_PITCH0	0x3008
#define G2D_WB_LADD0	0x3014
#define G2D_WB_HADD0	0x3018
#define G2D_VS_CTRL	0x8000
#define G2D_VS_OUT_SIZE	0x8040
#define G2D_VS_GLB_ALPHA	0x8044
#define G2D_VS_Y_SIZE	0x8080
#define G2D_VS_Y_HSTEP	0x8088
#define G2D_VS_Y_VSTEP	0x808c
#define G2D_VS_Y_HPHASE	0x8090
#define G2D_VS_Y_VPHASE0	0x8098
#define G2D_VS_C_SIZE	0x80c0
#define G2D_VS_C_HSTEP	0x80c8
#define G2D_VS_C_VSTEP	0x80cc
#define G2D_VS_C_HPHASE	0x80d0
#define G2D_VS_C_VPHASE0	0x80d8
#define G2D_VS_Y_HCOEF0	0x8200
#define G2D_VS_Y_VCOEF0	0x8300
#define G2D_VS_C_HCOEF0	0x8400

static const u32 g2d_lanczos[512] = {
	0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd, 0x00063efc, 0xff083dfc,
	0x000a3bfb, 0xff0d39fb, 0xff0f37fb, 0xff1136fa, 0xfe1433fb,
	0xfe1631fb, 0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb, 0xfc2127fc,
	0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd, 0xfb2f19fd,
	0xfb3116fe, 0xfb3314fe, 0xfa3611ff, 0xfb370fff, 0xfb390dff,
	0xfb3b0a00, 0xfc3d08ff, 0xfc3e0600, 0xfd3f0400, 0xfe3f0300,
	0xff400100,
	/* counter = 1 */
	0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd, 0x00063efc,
	0xff083dfc, 0x000a3bfb, 0xff0d39fb, 0xff0f37fb, 0xff1136fa,
	0xfe1433fb, 0xfe1631fb, 0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb,
	0xfc2127fc, 0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd,
	0xfb2f19fd, 0xfb3116fe, 0xfb3314fe, 0xfa3611ff, 0xfb370fff,
	0xfb390dff, 0xfb3b0a00, 0xfc3d08ff, 0xfc3e0600, 0xfd3f0400,
	0xfe3f0300, 0xff400100,
	/* counter = 2 */
	0xff053804, 0xff063803, 0xff083801, 0xff093701, 0xff0a3700,
	0xff0c3500, 0xff0e34ff, 0xff1033fe, 0xff1232fd, 0xfe1431fd,
	0xfe162ffd, 0xfe182dfd, 0xfd1b2cfc, 0xfd1d2afc, 0xfd1f28fc,
	0xfd2126fc, 0xfd2323fd, 0xfc2621fd, 0xfc281ffd, 0xfc2a1dfd,
	0xfc2c1bfd, 0xfd2d18fe, 0xfd2f16fe, 0xfd3114fe, 0xfd3212ff,
	0xfe3310ff, 0xff340eff, 0x00350cff, 0x00360a00, 0x01360900,
	0x02370700, 0x03370600,
	/* counter = 3 */
	0xff083207, 0xff093206, 0xff0a3205, 0xff0c3203, 0xff0d3103,
	0xff0e3102, 0xfe113001, 0xfe132f00, 0xfe142e00, 0xfe162dff,
	0xfe182bff, 0xfe192aff, 0xfe1b29fe, 0xfe1d27fe, 0xfe1f25fe,
	0xfd2124fe, 0xfe2222fe, 0xfe2421fd, 0xfe251ffe, 0xfe271dfe,
	0xfe291bfe, 0xff2a19fe, 0xff2b18fe, 0xff2d16fe, 0x002e14fe,
	0x002f12ff, 0x013010ff, 0x02300fff, 0x03310dff, 0x04310cff,
	0x05310a00, 0x06310900,
	/* counter = 4 */
	0xff0a2e09, 0xff0b2e08, 0xff0c2e07, 0xff0e2d06, 0xff0f2d05,
	0xff102d04, 0xff122c03, 0xfe142c02, 0xfe152b02, 0xfe172a01,
	0xfe182901, 0xfe1a2800, 0xfe1b2700, 0xfe1d2500, 0xff1e24ff,
	0xfe2023ff, 0xff2121ff, 0xff2320fe, 0xff241eff, 0x00251dfe,
	0x00261bff, 0x00281afe, 0x012818ff, 0x012a16ff, 0x022a15ff,
	0x032b13ff, 0x032c12ff, 0x052c10ff, 0x052d0fff, 0x062d0d00,
	0x072d0c00, 0x082d0b00,
	/* counter = 5 */
	0xff0c2a0b, 0xff0d2a0a, 0xff0e2a09, 0xff0f2a08, 0xff102a07,
	0xff112a06, 0xff132905, 0xff142904, 0xff162803, 0xff172703,
	0xff182702, 0xff1a2601, 0xff1b2501, 0xff1c2401, 0xff1e2300,
	0xff1f2200, 0x00202000, 0x00211f00, 0x01221d00, 0x01231c00,
	0x01251bff, 0x02251aff, 0x032618ff, 0x032717ff, 0x042815ff,
	0x052814ff, 0x052913ff, 0x06291100, 0x072a10ff, 0x082a0e00,
	0x092a0d00, 0x0a2a0c00,
	/* counter = 6 */
	0xff0d280c, 0xff0e280b, 0xff0f280a, 0xff102809, 0xff112808,
	0xff122708, 0xff142706, 0xff152705, 0xff162605, 0xff172604,
	0xff192503, 0xff1a2403, 0x001b2302, 0x001c2202, 0x001d2201,
	0x001e2101, 0x011f1f01, 0x01211e00, 0x01221d00, 0x02221c00,
	0x02231b00, 0x03241900, 0x04241800, 0x04251700, 0x052616ff,
	0x06261400, 0x072713ff, 0x08271100, 0x08271100, 0x09271000,
	0x0a280e00, 0x0b280d00,
	/* counter = 7 */
	0xff0e260d, 0xff0f260c, 0xff10260b, 0xff11260a, 0xff122609,
	0xff132608, 0xff142508, 0xff152507, 0x00152506, 0x00172405,
	0x00182305, 0x00192304, 0x001b2203, 0x001c2103, 0x011d2002,
	0x011d2002, 0x011f1f01, 0x021f1e01, 0x02201d01, 0x03211c00,
	0x03221b00, 0x04221a00, 0x04231801, 0x05241700, 0x06241600,
	0x07241500, 0x08251300, 0x09251200, 0x09261100, 0x0a261000,
	0x0b260f00, 0x0c260e00,
	/* counter = 8 */
	0xff0e250e, 0xff0f250d, 0xff10250c, 0xff11250b, 0x0011250a,
	0x00132409, 0x00142408, 0x00152407, 0x00162307, 0x00172306,
	0x00182206, 0x00192205, 0x011a2104, 0x011b2004, 0x011c2003,
	0x021c1f03, 0x021e1e02, 0x031e1d02, 0x03201c01, 0x04201b01,
	0x04211a01, 0x05221900, 0x05221801, 0x06231700, 0x07231600,
	0x07241500, 0x08241400, 0x09241300, 0x0a241200, 0x0b241100,
	0x0c241000, 0x0d240f00,
	/* counter = 9 */
	0x000e240e, 0x000f240d, 0x0010240c, 0x0011240b, 0x0013230a,
	0x0013230a, 0x00142309, 0x00152308, 0x00162208, 0x00172207,
	0x01182106, 0x01192105, 0x011a2005, 0x021b1f04, 0x021b1f04,
	0x021d1e03, 0x031d1d03, 0x031e1d02, 0x041e1c02, 0x041f1b02,
	0x05201a01, 0x05211901, 0x06211801, 0x07221700, 0x07221601,
	0x08231500, 0x09231400, 0x0a231300, 0x0a231300, 0x0b231200,
	0x0c231100, 0x0d231000,
	/* counter = 10 */
	0x000f220f, 0x0010220e, 0x0011220d, 0x0012220c, 0x0013220b,
	0x0013220b, 0x0015210a, 0x0015210a, 0x01162108, 0x01172008,
	0x01182007, 0x02191f06, 0x02191f06, 0x021a1e06, 0x031a1e05,
	0x031c1d04, 0x041c1c04, 0x041d1c03, 0x051d1b03, 0x051e1a03,
	0x061f1902, 0x061f1902, 0x07201801, 0x08201701, 0x08211601,
	0x09211501, 0x0a211500, 0x0b211400, 0x0b221300, 0x0c221200,
	0x0d221100, 0x0e221000,
	/* counter = 11 */
	0x0010210f, 0x0011210e, 0x0011210e, 0x0012210d, 0x0013210c,
	0x0014200c, 0x0114200b, 0x0115200a, 0x01161f0a, 0x01171f09,
	0x02171f08, 0x02181e08, 0x03181e07, 0x031a1d06, 0x031a1d06,
	0x041b1c05, 0x041c1c04, 0x051c1b04, 0x051d1a04, 0x061d1a03,
	0x071d1903, 0x071e1803, 0x081e1802, 0x081f1702, 0x091f1602,
	0x0a201501, 0x0b1f1501, 0x0b201401, 0x0c211300, 0x0d211200,
	0x0e201200, 0x0e211100,
	/* counter = 12 */
	0x00102010, 0x0011200f, 0x0012200e, 0x0013200d, 0x0013200d,
	0x01141f0c, 0x01151f0b, 0x01151f0b, 0x01161f0a, 0x02171e09,
	0x02171e09, 0x03181d08, 0x03191d07, 0x03191d07, 0x041a1c06,
	0x041b1c05, 0x051b1b05, 0x051c1b04, 0x061c1a04, 0x071d1903,
	0x071d1903, 0x081d1803, 0x081e1703, 0x091e1702, 0x0a1f1601,
	0x0a1f1502, 0x0b1f1501, 0x0c1f1401, 0x0d201300, 0x0d201300,
	0x0e201200, 0x0f201100,
	/* counter = 13 */
	0x00102010, 0x0011200f, 0x00121f0f, 0x00131f0e, 0x00141f0d,
	0x01141f0c, 0x01141f0c, 0x01151e0c, 0x02161e0a, 0x02171e09,
	0x03171d09, 0x03181d08, 0x03181d08, 0x04191c07, 0x041a1c06,
	0x051a1b06, 0x051b1b05, 0x061b1a05, 0x061c1a04, 0x071c1904,
	0x081c1903, 0x081d1803, 0x091d1703, 0x091e1702, 0x0a1e1602,
	0x0b1e1502, 0x0c1e1501, 0x0c1f1401, 0x0d1f1400, 0x0e1f1300,
	0x0e1f1201, 0x0f1f1200,
	/* counter = 14 */
	0x00111e11, 0x00121e10, 0x00131e0f, 0x00131e0f, 0x01131e0e,
	0x01141d0e, 0x02151d0c, 0x02151d0c, 0x02161d0b, 0x03161c0b,
	0x03171c0a, 0x04171c09, 0x04181b09, 0x05181b08, 0x05191b07,
	0x06191a07, 0x061a1a06, 0x071a1906, 0x071b1905, 0x081b1805,
	0x091b1804, 0x091c1704, 0x0a1c1703, 0x0a1c1604, 0x0b1d1602,
	0x0c1d1502, 0x0c1d1502, 0x0d1d1402, 0x0e1d1401, 0x0e1e1301,
	0x0f1e1300, 0x101e1200,
	/* counter = 15 */
	0x00111e11, 0x00121e10, 0x00131d10, 0x01131d0f, 0x01141d0e,
	0x01141d0e, 0x02151c0d, 0x02151c0d, 0x03161c0b, 0x03161c0b,
	0x04171b0a, 0x04171b0a, 0x05171b09, 0x05181a09, 0x06181a08,
	0x06191a07, 0x07191907, 0x071a1906, 0x081a1806, 0x081a1806,
	0x091a1805, 0x0a1b1704, 0x0a1b1704, 0x0b1c1603, 0x0b1c1603,
	0x0c1c1503, 0x0d1c1502, 0x0d1d1402, 0x0e1d1401, 0x0f1d1301,
	0x0f1d1301, 0x101e1200,
	/* counter = 16 */
};

static const u32 g2d_linear[32] = {
	0x00004000, 0x00023e00, 0x00043c00, 0x00063a00, 0x00083800,
	0x000a3600, 0x000c3400, 0x000e3200, 0x00103000, 0x00122e00,
	0x00142c00, 0x00162a00, 0x00182800, 0x001a2600, 0x001c2400,
	0x001e2200, 0x00202000, 0x00221e00, 0x00241c00, 0x00261a00,
	0x00281800, 0x002a1600, 0x002c1400, 0x002e1200, 0x00301000,
	0x00320e00, 0x00340c00, 0x00360a00, 0x00380800, 0x003a0600,
	0x003c0400, 0x003e0200,
};

static const u32 g2d_rgb_to_yuv_709[12] = {
	0x0bb, 0x0275, 0x03f, 0x4200, 0xFFFFFF99, 0xFFFFFEA6, 0x01c2, 0x20200,
	0x01c2, 0xFFFFFE67, 0xFFFFFFD7, 0x20200,
};

static const u32 g2d_yuv_to_rgb_709[12] = {
	0x04a8, 0x0, 0x072c, 0xFFFC1F7D, 0x04a8, 0xFFFFFF26, 0xFFFFFDDD,
	0x133F8, 0x04a8, 0x0876, 0, 0xFFFB7AA0,
};

static const u32 g2d_rgb_to_yuv_601[12] = {
	0x0107, 0x0204, 0x064, 0x4200,
	0xFFFFFF68, 0xFFFFFED6, 0x01c2, 0x20200,
	0x01c2, 0xFFFFFE87, 0xFFFFFFB7, 0x20200,
};

static const u32 g2d_yuv_to_rgb_601[12] = {
	0x04a8, 0x0, 0x0662, 0xFFFC865A,
	0x04a8, 0xFFFFFE70, 0xFFFFFCBF, 0x21FF4,
	0x04a8, 0x0812, 0x0, 0xFFFBAE4A,
};

static u32 g2d_coefficient_offset(u32 step)
{
	u32 ratio = step >> 16, integer = ratio >> 3, fraction = ratio & 7;
	u32 set;

	if (!integer)
		set = 1;
	else if (integer == 1)
		set = 1 + fraction;
	else if (integer == 2)
		set = 9 + (fraction >> 1);
	else
		set = min(integer + 10, 15U);
	return set * 32;
}

static void g2d_scale(struct rotate_dev *dev, const struct rotate_format *fmt,
		      u32 iw, u32 ih, u32 ow, u32 oh)
{
	bool packed_yuv = fmt->flags & ROTATE_FLAG_YUV && fmt->planes == 1;
	bool yuv420 = fmt->vsub == 2;
	u32 mw = packed_yuv ? iw : min(iw, ow * 8);
	u32 mh = packed_yuv ? ih : min(ih, oh * 4);
	u32 hs = div_u64((u64)mw << 19, ow);
	u32 vs = div_u64((u64)mh << 19, oh);
	u32 offset = g2d_coefficient_offset(hs);
	u32 coffset = g2d_coefficient_offset(hs / fmt->hsub);
	u32 voffset = g2d_coefficient_offset(vs);
	u32 cw = DIV_ROUND_UP(mw, fmt->hsub);
	u32 ch = DIV_ROUND_UP(mh, fmt->vsub);
	u32 ctrl = fmt->planes > 1 ? BIT(16) : 0;
	void __iomem *b = dev->top;
	unsigned int i;

	/* Coarse downsampling precedes the fractional scaler, as in stock. */
	if (!packed_yuv && iw >= ow * 8) {
		writel(mw << 16 | iw, b + G2D_V0_HDS_CTL0);
		/* Shipped 4:2:0 code writes chroma to CTL0 too; qualification pending. */
		writel((mw / fmt->hsub) << 16 | DIV_ROUND_UP(iw, fmt->hsub),
		       b + (yuv420 ? G2D_V0_HDS_CTL0 : G2D_V0_HDS_CTL1));
	}
	if (!packed_yuv && ih >= oh * 4) {
		writel(mh << 16 | ih, b + G2D_V0_VDS_CTL0);
		writel((mh / fmt->vsub) << 16 | DIV_ROUND_UP(ih, fmt->vsub),
		       b + G2D_V0_VDS_CTL1);
	}
	writel(ctrl | 0x101, b + G2D_VS_CTRL);
	writel(ROTATE_SIZE(ow, oh), b + G2D_VS_OUT_SIZE);
	writel(0xff, b + G2D_VS_GLB_ALPHA);
	writel(ROTATE_SIZE(mw, mh), b + G2D_VS_Y_SIZE);
	writel(ROTATE_SIZE(cw, ch), b + G2D_VS_C_SIZE);
	writel(hs << 1, b + G2D_VS_Y_HSTEP);
	writel(vs << 1, b + G2D_VS_Y_VSTEP);
	writel((hs << 1) / fmt->hsub, b + G2D_VS_C_HSTEP);
	writel((vs << 1) / fmt->vsub, b + G2D_VS_C_VSTEP);
	writel(0, b + G2D_VS_Y_HPHASE);
	writel(0, b + G2D_VS_Y_VPHASE0);
	writel(yuv420 ? 0xfffc0000 : 0, b + G2D_VS_C_HPHASE);
	writel(yuv420 ? 0xfffc0000 : 0, b + G2D_VS_C_VPHASE0);
	for (i = 0; i < 32; i++) {
		writel(g2d_lanczos[offset + i], b + G2D_VS_Y_HCOEF0 + i * 4);
		writel(fmt->planes > 1 ? g2d_lanczos[voffset + i] : g2d_linear[i],
		       b + G2D_VS_Y_VCOEF0 + i * 4);
		writel(g2d_lanczos[coffset + i], b + G2D_VS_C_HCOEF0 + i * 4);
	}
	writel(ctrl | 1, b + G2D_VS_CTRL);
}

void sun50i_g2d_mixer(struct rotate_ctx *ctx, dma_addr_t src, dma_addr_t dst)
{
	struct rotate_dev *dev = ctx->dev;
	const struct rotate_format *in = rotate_find_format(ctx->src_fmt.pixelformat);
	const struct rotate_format *out = rotate_find_format(ctx->dst_fmt.pixelformat);
	void __iomem *b = dev->top;
	u32 iw = ctx->src_fmt.width, ih = ctx->src_fmt.height;
	u32 ow = ctx->dst_fmt.width, oh = ctx->dst_fmt.height;
	dma_addr_t addr[3] = {};
	u32 pitch[3] = {};
	const u32 *csc;
	unsigned int i;
	bool iyuv = in->flags & ROTATE_FLAG_YUV;
	bool oyuv = out->flags & ROTATE_FLAG_YUV;

	/* The worker reset this engine before programming; all unused planes off. */
	writel(0xff000001 | in->hw_format << 8, b + G2D_V0_ATTCTL);
	writel(ROTATE_SIZE(iw, ih), b + G2D_V0_MBSIZE);
	writel(ROTATE_SIZE(iw, ih), b + G2D_V0_SIZE);
	rotate_calc_addr_pitch(src, ctx->src_fmt.bytesperline, ih, in, addr, pitch);
	for (i = 0; i < 3; i++) {
		writel(pitch[i], b + G2D_V0_PITCH0 + i * 4);
		writel(lower_32_bits(addr[i]), b + G2D_V0_LADD0 + i * 4);
	}
	writel(0, b + G2D_V0_HADD);
	if (iyuv || iw != ow || ih != oh)
		g2d_scale(dev, in, iw, ih, ow, oh);

	/* Single source, input color space, source-copy raster operation. */
	writel(0xf0, b + G2D_ROP_CTL);
	writel(BIT(8), b + G2D_BLD_EN_CTL);
	writel(ROTATE_SIZE(ow, oh), b + G2D_BLD_CH_ISIZE0);
	writel(ROTATE_SIZE(ow, oh), b + G2D_BLD_SIZE);
	writel(iyuv ? BIT(1) : 0, b + G2D_BLD_OUT_COLOR);
	if (iyuv != oyuv) {
		u32 enc = iyuv ? ctx->src_fmt.ycbcr_enc : ctx->dst_fmt.ycbcr_enc;

		if (enc == V4L2_YCBCR_ENC_DEFAULT)
			enc = V4L2_MAP_YCBCR_ENC_DEFAULT(ctx->src_fmt.colorspace);
		if (iyuv)
			csc = enc == V4L2_YCBCR_ENC_709 ? g2d_yuv_to_rgb_709 : g2d_yuv_to_rgb_601;
		else
			csc = enc == V4L2_YCBCR_ENC_709 ? g2d_rgb_to_yuv_709 : g2d_rgb_to_yuv_601;
		writel(BIT(2), b + G2D_CSC_CTL);
		for (i = 0; i < 12; i++)
			writel(csc[i], b + G2D_CSC2_COEFF + i * 4);
	}
	writel(out->hw_format, b + G2D_WB_ATT);
	writel(ROTATE_SIZE(ow, oh), b + G2D_WB_SIZE);
	memset(addr, 0, sizeof(addr));
	memset(pitch, 0, sizeof(pitch));
	rotate_calc_addr_pitch(dst, ctx->dst_fmt.bytesperline, oh, out, addr, pitch);
	for (i = 0; i < 3; i++) {
		writel(pitch[i], b + G2D_WB_PITCH0 + i * 4);
		writel(lower_32_bits(addr[i]), b + G2D_WB_LADD0 + i * 8);
		writel(0, b + G2D_WB_HADD0 + i * 8);
	}
	writel(BIT(4), b + G2D_MIXER_INT);
	writel(BIT(31), b + G2D_MIXER_CTL);
}
