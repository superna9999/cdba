/*
 * Copyright (c) 2016-2018, Linaro Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/file.h>
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "abcd-server.h"
#include "device.h"
#include "boot.h"
#include "pyamlboot.h"
#include "dfu.h"
#include "console.h"
#include "list.h"
#include "ppps.h"

#define ARRAY_SIZE(x) ((sizeof(x)/sizeof((x)[0])))

static struct list_head devices = LIST_INIT(devices);

void device_add(struct device *device)
{
	list_add(&devices, &device->node);
}

static void device_lock(struct device *device)
{
	char lock[PATH_MAX];
	int fd;
	int n;

	n = snprintf(lock, sizeof(lock), "/tmp/abcd-%s.lock", device->board);
	if (n >= (int)sizeof(lock))
		errx(1, "failed to build lockfile path");

	fd = open(lock, O_RDONLY | O_CREAT, 0666);
	if (fd >= 0)
		close(fd);

	fd = open(lock, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		err(1, "failed to open lockfile %s", lock);

	n = flock(fd, LOCK_EX | LOCK_NB);
	if (!n)
		return;

	warnx("board is in use, waiting...");

	n = flock(fd, LOCK_EX);
	if (n < 0)
		err(1, "failed to lock lockfile %s", lock);
}

static bool device_check_access(struct device *device,
				const char *username)
{
	struct device_user *user;

	if (!device->users)
		return true;

	if (!username)
		return false;

	list_for_each_entry(user, device->users, node) {
		if (!strcmp(user->username, username))
			return true;
	}

	return false;
}

static void device_open_boot(struct device *device)
{
	void *boot_stage_data;
	char *boot_stage_options;

	if (device->boot_stage >= MAX_BOOT_STAGES ||
	    device->boot_stage >= device->boot_num_stages)
		return;

	boot_stage_options = device->boot_stage_options[device->boot_stage];

	switch (device->boot_stages[device->boot_stage]) {
	case BOOT_PYAMLBOOT:
		boot_stage_data = pyamlboot_open(device, device->boot_ops, boot_stage_options);
		break;
	case BOOT_DFU:
		boot_stage_data = dfu_open(device, device->boot_ops, boot_stage_options);
		break;
	default:
		errx(1, "No boot type defined for stage %u", device->boot_stage);
	}

	device->boot_stage_data[device->boot_stage] = boot_stage_data;
}

void device_boot(struct device *device, const void *data, size_t len)
{
	void *boot_stage_data = device->boot_stage_data[device->boot_stage];

	warnx("booting the board...");

	device->do_boot(boot_stage_data, data, len);

	switch (device->boot_stages[device->boot_stage]) {
	case BOOT_PYAMLBOOT:
		pyamlboot_close(device, boot_stage_data);
		break;
	case BOOT_DFU:
		dfu_close(device, boot_stage_data);
		break;
	default:
		errx(1, "No boot type defined for stage %u", device->boot_stage);
	}

	device->do_boot = NULL;

	/* Increase boot stage */
	++device->boot_stage;

	if (device->boot_stage >= MAX_BOOT_STAGES ||
	    device->boot_stage >= device->boot_num_stages)
		return;

	device_open_boot(device);
}

struct device *device_open(const char *board,
			   const char *username,
			   struct boot_ops *boot_ops)
{
	struct device *device;

	list_for_each_entry(device, &devices, node) {
		if (!strcmp(device->board, board))
			goto found;
	}

	return NULL;

found:
	assert(device->open || device->console_dev);

	if (!device_check_access(device, username))
		return NULL;

	device_lock(device);

	if (device->open) {
		device->cdb = device->open(device);
		if (!device->cdb)
			errx(1, "failed to open device controller");
	}

	if (device->console_dev)
		console_open(device);

	if (device->usb_always_on)
		device_usb(device, true);

	device->boot_ops = boot_ops;

	device_open_boot(device);

	return device;
}

static void device_impl_power(struct device *device, bool on)
{
	device->power(device, on);
}

static void device_key(struct device *device, int key, bool asserted)
{
	if (device->key)
		device->key(device, key, asserted);
}

