/* Cinematic Noir: a rompler for MPC OS on the shared wrapper (wrapper/engine.h, an instrument).
 * Sounds are multisample folders of WAVs: the built-in set in <plugin>/rom (rendered by tools/render_rom.c) and the user's
 * own in <plugin>/User (a folder per instrument, or loose WAVs as one-shots). A file's root note comes from its name
 * (C3 = 60, or a plain MIDI number at the end); loops come from the WAV's smpl chunk. A rom folder's info.txt says which
 * modes list it ("modes=cinematic,dark"); User sounds show in every mode. The selected sound is saved by name.
 * Loading runs on a worker thread; render() picks the new instrument up with a trylock (docs/PORTING.md).
 * Voice: linear-interpolated playback, attack/release envelope, chop (auto-release into a stab), tempo-synced stutter
 * (retrigger with a short crossfade), velocity to level and tone, a sine sub (Sub Weight), and Octave Drop (a band-limited
 * square or the same sample at half speed, one octave down). Speed plays everything at 45, 33 or half speed.
 * Mix chain: crush (sample-rate and bit reduction) -> 24 dB low-pass with resonance -> tape (input compression,
 * asymmetric saturation, head bump, high roll-off, hiss) -> wow & flutter on mid and side (the side gets its own warble
 * above 100% width) -> dark FDN reverb send (room, plate, warehouse) -> M/S width with the lows kept mono -> Squash
 * (VCA compressor) -> cracked vinyl (dust, crackle, static, rumble, a scratch once a revolution; follows playing) ->
 * volume and limiter. Mode (cinematic, dark, gritty) voices the chain and picks which sounds the browser lists.
 * Presets live in the engine (src/presets.h from tools/gen_presets.py): state strings grouped by category. "preset" is
 * MPC's PRESET list (vst.json "programs"), "category" jumps to a category's first preset, preset_prev/next step within the
 * category. A load reports every parameter it moved through mpc_engine_changed(); the name shows " *" once edited.
 * disp_* are the display's readouts (sound info, FX summary, what is playing); squash_gr is the Squash gain reduction.
 * Keys, ranges and defaults must agree with params.json. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "engine.h"
#include "plugin_dir.h"
#include "presets.h"

#define SR 44100.0f
#define NV 16              /* voices */
#define NKO 6              /* key-off noises at once */
#define MAX_SOUNDS 128     /* listed; a mode's browser reaches the first VIEW_MAX of them (the sound param's range) */
#define VIEW_MAX 64
#define MAX_ZONES 64
#define MAX_FILE_SEC 60    /* longest WAV loaded, seconds at its own rate */
#define WOW_LEN 4096       /* wow delay line, frames (93 ms) */
#define FDN_N 8
#define FDN_LEN 8192
#define PRE_LEN 4096       /* reverb predelay + early reflections */
#define XF 64              /* stutter crossfade, frames */
#define ECHO_LEN 65536     /* Noir Echo line, frames (1.49 s; Echo Time tops out at 1.2 s) */
#define ENS_LEN 2048       /* Ensemble chorus lines */
#define PS_LEN 4096        /* Shadow and Shimmer pitch shifters */
#define PS_WIN 2048.0f     /* their grain, frames (46 ms) */
#define PI 3.14159265f

/* ---- parameters (params.json order) ---- */
enum { P_CUTOFF, P_DRIVE, P_DRIFT, P_SPACE, P_ATTACK, P_CRUSH, P_SUB, P_VOLUME, P_RESO, P_RELEASE, P_CHOP, P_STUTTER,
       P_KEYOFF, P_VINYL, P_WIDTH, P_SQUASH, P_SOUND, P_SOUND_PREV, P_SOUND_NEXT, P_MODE, P_SPACE_TYPE, P_SPEED, P_OCTDROP,
       P_VELSENS, P_STATUS, P_RESCAN, P_PRESET, P_PRESET_PREV, P_PRESET_NEXT, P_CATEGORY, P_VY_DUST, P_VY_STATIC,
       P_VY_SCRATCH, P_VY_FOLLOW, P_MONO_FREQ, P_TAIL_WARBLE, P_SQ_STYLE, P_SQ_RELEASE, P_SQ_MIX, P_DISP_INFO, P_DISP_FX,
       P_DISP_FX2, P_DISP_PLAY, P_SQ_GR, P_TRANSPOSE, P_FINE, P_DECAY, P_SUSTAIN, P_START, P_GLIDE, P_SPREAD, P_BEND_RANGE,
       P_VOICE_MODE, P_DISP_DESC, P_W0, P_WAVE_HEAD = P_W0 + 40, P_TRIG, P_WAVE_IMG, P_TAPE_LOW, P_TAPE_HIGH, P_TAPE_BUMP,
       P_TILT, P_WARMTH, P_AIR, P_SHADOW, P_SHIMMER, P_GHOST_TAIL, P_ECHO_TIME, P_ECHO_FB, P_ECHO_TONE, P_ECHO_MIX, P_ENSEMBLE,
       P_PULSE, P_PULSE_RATE, NP };
#define WAVE_IMGS 33       /* waveform pictures of the rom sounds: wave_img 1..N dim (idle), N+1..2N lit (playing); art/make_wave_images.py */
#define NWCOL 40           /* waveform columns on the screen (w00..w39) */
#define WLEVELS 12         /* their heights, 0..11 */
static const char *const KEYS[NP] = { "cutoff", "drive", "drift", "space", "attack", "crush", "sub", "volume", "reso",
    "release", "chop", "stutter", "keyoff", "vinyl", "width", "squash", "sound", "sound_prev", "sound_next", "mode",
    "space_type", "speed", "octdrop", "velsens", "status", "rescan", "preset", "preset_prev", "preset_next", "category",
    "vinyl_dust", "vinyl_static", "vinyl_scratch", "vinyl_follow", "mono_freq", "tail_warble", "squash_style",
    "squash_release", "squash_mix", "disp_info", "disp_fx", "disp_fx2", "disp_play", "squash_gr", "transpose", "fine",
    "amp_decay", "amp_sustain", "sample_start", "glide", "pan_spread", "bend_range", "voice_mode", "disp_desc",
    "w00", "w01", "w02", "w03", "w04", "w05", "w06", "w07", "w08", "w09", "w10", "w11", "w12", "w13", "w14", "w15",
    "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23", "w24", "w25", "w26", "w27", "w28", "w29", "w30", "w31",
    "w32", "w33", "w34", "w35", "w36", "w37", "w38", "w39", "wave_head", "trig", "wave_img", "tape_lowcut", "tape_highcut", "tape_bump",
    "tone_tilt", "tone_warmth", "tone_air", "shadow", "shimmer", "ghost_tail", "echo_time", "echo_feedback", "echo_tone",
    "echo_mix", "ensemble", "pulse_depth", "pulse_rate" };
static const char *const NAMES[NP] = { "CUTOFF", "TAPE DRIVE", "DRIFT", "SPACE", "ATTACK", "CRUSH", "SUB WEIGHT", "VOLUME",
    "RESONANCE", "RELEASE", "CHOP", "STUTTER", "KEY-OFF", "CRACKED VINYL", "WIDTH", "SQUASH", "SOUND", "", "", "MODE",
    "SPACE TYPE", "SPEED", "OCTAVE DROP", "VELOCITY", "", "", "", "", "", "CATEGORY", "DUST", "STATIC", "SCRATCH",
    "VINYL MODE", "MONO BELOW", "TAIL WARBLE", "SQUASH STYLE", "SQUASH RELEASE", "SQUASH MIX", "", "", "", "", "",
    "TRANSPOSE", "FINE TUNE", "DECAY", "SUSTAIN", "SAMPLE START", "GLIDE", "SPREAD", "BEND RANGE", "VOICE MODE",
    [P_TAPE_LOW] = "TAPE LOW CUT", [P_TAPE_HIGH] = "TAPE HIGH CUT", [P_TAPE_BUMP] = "HEAD BUMP", [P_TILT] = "TILT",
    [P_WARMTH] = "WARMTH", [P_AIR] = "AIR", [P_SHADOW] = "SHADOW", [P_SHIMMER] = "SHIMMER", [P_GHOST_TAIL] = "GHOST TAIL",
    [P_ECHO_TIME] = "ECHO TIME", [P_ECHO_FB] = "ECHO FEEDBACK", [P_ECHO_TONE] = "ECHO TONE", [P_ECHO_MIX] = "ECHO MIX",
    [P_ENSEMBLE] = "ENSEMBLE", [P_PULSE] = "PULSE", [P_PULSE_RATE] = "PULSE RATE" };   /* the rest NULL */
static const float PMIN[NP] = { [P_VOLUME] = -24, [P_TRANSPOSE] = -24, [P_FINE] = -50, [P_BEND_RANGE] = 1 };   /* the rest 0 */
static const float PMAX[NP] = { 1, 100, 100, 100, 1, 1, 100, 6, 100, 1, 1, 4, 100, 100, 200, 100, 63, 1, 1, 2, 2, 2, 2,
    100, 0, 1, NOIR_NPRESETS - 1, 1, 1, 7, 100, 100, 100, 1, 1, 100, 2, 1, 100, 0, 0, 0, 0, 0,
    24, 50, 1, 100, 100, 1, 100, 12, 1, 0, [P_W0 ... P_W0 + NWCOL - 1] = WLEVELS - 1, [P_WAVE_HEAD] = NWCOL, [P_TRIG] = 1, [P_WAVE_IMG] = 2 * WAVE_IMGS, [P_TAPE_LOW] = 1, [P_TAPE_HIGH] = 1, [P_TAPE_BUMP] = 100,
    [P_TILT] = 100, [P_WARMTH] = 100, [P_AIR] = 100, [P_SHADOW] = 100, [P_SHIMMER] = 100, [P_GHOST_TAIL] = 100, [P_ECHO_TIME] = 1,
    [P_ECHO_FB] = 95, [P_ECHO_TONE] = 1, [P_ECHO_MIX] = 100, [P_ENSEMBLE] = 100, [P_PULSE] = 100, [P_PULSE_RATE] = 3 };
static const float PDEF[NP] = { 0.66f, 30, 15, 30, 0, 0, 0, 0, 15, 0.35f, 0, 0, 0, 25, 100, 0, 0, 0, 0, 1, 0, 0, 0, 70,
    0, 0, 0, 0, 0, 0, 100, 100, 100, 0, 0.574f, 25, 0, 0.4f, 100, 0, 0, 0, 0, 0,
    0, 0, 0.5f, 100, 0, 0, 0, 2, 0, [P_TAPE_HIGH] = 0.75f, [P_TILT] = 40, [P_SHADOW] = 15, [P_GHOST_TAIL] = 50, [P_ECHO_TIME] = 0.45f,
    [P_ECHO_FB] = 35, [P_ECHO_TONE] = 0.4f, [P_PULSE_RATE] = 1 };   /* the defaults are DARK mode's character (preset 0) */
static int is_text(int i) { return i == P_STATUS || (i >= P_DISP_INFO && i <= P_SQ_GR) || i == P_DISP_DESC; }
static int is_visual(int i) { return i >= P_W0 && i <= P_WAVE_IMG; }   /* the waveform, playhead, lamp and picture: the engine's alone */
static int is_trigger(int i) { return i == P_SOUND_PREV || i == P_SOUND_NEXT || i == P_PRESET_PREV || i == P_PRESET_NEXT || i == P_RESCAN; }
static const char *const OPT_CAT[8] = { "KEYS", "STRINGS", "CHOIR", "BRASS", "PADS", "SYNTH", "TEXTURE", "LEADS" };
static const char *const OPT_VYF[2] = { "FOLLOW", "ALWAYS" };
static const char *const OPT_SQS[3] = { "VCA", "GLUE", "SPIT" };
static const char *const OPT_STUTTER[] = { "OFF", "1/4", "1/8", "1/16", "1/32" };
static const char *const OPT_MODE[] = { "CINEMATIC", "DARK", "GRITTY" };
static const char *const OPT_SPACE[] = { "ROOM", "PLATE", "WAREHOUSE" };
static const char *const OPT_SPEED[] = { "45", "33", "HALF" };
static const char *const OPT_OCT[] = { "OFF", "SQUARE", "DOUBLE" };
static const float STUT_BEATS[5] = { 0, 1.0f, 0.5f, 0.25f, 0.125f };
static const float SPEED_RATIO[3] = { 1.0f, 33.333f / 45.0f, 0.5f };

