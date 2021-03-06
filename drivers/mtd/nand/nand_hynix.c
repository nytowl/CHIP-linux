/*
 * Copyright (C) 2014 Boris BREZILLON <b.brezillon.dev@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/mtd/nand.h>
#include <linux/slab.h>

static u8 h27ucg8t2a_read_retry_regs[] = {
	0xcc, 0xbf, 0xaa, 0xab, 0xcd, 0xad, 0xae, 0xaf
};

static u8 h27q_read_retry_regs[] = {
	0x38, 0x39, 0x3a, 0x3b,
};

struct hynix_read_retry {
	int nregs;
	u8 *regs;
	u8 values[0];
};

struct hynix_nand {
	struct hynix_read_retry read_retry;
};

int nand_setup_read_retry_hynix(struct mtd_info *mtd, int retry_mode)
{
	struct nand_chip *chip = mtd->priv;
	struct hynix_nand *hynix = chip->manuf_priv;
	int offset = retry_mode * hynix->read_retry.nregs;
	int status;
	int i;

	chip->cmdfunc(mtd, 0x36, -1, -1);
	for (i = 0; i < hynix->read_retry.nregs; i++) {
		int column = hynix->read_retry.regs[i];
		column |= column << 8;
		chip->cmdfunc(mtd, NAND_CMD_NONE, column, -1);
		chip->write_byte(mtd, hynix->read_retry.values[offset + i]);
	}
	chip->cmdfunc(mtd, 0x16, -1, -1);

	status = chip->waitfunc(mtd, chip);
	if (status & NAND_STATUS_FAIL)
		return -EIO;

	return 0;
}

static void h27_cleanup(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	kfree(chip->manuf_priv);
}

static int h27ucg8t2a_init(struct mtd_info *mtd, const uint8_t *id)
{
	struct nand_chip *chip = mtd->priv;
	struct hynix_nand *hynix;
	u8 * buf = NULL;
	int i, j;
	int ret;

	buf = kzalloc(1024, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	chip->select_chip(mtd, 0);
	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	chip->cmdfunc(mtd, 0x36, 0xff, -1);
	chip->write_byte(mtd, 0x40);
	chip->cmdfunc(mtd, NAND_CMD_NONE, 0xcc, -1);
	chip->write_byte(mtd, 0x4d);
	chip->cmdfunc(mtd, 0x16, -1, -1);
	chip->cmdfunc(mtd, 0x17, -1, -1);
	chip->cmdfunc(mtd, 0x04, -1, -1);
	chip->cmdfunc(mtd, 0x19, -1, -1);
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0x0, 0x200);

	chip->read_buf(mtd, buf, 2);
	if (buf[0] != 0x8 || buf[1] != 0x8) {
		ret = -EINVAL;
		goto leave;
	}
	chip->read_buf(mtd, buf, 1024);

	ret = 0;
	for (j = 0; j < 8; j++) {
		for (i = 0; i < 64; i++) {
			u8 *tmp = buf + (128 * j);
			if ((tmp[i] | tmp[i + 64]) != 0xff) {
				ret = -EINVAL;
				goto leave;
			}
		}
	}

	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	chip->cmdfunc(mtd, 0x38, -1, -1);
	chip->select_chip(mtd, -1);

	if (!ret) {
		hynix = kzalloc(sizeof(*hynix) + (8 * 8),
				GFP_KERNEL);
		if (!hynix) {
			ret = -ENOMEM;
			goto leave;
		}

		hynix->read_retry.nregs = 8;
		hynix->read_retry.regs = h27ucg8t2a_read_retry_regs;
		memcpy(hynix->read_retry.values, buf, 64);
		chip->manuf_priv = hynix;
		chip->setup_read_retry = nand_setup_read_retry_hynix;
		chip->read_retries = 8;
		chip->manuf_cleanup = h27_cleanup;
	}

leave:
	kfree(buf);

	return ret;
}

static int h27q_get_best_val(const u8 *buf, int size, int min_cnt)
{
	int *val, *cnt, best = -1, max_cnt = 0;
	int i, j;

	val = kzalloc(size * 2 * sizeof(int), GFP_KERNEL);
	cnt = val + size;

	for (i = 0; i < size; i++) {
		if (val[i] < 0)
			continue;

		val[i] = buf[i];
		cnt[i] = 1;

		for (j = i + 1; j < size; j++) {
			if (buf[j] == val[i]) {
				val[j] = -1;
				cnt[i]++;
			}
		}

		if (max_cnt < cnt[i]) {
			max_cnt = cnt[i];
			best = i;
		}
	}

	if (best >= 0)
		best = val[best];

	kfree(val);

	if (best < 0 || max_cnt < min_cnt)
		return -EINVAL;

	return best;
}

#define H27Q_RR_TABLE_SIZE		784
#define H27Q_RR_TABLE_NSETS		8

static void h27q_set_slc_mode(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	u8 cmd = chip->slc_mode ? 0xbf : 0xbe;

	chip->cmdfunc(mtd, cmd, -1, -1);
}

static void h27q_fix_page(struct mtd_info *mtd, int *page)
{
	struct nand_chip *chip = mtd->priv;
	int blkmsk;
	int tmp;

	if (!chip->slc_mode)
		return;

	blkmsk = ((1 << (chip->phys_erase_shift - chip->page_shift)) - 1);
	tmp = *page & (blkmsk >> 1);
	tmp |= (*page << 1) & ~blkmsk;
	*page = tmp;
}

static int h27q_init(struct mtd_info *mtd, const uint8_t *id)
{
	struct nand_chip *chip = mtd->priv;
	struct hynix_nand *hynix = NULL;
	u8 * buf = NULL, tmp_buf[H27Q_RR_TABLE_NSETS];
	u8 total_rr_count, rr_reg_count;
	int i, j, k;
	int ret;

	buf = kzalloc(H27Q_RR_TABLE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	chip->select_chip(mtd, 0);
	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	chip->cmdfunc(mtd, 0x36, 0x38, -1);
	chip->write_byte(mtd, 0x52);
	chip->cmdfunc(mtd, 0x16, -1, -1);
	chip->cmdfunc(mtd, 0x17, -1, -1);
	chip->cmdfunc(mtd, 0x04, -1, -1);
	chip->cmdfunc(mtd, 0x19, -1, -1);
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0x0, 0x21f);

	chip->read_buf(mtd, buf, H27Q_RR_TABLE_SIZE);

	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	chip->cmdfunc(mtd, 0x36, 0x38, -1);
	chip->write_byte(mtd, 0x0);
	chip->cmdfunc(mtd, 0x16, -1, -1);
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0x0, -1);
	chip->select_chip(mtd, -1);

	/* TODO: support multi-die chips */

	ret = h27q_get_best_val(buf, 8, 5);
	if (ret < 0)
		goto err;
	total_rr_count = ret;

	ret = h27q_get_best_val(buf + 8, 8, 5);
	if (ret < 0)
		goto err;
	rr_reg_count = ret;

	if (rr_reg_count != sizeof(h27q_read_retry_regs)) {
		ret = -EINVAL;
		goto err;
	}

	hynix = kzalloc(sizeof(*hynix) +
			(total_rr_count * rr_reg_count),
			GFP_KERNEL);
	if (!hynix) {
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < total_rr_count; i++) {
		for (j = 0; j < rr_reg_count; j++) {
			int offset = 16 + (i * rr_reg_count) + j;

			for (k = 0; k < H27Q_RR_TABLE_NSETS; k++) {
				int set_offset = k * rr_reg_count *
						 total_rr_count * 2;

				tmp_buf[k] = buf[offset + set_offset];
			}

			ret = h27q_get_best_val(tmp_buf, H27Q_RR_TABLE_NSETS,
						5);
			if (ret >= 0) {
				hynix->read_retry.values[(i * rr_reg_count) + j] = ret;
				continue;
			}

			offset += rr_reg_count * total_rr_count;
			for (k = 0; k < H27Q_RR_TABLE_NSETS; k++) {
				int set_offset = k * rr_reg_count *
						 total_rr_count;

				tmp_buf[k] = buf[offset + set_offset];
			}
			ret = h27q_get_best_val(tmp_buf, H27Q_RR_TABLE_NSETS,
						5);
			if (ret < 0)
				goto err;
			hynix->read_retry.values[(i * rr_reg_count) + j] = ~ret;
		}
	}

	kfree(buf);

	hynix->read_retry.nregs = rr_reg_count;
	hynix->read_retry.regs = h27q_read_retry_regs;
	chip->manuf_priv = hynix;
	chip->setup_read_retry = nand_setup_read_retry_hynix;
	chip->read_retries = total_rr_count;
	chip->manuf_cleanup = h27_cleanup;
	chip->set_slc_mode = h27q_set_slc_mode;
	chip->fix_page = h27q_fix_page;

	return 0;

err:
	kfree(buf);
	kfree(hynix);

	return ret;
}

struct hynix_nand_initializer {
	u8 id[6];
	int (*init)(struct mtd_info *mtd, const uint8_t *id);
};

struct hynix_nand_initializer initializers[] = {
	{
		.id = {NAND_MFR_HYNIX, 0xde, 0x94, 0xda, 0x74, 0xc4},
		.init = h27ucg8t2a_init,
	},
	{
		.id = {NAND_MFR_HYNIX, 0xde, 0x14, 0xa7, 0x42, 0x4a},
		.init = h27q_init,
	},
};

int hynix_nand_init(struct mtd_info *mtd, const uint8_t *id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(initializers); i++) {
		struct hynix_nand_initializer *initializer = &initializers[i];
		if (memcmp(id, initializer->id, sizeof(initializer->id)))
			continue;

		return initializer->init(mtd, id);
	}

	return 0;
}
EXPORT_SYMBOL(hynix_nand_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Boris BREZILLON <b.brezillon.dev@gmail.com>");
MODULE_DESCRIPTION("Hynix NAND specific code");
