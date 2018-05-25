/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "config.h"

#include <string.h>

#include "fu-wac-device.h"
#include "fu-wac-common.h"
#include "fu-wac-firmware.h"
#include "fu-wac-module-bluetooth.h"
#include "fu-wac-module-touch.h"

#include "dfu-chunked.h"
#include "dfu-common.h"
#include "dfu-firmware.h"

typedef struct __attribute__((packed)) {
	guint32		 start_addr;
	guint32		 block_sz;
	guint16		 write_sz; /* bit 15 is write protection flag */
} FuWacFlashDescriptor;

typedef enum {
	FU_WAC_STATUS_UNKNOWN			= 0,
	FU_WAC_STATUS_WRITING			= 1 << 0,
	FU_WAC_STATUS_ERASING			= 1 << 1,
	FU_WAC_STATUS_ERROR_WRITE		= 1 << 2,
	FU_WAC_STATUS_ERROR_ERASE		= 1 << 3,
	FU_WAC_STATUS_WRITE_PROTECTED		= 1 << 4,
	FU_WAC_STATUS_LAST
} FuWacStatus;

#define FU_WAC_DEVICE_TIMEOUT			5000	/* ms */

struct _FuWacDevice
{
	FuUsbDevice		 parent_instance;
	GPtrArray		*flash_descriptors;
	GArray			*checksums;
	guint32			 status_word;
	guint16			 firmware_index;
	guint16			 loader_ver;
	guint16			 read_data_sz;
	guint16			 write_word_sz;
	guint16			 write_block_sz;	/* usb transfer size */
	guint16			 nr_flash_blocks;
	guint16			 configuration;
};

G_DEFINE_TYPE (FuWacDevice, fu_wac_device, FU_TYPE_USB_DEVICE)

static GString *
fu_wac_device_status_to_string (guint32 status_word)
{
	GString *str = g_string_new (NULL);
	if (status_word & FU_WAC_STATUS_WRITING)
		g_string_append (str, "writing,");
	if (status_word & FU_WAC_STATUS_ERASING)
		g_string_append (str, "erasing,");
	if (status_word & FU_WAC_STATUS_ERROR_WRITE)
		g_string_append (str, "error-write,");
	if (status_word & FU_WAC_STATUS_ERROR_ERASE)
		g_string_append (str, "error-erase,");
	if (status_word & FU_WAC_STATUS_WRITE_PROTECTED)
		g_string_append (str, "write-protected,");
	if (str->len == 0) {
		g_string_append (str, "none");
		return str;
	}
	g_string_truncate (str, str->len - 1);
	return str;
}

static gboolean
fu_wav_device_flash_descriptor_is_wp (const FuWacFlashDescriptor *fd)
{
	return fd->write_sz & 0x8000;
}

static void
fu_wac_device_to_string (FuDevice *device, GString *str)
{
	GPtrArray *children;
	FuWacDevice *self = FU_WAC_DEVICE (device);
	g_autoptr(GString) status_str = NULL;

	g_string_append (str, "  DfuWacDevice:\n");
	if (self->firmware_index != 0xffff) {
		g_string_append_printf (str, "    fw-index: 0x%04x\n",
					self->firmware_index);
	}
	if (self->loader_ver > 0) {
		g_string_append_printf (str, "    loader-ver: 0x%04x\n",
					(guint) self->loader_ver);
	}
	if (self->read_data_sz > 0) {
		g_string_append_printf (str, "    read-data-sz: 0x%04x\n",
					(guint) self->read_data_sz);
	}
	if (self->write_word_sz > 0) {
		g_string_append_printf (str, "    write-word-sz: 0x%04x\n",
					(guint) self->write_word_sz);
	}
	if (self->write_block_sz > 0) {
		g_string_append_printf (str, "    write-block-sz: 0x%04x\n",
					(guint) self->write_block_sz);
	}
	if (self->nr_flash_blocks > 0) {
		g_string_append_printf (str, "    nr-flash-blocks: 0x%04x\n",
					(guint) self->nr_flash_blocks);
	}
	if (self->configuration != 0xffff) {
		g_string_append_printf (str, "    configuration: 0x%04x\n",
					(guint) self->configuration);
	}
	for (guint i = 0; i < self->flash_descriptors->len; i++) {
		FuWacFlashDescriptor *fd = g_ptr_array_index (self->flash_descriptors, i);
		g_string_append_printf (str, "    flash-descriptor-%02u:\n", i);
		g_string_append_printf (str, "      start-addr:\t0x%08x\n",
					(guint) fd->start_addr);
		g_string_append_printf (str, "      block-sz:\t0x%08x\n",
					(guint) fd->block_sz);
		g_string_append_printf (str, "      write-sz:\t0x%04x\n",
					(guint) fd->write_sz & ~0x8000);
		g_string_append_printf (str, "      protected:\t%s\n",
					fu_wav_device_flash_descriptor_is_wp (fd) ? "yes" : "no");
	}
	status_str = fu_wac_device_status_to_string (self->status_word);
	g_string_append_printf (str, "    status:\t\t%s\n", status_str->str);

	/* print children also */
	children = fu_device_get_children (device);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child = g_ptr_array_index (children, i);
		g_autofree gchar *tmp = fu_device_to_string (FU_DEVICE (child));
		g_string_append (str, "  DfuWacDeviceChild:\n");
		g_string_append (str, tmp);
	}
}

