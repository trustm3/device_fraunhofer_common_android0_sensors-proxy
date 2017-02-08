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

#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>

#define LOG_TAG "SensorsServer"
#include <cutils/log.h>

#include "sensors-proxy.h"

#define SMODULE_CLIENT_MAX 8
#define EPOLL_DEFAULT_SIZE 32
#define EPOLL_EVENTS_MAX   (SMODULE_CLIENT_MAX + 1)

struct smodule;

// Sensors module
struct smodule_client {
	struct smodule *smod;
	int sock_fd;
	int sensors_enabled;	// number of sensors enabled by this client
	char *sensor_enabled;	// array with 'handle_last+1' fields
	int64_t *sensor_delay_ns;	// array with 'handle_last+1' fields
};

// Sensors module client
struct smodule {
	struct sensors_poll_device_t *device;
	struct sensors_module_t *module;
	const struct sensor_t *sensor_list;
	int sensor_count;
	int handle_last;	// highest handle number used
	int *sensors_enabled;	// array with 'handle_last+1' fields
	int64_t *sensor_delay_ns;	// array with 'handle_last+1' fields
	int epoll_fd;
	int sock_fd;
	pthread_t poll_thread;
	int stop_thread;	// used to stop the sensor polling thread
	pthread_mutex_t mutex;	// protects the list of clients
	struct smodule_client *clients[SMODULE_CLIENT_MAX];
	int client_count;
};

static int smodule_remove_client(struct smodule *smod, struct smodule_client *client);

//
// Some helper functions
//

static int recv_all(int sock_fd, void *buf, size_t len)
{
	char *ptr = (char *)buf;
	size_t done = 0;
	int n;

	while (done < len) {
		n = recv(sock_fd, ptr + done, len - done, 0);
		if (n <= 0) {
			ALOGE("fd%d: recv failed: %s",
			      sock_fd, n < 0 ? strerror(errno) : "peer orderly shutdown");
			if (n == 0)
				errno = ECONNRESET;	// handle orderly shutdown as error
			return -1;	// the errno value can be tested by the caller
		}
		done += n;
	}
	return 0;
}

static int epoll_add_fd(const int epoll_fd, const int fd, void *data)
{
	struct epoll_event event;
	int err;

	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN;
	event.data.ptr = data;

	err = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
	ALOGE_IF(err, "couldn't add fd %d to epoll fd %d: %s", fd, epoll_fd, strerror(errno));

	return err;
}

static int epoll_del_fd(const int epoll_fd, const int fd)
{
	struct epoll_event event;
	int err;

	memset(&event, 0, sizeof(event));
	err = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &event);
	ALOGE_IF(err, "couldn't delete fd %d from epoll fd %d: %s", fd, epoll_fd, strerror(errno));

	return err;
}

// Sensors module client functions
//

static int smodule_client_send_list(const struct smodule_client *client)
{
	const struct smodule *smod = client->smod;
	int ret;

	// First we send the number of sensors
	ret = send(client->sock_fd, &smod->sensor_count, sizeof(smod->sensor_count), 0);
	if (ret < 0) {
		ALOGE("fd%d: couldn't send sensor count: %s", client->sock_fd, strerror(errno));
		return -1;
	}
	// Next we extract and send the name and vendor strings
	struct sensors_strings_t slist[smod->sensor_count];
	for (int i = 0; i < smod->sensor_count; i++) {
		strncpy(slist[i].name, smod->sensor_list[i].name, sizeof(slist[i].name));
		strncpy(slist[i].vendor, smod->sensor_list[i].vendor, sizeof(slist[i].vendor));
	}
	ret = send(client->sock_fd, slist, sizeof(slist), 0);
	if (ret < 0) {
		ALOGE("fd%d: couldn't send sensor strings list: %s", client->sock_fd,
		      strerror(errno));
		return -1;
	}
	// Finally we send the sensor list
	ALOGV("sending %d bytes for sensor list", sizeof(sensor_t) * smod->sensor_count);
	ret = send(client->sock_fd, smod->sensor_list, sizeof(sensor_t) * smod->sensor_count, 0);
	if (ret < 0) {
		ALOGE("fd%d: couldn't send sensor list: %s", client->sock_fd, strerror(errno));
		return -1;
	}

	return 0;
}

