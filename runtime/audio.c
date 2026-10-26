/* The audio DSP, replaced at the level of the AX microcode's command list (what Dolphin calls AX
 * HLE). Each AX frame (__AXOutNewFrame, 96 samples at 32 kHz) mails the DSP a command list built by
 * __AXNextFrame (layout from the SDK's AXCL.c): SETUP, PROCESS <voice parameter blocks>, MIX_AUX*,
 * OUTPUT ... END. Voices are decoded (DSP ADPCM / PCM8 / PCM16), resampled, enveloped and mixed into
 * the main and aux buses; each voice's parameter block is written back like the DSP would, which the
 * game's sound engine depends on (playback positions, loops, end of sound). The final mix goes to
 * the host (SDL) and optionally a WAV file (RT_WAV=path). */
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "names.h"

#define HLE(name) void FN_##name(CPU *c)
#define N 96   /* samples per AX frame */
#define AX_OUT_DSP_READY 0x80796A80u   /* __AXOutDspReady (r13 + 2240) */

enum { BUS_L, BUS_R, BUS_S, BUS_AL, BUS_AR, BUS_AS, BUS_BL, BUS_BR, BUS_BS, BUS_CL, BUS_CR, BUS_CS, NBUS };
static s32 bus[NBUS][N];

static inline u32 pa(u32 a) { return a & 0x3FFFFFFFu; }   /* virtual or physical -> our physical alias */
static inline u16 r16(u32 a) { return mr16(pa(a)); }
static inline u32 r32(u32 a) { return mr32(pa(a)); }
static inline void w16(u32 a, u16 v) { mw16(pa(a), v); }
static inline s16 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }

/* ---------------------------------------------------------------- one voice */

typedef struct {
    u32 cur, end, loop;
    int loopflag, format, stream;
    s16 coef[16];
    u16 ps, loop_ps;
    s16 yn1, yn2, loop_yn1, loop_yn2;
    int stopped;
} Src;

static s16 next_sample(Src *s) {
    if (s->stopped) return 0;
    s16 out = 0;
    switch (s->format) {
    case 0: {   /* DSP ADPCM: nibble addresses, 8-byte frames (header + 14 samples) */
        if ((s->cur & 15) == 0) {
            s->ps = MEM[pa(s->cur >> 1)];
            s->cur += 2;
        }
        u8 b = MEM[pa(s->cur >> 1)];
        int nib = (s->cur & 1) ? (b & 15) : (b >> 4);
        if (nib >= 8) nib -= 16;
        int pred = (s->ps >> 4) & 7, scale = 1 << (s->ps & 15);
        s32 v = ((nib * scale) << 11) + s->coef[pred * 2] * s->yn1 + s->coef[pred * 2 + 1] * s->yn2;
        v = (v + 1024) >> 11;
        out = clamp16(v);
        s->yn2 = s->yn1;
        s->yn1 = out;
        break;
    }
    case 10: out = (s16)mr16(pa(s->cur * 2)); break;          /* PCM16: sample addresses */
    case 25: out = (s16)((s8)MEM[pa(s->cur)] << 8); break;    /* PCM8: byte addresses */
    default: out = 0;
    }
    if (s->cur == s->end) {
        /* the DSP always continues at the loop address; without looping it also stops the voice.
         * nw4r points the loop of one-shot sounds at a silent buffer and detects the end by the
         * playback address lying inside it. */
        s->cur = s->loop;
        if (s->loopflag) {
            if (s->format == 0) {
                if (!s->stream) { s->yn1 = s->loop_yn1; s->yn2 = s->loop_yn2; }
                s->ps = s->loop_ps;
            }
        } else
            s->stopped = 1;
    } else
        s->cur++;
    return out;
}

static void mix_one(u32 m, u32 ctrl, int bit_on, int bit_delta, u32 voff, u32 doff, s32 *dst, const s32 *in) {
    if (!(ctrl & (1u << bit_on))) return;
    s32 v = r16(m + voff);
    s32 d = (ctrl & (1u << bit_delta)) ? (s16)r16(m + doff) : 0;
    for (int i = 0; i < N; i++) {
        dst[i] += (s32)(((s64)in[i] * v) >> 15);
        v += d;
        if (v < 0) v = 0;
        if (v > 0xFFFF) v = 0xFFFF;
    }
    if (d) w16(m + voff, (u16)v);
}

