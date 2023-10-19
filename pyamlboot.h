#ifndef __PYAMLBOOT_H__
#define __PYAMLBOOT_H__

#include "device.h"

struct pyamlboot;
struct boot_ops;

struct pyamlboot *pyamlboot_open(const char *serial, struct boot_ops *ops, void *);
int pyamlboot_boot(struct device *dev, const void *data, size_t len);

#endif