static gboolean
fu_wac_device_get_feature_report (FuWacDevice *self,
				  guint8 *buf, gsize bufsz,
				  GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (self));
	gsize sz = 0;
	guint8 cmd = buf[0];

	/* hit hardware */
	fu_wac_buffer_dump (fu_wac_report_id_to_string (cmd), buf, bufsz);
	if (!g_usb_device_control_transfer (usb_device,
					    G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
					    G_USB_DEVICE_REQUEST_TYPE_CLASS,
					    G_USB_DEVICE_RECIPIENT_INTERFACE,
					    HID_REPORT_GET,		/* bRequest */
					    HID_FEATURE | cmd,		/* wValue */
					    0x0000,			/* wIndex */
					    buf, bufsz, &sz,
					    FU_WAC_DEVICE_TIMEOUT,
					    NULL, error)) {
		g_prefix_error (error, "Failed to get feature report: ");
		return FALSE;
	}
	fu_wac_buffer_dump (fu_wac_report_id_to_string (cmd), buf, sz);

	/* check packet */
	if (sz != bufsz) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "packet get bytes %" G_GSIZE_FORMAT
			     " expected %" G_GSIZE_FORMAT,
			     sz, bufsz);
		return FALSE;
	}
	if (buf[0] != cmd) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "command response was %i expected %i",
			     buf[0], cmd);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_wac_device_set_feature_report (FuWacDevice *self,
				  guint8 *buf, gsize bufsz,
				  GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (self));
	gsize sz = 0;
	guint8 cmd = buf[0];

	/* hit hardware */
	fu_wac_buffer_dump (fu_wac_report_id_to_string (cmd), buf, bufsz);
	if (g_getenv ("FWUPD_WAC_EMULATE") != NULL)
		return TRUE;
	if (!g_usb_device_control_transfer (usb_device,
					    G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
					    G_USB_DEVICE_REQUEST_TYPE_CLASS,
					    G_USB_DEVICE_RECIPIENT_INTERFACE,
					    HID_REPORT_SET,		/* bRequest */
					    HID_FEATURE | cmd,		/* wValue */
					    0x0000,			/* wIndex */
					    buf, bufsz, &sz,
					    FU_WAC_DEVICE_TIMEOUT,
					    NULL, error)) {
		g_prefix_error (error, "Failed to set feature report: ");
		return FALSE;
	}

	/* check packet */
	if (sz != bufsz) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "packet sent bytes %" G_GSIZE_FORMAT
			     " expected %" G_GSIZE_FORMAT,
			     sz, bufsz);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_wac_device_ensure_flash_descriptors (FuWacDevice *self, GError **error)
{
	gsize sz = (self->nr_flash_blocks * 10) + 1;
	g_autofree guint8 *buf = g_malloc (sz);

	/* already done */
	if (self->flash_descriptors->len > 0)
		return TRUE;

	/* hit hardware */
	memset (buf, 0xff, sz);
	buf[0] = FU_WAC_REPORT_ID_GET_FLASH_DESCRIPTOR;
	if (!fu_wac_device_get_feature_report (self, buf, sz, error))
		return FALSE;

	/* parse */
	for (guint i = 0; i < self->nr_flash_blocks; i++) {
		FuWacFlashDescriptor *fd = g_new0 (FuWacFlashDescriptor, 1);
		const guint blksz = sizeof(FuWacFlashDescriptor);
		fd->start_addr = fu_common_read_uint32 (buf + (i * blksz) + 1, G_LITTLE_ENDIAN);
		fd->block_sz = fu_common_read_uint32 (buf + (i * blksz) + 5, G_LITTLE_ENDIAN);
		fd->write_sz = fu_common_read_uint16 (buf + (i * blksz) + 9, G_LITTLE_ENDIAN);
		g_ptr_array_add (self->flash_descriptors, fd);
	}
	g_debug ("added %u flash descriptors", self->flash_descriptors->len);
	return TRUE;
}