static int axlog = -1, axlog_frame;
static void process_voice(u32 pb) {
    if (axlog < 0) axlog = ENV("RT_AXLOG") != NULL;
    if (axlog && axlog_frame % 333 == 0)
        fprintf(stderr, "ax: pb %08X state %u type %u fmt %u cur %08X end %08X loop %08X/%u ratio %08X vol %04X mix %08X L %04X R %04X\n", pb,
                r16(pb + 0x10), r16(pb + 0x12), r16(pb + 0x70), (u32)r16(pb + 0x7A) << 16 | r16(pb + 0x7C),
                (u32)r16(pb + 0x76) << 16 | r16(pb + 0x78), (u32)r16(pb + 0x72) << 16 | r16(pb + 0x74), r16(pb + 0x6E),
                (u32)r16(pb + 0xA6) << 16 | r16(pb + 0xA8), r16(pb + 0x6A), (u32)r16(pb + 0x0C) << 16 | r16(pb + 0x0E), r16(pb + 0x14), r16(pb + 0x18));
    if (r16(pb + 0x10) != 1) return;   /* state: running */
    Src s = {0};
    s.loopflag = r16(pb + 0x6E);
    s.format = r16(pb + 0x70);
    s.loop = (u32)r16(pb + 0x72) << 16 | r16(pb + 0x74);
    s.end = (u32)r16(pb + 0x76) << 16 | r16(pb + 0x78);
    s.cur = (u32)r16(pb + 0x7A) << 16 | r16(pb + 0x7C);
    s.stream = r16(pb + 0x12) == 1;
    for (int i = 0; i < 16; i++) s.coef[i] = (s16)r16(pb + 0x7E + 2 * i);
    s.ps = r16(pb + 0xA0);
    s.yn1 = (s16)r16(pb + 0xA2);
    s.yn2 = (s16)r16(pb + 0xA4);
    s.loop_ps = r16(pb + 0xB4);
    s.loop_yn1 = (s16)r16(pb + 0xB6);
    s.loop_yn2 = (s16)r16(pb + 0xB8);

    /* resampling: 16.16 ratio of input to output samples, fractional position, last 4 inputs */
    u32 ratio = (u32)r16(pb + 0xA6) << 16 | r16(pb + 0xA8);
    u32 frac = r16(pb + 0xAA);
    s16 hist[4];
    for (int i = 0; i < 4; i++) hist[i] = (s16)r16(pb + 0xAC + 2 * i);
    s32 smp[N];
    for (int i = 0; i < N; i++) {
        smp[i] = hist[2] + (((s32)(hist[3] - hist[2]) * (s32)frac) >> 16);
        frac += ratio;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            hist[0] = hist[1]; hist[1] = hist[2]; hist[2] = hist[3];
            hist[3] = next_sample(&s);
        }
    }
    /* volume envelope (1.15) */
    s32 vol = r16(pb + 0x6A), dv = (s16)r16(pb + 0x6C);
    for (int i = 0; i < N; i++) {
        smp[i] = (s32)(((s64)smp[i] * vol) >> 15);
        vol += dv;
        if (vol < 0) vol = 0;
        if (vol > 0xFFFF) vol = 0xFFFF;
    }
    /* one-pole low-pass */
    if (r16(pb + 0xBA)) {
        s32 y = (s16)r16(pb + 0xBC), a0 = r16(pb + 0xBE), b0 = r16(pb + 0xC0);
        for (int i = 0; i < N; i++) {
            y = (s32)(((s64)a0 * smp[i] + (s64)b0 * y) >> 15);
            smp[i] = y;
        }
        w16(pb + 0xBC, (u16)clamp16(y));
    }
    u32 ctrl = (u32)r16(pb + 0x0C) << 16 | r16(pb + 0x0E);
    u32 m = pb + 0x14;
    mix_one(m, ctrl, 0, 2, 0x00, 0x02, bus[BUS_L], smp);
    mix_one(m, ctrl, 1, 2, 0x04, 0x06, bus[BUS_R], smp);
    mix_one(m, ctrl, 16, 18, 0x08, 0x0A, bus[BUS_AL], smp);
    mix_one(m, ctrl, 17, 18, 0x0C, 0x0E, bus[BUS_AR], smp);
    mix_one(m, ctrl, 21, 23, 0x10, 0x12, bus[BUS_BL], smp);
    mix_one(m, ctrl, 22, 23, 0x14, 0x16, bus[BUS_BR], smp);
    mix_one(m, ctrl, 26, 28, 0x18, 0x1A, bus[BUS_CL], smp);
    mix_one(m, ctrl, 27, 28, 0x1C, 0x1E, bus[BUS_CR], smp);
    mix_one(m, ctrl, 3, 4, 0x20, 0x22, bus[BUS_S], smp);
    mix_one(m, ctrl, 19, 20, 0x24, 0x26, bus[BUS_AS], smp);
    mix_one(m, ctrl, 24, 25, 0x28, 0x2A, bus[BUS_BS], smp);
    mix_one(m, ctrl, 29, 30, 0x2C, 0x2E, bus[BUS_CS], smp);

    /* write back what the DSP updates */
    if (s.stopped) w16(pb + 0x10, 0);
    w16(pb + 0x7A, (u16)(s.cur >> 16));
    w16(pb + 0x7C, (u16)s.cur);
    w16(pb + 0xA0, s.ps);
    w16(pb + 0xA2, (u16)s.yn1);
    w16(pb + 0xA4, (u16)s.yn2);
    w16(pb + 0xAA, (u16)frac);
    for (int i = 0; i < 4; i++) w16(pb + 0xAC + 2 * i, (u16)hist[i]);
    w16(pb + 0x6A, (u16)vol);
}