/* per mode: drive, cutoff and reverb-time scale, reverb damping (Hz), width scale, squash ratio added */
typedef struct { float drive_k, cut_k, rt_k, damp_hz, width_k, ratio_add; } voicing_t;
static const voicing_t VOICING[3] = {
    { 0.8f, 1.0f, 1.25f, 7000.0f, 1.15f, 0.0f },   /* CINEMATIC: cleaner tape, longer and wider tails */
    { 1.0f, 0.7f, 1.0f, 4000.0f, 1.0f, 1.0f },     /* DARK: muffled, dark reverb */
    { 1.35f, 0.9f, 0.8f, 5000.0f, 0.8f, 4.0f },    /* GRITTY: hot tape, tighter and narrower, harder squash */
};

/* ---- instruments ---- */
typedef struct {
    int root, lo, hi, nch, len;      /* len in frames */
    int loop_start, loop_end;        /* loop_end <= 0: no loop */
    float rate;                      /* playback step at the root: file rate / 44100 */
    int16_t *data;                   /* interleaved, nch channels */
} zone_t;
typedef struct {
    char name[64], desc[96];
    int modes, nz, rom_idx;           /* rom_idx: position among the rom sounds (its waveform picture), -1 for User */
    zone_t z[MAX_ZONES];
    /* for the screen (inst_analyse): the waveform of the zone around middle C, its length, the memory used */
    unsigned char wave[NWCOL];
    float max_sec, mb;
} inst_t;
typedef struct { char name[64], desc[96]; char path[512]; int modes, is_file, rom_idx; } entry_t;   /* modes: bit per mode */

typedef struct {
    int active, note, held, sustained, released;
    const zone_t *z;
    double pos, opos, dpos, inc;      /* opos: the position before a stutter retrigger, faded out over XF frames */
    int xf, done_main, done_dbl;
    float env, att_step, vel_g, tone_a, tz[2];
    int decaying;                     /* past the attack: falling to the sustain level */
    float goff;                       /* glide: semitones still to slide, falling to 0 */
    float pan_l, pan_r;               /* Spread */
    float subph, sqph, sqz;
    int gate;                         /* chop: frames left before the auto-release, < 0 off */
    unsigned age;
} voice_t;
typedef struct { int active; float t, amp, nz; } keyoff_t;

typedef struct {
    float val[NP];
    char sname[64];                   /* the selected sound's name, kept for the state */
    int last_touched;                 /* for the status line */
    uint64_t changed[2];              /* params the engine moved, to report to the host (mpc_engine_changed), a bit each */
    /* the screen's waveform: what the host was last shown, and display_rev, bumped when any of it moves */
    unsigned char vis[NWCOL + 3];     /* columns, playhead, lamp, picture */
    const inst_t *vis_inst;
    unsigned disp_rev;
    float head_frac; int head_on, head_root;   /* the newest voice: where it is in its zone, and the zone's root */
    long trig_frames;                 /* the TRIG lamp stays lit this long after a note */
    int dirty, restoring;             /* dirty: edited since the preset loaded; restoring: a state is being applied */
    int last_note, last_vel, nactive; /* for the display */
    float gr_hold;                    /* Squash gain reduction, peak-held for the readout */
    float bpm, bend, modwheel;
    int sustain;
    unsigned rng, age;
    /* sound list (rebuilt by a scan) and the current mode's view of it */
    entry_t *list; int nlist;
    int view[MAX_SOUNDS]; int nview;
    char rom_dir[512], user_dir[512];
    /* the current instrument and the worker's hand-over */
    inst_t *cur, *fallback;
    pthread_t th; pthread_mutex_t mu; pthread_cond_t cv;
    int quit, req, req_gen, done_gen, loading, rescan_req;
    inst_t *pending, *retired;
    char load_name[64];
    /* voices */
    voice_t v[NV]; keyoff_t ko[NKO];
    long stut_ctr;
    /* crush */
    float cr_ph, cr_l, cr_r;
    /* filter: two SVFs per channel */
    float fc_s, svf[2][2][2];
    /* tape */
    float tp_env[2], hb[2][2], hl[2][2], hb_c[5], hl_c[5], tp_d_last;
    /* tape filter: low cut, head bump, high cut (biquads per channel) and the settings they were designed for */
    float tf_z[3][2][2], tf_c[3][5], tf_last[3];
    /* MOOD: tone (low shelf + high shelf tilt, warmth peak, air shelf), Noir Echo, Ensemble, Shadow and Shimmer, Pulse */
    float to_z[4][2][2], to_c[4][5], to_last[3];
    float echo[2][ECHO_LEN]; int echo_pos; float echo_lp[2], echo_hp[2], echo_t_s;
    float ens[2][ENS_LEN]; int ens_pos; float ens_ph[3];
    float ps_buf[PS_LEN]; int ps_pos; float ps_ph[2], ps_lp, ps_hp, ghost_fb;
    double pulse_ph;
    float vy_gate;                    /* Cracked Vinyl FOLLOW: open while voices sound, closing 0.4 s after the last */
    /* wow: mid and side delay lines */
    float wm[WOW_LEN], ws[WOW_LEN]; int wpos;
    float wow_ph, flut_ph, side_ph, drift_rnd, drift_tgt; int drift_ctr;
    /* reverb */
    float pre[PRE_LEN]; int prepos;
    float fdn[FDN_N][FDN_LEN]; int fpos[FDN_N]; float fdz[FDN_N];
    float ap[4][1024]; int appos[4];
    float rv_hp, rv_hpz, rv_outhp[2];
    float fdn_len[FDN_N], fdn_g[FDN_N], fdn_damp, pre_d; int er_on; float ap_g;
    int rv_type_last, rv_mode_last; float rv_ghost_rt;   /* rv_ghost_rt: the decay the ghost tails asked for (0 = none) */
    /* width */
    float s_hp[2];
    /* squash */
    float sq_env, sq_gain;
    /* vinyl */
    float vy_click, vy_clickz, vy_rum[2], vy_hiss[2][2], vy_scr_t; long vy_rev; float vy_scr_amp; int vy_scr_on;
    float vy_pan;
    /* limiter */
    float lim_env, lim_g;
} noir_t;

/* ---- helpers ---- */
static inline float frand(noir_t *e) { e->rng ^= e->rng << 13; e->rng ^= e->rng >> 17; e->rng ^= e->rng << 5; return (float)(e->rng & 0xffffff) / 8388608.0f - 1.0f; }
_Static_assert(NP <= 128, "changed[] holds 128 parameter bits");
static inline void mark_changed(noir_t *e, int i) { __atomic_or_fetch(&e->changed[i >> 6], 1ull << (i & 63), __ATOMIC_RELAXED); }
static inline float fast_tanh(float x) { if (x > 3) return 1; if (x < -3) return -1; float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
static inline float db2lin(float d) { return powf(10.0f, d / 20.0f); }
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float cutoff_hz(float v) { return 40.0f * powf(20000.0f / 40.0f, v); }
static float attack_s(float v) { return 0.0005f * powf(8000.0f, v); }      /* 0.5 ms .. 4 s */
static float release_s(float v) { return 0.01f * powf(600.0f, v); }        /* 10 ms .. 6 s */
static float chop_s(float v) { return 1.5f * powf(0.025f, v); }            /* 1.5 s .. 37 ms (0 = off) */
static float crush_rate(float v) { return 44100.0f * powf(5500.0f / 44100.0f, v); }
static float crush_bits(float v) { return 16.0f - 10.0f * v; }
static float mono_hz(float v) { return 40.0f * powf(10.0f, v); }               /* 40 .. 400 Hz */
static float squash_rel_s(float v) { return 0.02f * powf(40.0f, v); }        /* 20 ms .. 800 ms */
static float decay_s(float v) { return 0.05f * powf(400.0f, v); }            /* 50 ms .. 20 s */
static float glide_s(float v) { return 0.005f * powf(400.0f, v); }           /* 5 ms .. 2 s (0 = off) */
static float tape_low_hz(float v) { return 20.0f * powf(40.0f, v); }        /* 20 .. 800 Hz */
static float tape_high_hz(float v) { return 1500.0f * powf(20000.0f / 1500.0f, v); }   /* 1.5 .. 20 kHz */
static float echo_time_s(float v) { return 0.04f * powf(30.0f, v); }        /* 40 ms .. 1.2 s */
static float echo_tone_hz(float v) { return 800.0f * powf(15.0f, v); }      /* 0.8 .. 12 kHz */
static const float PULSE_BEATS[4] = { 2.0f, 1.0f, 0.5f, 0.25f };
static void note_name(int n, char *buf, int len) {   /* C3 = 60, MPC's naming */
    static const char *const NN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    snprintf(buf, (size_t)len, "%s%d", NN[n % 12], n / 12 - 2);
}
static int opt(const noir_t *e, int p) { return (int)lrintf(e->val[p]); }
static float note_hz(float n) { return 440.0f * powf(2.0f, (n - 69.0f) / 12.0f); }

static void biquad_lp(float *c, float hz, float q) {
    float w = 2 * PI * hz / SR, cw = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
    c[0] = (1 - cw) / 2 / a0; c[1] = (1 - cw) / a0; c[2] = c[0]; c[3] = -2 * cw / a0; c[4] = (1 - al) / a0;
}
static void biquad_hp(float *c, float hz, float q) {
    float w = 2 * PI * hz / SR, cw = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
    c[0] = (1 + cw) / 2 / a0; c[1] = -(1 + cw) / a0; c[2] = c[0]; c[3] = -2 * cw / a0; c[4] = (1 - al) / a0;
}
static void biquad_shelf(float *c, float hz, float db, int high) {   /* RBJ shelves, slope 1 */
    float A = powf(10, db / 40), w = 2 * PI * hz / SR, cw = cosf(w), al = sinf(w) / 2 * sqrtf(2), sA = 2 * sqrtf(A) * al, a0;
    if (high) {
        a0 = (A + 1) - (A - 1) * cw + sA;
        c[0] = A * ((A + 1) + (A - 1) * cw + sA) / a0; c[1] = -2 * A * ((A - 1) + (A + 1) * cw) / a0;
        c[2] = A * ((A + 1) + (A - 1) * cw - sA) / a0; c[3] = 2 * ((A - 1) - (A + 1) * cw) / a0; c[4] = ((A + 1) - (A - 1) * cw - sA) / a0;
    } else {
        a0 = (A + 1) + (A - 1) * cw + sA;
        c[0] = A * ((A + 1) - (A - 1) * cw + sA) / a0; c[1] = 2 * A * ((A - 1) - (A + 1) * cw) / a0;
        c[2] = A * ((A + 1) - (A - 1) * cw - sA) / a0; c[3] = -2 * ((A - 1) + (A + 1) * cw) / a0; c[4] = ((A + 1) + (A - 1) * cw - sA) / a0;
    }
}
static void biquad_peak(float *c, float hz, float q, float db) {
    float A = powf(10, db / 40), w = 2 * PI * hz / SR, cw = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al / A;
    c[0] = (1 + al * A) / a0; c[1] = -2 * cw / a0; c[2] = (1 - al * A) / a0; c[3] = -2 * cw / a0; c[4] = (1 - al / A) / a0;
}
static inline float biquad(const float *c, float *z, float x) {   /* Direct Form II transposed */
    float y = c[0] * x + z[0];
    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    return y;
}

/* ---- WAV loading ---- */
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* Reads a PCM 8/16/24/32-bit or float WAV into int16 (first two channels). Returns 0 on failure. */
static int load_wav(const char *path, zone_t *z) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[12];
    int ok = 0, fmt = 0, ch = 0, bits = 0, have_fmt = 0;
    unsigned rate = 0;
    long data_off = -1; unsigned data_len = 0;
    z->loop_start = z->loop_end = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto out;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        unsigned sz = rd32(c + 4);
        long next = ftell(f) + sz + (sz & 1);
        if (!memcmp(c, "fmt ", 4) && sz >= 16) {
            unsigned char b[40] = {0};
            if (fread(b, 1, sz < 40 ? sz : 40, f) < 16) goto out;
            fmt = rd16(b); ch = rd16(b + 2); rate = rd32(b + 4); bits = rd16(b + 14);
            if (fmt == 0xFFFE && sz >= 26) fmt = rd16(b + 24);   /* WAVE_FORMAT_EXTENSIBLE: the subformat's first word */
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            data_off = ftell(f); data_len = sz;
        } else if (!memcmp(c, "smpl", 4) && sz >= 36 + 24) {
            unsigned char b[60];
            if (fread(b, 1, 60, f) == 60 && rd32(b + 28) > 0) {
                z->loop_start = (int)rd32(b + 36 + 8);
                z->loop_end = (int)rd32(b + 36 + 12) + 1;   /* smpl's end is inclusive */
            }
        }
        if (fseek(f, next, SEEK_SET)) break;
    }
    if (!have_fmt || data_off < 0 || ch < 1 || rate < 4000 || rate > 192000) goto out;
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) goto out;
    int bps = bits / 8;
    long frames = data_len / (unsigned)(bps * ch);
    if (frames > (long)rate * MAX_FILE_SEC) frames = (long)rate * MAX_FILE_SEC;
    if (frames < 2) goto out;
    z->nch = ch >= 2 ? 2 : 1;
    z->len = (int)frames;
    z->rate = rate / SR;
    z->data = malloc((size_t)frames * z->nch * sizeof(int16_t));
    unsigned char *raw = malloc((size_t)4096 * bps * ch);
    if (!z->data || !raw) { free(z->data); free(raw); z->data = NULL; goto out; }
    fseek(f, data_off, SEEK_SET);
    long done = 0;
    while (done < frames) {
        long n = frames - done > 4096 ? 4096 : frames - done;
        long got = (long)fread(raw, (size_t)(bps * ch), (size_t)n, f);
        if (got <= 0) break;
        for (long i = 0; i < got; i++)
            for (int c = 0; c < z->nch; c++) {
                const unsigned char *s = raw + (i * ch + c) * bps;
                float x;
                if (fmt == 3) { float fl; memcpy(&fl, s, 4); x = fl; }
                else if (bits == 8) x = (s[0] - 128) / 128.0f;
                else if (bits == 16) x = (int16_t)rd16(s) / 32768.0f;
                else if (bits == 24) x = (float)((int32_t)((unsigned)s[0] << 8 | (unsigned)s[1] << 16 | (unsigned)s[2] << 24) >> 8) / 8388608.0f;
                else x = (float)(int32_t)rd32(s) / 2147483648.0f;
                z->data[(done + i) * z->nch + c] = (int16_t)lrintf(clampf(x, -1, 1) * 32767.0f);
            }
        done += got;
    }
    free(raw);
    z->len = (int)done;
    if (z->loop_end > z->len) z->loop_end = z->len;
    if (z->loop_end - z->loop_start < 16 || z->loop_start < 0) z->loop_start = z->loop_end = 0;
    ok = z->len >= 2;
    if (!ok) { free(z->data); z->data = NULL; }
