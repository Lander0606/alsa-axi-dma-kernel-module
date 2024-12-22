# ALSA-AXI-DMA kernel module

## Description

DMA-based ALSA PCM Module to create a soundcard and PCM device to AXI DMA

This driver provides a basic ALSA PCM device that uses a DMA channel to transfer audio samples from kernel-allocated buffers to a target device.
It sets up a single sound card and a single PCM playback device, where audio data is pulled from the ALSA buffer and converted into a 64-bit word format per frame before being sent out through the DMA engine.

Supported Audio Formats:
 - Signed 24-bit samples, packed in 3 bytes per sample (S24_3LE)
 - Signed 24-bit samples in 4-byte containers (S24_LE), where the fourth byte is ignored to maintain the same 64-bit frame structure.

Supported Configurations:
 - Stereo (2-channel) only
 - 48 kHz sample rate
 - Hardware parameters such as period size and buffer size are restricted to specific ranges to ensure stable operation.

Operation:
 - The driver allocates a continuous DMA buffer and uses DMA transfers to feed samples to the hardware. When a period completes, a DMA completion callback triggers a workqueue job to safely interact with ALSA APIs, mark the period as elapsed, and start transferring the next period.
 - The PCM operations (open, close, hw_params, prepare, trigger, etc.) are implemented to interact seamlessly with ALSA applications, ensuring that streams can be started, stopped, paused, or resumed without glitches.

Limitations:
- This is a simplified reference driver and may need adjustments for hardware-specific DMA handling or additional sample formats and rates.

## Soundcard Characteristics

The full hardware characteristics of the created pcm device can be found as a `snd_pcm_hardware` struct in the module itself:

```c
static struct snd_pcm_hardware dma_pcm_hardware = {
    .info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
    .formats = SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S24_LE,
    .rates = SNDRV_PCM_RATE_48000,
    .rate_min = 48000,
    .rate_max = 48000,
    .channels_min = 2,
    .channels_max = 2,
    .buffer_bytes_max = AUDIO_BUFFER_SIZE,
    .period_bytes_min = 4096,
    .period_bytes_max = 16384,
    .periods_min = 2,
    .periods_max = 8,
};
```

## Installation

### Using GCC

A Makefile is included to compile the module with gcc. Make sure that the neccessary kernel header files are available on the system.

```c
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
```

### Using PetaLinux

First, create a new kernel module in the project with the following command:

```bash
petalinux-create -t modules --name alsa-axi-dma --enable
```

The `enable` option ensures that the module will be included in the final image by default. After the module has been created in the project, add the source file `alsa-axi-dma.c` to the module files in `/<project-dir>/project-spec/meta-user/recipes-modules/alsa-axi-dma/files/`. Finally, compile the module with the following command:

```bash
petalinux-build -c alsa-axi-dma
```

or if you want to compile the whole image at once:

```bash
petalinux-build
```

## Usage

After the module has been compiled and installed, it can be loaded with the `sudo insmod /lib/modules/<kernel-version>/build/alsa-axi-dma.ko` command. A new ALSA playback device will be visible in `aplay -l`:

```console
card X: Card [DMA Audio Card], device Y: dma_pcm []
  Subdevices: 1/1
  Subdevice #0: subdevice #0
```

From now on, the soundcard is available on the system like any other. Integration with PulseAudio 16.1 and PipeWire 1.2.6 has been tested and found to work.

## Important

This module is created to work with **Linux kernel 6.1**. Every deviation from this version can result in a compile or runtime error of the module.

The module is tested on an [Ultra96v2](https://www.avnet.com/wps/portal/us/products/avnet-boards/avnet-board-families/ultra96-v2/) SBC with the Xilinx AXI DMA as DMA hardware. A custom Linux distribution was created using PetaLinux 2023.2 from a hardware design containing the Xilinx AXI DMA IP block.

The current module will only work when the Xilinx AXI DMA hardware is available on the system. 

## References

1. [Writing an ALSA driver](https://www.kernel.org/doc/html/next/sound/kernel-api/writing-an-alsa-driver.html)
2. [ALSA: Writing the soundcard driver](https://events.linuxfoundation.org/wp-content/uploads/2023/12/Ivan-Orlov-Mentorship-12-7-23-Writing-the-soundcard-driver.pdf)
3. [ALSA project library reference](https://www.alsa-project.org/alsa-doc/alsa-lib/index.html)
4. [Xilinx: Linux DMA from User Space 2.0](https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/1027702787/Linux+DMA+From+User+Space+2.0)