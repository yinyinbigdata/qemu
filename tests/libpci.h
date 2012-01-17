/*
 * QTest PCI library
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Stefan Hajnoczi <stefanha@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef LIBPCI_H
#define LIBPCI_H

#include <stdbool.h>
#include "hw/pci_regs.h"

/* A PCI Device
 *
 * Set up a PciDevice instance with pci_probe().  The device can then be used
 * for configuration space access and other operations.
 */
typedef struct {
    uint8_t devfn;
} PciDevice;

bool pci_probe(PciDevice *dev, unsigned int slot, unsigned int func);
void pci_map_bar_io(PciDevice *dev, unsigned int bar, uint16_t addr);
void pci_enable(PciDevice *dev);

/* Configuration space access */
uint8_t pci_config_readb(PciDevice *dev, unsigned int offset);
void pci_config_writeb(PciDevice *dev, unsigned int offset, uint8_t b);
uint16_t pci_config_readw(PciDevice *dev, unsigned int offset);
void pci_config_writew(PciDevice *dev, unsigned int offset, uint16_t w);
uint32_t pci_config_readl(PciDevice *dev, unsigned int offset);
void pci_config_writel(PciDevice *dev, unsigned int offset, uint32_t l);

#endif /* LIBPCI_H */
