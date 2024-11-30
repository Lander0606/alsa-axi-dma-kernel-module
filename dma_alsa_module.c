#include <linux/module.h>
#include <linux/init.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define PCM_DEVICE_NAME "dma_pcm"
#define CARD_NAME "DMA Audio Card"
#define AUDIO_BUFFER_SIZE (64 * 1024)   // 64 KB audio buffer

static struct snd_card *card;
static struct snd_pcm *pcm;
static struct dma_chan *dma_channel;    // A DMA Channel
static void *dma_buffer;                // Active buffer for PCM samples
static void *next_dma_buffer;           // Next buffer
static dma_addr_t dma_handle;           // Physical address for active DMA buffer
static dma_addr_t next_dma_handle;      // Physical address for next buffer
static struct mutex dma_lock;           // Mutex to protect DMA and buffer changes
static size_t buffer_fill_level = 0;    // Count for the amount of data in a buffer

/* ALSA PCM hardware parameters */
static struct snd_pcm_hardware dma_pcm_hardware = {
    .info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER, // Interleaved format (samples L/R on same place in memory) + supports block transfers
    .formats = SNDRV_PCM_FMTBIT_S24_3LE,                                 // 24-bit little-endian samples
    .rates = SNDRV_PCM_RATE_48000,                                      // Support only 48kHz sample frequency
    .rate_min = 48000,
    .rate_max = 48000,
    .channels_min = 2,                                                  // Stereo (L/R)
    .channels_max = 2,
    .buffer_bytes_max = AUDIO_BUFFER_SIZE,                              // Total buffer size = 64 KB
    .period_bytes_min = 4096,                                           // Min size of a "period" in the buffer -> 4096 / (6 bytes per frame) = minimal 682 frames per period
    .period_bytes_max = 16384,                                          // Max size of a "period" in the buffer -> 16384 / (6 bytes per frame) = minimal 2730 frames per period
    .periods_min = 2,                                                   // The buffer needs to contain at least 2 periods
    .periods_max = 4,                                                   // The buffer can contain at most 4 periods
};

/* DMA completed callback */
static void dma_transfer_callback(void *completion)
{
    /*
    This callback is executed whenever a DMA transfer is completed
        The DMA buffer is released so the memory is free again
    */

    void *completed_buffer = completion;

    // Check if completion buffer is valid
    if (!completion) {
        pr_err("dma-alsa: NULL completion buffer in transfer callback\n");
        return;
    }

    dma_addr_t phys_addr = virt_to_phys(completed_buffer);

    /* Free the transferred DMA buffer */
    dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, completed_buffer, phys_addr);

    pr_info("dma-alsa: dma transfer completed, buffer released at %p\n", completed_buffer);
}

/* Initialize the DMA channel */
static int init_dma_channel(void)
{
    /*
    This function is part of the __init() of the module to init the dma channel
        A DMA channel is requested taking into account the parameters set in the mask
    */

    dma_cap_mask_t mask;
    dma_cap_zero(mask);
 	dma_cap_set(DMA_SLAVE|DMA_PRIVATE, mask);
    dma_channel = dma_request_channel(mask, NULL, "dma0chan0");
    if (IS_ERR(dma_channel)) {
        pr_err("dma-alsa: couldnt request the dma channel\n");
        return PTR_ERR(dma_channel);
    }

    pr_info("dma-alsa: dma channel obtained: %s\n", dma_channel->device->dev->kobj.name);
    return 0;
}

