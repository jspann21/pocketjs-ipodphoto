#include "audio.h"

#include <stdbool.h>
#include <stdint.h>

#include "pp5020.h"
#include "timer.h"

/* The iPod Photo/Color uses a WM8975 at the standard 7-bit address 0x1a.
 * WM8975 register writes carry the register number shifted left one bit and
 * the ninth data bit in the low address bit. */
#define WM8975_I2C_ADDRESS 0x1au

#define AUDIO_I2C_SEND     0x80u
#define AUDIO_I2C_READ     0x20u
#define AUDIO_I2C_BUSY     0x40u
#define AUDIO_I2C_TIMEOUT_US 5000u
#define AUDIO_I2C_ATTEMPTS 3u

#define WM8975_RESET       0x0fu
#define WM8975_DAPCTRL     0x05u
#define WM8975_AINTFCE     0x07u
#define WM8975_SAMPCTRL    0x08u
#define WM8975_LOUT1VOL    0x02u
#define WM8975_ROUT1VOL    0x03u
#define WM8975_PWRMGMT1    0x19u
#define WM8975_PWRMGMT2    0x1au
#define WM8975_LOUTMIX1    0x22u
#define WM8975_LOUTMIX2    0x23u
#define WM8975_ROUTMIX1    0x24u
#define WM8975_ROUTMIX2    0x25u

#define WM8975_DAC_MUTE    0x0008u
#define WM8975_I2S_MASTER  0x0040u
#define WM8975_I2S_16BIT  0x0000u
#define WM8975_I2S_FORMAT 0x0002u
#define WM8975_44100HZ    0x0063u
#define WM8975_VREF       0x0040u
#define WM8975_VMID_5K    0x0180u
#define WM8975_VMID_50K   0x0080u
#define WM8975_DAC_OUTPUTS 0x01e0u
#define WM8975_LDAC_TO_HP 0x0100u
#define WM8975_RDAC_TO_HP 0x0100u

/* 0x51 is the WM8975 -40 dB output code.  The test samples are only 6.25%
 * full-scale as a second independent guard against an unsafe output level. */
#define WM8975_HEADPHONE_VOLUME 0x0051u

#define AUDIO_CHANNEL_FRAMES 11025u
#define AUDIO_GAP_FRAMES 4410u
#define AUDIO_TONE_FRAMES (2u * AUDIO_CHANNEL_FRAMES + AUDIO_GAP_FRAMES)
#define AUDIO_TONE_TIMEOUT_US 900000u
#define AUDIO_STOP_TIMEOUT_US 20000u
#define AUDIO_FIFO_PREFILL_FRAMES 8u
#define AUDIO_ENVELOPE_FRAMES 128u
#define AUDIO_PHASE_STEP 20924u /* 440 Hz at 44.1 kHz, Q16 table phase */

static const int16_t tone_table[32] = {
    0, 399, 785, 1138, 1448, 1703, 1892, 2009,
    2048, 2009, 1892, 1703, 1448, 1138, 785, 399,
    0, -399, -785, -1138, -1448, -1703, -1892, -2009,
    -2048, -2009, -1892, -1703, -1448, -1138, -785, -399,
};

static uint8_t audio_error_code(int result)
{
    int magnitude = result < 0 ? -result : result;
    return (uint8_t)(magnitude > 255 ? 255 : magnitude);
}

static int audio_fail(PjsAudioState *audio, int result)
{
    if (audio != 0) {
        audio->state = PJS_AUDIO_FAULT;
        audio->last_error = audio_error_code(result);
    }
    return result;
}

static bool audio_i2c_idle(void)
{
    uint32_t started = timer_now_us();
    while ((PP_I2C_STATUS & AUDIO_I2C_BUSY) != 0u) {
        if ((uint32_t)(timer_now_us() - started) >= AUDIO_I2C_TIMEOUT_US) {
            return false;
        }
    }
    return true;
}

