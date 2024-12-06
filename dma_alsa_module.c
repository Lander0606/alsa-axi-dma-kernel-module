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
static struct dma_chan *dma_channel;        // A DMA Channel
static void *dma_buffer1, *dma_buffer2;     // 2 DMA buffers for zero padded data
static dma_addr_t dma_handle1, dma_handle2; // Physical addresses for the dma buffers
static void *active_dma_buffer;             // Active DMA buffer to write samples to
static dma_addr_t active_dma_handle;        // Physical address of the active DMA buffer
static void *next_dma_buffer;               // Next DMA buffer if the active one is full
static dma_addr_t next_dma_handle;          // Physical address of the next DMA buffer
static struct mutex dma_lock;               // Mutex to protect DMA and buffer changes
static size_t buffer_fill_level = 0;        // Count for the amount of data in a buffer

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
    dma_addr_t phys_addr;

    // Check if completion buffer is valid
    if (!completion) {
        pr_err("dma-alsa: NULL completion buffer in transfer callback\n");
        return;
    }

    phys_addr = virt_to_phys(completed_buffer);

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
    if (!active_dma_buffer || !next_dma_buffer) {
        pr_err("dma-alsa: write: active or next dma buffer invalid\n");
        return;
    }

    while (size > 5) {
        size_t space_left = AUDIO_BUFFER_SIZE - buffer_fill_level;
        size_t to_copy = min(space_left, size);

        /* Format data to 2x24-bit in a 64-bit word */
        uint8_t *src = data;
        uint64_t *dst = (uint64_t *)((char *)active_dma_buffer + buffer_fill_level);

        pr_info("dma-alsa: write() will process %zu bytes to DMA buffer\n", size);

        for (size_t i = 0; i < to_copy; i += 6) {
            uint32_t left = (src[0] << 16) | (src[1] << 8) | src[2];
            uint32_t right = (src[3] << 16) | (src[4] << 8) | src[5];
            *dst++ = ((uint64_t)left << 40) | ((uint64_t)right << 16); // Format: 64 bit on 64 bit systems
            src += 6;
        }

        buffer_fill_level += to_copy;
        data = (char *)data + to_copy;
        size -= to_copy;

        pr_info("dma-alsa: %zu bytes left to process to DMA buffer in write()\n", size);

        if (buffer_fill_level >= AUDIO_BUFFER_SIZE) {
            pr_info("dma-alsa: buffer full of samples, starting the DMA and change to a new buffer\n");

            mutex_lock(&dma_lock);

            /* Start the DMA transfer of the current buffer */
            if (start_dma_transfer(active_dma_buffer, AUDIO_BUFFER_SIZE, active_dma_handle)) {
                pr_err("dma-alsa: dma transfer failed\n");
            }

            /* Change buffers */
            active_dma_buffer = next_dma_buffer;
            active_dma_handle = next_dma_handle;

            /* Allocate new empty buffer */
            next_dma_buffer = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &next_dma_handle, GFP_KERNEL);
            if (!next_dma_buffer) {
                pr_err("dma-alsa: couldnt allocate a new buffer\n");
                mutex_unlock(&dma_lock);
                return;
            }
            pr_info("dma-alsa: new buffer allocated at %p\n", active_dma_buffer);

            /* Reset the buffer fill level */
            buffer_fill_level = 0;

            mutex_unlock(&dma_lock);
        }
    }
    pr_info("dma-alsa: processed %zu bytes to DMA buffer\n", size);
}


/* PCM open callback */
static int dma_pcm_open(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed when an application opens the PCM device
        The hardware specific parameters dma_pcm_hardware are set
        1 ALSA buffer of AUDIO_BUFFER_SIZE is allocated and set to use => we use the ALSA API for buffer allocation
        2 DMA buffers of AUDIO_BUFFER_SIZE are allocated and set to use
    */

    struct snd_pcm_runtime *runtime = substream->runtime;

    runtime->hw = dma_pcm_hardware;

    /* Preallocate buffer memory */
    snd_pcm_lib_preallocate_pages(
        substream,
        SNDRV_DMA_TYPE_CONTINUOUS,
        NULL,  // Use NULL for default DMA memory allocator
        AUDIO_BUFFER_SIZE,
        AUDIO_BUFFER_SIZE
    );


    /* Allocate DMA buffers */
    dma_buffer1 = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &dma_handle1, GFP_KERNEL);
    if (!dma_buffer1) {
        pr_err("dma-alsa: could not allocate dma_buffer1\n");
        snd_pcm_lib_free_pages(substream);
        return -ENOMEM;
    }

    dma_buffer2 = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &dma_handle2, GFP_KERNEL);
    if (!dma_buffer2) {
        pr_err("dma-alsa: could not allocate dma_buffer2\n");
        snd_pcm_lib_free_pages(substream);
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer1, dma_handle1);
        return -ENOMEM;
    }

    /* Set DMA buffers */
    active_dma_buffer = dma_buffer1;
    active_dma_handle = dma_handle1;
    next_dma_buffer = dma_buffer2;
    next_dma_handle = dma_handle2;

    pr_info("dma-alsa: PCM opened, DMA buffers allocated at %p and %p\n", dma_buffer1, dma_buffer2);
    return 0;
}

