/*
 * This file is part of trust|me
 * Copyright(c) 2013 - 2017 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <trustme@aisec.fraunhofer.de>
 */

#include <hardware/sensors.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>

#include <cutils/properties.h>

#include <utils/Atomic.h>
#define LOG_TAG "SensorsClient"
#include <utils/Log.h>

#include "sensors-proxy.h"

static struct sensor_t sensors_list[SENSORS_MAX];
static int sensors_count;

static int open_sensors(const struct hw_module_t *module, const char *id,
			struct hw_device_t ** device);

static int sensors__get_sensors_list(struct sensors_module_t *module, struct sensor_t const **list)
{
	*list = sensors_list;
	ALOGI("%s: module=%p list=%p sensors_count=%d", __func__, module, list, sensors_count);
	return sensors_count;
}

static struct hw_module_methods_t sensors_module_methods = {
	open: open_sensors
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
	common:{
		tag: HARDWARE_MODULE_TAG,
		version_major: 1,
		version_minor: 0,
		id: SENSORS_HARDWARE_MODULE_ID,
		name: "Sensors client",
		author: "Fraunhofer AISEC",
		methods: &sensors_module_methods,
		dso: NULL,
		reserved: {0}
	},
	get_sensors_list:sensors__get_sensors_list,
};

struct sensors_poll_context_t {
	sensors_poll_device_1 device;	// must be first

	 sensors_poll_context_t();
	~sensors_poll_context_t();
	int activate(int handle, int enabled);
	int setDelay(int handle, int64_t ns);
	int pollEvents(sensors_event_t * data, int count);
	int query(int what, int *value);
	int batch(int handle, int flags, int64_t period_ns, int64_t timeout);

private:
	int sock_fd;
	struct sensors_strings_t sensors_strings_list[SENSORS_MAX];
};

/******************************************************************************/

static int recv_all(int sock_fd, void *buf, size_t len)
{
	char *ptr = (char *)buf;
	size_t done = 0;
	int n;

	while (done < len) {
		n = recv(sock_fd, ptr + done, len - done, 0);
		if (n <= 0) {
			ALOGE("fd%d: recv returned %d", sock_fd, n);
			if (n == 0)
				errno = ECONNRESET;	// handle orderly shutdown as error
			return -1;	// the errno value can be tested by the caller
		}
		done += n;
	}
	return 0;
}

/******************************************************************************/

sensors_poll_context_t::sensors_poll_context_t()
{
	struct sockaddr_un server;
	int err;

	// Connect to sensors server first
	sock_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sock_fd < 0) {
		ALOGE("couldn't not open socket: %s", strerror(errno));
		return;
	}

	ALOGV("%s: connecing to %s", __func__, SENSORS_PROXY_PATH);

	// Connect to the sensors proxy server
	memset(&server, 0, sizeof(server));
	server.sun_family = AF_UNIX;
	snprintf(server.sun_path, UNIX_PATH_MAX, "%s", SENSORS_PROXY_PATH);
	err = connect(sock_fd, (struct sockaddr *)&server, sizeof(server));
	if (err) {
		ALOGE("couldn't connect to server: %s", strerror(errno));
		close(sock_fd);
		sock_fd = -1;
		return;
	}
	ALOGI("UNIX socket %d connected to %s", sock_fd, SENSORS_PROXY_PATH);

	// The server sends the list of sensors on accept
	// First we read the sensors count
	err = recv_all(sock_fd, &sensors_count, sizeof(sensors_count));
	if (err) {
		ALOGE("couldn't read sensors count: %s", strerror(errno));
		close(sock_fd);
		sock_fd = -1;
		return;
	}
	ALOGI("%s: sensors count: %d", __func__, sensors_count);

	// Next we read the list of name and vendor strings
	err = recv_all(sock_fd, sensors_strings_list, sizeof(sensors_strings_t) * sensors_count);
	if (err) {
		ALOGE("couldn't read sensors strings list: %s", strerror(errno));
		close(sock_fd);
		sock_fd = -1;
		return;
	}
	// Finally we read the list of sensors
	err = recv_all(sock_fd, sensors_list, sizeof(sensor_t) * sensors_count);
	if (err) {
		ALOGE("couldn't read sensors list: %s", strerror(errno));
		close(sock_fd);
		sock_fd = -1;
		return;
	}
	// Now we need to replace the strings pointers
	for (int i = 0; i < sensors_count; i++) {
		struct sensor_t *list = &sensors_list[i];
		list->name = sensors_strings_list[i].name;
		list->vendor = sensors_strings_list[i].vendor;
		ALOGV("Name %s vendor %s version %d handle %d type %d "
		      "maxRange %f resolution %f power %fmA minDelay %d\n",
		      list->name, list->vendor, list->version, list->handle, list->type,
		      list->maxRange, list->resolution, list->power, list->minDelay);
	}
}

