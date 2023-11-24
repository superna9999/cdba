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
#include <sys/time.h>
#include <alloca.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "abcd-server.h"
#include "circ_buf.h"
#include "device.h"
#include "device_parser.h"
#include "boot.h"
#include "list.h"

static bool quit_invoked;
static const char *username;

struct device *selected_device;

int tty_open(const char *tty, struct termios *old)
{
	struct termios tios;
	int ret;
	int fd;

	fd = open(tty, O_RDWR | O_NOCTTY | O_EXCL);
	if (fd < 0)
		err(1, "unable to open \"%s\"", tty);

	ret = tcgetattr(fd, old);
	if (ret < 0)
		err(1, "unable to retrieve \"%s\" tios", tty);

	memset(&tios, 0, sizeof(tios));
	tios.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
	tios.c_iflag = IGNPAR;
	tios.c_oflag = 0;

	tcflush(fd, TCIFLUSH);

	ret = tcsetattr(fd, TCSANOW, &tios);
	if (ret < 0)
		err(1, "unable to update \"%s\" tios", tty);

	return fd;
}

static void boot_opened(void)
{
	const uint8_t one = 1;

	warnx("boot connection opened");

	abcd_send_buf(MSG_BOOT_PRESENT, 1, &one);
}

static void boot_info(const void *buf, size_t len)
{
	fprintf(stderr, "%s\n", (const char *)buf);
}

static void boot_disconnect(void)
{
	const uint8_t zero = 0;

	abcd_send_buf(MSG_BOOT_PRESENT, 1, &zero);
}

static struct boot_ops abcd_boot_ops = {
	.opened = boot_opened,
	.disconnect = boot_disconnect,
	.info = boot_info,
};

static void msg_select_board(const void *param)
{
	selected_device = device_open(param, username, &abcd_boot_ops);
	if (!selected_device) {
		fprintf(stderr, "failed to open %s\n", (const char *)param);
		quit_invoked = true;
	}

	abcd_send(MSG_SELECT_BOARD);
}

static void *boot_payload;
static size_t boot_size;

static void msg_boot_download(const void *data, size_t len)
{
	size_t new_size = boot_size + len;
	char *newp;

	newp = realloc(boot_payload, new_size);
	if (!newp)
		err(1, "failed too expant boot scratch area");

	memcpy(newp + boot_size, data, len);

	boot_payload = newp;
	boot_size = new_size;

	if (!len) {
		device_boot(selected_device, boot_payload, boot_size);

		abcd_send(MSG_BOOT_DOWNLOAD);
		free(boot_payload);
		boot_payload = NULL;
		boot_size = 0;
	}
}

void abcd_send_buf(int type, size_t len, const void *buf)
{
	struct msg msg = {
		.type = type,
		.len = len
	};

	write(STDOUT_FILENO, &msg, sizeof(msg));
	if (len)
		write(STDOUT_FILENO, buf, len);
}

static int handle_stdin(int fd, void *buf)
{
	static struct circ_buf recv_buf = { 0 };
	struct msg *msg;
	struct msg hdr;
	size_t n;
	int ret;

	ret = circ_fill(STDIN_FILENO, &recv_buf);
	if (ret < 0 && errno != EAGAIN) {
		fprintf(stderr, "read %d\n", ret);
		return -1;
	}

	for (;;) {
		n = circ_peak(&recv_buf, &hdr, sizeof(hdr));
		if (n != sizeof(hdr))
			return 0;

		if (CIRC_AVAIL(&recv_buf) < sizeof(*msg) + hdr.len)
			return 0;

		msg = malloc(sizeof(*msg) + hdr.len);
		circ_read(&recv_buf, msg, sizeof(*msg) + hdr.len);

		switch (msg->type) {
		case MSG_CONSOLE:
			device_write(selected_device, msg->data, msg->len);
			break;
		case MSG_BOOT_PRESENT:
			break;
		case MSG_SELECT_BOARD:
			msg_select_board(msg->data);
			break;
		case MSG_HARDRESET:
			// fprintf(stderr, "hard reset\n");
			break;
		case MSG_POWER_ON:
			device_power(selected_device, true);

			abcd_send(MSG_POWER_ON);
			break;
		case MSG_POWER_OFF:
			device_power(selected_device, false);

			abcd_send(MSG_POWER_OFF);
			break;
		case MSG_BOOT_DOWNLOAD:
			msg_boot_download(msg->data, msg->len);
			break;
		case MSG_BOOT:
			fprintf(stderr, "boot\n");
			break;
		case MSG_STATUS_UPDATE:
			device_print_status(selected_device);
			break;
		case MSG_VBUS_ON:
			device_usb(selected_device, true);
			break;
		case MSG_VBUS_OFF:
			device_usb(selected_device, false);
			break;
		case MSG_SEND_BREAK:
			device_send_break(selected_device);
			break;
		case MSG_LIST_DEVICES:
			device_list_devices(username);
			break;
		case MSG_BOARD_INFO:
			device_info(username, msg->data, msg->len);
			break;
		default:
			fprintf(stderr, "unk %d len %d\n", msg->type, msg->len);
			exit(1);
		}

		free(msg);
	}

	return 0;
}