/* ---------------------------------------------------------------- output */

static FILE *wav;
static u32 wav_bytes;
static void *sdl;
static u32 audio_dev;
static int (*SDL_QueueAudio_)(u32, const void *, u32);
static u32 (*SDL_GetQueuedAudioSize_)(u32);

void audio_finish(void) {
    if (!wav) return;
    u8 h[44];
    u32 v;
    u16 x;
    memcpy(h, "RIFF", 4);
    v = 36 + wav_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    x = 1; memcpy(h + 20, &x, 2);
    x = 2; memcpy(h + 22, &x, 2);
    v = 32000; memcpy(h + 24, &v, 4);
    v = 32000 * 4; memcpy(h + 28, &v, 4);
    x = 4; memcpy(h + 32, &x, 2);
    x = 16; memcpy(h + 34, &x, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &wav_bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fclose(wav);
    wav = NULL;
}

static void on_term(int sig) { (void)sig; exit(0); }

static void audio_open(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    const char *w = ENV("RT_WAV");
    if (w && (wav = fopen(w, "wb"))) {
        u8 hdr[44] = {0};
        fwrite(hdr, 1, 44, wav);
        atexit(audio_finish);
        signal(SIGTERM, on_term);
        signal(SIGINT, on_term);
    }
    if (ENV("RT_HEADLESS") || (!ENV("DISPLAY") && !ENV("WAYLAND_DISPLAY"))) return;
    sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
    if (!sdl) return;
    int (*init)(u32) = (int (*)(u32))dlsym(sdl, "SDL_InitSubSystem");
    u32 (*open_dev)(const char *, int, const void *, void *, int) =
        (u32(*)(const char *, int, const void *, void *, int))dlsym(sdl, "SDL_OpenAudioDevice");
    void (*pause)(u32, int) = (void (*)(u32, int))dlsym(sdl, "SDL_PauseAudioDevice");
    *(void **)&SDL_QueueAudio_ = dlsym(sdl, "SDL_QueueAudio");
    *(void **)&SDL_GetQueuedAudioSize_ = dlsym(sdl, "SDL_GetQueuedAudioSize");
    if (!init || !open_dev || !pause || !SDL_QueueAudio_ || !SDL_GetQueuedAudioSize_ || init(0x10) != 0) return;
    /* SDL_AudioSpec: freq, format (AUDIO_S16LSB), channels, silence, samples, padding, size, callback, userdata */
    struct { int freq; u16 format; u8 channels, silence; u16 samples, padding; u32 size; void *cb, *ud; } want = {32000, 0x8010, 2, 0, 512, 0, 0, NULL, NULL};
    audio_dev = open_dev(NULL, 0, &want, NULL, 0);
    if (audio_dev) pause(audio_dev, 0);
}

/* the mix goes to AX's output buffer in guest memory; the AI DMA (below) plays it */
static void output(u16 master, u32 lr_addr) {
    for (int i = 0; i < N && lr_addr; i++) {
        s32 l = (s32)(((s64)bus[BUS_L][i] * master) >> 15), r = (s32)(((s64)bus[BUS_R][i] * master) >> 15);
        w16(lr_addr + 4 * i, (u16)clamp16(l));
        w16(lr_addr + 4 * i + 2, (u16)clamp16(r));
    }
}

/* ---------------------------------------------------------------- AI DMA */

extern u32 g_ai_dma_addr, g_ai_dma_len;
static u32 play_addr, play_len;

u64 ai_block_ticks(void) {
    u32 len = play_len ? play_len : (g_ai_dma_len ? g_ai_dma_len : N * 4);
    return (u64)TB_CLOCK * (len / 4) / 32000;   /* 16-bit stereo at 32 kHz */
}

/* one DMA block has played: send it to the host, then the next block (set by AIInitDMA) starts */
void ai_block_done(void) {
    audio_open();
    if (play_addr && play_len && play_len <= 0x10000) {
        static s16 pcm[0x8000];
        u32 n = play_len / 2;
        for (u32 i = 0; i < n; i++) pcm[i] = (s16)r16(play_addr + 2 * i);
        if (wav) { fwrite(pcm, 2, n, wav); wav_bytes += n * 2; }
        if (audio_dev && SDL_GetQueuedAudioSize_(audio_dev) < 32000 * 4 / 5)   /* keep latency under 200 ms */
            SDL_QueueAudio_(audio_dev, pcm, n * 2);
    }
    play_addr = g_ai_dma_addr;
    play_len = g_ai_dma_len;
}

/* ---------------------------------------------------------------- command list */

static void aux(int which, u16 vol, u32 in, u32 out) {
    int b = BUS_AL + 3 * which;
    /* upload this frame's aux bus for the CPU effect (reverb etc.); mix back the effect's output */
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < N; i++) {
            mw32(pa(in + 4 * (k * N + i)), (u32)bus[b + k][i]);
            s32 r = (s32)r32(out + 4 * (k * N + i));
            bus[k == 0 ? BUS_L : k == 1 ? BUS_R : BUS_S][i] += (s32)(((s64)r * vol) >> 15);
        }
}