static void audio_i2c_recover(PjsAudioState *audio)
{
    PP_DEV_EN |= PP_DEV_I2C;
    PP_DEV_RS |= PP_DEV_I2C;
    pp_nop3();
    PP_DEV_RS &= ~PP_DEV_I2C;
    PP_I2C_CLOCK = 0u;
    PP_I2C_CLOCK = 0x80u;
    if (audio != 0 && audio->i2c_recoveries != UINT32_MAX) {
        ++audio->i2c_recoveries;
    }
}

static void audio_i2c_prepare(PjsAudioState *audio)
{
    PP_DEV_EN |= PP_DEV_I2C;
    if ((PP_I2C_STATUS & AUDIO_I2C_BUSY) != 0u) {
        audio_i2c_recover(audio);
    }
    PP_I2C_CLOCK = 0u;
    PP_I2C_CLOCK = 0x80u;
}

static bool audio_i2c_write(PjsAudioState *audio, uint8_t reg,
                            uint16_t value)
{
    uint8_t address_byte = (uint8_t)((reg << 1) | ((value >> 8) & 1u));
    uint8_t data_byte = (uint8_t)(value & 0xffu);
    for (uint32_t attempt = 0u; attempt < AUDIO_I2C_ATTEMPTS; ++attempt) {
        if (!audio_i2c_idle()) {
            audio_i2c_recover(audio);
            continue;
        }
        PP_I2C_ADDR = (uint8_t)(WM8975_I2C_ADDRESS << 1);
        PP_I2C_CTRL &= (uint8_t)~AUDIO_I2C_READ;
        PP_I2C_DATA(0u) = address_byte;
        PP_I2C_DATA(1u) = data_byte;
        PP_I2C_CTRL = (uint8_t)((PP_I2C_CTRL & (uint8_t)~0x06u) | 0x02u);
        PP_I2C_CTRL |= AUDIO_I2C_SEND;
        if (audio_i2c_idle()) {
            if (audio != 0 && audio->codec_writes != UINT32_MAX) {
                ++audio->codec_writes;
            }
            return true;
        }
        audio_i2c_recover(audio);
    }
    return false;
}

static void audio_i2s_reset(void)
{
    /* This is the PP502x/WM8975 sequence used by Rockbox. The codec owns the
     * bit clock; setting PP_IIS_MASTER here would select the wrong direction. */
    PP_DEV_INIT2 &= ~0x00000300u;
    PP_DEV_INIT1 &= ~0x03000000u;
    PP_DEV_RS |= PP_DEV_I2S;
    pp_nop3();
    PP_DEV_RS &= ~PP_DEV_I2S;
    PP_DEV_EN |= PP_DEV_I2S | PP_DEV_EXTCLOCKS;
    PP_AUDIO_CLOCK_CTRL &= ~0x0000000cu;

    PP_IISCONFIG |= PP_IIS_RESET;
    PP_IISCONFIG &= ~PP_IIS_RESET;
    PP_IISCONFIG &= ~(PP_IIS_TXFIFOEN | PP_IIS_RXFIFOEN |
                      PP_IIS_MASTER | PP_IIS_IRQTX | PP_IIS_IRQRX |
                      PP_IIS_FORMAT_MASK | PP_IIS_SIZE_MASK |
                      PP_IIS_FIFO_FORMAT_MASK);
    PP_IISCONFIG |= PP_IIS_FORMAT_IIS | PP_IIS_SIZE_16BIT |
                    PP_IIS_FIFO_FORMAT_LE16_2;
    PP_IISFIFO_CFG |= PP_IIS_RX_FULL_LVL_12 | PP_IIS_TX_EMPTY_LVL_4;
    PP_IISFIFO_CFG |= PP_IIS_RXCLR | PP_IIS_TXCLR;

    /* Rockbox's audio-pp path selects the headphone/output pin mux this way.
     * Use the PP502x atomic aliases so no unrelated GPIO bits are changed. */
    pp_gpio_set(&PP_GPIOI_OUTPUT_VAL_ATOMIC, 0x40u);
    pp_gpio_set(&PP_GPIOA_OUTPUT_VAL_ATOMIC, 0x04u);
}

