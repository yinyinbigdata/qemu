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

#include <glib.h>
#include "bswap.h"
#include "libqtest.h"
#include "libpci.h"

enum {
    /* PCI controller I/O ports */
    PCI_CONFIG_ADDR = 0xcf8,
    PCI_CONFIG_DATA = 0xcfc,
};

static void pci_config_setup(PciDevice *dev, unsigned int offset)
{
    outl(PCI_CONFIG_ADDR, 0x80000000 | (dev->devfn << 8) | (offset & ~3));
}

uint8_t pci_config_readb(PciDevice *dev, unsigned int offset)
{
    pci_config_setup(dev, offset);
    return inb(PCI_CONFIG_DATA + (offset & 3));
}

void pci_config_writeb(PciDevice *dev, unsigned int offset, uint8_t b)
{
    pci_config_setup(dev, offset);
    outb(PCI_CONFIG_DATA + (offset & 3), b);
}

uint16_t pci_config_readw(PciDevice *dev, unsigned int offset)
{
    pci_config_setup(dev, offset);
    return inw(PCI_CONFIG_DATA + (offset & 2));
}

void pci_config_writew(PciDevice *dev, unsigned int offset, uint16_t w)
{
    pci_config_setup(dev, offset);
    outw(PCI_CONFIG_DATA + (offset & 2), w);
}

uint32_t pci_config_readl(PciDevice *dev, unsigned int offset)
{
    pci_config_setup(dev, offset);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_writel(PciDevice *dev, unsigned int offset, uint32_t l)
{
    pci_config_setup(dev, offset);
    outl(PCI_CONFIG_DATA, l);
}

/* Initialize a PciDevice if a device is present on the bus */
bool pci_probe(PciDevice *dev, unsigned int slot, unsigned int func)
{
    uint16_t vendor;

    dev->devfn = (slot << 3) | func;
    vendor = pci_config_readw(dev, PCI_VENDOR_ID);
    if (vendor == 0xffff || vendor == 0x0) {
        return false;
    }
    return true;
}

/* Map an I/O BAR to a specific address */
void pci_map_bar_io(PciDevice *dev, unsigned int bar, uint16_t addr)
{
    uint32_t old_bar;

    old_bar = pci_config_readl(dev, bar);
    g_assert_cmphex(old_bar & PCI_BASE_ADDRESS_SPACE, ==,
                    PCI_BASE_ADDRESS_SPACE_IO);

    /* Address must be valid */
    g_assert_cmphex(addr & ~PCI_BASE_ADDRESS_IO_MASK, ==, 0);

    pci_config_writel(dev, bar, addr);

    /* BAR must have accepted address */
    old_bar = pci_config_readl(dev, bar);
    g_assert_cmphex(old_bar & PCI_BASE_ADDRESS_IO_MASK, ==, addr);
}

/* Enable memory and I/O decoding so BARs can be accessed */
void pci_enable(PciDevice *dev)
{
    uint16_t cmd;

    cmd = pci_config_readw(dev, PCI_COMMAND);
    pci_config_writew(dev, PCI_COMMAND,
                      cmd | PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
}
