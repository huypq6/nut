/*!
 * @file
 * @brief Generic USB communication backend (using libusb 1.0)
 *
 * @author Copyright (C) 2016 Eaton
 *         Copyright (C) 2016 Arnaud Quette <aquette.dev@gmail.com>
 *
 *      The logic of this file is ripped from mge-shut driver (also from
 *      Arnaud Quette), which is a "HID over serial link" UPS driver for
 *      Network UPS Tools <http://www.networkupstools.org/>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * -------------------------------------------------------------------------- */

#include "config.h" /* for HAVE_LIBUSB_DETACH_KERNEL_DRIVER flag */
#include "common.h" /* for xmalloc, upsdebugx prototypes */
#include "usb-common.h"
#include "nut_libusb.h"
#include "str.h"

#define USB_DRIVER_NAME		"USB communication driver (libusb 1.0)"
#define USB_DRIVER_VERSION	"0.17"

/* driver description structure */
upsdrv_info_t comm_upsdrv_info = {
	USB_DRIVER_NAME,
	USB_DRIVER_VERSION,
	NULL,
	0,
	{ NULL }
};

#define MAX_REPORT_SIZE         0x1800

/*! USB interface number.
 * So far, all of the supported UPS models use interface 0. Let's make this a
 * constant rather than a magic number.
 */
static const int usb_if_num = 0;

static void nut_libusb_close(libusb_device_handle *udev);

/*! Add USB-related driver variables with addvar() and dstate_setinfo().
 * This removes some code duplication across the USB drivers.
 */
void nut_usb_addvars(void)
{
	const struct libusb_version	*v = libusb_get_version();

	/* allow -x vendor=X, vendorid=X, product=X, productid=X, serial=X */
	addvar(VAR_VALUE, "vendor", "Regular expression to match UPS Manufacturer string");
	addvar(VAR_VALUE, "product", "Regular expression to match UPS Product string");
	addvar(VAR_VALUE, "serial", "Regular expression to match UPS Serial number");

	addvar(VAR_VALUE, "vendorid", "Regular expression to match UPS Manufacturer numerical ID (4 digits hexadecimal)");
	addvar(VAR_VALUE, "productid", "Regular expression to match UPS Product numerical ID (4 digits hexadecimal)");

	addvar(VAR_VALUE, "bus", "Regular expression to match USB bus name");
	addvar(VAR_VALUE, "usb_set_altinterface", "Force redundant call to usb_set_altinterface() (value=bAlternateSetting; default=0)");

#ifdef LIBUSB_API_VERSION
	dstate_setinfo("driver.version.usb", "libusb-%u.%u.%u (API: 0x%x)", v->major, v->minor, v->micro, LIBUSB_API_VERSION);
#else  /* LIBUSB_API_VERSION */
	dstate_setinfo("driver.version.usb", "libusb-%u.%u.%u", v->major, v->minor, v->micro);
#endif /* LIBUSB_API_VERSION */
}

/** @brief Claim the @ref usb_if_num interface on a given device handle, trying to detach the kernel driver (if the operation is supported and the driver active).
 * @return @ref LIBUSB_SUCCESS, on success,
 * @return a @ref libusb_error "LIBUSB_ERROR" code, on errors. */