static void audio_i2s_close(void)
{
    PP_IISCONFIG &= ~PP_IIS_TXFIFOEN;
    PP_IISFIFO_CFG |= PP_IIS_TXCLR;
    /* Do not gate the shared EXTCLOCKS domain without proving that no other
     * device needs it. Only the I2S device itself is reset and gated here. */
    PP_DEV_RS |= PP_DEV_I2S;
    pp_nop3();
    PP_DEV_RS &= ~PP_DEV_I2S;
    PP_DEV_EN &= ~PP_DEV_I2S;
}

static bool audio_codec_write(PjsAudioState *audio, uint8_t reg,
                              uint16_t value)
{
    return audio_i2c_write(audio, reg, value);
}

static bool audio_codec_open(PjsAudioState *audio)
{
    if (!audio_codec_write(audio, WM8975_RESET, 0u)) return false;
    /* Program the low headphone level while output buffers are still off.
     * Do not expose the reset-default volume during analog power-up. */
    if (!audio_codec_write(audio, WM8975_DAPCTRL, WM8975_DAC_MUTE) ||
        !audio_codec_write(audio, WM8975_LOUT1VOL,
                           0x0080u | WM8975_HEADPHONE_VOLUME) ||
        !audio_codec_write(audio, WM8975_ROUT1VOL,
                           0x0180u | WM8975_HEADPHONE_VOLUME)) return false;
    if (!audio_codec_write(audio, WM8975_PWRMGMT1,
                           WM8975_VMID_5K | WM8975_VREF)) {
        return false;
    }
    timer_delay_us(20000u);
    if (!audio_codec_write(audio, WM8975_PWRMGMT1,
                           WM8975_VMID_50K | WM8975_VREF) ||
        !audio_codec_write(audio, WM8975_PWRMGMT2, WM8975_DAC_OUTPUTS) ||
        !audio_codec_write(audio, WM8975_AINTFCE,
                           WM8975_I2S_MASTER | WM8975_I2S_16BIT |
                           WM8975_I2S_FORMAT) ||
        !audio_codec_write(audio, WM8975_DAPCTRL, WM8975_DAC_MUTE) ||
        !audio_codec_write(audio, WM8975_SAMPCTRL, WM8975_44100HZ) ||
        !audio_codec_write(audio, WM8975_LOUT1VOL,
                           0x0080u | WM8975_HEADPHONE_VOLUME) ||
        !audio_codec_write(audio, WM8975_ROUT1VOL,
                           0x0100u | 0x0080u | WM8975_HEADPHONE_VOLUME) ||
        !audio_codec_write(audio, WM8975_LOUTMIX1, WM8975_LDAC_TO_HP) ||
        !audio_codec_write(audio, WM8975_LOUTMIX2, 0u) ||
        !audio_codec_write(audio, WM8975_ROUTMIX1, 0u) ||
        !audio_codec_write(audio, WM8975_ROUTMIX2, WM8975_RDAC_TO_HP)) {
        return false;
    }
    return true;
}

static bool audio_fifo_wait_slot(uint32_t deadline,
                                 PjsAudioState *audio)
{
    /* A PP502x FIFO entry is a packed stereo word. Waiting for two free
     * entries leaves a safety margin around the status read/write boundary. */
    while (PP_IIS_TX_FREE_COUNT < 2u) {
        if ((int32_t)(timer_now_us() - deadline) >= 0) {
            if (audio != 0 && audio->fifo_timeouts != UINT32_MAX) {
                ++audio->fifo_timeouts;
            }
            return false;
        }
    }
    return true;
}

static bool audio_fifo_push(int16_t left, int16_t right, uint32_t deadline,
                            PjsAudioState *audio)
{
    if (!audio_fifo_wait_slot(deadline, audio)) return false;
    uint32_t packed = (uint32_t)(uint16_t)left |
                      ((uint32_t)(uint16_t)right << 16);
    PP_IISFIFO_WR = packed;
    return true;
}

