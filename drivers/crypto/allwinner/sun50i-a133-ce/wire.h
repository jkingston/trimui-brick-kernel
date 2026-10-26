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
/* CE v4 hash uses byte scatter lengths and a distinct header/launch class. */
struct ce_hash_desc {
	__le32 common, command, bits, key, iv;
	struct ce_sg src[8], dst[8];
	__le32 next, reserved[3];
};
static_assert(sizeof(struct ce_hash_desc) == 164);
static_assert(offsetof(struct ce_hash_desc, src) == 20);
static_assert(offsetof(struct ce_hash_desc, dst) == 84);
static_assert(offsetof(struct ce_hash_desc, next) == 148);

/* One immutable allocation per launch; hardware cannot reference caller memory. */
struct ce_storage {
	union {
		struct ce_desc desc;
		struct ce_hash_desc hash;
	};
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
/* Software padding follows the vendor's block-aligned finalization path:
 * full-block SHA-256 compression, explicit big-endian IV, no HW-padding bit.
 * Caller has copied bytes of input; the rest is populated here, never DMA'd
 * from a caller buffer. key[] is unused by hash and holds the 32-byte IV.
 */
static inline int ce_encode_sha256(struct ce_storage *s, u64 dma,
				  unsigned int flow, unsigned int bytes)
{
	static const u8 initial[32] = {
		0x6a, 0x09, 0xe6, 0x67, 0xbb, 0x67, 0xae, 0x85,
		0x3c, 0x6e, 0xf3, 0x72, 0xa5, 0x4f, 0xf5, 0x3a,
		0x51, 0x0e, 0x52, 0x7f, 0x9b, 0x05, 0x68, 0x8c,
		0x1f, 0x83, 0xd9, 0xab, 0x5b, 0xe0, 0xcd, 0x19,
	};
	unsigned int padded, i;
	u64 bits = (u64)bytes * 8;

	if (!s || flow >= 4 || bytes > 65408 || (dma & 15) ||
	    dma > 0xffffffffULL - (sizeof(*s) - 1))
		return -EINVAL;
	padded = (bytes + 9 + 63) & ~63U;
	memset(&s->desc, 0, sizeof(s->desc));
	memcpy(s->key, initial, sizeof(initial));
	memset(s->input + bytes, 0, padded - bytes);
	s->input[bytes] = 0x80;
	for (i = 0; i < 8; i++)
		s->input[padded - 1 - i] = bits >> (i * 8);
	s->hash.common = cpu_to_le32(flow | (1U << 16) | (1U << 8));
	s->hash.command = cpu_to_le32(3); /* SHA-256 */
	s->hash.bits = cpu_to_le32(padded * 8);
	s->hash.iv = cpu_to_le32((dma + offsetof(struct ce_storage, key)) >> 2);
	s->hash.src[0].addr = cpu_to_le32((dma + offsetof(struct ce_storage, input)) >> 2);
	s->hash.src[0].words = cpu_to_le32(padded); /* hash scatter lengths are bytes */
	s->hash.dst[0].addr = cpu_to_le32((dma + offsetof(struct ce_storage, output)) >> 2);
	s->hash.dst[0].words = cpu_to_le32(32);
	return 0;
}

/* A133 CE v4 hash/RNG task: SHA-256-conditioned TRNG selector, byte lengths. */
static inline int ce_encode_trng(struct ce_storage *s, u64 dma,
				 unsigned int flow, unsigned int bytes)
{
	if (!s || flow >= 4 || bytes < 32 || bytes > 8192 || (bytes & 31) ||
	    (dma & 15) || dma > 0xffffffffULL - (sizeof(*s) - 1))
		return -EINVAL;
	memset(&s->desc, 0, sizeof(s->desc));
	s->hash.common = cpu_to_le32(flow | (1U << 16));
	s->hash.command = cpu_to_le32(0x203);
	s->hash.dst[0].addr = cpu_to_le32((dma +
					offsetof(struct ce_storage, output)) >> 2);
	s->hash.dst[0].words = cpu_to_le32(bytes);
	return 0;
}
#endif
