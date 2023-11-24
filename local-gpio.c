/*
 * Copyright (c) 2023, Linaro Ltd.
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
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "abcd-server.h"
#include "local-gpio.h"

#include <gpiod.h>

enum {
	GPIO_POWER = 0,			// Power input enable
	GPIO_FASTBOOT_KEY,		// Usually volume key
	GPIO_POWER_KEY,			// Key to power the device
	GPIO_USB_DISCONNECT,		// Simulate main USB connection
	GPIO_COUNT
};

enum {
	GPIO_ACTIVE_HIGH = 0,
	GPIO_ACTIVE_LOW,
};

struct local_gpio {
	char * gpiochip_desc;
	struct gpiod_chip *chip;
	unsigned int gpio_present[GPIO_COUNT];
	unsigned int gpio_offset[GPIO_COUNT];
	unsigned int gpio_polarity[GPIO_COUNT];
	struct gpiod_line *gpio_line[GPIO_COUNT];
};

static int local_gpio_device_power(struct local_gpio *local_gpio, bool on);
static void local_gpio_device_usb(struct local_gpio *local_gpio, bool on);

/*
 * fdio_gpio parameter: <gpiod chip description>;[<gpios>;...]
 * - gpiod chip description: "gpiochip0"
 * - gpios: type,id,polarity
 *   - type: POWER, FASTBOOT_KEY, POWER_KEY or USB_DISCONNECT
 *   - offset: line offset in chip
 *   - polarity: ACTIVE_HIGH or ACTIVE_LOW
 *
 * Example: gpiochip0;POWER,0,ACTIVE_LOW;FASTBOOT_KEY,1,ACTIVE_HIGH;POWER_KEY,2,ACTIVE_HIGH;USB_DISCONNECT,3,ACTIVE_LOW
 */

static void local_gpio_parse_config(struct local_gpio *local_gpio, char *control_dev)
{
	char *c;
	size_t device_len;

	// First liblocal description
	c = strchr(control_dev, ';');
	if (!c)
		device_len = strlen(control_dev);
	else
		device_len = c - control_dev;

	local_gpio->gpiochip_desc = strndup(control_dev, device_len);

	if (!c)
		return;

	// GPIOs
	while(c) {
		char *name, *off, *pol;
		unsigned gpio_type;
		unsigned gpio_offset;
		unsigned gpio_polarity;

		name = c + 1;
		off = strchr(name, ',');
		if (!off)
			errx(1, "GPIOs config invalid");
		off += 1;
		pol = strchr(off, ',');
		if (!pol)
			errx(1, "GPIOs config invalid");
		pol += 1;

		c = strchr(pol, ';');

		if (strncmp("POWER", name, off - name - 1) == 0)
			gpio_type = GPIO_POWER;
		else if (strncmp("FASTBOOT_KEY", name, off - name - 1) == 0)
			gpio_type = GPIO_FASTBOOT_KEY;
		else if (strncmp("POWER_KEY", name, off - name - 1) == 0)
			gpio_type = GPIO_POWER_KEY;
		else if (strncmp("USB_DISCONNECT", name, off - name - 1) == 0)
			gpio_type = GPIO_USB_DISCONNECT;
		else
			errx(1, "GPIOs type invalid: '%s'", name);

		gpio_offset = strtoul(off, NULL, 0);

		if (strncmp("ACTIVE_HIGH", pol, c - pol - 1) == 0)
			gpio_polarity = GPIO_ACTIVE_HIGH;
		else if (strncmp("ACTIVE_LOW", pol, c - pol - 1) == 0)
			gpio_polarity = GPIO_ACTIVE_LOW;
		else
			errx(1, "GPIOs polarity invalid: '%s'", pol);

		local_gpio->gpio_present[gpio_type] = 1;
		local_gpio->gpio_offset[gpio_type] = gpio_offset;
		local_gpio->gpio_polarity[gpio_type] = gpio_polarity;
	}
}

void *local_gpio_open(struct device *dev)
{
	struct local_gpio *local_gpio;
	int i;

	local_gpio = calloc(1, sizeof(*local_gpio));

	local_gpio_parse_config(local_gpio, dev->control_dev);

	local_gpio->chip = gpiod_chip_open_lookup(local_gpio->gpiochip_desc);
	if (!local_gpio->chip) {
		err(1, "Unable to open gpiochip '%s'", local_gpio->gpiochip_desc);
		return NULL;
	}

	for (i = 0; i < GPIO_COUNT; ++i) {
		struct gpiod_line_request_config cfg;

		if (!local_gpio->gpio_present[i])
			continue;

		cfg.consumer = "ABCD";
		cfg.request_type = GPIOD_LINE_REQUEST_DIRECTION_OUTPUT;
		cfg.flags = 0;

		if (local_gpio->gpio_polarity[i])
			cfg.flags = GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW;

		local_gpio->gpio_line[i] = gpiod_chip_get_line(local_gpio->chip,
							       local_gpio->gpio_offset[i]);

		if (!local_gpio->gpio_line[i]) {
			err(1, "Unable to find gpio %d offset %u", i, local_gpio->gpio_offset[i]);
			return NULL;
		}

		if (gpiod_line_request(local_gpio->gpio_line[i], &cfg, 0))  {
			err(1, "Unable to request gpio %d offset %u", i, local_gpio->gpio_offset[i]);
			return NULL;
		}
	}

	if (local_gpio->gpio_present[GPIO_POWER_KEY])
		dev->has_power_key = true;

	local_gpio_device_power(local_gpio, 0);

	if (dev->usb_always_on)
		local_gpio_device_usb(local_gpio, 1);
	else
		local_gpio_device_usb(local_gpio, 0);

	usleep(500000);

	return local_gpio;
}

static int local_gpio_toggle_io(struct local_gpio *local_gpio, unsigned int gpio, bool on)
{
	if (!local_gpio->gpio_present[gpio])
		return -EINVAL;

	if (gpiod_line_set_value(local_gpio->gpio_line[gpio], on))
		warnx("%s:%d unable to set value", __func__, __LINE__);

	return 0;
}

static int local_gpio_device_power(struct local_gpio *local_gpio, bool on)
{
	return local_gpio_toggle_io(local_gpio, GPIO_POWER, on);
}

static void local_gpio_device_usb(struct local_gpio *local_gpio, bool on)
{
	local_gpio_toggle_io(local_gpio, GPIO_USB_DISCONNECT, on);
}

int local_gpio_power(struct device *dev, bool on)
{
	struct local_gpio *local_gpio = dev->cdb;

	return local_gpio_device_power(local_gpio, on);
}

void local_gpio_usb(struct device *dev, bool on)
{
	struct local_gpio *local_gpio = dev->cdb;

	local_gpio_device_usb(local_gpio, on);
}

void local_gpio_key(struct device *dev, int key, bool asserted)
{
	struct local_gpio *local_gpio = dev->cdb;

	switch (key) {
	case DEVICE_KEY_BOOT:
		local_gpio_toggle_io(local_gpio, GPIO_FASTBOOT_KEY, asserted);
		break;
	case DEVICE_KEY_POWER:
		local_gpio_toggle_io(local_gpio, GPIO_POWER_KEY, asserted);
		break;
	}
}
