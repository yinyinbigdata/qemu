/*
 * QTest testcase demo for virtio-pci devices
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
#include <string.h>
#include "bswap.h"
#include "libpci.h"
#include "hw/virtio-defs.h"
#include "hw/virtio-pci-defs.h"
#include "libqtest.h"

enum {
    /* Device address for this test */
    TEST_PCI_SLOT = 5,
    TEST_PCI_FUNC = 0,
    TEST_BAR0_IOADDR = 0x1000,
};

static void virtio_probe(void)
{
    PciDevice dev;

    if (!pci_probe(&dev, TEST_PCI_SLOT, TEST_PCI_FUNC)) {
        g_test_message("Probe failed, no device present\n");
        return;
    }

    /* "2.1 PCI Discovery" defines vendor/device IDs */
    g_assert_cmpint(pci_config_readw(&dev, PCI_VENDOR_ID), ==, 0x1af4);
    g_assert_cmpint(pci_config_readw(&dev, PCI_DEVICE_ID), ==, 0x1002);

    /* "2.1 PCI Discovery" defines the revision ID */
    g_assert_cmpint(pci_config_readb(&dev, PCI_REVISION_ID), ==, 0);

    /* "2.1 PCI Discovery" defines the subsystem IDs */
    g_assert_cmpint(pci_config_readw(&dev, PCI_SUBSYSTEM_ID), ==, 5);

    pci_map_bar_io(&dev, PCI_BASE_ADDRESS_0, TEST_BAR0_IOADDR);
    pci_enable(&dev);

    g_test_message("host features: %#x\n",
            le32_to_cpu(inl(TEST_BAR0_IOADDR + VIRTIO_PCI_HOST_FEATURES)));
    g_test_message("status: %#x\n", inb(TEST_BAR0_IOADDR + VIRTIO_PCI_STATUS));

    outl(TEST_BAR0_IOADDR + VIRTIO_PCI_STATUS,
         VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
}

int main(int argc, char **argv)
{
    const char *arch;
    QTestState *s = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);

    arch = qtest_get_arch();
    /* These tests only work on i386 and x86_64 */
    if (strcmp(arch, "i386") == 0 || strcmp(arch, "x86_64") == 0) {
        gchar *args =
            g_strdup_printf("-vnc none -device virtio-balloon-pci,addr=%u.%u",
                            TEST_PCI_SLOT, TEST_PCI_FUNC);
        s = qtest_start(args);
        g_free(args);

        qtest_add_func("/virtio/probe", virtio_probe);
    } else {
        g_test_message("Skipping unsupported arch `%s'\n", arch);
    }

    ret = g_test_run();

    if (s) {
        qtest_quit(s);
    }

    return ret;
}