static int	nut_usb_claim_interface(
	libusb_device_handle	*udev	/**< [in] handle of an already opened device */
) {
	int	ret;
#ifdef HAVE_LIBUSB_DETACH_KERNEL_DRIVER
	int	retries;
#endif	/* HAVE_LIBUSB_DETACH_KERNEL_DRIVER */

#if defined(HAVE_LIBUSB_KERNEL_DRIVER_ACTIVE) && defined(HAVE_LIBUSB_SET_AUTO_DETACH_KERNEL_DRIVER)
	/* Due to the way FreeBSD implements libusb_set_auto_detach_kernel_driver(),
	 * check to see if the kernel driver is active before setting the auto-detach flag.
	 * Otherwise, libusb_claim_interface() with the auto-detach flag only works if the driver is running as root. */
	ret = libusb_kernel_driver_active(udev, usb_if_num);
	/* Is the kernel driver active? Consider the unimplemented return code to be equivalent to inactive here. */
	if (ret == 1) {
		upsdebugx(3, "%s: libusb_kernel_driver_active() returned 1 (driver active).", __func__);
		/* Try the auto-detach kernel driver method.
		 * This function is not available on FreeBSD 10.1-10.3. */
		ret = libusb_set_auto_detach_kernel_driver(udev, 1);
		if (ret != LIBUSB_SUCCESS)
			upsdebugx(1, "%s: failed to set kernel driver auto-detach driver flag for USB device (%s).", __func__, libusb_strerror(ret));
		else
			upsdebugx(2, "%s: successfully set kernel driver auto-detach flag.", __func__);
	} else {
		upsdebugx(3, "%s: libusb_kernel_driver_active() returned %d (%s).", __func__, ret, ret ? libusb_strerror(ret) : "no driver active");
	}
#endif	/* HAVE_LIBUSB_KERNEL_DRIVER_ACTIVE + HAVE_LIBUSB_SET_AUTO_DETACH_KERNEL_DRIVER */

#if HAVE_LIBUSB_DETACH_KERNEL_DRIVER
	/* Then, try the explicit detach method.
	 * This function is available on FreeBSD 10.1-10.3. */
	retries = 3;
	while ((ret = libusb_claim_interface(udev, usb_if_num)) != LIBUSB_SUCCESS) {

		upsdebugx(2, "%s: failed to claim USB device (%s).", __func__, libusb_strerror(ret));

		if (retries-- == 0)
			return ret;

		ret = libusb_detach_kernel_driver(udev, usb_if_num);
		if (ret == LIBUSB_SUCCESS)
			upsdebugx(2, "%s: detached kernel driver from USB device...", __func__);
		else if (ret == LIBUSB_ERROR_NOT_FOUND)
			upsdebugx(2, "%s: kernel driver already detached.", __func__);
		else
			upsdebugx(1, "%s: failed to detach kernel driver from USB device (%s).", __func__, libusb_strerror(ret));

	}
#else	/* HAVE_LIBUSB_DETACH_KERNEL_DRIVER */
	if ((ret = libusb_claim_interface(udev, usb_if_num)) != LIBUSB_SUCCESS)
		return ret;
#endif	/* HAVE_LIBUSB_DETACH_KERNEL_DRIVER */

	return LIBUSB_SUCCESS;
}

/* invoke matcher against device */
static inline int matches(USBDeviceMatcher_t *matcher, USBDevice_t *device) {
	if (!matcher) {
		return 1;
	}
	return matcher->match_function(device, matcher->privdata);
}

/** @brief Set the USB alternate interface, if needed.
 *
 * In NUT 2.7.2 and earlier, the following (libusb-0.1) call was made unconditionally:
 *
 *     usb_set_altinterface(udev, 0);
 *
 * Although harmless on Linux and *BSD, this extra call prevents old Tripp Lite devices from working on Mac OS X
 * (presumably the OS is already setting altinterface to 0).
 *
 * @return @ref LIBUSB_SUCCESS, on success,
 * @return a @ref libusb_error "LIBUSB_ERROR" code, on errors. */
static int	nut_usb_set_altinterface(
	libusb_device_handle	*udev	/**< [in] handle of an already opened device */
) {
	int		 ret,
			 altinterface = 0;
	const char	*alt_string;

	if (!testvar("usb_set_altinterface")) {
		upsdebugx(3, "%s: skipped libusb_set_interface_alt_setting(udev, %d, 0).", __func__, usb_if_num);
		return LIBUSB_SUCCESS;
	}

	alt_string = getval("usb_set_altinterface");
	if (alt_string && !str_to_int(alt_string, &altinterface, 10))
		upslogx(LOG_WARNING, "%s: could not convert to an int the provided value (%s) for 'usb_set_altinterface' (%s).", __func__, alt_string, strerror(errno));
	else if (altinterface < 0 || altinterface > 255)
		upslogx(LOG_WARNING, "%s: setting bAlternateInterface to %d will probably not work.", __func__, altinterface);

	/* Set default interface */
	upsdebugx(2, "%s: calling libusb_set_interface_alt_setting(udev, %d, %d).", __func__, usb_if_num, altinterface);
	ret = libusb_set_interface_alt_setting(udev, usb_if_num, altinterface);
	if (ret != LIBUSB_SUCCESS)
		upslogx(LOG_WARNING, "%s: libusb_set_interface_alt_setting(udev, %d, %d) error (%s).", __func__, usb_if_num, altinterface, libusb_strerror(ret));

	upslogx(LOG_NOTICE, "%s: libusb_set_interface_alt_setting() should not be necessary - please email the nut-upsdev list with information about your device.", __func__);

	return ret;
}