out:
    fclose(f);
    return ok;
}

/* The root note in a file name: a note name at the end (C3 = 60, C#3, Db3, C-1) or else a trailing MIDI number. */
static int root_from_name(const char *file) {
    char s[256];
    snprintf(s, sizeof s, "%s", file);
    char *dot = strrchr(s, '.');
    if (dot) *dot = 0;
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '_')) s[--n] = 0;
    /* note name: letter, optional #/b, optional -, one digit */
    int i = n - 1;
    if (i >= 1 && isdigit((unsigned char)s[i])) {
        int oct = s[i] - '0', j = i - 1, neg = 0, acc = 0;
        if (j >= 0 && s[j] == '-') { neg = 1; j--; }
        if (j >= 0 && (s[j] == '#' || (s[j] == 'b' && j >= 1 && strchr("ABCDEFGabcdefg", s[j - 1])))) { acc = s[j] == '#' ? 1 : -1; j--; }
        if (j >= 0 && strchr("ABCDEFGabcdefg", s[j]) && (j == 0 || !isalpha((unsigned char)s[j - 1]))) {
            static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A..G */
            int note = base[toupper((unsigned char)s[j]) - 'A'] + acc + 12 * ((neg ? -oct : oct) + 2);
            if (note >= 0 && note <= 127) return note;
        }
    }
    int k = n;
    while (k > 0 && isdigit((unsigned char)s[k - 1])) k--;
    if (k < n && n - k <= 3) {
        int note = atoi(s + k);
        if (note >= 0 && note <= 127) return note;
    }
    return -1;
}

static int has_wav_ext(const char *n) { size_t l = strlen(n); return l > 4 && !strcasecmp(n + l - 4, ".wav"); }

/* What the screen shows about an instrument: the waveform of the zone that plays middle C (peak per column, square-root
 * scaled so quiet tails still show), the longest zone and the memory the samples take. */
static void inst_analyse(inst_t *in) {
    const zone_t *z = &in->z[0];
    long bytes = 0;
    in->max_sec = 0;
    for (int i = 0; i < in->nz; i++) {
        if (60 >= in->z[i].lo && 60 <= in->z[i].hi) z = &in->z[i];
        bytes += (long)in->z[i].len * in->z[i].nch * 2;
        float s = in->z[i].len / (in->z[i].rate * SR);
        if (s > in->max_sec) in->max_sec = s;
    }
    in->mb = bytes / 1048576.0f;
    float pk[NWCOL] = {0}, top = 1e-9f;
    for (int c = 0; c < NWCOL; c++) {
        long a = (long)z->len * c / NWCOL, b = (long)z->len * (c + 1) / NWCOL;
        for (long f = a; f < b; f += 3)
            for (int ch = 0; ch < z->nch; ch++) { float x = fabsf(z->data[f * z->nch + ch] / 32768.0f); if (x > pk[c]) pk[c] = x; }
        if (pk[c] > top) top = pk[c];
    }
    for (int c = 0; c < NWCOL; c++) {
        int lv = (int)lrintf(sqrtf(pk[c] / top) * (WLEVELS - 1));
        in->wave[c] = (unsigned char)(lv < 1 && pk[c] > top * 0.002f ? 1 : lv);
    }
}

static void free_inst(inst_t *in) {
    if (!in) return;
    for (int i = 0; i < in->nz; i++) free(in->z[i].data);
    free(in);
}

static int zone_cmp(const void *a, const void *b) { return ((const zone_t *)a)->root - ((const zone_t *)b)->root; }

static void map_zones(inst_t *in) {
    qsort(in->z, (size_t)in->nz, sizeof(zone_t), zone_cmp);
    for (int i = 0; i < in->nz; i++) {
        in->z[i].lo = i == 0 ? 0 : (in->z[i - 1].root + in->z[i].root) / 2 + 1;
        in->z[i].hi = i == in->nz - 1 ? 127 : (in->z[i].root + in->z[i + 1].root) / 2;
    }
}

static inst_t *load_entry(const entry_t *en) {
    inst_t *in = calloc(1, sizeof(inst_t));
    if (!in) return NULL;
    snprintf(in->name, sizeof in->name, "%s", en->name);
    snprintf(in->desc, sizeof in->desc, "%s", en->desc);
    in->modes = en->modes;
    in->rom_idx = en->rom_idx;
    if (en->is_file) {
        const char *base = strrchr(en->path, '/');
        int r = root_from_name(base ? base + 1 : en->path);
        if (load_wav(en->path, &in->z[0])) { in->z[0].root = r < 0 ? 60 : r; in->nz = 1; }
    } else {
        DIR *d = opendir(en->path);
        struct dirent *de;
        while (d && (de = readdir(d)) && in->nz < MAX_ZONES) {
            if (de->d_name[0] == '.' || !has_wav_ext(de->d_name)) continue;
            int r = root_from_name(de->d_name);
            if (r < 0) r = 60;
            int dup = 0;
            for (int i = 0; i < in->nz; i++) dup |= in->z[i].root == r;
            if (dup) continue;   /* one file per root note (no velocity layers yet) */
            char p[800];
            snprintf(p, sizeof p, "%s/%s", en->path, de->d_name);
            if (load_wav(p, &in->z[in->nz])) { in->z[in->nz].root = r; in->nz++; }
        }
        if (d) closedir(d);
    }
    if (!in->nz) { free(in); return NULL; }
    map_zones(in);
    inst_analyse(in);
    return in;
}

/* A built-in tone for when no sound folder is found (and for the offline test): a soft, decaying electric-piano-ish
 * note at C3, two seconds. */
static inst_t *make_fallback(void) {
    inst_t *in = calloc(1, sizeof(inst_t));
    if (!in) return NULL;
    snprintf(in->name, sizeof in->name, "Fallback Tone");
    zone_t *z = &in->z[0];
    z->root = 60; z->nch = 1; z->len = 88200; z->rate = 1.0f;
    z->data = malloc((size_t)z->len * sizeof(int16_t));
    if (!z->data) { free(in); return NULL; }
    float f = note_hz(60);
    for (int i = 0; i < z->len; i++) {
        float t = i / SR, env = expf(-t * 2.2f) * (1 - expf(-t * 400));
        float x = sinf(2 * PI * f * t + 1.2f * expf(-t * 6) * sinf(2 * PI * f * t)) + 0.2f * sinf(4 * PI * f * t) * expf(-t * 5);
        z->data[i] = (int16_t)lrintf(clampf(x * env * 0.5f, -1, 1) * 32767);
    }
    in->nz = 1;
    in->modes = 7;
    in->rom_idx = -1;   /* no drawn picture: the column bars */
    snprintf(in->desc, sizeof in->desc, "No sound folder found: add the rom folder next to the plugin");
    map_zones(in);
    inst_analyse(in);
    return in;
}

/* ---- sound list ---- */
static int entry_cmp(const void *a, const void *b) { return strcasecmp(((const entry_t *)a)->path, ((const entry_t *)b)->path); }

/* a rom folder's info.txt: modes=<cinematic,dark,...> and desc=<one line> */
static void read_info(const char *dir, entry_t *en) {
    char p[600], line[256];
    snprintf(p, sizeof p, "%s/info.txt", dir);
    en->modes = 7;
    FILE *f = fopen(p, "r");
    if (!f) return;
    int m = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncasecmp(line, "desc=", 5)) { snprintf(en->desc, sizeof en->desc, "%s", line + 5); continue; }
        if (strncasecmp(line, "modes=", 6)) continue;
        char *save = NULL;
        for (char *t = strtok_r(line + 6, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
            for (int i = 0; i < 3; i++) if (!strcasecmp(t, OPT_MODE[i])) m |= 1 << i;
    }
    fclose(f);
    if (m) en->modes = m;
}

/* rom: folders "NN Category - Name" (the number orders them and is not shown). User: folders and loose WAVs. */
static int scan_dir(const char *dir, int user, entry_t *out, int n, int max) {
    DIR *d = opendir(dir);
    if (!d) return n;
    int first = n;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;
        entry_t *en = &out[n];
        snprintf(en->path, sizeof en->path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(en->path, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!isdir && !(user && has_wav_ext(de->d_name))) continue;
        const char *nm = de->d_name;
        if (!user) { while (isdigit((unsigned char)*nm)) nm++; while (*nm == ' ' || *nm == '_') nm++; }
        char base[64];
        snprintf(base, sizeof base, "%s", *nm ? nm : de->d_name);
        if (!isdir) { char *dot = strrchr(base, '.'); if (dot) *dot = 0; }
        snprintf(en->name, sizeof en->name, "%s%s", user ? "User - " : "", base);
        en->is_file = !isdir;
        en->desc[0] = 0;
        en->modes = 7;
        if (!user && isdir) read_info(en->path, en);
        else snprintf(en->desc, sizeof en->desc, isdir ? "Your own multisample, from the User folder" : "Your own one-shot, from the User folder");
        n++;
    }
    closedir(d);
    qsort(out + first, (size_t)(n - first), sizeof(entry_t), entry_cmp);
    for (int k = first; k < n; k++) out[k].rom_idx = user ? -1 : k - first;   /* folder order, as make_wave_images.py draws them */
    return n;
}

static void build_view(noir_t *e) {
    int m = 1 << opt(e, P_MODE);
    e->nview = 0;
    for (int i = 0; i < e->nlist && e->nview < VIEW_MAX; i++)
        if (e->list[i].modes & m) e->view[e->nview++] = i;
}

static const entry_t *selected(const noir_t *e) {
    if (!e->nview) return NULL;
    int i = (int)lrintf(e->val[P_SOUND]);
    if (i < 0) i = 0;
    return &e->list[e->view[i % e->nview]];
}

static int find_in_view(const noir_t *e, const char *name) {
    for (int i = 0; i < e->nview; i++) if (!strcmp(e->list[e->view[i]].name, name)) return i;
    return -1;
}

/* asks the worker for the selected sound (call with e->mu held) */
static void request_load(noir_t *e) {
    const entry_t *en = selected(e);
    e->req = en ? (int)(en - e->list) : -1;
    if (en) snprintf(e->sname, sizeof e->sname, "%s", en->name);
    e->req_gen++;
    pthread_cond_signal(&e->cv);
}

