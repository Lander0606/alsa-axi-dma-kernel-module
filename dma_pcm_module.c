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
static struct dma_chan *dma_channel; // DMA-channel
static void *dma_buffer;             // Actieve buffer voor PCM
static void *next_dma_buffer;        // Volgende buffer
static dma_addr_t dma_handle;        // Fysiek DMA-adres voor de buffer
static dma_addr_t next_dma_handle;   // Fysiek adres voor de volgende buffer
static struct mutex dma_lock;        // Beschermt DMA en bufferwisselingen
static size_t buffer_fill_level = 0; // Houdt bij hoeveel data in de buffer zit

/* PCM hardware parameters */
static struct snd_pcm_hardware dma_pcm_hardware = {
    .info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
    .formats = SNDRV_PCM_FMTBIT_S24_LE, // 24-bit little-endian samples
    .rates = SNDRV_PCM_RATE_48000,
    .rate_min = 48000,
    .rate_max = 48000,
    .channels_min = 2, // Stereo (L/R)
    .channels_max = 2,
    .buffer_bytes_max = AUDIO_BUFFER_SIZE,
    .period_bytes_min = 4096,  // Minimaal 4 KB per periode
    .period_bytes_max = 16384, // Maximaal 16 KB per periode
    .periods_min = 2,          // Minimaal 2 periodes
    .periods_max = 4,          // Maximaal 4 periodes
};

/* DMA voltooiingscallback */
static void dma_transfer_callback(void *completion)
{
    void *completed_buffer = completion;
    dma_addr_t phys_addr = virt_to_phys(completed_buffer);

    /* Vrijgeven van de buffer */
    dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, completed_buffer, phys_addr);

    pr_info("DMA voltooid, buffer vrijgegeven op %p\n", completed_buffer);
}

/* Initialiseer DMA-channel */
static int init_dma_channel(void)
{
    dma_cap_mask_t mask;
    dma_cap_zero(mask);
 	dma_cap_set(DMA_SLAVE|DMA_PRIVATE, mask);
    dma_channel = dma_request_channel(mask, NULL, "dma0chan0");
    if (IS_ERR(dma_channel)) {
        pr_err("Kan DMA-channel niet verkrijgen\n");
        return PTR_ERR(dma_channel);
    }

    pr_info("DMA-channel verkregen: %s\n", dma_channel->device->dev->kobj.name);
    return 0;
}

/* Start DMA-transfer */
static int start_dma_transfer(void *src, size_t len, dma_addr_t phys_addr)
{
    struct dma_async_tx_descriptor *desc;
    dma_cookie_t cookie;

    /* Stel een descriptor in voor de transfer */
    desc = dmaengine_prep_slave_single(dma_channel, phys_addr, len, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
    if (!desc) {
        pr_err("Kan DMA-descriptor niet voorbereiden\n");
        return -EINVAL;
    }

    /* Stel een callback in */
    desc->callback = dma_transfer_callback;
    desc->callback_param = src;

    /* Start de transfer */
    cookie = dmaengine_submit(desc);
    if (dma_submit_error(cookie)) {
        pr_err("DMA-submissie mislukt\n");
        return -EINVAL;
    }

    dma_async_issue_pending(dma_channel);

    pr_info("DMA gestart voor buffer op %p, lengte: %zu bytes\n", src, len);
    return 0;
}

/* Schrijf audio naar de buffer */
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
            pr_info("Buffer vol, starten van DMA en wisselen van buffers\n");

            mutex_lock(&dma_lock);

            /* Start DMA-transfer met de huidige buffer */
            if (start_dma_transfer(dma_buffer, AUDIO_BUFFER_SIZE, dma_handle)) {
                pr_err("DMA-transfer mislukt\n");
            }

            /* Wissel naar nieuwe buffer */
            dma_buffer = next_dma_buffer;
            dma_handle = next_dma_handle;

            /* Allocate nieuwe buffer */
            next_dma_buffer = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &next_dma_handle, GFP_KERNEL);
            if (!next_dma_buffer) {
                pr_err("Kan nieuwe DMA-buffer niet toewijzen\n");
                mutex_unlock(&dma_lock);
                return;
            }
            pr_info("Nieuwe buffer toegewezen op %p\n", dma_buffer);

            /* Reset de vulstatus van de buffer */
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
        pr_err("Kan DMA-buffer niet toewijzen\n");
        return -ENOMEM;
    }

    next_dma_buffer = dma_alloc_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, &next_dma_handle, GFP_KERNEL);
    if (!next_dma_buffer) {
        pr_err("Kan volgende DMA-buffer niet toewijzen\n");
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        return -ENOMEM;
    }

    runtime->dma_area = dma_buffer;
    runtime->dma_bytes = AUDIO_BUFFER_SIZE;

    pr_info("PCM geopend, buffer toegewezen op %p\n", dma_buffer);
    return 0;
}

/* PCM close callback */
static int dma_pcm_close(struct snd_pcm_substream *substream)
{
    mutex_lock(&dma_lock);

    if (dma_buffer) {
        pr_info("Vrijgeven actieve buffer op %p\n", dma_buffer);
        dma_free_coherent(dma_channel->device->dev, AUDIO_BUFFER_SIZE, dma_buffer, dma_handle);
        dma_buffer = NULL;
    }

    if (next_dma_buffer) {
        pr_info("Vrijgeven volgende buffer op %p\n", next_dma_buffer);
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
        pr_info("Playback gestart\n");
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        pr_info("Playback gestopt\n");
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int dma_pcm_ack(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    snd_pcm_sframes_t frames = snd_pcm_lib_buffer_bytes(substream);

    /* Simuleer audio-data schrijven */
    void *data = runtime->dma_area; // Data van de gebruiker
    size_t size = frames * 6;       // 24-bit stereo, 6 bytes per frame

    write_to_buffer(data, size);

    return 0;
}

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
    .pointer = NULL, // Aanpassen indien nodig
    .ack = dma_pcm_ack,
};

/* Module initialisatie */
static int __init dma_pcm_init(void)
{
    int err;

    mutex_init(&dma_lock);

    pr_info("Initialiseren DMA ALSA-module\n");

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

    pr_info("DMA ALSA-module succesvol geinitialiseerd\n");
    return 0;
}

/* Module opruimen */
static void __exit dma_pcm_exit(void)
{
    if (dma_channel) {
        dma_release_channel(dma_channel);
        pr_info("DMA-channel vrijgegeven\n");
    }

    if (card)
        snd_card_free(card);

    pr_info("DMA ALSA-module verwijderd\n");

    // Also freeing of the dma allocated blocks
}

module_init(dma_pcm_init);
module_exit(dma_pcm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lander Van Loock");
MODULE_DESCRIPTION("DMA ALSA PCM Kernel Module met AXI DMA via DMAengine");