/* On success, fill in the curDevice structure and return the report
 * descriptor length. On failure, return -1.
 * Note: When callback is not NULL, the report descriptor will be
 * passed to this function together with the udev and USBDevice_t
 * information. This callback should return a value > 0 if the device
 * is accepted, or < 1 if not. If it isn't accepted, the next device
 * (if any) will be tried, until there are no more devices left.
 */
static int nut_libusb_open(libusb_device_handle **udevp, USBDevice_t *curDevice, USBDeviceMatcher_t *matcher,
	int (*callback)(libusb_device_handle *udev, USBDevice_t *hd, unsigned char *rdbuf, int rdlen))
{
	int rdlen1, rdlen2; /* report descriptor length, method 1+2 */
	USBDeviceMatcher_t *m;
	libusb_device **devlist;
	ssize_t	devcount = 0,
		devnum;
	struct libusb_device_descriptor dev_desc;
	struct libusb_config_descriptor *conf_desc = NULL;
	const struct libusb_interface_descriptor *if_desc;
	libusb_device_handle *udev;
	uint8_t bus;
	int ret, res;
	unsigned char buf[20];
	const unsigned char *p;
	char string[256];
	/* All devices use HID descriptor at index 0. However, some newer
	 * Eaton units have a light HID descriptor at index 0, and the full
	 * version is at index 1 (in which case, bcdDevice == 0x0202) */
	int hid_desc_index = 0;

	/* report descriptor */
	unsigned char	rdbuf[MAX_REPORT_SIZE];
	int		rdlen;

	/* libusb base init */
	if ((ret = libusb_init(NULL)) != LIBUSB_SUCCESS) {
		libusb_exit(NULL);
		fatalx(EXIT_FAILURE, "Failed to init libusb (%s).", libusb_strerror(ret));
	}

#ifndef __linux__ /* SUN_LIBUSB (confirmed to work on Solaris and FreeBSD) */
	/* Causes a double free corruption in linux if device is detached! */
	if (*udevp)
		libusb_close(*udevp);
#endif

	devcount = libusb_get_device_list(NULL, &devlist);

	for (devnum = 0; devnum < devcount; devnum++) {
		/* int		if_claimed = 0; */
		int		i;
		libusb_device	*device = devlist[devnum];

		upsdebugx(2, "Checking device %lu of %lu.", devnum + 1, devcount);

		/* Get DEVICE descriptor */
		ret = libusb_get_device_descriptor(device, &dev_desc);
		if (ret != LIBUSB_SUCCESS) {
			upsdebugx(2, "Unable to get DEVICE descriptor (%s).", libusb_strerror(ret));
			continue;
		}

		/* open the device */
		ret = libusb_open(device, udevp);
		if (ret != 0) {
			upsdebugx(2, "Failed to open device %04X:%04X (%s), skipping.", dev_desc.idVendor, dev_desc.idProduct, libusb_strerror(ret));
			continue;
		}
		udev = *udevp;

		/* collect the identifying information of this
		   device. Note that this is safe, because
		   there's no need to claim an interface for
		   this (and therefore we do not yet need to
		   detach any kernel drivers). */

		free(curDevice->Vendor);
		free(curDevice->Product);
		free(curDevice->Serial);
		free(curDevice->Bus);
		memset(curDevice, '\0', sizeof(*curDevice));

		bus = libusb_get_bus_number(device);
		if ((curDevice->Bus = (char *)malloc(4)) == NULL)
			goto oom_error;
		sprintf(curDevice->Bus, "%03d", bus);
		curDevice->VendorID = dev_desc.idVendor;
		curDevice->ProductID = dev_desc.idProduct;
		curDevice->bcdDevice = dev_desc.bcdDevice;

		if (dev_desc.iManufacturer) {
			ret = libusb_get_string_descriptor_ascii(udev, dev_desc.iManufacturer, (unsigned char*)string, sizeof(string));
			if (ret > 0 && (curDevice->Vendor = strdup(string)) == NULL)
				goto oom_error;
		}

		if (dev_desc.iProduct) {
			ret = libusb_get_string_descriptor_ascii(udev, dev_desc.iProduct, (unsigned char*)string, sizeof(string));
			if (ret > 0 && (curDevice->Product = strdup(string)) == NULL)
				goto oom_error;
		}

		if (dev_desc.iSerialNumber) {
			ret = libusb_get_string_descriptor_ascii(udev, dev_desc.iSerialNumber, (unsigned char*)string, sizeof(string));
			if (ret > 0 && (curDevice->Serial = strdup(string)) == NULL)
				goto oom_error;
		}

		upsdebugx(2, "- VendorID: %04x", curDevice->VendorID);
		upsdebugx(2, "- ProductID: %04x", curDevice->ProductID);
		upsdebugx(2, "- Manufacturer: %s", curDevice->Vendor ? curDevice->Vendor : "unknown");
		upsdebugx(2, "- Product: %s", curDevice->Product ? curDevice->Product : "unknown");
		upsdebugx(2, "- Serial Number: %s", curDevice->Serial ? curDevice->Serial : "unknown");
		upsdebugx(2, "- Bus: %s", curDevice->Bus);
		upsdebugx(2, "- Device release number: %04x", curDevice->bcdDevice);

		/* FIXME: extend to Eaton OEMs (HP, IBM, ...) */
		if ((curDevice->VendorID == 0x463) && (curDevice->bcdDevice == 0x0202)) {
			hid_desc_index = 1;
		}

		upsdebugx(2, "Trying to match device");
		for (m = matcher; m; m=m->next) {
			ret = matches(m, curDevice);
			if (ret==0) {
				upsdebugx(2, "Device does not match - skipping");
				goto next_device;
			} else if (ret==-1) {
				libusb_free_device_list(devlist, 1);
				fatal_with_errno(EXIT_FAILURE, "matcher");
			} else if (ret==-2) {
				upsdebugx(2, "matcher: unspecified error");
				goto next_device;
			}
		}
		upsdebugx(2, "Device matches");


		upsdebugx(2, "Reading first configuration descriptor");
		ret = libusb_get_config_descriptor(device, /* config */ 0, &conf_desc);
		/*ret = libusb_get_active_config_descriptor(device, &conf_desc);*/
		if (ret < 0)
			upsdebugx(2, "result: %i (%s)", ret, libusb_strerror((enum libusb_error)ret));

		/* Now that we have matched the device we wanted, claim it. */
		ret = nut_usb_claim_interface(udev);
		if (ret != LIBUSB_SUCCESS) {
			libusb_free_config_descriptor(conf_desc);
			libusb_free_device_list(devlist, 1);
			fatalx(EXIT_FAILURE, "Can't claim USB device %04x:%04x (%s).", curDevice->VendorID, curDevice->ProductID, libusb_strerror(ret));
		}
	/*	if_claimed = 1;	*/
		upsdebugx(2, "Claimed interface %d successfully", usb_if_num);

		/* Set the USB alternate setting for the interface, if needed. */
		nut_usb_set_altinterface(udev);

		if (!callback) {
			libusb_free_config_descriptor(conf_desc);
			libusb_free_device_list(devlist, 1);
			return 1;
		}

		if (!conf_desc) { /* ?? this should never happen */
			upsdebugx(2, "  Couldn't retrieve descriptors");
			goto next_device;
		}

		rdlen1 = -1;
		rdlen2 = -1;

		/* Get HID descriptor */
		/* FIRST METHOD: ask for HID descriptor directly. */
		res = libusb_control_transfer(udev, LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_STANDARD|LIBUSB_RECIPIENT_INTERFACE,
			LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_HID<<8) + hid_desc_index, usb_if_num, buf, 0x9, USB_TIMEOUT);

		if (res < 0) {
			upsdebugx(2, "Unable to get HID descriptor (%s)", libusb_strerror((enum libusb_error)res));
		} else if (res < 9) {
			upsdebugx(2, "HID descriptor too short (expected %d, got %d)", 8, res);
		} else {

			upsdebug_hex(3, "HID descriptor, method 1", buf, 9);

			rdlen1 = buf[7] | (buf[8] << 8);
		}

		if (rdlen1 < -1) {
			upsdebugx(2, "Warning: HID descriptor, method 1 failed");
		}
		upsdebugx(3, "HID descriptor length (method 1) %d", rdlen1);

		/* SECOND METHOD: find HID descriptor among "extra" bytes of
		   interface descriptor, i.e., bytes tucked onto the end of
		   descriptor 2. */

		/* Note: on some broken UPS's (e.g. Tripp Lite Smart1000LCD),
			only this second method gives the correct result */

		/* for now, we always assume configuration 0, interface 0,
		   altsetting 0, as above. */

		if_desc = &(conf_desc->interface[0].altsetting[0]);
		for (i=0; i<if_desc->extra_length; i+=if_desc->extra[i]) {
			upsdebugx(4, "i=%d, extra[i]=%02x, extra[i+1]=%02x", i,
				if_desc->extra[i], if_desc->extra[i+1]);
			if (i+9 <= if_desc->extra_length && if_desc->extra[i] >= 9 && if_desc->extra[i+1] == 0x21) {
				p = &if_desc->extra[i];
				upsdebug_hex(3, "HID descriptor, method 2", p, 9);
				rdlen2 = p[7] | (p[8] << 8);
				break;
			}
		}

		/* we can now free the config descriptor */
		libusb_free_config_descriptor(conf_desc);

		if (rdlen2 < -1) {
			upsdebugx(2, "Warning: HID descriptor, method 2 failed");
		}
		upsdebugx(3, "HID descriptor length (method 2) %d", rdlen2);

		/* when available, always choose the second value, as it
			seems to be more reliable (it is the one reported e.g. by
			lsusb). Note: if the need arises, can change this to use
			the maximum of the two values instead. */
		if ((curDevice->VendorID == 0x463) && (curDevice->bcdDevice == 0x0202)) {
			upsdebugx(1, "Eaton device v2.02. Using full report descriptor");
			rdlen = rdlen1;
		}
		else {
			rdlen = rdlen2 >= 0 ? rdlen2 : rdlen1;
		}

		if (rdlen < 0) {
			upsdebugx(2, "Unable to retrieve any HID descriptor");
			goto next_device;
		}
		if (rdlen1 >= 0 && rdlen2 >= 0 && rdlen1 != rdlen2) {
			upsdebugx(2, "Warning: two different HID descriptors retrieved (Reportlen = %d vs. %d)", rdlen1, rdlen2);
		}

		upsdebugx(2, "HID descriptor length %d", rdlen);

		if (rdlen > (int)sizeof(rdbuf)) {
			upsdebugx(2, "HID descriptor too long %d (max %d)", rdlen, (int)sizeof(rdbuf));
			goto next_device;
		}

		res = libusb_control_transfer(udev, LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_STANDARD|LIBUSB_RECIPIENT_INTERFACE,
			LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_REPORT<<8) + hid_desc_index, usb_if_num, rdbuf, rdlen, USB_TIMEOUT);

		if (res < 0)
		{
			upsdebugx(2, "Unable to get Report descriptor (%s)", libusb_strerror(res));
			goto next_device;
		}

		if (res < rdlen)
		{
			upsdebugx(2, "Warning: report descriptor too short (expected %d, got %d)", rdlen, res);
			rdlen = res; /* correct rdlen if necessary */
		}

		res = callback(udev, curDevice, rdbuf, rdlen);
		if (res < 1) {
			upsdebugx(2, "Caller doesn't like this device");
			goto next_device;
		}

		upsdebugx(2, "Report descriptor retrieved (Reportlen = %d)", rdlen);
		upsdebugx(2, "Found HID device");
		fflush(stdout);
		libusb_free_device_list(devlist, 1);

		return rdlen;

	next_device:
		/* usb_release_interface() sometimes blocks and goes
		into uninterruptible sleep.  So don't do it. */
		/* if (if_claimed)
			libusb_release_interface(udev, usb_if_num); */
		libusb_close(udev);
		continue;

	oom_error:
		libusb_free_device_list(devlist, 1);
		fatal_with_errno(EXIT_FAILURE, "Out of memory");

	}

	*udevp = NULL;
	libusb_free_device_list(devlist, 1);
	upsdebugx(2, "No appropriate HID device found");
	fflush(stdout);

	return -1;
}

