/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CEDRUS_JOB_H_
#define _CEDRUS_JOB_H_

#include <media/videobuf2-core.h>

struct cedrus_ctx;

bool cedrus_job_begin(struct cedrus_ctx *ctx);
bool cedrus_job_active(struct cedrus_ctx *ctx);
void cedrus_job_mark_running(struct cedrus_ctx *ctx);
void cedrus_job_finish(struct cedrus_ctx *ctx, enum vb2_buffer_state state,
		       bool fault_owner);
void cedrus_job_abort(void *priv);
void cedrus_encoder_device_run(struct cedrus_ctx *ctx);

#endif
