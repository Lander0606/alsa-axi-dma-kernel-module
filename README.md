# DMA-ALSA PCM kernel module 

## Description

Custom Linux kernel module to create a new ALSA PCM device from a buffer in memory. Buffer management is included so that samples are transferred with a DMA engine and buffers are automatically allocated and released.

A Makefile is included to compile the module.

After compilation, the module can be loaded with the `sudo insmod /lib/modules/<kernel-version>/build/dma-alsa.ko` command. A new ALSA playback device will be visible in `aplay -l`:

```console
card X: Card [DMA Audio Card], device Y: dma_pcm []
  Subdevices: 1/1
  Subdevice #0: subdevice #0
```

## Important

This module is created for **Linux kernel 6.1**. Every deviation from this version can result in a compile and/or runtime error of the module.

The module is tested on an [Ultra96v2](https://www.avnet.com/wps/portal/us/products/avnet-boards/avnet-board-families/ultra96-v2/) SBC with the Xilinx AXI DMA as DMA Engine. A Linux distribution with kernel 6.1 was created with PetaLinux 2023.2 from a hardware design containing the Xilinx AXI DMA IP block.