sensors_poll_context_t::~sensors_poll_context_t()
{
	ALOGI("%s", __func__);
	if (sock_fd >= 0)
		close(sock_fd);
}

int sensors_poll_context_t::activate(int handle, int enabled)
{
	ALOGI("%s: handle=%d enabled=%d", __func__, handle, enabled);

	if (sock_fd >= 0) {
		struct sensors_proxy_cmd cmd;

		cmd.cmd = SENSORS_PROXY_CMD_ACTIVATE;
		cmd.handle = handle;
		cmd.activate_enabled = enabled;

		int ret = send(sock_fd, &cmd, sizeof(cmd), 0);
		ALOGE_IF(ret <= 0, "couldn't send activate command: %s",
			 ret ? strerror(errno) : "not enough data");
	}
	return 0;
}

int sensors_poll_context_t::setDelay(int handle, int64_t ns)
{
	ALOGI("%s: handle=%d ns=%lld", __func__, handle, ns);

	if (sock_fd >= 0) {
		struct sensors_proxy_cmd cmd;
		int ret;

		cmd.cmd = SENSORS_PROXY_CMD_SET_DELAY;
		cmd.handle = handle;
		cmd.set_delay_ns = ns;

		ret = send(sock_fd, &cmd, sizeof(cmd), 0);
		ALOGE_IF(ret != sizeof(cmd),
			 "fd%d: couldn't send setDelay command: %s",
			 sock_fd, ret < 0 ? strerror(errno) : "not enough data sent");
	}
	return 0;
}

int sensors_poll_context_t::pollEvents(sensors_event_t * data, int count)
{
	ALOGV("%s: data %p count %d", __func__, data, count);

	if (sock_fd >= 0) {
		int size = sizeof(sensors_event_t) * count;
		char *p = (char *)data;
		int done = 0;

		// receive multiples of the sensors event size
		do {
			int ret = recv(sock_fd, p + done, size - done, 0);
			if (ret <= 0) {
				int fd = sock_fd;
				sock_fd = -1;
				close(fd);
				ALOGE("fd%d: couldn't receive sensors data: %s",
				      fd, ret ? strerror(errno) : "peer orderly shutdown");
				break;
			} else {
				done += ret;
			}
		} while ((done % sizeof(sensors_event_t)) != 0);

		return done / sizeof(sensors_event_t);
	}
	return 0;
}

int sensors_poll_context_t::query(int what, int *value)
{
	ALOGI("%s: what=%d value@%p", __func__, what, value);
	return 0;
}

int sensors_poll_context_t::batch(int handle, int flags, int64_t period_ns, int64_t timeout)
{
	ALOGI("%s: handle=%d flags=%d period_ns=%lld timeout=%lld",
	      __func__, handle, flags, period_ns, timeout);
	return 0;
}

/******************************************************************************/

static int poll__close(struct hw_device_t *dev)
{
	sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
	if (ctx) {
		delete ctx;
	}
	return 0;
}

static int poll__activate(struct sensors_poll_device_t *dev, int handle, int enabled)
{
	sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
	return ctx->activate(handle, enabled);
}

static int poll__setDelay(struct sensors_poll_device_t *dev, int handle, int64_t ns)
{
	sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
	int s = ctx->setDelay(handle, ns);
	return s;
}

static int poll__poll(struct sensors_poll_device_t *dev, sensors_event_t * data, int count)
{
	sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
	return ctx->pollEvents(data, count);
}

static int poll__query(struct sensors_poll_device_1 *dev, int what, int *value)
{
	sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
	return ctx->query(what, value);
}

static int poll__batch(struct sensors_poll_device_1 *dev, int handle, int flags, int64_t period_ns,
		       int64_t timeout)
{
	sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
	return ctx->batch(handle, flags, period_ns, timeout);
}

/******************************************************************************/

/** Open a new instance of a sensor device using name */
static int open_sensors(const struct hw_module_t *module, const char *id,
			struct hw_device_t **device)
{
	int status = -EINVAL;

	ALOGI("%s: module=%p id@%p device@%p", __func__, module, id, device);

	sensors_poll_context_t *dev = new sensors_poll_context_t();

	memset(&dev->device, 0, sizeof(sensors_poll_device_1));

	dev->device.common.tag = HARDWARE_DEVICE_TAG;
	dev->device.common.version = SENSORS_DEVICE_API_VERSION_1_0;
	dev->device.common.module = const_cast < hw_module_t * >(module);
	dev->device.common.close = poll__close;
	dev->device.activate = poll__activate;
	dev->device.setDelay = poll__setDelay;
	dev->device.poll = poll__poll;

	/* Batch processing */
	dev->device.batch = poll__batch;

	*device = &dev->device.common;
	status = 0;

	return status;
}