static void smodule_client_update_delay(struct smodule_client *client, int handle)
{
	struct smodule *smod = client->smod;
	// We maintain two arrays to track the sensor delay settings:
	// smod->sensor_delay_ns[handle] : holds the current delay
	//   value set (hardware wise) for sensor <handle>
	// client->sensor_enabled[handle]: holds the delay value of
	//   sensor <handle> set by the client
	int64_t delay_min = LLONG_MAX;
	int i, err;

	ALOG_ASSERT(handle >= 0 && handle <= smod->handle_last,
		    "sensor %s is invalid", handle);

	pthread_mutex_lock(&smod->mutex);

	// Find the lowest delay value specified by the client(s)
	for (i = 0; i < smod->client_count; i++) {
		struct smodule_client *c = smod->clients[i];
		int64_t delay = c->sensor_delay_ns[handle];
		if (c->sensor_enabled[handle] && delay > 0 && delay < delay_min)
			delay_min = delay;
	}

	pthread_mutex_unlock(&smod->mutex);

	if (delay_min == LLONG_MAX)
		return;

	// Fixme: delay==0 must be handled in a special way
	if (smod->sensor_delay_ns[handle] == 0 || delay_min != smod->sensor_delay_ns[handle]) {
		ALOGI("fd%d: setting delay of sensor %d to %lld ns", client->sock_fd, handle,
		      delay_min);
		smod->sensor_delay_ns[handle] = delay_min;
		err = smod->device->setDelay(smod->device, handle, delay_min);
		ALOGE_IF(err, "fd%d: setDelay() for handle %d failed: %s",
			 client->sock_fd, handle, strerror(-err));
	}
}

static void smodule_client_update_activate(struct smodule_client *client, int handle,
					   int activate_enabled)
{
	struct smodule *smod = client->smod;
	// We maintain various arrays to track the sensor usage:
	// smod->sensors_enabled[handle] : holds the number of
	//   clients having sensor <handle> enabled.
	// client->sensor_enabled[handle]: tells if the sensor
	//   <handle> is enabled or disabled.
	char *enabled = &client->sensor_enabled[handle];
	int *enabled_count = &smod->sensors_enabled[handle];
	int enabled_count_old;
	int do_activate = 0;
	int err;

	ALOG_ASSERT(handle >= 0 && handle <= smod->handle_last,
		    "sensor %s is invalid", handle);

	pthread_mutex_lock(&smod->mutex);

	enabled_count_old = *enabled_count;
	if (activate_enabled) {
		if (*enabled) {
			pthread_mutex_unlock(&smod->mutex);
			return;	// nop
		}
		client->sensors_enabled++;
		*enabled = 1;
		if (!*enabled_count)
			do_activate = 1;	// enable sensor
		(*enabled_count)++;
	} else {
		if (!*enabled) {
			pthread_mutex_unlock(&smod->mutex);
			return;	// nop
		}
		client->sensors_enabled--;
		*enabled = 0;
		(*enabled_count)--;
		if (!*enabled_count)
			do_activate = 1;	// disable sensor
	}

	pthread_mutex_unlock(&smod->mutex);

	ALOGI("fd%d: %sable sensor %d, do_activate %d",
	      client->sock_fd, activate_enabled ? "en" : "dis", handle, do_activate);

	if (do_activate) {
		err = smod->device->activate(smod->device, handle, activate_enabled);
		ALOGE_IF(err, "fd%d: activate() for handle %d failed: %s",
			 client->sock_fd, handle, strerror(-err));

		// Re-set the delay when the sensor is activated.
		if (activate_enabled && smod->sensor_delay_ns[handle]) {
			err = smod->device->setDelay(smod->device, handle,
						     smod->sensor_delay_ns[handle]);
			ALOGE_IF(err, "fd%d: setDelay() for handle %d failed: %s",
				 client->sock_fd, handle, strerror(-err));
		}
	}
#if 1
	{
		int i;
		for (i = 0; i <= smod->handle_last; i++) {
			if (smod->sensors_enabled[i]) {
				ALOGI("Sensor %d is enabled by %d client(s) with delay %lld ns",
				      i, smod->sensors_enabled[i], smod->sensor_delay_ns[i]);
			}
		}
	}
#endif
}

static struct smodule_client *smodule_client_new(struct smodule *smod)
{
	struct smodule_client *client;
	struct sockaddr_in remote;
	socklen_t remote_addrlen = sizeof(remote);
	int fd, err;

	fd = accept(smod->sock_fd, (struct sockaddr *)&remote, &remote_addrlen);
	if (fd < 0) {
		ALOGE("fd%d: couldn't accept client connection: %s",
		      smod->sock_fd, strerror(errno));
		goto err;
	}