/* Function to start the DMA transfer */
static int start_dma_transfer(void *src, size_t len, dma_addr_t phys_addr)
{
    /*
    This function is executed in write_to_buffer() to start a dma transfer
        A descriptor for the transfer is configured
        The callback for completion of the transfer is configured
    */

    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;

    if (!dma_channel) {
        pr_err("dma-alsa: dma_channel is NULL, cannot start transfer\n");
        return -EINVAL;
    }

    if (!src) {
        pr_err("dma-alsa: source buffer is NULL, cannot start transfer\n");
        return -EINVAL;
    }

    desc = dmaengine_prep_slave_single(dma_channel, phys_addr, len, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
    if (!desc) {
        pr_err("dma-alsa: could not prepare the dma descriptor\n");
        return -EINVAL;
    }

    desc->callback = dma_transfer_callback;
    desc->callback_param = src;

    cookie = dmaengine_submit(desc);
    if (dma_submit_error(cookie)) {
        pr_err("dma-alsa: dma transfer submission failed\n");
        return -EINVAL;
    }

    dma_async_issue_pending(dma_channel);

    pr_info("dma-alsa: dma transfer started for buffer at %p, length: %zu bytes\n", src, len);
    return 0;
}

/* Write audio to the buffer */
static void write_to_buffer(void *data, size_t size)
{
    /*
    This function is executed in the .ack callback to add and zero pad the new data to the DMA buffer
        The received data from the ALSA buffer is zero padded and combined to an L/R sample in 1 word (64 bit or 8 bytes)
        If the current dma_buffer is full, the dma transfer is started and a new buffer is assigned
    */

    // Ensure buffers are valid before accessing them
    if (!dma_buffer) {
        pr_err("dma-alsa: dma_buffer is NULL, cannot write data\n");
        return;
    }

    if (!next_dma_buffer) {
        pr_err("dma-alsa: next_dma_buffer is NULL, cannot switch buffers\n");
        return;
    }

    while (size > 0) {
        size_t space_left = AUDIO_BUFFER_SIZE - buffer_fill_level;
        size_t to_copy = min(space_left, size);

        /* Format data to 2x24-bit in a 64-bit word */
        uint8_t *src = data;
        uint64_t *dst = (uint64_t *)((char *)dma_buffer + buffer_fill_level);

        for (size_t i = 0; i < to_copy; i += 6) {
            uint32_t left = (src[0] << 16) | (src[1] << 8) | src[2];
            uint32_t right = (src[3] << 16) | (src[4] << 8) | src[5];
            *dst++ = ((uint64_t)left << 40) | ((uint64_t)right << 16); // Format: 64 bit on 64 bit systems
            src += 6;
        }

        buffer_fill_level += to_copy;
        data = (char *)data + to_copy;
        size -= to_copy;

        if (buffer_fill_level >= AUDIO_BUFFER_SIZE) {
            pr_info("dma-alsa: buffer full of samples, starting the DMA and change to a new buffer\n");

            mutex_lock(&dma_lock);

            /* Start the DMA transfer of the current buffer */
            if (start_dma_transfer(dma_buffer, AUDIO_BUFFER_SIZE, dma_handle)) {
                pr_err("dma-alsa: dma transfer failed\n");
            }

            /* Change buffers */
            dma_buffer = next_dma_buffer;
            dma_handle = next_dma_handle;

            /* Allocate new empty buffer */
            next_dma_buffer = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &next_dma_handle, GFP_KERNEL);
            if (!next_dma_buffer) {
                pr_err("dma-alsa: couldnt allocate a new buffer\n");
                mutex_unlock(&dma_lock);
                return;
            }
            pr_info("dma-alsa: new buffer allocated at %p\n", dma_buffer);

            /* Reset the buffer fill level */
            buffer_fill_level = 0;

            mutex_unlock(&dma_lock);
        }
    }
}


/* PCM open callback */
static int dma_pcm_open(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed when an application opens the PCM device
        The hardware specific parameters dma_pcm_hardware are set
        2 DMA buffers of AUDIO_BUFFER_SIZE are allocated and set to use
    */

    struct snd_pcm_runtime *runtime = substream->runtime;

    runtime->hw = dma_pcm_hardware;

    dma_buffer = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &dma_handle, GFP_KERNEL);
    if (!dma_buffer) {
        pr_err("dma-alsa: couldnt allocate dma buffer\n");
        return -ENOMEM;
    }

    next_dma_buffer = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &next_dma_handle, GFP_KERNEL);
    if (!next_dma_buffer) {
        pr_err("dma-alsa: couldnt allocate next dma buffer\n");
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        return -ENOMEM;
    }

    runtime->dma_area = dma_buffer; // Let ALSA use the dma_buffer as buffer to place frames (sound samples)
    runtime->dma_bytes = AUDIO_BUFFER_SIZE;

    pr_info("dma-alsa: pcm opened, buffer allocated at %p\n", dma_buffer);
    return 0;
}

