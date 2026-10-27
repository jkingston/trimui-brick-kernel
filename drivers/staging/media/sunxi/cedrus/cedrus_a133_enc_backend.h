/* SPDX-License-Identifier: GPL-2.0 */
/* Unexposed combined A133 encoder backend ownership. */

#ifndef _CEDRUS_A133_ENC_BACKEND_H_
#define _CEDRUS_A133_ENC_BACKEND_H_

#include "cedrus_enc.h"
#include "cedrus_h264_enc_job.h"

struct cedrus_a133_jpeg_job_lifecycle;

/* This candidate intentionally fails the complete-backend contract gate. */
extern const struct cedrus_enc_backend_ops
cedrus_a133_enc_backend_candidate_ops;

#if IS_ENABLED(CONFIG_VIDEO_SUNXI_CEDRUS_ENCODER_CORE_KUNIT_TEST)
/* Test inspection is valid only while the synthetic context is quiescent. */
struct cedrus_a133_h264_backend_job *
cedrus_a133_enc_backend_test_job(struct cedrus_ctx *ctx);
struct cedrus_a133_jpeg_job_lifecycle *
cedrus_a133_enc_backend_test_jpeg_job(struct cedrus_ctx *ctx);
void
cedrus_a133_enc_backend_test_set_active(struct cedrus_ctx *ctx, u32 pixelformat);
#endif

#endif /* _CEDRUS_A133_ENC_BACKEND_H_ */