enum {
	DEVICE_STATE_START,
	DEVICE_STATE_CONNECT,
	DEVICE_STATE_PRESS,
	DEVICE_STATE_RELEASE_PWR,
	DEVICE_STATE_RELEASE_BOOT,
	DEVICE_STATE_RUNNING,
};

static void device_tick(void *data)
{
	struct device *device = data;

	switch (device->state) {
	case DEVICE_STATE_START:
		if (device->boot_key_timeout)
			device_key(device, DEVICE_KEY_BOOT, true);
		if (device->has_power_key)
			device_key(device, DEVICE_KEY_POWER, false);

		device->state = DEVICE_STATE_CONNECT;
		watch_timer_add(10, device_tick, device);
		break;
	case DEVICE_STATE_CONNECT:
		/* Connect power and USB */
		device_impl_power(device, true);
		device_usb(device, true);

		if (device->has_power_key) {
			device->state = DEVICE_STATE_PRESS;
			watch_timer_add(250, device_tick, device);
		} else if (device->boot_key_timeout) {
			device->state = DEVICE_STATE_RELEASE_BOOT;
			watch_timer_add(device->boot_key_timeout * 1000, device_tick, device);
		} else {
			device->state = DEVICE_STATE_RUNNING;
		}
		break;
	case DEVICE_STATE_PRESS:
		/* Press power key */
		device_key(device, DEVICE_KEY_POWER, true);

		device->state = DEVICE_STATE_RELEASE_PWR;
		watch_timer_add(100, device_tick, device);
		break;
	case DEVICE_STATE_RELEASE_PWR:
		/* Release power key */
		device_key(device, DEVICE_KEY_POWER, false);

		if (device->boot_key_timeout) {
			device->state = DEVICE_STATE_RELEASE_BOOT;
			watch_timer_add(device->boot_key_timeout * 1000, device_tick, device);
		} else {
			device->state = DEVICE_STATE_RUNNING;
		}
		break;
	case DEVICE_STATE_RELEASE_BOOT:
		device_key(device, DEVICE_KEY_BOOT, false);
		device->state = DEVICE_STATE_RUNNING;
		break;
	}
}

static int device_power_on(struct device *device)
{
	if (!device || !device->power)
		return 0;

	device->state = DEVICE_STATE_START;
	device_tick(device);

	return 0;
}

static int device_power_off(struct device *device)
{
	if (!device || !device->power)
		return 0;

	device->power(device, false);

	return 0;
}

int device_power(struct device *device, bool on)
{
	if (on)
		return device_power_on(device);
	else
		return device_power_off(device);
}

void device_print_status(struct device *device)
{
	if (device->print_status)
		device->print_status(device);
}

void device_usb(struct device *device, bool on)
{
	if (device->usb) {
		if (device->ppps_path)
			ppps_power(device, on);
		else
			device->usb(device, on);
	}
}

int device_write(struct device *device, const void *buf, size_t len)
{
	if (!device)
		return 0;

	assert(device->write);

	return device->write(device, buf, len);
}

void device_send_break(struct device *device)
{
	if (device->send_break)
		device->send_break(device);
}

void device_list_devices(const char *username)
{
	struct device *device;
	size_t len;
	char buf[80];

	list_for_each_entry(device, &devices, node) {
		if (!device_check_access(device, username))
			continue;

		if (device->name)
			len = snprintf(buf, sizeof(buf), "%-20s %s", device->board, device->name);
		else
			len = snprintf(buf, sizeof(buf), "%s", device->board);

		abcd_send_buf(MSG_LIST_DEVICES, len, buf);
	}

	abcd_send_buf(MSG_LIST_DEVICES, 0, NULL);
}

void device_info(const char *username, const void *data, size_t dlen)
{
	char *description = NULL;
	struct device *device;
	size_t len = 0;

	list_for_each_entry(device, &devices, node) {
		if (strncmp(device->board, data, dlen))
			continue;

		if (!device_check_access(device, username))
			continue;

		if (device->description) {
			description = device->description;
			len = strlen(device->description);
			break;
		}
	}

	abcd_send_buf(MSG_BOARD_INFO, len, description);
}

void device_close(struct device *dev)
{
	if (!dev->usb_always_on)
		device_usb(dev, false);
	device_power(dev, false);

	if (dev->close)
		dev->close(dev);
}
