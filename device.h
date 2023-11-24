#ifndef __DEVICE_H__
#define __DEVICE_H__

#include <termios.h>
#include "list.h"

struct boot_ops;

#define MAX_BOOT_STAGES  4

enum boot_stage {
	BOOT_NONE = 0,
	BOOT_PYAMLBOOT,
	BOOT_DFU,
};

struct device {
	char *board;
	char *control_dev;
	char *console_dev;
	char *name;
	char *description;
	char *ppps_path;
	struct list_head *users;
	unsigned voltage;
	bool tickle_mmc;
	bool usb_always_on;
	
	unsigned int boot_key_timeout;
	int state;
	bool has_power_key;

	struct boot_ops *boot_ops;
	char *boot_stage_options[MAX_BOOT_STAGES];
	void *boot_stage_data[MAX_BOOT_STAGES];
	enum boot_stage boot_stages[MAX_BOOT_STAGES];
	unsigned int boot_num_stages;
	unsigned int boot_stage;
	int (*do_boot)(void *boot_data, const void *data, size_t len);

	void *(*open)(struct device *dev);
	void (*close)(struct device *dev);
	int (*power)(struct device *dev, bool on);
	void (*usb)(struct device *dev, bool on);
	void (*print_status)(struct device *dev);
	int (*write)(struct device *dev, const void *buf, size_t len);
	void (*boot_key)(struct device *dev, bool on);
	void (*key)(struct device *device, int key, bool asserted);

	void (*send_break)(struct device *dev);

	void *cdb;

	int console_fd;
	struct termios console_tios;

	struct list_head node;
};

struct device_user {
	const char *username;

	struct list_head node;
};

void device_add(struct device *device);

struct device *device_open(const char *board,
			   const char *username,
			   struct boot_ops *boot_ops);
void device_close(struct device *dev);
int device_power(struct device *device, bool on);

void device_print_status(struct device *device);
void device_usb(struct device *device, bool on);
int device_write(struct device *device, const void *buf, size_t len);

void device_boot(struct device *device, const void *data, size_t len);

void device_send_break(struct device *device);
void device_list_devices(const char *username);
void device_info(const char *username, const void *data, size_t dlen);

enum {
	DEVICE_KEY_BOOT,
	DEVICE_KEY_POWER,
};

#endif