static gboolean
fu_wac_device_ensure_status (FuWacDevice *self, GError **error)
{
	const gsize sz = 5;
	guint8 buf[sz];
	g_autoptr(GString) str = NULL;

	/* hit hardware */
	memset (buf, 0xff, sz);
	buf[0] = FU_WAC_REPORT_ID_GET_STATUS;
	if (!fu_wac_device_get_feature_report (self, buf, sz, error))
		return FALSE;

	/* parse */
	self->status_word = fu_common_read_uint32 (buf + 1, G_LITTLE_ENDIAN);
	str = fu_wac_device_status_to_string (self->status_word);
	g_debug ("status now: %s", str->str);
	return TRUE;
}

static gboolean
fu_wac_device_ensure_checksums (FuWacDevice *self, GError **error)
{
	gsize sz = (self->nr_flash_blocks * 4) + 5;
	guint32 updater_version;
	g_autofree guint8 *buf = g_malloc (sz);

	/* hit hardware */
	memset (buf, 0xff, sz);
	buf[0] = FU_WAC_REPORT_ID_GET_CHECKSUMS;
	if (!fu_wac_device_get_feature_report (self, buf, sz, error))
		return FALSE;

	/* parse */
	updater_version = fu_common_read_uint32 (buf + 1, G_LITTLE_ENDIAN);
	g_debug ("updater-version: %" G_GUINT32_FORMAT, updater_version);

	/* get block checksums */
	g_array_set_size (self->checksums, 0);
	for (guint i = 0; i < self->nr_flash_blocks; i++) {
		guint32 csum = fu_common_read_uint32 (buf + 5 + (i * 4), G_LITTLE_ENDIAN);
		g_debug ("checksum block %02u: 0x%08x", i, (guint) csum);
		g_array_append_val (self->checksums, csum);
	}
	g_debug ("added %u checksums", self->flash_descriptors->len);

	return TRUE;
}


static gboolean
fu_wac_device_ensure_firmware_index (FuWacDevice *self, GError **error)
{
	const gsize sz = 3;
	guint8 buf[sz];

	/* hit hardware */
	memset (buf, 0xff, sz);
	buf[0] = FU_WAC_REPORT_ID_GET_CURRENT_FIRMWARE_IDX;
	if (!fu_wac_device_get_feature_report (self, buf, sz, error))
		return FALSE;

	/* parse */
	self->firmware_index = fu_common_read_uint16 (buf + 1, G_LITTLE_ENDIAN);
	return TRUE;
}

static gboolean
fu_wac_device_ensure_parameters (FuWacDevice *self, GError **error)
{
	const gsize sz = 13;
	guint8 buf[sz];

	/* hit hardware */
	memset (buf, 0xff, sz);
	buf[0] = FU_WAC_REPORT_ID_GET_PARAMETERS;
	if (!fu_wac_device_get_feature_report (self, buf, sz, error))
		return FALSE;

	/* parse */
	self->loader_ver = fu_common_read_uint16 (buf + 1, G_LITTLE_ENDIAN);
	self->read_data_sz = fu_common_read_uint16 (buf + 3, G_LITTLE_ENDIAN);
	self->write_word_sz = fu_common_read_uint16 (buf + 5, G_LITTLE_ENDIAN);
	self->write_block_sz = fu_common_read_uint16 (buf + 7, G_LITTLE_ENDIAN);
	self->nr_flash_blocks = fu_common_read_uint16 (buf + 9, G_LITTLE_ENDIAN);
	self->configuration = fu_common_read_uint16 (buf + 11, G_LITTLE_ENDIAN);
	return TRUE;
}

