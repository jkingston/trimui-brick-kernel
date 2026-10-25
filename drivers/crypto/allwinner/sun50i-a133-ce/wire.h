/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A133_CE_WIRE_H
#define A133_CE_WIRE_H
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/build_bug.h>
#include <linux/string.h>

struct ce_sg {
	__le32 addr, words;
};
struct ce_desc {
	__le32 id, common, sym, asym, key, iv, ctr, bytes;
	struct ce_sg src[8], dst[8];
	__le32 next, reserved[3];
};
static_assert(sizeof(struct ce_desc) == 176);
static_assert(offsetof(struct ce_desc, src) == 32);
static_assert(offsetof(struct ce_desc, dst) == 96);
static_assert(offsetof(struct ce_desc, next) == 160);
/* One immutable allocation per launch; hardware cannot reference caller memory. */
struct ce_storage {
	struct ce_desc desc;
	u8 key[32] __aligned(16);
	u8 iv[16] __aligned(16);
	u8 input[65536] __aligned(16);
	u8 output[65536] __aligned(16);
};
static_assert(sizeof(struct ce_storage) == 131296);

/* No hardware access; all task-readable pointers remain in one allocation. */
static inline int ce_encode_aes(struct ce_storage *s, u64 dma,
				unsigned int flow, unsigned int keybytes,
				unsigned int mode, unsigned int decrypt,
				unsigned int bytes)
{
	unsigned int code;

	if (!s || flow >= 4 || mode > 1 || decrypt > 1 ||
	    !bytes || bytes > 65536 || (bytes & 15) || (dma & 15) ||
	    dma > 0xffffffffULL - (sizeof(*s) - 1))
		return -EINVAL;
	switch (keybytes) {
	case 16:
		code = 0;
		break;
	case 24:
		code = 1;
		break;
	case 32:
		code = 2;
		break;
	default:
		return -EINVAL;
	}
	memset(&s->desc, 0, sizeof(s->desc));
	s->desc.id = cpu_to_le32(flow);
	s->desc.common = cpu_to_le32((1U << 31) | (decrypt << 8));
	s->desc.sym = cpu_to_le32(code | (mode << 8));
	s->desc.bytes = cpu_to_le32(bytes);
	if (mode == 1)
		s->desc.iv = cpu_to_le32((dma + offsetof(struct ce_storage, iv)) >> 2);
	s->desc.key = cpu_to_le32((dma + offsetof(struct ce_storage, key)) >> 2);
	s->desc.src[0].addr = cpu_to_le32((dma + offsetof(struct ce_storage, input)) >> 2);
	s->desc.src[0].words = cpu_to_le32(bytes >> 2);
	s->desc.dst[0].addr = cpu_to_le32((dma + offsetof(struct ce_storage, output)) >> 2);
	s->desc.dst[0].words = cpu_to_le32(bytes >> 2);
	return 0;
}
#endif
