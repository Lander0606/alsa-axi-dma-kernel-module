/*
Simple version of the Xilinx AXI DMA driver made by Xilinx
Original driver: https://github.com/Xilinx/linux-xlnx/blob/master/drivers/dma/xilinx/xilinx_dma.c

Changes made by Lander Van Loock
*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lander Van Loock");
MODULE_DESCRIPTION("Custom AXI DMA Driver with Debugging and Error Handling");

#define DRIVER_NAME "lander_axidma"
#define DMA_MM2S_CTRL_OFFSET  0x00  /* MM2S Control Register Offset */
#define DMA_MM2S_STATUS_OFFSET  0x04 /* MM2S Status Register Offset */
#define DMA_S2MM_CTRL_OFFSET  0x30  /* S2MM Control Register Offset */
#define DMA_S2MM_STATUS_OFFSET  0x34 /* S2MM Status Register Offset */
#define DMA_RESET_MASK  0x4         /* DMA Reset Mask */

/* Logging macros for better readability */
#define LOG_INFO(dev, fmt, ...) dev_info(dev, "[INFO] " fmt, ##__VA_ARGS__)
#define LOG_ERR(dev, fmt, ...) dev_err(dev, "[ERROR] " fmt, ##__VA_ARGS__)

struct axidma_local {
    unsigned long mem_start;
    unsigned long mem_end;
    void __iomem *base_addr;
    int irq_mm2s;
    int irq_s2mm;
};

/* Helper function to read a register */
static inline u32 axidma_read(struct axidma_local *lp, u32 offset)
{
    return readl(lp->base_addr + offset);
}

/* Helper function to write to a register */
static inline void axidma_write(struct axidma_local *lp, u32 offset, u32 value)
{
    writel(value, lp->base_addr + offset);
}

/* Function to reset the DMA */
static void axidma_reset(struct axidma_local *lp)
{
    LOG_INFO(NULL, "Resetting AXI DMA...\n");

    /* Set the reset bit in both MM2S and S2MM control registers */
    axidma_write(lp, DMA_MM2S_CTRL_OFFSET, DMA_RESET_MASK);
    axidma_write(lp, DMA_S2MM_CTRL_OFFSET, DMA_RESET_MASK);

    /* Wait for the reset to complete */
    while (axidma_read(lp, DMA_MM2S_CTRL_OFFSET) & DMA_RESET_MASK ||
           axidma_read(lp, DMA_S2MM_CTRL_OFFSET) & DMA_RESET_MASK) {
        LOG_INFO(NULL, "Waiting for DMA reset to complete...\n");
    }

    LOG_INFO(NULL, "AXI DMA reset completed.\n");
}

static irqreturn_t axidma_irq_handler(int irq, void *dev_id)
{
    struct axidma_local *lp = (struct axidma_local *)dev_id;
    u32 mm2s_status = axidma_read(lp, DMA_MM2S_STATUS_OFFSET);
    u32 s2mm_status = axidma_read(lp, DMA_S2MM_STATUS_OFFSET);

    printk(KERN_INFO "AXI DMA IRQ: MM2S Status = 0x%x, S2MM Status = 0x%x\n",
           mm2s_status, s2mm_status);

    /* Clear interrupts (write back the status register values) */
    axidma_write(lp, DMA_MM2S_STATUS_OFFSET, mm2s_status);
    axidma_write(lp, DMA_S2MM_STATUS_OFFSET, s2mm_status);

    return IRQ_HANDLED;
}

static int axidma_probe(struct platform_device *pdev)
{
    struct resource *r_mem;
    struct resource *r_irq_mm2s, *r_irq_s2mm;
    struct axidma_local *lp;
    struct device *dev = &pdev->dev;
    int ret;

    LOG_INFO(dev, "Probing AXI DMA\n");

    /* Allocate memory for the driver structure */
    lp = devm_kzalloc(dev, sizeof(*lp), GFP_KERNEL);
    if (!lp) {
        LOG_ERR(dev, "Failed to allocate memory for driver data\n");
        return -ENOMEM;
    }
    platform_set_drvdata(pdev, lp);

    /* Map memory */
    r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!r_mem) {
        LOG_ERR(dev, "Failed to get memory resource\n");
        return -ENODEV;
    }

    lp->mem_start = r_mem->start;
    lp->mem_end = r_mem->end;

    if (!devm_request_mem_region(dev, lp->mem_start, resource_size(r_mem), DRIVER_NAME)) {
        LOG_ERR(dev, "Failed to request memory region\n");
        return -EBUSY;
    }

    lp->base_addr = devm_ioremap(dev, lp->mem_start, resource_size(r_mem));
    if (!lp->base_addr) {
        LOG_ERR(dev, "Failed to map memory\n");
        return -EIO;
    }

    LOG_INFO(dev, "Memory mapped at %p\n", lp->base_addr);

    /* Get MM2S IRQ */
    lp->irq_mm2s = platform_get_irq(pdev, 0);
    if (lp->irq_mm2s < 0) {
        dev_err(dev, "Failed to get MM2S IRQ\n");
        return lp->irq_mm2s;
    }

    /* Request MM2S IRQ */
    rc = devm_request_irq(dev, lp->irq_mm2s, landeraxidriver_irq, 0, DRIVER_NAME, lp);
    if (rc) {
        dev_err(dev, "Failed to request MM2S IRQ %d\n", lp->irq_mm2s);
        return rc;
    }

    /* Get S2MM IRQ */
    lp->irq_s2mm = platform_get_irq(pdev, 1);
    if (lp->irq_s2mm < 0) {
        dev_err(dev, "Failed to get S2MM IRQ\n");
        return lp->irq_s2mm;
    }

    /* Request S2MM IRQ */
    rc = devm_request_irq(dev, lp->irq_s2mm, landeraxidriver_irq, 0, DRIVER_NAME, lp);
    if (rc) {
        dev_err(dev, "Failed to request S2MM IRQ %d\n", lp->irq_s2mm);
        return rc;
    }

    dev_info(dev, "landeraxidriver initialized at 0x%lx, IRQs: MM2S=%d, S2MM=%d\n",
             lp->mem_start, lp->irq_mm2s, lp->irq_s2mm);

    platform_set_drvdata(pdev, lp);

    LOG_INFO(dev, "AXI DMA successfully probed\n");
    return 0;
}

static int axidma_remove(struct platform_device *pdev)
{
    struct axidma_local *lp = platform_get_drvdata(pdev);

    LOG_INFO(&pdev->dev, "Removing AXI DMA driver\n");

    platform_set_drvdata(pdev, NULL);
    return 0;
}

static const struct of_device_id axidma_of_match[] = {
    { .compatible = "lander,axi-dma" },
    {},
};
MODULE_DEVICE_TABLE(of, axidma_of_match);

static struct platform_driver axidma_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = axidma_of_match,
    },
    .probe = axidma_probe,
    .remove = axidma_remove,
};

module_platform_driver(axidma_driver);