static gboolean
fu_wac_device_write_block (FuWacDevice *self,
			   guint32 addr,
			   GBytes *blob,
			   GError **error)
{
	const guint8 *tmp;
	gsize bufsz = self->write_block_sz + 5;
	gsize sz = 0;
	g_autofree guint8 *buf = g_malloc (bufsz);

	/* check size */
	tmp = g_bytes_get_data (blob, &sz);
	if (sz > self->write_block_sz) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "packet was too large at %" G_GSIZE_FORMAT " bytes",
			     sz);
		return FALSE;
	}

	/* build packet */
	memset (buf, 0xff, bufsz);
	buf[0] = FU_WAC_REPORT_ID_WRITE_BLOCK;
	fu_common_write_uint32 (buf + 1, addr, G_LITTLE_ENDIAN);
	if (sz > 0)
		memcpy (buf + 5, tmp, sz);

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, bufsz, error);
}

static gboolean
fu_wac_device_erase_block (FuWacDevice *self, guint32 addr, GError **error)
{
	guint8 buf[5];

	/* build packet */
	memset (buf, 0xff, sizeof(buf));
	buf[0] = FU_WAC_REPORT_ID_ERASE_BLOCK;
	fu_common_write_uint32 (buf + 1, addr, G_LITTLE_ENDIAN);

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, sizeof(buf), error);
}

gboolean
fu_wac_device_update_reset (FuWacDevice *self, GError **error)
{
	guint8 buf[5];

	/* build packet */
	memset (buf, 0xff, sizeof(buf));
	buf[0] = FU_WAC_REPORT_ID_UPDATE_RESET;

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, sizeof(buf), error);
}

static gboolean
fu_wac_device_set_checksum_of_block (FuWacDevice *self,
				     guint16 block_nr,
				     guint32 checksum,
				     GError **error)
{
	guint8 buf[7];

	/* build packet */
	memset (buf, 0xff, sizeof(buf));
	buf[0] = FU_WAC_REPORT_ID_SET_CHECKSUM_FOR_BLOCK;
	fu_common_write_uint16 (buf + 1, block_nr, G_LITTLE_ENDIAN);
	fu_common_write_uint32 (buf + 3, checksum, G_LITTLE_ENDIAN);

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, sizeof(buf), error);
}

static gboolean
fu_wac_device_calculate_checksum_of_block (FuWacDevice *self,
					   guint16 block_nr,
					   GError **error)
{
	guint8 buf[3];

	/* build packet */
	memset (buf, 0xff, sizeof(buf));
	buf[0] = FU_WAC_REPORT_ID_CALCULATE_CHECKSUM_FOR_BLOCK;
	fu_common_write_uint16 (buf + 1, block_nr, G_LITTLE_ENDIAN);

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, sizeof(buf), error);
}

static gboolean
fu_wac_device_write_checksum_table (FuWacDevice *self, GError **error)
{
	guint8 buf[5];

	/* build packet */
	memset (buf, 0xff, sizeof(buf));
	buf[0] = FU_WAC_REPORT_ID_WRITE_CHECKSUM_TABLE;

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, sizeof(buf), error);
}

static gboolean
fu_wac_device_switch_to_flash_loader (FuWacDevice *self, GError **error)
{
	guint8 buf[3];

	/* build packet */
	memset (buf, 0xff, sizeof(buf));
	buf[0] = FU_WAC_REPORT_ID_SWITCH_TO_FLASH_LOADER;
	buf[1] = 0x05;
	buf[2] = 0x6a;

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, sizeof(buf), error);
}

static gboolean
fu_wac_device_quit_and_reset (FuWacDevice *self, GError **error)
{
	guint8 buf[FU_WAC_PACKET_LEN];

	/* build packet */
	memset (buf, 0xff, sizeof(buf));
	buf[0] = FU_WAC_REPORT_ID_QUIT_AND_RESET;
	buf[1] = 0x05;
	buf[2] = 0x6a;

	/* hit hardware */
	return fu_wac_device_set_feature_report (self, buf, sizeof(buf), error);
}