static void *worker(void *arg) {
    noir_t *e = arg;
    pthread_mutex_lock(&e->mu);
    while (!e->quit) {
        if (e->retired) { inst_t *r = e->retired; e->retired = NULL; pthread_mutex_unlock(&e->mu); free_inst(r); pthread_mutex_lock(&e->mu); continue; }
        if (e->rescan_req) {   /* rescan: a new list, the same sound kept by name */
            e->rescan_req = 0;
            entry_t *nl = calloc(MAX_SOUNDS * 2, sizeof(entry_t));
            if (nl) {
                pthread_mutex_unlock(&e->mu);
                int n = scan_dir(e->rom_dir, 0, nl, 0, MAX_SOUNDS * 2);
                n = scan_dir(e->user_dir, 1, nl, n, MAX_SOUNDS * 2);
                pthread_mutex_lock(&e->mu);
                free(e->list); e->list = nl; e->nlist = n;
                char keep[64]; snprintf(keep, sizeof keep, "%s", e->sname);
                build_view(e);
                int i = find_in_view(e, keep);
                if (i >= 0 && i != (int)lrintf(e->val[P_SOUND])) { e->val[P_SOUND] = (float)i; mark_changed(e, P_SOUND); }
                request_load(e);
            }
            continue;
        }
        if (e->req_gen != e->done_gen && !e->pending) {
            /* debounce: wait until the request has stood still for 120 ms (a Q-Link sweep loads only where it stops) */
            int gen = e->req_gen;
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 120000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (gen > 1) pthread_cond_timedwait(&e->cv, &e->mu, &ts);
            if (e->quit) break;
            if (gen != e->req_gen) continue;
            int idx = e->req;
            entry_t en;
            if (idx >= 0 && idx < e->nlist) en = e->list[idx];
            e->loading = 1;
            snprintf(e->load_name, sizeof e->load_name, "%s", idx >= 0 ? en.name : "");
            pthread_mutex_unlock(&e->mu);
            inst_t *in = idx >= 0 ? load_entry(&en) : NULL;
            pthread_mutex_lock(&e->mu);
            e->loading = 0;
            e->done_gen = gen;
            if (in) e->pending = in;
            continue;
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&e->cv, &e->mu, &ts);
    }
    pthread_mutex_unlock(&e->mu);
    return NULL;
}

/* ---- reverb setup ---- */
static const float FDN_BASE[FDN_N] = { 1031, 1327, 1523, 1871, 2053, 2311, 2593, 2903 };
static const float RV_SCALE[3] = { 0.32f, 0.55f, 1.45f };   /* room, plate, warehouse */
static const float RV_RT[3] = { 0.55f, 1.9f, 3.4f };         /* seconds */
static const float RV_PRE[3] = { 0.006f, 0.018f, 0.035f };
static const int AP_LEN[4] = { 142, 107, 379, 277 };

/* the reverb decay Shadow and Shimmer need so their ghosts ring out: 1.8 .. 4.8 s by Ghost Tail, 0 when both are off */
static float ghost_rt(const noir_t *e) {
    return e->val[P_SHADOW] > 0.1f || e->val[P_SHIMMER] > 0.1f ? 1.8f + 3.0f * e->val[P_GHOST_TAIL] / 100.0f : 0;
}

static void reverb_setup(noir_t *e) {
    int t = opt(e, P_SPACE_TYPE), m = opt(e, P_MODE);
    float rt = fmaxf(RV_RT[t] * VOICING[m].rt_k, ghost_rt(e));   /* Shadow or Shimmer: at least their own long decay */
    for (int i = 0; i < FDN_N; i++) {
        e->fdn_len[i] = FDN_BASE[i] * RV_SCALE[t];
        e->fdn_g[i] = powf(10.0f, -3.0f * e->fdn_len[i] / (rt * SR));
    }
    float damp = t == 1 ? VOICING[m].damp_hz * 1.3f : VOICING[m].damp_hz;
    e->fdn_damp = 1 - expf(-2 * PI * damp / SR);
    e->pre_d = RV_PRE[t] * SR;
    e->er_on = t != 1;
    e->ap_g = t == 1 ? 0.72f : 0.6f;
    e->rv_type_last = t; e->rv_mode_last = m; e->rv_ghost_rt = ghost_rt(e);
}

/* ---- engine ---- */
static void *create(const char *data_dir) {
    (void)data_dir;
    noir_t *e = calloc(1, sizeof(noir_t));
    if (!e) return NULL;
    for (int i = 0; i < NP; i++) e->val[i] = PDEF[i];
    e->bpm = 90; e->rng = 0x9E3779B9u ^ (unsigned)(uintptr_t)e; e->last_touched = -1;
    e->lim_g = 1; e->sq_gain = 1; e->fc_s = cutoff_hz(PDEF[P_CUTOFF]); e->tp_d_last = -1; e->tf_last[0] = -1; e->to_last[0] = -1;
    e->echo_t_s = echo_time_s(PDEF[P_ECHO_TIME]) * SR;
    char dir[512];
    if (!mpc_plugin_dir(dir, sizeof dir)) snprintf(dir, sizeof dir, ".");
    snprintf(e->rom_dir, sizeof e->rom_dir, "%s/rom", dir);
    snprintf(e->user_dir, sizeof e->user_dir, "%s/User", dir);
    e->list = calloc(MAX_SOUNDS * 2, sizeof(entry_t));
    if (e->list) {   /* listing folders is quick; the WAVs load on the worker */
        e->nlist = scan_dir(e->rom_dir, 0, e->list, 0, MAX_SOUNDS * 2);
        e->nlist = scan_dir(e->user_dir, 1, e->list, e->nlist, MAX_SOUNDS * 2);
    }
    build_view(e);
    e->fallback = make_fallback();
    e->cur = e->fallback;
    reverb_setup(e);
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    pthread_mutex_lock(&e->mu);
    request_load(e);
    pthread_mutex_unlock(&e->mu);
    if (pthread_create(&e->th, NULL, worker, e)) e->th = 0;
    return e;
}

static void destroy(void *inst) {
    noir_t *e = inst;
    pthread_mutex_lock(&e->mu);
    e->quit = 1;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
    if (e->th) pthread_join(e->th, NULL);
    if (e->cur != e->fallback) free_inst(e->cur);
    if (e->pending != e->fallback) free_inst(e->pending);
    if (e->retired != e->fallback) free_inst(e->retired);
    free_inst(e->fallback);
    free(e->list);
    pthread_mutex_destroy(&e->mu);
    pthread_cond_destroy(&e->cv);
    free(e);
}

static const zone_t *zone_for(const inst_t *in, int note) {
    if (!in) return NULL;
    for (int i = 0; i < in->nz; i++) if (note >= in->z[i].lo && note <= in->z[i].hi) return &in->z[i];
    return in->nz ? &in->z[0] : NULL;
}

static void voice_release(noir_t *e, voice_t *v) {
    if (v->released) return;
    v->released = 1;
    if (e->val[P_KEYOFF] > 0.5f) {   /* the mechanical key-off: a damper thump on the release */
        for (int i = 0; i < NKO; i++) if (!e->ko[i].active) {
            e->ko[i].active = 1; e->ko[i].t = 0; e->ko[i].nz = 0;
            e->ko[i].amp = e->val[P_KEYOFF] / 100.0f * 0.14f * (0.4f + 0.6f * v->vel_g);
            break;
        }
    }
}

static void note_on(noir_t *e, int note, int vel) {
    const zone_t *z = zone_for(e->cur, note);
    if (!z) return;
    int prev = e->last_note > 0 ? e->last_note : note;   /* a glide starts from the last note played */
    float glide = e->val[P_GLIDE];
    if (opt(e, P_VOICE_MODE) == 1) {   /* MONO: a held voice slides to the new note without restarting (legato) */
        for (int i = 0; i < NV; i++) {
            voice_t *v = &e->v[i];
            if (!v->active || v->released || !(v->held || v->sustained)) continue;
            v->goff = glide > 0.001f ? v->goff + (float)(v->note - note) : 0;
            v->note = note; v->held = 1;
            for (int k = 0; k < NV; k++) if (k != i && e->v[k].active && !e->v[k].released) voice_release(e, &e->v[k]);
            e->last_note = note; e->last_vel = vel;
            e->trig_frames = (long)(0.15f * SR);
            return;
        }
        for (int i = 0; i < NV; i++) if (e->v[i].active && !e->v[i].released) voice_release(e, &e->v[i]);
    }
    for (int i = 0; i < NV; i++)   /* the same key again: the old one lets go */
        if (e->v[i].active && e->v[i].note == note && !e->v[i].released) { e->v[i].held = 0; e->v[i].sustained = 0; voice_release(e, &e->v[i]); }
    voice_t *v = NULL;
    for (int i = 0; i < NV && !v; i++) if (!e->v[i].active) v = &e->v[i];
    if (!v) {   /* steal: the oldest released voice, else the oldest */
        unsigned best = 0xffffffffu;
        for (int i = 0; i < NV; i++) if (e->v[i].released && e->v[i].age < best) { best = e->v[i].age; v = &e->v[i]; }
        if (!v) for (int i = 0; i < NV; i++) if (e->v[i].age < best) { best = e->v[i].age; v = &e->v[i]; }
    }
    memset(v, 0, sizeof *v);
    v->active = 1; v->held = 1; v->note = note; v->z = z; v->age = ++e->age;
    e->last_note = note; e->last_vel = vel;
    e->trig_frames = (long)(0.15f * SR);   /* the TRIG lamp */
    if (!e->nactive) e->pulse_ph = 0;   /* Pulse starts on the beat with the first note */
    float vs = e->val[P_VELSENS] / 100.0f, vn = vel / 127.0f;
    v->vel_g = (1 - vs) + vs * powf(vn, 1.6f);
    float tone = 1 - vs + vs * vn;
    v->tone_a = 1 - expf(-2 * PI * (1500.0f + 18500.0f * tone * tone) / SR);
    float at = attack_s(e->val[P_ATTACK]);
    v->att_step = 1.0f / (at * SR);
    v->env = e->val[P_ATTACK] < 0.001f ? 1.0f : 0.0f;
    v->gate = e->val[P_CHOP] > 0.001f ? (int)(chop_s(e->val[P_CHOP]) * SR) : -1;
    v->goff = glide > 0.001f ? (float)(prev - note) : 0;
    /* Spread: voices alternate left and right, a little apart each time */
    float sp = e->val[P_SPREAD] / 100.0f * ((e->age & 1) ? 1.0f : -1.0f) * (0.6f + 0.4f * fabsf(frand(e)));
    v->pan_l = 1.0f - fmaxf(0, sp); v->pan_r = 1.0f + fminf(0, sp);
    /* Sample Start: skip into the sound (a looped sound wraps into its loop) */
    if (e->val[P_START] > 0.01f) {
        v->pos = (double)(e->val[P_START] / 100.0f) * 0.95 * (z->len - 2);
        if (z->loop_end > 0) while (v->pos >= z->loop_end) v->pos -= z->loop_end - z->loop_start;
    }
}

static void note_off(noir_t *e, int note) {
    for (int i = 0; i < NV; i++) {
        voice_t *v = &e->v[i];
        if (!v->active || v->note != note || !v->held) continue;
        v->held = 0;
        if (e->sustain) v->sustained = 1;
        else voice_release(e, v);
    }
}

static void midi(void *inst, const uint8_t *msg, int len) {
    noir_t *e = inst;
    if (len < 2) return;
    int st = msg[0] & 0xf0, d1 = msg[1] & 0x7f, d2 = len > 2 ? msg[2] & 0x7f : 0;
    if (st == 0x90 && d2) note_on(e, d1, d2);
    else if (st == 0x80 || st == 0x90) note_off(e, d1);
    else if (st == 0xE0) e->bend = ((d2 << 7 | d1) - 8192) / 8192.0f;   /* -1..1, times Bend Range at use */
    else if (st == 0xB0) {
        if (d1 == 1) e->modwheel = d2 / 127.0f;
        else if (d1 == 64) {
            e->sustain = d2 >= 64;
            if (!e->sustain) for (int i = 0; i < NV; i++) if (e->v[i].active && e->v[i].sustained) { e->v[i].sustained = 0; voice_release(e, &e->v[i]); }
        } else if (d1 == 120 || d1 == 123) {
            for (int i = 0; i < NV; i++) if (e->v[i].active) { e->v[i].held = 0; voice_release(e, &e->v[i]); if (d1 == 120) e->v[i].active = 0; }
        }
    }
}

static int key_index(const char *k) { for (int i = 0; i < NP; i++) if (!strcmp(KEYS[i], k)) return i; return -1; }

static void load_preset(noir_t *e, int n);

/* The preset a switch to mode m loads: the first of that mode in the current category with a different sound (so the
 * switch is heard), else the first of that mode in the category, else the first of that mode anywhere; -1 if none.
 * Going back finds the first again, so A -> B -> A returns to the same preset. */