/* PCM close callback */
static int dma_pcm_close(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed when an application closes the PCM device
        The mutex lock is requested to release the dma buffers
        2 DMA buffers are released
    */

    mutex_lock(&dma_lock);

    // Check and release the current buffer
    if (dma_buffer) {
        pr_info("dma-alsa: releasing current buffer at %p\n", dma_buffer);
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        dma_buffer = NULL; // Prevent double-free
    } else {
        pr_warn("dma-alsa: no current buffer to release\n");
    }

    // Check and release the next buffer
    if (next_dma_buffer) {
        pr_info("dma-alsa: releasing next buffer at %p\n", next_dma_buffer);
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, next_dma_buffer, next_dma_handle);
        next_dma_buffer = NULL; // Prevent double-free
    } else {
        pr_warn("dma-alsa: no next buffer to release\n");
    }

    mutex_unlock(&dma_lock);
    return 0;
}

/* PCM trigger callback */
static int dma_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    /*
    This callback is executed when a certain trigger is sent to the pcm stream: start, stop, pause
        The received trigger is determined (start / stop)
        Action is taken accordingly to the trigger type
    */

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        pr_info("dma-alsa: playback started\n");
        break;

    case SNDRV_PCM_TRIGGER_STOP:
        pr_info("dma-alsa: playback stopped\n");
        dmaengine_terminate_sync(dma_channel);
        break;

    case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
        pr_info("dma-alsa: playback paused\n");
        dmaengine_pause(dma_channel);
        break;

    case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
        pr_info("dma-alsa: playback resumed\n");
        dmaengine_resume(dma_channel);
        break;

    default:
        pr_err("dma-alsa: unsupported trigger command: %d\n", cmd);
        return -EINVAL;
    }

    return 0;
}

/* PCM ACK callback */
static int dma_pcm_ack(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed whenever new data from ALSA is available in dma_area
        The amount of frames in the ALSA buffer is requested in frames
        The data is fetched from the buffer
        The data is written to another buffer to be zero padded and transferred with dma (TODO?)
    */

    struct snd_pcm_runtime *runtime = substream->runtime;
    snd_pcm_sframes_t frames = snd_pcm_lib_buffer_bytes(substream);

    /* Write data */
    void *data = runtime->dma_area; // Data from the application
    size_t size = frames * 6;       // 24-bit stereo, 6 bytes per frame

    write_to_buffer(data, size);

    return 0;
}

/* PCM hw_params callback */
static int dma_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
    /*
    This callback is executed when new parameters of the pcm device need to be set
        The function just forwards the call to snd_pcm_lib_malloc_pages
    */

    struct snd_pcm_runtime *runtime = substream->runtime;

    if (!runtime) {
        pr_err("dma-alsa: runtime is NULL\n");
        return -EINVAL;
    }

    if (!dma_buffer) {
        pr_err("dma-alsa: dma_buffer is NULL\n");
        return -ENOMEM;
    }

    // Check if the sample format is correct
    if (params_format(params) != SNDRV_PCM_FORMAT_S24_3LE) {
        pr_err("dma-alsa: unsupported format requested: %d\n", params_format(params));
        return -EINVAL;
    }

    // Check if sample rate is correct
    if (params_rate(params) != 48000) {
        pr_err("dma-alsa: unsupported sample rate\n");
        return -EINVAL;
    }

    // Check if number of channels is correct
    if (params_channels(params) != 2) {
        pr_err("dma-alsa: unsupported number of channels requested: %d\n", params_channels(params));
        return -EINVAL;
    }

    // Keep the existing buffer settings
    runtime->dma_area = dma_buffer;
    runtime->dma_bytes = AUDIO_BUFFER_SIZE;

    if (!runtime->dma_area) {
        pr_err("dma-alsa: unable to allocate DMA buffer\n");
        return -ENOMEM;
    }

    pr_info("dma-alsa: hw_params configured, buffer size: %zu, buffer address: %p\n", runtime->dma_bytes, runtime->dma_area);
    return 0;
}

/* PCM pointer callback */
static snd_pcm_uframes_t dma_pcm_pointer(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed whenever the application wants to know where this module (the hardware) is located in the playback buffer
        The buffer fill level is used to calculate how many frames are currently in the buffer
    */

    size_t buffer_pos = buffer_fill_level;
    return bytes_to_frames(substream->runtime, buffer_pos);
}

/* PCM hw_free callback */
static int dma_pcm_hw_free(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed by ALSA to free all hardware and buffers
        The buffer is freed by resetting the buffer fill level
    */
    
    struct snd_pcm_runtime *runtime = substream->runtime;

    // Check if the runtime and dma are valid
    if (!runtime || !dma_buffer) {
        pr_err("dma-alsa: hw free failed, invalid runtime or buffer\n");
        return -EINVAL;
    }

    // Reset buffer management
    buffer_fill_level = 0;
    
    pr_info("dma-alsa: hw free successfull\n");
    return 0;
}

