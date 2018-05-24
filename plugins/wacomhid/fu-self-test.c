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

#include <glib-object.h>
#include <stdlib.h>
#include <string.h>

#include "fu-common.h"
#include "fu-test.h"
#include "fu-wac-common.h"
#include "fu-wac-firmware.h"

#include "fwupd-error.h"

static void
fu_firmware_wac_func (void)
{
	DfuElement *element;
	DfuImage *image;
	gboolean ret;
	g_autofree gchar *fn = NULL;
	g_autoptr(DfuFirmware) firmware = dfu_firmware_new ();
	g_autoptr(GBytes) blob_block = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;

	/* parse the test file */
	fn = fu_test_get_filename (TESTDATADIR, "test.wac");
	if (fn == NULL) {
		g_test_skip ("no data file found");
		return;
	}
	bytes = fu_common_get_contents_bytes (fn, &error);
	g_assert_no_error (error);
	g_assert_nonnull (bytes);
	ret = fu_wac_firmware_parse_data (firmware, bytes,
					  DFU_FIRMWARE_PARSE_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert_true (ret);

	/* get image data */
	image = dfu_firmware_get_image (firmware, 0);
	g_assert_nonnull (image);
	element = dfu_image_get_element_default (image);
	g_assert_nonnull (element);

	/* get block */
	blob_block = dfu_element_get_contents_chunk (element, 0x8008000,
						     1024, &error);
	g_assert_no_error (error);
	g_assert_nonnull (blob_block);
	fu_wac_buffer_dump ("block",
			    g_bytes_get_data (blob_block, NULL),
			    g_bytes_get_size (blob_block));
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	/* only critical and error are fatal */
	g_log_set_fatal_mask (NULL, G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);

	/* log everything */
	g_setenv ("G_MESSAGES_DEBUG", "all", FALSE);

	/* tests go here */
	g_test_add_func ("/wac/firmware", fu_firmware_wac_func);
	return g_test_run ();
}