static gboolean
fu_wac_device_write_firmware (FuDevice *device, GBytes *blob, GError **error)
{
	DfuElement *element;
	DfuImage *image;
	FuWacDevice *self = FU_WAC_DEVICE (device);
	gsize blocks_done = 0;
	gsize blocks_total = 0;
	guint16 firmware_index_old;
	g_autoptr(DfuFirmware) firmware = dfu_firmware_new ();
	g_autoptr(GHashTable) fd_blobs = NULL;
	g_autofree guint32 *csum_local = NULL;

	//FIXME when to call this?
	if (!fu_wac_device_ensure_status (self, error))
		return FALSE;

	/* load .wac file, including metadata */
	if (!fu_wac_firmware_parse_data (firmware, blob,
					 DFU_FIRMWARE_PARSE_FLAG_NONE,
					 error))
		return FALSE;
	if (dfu_firmware_get_format (firmware) != DFU_FIRMWARE_FORMAT_SREC) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "expected firmware format is 'srec', got '%s'",
			     dfu_firmware_format_to_string (dfu_firmware_get_format (firmware)));
		return FALSE;
	}

	/* enter flash mode */
	if (!fu_wac_device_switch_to_flash_loader (self, error))
		return FALSE;

	/* get current selected device */
	if (!fu_wac_device_ensure_firmware_index (self, error))
		return FALSE;
	firmware_index_old = self->firmware_index;

	/* use the correct image from the firmware */
	image = dfu_firmware_get_image (firmware, self->firmware_index == 1 ? 1 : 0);
	if (image == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "no firmware image for index %" G_GUINT16_FORMAT,
			     self->firmware_index);
		return FALSE;
	}
	element = dfu_image_get_element_default (image);
	if (element == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "no element in image %" G_GUINT16_FORMAT,
			     self->firmware_index);
		return FALSE;
	}
	g_debug ("using element at addr 0x%0x",
		 (guint) dfu_element_get_address (element));

	/* get firmware parameters (page sz and transfer sz) */
	if (!fu_wac_device_ensure_parameters (self, error))
		return FALSE;

	/* get the current flash descriptors */
	if (!fu_wac_device_ensure_flash_descriptors (self, error))
		return FALSE;

	/* get the updater protocol version */
	if (!fu_wac_device_ensure_checksums (self, error))
		return FALSE;

	/* clear all checksums of pages */
	for (guint16 i = 0; i < self->flash_descriptors->len; i++) {
		FuWacFlashDescriptor *fd = g_ptr_array_index (self->flash_descriptors, i);
		if (fu_wav_device_flash_descriptor_is_wp (fd))
			continue;
		if (!fu_wac_device_set_checksum_of_block (self, i, 0x0, error))
			return FALSE;
	}

	/* store host CRC into flash */
	if (!fu_wac_device_write_checksum_table (self, error))
		return FALSE;

	/* get the blobs for each chunk */
	fd_blobs = g_hash_table_new_full (g_direct_hash, g_direct_equal,
					  NULL, (GDestroyNotify) g_bytes_unref);
	for (guint16 i = 0; i < self->flash_descriptors->len; i++) {
		FuWacFlashDescriptor *fd = g_ptr_array_index (self->flash_descriptors, i);
		GBytes *blob_block;
		g_autoptr(GBytes) blob_tmp = NULL;

		if (fu_wav_device_flash_descriptor_is_wp (fd))
			continue;
		blob_tmp = dfu_element_get_contents_chunk (element,
							   fd->start_addr,
							   fd->block_sz,
							   NULL);
		if (blob_tmp == NULL)
			break;
		blob_block = dfu_utils_bytes_pad (blob_tmp, fd->block_sz);
		g_hash_table_insert (fd_blobs, fd, blob_block);
	}

	/* checksum actions post-write */
	blocks_total = g_hash_table_size (fd_blobs) + 2;

	/* write the data into the flash page */
	csum_local = g_new0 (guint32, self->flash_descriptors->len);
	for (guint16 i = 0; i < self->flash_descriptors->len; i++) {
		FuWacFlashDescriptor *fd = g_ptr_array_index (self->flash_descriptors, i);
		GBytes *blob_block;
		g_autoptr(GPtrArray) chunks = NULL;

		/* if page is protected */
		if (fu_wav_device_flash_descriptor_is_wp (fd))
			continue;

		/* get data for page */
		blob_block = g_hash_table_lookup (fd_blobs, fd);
		if (blob_block == NULL)
			break;

		/* erase entire block */
		if (!fu_wac_device_erase_block (self, i, error))
			return FALSE;

		/* write block in chunks */
		chunks = dfu_chunked_new_from_bytes (blob_block,
						     fd->start_addr,
						     0, /* page_sz */
						     self->write_block_sz);
		for (guint j = 0; j < chunks->len; j++) {
			DfuChunkedPacket *pkt = g_ptr_array_index (chunks, j);
			g_autoptr(GBytes) blob_chunk = g_bytes_new (pkt->data, pkt->data_sz);
			if (!fu_wac_device_write_block (self, pkt->address, blob_chunk, error))
				return FALSE;
		}

		/* calculate expected checksum and save to device RAM */
		csum_local[i] = fu_wac_calculate_checksum32be_bytes (blob_block);
		g_debug ("block checksum %02u: 0x%08x", i, csum_local[i]);
		if (!fu_wac_device_set_checksum_of_block (self, i, csum_local[i], error))
			return FALSE;

		/* update device progress */
		fu_device_set_progress_full (FU_DEVICE (self),
					     blocks_done++,
					     blocks_total);
	}

	/* calculate CRC inside device */
	for (guint16 i = 0; i < self->flash_descriptors->len; i++) {
		if (!fu_wac_device_calculate_checksum_of_block (self, i, error))
			return FALSE;
	}

	/* update device progress */
	fu_device_set_progress_full (FU_DEVICE (self), blocks_done++, blocks_total);

	/* read all CRC of all pages and verify with local CRC */
	if (!fu_wac_device_ensure_checksums (self, error))
		return FALSE;
	for (guint16 i = 0; i < self->flash_descriptors->len; i++) {
		FuWacFlashDescriptor *fd = g_ptr_array_index (self->flash_descriptors, i);
		guint32 csum_rom;

		/* if page is protected */
		if (fu_wav_device_flash_descriptor_is_wp (fd))
			continue;

		/* no more written pages */
		if (g_hash_table_lookup (fd_blobs, fd) == NULL)
			break;

		/* check checksum matches */
		csum_rom = g_array_index (self->checksums, guint32, i);
		if (csum_rom != csum_local[i]) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "failed local checksum at block %u, "
				     "got 0x%08x expected 0x%08x", i,
				     (guint) csum_rom, (guint) csum_local[i]);
			return FALSE;
		}
		g_debug ("matched checksum at block %u of 0x%08x", i, csum_rom);
	}

	/* update device progress */
	fu_device_set_progress_full (FU_DEVICE (self), blocks_done++, blocks_total);

	/* store host CRC into flash */
	if (!fu_wac_device_write_checksum_table (self, error))
		return FALSE;

	/* update progress */
	fu_device_set_progress_full (FU_DEVICE (self), blocks_total, blocks_total);

	/* reboot, which switches the boot index of the firmware */
	if (!fu_wac_device_update_reset (self, error))
		return FALSE;

	/* FIXME */
	if (0 && !fu_wac_device_quit_and_reset (self, error))
		return FALSE;

	/* verify the current device is different from when selected */
	if (!fu_wac_device_ensure_firmware_index (self, error))
		return FALSE;
	if (firmware_index_old == self->firmware_index) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "expected firmware index to change");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_wac_device_probe (FuUsbDevice *device, GError **error)
{
	const gchar *plugin_hints;

	/* devices have to be whitelisted */
	plugin_hints = fu_device_get_plugin_hints (FU_DEVICE (device));
	if (plugin_hints == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "not supported with this device");
		return FALSE;
	}

	/* hardware cannot respond to GetReport(DeviceFirmwareDescriptor) */
	if (TRUE || g_strcmp0 (plugin_hints, "use-runtime-version") == 0) {
		fu_device_add_flag (FU_DEVICE (device),
				    FWUPD_DEVICE_FLAG_USE_RUNTIME_VERSION);
	}

	/* hardcoded */
	fu_device_add_icon (FU_DEVICE (device), "input-tablet");
	fu_device_add_flag (FU_DEVICE (device), FWUPD_DEVICE_FLAG_UPDATABLE);
	return TRUE;
}