struct watch {
	struct list_head node;

	int fd;
	int (*cb)(int, void*);
	void *data;
};

struct timer {
	struct list_head node;
	struct timeval tv;

	void (*cb)(void *);
	void *data;
};

static struct list_head read_watches = LIST_INIT(read_watches);
static struct list_head timer_watches = LIST_INIT(timer_watches);

void watch_del_readfd(int fd)
{
	struct watch *w;
	struct watch *tmp;

	list_for_each_entry_safe(w, tmp, &read_watches, node) {
		if (w->fd == fd) {
			list_del(&w->node);
			free(w);
		}
	}
}

void watch_add_readfd(int fd, int (*cb)(int, void*), void *data)
{
	struct watch *w;

	w = calloc(1, sizeof(*w));
	w->fd = fd;
	w->cb = cb;
	w->data = data;

	list_add(&read_watches, &w->node);
}

void watch_timer_add(int timeout_ms, void (*cb)(void *), void *data)
{
	struct timeval tv_timeout;
	struct timeval now;
	struct timer *t;

	t = calloc(1, sizeof(*t));

	gettimeofday(&now, NULL);

	tv_timeout.tv_sec = timeout_ms / 1000;
	tv_timeout.tv_usec = (timeout_ms % 1000) * 1000;

	t->cb = cb;
	t->data = data;
	timeradd(&now, &tv_timeout, &t->tv);

	list_add(&timer_watches, &t->node);
}

static struct timeval *watch_timer_next(void)
{
	static struct timeval timeout;
	struct timeval now;
	struct timer *next;
	struct timer *t;

	if (list_empty(&timer_watches))
		return NULL;

	next = list_entry_first(&timer_watches, struct timer, node);

	list_for_each_entry(t, &timer_watches, node) {
		if (timercmp(&t->tv, &next->tv, <))
			next = t;
	}

	gettimeofday(&now, NULL);
	timersub(&next->tv, &now, &timeout);
	if (timeout.tv_sec < 0) {
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
	}

	return &timeout;
}

static void watch_timer_invoke(void)
{
	struct timeval now;
	struct timer *tmp;
	struct timer *t;

	gettimeofday(&now, NULL);

	list_for_each_entry_safe(t, tmp, &timer_watches, node) {
		if (timercmp(&t->tv, &now, <)) {
			t->cb(t->data);

			list_del(&t->node);
			free(t);
		}
	}
}

static void sigpipe_handler(int signo)
{
	quit_invoked = true;
}

void watch_quit(void)
{
	quit_invoked = true;
}

int main(int argc, char **argv)
{
	struct timeval *timeoutp;
	struct watch *w;
	fd_set rfds;
	int flags;
	int nfds;
	int ret;

	signal(SIGPIPE, sigpipe_handler);

	username = getenv("ABCD_USER");

	ret = device_parser(".abcd");
	if (ret) {
		ret = device_parser("/etc/abcd");
		if (ret) {
			fprintf(stderr, "device parser: unable to open config file\n");
			exit(1);
		}
	}

	watch_add_readfd(STDIN_FILENO, handle_stdin, NULL);

	flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

	while (!quit_invoked) {
		nfds = 0;

		list_for_each_entry(w, &read_watches, node) {
			nfds = MAX(nfds, w->fd);
			FD_SET(w->fd, &rfds);
		}

		if (!FD_ISSET(STDIN_FILENO, &rfds)) {
			fprintf(stderr, "rfds is trash!\n");
			goto done;
		}

		timeoutp = watch_timer_next();
		ret = select(nfds + 1, &rfds, NULL, NULL, timeoutp);
		if (ret < 0 && errno == EINTR)
			continue;
		else if (ret < 0)
			break;

		watch_timer_invoke();

		list_for_each_entry(w, &read_watches, node) {
			if (FD_ISSET(w->fd, &rfds)) {
				ret = w->cb(w->fd, w->data);
				if (ret < 0) {
					fprintf(stderr, "cb returned %d\n", ret);
					goto done;
				}
			}
		}
	}

done:

	if (selected_device)
		device_close(selected_device);

	return 0;
}
