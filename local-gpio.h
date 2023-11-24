#ifndef __LOCAL_GPIO_H__
#define __LOCAL_GPIO_H__

#include "device.h"

struct local_gpio;

void *local_gpio_open(struct device *dev);
int local_gpio_power(struct device *dev, bool on);
void local_gpio_usb(struct device *dev, bool on);
void local_gpio_key(struct device *dev, int key, bool on);

#endif
