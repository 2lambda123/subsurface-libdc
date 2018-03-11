/*
 * libdivecomputer
 *
 * Copyright (C) 2017 Jef Driesen
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

#include "socket.h"

#include "common-private.h"
#include "context-private.h"

dc_status_t
dc_socket_syserror (s_errcode_t errcode)
{
	switch (errcode) {
	case S_EINVAL:
		return DC_STATUS_INVALIDARGS;
	case S_ENOMEM:
		return DC_STATUS_NOMEMORY;
	case S_EACCES:
		return DC_STATUS_NOACCESS;
	case S_EAFNOSUPPORT:
		return DC_STATUS_UNSUPPORTED;
	default:
		return DC_STATUS_IO;
	}
}

dc_status_t
dc_socket_init (dc_context_t *context)
{
#ifdef _WIN32
	// Initialize the winsock dll.
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD (2, 2);
	int rc = WSAStartup (wVersionRequested, &wsaData);
	if (rc != 0) {
		SYSERROR (context, rc);
		return DC_STATUS_UNSUPPORTED;
	}

	// Confirm that the winsock dll supports version 2.2.
	// Note that if the dll supports versions greater than 2.2 in addition to
	// 2.2, it will still return 2.2 since that is the version we requested.
	if (LOBYTE (wsaData.wVersion) != 2 ||
		HIBYTE (wsaData.wVersion) != 2) {
		ERROR (context, "Incorrect winsock version.");
		return DC_STATUS_UNSUPPORTED;
	}
#endif

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_exit (dc_context_t *context)
{
#ifdef _WIN32
	// Terminate the winsock dll.
	if (WSACleanup () != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (context, errcode);
		return dc_socket_syserror(errcode);
	}
#endif

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_open (dc_iostream_t *abstract, int family, int type, int protocol)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_socket_t *device = (dc_socket_t *) abstract;

	// Default to blocking reads.
	device->timeout = -1;

	// Initialize the socket library.
	status = dc_socket_init (abstract->context);
	if (status != DC_STATUS_SUCCESS) {
		return status;
	}

	// Open the socket.
	device->fd = socket (family, type, protocol);
	if (device->fd == S_INVALID) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		status = dc_socket_syserror(errcode);
		goto error;
	}

	return DC_STATUS_SUCCESS;

error:
	dc_socket_exit (abstract->context);
	return status;
}

dc_status_t
dc_socket_close (dc_iostream_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_socket_t *socket = (dc_socket_t *) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Terminate all send and receive operations.
	shutdown (socket->fd, 0);

	// Close the socket.
	if (S_CLOSE (socket->fd) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, dc_socket_syserror(errcode));
	}

	// Terminate the socket library.
	rc = dc_socket_exit (abstract->context);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}

dc_status_t
dc_socket_connect (dc_iostream_t *abstract, const struct sockaddr *addr, s_socklen_t addrlen)
{
	dc_socket_t *socket = (dc_socket_t *) abstract;

	if (connect (socket->fd, addr, addrlen) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		return dc_socket_syserror(errcode);
	}

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_socket_t *socket = (dc_socket_t *) abstract;

	socket->timeout = timeout;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_set_latency (dc_iostream_t *iostream, unsigned int value)
{
	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_set_break (dc_iostream_t *iostream, unsigned int value)
{
	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_set_dtr (dc_iostream_t *iostream, unsigned int value)
{
	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_set_rts (dc_iostream_t *iostream, unsigned int value)
{
	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_get_lines (dc_iostream_t *iostream, unsigned int *value)
{
	if (value)
		*value = 0;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_socket_t *socket = (dc_socket_t *) abstract;

#ifdef _WIN32
	unsigned long bytes = 0;
#else
	int bytes = 0;
#endif

	if (S_IOCTL (socket->fd, FIONREAD, &bytes) != 0) {
		s_errcode_t errcode = S_ERRNO;
		SYSERROR (abstract->context, errcode);
		return dc_socket_syserror(errcode);
	}

	if (value)
		*value = bytes;

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_socket_t *socket = (dc_socket_t *) abstract;
	size_t nbytes = 0;

	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (socket->fd, &fds);

		struct timeval tvt;
		if (socket->timeout > 0) {
			tvt.tv_sec  = (socket->timeout / 1000);
			tvt.tv_usec = (socket->timeout % 1000) * 1000;
		} else if (socket->timeout == 0) {
			timerclear (&tvt);
		}

		int rc = select (socket->fd + 1, &fds, NULL, NULL, socket->timeout >= 0 ? &tvt : NULL);
		if (rc < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = dc_socket_syserror(errcode);
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		s_ssize_t n = recv (socket->fd, (char *) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR || errcode == S_EAGAIN)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = dc_socket_syserror(errcode);
			goto out;
		} else if (n == 0) {
			break; // EOF reached.
		}

		nbytes += n;
	}

	if (nbytes != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	if (actual)
		*actual = nbytes;

	return status;
}

dc_status_t
dc_socket_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_socket_t *socket = (dc_socket_t *) abstract;
	size_t nbytes = 0;

	while (nbytes < size) {
		fd_set fds;
		FD_ZERO (&fds);
		FD_SET (socket->fd, &fds);

		int rc = select (socket->fd + 1, NULL, &fds, NULL, NULL);
		if (rc < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = dc_socket_syserror(errcode);
			goto out;
		} else if (rc == 0) {
			break; // Timeout.
		}

		s_ssize_t n = send (socket->fd, (const char *) data + nbytes, size - nbytes, 0);
		if (n < 0) {
			s_errcode_t errcode = S_ERRNO;
			if (errcode == S_EINTR || errcode == S_EAGAIN)
				continue; // Retry.
			SYSERROR (abstract->context, errcode);
			status = dc_socket_syserror(errcode);
			goto out;
		} else if (n == 0) {
			break; // EOF.
		}

		nbytes += n;
	}

	if (nbytes != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	if (actual)
		*actual = nbytes;

	return status;
}

dc_status_t
dc_socket_flush (dc_iostream_t *abstract)
{
	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_socket_sleep (dc_iostream_t *abstract, unsigned int timeout)
{
	return DC_STATUS_SUCCESS;
}