static gboolean
fu_wac_device_add_modules (FuWacDevice *self, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (self));
	g_autofree gchar *version_bootloader = NULL;
	const gsize sz = 32;
	guint8 buf[sz];

	memset (buf, 0xff, sz);
	buf[0] = FU_WAC_REPORT_ID_FW_DESCRIPTOR;
	if (!fu_wac_device_get_feature_report (self, buf, sz, error)) {
		g_prefix_error (error, "Failed to get DeviceFirmwareDescriptor: ");
		return FALSE;
	}

	/* verify bootloader is compatible */
	if (buf[1] != 0x01) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "bootloader major version not compatible");
		return FALSE;
	}

	/* verify the number of submodules is possible */
	if (buf[3] > (512 - 4) / 4) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "number of submodules is impossible");
		return FALSE;
	}

	/* bootloader version */
	version_bootloader = g_strdup_printf ("%u.%u", buf[1], buf[2]);
	fu_device_set_version_bootloader (FU_DEVICE (self), version_bootloader);

	/* get versions of each submodule */
	for (guint8 i = 0; i < buf[3]; i++) {
		guint8 fw_type = buf[(i * 4) + 4] & ~0x80;
		g_autofree gchar *version = NULL;
		g_autoptr(FuWacModule) module = NULL;

		/* version number is decimal */
		version = g_strdup_printf ("%u.%u", buf[(i * 4) + 5], buf[(i * 4) + 6]);

		switch (fw_type) {
		case FU_WAC_MODULE_FW_TYPE_TOUCH:
			module = fu_wac_module_touch_new (usb_device);
			fu_device_add_child (FU_DEVICE (self), FU_DEVICE (module));
			fu_device_set_version (FU_DEVICE (module), version);
			break;
		case FU_WAC_MODULE_FW_TYPE_BLUETOOTH:
			module = fu_wac_module_bluetooth_new (usb_device);
			fu_device_add_child (FU_DEVICE (self), FU_DEVICE (module));
			fu_device_set_version (FU_DEVICE (module), version);
			break;
		case FU_WAC_MODULE_FW_TYPE_MAIN:
			fu_device_set_version (FU_DEVICE (self), version);
			break;
		default:
			g_warning ("unknown submodule type 0x%0x", fw_type);
			break;
		}
	}
	return TRUE;
}