/** @brief Log errors, if any, of nut_libusb_get_report(), nut_libusb_set_report(), nut_libusb_get_string(), nut_libusb_get_interrupt().
 * @return *ret*. */
static int	nut_usb_logerror(
	const int	 ret,	/**< [in] a libusb return code (negative, possibly a @ref libusb_error "LIBUSB_ERROR" code, on errors) */
	const char	*desc	/**< [in] text to print alongside the short description of the error */
) {
	if (ret >= 0)
		return ret;

	switch (ret)
	{
	case LIBUSB_ERROR_INVALID_PARAM:
	case LIBUSB_ERROR_INTERRUPTED:
	case LIBUSB_ERROR_NO_MEM:
	case LIBUSB_ERROR_TIMEOUT:
	case LIBUSB_ERROR_OVERFLOW:
		upsdebugx(2, "%s: %s.", desc, libusb_strerror(ret));
		return ret;
	case LIBUSB_ERROR_BUSY:
	case LIBUSB_ERROR_NO_DEVICE:
	case LIBUSB_ERROR_ACCESS:
	case LIBUSB_ERROR_IO:
	case LIBUSB_ERROR_NOT_FOUND:
	case LIBUSB_ERROR_PIPE:
	case LIBUSB_ERROR_NOT_SUPPORTED:
	case LIBUSB_ERROR_OTHER:
	default:
		upslogx(LOG_DEBUG, "%s: %s.", desc, libusb_strerror(ret));
		return ret;
	}
}