	client = (struct smodule_client *)calloc(1, sizeof(*client));
	if (!client) {
		ALOGE("couldn't allocate memory for client connection");
		goto err_accept;
	}

	client->sensor_enabled = (char *)calloc(smod->handle_last + 1, sizeof(char));
	if (!client->sensor_enabled) {
		ALOGE("couldn't allocate memory for sensor enabled array");
		goto err_calloc_client;
	}

	client->sensor_delay_ns = (int64_t *) calloc(smod->handle_last + 1, sizeof(int64_t));
	if (!client->sensor_delay_ns) {
		ALOGE("couldn't allocate memory for sensor delay array");
		goto err_calloc_sensor_enabled;
	}

	client->smod = smod;
	client->sock_fd = fd;
	epoll_add_fd(smod->epoll_fd, fd, client);

	err = smodule_client_send_list(client);

	ALOGI("new client connected on fd %d\n", fd);

	return client;

err_calloc_sensor_enabled:
	free(client->sensor_enabled);
err_calloc_client:
	free(client);
err_accept:
	close(fd);
err:
	return NULL;
}

static int smodule_client_free(struct smodule_client *client)
{
	struct smodule *smod = client->smod;
	int i;

	ALOGI("fd%d: removing this sensor client\n", client->sock_fd);

	// First disable sensors if necessary
	for (i = 0; i <= smod->handle_last; i++) {
		if (client->sensor_enabled[i])
			smodule_client_update_activate(client, i, 0);
	}

	epoll_del_fd(smod->epoll_fd, client->sock_fd);
	smodule_remove_client(smod, client);
	if (client->sensor_delay_ns)
		free(client->sensor_delay_ns);
	if (client->sensor_enabled)
		free(client->sensor_enabled);
	free(client);

	return 0;
}

static void smodule_client_handle_event(struct smodule_client *client, struct epoll_event *event)
{
	ALOGV("fd%d: events=%x", client->sock_fd, event->events);

	if (event->events & EPOLLIN) {
		struct sensors_proxy_cmd cmd;
		int err = recv_all(client->sock_fd, &cmd, sizeof(cmd));
		if (err) {
			if (errno == ECONNRESET) {
				// Peer resetted the connection, remove client and clean up
				smodule_client_free(client);
			} else {
				ALOGE("fd%d: recv client data failed unexpectedly",
				      client->sock_fd);
			}
		} else {
			switch (cmd.cmd) {
			case SENSORS_PROXY_CMD_ACTIVATE:
				smodule_client_update_activate(client, cmd.handle,
							       cmd.activate_enabled);

				// We also may need to update the sensor delay
				smodule_client_update_delay(client, cmd.handle);

				break;

			case SENSORS_PROXY_CMD_SET_DELAY:{
					ALOGI("fd%d: setDelay: handle=%d ns=%lld",
					      client->sock_fd, cmd.handle, cmd.set_delay_ns);

					// First store the clients delay setting
					client->sensor_delay_ns[cmd.handle] = cmd.set_delay_ns;
					smodule_client_update_delay(client, cmd.handle);
					break;
				}
			default:
				break;
			}
		}
	} else {
		ALOGI("fd%d: unexpected event: events=%x\n", client->sock_fd, event->events);
		if (errno == ECONNRESET) {
			// Peer resetted the connection, remove client and clean up
			smodule_client_free(client);
		}
	}
}

// Sensors module functions
//
static void *smodule_poll_thread(void *arg)
{
	struct smodule *smod = (struct smodule *)arg;
	sensors_event_t events[smod->sensor_count];
	int ret, n, i, j;

	ALOGI("%s: thread started: smod@%p", __func__, smod);

	// Sensor event dispatcher
	//
	// We deliver a single sensor event to the client only if that sensor is
	// enabled. Sending unkown events to clients makes real trouble. There
	// is still room for optimization.
	//
	while (!smod->stop_thread) {
		n = smod->device->poll(smod->device, events, smod->sensor_count);
		ALOGV("%s: poll returned: %d", __func__, n);
		if (n <= 0) {
			ALOGE("sensor poll failed: %s", n < 0 ? strerror(-n) : "returned 0");
			continue;
		}
		// Policy: send sensor data to clients with at least one sensor enabled
		pthread_mutex_lock(&smod->mutex);
		for (i = 0; i < smod->client_count; i++) {
			struct smodule_client *client = smod->clients[i];
			if (client->sensors_enabled <= 0)
				continue;
			for (j = 0; j < n; j++) {
				if (client->sensor_enabled[events[j].sensor]) {
					ret = send(client->sock_fd, &events[j],
						   sizeof(sensors_event_t), 0);
					if (ret < 0) {
						ALOGE("couldn't send sensor event %d to fd %d: %s",
						      j, client->sock_fd, strerror(errno));
						break;
					}
				}
			}
		}
		pthread_mutex_unlock(&smod->mutex);
	}
	return NULL;
}

