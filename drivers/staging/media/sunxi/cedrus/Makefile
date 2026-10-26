# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_VIDEO_SUNXI_CEDRUS) += sunxi-cedrus.o

sunxi-cedrus-y = cedrus.o cedrus_video.o cedrus_hw.o cedrus_dec.o cedrus_job.o \
		 cedrus_mpeg2.o cedrus_h264.o cedrus_h265.o \
		 cedrus_jpeg.o cedrus_vp8.o

sunxi-cedrus-$(CONFIG_VIDEO_SUNXI_CEDRUS_ENCODER_CORE) += cedrus_enc.o cedrus_enc_video.o
sunxi-cedrus-$(CONFIG_VIDEO_SUNXI_CEDRUS_ENCODER_CORE_KUNIT_TEST) += \
		 cedrus_enc_test.o
sunxi-cedrus-$(CONFIG_VIDEO_SUNXI_CEDRUS_KUNIT_TEST) += cedrus_jpeg_test.o