static void run_list(u32 list) {
    u32 p = list;
    for (int guard = 0; guard < 64; guard++) {
        u16 cmd = r16(p);
        p += 2;
#define A32() (p += 4, (u32)r16(p - 4) << 16 | r16(p - 2))
        switch (cmd) {
        case 0: A32(); memset(bus, 0, sizeof bus); break;   /* SETUP */
        case 1: case 2: case 3: A32(); break;                 /* surround into LR (stereo: nothing) */
        case 4: {   /* PROCESS: walk the voice list */
            axlog_frame++;
            u32 pb = A32();
            for (int n = 0; pb && n < 256; n++) {
                process_voice(pb);
                pb = (u32)r16(pb) << 16 | r16(pb + 2);
            }
            break;
        }
        case 5: case 6: case 7: { u16 vol = r16(p); p += 2; u32 in = A32(), out = A32(); aux(cmd - 5, vol, in, out); break; }
        case 8: case 9: p += 2; for (int i = 0; i < 6; i++) A32(); break;
        case 10: p += 4; A32(); break;   /* COMPRESSOR */
        case 11: case 12: { u16 vol = r16(p); p += 2; A32(); u32 lr = A32(); output(vol, lr); break; }
        case 13: for (int i = 0; i < 4; i++) A32(); break;   /* remote speakers */
        case 14: return;   /* END */
        default:
            if (ENV("RT_HLELOG")) fprintf(stderr, "ax: unknown command %u\n", cmd);
            return;
        }
#undef A32
    }
}

/* the AX microcode's mailbox: "command list size" (0xBABExxxx), then the list address */
static int expect_list;
HLE(DSPSendMailToDSP) {
    u32 m = c->r[3];
    if ((m >> 16) == 0xBABE) expect_list = 1;
    else if (expect_list) {
        expect_list = 0;
        run_list(m);
        mw32(AX_OUT_DSP_READY, 1);   /* the DSP is done with the frame (__AXDSPResumeCallback) */
    }
}

void audio_frame_done(void) {}

/* debug (thread dump): AX voices on the priority stacks, CPU-side and DSP-side state */
void ax_dump(void) {
    fprintf(stderr, "---- AX: %u voices serviced last frame, PB array %08X\n", mr32(DATA___AXNumVoices), mr32(DATA___AXPB));
    for (int prio = 31; prio >= 0; prio--)
        for (u32 v = mr32(DATA___AXStackHead + 4 * prio), n = 0; v && n < 96; v = mr32(v), n++) {
            u32 idx = mr32(v + 0x18), pb = mr32(DATA___AXPB) + idx * 0x140;
            fprintf(stderr, "  prio %2d vpb %08X idx %2u sync %08X | cpu state %u fmt %u cur %08X | dsp state %u\n", prio, v, idx,
                    mr32(v + 0x1C), mr16(v + 0x28 + 0x10), mr16(v + 0x28 + 0x70),
                    (u32)mr16(v + 0x28 + 0x7A) << 16 | mr16(v + 0x28 + 0x7C), r16(pb + 0x10));
        }
}