static int smodule_add_client(struct smodule *smod, struct smodule_client *client)
{
	int i, err = -1;

	pthread_mutex_lock(&smod->mutex);
	for (i = 0; i < SMODULE_CLIENT_MAX; i++) {
		if (!smod->clients[i]) {
			smod->clients[i] = client;
			smod->client_count = i + 1;
			ALOGI("added client@%p, fd=%d: client_count=%d\n",
			      client, client->sock_fd, smod->client_count);
			err = 0;
			break;
		}
	}
	pthread_mutex_unlock(&smod->mutex);

	ALOGE_IF(err, "couldn't add client");
	return -1;
}

static int smodule_remove_client(struct smodule *smod, struct smodule_client *client)
{
	int i, j, err = -1;

	pthread_mutex_lock(&smod->mutex);
	for (i = 0; i < smod->client_count; i++) {
		if (smod->clients[i] == client) {
			smod->client_count--;
			for (j = i; j < smod->client_count; j++)
				smod->clients[j] = smod->clients[j + 1];
			smod->clients[smod->client_count] = NULL;
			ALOGI("fd%d: client@%p removed, %d client(s) remaining\n",
			      client->sock_fd, client, smod->client_count);
			err = 0;
			break;
		}
	}
	pthread_mutex_unlock(&smod->mutex);

	ALOGE_IF(err, "couldn't remove client");
	return err;
}

static struct smodule *smodule_new(const char *hw_module_id)
{
	struct sensors_module_t *module;
	struct sockaddr_un server;
	struct smodule *smod;
	pthread_attr_t attr;
	int err;

	smod = (struct smodule *)calloc(1, sizeof(*smod));
	if (!smod) {
		ALOGE("couldn't allocate memory for smodule");
		return NULL;
	}
	module = smod->module;

	// Load the proprietary hardware sensor libraries
	ALOGI("loading '%s' hw module\n", hw_module_id);
	err = hw_get_module(hw_module_id, (hw_module_t const **)&module);
	if (err) {
		ALOGE("hw_get_module() failed: %s", strerror(-err));
		return NULL;
	}

	ALOGI("Hardware module '%s' loded", hw_module_id);
	ALOGI("  Module API version: %d", module->common.module_api_version);
	ALOGI("  HAL API version: %d", module->common.hal_api_version);
	ALOGI("  ID: %s", module->common.id);
	ALOGI("  Name: %s", module->common.name);
	ALOGI("  Author: %s", module->common.author);

	err = sensors_open(&module->common, &smod->device);
	if (err) {
		ALOGE("sensor_open() failed: %s", strerror(-err));
		goto err_calloc_smod;
	}

	// Get list of sensors for that sensor device
	smod->sensor_count = module->get_sensors_list(module, &smod->sensor_list);
	if (smod->sensor_count <= 0) {
		ALOGE("get_sensor_list() returned %d", smod->sensor_count);
		goto err_sensors_open;
	}

	ALOGI("Sensors found: %d", smod->sensor_count);
	for (int i = 0; i < smod->sensor_count; i++) {
		const struct sensor_t *s = &smod->sensor_list[i];
		if (s->handle > smod->handle_last)
			smod->handle_last = s->handle;
		ALOGI("Name %s vendor %s version %d handle %d type %d "
		      "maxRange %f resolution %f power %fmA minDelay %d\n",
		      s->name, s->vendor, s->version, s->handle, s->type,
		      s->maxRange, s->resolution, s->power, s->minDelay);
	}
	ALOGI("Last sensor handle: %d", smod->handle_last);

	smod->sensors_enabled = (int *)calloc(smod->handle_last + 1, sizeof(int));
	if (!smod->sensors_enabled) {
		ALOGE("couldn't allocate memory for sensors enabled array");
		goto err_sensors_open;
	}

	smod->sensor_delay_ns = (int64_t *) calloc(smod->handle_last + 1, sizeof(int64_t));
	if (!smod->sensor_delay_ns) {
		ALOGE("couldn't allocate memory for sensor delay array");
		goto err_calloc_sensors_enabled;
	}

