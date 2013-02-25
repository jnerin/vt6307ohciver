/*
 * Copyright (C) 2007 Krzysztof Halasa <khc@pm.waw.pl>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is based on undocumented features of VT6307 IEEE-1394
 * OHCI chip. Use at your own risk.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pci/pci.h>
#include <sys/io.h>
#include <sys/mman.h>

#define DEBUG 0

#define	VENDOR_ID		0x1106
#define	DEVICE_ID		0x3044
#define	DEVICE_CLASS		0x0C0000UL

#define MEM_SIZE		0x800 /* bytes */
#define IO_SIZE			0x80 /* bytes */
#define EEPROM_SIZE		0x30 /* bytes */

#define I2C_DEVSEL_RD		(0x50 << 1 | 1)
#define I2C_DEVSEL_WR		(0x50 << 1)

static uint32_t io_ports;


static void output(int cs, int clock, int dataout)
{
	uint32_t value = 0x10 /* enable outputs */ |
		(cs ? 8 : 0) | (clock ? 4 : 0) | (dataout ? 2 : 0);

#if DEBUG
	fprintf(stderr, " %c%c%c ", cs ? ' ' : '-', clock ? 'c' : ' ',
		dataout ? '1' : '0');
#endif
	outl_p(value, io_ports + 0x20);
}

/**************************************************************************
 *
 * I^2C version
 *
 **************************************************************************/

static void start_i2c(void)
{
	output(1, 1, 0); usleep(1000); // stopped (SCL = 1, SDA = 1)
	output(1, 0, 0); usleep(1000); // running (SCL = 0)
}

static void stop_i2c(void)
{
	output(1, 0, 0); usleep(100); // running (SCL = 0)
	output(1, 1, 0); usleep(100);
	output(1, 1, 1); usleep(100); // stopped (SCL = 1, SDA = 1)
	
}

static void send_bit_i2c(uint32_t v)
{
	output(1, 0, v); usleep(100); // running (SCL = 0)
	output(1, 1, v); usleep(100);
	output(1, 0, v); usleep(100); // running (SCL = 0)
}


static void write_byte_i2c(uint8_t value)
{
	int i = 8;
	while (i--) {
		send_bit_i2c(value & 0x80);
		value <<= 1;
	}
	send_bit_i2c(0); /* actually we should read it - ACK bit */
}

static void write_i2c(uint8_t address, uint8_t value)
{
	printf("writing 0x%02X at address 0x%02X\n", value, address);

	output(1, 0, 1); usleep(100); /* Init */
	output(1, 1, 1); usleep(2000);

	start_i2c();
	write_byte_i2c(I2C_DEVSEL_WR);
	write_byte_i2c(address);
	write_byte_i2c(value); usleep(2000);
	stop_i2c();
	usleep(2000);
}

/**************************************************************************
 *
 * 4-wire version
 *
 **************************************************************************/

static void send_bits_4w(int bits, uint32_t value)
{
	if (bits < 32)
		value <<= 32 - bits;

	while (bits--) {
		output(1, 0, value & 0x80000000); usleep(10);
		output(1, 1, value & 0x80000000); usleep(10);
		output(1, 0, value & 0x80000000); usleep(10);
		value <<= 1;
	}
}

static void write_4w(uint8_t address, uint16_t value)
{
	printf("writing 0x%04X at address 0x%02X\n", value, address);

	output(0, 0, 0); usleep(10); /* Init */

	output(1, 0, 0); usleep(10);
	send_bits_4w(3, 0x4);	/* CMD: EWEN = write enable */
	send_bits_4w(6, 0x30);
	send_bits_4w(16, 0x0000);
	output(0, 0, 0); usleep(10);

	output(1, 0, 0); usleep(10);
	send_bits_4w(3, 0x5);	/* CMD: WRITE */
	send_bits_4w(6, address);
	send_bits_4w(16, value);
	output(0, 0, 0); usleep(1000);

	output(1, 0, 0); usleep(10);
	send_bits_4w(3, 0x4);	/* CMD: EWDS = write disable */
	send_bits_4w(6, 0x00);
	send_bits_4w(16, 0x0000);
	output(0, 0, 0); usleep(10);
}

/**************************************************************************
 *
 * display EEPROM contents
 *
 **************************************************************************/

static inline uint32_t readl(uint32_t *reg)
{
	return *(volatile uint32_t *)reg;
}

static inline void writel(uint32_t *reg, uint32_t value)
{
	*(volatile uint32_t *)reg = value;
	readl(reg); /* flush buffers */
}