/* return the report of ID=type in report
 * return -1 on failure, report length on success
 */

static int nut_libusb_get_report(libusb_device_handle *udev, int ReportId, unsigned char *raw_buf, int ReportSize )
{
	int	ret;

	upsdebugx(4, "Entering libusb_get_report");

	if (!udev) {
		return 0;
	}

	ret = libusb_control_transfer(udev,
		LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE,
		0x01, /* HID_REPORT_GET */
		ReportId+(0x03<<8), /* HID_REPORT_TYPE_FEATURE */
		usb_if_num, raw_buf, ReportSize, USB_TIMEOUT);

	/* Ignore "protocol stall" (for unsupported request) on control endpoint */
	if (ret == LIBUSB_ERROR_PIPE) {
		return 0;
	}

	return nut_usb_logerror(ret, __func__);
}

static int nut_libusb_set_report(libusb_device_handle *udev, int ReportId, unsigned char *raw_buf, int ReportSize )
{
	int	ret;

	if (!udev) {
		return 0;
	}

	ret = libusb_control_transfer(udev,
		LIBUSB_ENDPOINT_OUT|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE,
		0x09, /* HID_REPORT_SET = 0x09*/
		ReportId+(0x03<<8), /* HID_REPORT_TYPE_FEATURE */
		usb_if_num, raw_buf, ReportSize, USB_TIMEOUT);

	/* Ignore "protocol stall" (for unsupported request) on control endpoint */
	if (ret == LIBUSB_ERROR_PIPE) {
		return 0;
	}

	return nut_usb_logerror(ret, __func__);
}