/* PCM close callback */
static int dma_pcm_close(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed when an application closes the PCM device
        The mutex lock is requested to release the dma buffers
        1 ALSA buffer is released
        2 DMA buffers are released
    */
    mutex_lock(&dma_lock);

    /* Free preallocated buffer memory */
    snd_pcm_lib_free_pages(substream);

    // Check and release the DMA buffers
    if (dma_buffer1) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer1, dma_handle1);
        dma_buffer1 = NULL;
        active_dma_buffer = NULL;
    }

    if (dma_buffer2) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer2, dma_handle2);
        dma_buffer2 = NULL;
        next_dma_buffer = NULL;
    }

    mutex_unlock(&dma_lock);
    return 0;
}

/* PCM ACK callback */
static int dma_pcm_ack(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed when ALSA has placed data in the PCM ringbuffer and asks the hardware to process it
        The amount of new frames in the buffer are calculated with appl_ptr (application) and hw_ptr (hardware)
        The amount of new bytes is calculated from it
        The new data is written to the DMA buffer
        ALSA is informed that the information is processed so that the hardware pointer can be updated (by ALSA internally)

    IMPORTANT: appl_ptr and hw_ptr are the application and hardware pointer but are abstract frame indexes rather than memory addresses
    */

    struct snd_pcm_runtime *runtime = substream->runtime;
    size_t avail_frames, size;
    void *src;

    pr_debug("dma-alsa: Current ALSA state: %d\n", runtime->status->state);

    if (!runtime || !runtime->dma_area || !active_dma_buffer) {
        pr_err("dma-alsa: invalid buffers in ack\n");
        return -EINVAL;
    }

    /* Calculate available frames using appl_ptr and hw_ptr */
    avail_frames = runtime->control->appl_ptr - runtime->status->hw_ptr;
    if ((ssize_t)avail_frames < 0) {
        avail_frames += runtime->buffer_size; // Handle circular buffer wrap-around
    }

    if (avail_frames == 0) {
        pr_info("dma-alsa: no data available in ALSA buffer\n");
        return 0;
    }

    size = frames_to_bytes(runtime, avail_frames);
    src = runtime->dma_area + frames_to_bytes(runtime, runtime->status->hw_ptr);

    pr_debug("dma-alsa: processing %zu bytes from ALSA buffer\n", size);

    // write_to_buffer()

    /* Update hw_ptr for ALSA's internal tracking */
    runtime->status->hw_ptr = (runtime->status->hw_ptr + avail_frames) % runtime->buffer_size;

    /* Inform ALSA that a period has been processed if needed */
    if (runtime->status->state == SNDRV_PCM_STATE_RUNNING && avail_frames >= runtime->period_size) {
        pr_debug("dma-alsa: Period elapsed for hw_ptr=%lu\n", runtime->status->hw_ptr);
        snd_pcm_period_elapsed_under_stream_lock(substream);
    }

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
    unsigned int requested_buffer_size = params_buffer_bytes(params);
    unsigned int requested_period_size = params_period_bytes(params);

    if (!runtime) {
        pr_err("dma-alsa: runtime is NULL\n");
        return -EINVAL;
    }

    // Validation of format, sample rate and channels
    if (params_format(params) != SNDRV_PCM_FORMAT_S24_3LE) {
        pr_err("dma-alsa: unsupported format requested: %d\n", params_format(params));
        return -EINVAL;
    }

    if (params_rate(params) != 48000) {
        pr_err("dma-alsa: unsupported sample rate requested: %u\n", params_rate(params));
        return -EINVAL;
    }

    if (params_channels(params) != 2) {
        pr_err("dma-alsa: unsupported number of channels: %u\n", params_channels(params));
        return -EINVAL;
    }

    pr_info("dma-alsa: runtime->frame_bits=%d, requested_buffer_size=%u, requested_period_size=%u\n",
        runtime->frame_bits, requested_buffer_size, requested_period_size);

    // Validation of buffer_size and period_size
    if (requested_buffer_size > AUDIO_BUFFER_SIZE) {
        pr_warn("dma-alsa: requested buffer_size too large, adjusting to %u\n", AUDIO_BUFFER_SIZE);
        requested_buffer_size = AUDIO_BUFFER_SIZE;
    }

    if (requested_period_size < dma_pcm_hardware.period_bytes_min ||
        requested_period_size > dma_pcm_hardware.period_bytes_max) {
        pr_warn("dma-alsa: requested period_size out of bounds, adjusting to %zu\n",
                dma_pcm_hardware.period_bytes_min);
        requested_period_size = dma_pcm_hardware.period_bytes_min;
    }

    if (requested_buffer_size < (requested_period_size * dma_pcm_hardware.periods_min)) {
        pr_err("dma-alsa: buffer_size too small for requested period_size\n");
        return -EINVAL;
    }

    // Allocation of the ALSA-buffer
    if (snd_pcm_lib_malloc_pages(substream, requested_buffer_size) < 0) {
        pr_err("dma-alsa: Failed to allocate ALSA buffer\n");
        return -ENOMEM;
    }

    // Dynamische configuratie van runtime buffer- en periodinstellingen
    //runtime->dma_bytes = requested_buffer_size;
    runtime->frame_bits = params_channels(params) * snd_pcm_format_physical_width(params_format(params));
    runtime->period_size = bytes_to_frames(runtime, requested_period_size);
    runtime->buffer_size = bytes_to_frames(runtime, requested_buffer_size);
    runtime->start_threshold = runtime->buffer_size / 2;
    runtime->control->avail_min = runtime->period_size;

    pr_info("dma-alsa: hw_params configured, buffer_size=%zu frames, period_size=%zu frames, address=%p\n",
            runtime->buffer_size, runtime->period_size, runtime->dma_area);

    return 0;
}