/* PCM prepare callback */
static int dma_pcm_prepare(struct snd_pcm_substream *substream)
{
    /*
    The prepare callback is called by ALSA when a sound stream (playback or capture) is ready to start
    This occurs just before the trigger callback with the SNDRV_PCM_TRIGGER_START command is called
        The runtime and DMA buffer need to be checked for existance
        Earlier DMA transfers need to be stopped
    */

    struct snd_pcm_runtime *runtime = substream->runtime;

    // Check if the runtime and dma are valid
    if (!runtime || !dma_buffer) {
        pr_err("dma-alsa: prepare failed, invalid runtime or buffer\n");
        return -EINVAL;
    }

    pr_info("dma-alsa: preparing hw, resetting DMA and buffers\n");

    // Reset buffer management
    buffer_fill_level = 0;

    // Stop ealier DMA transfers
    dmaengine_terminate_sync(dma_channel);

    pr_info("dma-alsa: prepare completed successfully\n");
    return 0;
}

/* PCM operations */
static struct snd_pcm_ops dma_pcm_ops = {
    /*
    This struct contains the callback functions for pcm stream operations from ALSA
    */

    .open = dma_pcm_open,
    .close = dma_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = dma_pcm_hw_params,
    .hw_free = dma_pcm_hw_free,
    .prepare = dma_pcm_prepare,
    .trigger = dma_pcm_trigger,
    .pointer = dma_pcm_pointer,
    .ack = dma_pcm_ack,
};

/* Kernel module init */
static int __init dma_pcm_init(void)
{
    /*
    This function contains the init of the DMA ALSA kernel module
        A mutex lock for the DMA buffer is created
        The DMA channel is requested
        A new sound card is created from this module
        A new pcm device is created from this module
        The callback functions for this sound card are set
        The sound card is registered with the system
    */

    int err;

    mutex_init(&dma_lock);

    pr_info("dma-alsa: initialization of the module\n");

    err = init_dma_channel();
    if (err)
        return err;

    err = snd_card_new(dma_channel->device->dev, -1, NULL, THIS_MODULE, 0, &card);
    if (err < 0)
        return err;

    snprintf(card->driver, sizeof(card->driver), CARD_NAME);
    snprintf(card->shortname, sizeof(card->shortname), CARD_NAME);
    snprintf(card->longname, sizeof(card->longname), CARD_NAME);

    err = snd_pcm_new(card, PCM_DEVICE_NAME, 0, 1, 0, &pcm);
    if (err < 0) {
        snd_card_free(card);
        return err;
    }

    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &dma_pcm_ops);

    err = snd_card_register(card);
    if (err < 0) {
        snd_card_free(card);
        return err;
    }

    pr_info("dma-alsa: module successfully initialized\n");
    return 0;
}

/* Cleanup kernel module */
static void __exit dma_pcm_exit(void)
{
    /*
    This function contains the exit of the DMA ALSA kernel module
        The 2 allocated DMA buffers are released
        The DMA channel is released
        The sound card is unregistered from the system
    */

    pr_info("dma-alsa: module cleanup started\n");

    // Release buffers with checks
    if (dma_buffer) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        dma_buffer = NULL;
        pr_info("dma-alsa: dma buffer released\n");
    } else {
        pr_warn("dma-alsa: dma buffer already released\n");
    }

    if (next_dma_buffer) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, next_dma_buffer, next_dma_handle);
        next_dma_buffer = NULL;
        pr_info("dma-alsa: next dma buffer released\n");
    } else {
        pr_warn("dma-alsa: next dma buffer already released\n");
    }

    if (dma_channel) {
        dma_release_channel(dma_channel);
        dma_channel = NULL; // Ensure dma_channel is not reused
        pr_info("dma-alsa: dma channel released\n");
    }

    if (card) {
        snd_card_free(card);
        card = NULL; // Ensure card is not reused
    }

    pr_info("dma-alsa: module removed\n");
}

module_init(dma_pcm_init);
module_exit(dma_pcm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lander Van Loock");
MODULE_DESCRIPTION("DMA ALSA PCM kernel module with AXI DMA via DMAengine");