	// We use epoll to monitor multiple file descriptors
	smod->epoll_fd = epoll_create(EPOLL_DEFAULT_SIZE);
	if (smod->epoll_fd < 0) {
		ALOGE("couldn't create epoll instance: %s", strerror(errno));
		goto err_calloc_sensor_delay_ns;
	}

	// We use TCP stream sockets for communication
	smod->sock_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (smod->sock_fd < 0) {
		ALOGE("couldn't open ocket: %s", strerror(errno));
		goto err_epoll_create;
	}

	/* Unlink existing socket */
	err = unlink(SENSORS_PROXY_PATH);
	if (err && errno != ENOENT)
		ALOGE("couldn't unlink %s: %s", SENSORS_PROXY_PATH, strerror(errno));

	// Setup listening on that socket
	memset(&server, 0, sizeof(server));
	server.sun_family = AF_UNIX;
	snprintf(server.sun_path, UNIX_PATH_MAX, "%s", SENSORS_PROXY_PATH);
	err = bind(smod->sock_fd, (struct sockaddr *)&server, sizeof(server));
	if (err) {
		ALOGE("couldn't bind socket: %s", strerror(errno));
		goto err_socket;
	}

	err = listen(smod->sock_fd, 5);
	if (err) {
		ALOGE("couldn't listen: %s", strerror(errno));
		goto err_socket;
	}

	// Add fd to epoll list to accept connections
	err = epoll_add_fd(smod->epoll_fd, smod->sock_fd, smod);
	if (err)
		goto err_socket;

	// We need a mutex to protect the list of sensor clients.
	err = pthread_mutex_init(&smod->mutex, NULL);
	if (err) {
		ALOGE("couldn't initialze mutex: %s", strerror(-err));
		goto err_socket;
	}
	// Finally we start the sensor data polling thread.
	pthread_attr_init(&attr);
	err = pthread_create(&smod->poll_thread, &attr, smodule_poll_thread, smod);
	if (err) {
		ALOGE("couldn't create sensor polling thread: %s", strerror(-err));
		goto err_socket;
	}

	return smod;

err_socket:
	close(smod->sock_fd);
err_epoll_create:
	close(smod->epoll_fd);
err_calloc_sensors_enabled:
	free(smod->sensors_enabled);
err_calloc_sensor_delay_ns:
	free(smod->sensor_delay_ns);
err_sensors_open:
	sensors_close(smod->device);
err_calloc_smod:
	free(smod);

	return NULL;
}

static void smodule_free(struct smodule *smod)
{
	// Stop and cancel the sensor poll thread
	smod->stop_thread = 1;
	pthread_kill(smod->poll_thread, SIGSTOP);
	pthread_join(smod->poll_thread, NULL);

	// Now cleanup
	close(smod->sock_fd);
	close(smod->epoll_fd);
	free(smod->sensors_enabled);
	free(smod->sensor_delay_ns);
	sensors_close(smod->device);
	free(smod);
}

static void smodule_handle_event(struct smodule *smod, struct epoll_event *event)
{
	struct smodule_client *client;

	if (event->events & EPOLLIN) {
		client = smodule_client_new(smod);
		if (client)
			smodule_add_client(smod, client);
	} else {
		ALOGW("%s: unknown event %x", __func__, event->events);
	}
}

void smodule_event_loop(struct smodule *smod)
{
	struct epoll_event events[EPOLL_EVENTS_MAX];

	while (1) {
		const int nfds = epoll_wait(smod->epoll_fd, events, EPOLL_EVENTS_MAX, -1);
		int i;

		if (nfds < 0) {
			if (errno == EINTR)	/* Interrupted by signal, restart */
				continue;
			ALOGE("epoll_wait failed: %s", strerror(errno));
			return;
		}

		for (i = 0; i < nfds; i++) {
			struct epoll_event *event = &events[i];
			if (event->data.ptr == smod) {
				smodule_handle_event(smod, event);
			} else {
				struct smodule_client *client =
				    (struct smodule_client *)event->data.ptr;
				smodule_client_handle_event(client, event);
			}
		}
	}
}

int main(int argc, char *argv[])
{
	struct smodule *smod;

	// Create a sensor module instance
	smod = smodule_new(SENSORS_SERVER_HARDWARE_MODULE_ID);
	if (!smod)
		return -1;

	// Main event loop
	smodule_event_loop(smod);

	ALOGE("exiting main()...");

	smodule_free(smod);

	return 0;
}
