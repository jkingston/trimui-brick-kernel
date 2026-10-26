/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CEDRUS_ENC_VIDEO_H_
#define _CEDRUS_ENC_VIDEO_H_

struct cedrus_dev;
struct cedrus_ctx;

#if IS_ENABLED(CONFIG_VIDEO_SUNXI_CEDRUS_ENCODER_CORE)
int cedrus_enc_video_register(struct cedrus_dev *dev);
void cedrus_enc_video_unregister(struct cedrus_dev *dev);
void cedrus_enc_job_finish(struct cedrus_ctx *ctx);
#else
static inline int cedrus_enc_video_register(struct cedrus_dev *dev) { return 0; }
static inline void cedrus_enc_video_unregister(struct cedrus_dev *dev) { }
static inline void cedrus_enc_job_finish(struct cedrus_ctx *ctx) { }
#endif

#endif
