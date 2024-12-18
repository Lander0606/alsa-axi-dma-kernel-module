/*
 * DMA-based ALSA PCM Module to create an audio card to AXI DMA
 *
 * This driver provides a basic ALSA PCM device that uses a DMA channel
 * to transfer audio samples from kernel-allocated buffers to a target device.
 * It sets up a single sound card and a single PCM playback device, where audio
 * data is pulled from the ALSA buffer and converted into a 64-bit word format
 * per frame before being sent out through the DMA engine.
 *
 * Supported Audio Format:
 * - Signed 24-bit samples, packed in 3 bytes per sample (S24_3LE)
 * - Signed 24-bit samples in 4-byte containers (S24_LE), where the fourth byte
 *   is ignored to maintain the same 64-bit frame structure.
 *
 * Supported Configuration:
 * - Stereo (2-channel) only
 * - 48 kHz sample rate
 * - Hardware parameters such as period size and buffer size are restricted to
 *   specific ranges to ensure stable operation.
 *
 * Operation:
 * - The driver allocates a continuous DMA buffer and uses DMA transfers to 
 *   feed samples to the hardware. When a period completes, a DMA completion 
 *   callback triggers a workqueue job to safely interact with ALSA APIs, 
 *   mark the period as elapsed, and start transferring the next period.
 * - The PCM operations (open, close, hw_params, prepare, trigger, etc.) are 
 *   implemented to interact seamlessly with ALSA applications, ensuring that 
 *   streams can be started, stopped, paused, or resumed without glitches.
 *
 * Limitations:
 * - This is a simplified reference driver and may need adjustments for 
 *   hardware-specific DMA handling, proper error handling, or additional 
 *   sample formats and rates.
 *
 * Author: Lander Van Loock
 * License: GPL
 * 
 * Latest edit: 18/12/2024 - 16:53
 *
 */


#include <linux/module.h>
#include <linux/init.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define PCM_DEVICE_NAME "dma_pcm"
#define CARD_NAME "DMA Audio Card"
#define AUDIO_BUFFER_SIZE (64 * 1024)   // 64 KB audio buffer

static struct snd_card *card;
static struct snd_pcm *pcm;
static struct dma_chan *dma_channel;                    // A DMA Channel
static void *dma_buffer;                                // Single DMA buffer
static dma_addr_t dma_handle;                           // Physical address for the dma buffer
static struct mutex dma_lock;                           // Mutex for non-atomic contexts
static struct snd_pcm_substream *g_substream = NULL;
static size_t buffer_fill_level = 0;
static snd_pcm_uframes_t driver_hw_ptr = 0;

// Declare a workqueue and a work struct to handle DMA completion outside interrupt context
static struct work_struct dma_work;
static bool work_pending = false;

/* ALSA PCM hardware parameters */
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
    .periods_max = 4,
};

/* Forward declaration of write_to_buffer so we can call it from work handler */
static void write_to_buffer(struct snd_pcm_substream *substream);

/* Workqueue handler - runs in process context, can use mutexes and call ALSA functions safely */
static void dma_work_handler(struct work_struct *work)
{
    struct snd_pcm_runtime *runtime;
    snd_pcm_sframes_t available_frames;

    if (!g_substream)
        return;

    runtime = g_substream->runtime;

    // *** CHANGED: This logic was previously in the dma_transfer_callback interrupt handler.
    // Now we are in a safe context to call ALSA functions, hold mutexes, etc.

    // First, inform ALSA that a period has elapsed
    driver_hw_ptr = (driver_hw_ptr + runtime->period_size) % runtime->buffer_size;
    snd_pcm_period_elapsed(g_substream);

    // Check if there is another period available
    available_frames = runtime->control->appl_ptr - driver_hw_ptr;
    if (available_frames < 0)
        // Compensate for the circular alsa buffer
        available_frames += runtime->buffer_size;

    if (available_frames < (snd_pcm_sframes_t)runtime->period_size) {
        // Not enough data for next period -> UNDERRUN
        pr_info("dma-alsa: underrun detected in work handler\n");
        snd_pcm_stop(g_substream, SNDRV_PCM_STATE_XRUN);
        return;
    }

    // If we have enough data, load next period
    // This will copy data, zero-pad it, and start a new DMA transfer
    write_to_buffer(g_substream);

    work_pending = false; 
}

