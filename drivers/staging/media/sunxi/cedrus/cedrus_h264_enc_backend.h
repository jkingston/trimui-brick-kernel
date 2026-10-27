/* SPDX-License-Identifier: GPL-2.0 */
/* Unexposed A133 H.264 encoder backend ownership. */

#ifndef _CEDRUS_H264_ENC_BACKEND_H_
#define _CEDRUS_H264_ENC_BACKEND_H_

#include "cedrus_enc.h"
#include "cedrus_h264_enc_job.h"

/* This candidate intentionally fails the complete-backend contract gate. */
extern const struct cedrus_enc_backend_ops
cedrus_a133_h264_backend_candidate_ops;

#if IS_ENABLED(CONFIG_VIDEO_SUNXI_CEDRUS_ENCODER_CORE_KUNIT_TEST)
/* Test inspection is valid only while the synthetic context is quiescent. */
struct cedrus_a133_h264_backend_job *
cedrus_a133_h264_backend_test_job(struct cedrus_ctx *ctx);
#endif

#endif /* _CEDRUS_H264_ENC_BACKEND_H_ */