static int nut_libusb_get_string(libusb_device_handle *udev, int StringIdx, char *buf, size_t buflen)
{
	int ret;

	if (!udev) {
		return -1;
	}
	ret = libusb_get_string_descriptor_ascii(udev, StringIdx,
		(unsigned char*)buf, buflen);

	return nut_usb_logerror(ret, __func__);
}

static int nut_libusb_get_interrupt(libusb_device_handle *udev, unsigned char *buf, int bufsize, int timeout)
{
	int ret;

	if (!udev) {
		return -1;
	}

	/* FIXME: hardcoded interrupt EP => need to get EP descr for IF descr */
	ret = libusb_interrupt_transfer(udev, 0x81, buf, bufsize, &bufsize, timeout);

	if (ret == LIBUSB_SUCCESS)
		return bufsize;

	/* Clear stall condition */
	if (ret == LIBUSB_ERROR_PIPE) {
		ret = libusb_clear_halt(udev, 0x81);
	}

	return nut_usb_logerror(ret, __func__);
}

static void nut_libusb_close(libusb_device_handle *udev)
{
	if (!udev) {
		return;
	}

	/* usb_release_interface() sometimes blocks and goes
	into uninterruptible sleep.  So don't do it. */
	/* libusb_release_interface(udev, usb_if_num); */
	libusb_close(udev);
	libusb_exit(NULL);
}

usb_communication_subdriver_t usb_subdriver = {
	USB_DRIVER_VERSION,
	USB_DRIVER_NAME,
	nut_libusb_open,
	nut_libusb_close,
	nut_libusb_get_report,
	nut_libusb_set_report,
	nut_libusb_get_string,
	nut_libusb_get_interrupt
};