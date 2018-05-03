/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <string.h>

#include "fu-wac-common.h"

//FIXME: this is untested
guint32
fu_wac_calculate_checksum32be (const guint8 *data, gsize len)
{
	guint32 csum = 0x0;
	g_return_val_if_fail (len % 4 == 0, 0xff);
	for (guint i = 0; i < len; i += 4) {
		guint32 tmp;
		memcpy (&tmp, &data[i], sizeof(guint32));
		csum += GUINT32_FROM_LE (tmp);
	}
	return GUINT32_TO_LE (csum);
}

guint32
fu_wac_calculate_checksum32be_bytes (GBytes *blob)
{
	gsize len = 0;
	const guint8 *data = g_bytes_get_data (blob, &len);
	return fu_wac_calculate_checksum32be (data, len);
}

const gchar *
fu_wac_report_id_to_string (guint8 report_id)
{
	if (report_id == FU_WAC_REPORT_ID_FW_DESCRIPTOR)
		return "FwDescriptor";
	if (report_id == FU_WAC_REPORT_ID_SWITCH_TO_FLASH_LOADER)
		return "SwitchToFlashLoader";
	if (report_id == FU_WAC_REPORT_ID_QUIT_AND_RESET)
		return "QuitAndReset";
	if (report_id == FU_WAC_REPORT_ID_READ_BLOCK_DATA)
		return "ReadBlockData";
	if (report_id == FU_WAC_REPORT_ID_WRITE_BLOCK)
		return "WriteBlock";
	if (report_id == FU_WAC_REPORT_ID_ERASE_BLOCK)
		return "EraseBlock";
	if (report_id == FU_WAC_REPORT_ID_SET_READ_ADDRESS)
		return "SetReadAddress";
	if (report_id == FU_WAC_REPORT_ID_GET_STATUS)
		return "GetStatus";
	if (report_id == FU_WAC_REPORT_ID_UPDATE_RESET)
		return "UpdateReset";
	if (report_id == FU_WAC_REPORT_ID_WRITE_WORD)
		return "WriteWord";
	if (report_id == FU_WAC_REPORT_ID_GET_PARAMETERS)
		return "GetParameters";
	if (report_id == FU_WAC_REPORT_ID_GET_FLASH_DESCRIPTOR)
		return "GetFlashDescriptor";
	if (report_id == FU_WAC_REPORT_ID_GET_CHECKSUMS)
		return "GetChecksums";
	if (report_id == FU_WAC_REPORT_ID_SET_CHECKSUM_FOR_BLOCK)
		return "SetChecksumForBlock";
	if (report_id == FU_WAC_REPORT_ID_CALCULATE_CHECKSUM_FOR_BLOCK)
		return "CalculateChecksumForBlock";
	if (report_id == FU_WAC_REPORT_ID_WRITE_CHECKSUM_TABLE)
		return "WriteChecksumTable";
	if (report_id == FU_WAC_REPORT_ID_GET_CURRENT_FIRMWARE_IDX)
		return "GetCurrentFirmwareIdx";
	if (report_id == FU_WAC_REPORT_ID_MODULE)
		return "Module";
	return NULL;
}
