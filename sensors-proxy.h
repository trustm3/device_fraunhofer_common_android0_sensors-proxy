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

#ifndef ANDROID_SENSORS_PROXY_H
#define ANDROID_SENSORS_PROXY_H

#include <stdint.h>
#include <errno.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include <hardware/hardware.h>
#include <hardware/sensors.h>

__BEGIN_DECLS

#define SENSORS_PROXY_PATH "/data/trustme-com/sensors/sensors-proxy.sock"
#define SENSORS_MAX 32

enum sensors_proxy_cmd_e {
	SENSORS_PROXY_CMD_ACTIVATE = 0,
	SENSORS_PROXY_CMD_SET_DELAY,
	SENSORS_PROXY_CMD_BATCH,
	SENSORS_PROXY_CMD_FLUSH,
};

#define SENSORS_CHARS_MAX 64

struct sensors_strings_t {
	char name[SENSORS_CHARS_MAX];
	char vendor[SENSORS_CHARS_MAX];
};

struct sensors_proxy_cmd {
	int32_t cmd;
	int32_t handle;
	union {
		int32_t activate_enabled;
		int64_t set_delay_ns;
	};
};

__END_DECLS

#endif // ANDROID_SENSORS_PROXY_H