static int mode_preset(const noir_t *e, int m) {
    int cat = opt(e, P_CATEGORY), any_cat = -1, any = -1;
    for (int n = 0; n < NOIR_NPRESETS; n++) {
        if (NOIR_PRESETS[n].mode != m) continue;
        if (any < 0) any = n;
        if (NOIR_PRESETS[n].cat != cat) continue;
        if (any_cat < 0) any_cat = n;
        if (strcmp(NOIR_PRESETS[n].sound, e->sname)) return n;
    }
    return any_cat >= 0 ? any_cat : any;
}

static void set_value(noir_t *e, int i, float x) {
    if (is_text(i) || is_visual(i) || i == P_SOUND_PREV || i == P_SOUND_NEXT) return;   /* sound arrows: the wrapper steps "sound" itself */
    x = clampf(x, PMIN[i], PMAX[i]);
    if (i == P_SOUND || i == P_PRESET) x = roundf(x);
    float old = e->val[i];
    e->val[i] = x;
    if (i == P_RESCAN) { if (x > 0.5f) { pthread_mutex_lock(&e->mu); e->rescan_req = 1; pthread_cond_signal(&e->cv); pthread_mutex_unlock(&e->mu); } e->val[i] = 0; return; }
    if (i == P_PRESET_PREV || i == P_PRESET_NEXT) {   /* step within the category, wrapping */
        e->val[i] = 0;
        if (x < 0.5f) return;
        int cat = opt(e, P_CATEGORY), cur = opt(e, P_PRESET), d = i == P_PRESET_NEXT ? 1 : -1;
        for (int k = 1; k <= NOIR_NPRESETS; k++) {
            int n = ((cur + d * k) % NOIR_NPRESETS + NOIR_NPRESETS) % NOIR_NPRESETS;
            if (NOIR_PRESETS[n].cat == cat) { load_preset(e, n); break; }
        }
        return;
    }
    if (e->restoring && (i == P_PRESET || i == P_CATEGORY)) return;   /* a saved state keeps the name, not the preset's values */
    if (i == P_PRESET) { load_preset(e, (int)x); return; }
    if (i == P_CATEGORY) {   /* a category tap: its first preset */
        if (opt(e, P_CATEGORY) == (int)lrintf(old)) return;
        for (int n = 0; n < NOIR_NPRESETS; n++) if (NOIR_PRESETS[n].cat == opt(e, P_CATEGORY)) { load_preset(e, n); break; }
        return;
    }
    if (i == P_SOUND && x != old) { pthread_mutex_lock(&e->mu); request_load(e); pthread_mutex_unlock(&e->mu); }
    if (i == P_MODE && opt(e, P_MODE) != (int)lrintf(old)) {
        /* a mode tap loads a preset of that mode in the current category, so the sound and the knobs change with it */
        if (!e->restoring) { int n = mode_preset(e, opt(e, P_MODE)); if (n >= 0) { load_preset(e, n); return; } }
        /* a preset or saved state: the browser follows the mode, the sound stays if listed */
        pthread_mutex_lock(&e->mu);
        build_view(e);
        int k = find_in_view(e, e->sname);
        e->val[P_SOUND] = (float)(k >= 0 ? k : 0);
        mark_changed(e, P_SOUND);
        request_load(e);
        pthread_mutex_unlock(&e->mu);
    }
}

/* "key=value;...;sname=<name>" (a saved state or a preset); unknown keys are skipped */
static void apply_state(noir_t *e, const char *val) {
    char buf[4096], want[64] = "";
    snprintf(buf, sizeof buf, "%s", val);
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(t, "sname")) { snprintf(want, sizeof want, "%s", eq + 1); continue; }
        int i = key_index(t);
        if (i >= 0 && !is_trigger(i)) set_value(e, i, (float)atof(eq + 1));
    }
    if (want[0]) {   /* the sound by name: positions shift as sounds are added */
        pthread_mutex_lock(&e->mu);
        snprintf(e->sname, sizeof e->sname, "%s", want);
        int k = find_in_view(e, want);
        if (k >= 0) { e->val[P_SOUND] = (float)k; request_load(e); }
        pthread_mutex_unlock(&e->mu);
    }
    e->last_touched = -1;
}

static void load_preset(noir_t *e, int n) {
    if (n < 0 || n >= NOIR_NPRESETS) return;
    e->restoring = 1;
    apply_state(e, NOIR_PRESETS[n].state);
    e->restoring = 0;
    e->val[P_PRESET] = (float)n;
    e->val[P_CATEGORY] = (float)NOIR_PRESETS[n].cat;
    for (int i = 0; i < NP; i++) if (!is_text(i) && !is_trigger(i) && !is_visual(i)) mark_changed(e, i);   /* the screen follows */
    e->dirty = 0;
}

static void set_param(void *inst, const char *key, const char *val) {
    noir_t *e = inst;
    if (!strcmp(key, "lfo_bpm")) { float b = (float)atof(val); if (b > 20 && b < 400) e->bpm = b; return; }
    if (!strcmp(key, "state")) {
        e->restoring = 1;
        apply_state(e, val);
        e->restoring = 0;
        e->dirty = strstr(val, "dirty=1") != NULL;
        return;
    }
    int i = key_index(key);
    if (i < 0) return;
    float old = e->val[i];
    set_value(e, i, (float)atof(val));
    if (is_text(i) || is_trigger(i) || is_visual(i) || i == P_PRESET || i == P_CATEGORY) return;
    e->last_touched = i;
    if (e->val[i] != old) e->dirty = 1;
}

static int fmt_time(char *buf, int len, float s) {
    return s < 1.0f ? snprintf(buf, len, "%.0f ms", s * 1000) : snprintf(buf, len, "%.2f s", s);
}

static int display(noir_t *e, int i, char *buf, int len) {
    float x = e->val[i];
    switch (i) {
    case P_CUTOFF: { float hz = cutoff_hz(x); return hz < 1000 ? snprintf(buf, len, "%.0f Hz", hz) : snprintf(buf, len, "%.1f kHz", hz / 1000); }
    case P_ATTACK: return fmt_time(buf, len, attack_s(x));
    case P_RELEASE: return fmt_time(buf, len, release_s(x));
    case P_CHOP: return x < 0.001f ? snprintf(buf, len, "OFF") : fmt_time(buf, len, chop_s(x));
    case P_CRUSH: return x < 0.001f ? snprintf(buf, len, "OFF") : snprintf(buf, len, "%.0f-bit %.1fk", crush_bits(x), crush_rate(x) / 1000);
    case P_SOUND: {
        pthread_mutex_lock(&e->mu);
        const entry_t *en = selected(e);
        int n = snprintf(buf, len, "%s", en ? en->name : "No sounds found");
        pthread_mutex_unlock(&e->mu);
        return n;
    }
    case P_STUTTER: return snprintf(buf, len, "%s", OPT_STUTTER[opt(e, i)]);
    case P_MODE: return snprintf(buf, len, "%s", OPT_MODE[opt(e, i)]);
    case P_SPACE_TYPE: return snprintf(buf, len, "%s", OPT_SPACE[opt(e, i)]);
    case P_SPEED: return snprintf(buf, len, "%s", OPT_SPEED[opt(e, i)]);
    case P_OCTDROP: return snprintf(buf, len, "%s", OPT_OCT[opt(e, i)]);
    case P_VOLUME: return snprintf(buf, len, "%+.1f dB", x);
    case P_PRESET: return snprintf(buf, len, "%s%s", NOIR_PRESETS[opt(e, i)].name, e->dirty ? " *" : "");
    case P_CATEGORY: return snprintf(buf, len, "%s", OPT_CAT[opt(e, i)]);
    case P_VY_FOLLOW: return snprintf(buf, len, "%s", OPT_VYF[opt(e, i)]);
    case P_SQ_STYLE: return snprintf(buf, len, "%s", OPT_SQS[opt(e, i)]);
    case P_MONO_FREQ: return snprintf(buf, len, "%.0f Hz", mono_hz(x));
    case P_SQ_RELEASE: return fmt_time(buf, len, squash_rel_s(x));
    case P_TRANSPOSE: return snprintf(buf, len, "%+d st", (int)lrintf(x));
    case P_FINE: return snprintf(buf, len, "%+.0f ct", x);
    case P_DECAY: return e->val[P_SUSTAIN] > 99.5f ? snprintf(buf, len, "HOLD") : fmt_time(buf, len, decay_s(x));
    case P_GLIDE: return x < 0.001f ? snprintf(buf, len, "OFF") : fmt_time(buf, len, glide_s(x));
    case P_BEND_RANGE: return snprintf(buf, len, "+/-%d st", (int)lrintf(x));
    case P_VOICE_MODE: return snprintf(buf, len, "%s", opt(e, i) ? "MONO" : "POLY");
    case P_TAPE_LOW: return x < 0.005f ? snprintf(buf, len, "OFF") : snprintf(buf, len, "%.0f Hz", tape_low_hz(x));
    case P_TAPE_HIGH: return x > 0.995f ? snprintf(buf, len, "OFF") : snprintf(buf, len, "%.1f kHz", tape_high_hz(x) / 1000);
    case P_TILT: return fabsf(x - 50) < 2 ? snprintf(buf, len, "FLAT") : snprintf(buf, len, "%s %.1f dB", x < 50 ? "DARK" : "BRIGHT", fabsf(x - 50) / 50 * 6);
    case P_ECHO_TIME: return fmt_time(buf, len, echo_time_s(x));
    case P_ECHO_TONE: return snprintf(buf, len, "%.1f kHz", echo_tone_hz(x) / 1000);
    case P_PULSE_RATE: { static const char *const PR[4] = { "1/2", "1/4", "1/8", "1/16" }; return snprintf(buf, len, "%s", PR[opt(e, i)]); }
    default: return snprintf(buf, len, "%.0f%%", x);
    }
}