/* PCM pointer callback */
static snd_pcm_uframes_t dma_pcm_pointer(struct snd_pcm_substream *substream)
{
    /*
    This callback is executed whenever the application wants to know where the hardware pointer
    is currently located in the ALSA ringbuffer.
    */

    struct snd_pcm_runtime *runtime = substream->runtime;
    snd_pcm_uframes_t hw_ptr;

    // Get the current hw_ptr safely
    hw_ptr = runtime->status->hw_ptr;

    pr_info("dma-alsa: Returning hw_ptr=%lu frames\n", hw_ptr);

    return hw_ptr;
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
    if (!runtime || !active_dma_buffer || !next_dma_buffer) {
        pr_err("dma-alsa: hw free failed, invalid runtime or buffer\n");
        return -EINVAL;
    }

    // Reset buffer management
    buffer_fill_level = 0;

    // Free ALSA buffer
    snd_pcm_lib_free_pages(substream);
    
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
    if (!runtime || !next_dma_buffer || !active_dma_buffer) {
        pr_err("dma-alsa: prepare failed, invalid runtime or buffer\n");
        return -EINVAL;
    }

    if (runtime->buffer_size == 0 || runtime->period_size == 0) {
        pr_err("dma-alsa: Invalid buffer_size or period_size in prepare\n");
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

/* PCM trigger callback */
static int dma_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    /*
    This callback is executed when a certain trigger is sent to the pcm stream: start, stop, pause
        The received trigger is determined (start / stop)
        Action is taken accordingly to the trigger type
    */
    struct snd_pcm_runtime *runtime = substream->runtime;

    pr_debug("dma-alsa: Current ALSA state: %d\n", runtime->status->state);

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
        The ALSA buffer is released
        The 2 allocated DMA buffers are released
        The DMA channel is released
        The sound card is unregistered from the system
    */

    pr_info("dma-alsa: module cleanup started\n");

    if (active_dma_buffer) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, active_dma_buffer, active_dma_handle);
        dma_buffer1 = NULL;
        active_dma_buffer = NULL;
        pr_info("dma-alsa: active dma buffer released\n");
    } else {
        pr_warn("dma-alsa: active dma buffer already released\n");
    }

    if (next_dma_buffer) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, next_dma_buffer, next_dma_handle);
        dma_buffer2 = NULL;
        next_dma_buffer = NULL;
        pr_info("dma-alsa: next dma buffer released\n");
    } else {
        pr_warn("dma-alsa: next dma buffer already released\n");
    }

    if (dma_channel) {
        dma_release_channel(dma_channel);
        dma_channel = NULL;
        pr_info("dma-alsa: dma channel released\n");
    }

    if (card) {
        snd_card_free(card);
        card = NULL;
    }

    pr_info("dma-alsa: module removed\n");
}

module_init(dma_pcm_init);
module_exit(dma_pcm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lander Van Loock");
MODULE_DESCRIPTION("DMA ALSA PCM kernel module with AXI DMA via DMAengine");