/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015-2017 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <colord.h>
#include <colorhug.h>

#include "fu-plugin.h"
#include "fu-plugin-vfuncs.h"

#include "fu-colorhug-device.h"

void
fu_plugin_init (FuPlugin *plugin)
{
	g_autofree gchar *tmp = g_strdup_printf ("%i.%i.%i",
						 CH_MAJOR_VERSION,
						 CH_MINOR_VERSION,
						 CH_MICRO_VERSION);
	fu_plugin_add_compile_version (plugin, "com.hughski.colorhug", tmp);
}

gboolean
fu_plugin_update_detach (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuColorhugDevice *self = FU_COLORHUG_DEVICE (device);
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(GUsbDevice) usb_device2 = NULL;

	/* open device */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;

	/* switch to bootloader mode is not required */
	if (fu_colorhug_device_get_is_bootloader (self)) {
		g_debug ("already in bootloader mode, skipping");
		return TRUE;
	}

	/* reset */
	if (!fu_device_detach (FU_DEVICE (device), error))
		return FALSE;

	/* wait for replug */
	g_clear_object (&locker);
	usb_device2 = g_usb_context_wait_for_replug (fu_plugin_get_usb_context (plugin),
						     usb_device,
						     10000, error);
	if (usb_device2 == NULL) {
		g_prefix_error (error, "device did not come back: ");
		return FALSE;
	}

	/* set the new device until we can use a new FuDevice */
	fu_usb_device_set_dev (FU_USB_DEVICE (self), usb_device2);

	/* success */
	return TRUE;
}

gboolean
fu_plugin_update_attach (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuColorhugDevice *self = FU_COLORHUG_DEVICE (device);
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(GUsbDevice) usb_device2 = NULL;

	/* open device */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;

	/* switch to runtime mode is not required */
	if (!fu_colorhug_device_get_is_bootloader (self)) {
		g_debug ("already in runtime mode, skipping");
		return TRUE;
	}

	/* reset */
	if (!fu_device_attach (device, error))
		return FALSE;

	/* wait for replug */
	g_clear_object (&locker);
	usb_device2 = g_usb_context_wait_for_replug (fu_plugin_get_usb_context (plugin),
						     usb_device,
						     10000, error);
	if (usb_device2 == NULL) {
		g_prefix_error (error, "device did not come back: ");
		return FALSE;
	}

	/* set the new device until we can use a new FuDevice */
	fu_usb_device_set_dev (FU_USB_DEVICE (self), usb_device2);

	/* success */
	return TRUE;
}

gboolean
fu_plugin_update_reload (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuColorhugDevice *self = FU_COLORHUG_DEVICE (device);
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* also set flash success */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	if (!fu_colorhug_device_set_flash_success (self, error))
		return FALSE;
	return TRUE;
}

gboolean
fu_plugin_update (FuPlugin *plugin,
		  FuDevice *device,
		  GBytes *blob_fw,
		  FwupdInstallFlags flags,
		  GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(GError) error_local = NULL;

	/* check this firmware is actually for this device */
	if (!ch_device_check_firmware (usb_device,
				       g_bytes_get_data (blob_fw, NULL),
				       g_bytes_get_size (blob_fw),
				       &error_local)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "firmware is not suitable: %s",
			     error_local->message);
		return FALSE;
	}

	/* write firmware */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_write_firmware (device, blob_fw, error);
}

gboolean
fu_plugin_verify (FuPlugin *plugin,
		  FuDevice *device,
		  FuPluginVerifyFlags flags,
		  GError **error)
{
	FuColorhugDevice *self = FU_COLORHUG_DEVICE (device);
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* write firmware */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	return fu_colorhug_device_verify_firmware (self, error);
}

gboolean
fu_plugin_usb_device_added (FuPlugin *plugin, GUsbDevice *usb_device, GError **error)
{
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuColorhugDevice) device = NULL;

	/* open the device */
	device = fu_colorhug_device_new (usb_device);
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;

	/* insert to hash */
	fu_plugin_device_add (plugin, FU_DEVICE (device));
	return TRUE;
}