static int audio_codec_close(PjsAudioState *audio)
{
    int result = PJS_AUDIO_RESULT_OK;
    if (!audio_codec_write(audio, WM8975_DAPCTRL, WM8975_DAC_MUTE)) {
        result = PJS_AUDIO_RESULT_I2C;
    }

    uint32_t deadline = timer_now_us() + AUDIO_STOP_TIMEOUT_US;
    while (!PP_IIS_TX_IS_EMPTY) {
        if ((int32_t)(timer_now_us() - deadline) >= 0) {
            if (audio != 0 && audio->fifo_timeouts != UINT32_MAX) {
                ++audio->fifo_timeouts;
            }
            if (result == PJS_AUDIO_RESULT_OK) result = PJS_AUDIO_RESULT_FIFO;
            break;
        }
    }
    PP_IISCONFIG &= ~PP_IIS_TXFIFOEN;
    PP_IISFIFO_CFG |= PP_IIS_TXCLR;

    if (!audio_codec_write(audio, WM8975_PWRMGMT2, 0u) &&
        result == PJS_AUDIO_RESULT_OK) {
        result = PJS_AUDIO_RESULT_I2C;
    }
    if (!audio_codec_write(audio, WM8975_PWRMGMT1, 0u) &&
        result == PJS_AUDIO_RESULT_OK) {
        result = PJS_AUDIO_RESULT_I2C;
    }
    audio_i2s_close();
    return result;
}

void pjs_audio_state_init(PjsAudioState *audio)
{
    if (audio != 0) *audio = (PjsAudioState){0};
}

int pjs_audio_init(PjsAudioState *audio)
{
    if (audio == 0) return PJS_AUDIO_RESULT_ARGUMENT;
    if (audio->state == PJS_AUDIO_READY) return PJS_AUDIO_RESULT_OK;
    uint32_t codec_writes = audio->codec_writes;
    uint32_t i2c_recoveries = audio->i2c_recoveries;
    uint32_t fifo_timeouts = audio->fifo_timeouts;
    uint32_t tones = audio->tones;
    *audio = (PjsAudioState){
        .codec_writes = codec_writes,
        .i2c_recoveries = i2c_recoveries,
        .fifo_timeouts = fifo_timeouts,
        .tones = tones,
    };
    audio_i2c_prepare(audio);
    audio_i2s_reset();
    if (!audio_codec_open(audio)) {
        /* The codec may have accepted a prefix of the register sequence. Run
         * the same bounded mute/power-down path before exposing the fault. */
        (void)audio_codec_close(audio);
        return audio_fail(audio, PJS_AUDIO_RESULT_I2C);
    }
    audio->state = PJS_AUDIO_READY;
    audio->last_error = 0u;
    return PJS_AUDIO_RESULT_OK;
}

int pjs_audio_stop(PjsAudioState *audio)
{
    if (audio == 0) return PJS_AUDIO_RESULT_ARGUMENT;
    if (audio->state == PJS_AUDIO_OFF) return PJS_AUDIO_RESULT_OK;
    int result = audio_codec_close(audio);
    if (result == PJS_AUDIO_RESULT_OK) {
        audio->state = PJS_AUDIO_OFF;
        audio->last_error = 0u;
    } else {
        audio_fail(audio, result);
    }
    return result;
}

int pjs_audio_resume(PjsAudioState *audio)
{
    return pjs_audio_init(audio);
}

int pjs_audio_pcm_prepare(PjsAudioState *audio)
{
    int result = pjs_audio_init(audio);
    if (result != PJS_AUDIO_RESULT_OK) return result;
    if (!audio_codec_write(audio, WM8975_DAPCTRL, WM8975_DAC_MUTE)) {
        (void)pjs_audio_stop(audio);
        return audio_fail(audio, PJS_AUDIO_RESULT_I2C);
    }
    /* Allow the analog reference to settle while muted. DMA then supplies a
     * separate silence lead-in before unmute, so no live FIFO feed is paused
     * by codec control traffic. This is a mitigation, not a no-pop guarantee. */
    timer_delay_us(80000u);
    return PJS_AUDIO_RESULT_OK;
}

