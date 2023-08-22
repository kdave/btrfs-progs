#include "kerncompat.h"
#include <stddef.h>
#include "kernel-shared/ctree.h"
#include "crypto/crc32c.h"
#include "image/common.h"

void csum_block(u8 *buf, size_t len)
{
	u16 csum_size = btrfs_csum_type_size(BTRFS_CSUM_TYPE_CRC32);
	u8 result[csum_size];
	u32 crc = ~(u32)0;
	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len - BTRFS_CSUM_SIZE);
	put_unaligned_le32(~crc, result);
	memcpy(buf, result, csum_size);
}