static void display(pciaddr_t mem_addr)
{
	int i, page_size = getpagesize();
	uint32_t mem_length, offset, *reg, cnt, v22 = 0xFF;

	mem_addr += 4; /* Only interested in GUID PROM register */
	mem_length = 4;

	fprintf(stderr, "Page size is %i (0x%X) bytes\n"
		"GUID PROM register address is 0x%08llx\n",
		page_size, page_size, (unsigned long long)mem_addr);
	offset = mem_addr & (page_size - 1);
	mem_addr -= offset;
	mem_length += offset;

	fprintf(stderr, "Mapping %i (0x%X) bytes of memory at 0x%08llx\n",
		mem_length, mem_length, (unsigned long long)mem_addr);

	if ((i = open("/dev/mem", O_RDWR) ) < 0) {
		fprintf(stderr, "Cannot open /dev/mem: %s\n", strerror(errno));
		exit(1);
	}
	reg = mmap(0, mem_length, PROT_READ | PROT_WRITE, MAP_SHARED, i,
		   mem_addr);
	if (reg == MAP_FAILED) {
		fprintf(stderr, "Unable to mmap memory region #0: %s\n",
			strerror(errno));
		exit(1);
	}

	fprintf(stderr, "Mapped mem region #0 at virtual address %p\n", reg);
	/* point to the GUID PROM register */
	reg = (uint32_t*)(offset + (uint8_t *)reg);

	fprintf(stderr, "GUID PROM register is at virtual address %p\n\n"
		"EEPROM dump:\n", reg);

	writel(reg, 0x80 << 24); /* reset internal counter */
	while (readl(reg) & (0x80 << 24))
		; /* wait */

	for (cnt = 0; cnt < EEPROM_SIZE; cnt++) {
		if (cnt % 16 == 0)
			printf("%02X:", cnt);
		writel(reg, 0x02 << 24); /* next byte */
		while (readl(reg) & (0x02 << 24))
			; /* wait */
		printf(" %02X", i = (readl(reg) >> 16) & 0xFF);
		if (cnt == 0x22)
			v22 = i;
		if (cnt % 16 == 15)
			printf("\n");
	}
	printf("\n");

	printf("Your VT6307 chip is in %s mode\n", (v22 == 0) ? "OHCI 1.0" :
	       (v22 == 8) ? "OHCI 1.1" : "unknown");
}


/**************************************************************************
 *
 * main
 *
 **************************************************************************/

int main (int argc, char *argv[])
{
	struct pci_access *pacc;
	struct pci_dev *dev;
	struct pci_filter filter;
	char *msg;
	uint16_t command;
	int v1_1 = 0, i2c;

	if (argc != 2 &&
	    ((argc != 3) ||
	     (!(v1_1 = !strcmp(argv[2], "1.1")) && strcmp(argv[2], "1.0")))) {
		fprintf(stderr,
			"VT6307 OHCI mode config\n"
			"Version 0.9\n"
			"Copyright (C) 2007 Krzysztof Halasa <khc@pm.waw.pl>\n"
			"\n"
			"Usage: vt6307ohciver <pci_device> [ 1.0 | 1.1 ]\n");
		exit(1);
	}

	if (iopl(3)) {
		fprintf(stderr, "iopl() failed (must be root)\n");
		exit(1);
	}

	pacc = pci_alloc();
	pci_filter_init(pacc, &filter);
	if ((msg = pci_filter_parse_slot(&filter, argv[1]))) {
		fprintf(stderr, "Invalid pci_device\n");
		exit(1);
	}

	filter.vendor = VENDOR_ID;
	filter.device = DEVICE_ID;

	pci_init(pacc);
	pci_scan_bus(pacc);

	for (dev = pacc->devices; dev; dev = dev->next)
		if (pci_filter_match(&filter, dev))
			break;

	if (!dev) {
		fprintf(stderr, "Device %s not found\n", argv[2]);
		exit(1);
	}

	pci_fill_info(dev, PCI_FILL_BASES | PCI_FILL_SIZES);

	if (dev->size[0] != MEM_SIZE || dev->size[1] != IO_SIZE) {
		fprintf(stderr, "Unexpected MEM/IO region size, is it"
			" VT6307 chip?\n");
		exit(1);
	}

	command = pci_read_word(dev, PCI_COMMAND);

	if ((command & PCI_COMMAND_IO) == 0) {
		fprintf(stderr, "Device disabled, trying to enable it\n");
		pci_write_word(dev, PCI_COMMAND, command | PCI_COMMAND_IO);
	}

	io_ports = dev->base_addr[1] & PCI_BASE_ADDRESS_IO_MASK;

	i2c = (inl(io_ports + 0x20) & 0x80) ? 0 : 1;

	fprintf(stderr, "I/O region #1 is at %04X\n", io_ports);
	fprintf(stderr, "It seems your VT6307 chip is connected to %s "
		"EEPROM\n", i2c ? "I^2C (24c01 or similar)" : "93c46");

	if (argc == 3) {
		/* enable direct access to pins */
		outl_p(inl(io_ports) | 0x80, io_ports);
		if (i2c)
			write_i2c(0x22, v1_1 ? 8 : 0);
		else
			write_4w(0x11, v1_1 ? 8 : 0);
		/* disable direct access to pins */
		outl_p(0x20, io_ports + 0x20);
		fprintf(stderr, "Please reboot\n");
	} else
		display(dev->base_addr[0] & PCI_BASE_ADDRESS_MEM_MASK);

	if ((command & PCI_COMMAND_IO) == 0) {
		fprintf(stderr, "Disabling device\n");
		pci_write_word(dev, PCI_COMMAND, command);
	}

	exit(0);
}
