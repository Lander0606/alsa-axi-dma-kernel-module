#include <linux/module.h>
#include <linux/init.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <sound/core.h>
#include <sound/pcm.h>

#define PCM_DEVICE_NAME "dma_pcm"
#define CARD_NAME "DMA Audio Card"
#define AUDIO_BUFFER_SIZE (64 * 1024) // 64 KB audio buffer

static struct snd_card *card;
static struct snd_pcm *pcm;
static struct dma_chan *dma_channel; // A DMA Channel
static void *dma_buffer;             // Active buffer for PCM samples
static void *next_dma_buffer;        // Next buffer
static dma_addr_t dma_handle;        // Physical address for active DMA buffer
static dma_addr_t next_dma_handle;   // Physical address for next buffer
static struct mutex dma_lock;        // Mutex to protect DMA and buffer changes
static size_t buffer_fill_level = 0; // Count for the amount of data in a buffer

/* ALSA PCM hardware parameters */
static struct snd_pcm_hardware cma_pcm_hardware = {
    .info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER, // Interleaved format (samples L/R on same place in memory) + supports block transfers
    .formats = SNDRV_PCM_FMTBIT_S24_LE, // 24-bit little-endian samples
    .rates = SNDRV_PCM_RATE_48000, // Support only 48kHz sample frequency
    .rate_min = 48000,
    .rate_max = 48000,
    .channels_min = 2, // Stereo (L/R)
    .channels_max = 2,
    .buffer_bytes_max = AUDIO_BUFFER_SIZE, // Total buffer size = 64 KB
    .period_bytes_min = 4096, // Min size of a "period" in the buffer -> 4096 / (6 bytes per frame) = minimal 682 frames per period
    .period_bytes_max = 16384, // Max size of a "period" in the buffer -> 16384 / (6 bytes per frame) = minimal 2730 frames per period
    .periods_min = 2, // The buffer needs to contain at least 2 periods
    .periods_max = 4, // The buffer can contain at most 4 periods
};

/* DMA completed callback */
static void dma_transfer_callback(void *completion)
{
    void *completed_buffer = completion;
    dma_addr_t phys_addr = virt_to_phys(completed_buffer);

    /* Free the transferred DMA buffer */
    dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, completed_buffer, phys_addr);

    pr_info("dma-alsa: dma transfer completed, buffer released at %p\n", completed_buffer);
}

/* Initialize the DMA channel */
static int init_dma_channel(void)
{
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
    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;

    /* Configure a descriptor for the transfer */
    desc = dmaengine_prep_slave_single(dma_channel, phys_addr, len, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
    if (!desc) {
        pr_err("dma-alsa: couldnt prepare the dma descriptor\n");
        return -EINVAL;
    }

    /* Configure the callback when completed */
    desc->callback = dma_transfer_callback;
    desc->callback_param = src;

    /* Start the transfer */
    cookie = dmaengine_submit(desc);
    if (dma_submit_error(cookie)) {
        pr_err("dma-alsa: dma transfer submission failed\n");
        return -EINVAL;
    }

    dma_async_issue_pending(dma_channel);

    pr_info("dma-alsa: dma transfer started for a buffer at %p, lenght: %zu bytes\n", src, len);
    return 0;
}

/* Write audio to the buffer */
static void write_to_buffer(void *data, size_t size)
{
    while (size > 0) {
        size_t space_left = AUDIO_BUFFER_SIZE - buffer_fill_level;
        size_t to_copy = min(space_left, size);

        memcpy((char *)dma_buffer + buffer_fill_level, data, to_copy);
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

            /* Change to a new empty buffer */
            dma_buffer = next_dma_buffer;
            dma_handle = next_dma_handle;

            /* Allocate a next empty buffer */
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

    runtime->dma_area = dma_buffer;
    runtime->dma_bytes = AUDIO_BUFFER_SIZE;

    pr_info("dma-alsa: pcm opened, buffer allocated at %p\n", dma_buffer);
    return 0;
}

/* PCM close callback */
static int dma_pcm_close(struct snd_pcm_substream *substream)
{
    mutex_lock(&dma_lock);

    if (dma_buffer) {
        pr_info("dma-alsa: released current buffer at %p\n", dma_buffer);
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        dma_buffer = NULL;
    }

    if (next_dma_buffer) {
        pr_info("dma-alsa: released next empty buffer at %p\n", next_dma_buffer);
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, next_dma_buffer, next_dma_handle);
        next_dma_buffer = NULL;
    }

    mutex_unlock(&dma_lock);
    return 0;
}

/* PCM trigger callback */
static int dma_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        pr_info("dma-alsa: playback started\n");
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        pr_info("dma-alsa: playback stopped\n");
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

/* PCM ACK callback */
static int dma_pcm_ack(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    snd_pcm_sframes_t frames = snd_pcm_lib_buffer_bytes(substream);

    /* Write data */
    void *data = runtime->dma_area; // Data of the user
    size_t size = frames * 6;       // 24-bit stereo, 6 bytes per frame

    write_to_buffer(data, size);

    return 0;
}

/* PCM hw_params callback */
static int dma_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
    return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
}

/* PCM operations */
static struct snd_pcm_ops dma_pcm_ops = {
    .open = dma_pcm_open,
    .close = dma_pcm_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = dma_pcm_hw_params,
    .hw_free = snd_pcm_lib_free_pages,
    .trigger = dma_pcm_trigger,
    .pointer = NULL,
    .ack = dma_pcm_ack,
};

/* Kernel module init */
static int __init dma_pcm_init(void)
{
    int err;

    mutex_init(&dma_lock);

    pr_info("dma-alsa: initialization of the module\n");

    err = init_dma_channel();
    if (err)
        return err;

    struct device *dev = dma_channel->device->dev;
    err = snd_card_new(dev, -1, NULL, THIS_MODULE, 0, &card);
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
    if (dma_channel) {
        dma_release_channel(dma_channel);
        pr_info("dma-alsa: dma channel released\n");
    }

    if (card)
        snd_card_free(card);

    pr_info("dma-alsa: module removed\n");

    // TODO: Also freeing of the dma allocated blocks
}

module_init(dma_pcm_init);
module_exit(dma_pcm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lander Van Loock");
MODULE_DESCRIPTION("DMA ALSA PCM kernel module with AXI DMA via DMAengine");