int pjs_audio_pcm_mute(PjsAudioState *audio, bool muted)
{
    if (audio == 0) return PJS_AUDIO_RESULT_ARGUMENT;
    if (audio->state != PJS_AUDIO_READY) return PJS_AUDIO_RESULT_NOT_READY;
    if (!audio_codec_write(audio, WM8975_DAPCTRL,
                           muted ? WM8975_DAC_MUTE : 0u)) {
        return audio_fail(audio, PJS_AUDIO_RESULT_I2C);
    }
    return PJS_AUDIO_RESULT_OK;
}

int pjs_audio_tone(PjsAudioState *audio)
{
    if (audio == 0) return PJS_AUDIO_RESULT_ARGUMENT;
    if (audio->state != PJS_AUDIO_READY) {
        int init_result = pjs_audio_init(audio);
        if (init_result != PJS_AUDIO_RESULT_OK) return init_result;
    }

    audio->state = PJS_AUDIO_TONE;
    PP_IISCONFIG &= ~PP_IIS_TXFIFOEN;
    PP_IISFIFO_CFG |= PP_IIS_TXCLR;
    uint32_t deadline = timer_now_us() + AUDIO_TONE_TIMEOUT_US;
    for (uint32_t index = 0u; index < AUDIO_FIFO_PREFILL_FRAMES; ++index) {
        if (!audio_fifo_push(0, 0, deadline, audio)) {
            (void)audio_codec_close(audio);
            return audio_fail(audio, PJS_AUDIO_RESULT_FIFO);
        }
    }
    /* Unmute over a prefilled silent FIFO, before clocking the tone. No I2C
     * operation interrupts the PCM feed after transmission starts. */
    if (!audio_codec_write(audio, WM8975_DAPCTRL, 0u)) {
        (void)audio_codec_close(audio);
        return audio_fail(audio, PJS_AUDIO_RESULT_I2C);
    }
    uint32_t started = timer_now_us();
    PP_IISCONFIG |= PP_IIS_TXFIFOEN;
    uint32_t phase = 0u;
    for (uint32_t frame = 0u; frame < AUDIO_TONE_FRAMES; ++frame) {
        bool left_channel = frame < AUDIO_CHANNEL_FRAMES;
        bool right_channel = frame >= AUDIO_CHANNEL_FRAMES + AUDIO_GAP_FRAMES;
        uint32_t channel_frame = right_channel ?
            frame - AUDIO_CHANNEL_FRAMES - AUDIO_GAP_FRAMES : frame;
        int32_t sample = tone_table[(phase >> 16) & 31u];
        uint32_t gain = 128u;
        if (!left_channel && !right_channel) {
            gain = 0u;
        } else if (channel_frame < AUDIO_ENVELOPE_FRAMES) {
            gain = channel_frame;
        } else if (AUDIO_CHANNEL_FRAMES - channel_frame <= AUDIO_ENVELOPE_FRAMES) {
            gain = AUDIO_CHANNEL_FRAMES - channel_frame;
        }
        sample = (sample * (int32_t)gain) / 128;
        if (!audio_fifo_push(left_channel ? (int16_t)sample : 0,
                             right_channel ? (int16_t)sample : 0,
                             deadline, audio)) {
            (void)audio_codec_close(audio);
            return audio_fail(audio, PJS_AUDIO_RESULT_FIFO);
        }
        phase += AUDIO_PHASE_STEP;
    }
    uint32_t elapsed = timer_now_us() - started;
    int result = audio_codec_close(audio);
    if (result != PJS_AUDIO_RESULT_OK) {
        audio_fail(audio, result);
        return result;
    }
    /* A FIFO that never backpressures, or the wrong inherited audio rate,
     * must not be reported as a successful 600 ms hardware-clocked tone. */
    if (elapsed < 540000u || elapsed > 660000u) {
        return audio_fail(audio, PJS_AUDIO_RESULT_CLOCK);
    }
    if (audio->tones != UINT32_MAX) ++audio->tones;
    audio->state = PJS_AUDIO_OFF;
    audio->last_error = 0u;
    return PJS_AUDIO_RESULT_OK;
}