static int get_param(void *inst, const char *key, char *buf, int len) {
    noir_t *e = inst;
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int i = 0; i < NP && n < len; i++) {
            if (is_text(i) || is_trigger(i) || is_visual(i)) continue;
            n += snprintf(buf + n, (size_t)(len - n), "%s=%g;", KEYS[i], e->val[i]);
        }
        pthread_mutex_lock(&e->mu);
        if (n < len) n += snprintf(buf + n, (size_t)(len - n), "dirty=%d;sname=%s;", e->dirty, e->sname);
        pthread_mutex_unlock(&e->mu);
        return n < len ? n : 0;
    }
    if (!strncmp(key, "preset:", 7)) {   /* MPC's PRESET menu: "Category: Name", grouped */
        int n = atoi(key + 7);
        if (n < 0 || n >= NOIR_NPRESETS) return 0;
        static const char *const CATN[8] = { "Keys", "Strings", "Choir", "Brass", "Pads", "Synth", "Texture", "Leads" };
        return snprintf(buf, len, "%s: %s", CATN[NOIR_PRESETS[n].cat], NOIR_PRESETS[n].name);
    }
    if (!strcmp(key, "disp_desc")) {   /* the sound's name and character */
        const inst_t *in = e->cur;
        if (!in) return snprintf(buf, len, "NO SOUND");
        const char *nm = strstr(in->name, " - ") ? strstr(in->name, " - ") + 3 : in->name;
        return snprintf(buf, len, "%s:  %s", nm, in->desc);
    }
    if (!strcmp(key, "display_rev")) {   /* HAS_DISPLAY_REV: the waveform, playhead and lamp the host should show */
        unsigned char nv[NWCOL + 3];
        const inst_t *in = e->cur;
        /* a rom sound shows its drawn waveform picture, dim while idle and lit while a note plays; anything else
         * (User sounds, the fallback) the column bars */
        int img = in && in->rom_idx >= 0 && in->rom_idx < WAVE_IMGS ? in->rom_idx + 1 + (e->head_on ? WAVE_IMGS : 0) : 0;
        for (int c = 0; c < NWCOL; c++) nv[c] = in && !img ? in->wave[c] : 0;
        nv[NWCOL] = (unsigned char)(e->head_on ? 1 + (int)fminf(NWCOL - 1, fmaxf(0, e->head_frac * NWCOL)) : 0);
        nv[NWCOL + 1] = e->trig_frames > 0;
        nv[NWCOL + 2] = (unsigned char)img;
        if (memcmp(nv, e->vis, sizeof nv) || e->vis_inst != in) { memcpy(e->vis, nv, sizeof nv); e->vis_inst = in; e->disp_rev++; }
        return snprintf(buf, len, "%u", e->disp_rev);
    }
    if (!strcmp(key, "disp_info")) {   /* the loaded sound: zones, range, stereo, loop, length, memory, modes */
        const inst_t *in = e->cur;
        if (!in || !in->nz) return snprintf(buf, len, "NO SOUND");
        char lo[8], hi[8], md[40] = "";
        note_name(in->z[0].root, lo, sizeof lo); note_name(in->z[in->nz - 1].root, hi, sizeof hi);
        int st = 0, lp = 0;
        for (int z = 0; z < in->nz; z++) { st |= in->z[z].nch == 2; lp |= in->z[z].loop_end > 0; }
        for (int m = 0; m < 3; m++) if (in->modes & (1 << m)) { if (md[0]) strcat(md, " + "); strcat(md, OPT_MODE[m]); }
        return snprintf(buf, len, "%d ZONES %s-%s   %s   %s   %.1f s   %.1f MB   %s", in->nz, lo, hi, st ? "STEREO" : "MONO",
                        lp ? "LOOPED" : "ONE-SHOT", in->max_sec, in->mb, in->modes == 7 ? "ALL MODES" : md);
    }
    if (!strcmp(key, "disp_fx")) {
        char cut[16], cr[24];
        display(e, P_CUTOFF, cut, sizeof cut); display(e, P_CRUSH, cr, sizeof cr);
        return snprintf(buf, len, "TAPE %.0f%%   DRIFT %.0f%%   %s %.0f%%   CRUSH %s   CUT %s", e->val[P_DRIVE], e->val[P_DRIFT],
                        OPT_SPACE[opt(e, P_SPACE_TYPE)], e->val[P_SPACE], cr, cut);
    }
    if (!strcmp(key, "disp_fx2")) {
        return snprintf(buf, len, "VINYL %.0f%%   WIDTH %.0f%%   SQUASH %.0f%% %s   SUB %.0f%%   %s RPM", e->val[P_VINYL],
                        e->val[P_WIDTH], e->val[P_SQUASH], OPT_SQS[opt(e, P_SQ_STYLE)], e->val[P_SUB],
                        opt(e, P_SPEED) == 2 ? "HALF" : OPT_SPEED[opt(e, P_SPEED)]);
    }
    if (!strcmp(key, "disp_play")) {   /* what is playing, live */
        if (!e->nactive) return snprintf(buf, len, "READY   %s MODE", OPT_MODE[opt(e, P_MODE)]);
        char nn[8], zn[8]; note_name(e->last_note, nn, sizeof nn); note_name(e->head_root, zn, sizeof zn);
        return snprintf(buf, len, "%s  VEL %d   ZONE %s   %d VOICE%s   %s MODE", nn, e->last_vel, zn, e->nactive,
                        e->nactive == 1 ? "" : "S", OPT_MODE[opt(e, P_MODE)]);
    }
    if (!strcmp(key, "squash_gr")) return snprintf(buf, len, e->val[P_SQUASH] < 0.5f ? "SQUASH OFF" : "GR -%.1f dB", e->gr_hold);
    if (!strcmp(key, "status")) {
        pthread_mutex_lock(&e->mu);
        int loading = e->loading || e->req_gen != e->done_gen;
        char ln[64]; snprintf(ln, sizeof ln, "%s", e->sname);
        int empty = e->nview == 0;
        pthread_mutex_unlock(&e->mu);
        if (empty) return snprintf(buf, len, "NO SOUNDS FOUND: ADD THE ROM FOLDER");
        if (loading) return snprintf(buf, len, "LOADING  %s", ln);
        if (e->last_touched >= 0 && NAMES[e->last_touched] && NAMES[e->last_touched][0]) {
            char d[64]; display(e, e->last_touched, d, sizeof d);
            return snprintf(buf, len, "%s  %s", NAMES[e->last_touched], d);
        }
        return snprintf(buf, len, "%s", ln);
    }
    size_t kl = strlen(key);
    if (kl > 8 && !strcmp(key + kl - 8, "_display")) {
        char k[32];
        snprintf(k, sizeof k, "%.*s", (int)(kl - 8), key);
        int i = key_index(k);
        return i >= 0 ? display(e, i, buf, len) : 0;
    }
    int i = key_index(key);
    if (i < 0) return 0;
    if (is_visual(i)) return snprintf(buf, len, "%d", e->vis[i - P_W0]);   /* as of the last display_rev */
    return snprintf(buf, len, "%g", e->val[i]);
}

int mpc_engine_changed(void *inst, char *key, int len) {   /* one moved parameter per call, lowest index first */
    noir_t *e = inst;
    for (int w = 0; w < 2; w++) {
        uint64_t c = __atomic_load_n(&e->changed[w], __ATOMIC_RELAXED);   /* the worker sets bits too (a rescan) */
        if (!c) continue;
        int b = __builtin_ctzll(c);
        __atomic_and_fetch(&e->changed[w], ~(1ull << b), __ATOMIC_RELAXED);
        snprintf(key, (size_t)len, "%s", KEYS[w * 64 + b]);
        return 1;
    }
    return 0;
}

/* ---- voices ---- */
static inline float zone_read(const zone_t *z, double pos, int c) {
    int i = (int)pos;
    float fr = (float)(pos - i);
    int j = i + 1;
    if (z->loop_end > 0 && j >= z->loop_end) j = z->loop_start + (j - z->loop_end);
    if (j >= z->len) j = z->len - 1;
    int cc = z->nch == 2 ? c : 0;
    float a = z->data[i * z->nch + cc], b = z->data[j * z->nch + cc];
    return (a + (b - a) * fr) * (1.0f / 32768.0f);
}

/* advances a play position; returns 0 once a one-shot runs off the end */
static inline int zone_step(const zone_t *z, double *pos, double inc) {
    *pos += inc;
    if (z->loop_end > 0) { while (*pos >= z->loop_end) *pos -= z->loop_end - z->loop_start; return 1; }
    return *pos < z->len - 1;
}

static inline float poly_blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}

static void render_voices(noir_t *e, float *L, float *R, int frames) {
    float tune = e->bend * e->val[P_BEND_RANGE] + e->val[P_TRANSPOSE] + e->val[P_FINE] / 100.0f;   /* semitones */
    float ratio = SPEED_RATIO[opt(e, P_SPEED)] * powf(2.0f, tune / 12.0f);
    float rel = release_s(e->val[P_RELEASE]);
    float sus = e->val[P_SUSTAIN] / 100.0f, dec_k = expf(-6.9f / (decay_s(e->val[P_DECAY]) * SR));
    float glide_k = e->val[P_GLIDE] > 0.001f ? expf(-4.6f * frames / (glide_s(e->val[P_GLIDE]) * SR)) : 0;
    float sub = e->val[P_SUB] / 100.0f;
    int oct = opt(e, P_OCTDROP);
    int stut = opt(e, P_STUTTER);
    long period = stut ? (long)(60.0f / e->bpm * STUT_BEATS[stut] * SR) : 0;
    for (int n = 0; n < NV; n++) {
        voice_t *v = &e->v[n];
        if (!v->active) continue;
        const zone_t *z = v->z;
        v->goff *= glide_k;   /* the glide eases into the note, block by block */
        if (fabsf(v->goff) < 0.001f) v->goff = 0;
        double inc = z->rate * pow(2.0, (v->note - z->root + v->goff) / 12.0) * ratio;
        float f0 = note_hz((float)v->note + v->goff) * ratio;
        float sub_inc = f0 * 0.5f / SR;
        float sub_fade = clampf((84.0f - v->note) / 12.0f, 0, 1);   /* no sub above C6 */
        float rel_k = expf(-6.9f / ((v->gate == 0 ? fminf(rel, 0.04f) : rel) * SR));
        long sc = e->stut_ctr;
        for (int i = 0; i < frames; i++) {
            if (period && !v->released && sc > 0 && sc % period == 0) {   /* stutter: back to the start, crossfaded */
                v->opos = v->pos; v->pos = 0; v->dpos = 0; v->xf = XF; v->done_main = v->done_dbl = 0;
            }
            sc++;
            if (v->gate > 0 && --v->gate == 0) { voice_release(e, v); rel_k = expf(-6.9f / (fminf(rel, 0.04f) * SR)); }
            if (v->released) v->env *= rel_k;
            else if (!v->decaying) { v->env += v->att_step; if (v->env >= 1) { v->env = 1; v->decaying = 1; } }
            else if (sus < 0.999f) v->env = sus + (v->env - sus) * dec_k;   /* decay to the sustain level */
            float l = 0, r = 0;
            if (!v->done_main) {
                l = zone_read(z, v->pos, 0); r = zone_read(z, v->pos, 1);
                if (v->xf) {
                    float k = (float)v->xf / XF;
                    l = l * (1 - k) + zone_read(z, v->opos, 0) * k;
                    r = r * (1 - k) + zone_read(z, v->opos, 1) * k;
                    v->xf--;
                    zone_step(z, &v->opos, inc);
                }
                if (!zone_step(z, &v->pos, inc)) v->done_main = 1;
            }
            if (oct == 2 && !v->done_dbl) {   /* the same sample at half speed */
                l += 0.7f * zone_read(z, v->dpos, 0); r += 0.7f * zone_read(z, v->dpos, 1);
                if (!zone_step(z, &v->dpos, inc * 0.5)) v->done_dbl = 1;
            } else if (oct != 2) v->done_dbl = 1;
            /* velocity tone */
            v->tz[0] += v->tone_a * (l - v->tz[0]); v->tz[1] += v->tone_a * (r - v->tz[1]);
            l = v->tz[0]; r = v->tz[1];
            float lowl = 0;
            if (sub > 0) { lowl += sub * 0.45f * sub_fade * sinf(2 * PI * v->subph); }
            if (oct == 1) {   /* band-limited square an octave down, softened */
                float sq = (v->sqph < 0.5f ? 1.0f : -1.0f) + poly_blep(v->sqph, sub_inc) - poly_blep(fmodf(v->sqph + 0.5f, 1.0f), sub_inc);
                v->sqz += 0.12f * (sq - v->sqz);
                lowl += 0.22f * v->sqz * sub_fade;
                v->sqph += sub_inc; if (v->sqph >= 1) v->sqph -= 1;
            }
            v->subph += sub_inc; if (v->subph >= 1) v->subph -= 1;
            float g = v->env * v->vel_g;
            L[i] += (l * v->pan_l + lowl) * g; R[i] += (r * v->pan_r + lowl) * g;   /* the sub layer stays centred */
            if ((v->released && v->env < 0.0002f) || (v->done_main && v->done_dbl && v->xf == 0)) { v->active = 0; break; }
        }
    }
    /* key-off thumps */
    for (int k = 0; k < NKO; k++) {
        keyoff_t *o = &e->ko[k];
        if (!o->active) continue;
        for (int i = 0; i < frames; i++) {
            float t = o->t;
            o->nz += 0.25f * (frand(e) - o->nz);
            float x = o->amp * (sinf(2 * PI * 150.0f * t) * expf(-t * 45) + 1.6f * o->nz * expf(-t * 28));
            L[i] += x; R[i] += x;
            o->t += 1.0f / SR;
        }
        if (o->t > 0.25f) o->active = 0;
    }
    if (period) e->stut_ctr += frames; else e->stut_ctr = 0;
    int na = 0;
    const voice_t *newest = NULL;
    for (int n = 0; n < NV; n++) {
        na += e->v[n].active;
        if (e->v[n].active && (!newest || e->v[n].age > newest->age)) newest = &e->v[n];
    }
    e->nactive = na;
    /* the playhead follows the newest voice through its zone, until it fades */
    e->head_on = newest && !(newest->released && newest->env < 0.05f);
    if (newest) { e->head_frac = (float)(newest->pos / newest->z->len); e->head_root = newest->z->root; }
    if (e->trig_frames > 0) e->trig_frames -= frames;
}

/* ---- the mix chain ---- */
static inline float svf_lp(float *s, float g, float k, float x) {   /* TPT state-variable filter, low-pass out */
    float hp = (x - (k + g) * s[0] - s[1]) / (1 + g * (g + k));
    float bp = g * hp + s[0]; s[0] = g * hp + bp;
    float lp = g * bp + s[1]; s[1] = g * bp + lp;
    return lp;
}

static inline float ring_read(const float *buf, int mask, int wpos, float d) {   /* linear, d frames back, size mask + 1 */
    float rp = (float)wpos - d;
    while (rp < 0) rp += mask + 1;
    int i = (int)rp; float f = rp - i;
    return buf[i & mask] + (buf[(i + 1) & mask] - buf[i & mask]) * f;
}

