/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr

#include "uwatec_smart.h"
#include "context-private.h"
#include "device-private.h"
#include "irda.h"
#include "array.h"
#include "platform.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &uwatec_smart_device_vtable)

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

typedef struct uwatec_smart_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} uwatec_smart_device_t;

static dc_status_t uwatec_smart_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t uwatec_smart_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t uwatec_smart_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t uwatec_smart_device_close (dc_device_t *abstract);

static const dc_device_vtable_t uwatec_smart_device_vtable = {
	sizeof(uwatec_smart_device_t),
	DC_FAMILY_UWATEC_SMART,
	uwatec_smart_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	uwatec_smart_device_dump, /* dump */
	uwatec_smart_device_foreach, /* foreach */
	NULL, /* timesync */
	uwatec_smart_device_close /* close */
};

static dc_status_t
uwatec_smart_extract_dives (dc_device_t *device, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata);

static int
uwatec_smart_filter (const char *name)
{
	static const char *names[] = {
		"Aladin Smart Com",
		"Aladin Smart Pro",
		"Aladin Smart Tec",
		"Aladin Smart Z",
		"Uwatec Aladin",
		"UWATEC Galileo",
		"UWATEC Galileo Sol",
	};

	if (name == NULL)
		return 0;

	for (size_t i = 0; i < C_ARRAY_SIZE(names); ++i) {
		if (strcasecmp(name, names[i]) == 0) {
			return 1;
		}
	}

	return 0;
}


static dc_status_t
uwatec_smart_transfer (uwatec_smart_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	status = dc_iostream_write (device->iostream, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	status = dc_iostream_read (device->iostream, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_handshake (uwatec_smart_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Command template.
	unsigned char answer[1] = {0};
	unsigned char command[5] = {0x00, 0x10, 0x27, 0, 0};

	// Handshake (stage 1).
	command[0] = 0x1B;
	dc_status_t rc = uwatec_smart_transfer (device, command, 1, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Handshake (stage 2).
	command[0] = 0x1C;
	rc = uwatec_smart_transfer (device, command, 5, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
uwatec_smart_device_open (dc_device_t **out, dc_context_t *context)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	uwatec_smart_device_t *device = NULL;
	dc_iterator_t *iterator = NULL;
	dc_irda_device_t *dev = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (uwatec_smart_device_t *) dc_device_allocate (context, &uwatec_smart_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = NULL;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	// Create the irda device iterator.
	status = dc_irda_iterator_new (&iterator, context, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to create the irda iterator.");
		goto error_free;
	}

	// Enumerate the irda devices.
	while (1) {
		dc_irda_device_t *current = NULL;
		status = dc_iterator_next (iterator, &current);
		if (status != DC_STATUS_SUCCESS) {
			if (status == DC_STATUS_DONE) {
				ERROR (context, "No dive computer found.");
				status = DC_STATUS_NODEVICE;
			} else {
				ERROR (context, "Failed to enumerate the irda devices.");
			}
			goto error_iterator_free;
		}

		if (uwatec_smart_filter (dc_irda_device_get_name (current))) {
			dev = current;
			break;
		}

		dc_irda_device_free (current);
	}

	// Open the irda socket.
	status = dc_irda_open (&device->iostream, context, dc_irda_device_get_address (dev), 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the irda socket.");
		goto error_device_free;
	}

	// Perform the handshaking.
	status = uwatec_smart_handshake (device);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to handshake with the device.");
		goto error_close;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_iostream_close (device->iostream);
error_device_free:
	dc_irda_device_free (dev);
error_iterator_free:
	dc_iterator_free (iterator);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
uwatec_smart_device_close (dc_device_t *abstract)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	// Close the device and pass up the return code.
	return dc_iostream_close (device->iostream);
}


static dc_status_t
uwatec_smart_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Read the model number.
	unsigned char cmd_model[1] = {0x10};
	unsigned char model[1] = {0};
	rc = uwatec_smart_transfer (device, cmd_model, sizeof (cmd_model), model, sizeof (model));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the serial number.
	unsigned char cmd_serial[1] = {0x14};
	unsigned char serial[4] = {0};
	rc = uwatec_smart_transfer (device, cmd_serial, sizeof (cmd_serial), serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Read the device clock.
	unsigned char cmd_devtime[1] = {0x1A};
	unsigned char devtime[4] = {0};
	rc = uwatec_smart_transfer (device, cmd_devtime, sizeof (cmd_devtime), devtime, sizeof (devtime));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (devtime);

	// Update and emit a progress event.
	progress.current += 9;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = model[0];
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (serial);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Command template.
	unsigned char command[9] = {0x00,
			(device->timestamp      ) & 0xFF,
			(device->timestamp >> 8 ) & 0xFF,
			(device->timestamp >> 16) & 0xFF,
			(device->timestamp >> 24) & 0xFF,
			0x10,
			0x27,
			0,
			0};

	// Data Length.
	command[0] = 0xC6;
	unsigned char answer[4] = {0};
	rc = uwatec_smart_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int length = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.maximum = 4 + 9 + (length ? length + 4 : 0);
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

  	if (length == 0)
		return DC_STATUS_SUCCESS;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, length)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Data.
	command[0] = 0xC4;
	rc = uwatec_smart_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int total = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	if (total != length + 4) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int nbytes = 0;
	while (nbytes < length) {
		// Set the minimum packet size.
		unsigned int len = 32;

		// Increase the packet size if more data is immediately available.
		size_t available = 0;
		rc = dc_iostream_get_available (device->iostream, &available);
		if (rc == DC_STATUS_SUCCESS && available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > length)
			len = length - nbytes;

		rc = dc_iostream_read (device->iostream, data + nbytes, len, NULL);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return rc;
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = uwatec_smart_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = uwatec_smart_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


static dc_status_t
uwatec_smart_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + current + 4);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DC_STATUS_DATAFORMAT;

			if (callback && !callback (data + current, len, data + current + 8, 4, userdata))
				return DC_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DC_STATUS_SUCCESS;
}
