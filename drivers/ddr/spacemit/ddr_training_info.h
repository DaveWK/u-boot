/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DDR training scratch area and defaults for the SpacemiT K1 LPDDR4X init.
 *
 * Inherited from the vendor platform (SpacemiT X1 / K1): the lifted DDR code
 * stashes its training results in SRAM at these addresses while bringing up
 * LPDDR4X, so they must match the layout the vendor FSBL/SPL reserves.
 *
 * These used to live in the board config header, which made them implicit;
 * keep them next to the only code that uses them instead.
 */

#ifndef __SPACEMIT_DDR_TRAINING_INFO_H
#define __SPACEMIT_DDR_TRAINING_INFO_H

#define DDR_TRAINING_INFO_BUFF		0xc0800000
#define DDR_TRAINING_INFO_SAVE_ADDR	0
#define DDR_TRAINING_INFO_MAGIC		0x54524444 /* "DDRT" */
#define DDR_TRAINING_INFO_VER		0x00010000
#define DDR_TRAINING_DATA_BASE		0xc0832000

/* Last-resort chip-select count; the DT "cs-num" property overrides it. */
#define DDR_CS_NUM			1

#ifndef __ASSEMBLY__
#include <linux/types.h>

struct ddr_training_info_t {
	uint32_t magic;
	uint32_t crc32;
	uint64_t chipid;
	uint64_t mac_addr;
	uint32_t version;
	uint32_t cs_num;
	uint8_t reserved[32];
	uint8_t para[1024];
	uint8_t reserved2[448];
};
#endif /* __ASSEMBLY__ */

#endif /* __SPACEMIT_DDR_TRAINING_INFO_H */