static inline float delay_read(const float *buf, int wpos, float d) {   /* cubic Hermite, d frames back */
    float rp = (float)wpos - d;
    while (rp < 0) rp += WOW_LEN;
    int i = (int)rp; float f = rp - i;
    float y0 = buf[(i - 1) & (WOW_LEN - 1)], y1 = buf[i & (WOW_LEN - 1)], y2 = buf[(i + 1) & (WOW_LEN - 1)], y3 = buf[(i + 2) & (WOW_LEN - 1)];
    float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

static void render(void *inst, int16_t *out_lr, int frames) {
    noir_t *e = inst;
    /* pick up a freshly loaded sound */
    if (!pthread_mutex_trylock(&e->mu)) {
        if (e->pending && !e->retired) {
            for (int i = 0; i < NV; i++) e->v[i].active = 0;
            if (e->cur != e->fallback) e->retired = e->cur;
            e->cur = e->pending; e->pending = NULL;
            pthread_cond_signal(&e->cv);
        }
        pthread_mutex_unlock(&e->mu);
    }
    float L[128], R[128];
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    render_voices(e, L, R, frames);

    const voicing_t *vc = &VOICING[opt(e, P_MODE)];
    /* per-block settings */
    float crush = e->val[P_CRUSH];
    float cr_inc = crush_rate(crush) / SR, cr_q = powf(2.0f, crush_bits(crush) - 1);
    float fc_t = clampf(cutoff_hz(e->val[P_CUTOFF]) * vc->cut_k, 30, 19000);
    e->fc_s += 0.3f * (fc_t - e->fc_s);
    float g = tanf(PI * e->fc_s / SR), k2 = 2.0f - 1.85f * e->val[P_RESO] / 100.0f;
    float d = clampf(e->val[P_DRIVE] / 100.0f * vc->drive_k, 0, 1.4f);
    if (fabsf(d - e->tp_d_last) > 0.002f) {
        biquad_peak(e->hb_c, 90.0f, 0.8f, 3.5f * d);
        biquad_lp(e->hl_c, 17000.0f - 9500.0f * fminf(d, 1.0f), 0.6f);
        e->tp_d_last = d;
    }
    float ingain = db2lin(20.0f * d), makeup = 1.0f / sqrtf(ingain), bias = 0.15f * d;
    /* tape filter: a deck's bandwidth, low cut and high cut at 12 dB/oct, with a resonant head bump just above the low cut */
    float tfl = e->val[P_TAPE_LOW], tfh = e->val[P_TAPE_HIGH], tfb = e->val[P_TAPE_BUMP];
    int tf_on = tfl > 0.005f || tfh < 0.995f || tfb > 0.5f;
    if (tfl != e->tf_last[0] || tfh != e->tf_last[1] || tfb != e->tf_last[2]) {
        float lo = tape_low_hz(tfl);
        biquad_hp(e->tf_c[0], lo, 0.707f);
        biquad_peak(e->tf_c[1], lo * 1.7f, 1.4f, 7.0f * tfb / 100.0f);
        biquad_lp(e->tf_c[2], tape_high_hz(tfh), 0.62f);
        e->tf_last[0] = tfl; e->tf_last[1] = tfh; e->tf_last[2] = tfb;
    }
    /* MOOD: tone (a tilt of two shelves at 800 Hz, warmth at 220 Hz, air above 4.5 kHz), Noir Echo, Ensemble, the ghost
     * tails (Shadow an octave down, Shimmer an octave up, fed into the reverb), Pulse */
    float tl = e->val[P_TILT], wa = e->val[P_WARMTH], air = e->val[P_AIR];
    int to_on = fabsf(tl - 50) > 1 || wa > 0.5f || air > 0.5f;
    if (tl != e->to_last[0] || wa != e->to_last[1] || air != e->to_last[2]) {
        float t = (tl - 50) / 50 * 6;
        biquad_shelf(e->to_c[0], 800, -t, 0);
        biquad_shelf(e->to_c[1], 800, t, 1);
        biquad_peak(e->to_c[2], 220, 0.8f, 6 * wa / 100);
        biquad_shelf(e->to_c[3], 4500, 9 * air / 100, 1);
        e->to_last[0] = tl; e->to_last[1] = wa; e->to_last[2] = air;
    }
    float ens = e->val[P_ENSEMBLE] / 100.0f, echo_mix = e->val[P_ECHO_MIX] / 100.0f, echo_fb = e->val[P_ECHO_FB] / 100.0f;
    e->echo_t_s += 0.2f * (echo_time_s(e->val[P_ECHO_TIME]) * SR - e->echo_t_s);   /* glides, like a tape speed change */
    float echo_lpa = 1 - expf(-2 * PI * echo_tone_hz(e->val[P_ECHO_TONE]) / SR), echo_hpa = 1 - expf(-2 * PI * 120.0f / SR);
    float shadow = e->val[P_SHADOW] / 100.0f, shimmer = e->val[P_SHIMMER] / 100.0f, gtail = e->val[P_GHOST_TAIL] / 100.0f;
    int ghost_on = shadow > 0.001f || shimmer > 0.001f;
    float ps_lpa = 1 - expf(-2 * PI * 1400.0f / SR), ps_hpa = 1 - expf(-2 * PI * 700.0f / SR);
    float pulse = e->val[P_PULSE] / 100.0f;
    double pulse_inc = e->bpm / 60.0 / PULSE_BEATS[opt(e, P_PULSE_RATE)] / SR;
    /* Cracked Vinyl FOLLOW opens while a voice sounds and closes 0.4 s after the last (not after the reverb's tail) */
    float vg_t = e->nactive > 0 ? 1.0f : 0.0f, vg_att = 1 - expf(-1.0f / (0.02f * SR)), vg_rel = 1 - expf(-1.0f / (0.08f * SR));   /* about -40 dB by 0.4 s */
    float drift = clampf(e->val[P_DRIFT] / 100.0f + 0.5f * e->modwheel, 0, 1.5f);
    float width = e->val[P_WIDTH] / 100.0f * vc->width_k;
    float side_warble = e->val[P_TAIL_WARBLE] / 100.0f;
    float space = e->val[P_SPACE] / 100.0f;
    if (opt(e, P_SPACE_TYPE) != e->rv_type_last || opt(e, P_MODE) != e->rv_mode_last || ghost_rt(e) != e->rv_ghost_rt) reverb_setup(e);
    /* Squash styles: VCA (fast, hard), GLUE (bus glue: slow attack, gentle ratio, wide knee), SPIT (brutal, then driven) */
    float sq = e->val[P_SQUASH] / 100.0f;
    int sqs = opt(e, P_SQ_STYLE);
    float sq_thr, sq_ratio, sq_att_s, sq_knee;
    if (sqs == 1) { sq_thr = -4.0f - 24.0f * sq; sq_ratio = 1.5f + 2.5f * sq + 0.3f * vc->ratio_add; sq_att_s = 0.010f; sq_knee = 10; }
    else if (sqs == 2) { sq_thr = -4.0f - 34.0f * sq; sq_ratio = 4.0f + 16.0f * sq + vc->ratio_add; sq_att_s = 0.0001f; sq_knee = 3; }
    else { sq_thr = -4.0f - 30.0f * sq; sq_ratio = 1.0f + (3.0f + vc->ratio_add) * sq * 2.0f; sq_att_s = 0.0008f; sq_knee = 6; }
    float sq_rel = expf(-1.0f / (squash_rel_s(e->val[P_SQ_RELEASE]) * SR)), sq_att = expf(-1.0f / (sq_att_s * SR));
    float sq_makeup = db2lin(-sq_thr * (1 - 1 / sq_ratio) * 0.5f), sq_mix = e->val[P_SQ_MIX] / 100.0f;
    float sq_drive = sqs == 2 ? 1.0f + 1.5f * sq : 0;
    float gr_max = 0;
    float vinyl = e->val[P_VINYL] / 100.0f;
    float vy_dust = e->val[P_VY_DUST] / 100.0f, vy_static = e->val[P_VY_STATIC] / 100.0f, vy_scr = e->val[P_VY_SCRATCH] / 100.0f;
    int vy_always = opt(e, P_VY_FOLLOW) == 1;
    float vol = db2lin(e->val[P_VOLUME]);
    float rv_lp = e->fdn_damp, hp_a = 1 - expf(-2 * PI * 180.0f / SR), shp_a = 1 - expf(-2 * PI * mono_hz(e->val[P_MONO_FREQ]) / SR);
    float env_att = 1 - expf(-1.0f / (0.003f * SR)), env_rel = 1 - expf(-1.0f / (0.15f * SR));
    float lim_rel = expf(-1.0f / (0.12f * SR));
    /* the wow's slow random drift: a new target every 0.3 s */
    if ((e->drift_ctr -= frames) <= 0) { e->drift_ctr = (int)(0.3f * SR); e->drift_tgt = frand(e); }

    for (int i = 0; i < frames; i++) {
        float l = L[i], r = R[i];
        /* crush */
        if (crush > 0.001f) {
            e->cr_ph += cr_inc;
            if (e->cr_ph >= 1) { e->cr_ph -= 1; e->cr_l = roundf(l * cr_q) / cr_q; e->cr_r = roundf(r * cr_q) / cr_q; }
            l = e->cr_l; r = e->cr_r;
        }
        /* 24 dB low-pass: a plain stage, then a resonant one with a soft clip in its feedback */
        l = svf_lp(e->svf[0][0], g, 1.414f, l); r = svf_lp(e->svf[1][0], g, 1.414f, r);
        l = svf_lp(e->svf[0][1], g, k2, l); r = svf_lp(e->svf[1][1], g, k2, r);
        e->svf[0][1][0] = fast_tanh(e->svf[0][1][0]); e->svf[1][1][0] = fast_tanh(e->svf[1][1][0]);
        /* tape */
        if (d > 0.001f) {
            float x[2] = { l * ingain, r * ingain };
            for (int c = 0; c < 2; c++) {
                float a = fabsf(x[c]);
                e->tp_env[c] += (a > e->tp_env[c] ? env_att : env_rel) * (a - e->tp_env[c]);
                float over = e->tp_env[c] - 0.5f;
                if (over > 0) x[c] /= 1 + 1.6f * d * over;   /* input-gain compression of a deck running hot */
                x[c] = fast_tanh(x[c] + bias) - fast_tanh(bias);
                x[c] = biquad(e->hb_c, e->hb[c], x[c]);
                x[c] = biquad(e->hl_c, e->hl[c], x[c]) * makeup;
            }
            float hiss = 0.0005f * d * e->vy_gate;
            l = x[0] + hiss * frand(e); r = x[1] + hiss * frand(e);
        }
        if (tf_on) {
            for (int s = 0; s < 3; s++) { l = biquad(e->tf_c[s], e->tf_z[s][0], l); r = biquad(e->tf_c[s], e->tf_z[s][1], r); }
        }
        if (to_on) {   /* MOOD tone */
            for (int s = 0; s < 4; s++) { l = biquad(e->to_c[s], e->to_z[s][0], l); r = biquad(e->to_c[s], e->to_z[s][1], r); }
        }
        /* wow & flutter on mid and side */
        float m = 0.5f * (l + r), s = 0.5f * (l - r);
        e->wm[e->wpos] = m; e->ws[e->wpos] = s;
        e->drift_rnd += 0.00002f * (e->drift_tgt - e->drift_rnd);
        e->wow_ph += 0.55f / SR; if (e->wow_ph >= 1) e->wow_ph -= 1;
        e->flut_ph += 7.3f / SR; if (e->flut_ph >= 1) e->flut_ph -= 1;
        e->side_ph += 0.83f / SR; if (e->side_ph >= 1) e->side_ph -= 1;
        float wow = sinf(2 * PI * e->wow_ph), flut = sinf(2 * PI * e->flut_ph);
        float dm = 0.008f * SR + drift * SR * (0.0042f * wow + 0.0030f * e->drift_rnd + 0.00008f * flut);
        float ds = dm + side_warble * SR * 0.0035f * sinf(2 * PI * e->side_ph);
        m = delay_read(e->wm, e->wpos, dm); s = delay_read(e->ws, e->wpos, ds);
        e->wpos = (e->wpos + 1) & (WOW_LEN - 1);
        l = m + s; r = m - s;
        /* Ensemble: three slow chorus voices, crossed between the sides for width */
        if (ens > 0.001f) {
            static const float ENS_RATE[3] = { 0.33f, 0.47f, 0.61f };
            float tap[3];
            e->ens[0][e->ens_pos] = l; e->ens[1][e->ens_pos] = r;
            for (int k = 0; k < 3; k++) {
                e->ens_ph[k] += ENS_RATE[k] / SR; if (e->ens_ph[k] >= 1) e->ens_ph[k] -= 1;
                tap[k] = ring_read(e->ens[k == 1], ENS_LEN - 1, e->ens_pos, (0.011f + 0.0045f * sinf(2 * PI * (e->ens_ph[k] + k / 3.0f))) * SR);
            }
            e->ens_pos = (e->ens_pos + 1) & (ENS_LEN - 1);
            float kd = 1 - 0.35f * ens, kw = 0.6f * ens;
            l = l * kd + kw * (tap[0] + 0.6f * tap[2]); r = r * kd + kw * (tap[1] - 0.6f * tap[2]);
        }
        /* Noir Echo: a dark tape echo, the repeats crossing sides, each one darker (Tone) and thinner (120 Hz cut) */
        if (echo_mix > 0.001f) {
            float el = ring_read(e->echo[0], ECHO_LEN - 1, e->echo_pos, e->echo_t_s);
            float er = ring_read(e->echo[1], ECHO_LEN - 1, e->echo_pos, e->echo_t_s * 1.03f);
            for (int c = 0; c < 2; c++) {
                float *x = c ? &er : &el;
                e->echo_lp[c] += echo_lpa * (*x - e->echo_lp[c]); *x = e->echo_lp[c];
                e->echo_hp[c] += echo_hpa * (*x - e->echo_hp[c]); *x -= e->echo_hp[c];
            }
            e->echo[0][e->echo_pos] = fast_tanh(l + echo_fb * er);
            e->echo[1][e->echo_pos] = fast_tanh(r + echo_fb * el);
            e->echo_pos = (e->echo_pos + 1) & (ECHO_LEN - 1);
            l += echo_mix * el; r += echo_mix * er;
        }
        /* Shadow and Shimmer: two grain pitch shifters (an octave down, an octave up) on the dry signal plus the reverb's
         * last output, fed back into the reverb so the ghosts ring out as tails */
        float ghost = 0;
        if (ghost_on) {
            e->ps_buf[e->ps_pos] = 0.5f * (l + r) + e->ghost_fb * gtail * 0.6f;
            static const float PS_RATIO[2] = { 0.5f, 2.0f };
            float out[2];
            for (int k = 0; k < 2; k++) {
                e->ps_ph[k] += (1 - PS_RATIO[k]) / PS_WIN;
                if (e->ps_ph[k] >= 1) e->ps_ph[k] -= 1;
                if (e->ps_ph[k] < 0) e->ps_ph[k] += 1;
                float p2 = e->ps_ph[k] + 0.5f; if (p2 >= 1) p2 -= 1;
                out[k] = delay_read(e->ps_buf, e->ps_pos, 2 + e->ps_ph[k] * PS_WIN) * sinf(PI * e->ps_ph[k])
                       + delay_read(e->ps_buf, e->ps_pos, 2 + p2 * PS_WIN) * sinf(PI * p2);
            }
            e->ps_pos = (e->ps_pos + 1) & (PS_LEN - 1);
            e->ps_lp += ps_lpa * (out[0] - e->ps_lp);   /* the shadow: dark */
            e->ps_hp += ps_hpa * (out[1] - e->ps_hp);   /* the shimmer: no mud */
            ghost = fast_tanh(shadow * 1.3f * e->ps_lp + shimmer * 1.1f * (out[1] - e->ps_hp));
        }
        /* reverb send */
        if (space > 0.001f || ghost_on) {
            float in = 0.5f * (l + r) * (space > 0.001f ? 1.0f : 0.0f) + ghost;
            e->rv_hpz += hp_a * (in - e->rv_hpz); in -= e->rv_hpz;   /* keep the lows out of the tail */
            e->pre[e->prepos] = in;
            int pd = (int)e->pre_d;
            float x = e->pre[(e->prepos - pd + PRE_LEN) & (PRE_LEN - 1)];
            float erl = 0, err = 0;
            if (e->er_on) {   /* a few early reflections off concrete */
                static const float TL[4] = { 0.0043f, 0.0097f, 0.0151f, 0.0213f }, TR[4] = { 0.0061f, 0.0118f, 0.0172f, 0.0239f };
                float sc = opt(e, P_SPACE_TYPE) == 2 ? 2.2f : 1.0f;
                for (int t = 0; t < 4; t++) {
                    erl += e->pre[(e->prepos - pd - (int)(TL[t] * sc * SR) + 2 * PRE_LEN) & (PRE_LEN - 1)] * (0.5f - 0.1f * t);
                    err += e->pre[(e->prepos - pd - (int)(TR[t] * sc * SR) + 2 * PRE_LEN) & (PRE_LEN - 1)] * (0.5f - 0.1f * t);
                }
            }
            e->prepos = (e->prepos + 1) & (PRE_LEN - 1);
            for (int a = 0; a < 4; a++) {   /* input diffusion */
                float *b = e->ap[a]; int p = e->appos[a];
                float dl = b[p], y = -e->ap_g * x + dl;
                b[p] = x + e->ap_g * y;
                e->appos[a] = (p + 1) % AP_LEN[a];
                x = y;
            }
            float o[FDN_N];
            for (int n = 0; n < FDN_N; n++) {
                int rp = (e->fpos[n] - (int)e->fdn_len[n] + FDN_LEN) & (FDN_LEN - 1);
                o[n] = e->fdn[n][rp];
                e->fdz[n] += rv_lp * (o[n] - e->fdz[n]);
                o[n] = e->fdz[n] * e->fdn_g[n];
            }
            /* 8-point Hadamard mix */
            for (int h = 1; h < FDN_N; h <<= 1)
                for (int n = 0; n < FDN_N; n += h << 1)
                    for (int j = n; j < n + h; j++) { float a = o[j], b = o[j + h]; o[j] = a + b; o[j + h] = a - b; }
            for (int n = 0; n < FDN_N; n++) {
                float y = o[n] * 0.35355339f + x * ((n & 1) ? -0.5f : 0.5f);
                e->fdn[n][e->fpos[n]] = y + 1e-20f;
                e->fpos[n] = (e->fpos[n] + 1) & (FDN_LEN - 1);
            }
            float wl = e->fdz[0] - e->fdz[2] + e->fdz[4] - e->fdz[6] + erl;
            float wr = e->fdz[1] - e->fdz[3] + e->fdz[5] - e->fdz[7] + err;
            if (!e->er_on) {   /* plate: thin out the bottom for that metallic sheen */
                e->rv_outhp[0] += 0.03f * (wl - e->rv_outhp[0]); wl -= e->rv_outhp[0];
                e->rv_outhp[1] += 0.03f * (wr - e->rv_outhp[1]); wr -= e->rv_outhp[1];
            }
            float wet = fminf(0.6f, (space + 0.7f * fmaxf(shadow, shimmer)) * 0.45f), dry = 1 - 0.25f * space;
            l = l * dry + wl * wet; r = r * dry + wr * wet;
            e->ghost_fb = 0.5f * (wl + wr);   /* the ghost shifters take the tail back in */
        }
        /* M/S width, lows mono */
        m = 0.5f * (l + r); s = 0.5f * (l - r);
        e->s_hp[0] += shp_a * (s - e->s_hp[0]); s -= e->s_hp[0];
        e->s_hp[1] += shp_a * (s - e->s_hp[1]); s -= e->s_hp[1];
        s *= width;
        l = m + s; r = m - s;
        /* squash: a linked compressor in the chosen style, blended in parallel by Mix */
        if (sq > 0.001f) {
            float pk = fmaxf(fabsf(l), fabsf(r)), hk = sq_knee * 0.5f;
            e->sq_env = pk > e->sq_env ? sq_att * e->sq_env + (1 - sq_att) * pk : sq_rel * e->sq_env + (1 - sq_rel) * pk;
            float lv = 20.0f * log10f(e->sq_env + 1e-9f), over = lv - sq_thr, gr = 0;
            if (over > hk) gr = over * (1 - 1 / sq_ratio);
            else if (over > -hk) gr = (over + hk) * (over + hk) / (4 * hk) * (1 - 1 / sq_ratio);   /* soft knee */
            if (gr > gr_max) gr_max = gr;
            float gg = db2lin(-gr) * sq_makeup, cl = l * gg, cr = r * gg;
            if (sq_drive > 0) { cl = fast_tanh(cl * sq_drive) / fast_tanh(sq_drive); cr = fast_tanh(cr * sq_drive) / fast_tanh(sq_drive); }
            l += sq_mix * (cl - l); r += sq_mix * (cr - r);
        }
        /* Pulse: a raised-cosine tremolo, tempo-synced, restarting with the first note */
        if (pulse > 0.001f) {
            e->pulse_ph += pulse_inc; if (e->pulse_ph >= 1) e->pulse_ph -= 1;
            float pg = 1 - pulse * 0.5f * (1 - cosf(2 * PI * (float)e->pulse_ph));
            l *= pg; r *= pg;
        }
        /* cracked vinyl: dust, crackle, static, rumble and a scratch every revolution, while the voices sound */
        e->vy_gate += (vg_t > e->vy_gate ? vg_att : vg_rel) * (vg_t - e->vy_gate);
        if (vinyl > 0.001f) {   /* FOLLOW: with the notes, closing 0.4 s after the last; ALWAYS: a record that keeps spinning */
            float gate = vy_always ? 1.0f : e->vy_gate;
            float u = frand(e);
            if (fabsf(u) < vinyl * 0.0012f) { float a = frand(e); e->vy_click += a * a * a * 0.5f * vinyl * vy_dust; e->vy_pan = frand(e); }
            else if (fabsf(u) < vinyl * 0.006f) { e->vy_click += frand(e) * 0.035f * vinyl * vy_dust; e->vy_pan = frand(e); }
            float click = e->vy_click - e->vy_clickz; e->vy_clickz += 0.5f * click; e->vy_click *= 0.55f;
            e->vy_rum[0] += 0.004f * (frand(e) - e->vy_rum[0]);
            e->vy_rum[1] += 0.02f * (e->vy_rum[0] - e->vy_rum[1]);
            float hl = frand(e), hr = frand(e);
            e->vy_hiss[0][0] += 0.45f * (hl - e->vy_hiss[0][0]); e->vy_hiss[1][0] += 0.45f * (hr - e->vy_hiss[1][0]);
            float scr = 0;
            if (++e->vy_rev >= (long)(1.8f * SR)) { e->vy_rev = 0; e->vy_scr_on = vinyl > 0.25f && vy_scr > 0; e->vy_scr_t = 0; e->vy_scr_amp = 0.18f * powf(vinyl, 1.5f) * vy_scr; }
            if (e->vy_scr_on) {   /* a deep scratch: a falling, filtered burst */
                float t = e->vy_scr_t;
                e->vy_hiss[0][1] += (0.6f - t * 12) * (frand(e) - e->vy_hiss[0][1]);
                scr = e->vy_scr_amp * e->vy_hiss[0][1] * expf(-t * 90) * (1 + 0.5f * sinf(2 * PI * (900 - 6000 * t) * t));
                e->vy_scr_t += 1.0f / SR;
                if (t > 0.05f) e->vy_scr_on = 0;
            }
            float st = 0.006f * vinyl * vy_static;
            float base = gate * (vinyl * vy_static * 0.35f * e->vy_rum[1] * 8.0f + scr);
            l += base + gate * (click * (1 - 0.5f * e->vy_pan) + st * e->vy_hiss[0][0]);
            r += base + gate * (click * (1 + 0.5f * e->vy_pan) + st * e->vy_hiss[1][0]);
        }
        /* volume and limiter */
        l *= vol; r *= vol;
        float pk = fmaxf(fabsf(l), fabsf(r));
        e->lim_env = pk > e->lim_env ? pk : lim_rel * e->lim_env + (1 - lim_rel) * pk;
        float tg = e->lim_env > 0.89f ? 0.89f / e->lim_env : 1.0f;
        e->lim_g += (tg < e->lim_g ? 0.5f : 0.0005f) * (tg - e->lim_g);
        l *= e->lim_g; r *= e->lim_g;
        l = fast_tanh(l * 1.05f); r = fast_tanh(r * 1.05f);
        out_lr[2 * i] = (int16_t)lrintf(clampf(l, -1, 1) * 32767.0f);
        out_lr[2 * i + 1] = (int16_t)lrintf(clampf(r, -1, 1) * 32767.0f);
    }
    e->gr_hold = gr_max > e->gr_hold ? gr_max : e->gr_hold * 0.97f;   /* the readout: peak, falling ~1 s */
    /* keep recursive states out of denormals */
    for (int c = 0; c < 2; c++) for (int s2 = 0; s2 < 2; s2++) for (int q = 0; q < 2; q++) if (fabsf(e->svf[c][s2][q]) < 1e-15f) e->svf[c][s2][q] = 0;
    for (int n = 0; n < FDN_N; n++) if (fabsf(e->fdz[n]) < 1e-15f) e->fdz[n] = 0;
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