static gboolean
fu_wac_device_open (FuUsbDevice *device, GError **error)
{
	FuWacDevice *self = FU_WAC_DEVICE (device);
	GUsbDevice *usb_device = fu_usb_device_get_dev (device);
	g_autoptr(GString) str = g_string_new (NULL);

	/* open device */
	if (!g_usb_device_claim_interface (usb_device, 0x00, /* HID */
					   G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					   error)) {
		g_prefix_error (error, "failed to claim HID interface: ");
		return FALSE;
	}

	/* get current status */
	if (!fu_wac_device_ensure_status (self, error))
		return FALSE;

	/* get version of each sub-module */
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_USE_RUNTIME_VERSION)) {
		if (!fu_wac_device_add_modules (self, error))
			return FALSE;
	}

	/* success */
	fu_wac_device_to_string (FU_DEVICE (self), str);
	g_debug ("opened: %s", str->str);
	return TRUE;
}

static gboolean
fu_wac_device_close (FuUsbDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (device);

	/* FIXME: do not re-attach the wacom.ko kernel driver with
	 * G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER as there is a race
	 * in the hidcore subsystem that causes a deadlock... */
	if (!g_usb_device_release_interface (usb_device, 0x00, /* HID */
					     0, /* GUsbDeviceClaimInterfaceFlags */
					     error)) {
		g_prefix_error (error, "failed to release interface: ");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static void
fu_wac_device_init (FuWacDevice *self)
{
	self->flash_descriptors = g_ptr_array_new_with_free_func (g_free);
	self->checksums = g_array_new (FALSE, FALSE, sizeof(guint32));
	self->configuration = 0xffff;
	self->firmware_index = 0xffff;
}

static void
fu_wac_device_finalize (GObject *object)
{
	FuWacDevice *self = FU_WAC_DEVICE (object);

	g_ptr_array_unref (self->flash_descriptors);
	g_array_unref (self->checksums);

	G_OBJECT_CLASS (fu_wac_device_parent_class)->finalize (object);
}

static void
fu_wac_device_class_init (FuWacDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	FuUsbDeviceClass *klass_usb_device = FU_USB_DEVICE_CLASS (klass);
	object_class->finalize = fu_wac_device_finalize;
	klass_device->write_firmware = fu_wac_device_write_firmware;
	klass_device->to_string = fu_wac_device_to_string;
	klass_usb_device->open = fu_wac_device_open;
	klass_usb_device->close = fu_wac_device_close;
	klass_usb_device->probe = fu_wac_device_probe;
}

FuWacDevice *
fu_wac_device_new (GUsbDevice *usb_device)
{
	FuWacDevice *device = NULL;
	device = g_object_new (FU_TYPE_WAC_DEVICE,
			       "usb-device", usb_device,
			       NULL);
	return device;
}