/* DMA completed callback */
static void dma_transfer_callback(void *completion)
{
    /*
    This callback is executed in interrupt (atomic) context. 
    We must not use mutex or ALSA calls that can sleep here.
    Instead, we schedule our workqueue task.
    */

    // Minimal work here: just schedule the work
    pr_info("dma-alsa: dma transfer completed (interrupt handler), scheduling work\n");

    // To avoid queueing multiple works before handling, check a flag
    if (!work_pending) {
        work_pending = true;
        schedule_work(&dma_work);
    }
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

/* Write audio from ALSA buffer to dma_buffer */
// This function can now safely use mutex and ALSA functions since we'll call it from non-atomic context (work handler or trigger start)
static void write_to_buffer(struct snd_pcm_substream *substream)
{
    /*
    This function is executed in the .ack callback to add and zero pad the new data to the DMA buffer
        The received data from the ALSA buffer is zero padded and combined to an L/R sample in 1 word (64 bit or 8 bytes)
        If the current dma_buffer is full, the dma transfer is started and a new buffer is assigned
    */
    if(substream == NULL) {
        pr_err("dma-alsa: substream NULL in write");
        return;
    }
    struct snd_pcm_runtime *runtime = substream->runtime;
    snd_pcm_sframes_t available_frames;
    void *src;
    size_t i;
    uint8_t *src_byte;
    uint64_t *dst;
    int processed = 0;
    size_t period_bytes = frames_to_bytes(runtime, runtime->period_size);

    if(runtime == NULL) {
        pr_err("dma-alsa: runtime NULL in write");
        return;
    }

    if(dma_buffer == NULL) {
        pr_err("dma-alsa: dma buffer NULL in write");
        return;
    }

    mutex_lock(&dma_lock);

    available_frames = runtime->control->appl_ptr - driver_hw_ptr;
    if (available_frames < 0)
        available_frames += runtime->buffer_size;

    if (available_frames < (snd_pcm_sframes_t)runtime->period_size) {
        pr_info("dma-alsa: underrun detected in write_to_buffer\n");
        snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
        mutex_unlock(&dma_lock);
        return;
    }

    int sample_size;
    if (runtime->format == SNDRV_PCM_FORMAT_S24_3LE) {
        sample_size = 3;
    } else if (runtime->format == SNDRV_PCM_FORMAT_S24_LE) {
        sample_size = 4;
    } else {
        pr_err("dma-alsa: unsupported runtime format in write_to_buffer\n");
        mutex_unlock(&dma_lock);
        return;
    }

    int frame_bytes = sample_size * runtime->channels;


    src = runtime->dma_area + frames_to_bytes(runtime, driver_hw_ptr);

    dst = (uint64_t *)dma_buffer;
    src_byte = (uint8_t *)src;
    buffer_fill_level = 0;

    for (i = 0; i < period_bytes; i += frame_bytes) {
        uint64_t word = 0;

        if (sample_size == 3) {
            for (int j = 0; j < 6; j++) {
                word |= ((uint64_t)src_byte[processed+j]) << (8 * (5 - j));
            }
            word <<= 16;
            processed += 6;
        } else {
            word |= ((uint64_t)src_byte[processed+0]) << 40; // L1
            word |= ((uint64_t)src_byte[processed+1]) << 32; // L2
            word |= ((uint64_t)src_byte[processed+2]) << 24; // L3
            word |= ((uint64_t)src_byte[processed+4]) << 16; // R1
            word |= ((uint64_t)src_byte[processed+5]) << 8;  // R2
            word |= ((uint64_t)src_byte[processed+6]);       // R3

            word <<= 16;
            processed += 8;
        }

        *dst = word;
        dst++;
        buffer_fill_level += 8; 
    }

    if (start_dma_transfer(dma_buffer, buffer_fill_level, dma_handle)) {
        pr_err("dma-alsa: failed to start DMA for period in write_to_buffer\n");
        // If this fails, we can stop the stream
        snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
    }

    mutex_unlock(&dma_lock);
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

    dma_buffer = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &dma_handle, GFP_KERNEL);
    if (!dma_buffer) {
        pr_err("dma-alsa: could not allocate dma_buffer\n");
        snd_pcm_lib_free_pages(substream);
        return -ENOMEM;
    }

    g_substream = substream;
    driver_hw_ptr = 0;

    pr_info("dma-alsa: PCM opened, DMA buffer allocated at %p\n", dma_buffer);
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

    snd_pcm_lib_free_pages(substream);

    if (dma_buffer) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        dma_buffer = NULL;
    }

    mutex_unlock(&dma_lock);

    g_substream = NULL;

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

    if (params_format(params) != SNDRV_PCM_FORMAT_S24_3LE && params_format(params) != SNDRV_PCM_FORMAT_S24_LE) {
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

    if (snd_pcm_lib_malloc_pages(substream, requested_buffer_size) < 0) {
        pr_err("dma-alsa: Failed to allocate ALSA buffer\n");
        return -ENOMEM;
    }

    runtime->frame_bits = params_channels(params) * snd_pcm_format_physical_width(params_format(params));
    runtime->period_size = bytes_to_frames(runtime, requested_period_size);
    runtime->buffer_size = bytes_to_frames(runtime, requested_buffer_size);

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

    snd_pcm_uframes_t hw_ptr = driver_hw_ptr;

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

    if (!runtime || !dma_buffer) {
        pr_err("dma-alsa: hw free failed, invalid runtime or buffer\n");
        return -EINVAL;
    }

    snd_pcm_lib_free_pages(substream);
    
    pr_info("dma-alsa: hw free successful\n");
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

    if (!runtime || !dma_buffer) {
        pr_err("dma-alsa: prepare failed, invalid runtime or buffer\n");
        return -EINVAL;
    }

    if (runtime->buffer_size == 0 || runtime->period_size == 0) {
        pr_err("dma-alsa: Invalid buffer_size or period_size in prepare\n");
        return -EINVAL;
    }

    pr_info("dma-alsa: preparing hw, resetting DMA and buffers\n");
    buffer_fill_level = 0;
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
        // Load first period and start DMA
        write_to_buffer(substream);
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

    snd_pcm_lib_preallocate_pages_for_all(pcm,
                                      SNDRV_DMA_TYPE_CONTINUOUS,
                                      NULL,
                                      AUDIO_BUFFER_SIZE,
                                      AUDIO_BUFFER_SIZE);

    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &dma_pcm_ops);

    err = snd_card_register(card);
    if (err < 0) {
        snd_card_free(card);
        return err;
    }

    // Initialize the work struct
    INIT_WORK(&dma_work, dma_work_handler);

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

    // Flush any pending work
    flush_work(&dma_work);

    if (dma_buffer) {
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        dma_buffer = NULL;
        pr_info("dma-alsa: dma buffer released\n");
    } else {
        pr_warn("dma-alsa: dma buffer already released\n");
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