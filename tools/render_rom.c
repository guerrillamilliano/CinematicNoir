/* Renders Cinematic Noir's built-in sound set: every sound is modelled in software (no recordings), multisampled every
 * few semitones, printed through a tube and tape stage, and written as 16-bit WAVs with smpl loops, one folder per
 * instrument ("NN Category - Name/<Name> <root>.wav" plus info.txt with the modes that list it).
 *   render_rom <out dir> [catalogue.json]      (tools/make_rom.ps1 runs it in Docker)
 * The catalogue (folder, name, modes, in browser order) is what gen_presets.py builds the presets from.
 * Deterministic: the same build always renders the same audio. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SR 44100.0
#define PI 3.14159265358979

/* ---- random ---- */
static uint32_t rs = 1;
static double rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs & 0xffffff) / 8388608.0 - 1.0; }   /* -1..1 */
static double hz(double note) { return 440.0 * pow(2.0, (note - 69) / 12.0); }
static double cents(double c) { return pow(2.0, c / 1200.0); }

/* ---- filters ---- */
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } bq_t;
static void bq_set(bq_t *q, int type, double f, double Q, double db) {   /* RBJ: 0 LP, 1 HP, 2 BP (peak 0 dB), 3 peak */
    double w = 2 * PI * fmin(f, SR * 0.45) / SR, cw = cos(w), al = sin(w) / (2 * Q), A = pow(10, db / 40), a0;
    q->z1 = q->z2 = 0;   /* filters are stack locals: start from silence, not whatever was there */
    switch (type) {
    case 0: a0 = 1 + al; q->b0 = (1 - cw) / 2 / a0; q->b1 = (1 - cw) / a0; q->b2 = q->b0; q->a1 = -2 * cw / a0; q->a2 = (1 - al) / a0; break;
    case 1: a0 = 1 + al; q->b0 = (1 + cw) / 2 / a0; q->b1 = -(1 + cw) / a0; q->b2 = q->b0; q->a1 = -2 * cw / a0; q->a2 = (1 - al) / a0; break;
    case 2: a0 = 1 + al; q->b0 = al / a0; q->b1 = 0; q->b2 = -al / a0; q->a1 = -2 * cw / a0; q->a2 = (1 - al) / a0; break;
    default: a0 = 1 + al / A; q->b0 = (1 + al * A) / a0; q->b1 = -2 * cw / a0; q->b2 = (1 - al * A) / a0; q->a1 = -2 * cw / a0; q->a2 = (1 - al / A) / a0;
    }
}
static double bq(bq_t *q, double x) {
    double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}
typedef struct { double s1, s2; } svf_t;   /* TPT state-variable low-pass */
static double svf_lp(svf_t *s, double f, double res, double x) {
    double g = tan(PI * fmin(f, SR * 0.45) / SR), k = 2 - 2 * res;
    double hp = (x - (k + g) * s->s1 - s->s2) / (1 + g * (g + k));
    double bp = g * hp + s->s1; s->s1 = g * hp + bp;
    double lp = g * bp + s->s2; s->s2 = g * bp + lp;
    return lp;
}

/* ---- oscillators ---- */
static double blep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}
typedef struct { double ph; } osc_t;
static double saw(osc_t *o, double f) {
    double dt = f / SR, y = 2 * o->ph - 1 - blep(o->ph, dt);
    o->ph += dt; if (o->ph >= 1) o->ph -= 1;
    return y;
}
static double square(osc_t *o, double f) {
    double dt = f / SR, y = (o->ph < 0.5 ? 1 : -1) + blep(o->ph, dt) - blep(fmod(o->ph + 0.5, 1), dt);
    o->ph += dt; if (o->ph >= 1) o->ph -= 1;
    return y;
}
/* a damped sine as a rotating complex number: one multiply a sample */
typedef struct { double re, im, cr, ci; } ph_t;
static void ph_init(ph_t *p, double f, double t60, double amp, double phase) {
    double r = t60 > 0 ? pow(10, -3.0 / (t60 * SR)) : 1, w = 2 * PI * f / SR;
    p->re = amp * cos(phase); p->im = amp * sin(phase); p->cr = r * cos(w); p->ci = r * sin(w);
}
static double ph_step(ph_t *p) {
    double re = p->re * p->cr - p->im * p->ci, im = p->re * p->ci + p->im * p->cr;
    p->re = re; p->im = im;
    return im;
}

/* ---- shared stages ---- */
static void comp(float *L, float *R, int n, double thr_db, double ratio, double att, double rel) {
    double env = 0, ka = exp(-1 / (att * SR)), kr = exp(-1 / (rel * SR));
    for (int i = 0; i < n; i++) {
        double pk = fmax(fabs(L[i]), fabs(R[i]));
        env = pk > env ? ka * env + (1 - ka) * pk : kr * env + (1 - kr) * pk;
        double lv = 20 * log10(env + 1e-9), over = lv - thr_db, g = over > 0 ? pow(10, -over * (1 - 1 / ratio) / 20) : 1;
        L[i] *= g; R[i] *= g;
    }
}
static void room(float *L, float *R, int n, double size, double mix, double decay) {   /* small boxy room: combs + allpasses */
    static const int CL[4] = { 557, 617, 691, 773 }, AL[2] = { 225, 341 };
    float *buf = calloc(8192 * 6, sizeof(float));
    int pos[6] = {0}, len[6];
    double lp[4] = {0};
    for (int c = 0; c < 4; c++) len[c] = (int)(CL[c] * size);
    for (int a = 0; a < 2; a++) len[4 + a] = AL[a];
    for (int i = 0; i < n; i++) {
        double in = 0.5 * (L[i] + R[i]) * 0.3, s = 0;
        for (int c = 0; c < 4; c++) {
            float *b = buf + c * 8192;
            double y = b[pos[c]];
            lp[c] += 0.45 * (y - lp[c]);
            b[pos[c]] = (float)(in + lp[c] * decay);
            pos[c] = (pos[c] + 1) % len[c];
            s += y;
        }
        for (int a = 0; a < 2; a++) {
            float *b = buf + (4 + a) * 8192;
            double d = b[pos[4 + a]], y = -0.5 * s + d;
            b[pos[4 + a]] = (float)(s + 0.5 * y);
            pos[4 + a] = (pos[4 + a] + 1) % len[4 + a];
            s = y;
        }
        L[i] = (float)(L[i] * (1 - mix) + s * mix);
        R[i] = (float)(R[i] * (1 - mix) - s * mix * 0.9 + s * mix * 0.1);
    }
    free(buf);
}
/* the print: a tube stage (asymmetric, mostly 2nd harmonic) then tape (head bump, high roll-off, gentle compression) */
static void print_chain(float *L, float *R, int n, double heat) {
    bq_t hb[2], hl[2];
    for (int c = 0; c < 2; c++) { bq_set(&hb[c], 3, 85, 0.8, 1.8 * heat); bq_set(&hl[c], 0, 14500 - 3000 * heat, 0.6, 0); }
    double peak = 1e-9;
    for (int i = 0; i < n; i++) peak = fmax(peak, fmax(fabs(L[i]), fabs(R[i])));
    double pre = 0.7 / peak, bias = 0.1 * heat;
    for (int i = 0; i < n; i++) {
        float *x[2] = { &L[i], &R[i] };
        for (int c = 0; c < 2; c++) {
            double v = *x[c] * pre * (1 + heat);
            v = tanh(v + bias) - tanh(bias);
            v = bq(&hb[c], v);
            v = bq(&hl[c], v);
            *x[c] = (float)v;
        }
    }
}

/* ---- instruments ---- */
enum { K_PIANO, K_EP, K_WURLI, K_ORGAN, K_STRINGS, K_PIZZ, K_CHOIR, K_BRASS, K_MUTE, K_SAX, K_DRONE, K_SUB, K_BASS,
       K_MUSICBOX, K_GLASS, K_VIBES, K_PAD, K_LEAD };
typedef struct {
    const char *folder, *modes; int kind;
    int lo, hi, step, stereo;
    double len, loop;           /* seconds; loop = loop start (0 = one-shot) */
    double p[6];                /* per-kind settings, see render_note() */
} inst_def_t;

static const inst_def_t INST[] = {
    /* piano p: detune spread (cents), global detune, darkness (spectral tilt), hammer level, room, compression */
    { "01 Keys - Dark Grand", "dark,gritty", K_PIANO, 24, 96, 6, 1, 6.0, 0, { 1.8, -4, 1.55, 0.6, 0.15, 0 } },
    { "02 Keys - Detuned Grand", "dark,cinematic", K_PIANO, 24, 96, 6, 1, 6.0, 0, { 6.0, -9, 1.35, 0.5, 0.25, 0 } },
    { "03 Keys - Saloon Upright", "gritty,dark", K_PIANO, 24, 96, 6, 1, 4.0, 0, { 14.0, 0, 1.15, 1.0, 0.45, 1 } },
    /* ep p: bark (FM index), tine level, decay scale */
    { "04 Keys - Smoky Rhodes", "dark", K_EP, 28, 96, 6, 0, 4.5, 0, { 1.1, 0.25, 1.0 } },
    { "05 Keys - Velvet Wurli", "dark,gritty", K_WURLI, 33, 93, 6, 0, 3.5, 0, { 1.8, 0, 0.8 } },
    { "06 Keys - Funeral Organ", "cinematic,dark", K_ORGAN, 24, 96, 6, 0, 3.0, 1.0, { 0 } },
    /* strings p: voices, detune (cents), vibrato depth (cents), wander (cents), swell (s), brightness (Hz) */
    { "07 Strings - Horror Swell", "cinematic", K_STRINGS, 52, 96, 6, 1, 5.0, 2.4, { 8, 9, 22, 6, 1.6, 7500 } },
    { "08 Strings - Weeping Section", "cinematic", K_STRINGS, 43, 91, 6, 1, 4.5, 1.6, { 10, 12, 14, 14, 0.35, 5000 } },
    { "09 Strings - Low Cellos", "cinematic,gritty,dark", K_STRINGS, 28, 67, 6, 1, 4.0, 1.4, { 7, 10, 12, 6, 0.22, 3200 } },
    { "10 Strings - Tremolo Panic", "cinematic,gritty", K_STRINGS, 43, 96, 6, 1, 4.0, 1.2, { 8, 14, 8, 8, -0.12, 8000 } },
    { "11 Strings - Pizzicato Plucks", "cinematic,dark", K_PIZZ, 31, 91, 6, 1, 2.2, 0, { 0 } },
    /* choir p: singers, vowel (0 aah, 1 ooh), attack (s), stab (1 = a short hit with a tail) */
    { "12 Choir - Opera Aah", "cinematic,dark,gritty", K_CHOIR, 40, 84, 6, 1, 4.5, 1.6, { 7, 0, 0.35, 0 } },
    { "13 Choir - Dark Ooh", "cinematic,dark", K_CHOIR, 36, 79, 6, 1, 4.5, 1.6, { 7, 1, 0.5, 0 } },
    { "14 Choir - Opera Stab", "cinematic,gritty", K_CHOIR, 40, 84, 6, 1, 2.2, 0, { 7, 0, 0.012, 1 } },
    /* brass p: players, detune (cents), stab (1 = a short hit), brightness (x f), attack (s) */
    { "15 Brass - Mafioso Stab", "gritty", K_BRASS, 36, 84, 6, 1, 1.4, 0, { 5, 9, 1, 16, 0.025 } },
    { "16 Brass - Muted Trumpet", "gritty,dark,cinematic", K_MUTE, 52, 88, 6, 0, 4.0, 1.4, { 0 } },
    { "17 Brass - Dark Horns", "cinematic,gritty", K_BRASS, 31, 77, 6, 1, 4.5, 1.6, { 4, 7, 0, 6, 0.28 } },
    { "18 Brass - Smoke Sax", "dark,gritty", K_SAX, 40, 82, 6, 0, 4.0, 1.4, { 0 } },
    { "19 Synth - Suspense Drone", "cinematic,dark", K_DRONE, 24, 84, 12, 1, 6.0, 1.0, { 0 } },
    { "20 Synth - Sub Rumble", "dark,gritty,cinematic", K_SUB, 24, 60, 6, 0, 3.0, 1.0, { 0 } },
    { "21 Synth - Sinister Bass", "gritty,dark", K_BASS, 24, 60, 6, 0, 3.0, 1.2, { 0 } },
    { "22 Texture - Music Box", "dark,cinematic,gritty", K_MUSICBOX, 60, 102, 6, 1, 3.5, 0, { 0 } },
    { "23 Texture - Ghost Glass", "cinematic,dark", K_GLASS, 48, 96, 12, 1, 5.0, 1.5, { 0 } },
    { "24 Texture - Vibes Noir", "dark,gritty", K_VIBES, 48, 96, 6, 1, 4.0, 0, { 0 } },
    /* pad p: flavour (0 choir and strings, 1 analog saws, 2 strings with a shimmer an octave up) */
    { "25 Pads - Ominous Choir Pad", "cinematic,dark,gritty", K_PAD, 36, 84, 6, 1, 6.0, 2.2, { 0 } },
    { "26 Pads - Cold War Pad", "dark,gritty", K_PAD, 31, 79, 6, 1, 6.0, 2.0, { 1 } },
    { "27 Pads - Blood Moon", "cinematic,dark", K_PAD, 36, 84, 6, 1, 6.0, 2.2, { 2 } },
    /* lead p: flavour (0 theremin, 1 saw, 2 whistle, 3 pulse, 4 fat saws + sub, 5 flute) */
    { "28 Leads - Theremin Ghost", "cinematic,dark", K_LEAD, 48, 90, 6, 0, 4.0, 1.4, { 0 } },
    { "29 Leads - Saw Detective", "gritty,dark", K_LEAD, 36, 84, 6, 0, 4.0, 1.2, { 1 } },
    { "30 Leads - Noir Whistle", "cinematic,gritty", K_LEAD, 60, 96, 6, 0, 4.0, 1.4, { 2 } },
    { "31 Leads - Square Specter", "dark,gritty", K_LEAD, 36, 84, 6, 0, 4.0, 1.2, { 3 } },
    { "32 Leads - Moog Menace", "gritty,cinematic", K_LEAD, 31, 79, 6, 1, 4.0, 1.2, { 4 } },
    { "33 Leads - Glass Flute", "cinematic,dark", K_LEAD, 55, 91, 6, 0, 4.0, 1.4, { 5 } },
};
#define NINST ((int)(sizeof INST / sizeof INST[0]))

/* one line on each sound's character, in INST order: info.txt's desc=, shown on the plugin's screen */
static const char *const DESC[] = {
    "Felt-hammer concert grand, darkened highs, three strings per note",
    "Grand with drifting, out-of-tune strings: a haunted-parlor ache",
    "Honky-tonk upright in a tight room, heavily compressed",
    "Mellow tine electric piano with a soft bark",
    "Reedy electric piano that growls when pushed",
    "Dark drawbar organ, slow and sacred",
    "Violin section swelling out of silence, wide vibrato",
    "Legato strings with sagging, weeping pitch",
    "Cellos and basses, short bow attack, menacing low end",
    "Bowed tremolo strings for rising tension",
    "Plucked string section, creeping and dry",
    "Operatic choir on an open AAH",
    "Hushed choir on a dark OOH",
    "Choir hit with a cathedral tail",
    "Brass section stab, short and hard",
    "Harmon-muted trumpet with a late warble",
    "Slow French-horn section, ominous and round",
    "Breathy tenor sax with a little growl",
    "Detuned analog drone under a slow filter sweep",
    "Saturated sub-bass with a low rumble",
    "Analog bass, the filter snaps shut on every note",
    "Two music-box combs slightly apart: eerie beating",
    "Rubbed-glass harmonics, airy and cold",
    "Vibraphone bars, soft mallets, late-night jazz",
    "Strings and a dark choir blooming together",
    "Six detuned analog saws breathing under a slow sweep",
    "Weeping strings with a ghostly shimmer an octave up",
    "Theremin: a pure, wavering tone straight out of a 1950s thriller",
    "Filtered analog saw lead from a 70s crime-show score",
    "A lonely whistled melody with breath and a slow vibrato",
    "Hollow pulse-wave lead, haunted and thin",
    "Fat detuned saws with a sub and a snarling resonant filter",
    "Dark, breathy flute with a soft chiff on every note",
};

static void piano(const inst_def_t *d, double f, int note, float *L, float *R, int n) {
    double spread = d->p[0], tilt = d->p[2], hammer = d->p[3];
    double base_t60 = fmin(16.0, fmax(1.4, 14.0 * pow(2.0, -(note - 24) / 22.0)));
    double B = 0.00012 * pow(2.0, (note - 60) / 18.0);
    double fmax_h = note < 50 ? 7000 : 9500;
    if (tilt > 1.4) fmax_h *= 0.75;   /* the dark grand: fewer highs */
    double fd = f * cents(d->p[1]);
    for (int k = 1; k <= 90; k++) {
        double fk = fd * k * sqrt(1 + B * k * k);
        if (fk > fmax_h) break;
        double a = pow(k, -tilt) * (0.35 + fabs(sin(PI * k / 7.3)));   /* struck at 1/7 of the string */
        double t60 = base_t60 / (1 + 0.12 * (k - 1) * (fk / 1000 + 0.5));
        for (int s = 0; s < 3; s++) {   /* three strings, slightly apart; the outer two lean left and right */
            double c = (s - 1) * spread + rnd() * spread * 0.3;
            ph_t fast, slow;
            double ph = rnd() * PI;
            ph_init(&fast, fk * cents(c), t60 * 0.28, a * 0.6, ph);
            ph_init(&slow, fk * cents(c * 1.1), t60, a * 0.4, ph);
            double pl = s == 0 ? 0.8 : s == 1 ? 0.5 : 0.2;
            for (int i = 0; i < n; i++) {
                double y = ph_step(&fast) + ph_step(&slow);
                L[i] += (float)(y * pl); R[i] += (float)(y * (1 - pl));
                if ((i & 1023) == 0 && fabs(fast.re) + fabs(fast.im) + fabs(slow.re) + fabs(slow.im) < 1e-6) break;
            }
        }
    }
    /* hammer: a felt thump and a short knock */
    bq_t lp; bq_set(&lp, 0, 900 + f * 2, 0.7, 0);
    for (int i = 0; i < (int)(0.05 * SR) && i < n; i++) {
        double t = i / SR, x = bq(&lp, rnd()) * exp(-t * 90) * hammer * 0.6 + sin(2 * PI * 95 * t) * exp(-t * 60) * hammer * 0.3;
        L[i] += (float)x; R[i] += (float)x;
    }
    if (d->p[4] > 0) room(L, R, n, d->kind == K_PIANO && d->p[5] ? 0.45 : 0.8, d->p[4], 0.55);
    if (d->p[5]) comp(L, R, n, -24, 6, 0.002, 0.12);
    /* fade the tail */
    for (int i = n - (int)(0.4 * SR); i < n; i++) if (i >= 0) { double g = (double)(n - i) / (0.4 * SR); L[i] *= (float)g; R[i] *= (float)g; }
}

static void ep(const inst_def_t *d, double f, int note, float *L, float *R, int n, int wurli) {
    double t60 = fmin(6, fmax(1.2, 5.0 * pow(2.0, -(note - 40) / 24.0))) * d->p[2];
    double r = pow(10, -3.0 / (t60 * SR)), env = 1, pc = 0, pm = 0;
    bq_t lp; bq_set(&lp, 0, wurli ? 3800 : 3000, 0.7, 0);
    for (int i = 0; i < n; i++) {
        double t = i / SR, I = d->p[0] * exp(-t * (wurli ? 5 : 3.5)) + 0.15;
        double m = sin(pm), y = sin(pc + I * m);
        if (wurli) y = tanh(1.8 * y + 0.3) - tanh(0.3);
        y += d->p[1] * sin(2 * PI * f * 7.03 * t) * exp(-t * 35);   /* tine ping */
        y *= env * (1 - exp(-t * 900));
        y = bq(&lp, y);
        L[i] = R[i] = (float)y;
        env *= r;
        pc += 2 * PI * f / SR; pm += 2 * PI * f / SR;
        if (pc > 2 * PI) pc -= 2 * PI;
        if (pm > 2 * PI) pm -= 2 * PI;
    }
}

static void organ(const inst_def_t *d, double f, float *L, float *R, int n) {
    (void)d;
    static const double RATIO[5] = { 0.5, 1, 2, 3, 4 }, AMP[5] = { 0.8, 1.0, 0.45, 0.25, 0.12 };
    double ph[5] = {0};
    for (int i = 0; i < n; i++) {
        double t = i / SR, y = 0;
        for (int k = 0; k < 5; k++) {
            y += AMP[k] * sin(ph[k]);
            ph[k] += 2 * PI * f * RATIO[k] * cents(k * 0.7) / SR;
            if (ph[k] > 2 * PI) ph[k] -= 2 * PI;
        }
        y += 0.02 * rnd() * exp(-t * 200);   /* key click */
        y *= 1 - exp(-t * 300);
        L[i] = R[i] = (float)(y * 0.3);
    }
}

static void strings(const inst_def_t *d, double f, float *L, float *R, int n) {
    int nv = (int)d->p[0];
    double det = d->p[1], vib = d->p[2], wander = d->p[3], swell = d->p[4], bright = d->p[5];
    int tremolo = swell < 0;
    for (int v = 0; v < nv; v++) {
        osc_t o = { (rnd() + 1) / 2 };
        double c0 = rnd() * det, vr = 4.6 + rnd() * 0.9, vp = rnd() * PI, wv = 0, wt = 0;
        double pan = 0.5 + 0.45 * rnd(), onset = (rnd() + 1) * 0.04;
        double trem_ph = rnd() * PI, trem_r = 11 + rnd() * 1.5;
        for (int i = 0; i < n; i++) {
            double t = i / SR;
            if ((i & 4095) == 0) wt = rnd() * wander;
            wv += 0.00004 * (wt - wv);
            double vibd = vib * fmin(1, t / 0.6);   /* vibrato grows in */
            double fr = f * cents(c0 + wv + vibd * sin(2 * PI * vr * t + vp));
            double a = tremolo ? 0.5 + 0.5 * fabs(sin(PI * trem_r * t + trem_ph)) : 1;
            double sw = fabs(swell);
            double env = t < onset ? 0 : fmin(1, (t - onset) / sw);
            env = env * env * (3 - 2 * env);
            double y = saw(&o, fr) * a * env;
            L[i] += (float)(y * pan); R[i] += (float)(y * (1 - pan));
        }
    }
    /* body: wood resonances, bow noise, brightness */
    bq_t b1[2], b2[2], b3[2], lp[2], hp[2];
    for (int c = 0; c < 2; c++) {
        bq_set(&b1[c], 3, 290, 1.4, 5); bq_set(&b2[c], 3, 950, 1.6, 3); bq_set(&b3[c], 3, 2700, 1.2, 4);
        bq_set(&lp[c], 0, bright, 0.6, 0); bq_set(&hp[c], 1, fmax(60, f * 0.7), 0.7, 0);
    }
    swell = fabs(swell);
    for (int i = 0; i < n; i++) {
        double t = i / SR, env = fmin(1, t / swell), bow = 0.015 * rnd() * env;
        double bl = 0.4 + 0.6 * env;   /* a swell brightens as it grows */
        float *x[2] = { &L[i], &R[i] };
        for (int c = 0; c < 2; c++) {
            double v = *x[c] + bow;
            v = bq(&b1[c], v); v = bq(&b2[c], v); v = bq(&b3[c], v);
            v = bq(&hp[c], v);
            double lpd = bq(&lp[c], v);
            *x[c] = (float)(lpd * bl + v * (1 - bl) * 0.2);
        }
    }
}

static void pizz(double f, float *L, float *R, int n) {
    for (int s = 0; s < 3; s++) {   /* three players: Karplus-Strong strings */
        double fr = f * cents(rnd() * 8);
        int len = (int)(SR / fr);
        double *ks = calloc((size_t)len + 2, sizeof(double)), last = 0, pan = 0.5 + 0.4 * rnd();
        bq_t ex; bq_set(&ex, 0, 1800, 0.7, 0);
        for (int i = 0; i < len; i++) ks[i] = bq(&ex, rnd());
        double damp = 0.996 - 0.0006 * (f / 200);
        int p = 0;
        for (int i = 0; i < n; i++) {
            double y = ks[p];
            double nv = damp * 0.5 * (y + last);
            last = y; ks[p] = nv;
            p = (p + 1) % len;
            L[i] += (float)(y * pan); R[i] += (float)(y * (1 - pan));
        }
        free(ks);
    }
    bq_t b[2]; for (int c = 0; c < 2; c++) bq_set(&b[c], 3, 320, 1.2, 6);
    for (int i = 0; i < n; i++) { L[i] = (float)bq(&b[0], L[i]); R[i] = (float)bq(&b[1], R[i]); }
}

static void choir(const inst_def_t *d, double f, int note, float *L, float *R, int n) {
    int ns = (int)d->p[0], vowel = (int)d->p[1], stab = (int)d->p[3];
    double att = d->p[2];
    int female = note >= 55;
    static const double FM[2][2][4] = { { { 650, 1080, 2650, 2900 }, { 350, 640, 2400, 2675 } },     /* male aah, ooh */
                                        { { 800, 1150, 2900, 3900 }, { 330, 700, 2530, 3500 } } };   /* female aah, ooh */
    static const double AMP[2][4] = { { 1, 0.5, 0.25, 0.1 }, { 1, 0.25, 0.06, 0.03 } };
    double src_l[1], src_r[1]; (void)src_l; (void)src_r;
    float *sl = calloc((size_t)n, sizeof(float)), *sr = calloc((size_t)n, sizeof(float));
    for (int s = 0; s < ns; s++) {
        osc_t o = { (rnd() + 1) / 2 };
        double c0 = rnd() * 10, vr = 5.0 + rnd() * 0.7, vp = rnd() * PI, pan = 0.5 + 0.45 * rnd(), jit = 0;
        svf_t tilt = {0};
        for (int i = 0; i < n; i++) {
            double t = i / SR;
            if ((i & 511) == 0) jit = rnd() * 4;
            double fr = f * cents(c0 + jit + 28 * fmin(1, t / 0.5) * sin(2 * PI * vr * t + vp));
            double y = svf_lp(&tilt, f * 6, 0, saw(&o, fr));   /* a glottal-ish source: a saw tilted down */
            y += 0.06 * rnd();   /* breath */
            sl[i] += (float)(y * pan); sr[i] += (float)(y * (1 - pan));
        }
    }
    bq_t fb[2][4];
    for (int c = 0; c < 2; c++) for (int k = 0; k < 4; k++) bq_set(&fb[c][k], 2, FM[female][vowel][k], FM[female][vowel][k] / (70 + 30 * k), 0);
    for (int i = 0; i < n; i++) {
        double t = i / SR;
        double env = fmin(1, t / att);
        if (stab) env *= exp(-fmax(0, t - 0.08) * 6);
        double yl = 0, yr = 0;
        for (int k = 0; k < 4; k++) { yl += AMP[vowel][k] * bq(&fb[0][k], sl[i]); yr += AMP[vowel][k] * bq(&fb[1][k], sr[i]); }
        L[i] = (float)(yl * env); R[i] = (float)(yr * env);
    }
    free(sl); free(sr);
    if (stab) room(L, R, n, 1.6, 0.35, 0.78);
}

static void brass(const inst_def_t *d, double f, float *L, float *R, int n) {
    int np = (int)d->p[0], stab = (int)d->p[2];
    double det = d->p[1], bright = d->p[3], att = d->p[4];
    for (int p = 0; p < np; p++) {
        osc_t o = { (rnd() + 1) / 2 };
        svf_t lp = {0};
        double c0 = rnd() * det, pan = 0.5 + 0.4 * rnd(), late = (rnd() + 1) * 0.012;
        for (int i = 0; i < n; i++) {
            double t = i / SR - late;
            if (t < 0) continue;
            double env;
            if (stab) env = t < att ? t / att : t < 0.32 ? 1 - 0.25 * (t - att) / 0.3 : 0.75 * exp(-(t - 0.32) * 14);
            else env = fmin(1, t / att) * (1 + 0.15 * exp(-t * 3));
            double scoop = -45 * exp(-t * 30);   /* lips find the note */
            double fr = f * cents(c0 + scoop + (stab ? 0 : 6 * sin(2 * PI * 5.2 * t) * fmin(1, t / 0.8)));
            double fc = f * (1.5 + bright * pow(fmin(env, 1.2), 1.6));   /* brighter as it gets louder */
            double y = svf_lp(&lp, fc, 0.15, saw(&o, fr)) * env;
            L[i] += (float)(y * pan); R[i] += (float)(y * (1 - pan));
        }
    }
    if (stab) room(L, R, n, 0.7, 0.22, 0.6);
}

static void muted(double f, float *L, float *R, int n) {
    osc_t o = {0};
    bq_t hp1, hp2, pk, lp;
    bq_set(&hp1, 1, 900, 0.7, 0); bq_set(&hp2, 1, 900, 0.7, 0); bq_set(&pk, 3, 1750, 4, 12); bq_set(&lp, 0, 4800, 0.7, 0);
    double wob = 0, wt = 0;
    for (int i = 0; i < n; i++) {
        double t = i / SR;
        if ((i & 2047) == 0) wt = rnd() * 8;
        wob += 0.0005 * (wt - wob);
        double vib = 30 * fmin(1, fmax(0, t - 0.35) / 0.5) * sin(2 * PI * 5.6 * t);   /* a late, warbling vibrato */
        double env = fmin(1, t / 0.04);
        double y = saw(&o, f * cents(vib + wob - 30 * exp(-t * 25))) * env;
        y = bq(&hp1, y); y = bq(&hp2, y); y = bq(&pk, y); y = bq(&lp, y);
        L[i] = R[i] = (float)y;
    }
}

static void sax(double f, float *L, float *R, int n) {
    osc_t o1 = {0}, o2 = {0.3};
    bq_t f1, f2, f3, lp;
    bq_set(&f1, 3, 520, 2, 9); bq_set(&f2, 3, 1500, 2.5, 6); bq_set(&f3, 3, 2600, 2, 4); bq_set(&lp, 0, 5000, 0.6, 0);
    for (int i = 0; i < n; i++) {
        double t = i / SR, env = fmin(1, t / 0.07);
        double vib = 18 * fmin(1, fmax(0, t - 0.5) / 0.6) * sin(2 * PI * 5.0 * t);
        double fr = f * cents(vib - 20 * exp(-t * 20));
        double y = 0.6 * saw(&o1, fr) + 0.4 * square(&o2, fr);
        y *= 1 + 0.08 * sin(2 * PI * 31 * t) * rnd();   /* a little growl */
        y += 0.05 * rnd();                               /* breath */
        y = bq(&f1, y); y = bq(&f2, y); y = bq(&f3, y); y = bq(&lp, y);
        L[i] = R[i] = (float)(y * env);
    }
}

static void drone(const inst_def_t *d, double f, float *L, float *R, int n) {
    osc_t o[3] = { {0}, {0.33}, {0.66} }, sq = {0};
    svf_t fl = {0}, fr = {0};
    double loop = d->loop, period = d->len - loop;   /* the filter sweep repeats exactly once per loop */
    for (int i = 0; i < n; i++) {
        double t = i / SR, env = fmin(1, t / 0.8);
        double sweep = 0.5 - 0.5 * cos(2 * PI * fmax(0, t - loop) / period);
        double a = saw(&o[0], f * cents(-7)), b = saw(&o[1], f), c = saw(&o[2], f * cents(7)), s = square(&sq, f / 2);
        double fc = f * (2 + 6 * sweep);
        double yl = svf_lp(&fl, fc, 0.45, a + 0.6 * b + 0.5 * s), yr = svf_lp(&fr, fc * 1.05, 0.45, c + 0.6 * b + 0.5 * s);
        L[i] = (float)(yl * env); R[i] = (float)(yr * env);
    }
}

static void sub(double f, float *L, float *R, int n) {
    bq_t lp; bq_set(&lp, 0, 90, 0.7, 0);
    double ph = 0;
    for (int i = 0; i < n; i++) {
        double t = i / SR, env = fmin(1, t / 0.03);
        double y = tanh(1.6 * (sin(ph) + 0.25 * sin(2 * ph))) + 0.3 * bq(&lp, rnd()) * (0.6 + 0.4 * sin(2 * PI * 0.5 * t));
        ph += 2 * PI * f / SR; if (ph > 2 * PI) ph -= 2 * PI;
        L[i] = R[i] = (float)(y * env);
    }
}

static void bass(double f, float *L, float *R, int n) {
    osc_t a = {0}, b = {0.5};
    svf_t lp = {0};
    for (int i = 0; i < n; i++) {
        double t = i / SR, env = fmin(1, t / 0.004) * (0.75 + 0.25 * exp(-t * 4));
        double fc = f * (3 + 10 * exp(-t * 7));
        double y = svf_lp(&lp, fc, 0.35, 0.7 * saw(&a, f) + 0.5 * square(&b, f * cents(-4)));
        L[i] = R[i] = (float)(y * env);
    }
}

static void musicbox(double f, float *L, float *R, int n) {
    static const double RATIO[4] = { 1, 2.02, 5.37, 9.31 }, T60[4] = { 2.8, 1.1, 0.35, 0.14 }, A[4] = { 1, 0.18, 0.12, 0.06 };
    for (int v = 0; v < 2; v++) {   /* two combs, a few cents apart: the slow beating makes it eerie */
        double pan = v ? 0.25 : 0.75, c = v ? 6 : 0;
        for (int k = 0; k < 4; k++) {
            ph_t p; ph_init(&p, f * RATIO[k] * cents(c), T60[k] * (1.4 - f / 3000), A[k] * (v ? 0.7 : 1), rnd() * PI);
            for (int i = 0; i < n; i++) { double y = ph_step(&p); L[i] += (float)(y * pan); R[i] += (float)(y * (1 - pan)); }
        }
    }
    for (int i = 0; i < (int)(0.004 * SR); i++) { double x = 0.2 * rnd(); L[i] += (float)x; R[i] += (float)x; }
    room(L, R, n, 1.1, 0.3, 0.7);
}

static void glass(double f, float *L, float *R, int n) {
    static const double RATIO[3] = { 1, 2.005, 3.012 }, A[3] = { 1, 0.4, 0.15 };
    double ph[2][3] = {{0}};
    bq_t bp[2]; bq_set(&bp[0], 2, f * 2, 8, 0); bq_set(&bp[1], 2, f * 2.01, 8, 0);
    for (int i = 0; i < n; i++) {
        double t = i / SR, env = fmin(1, t / 0.45), yl = 0, yr = 0;
        for (int k = 0; k < 3; k++) {
            yl += A[k] * sin(ph[0][k]); yr += A[k] * sin(ph[1][k]);
            ph[0][k] += 2 * PI * f * RATIO[k] / SR; ph[1][k] += 2 * PI * f * RATIO[k] * cents(4) / SR;
        }
        yl += 0.6 * bq(&bp[0], rnd()); yr += 0.6 * bq(&bp[1], rnd());   /* the airy rub of a wet finger */
        L[i] = (float)(yl * env); R[i] = (float)(yr * env);
    }
}

static void vibes(double f, float *L, float *R, int n) {
    static const double RATIO[3] = { 1, 4.0, 10.0 }, T60[3] = { 3.5, 0.9, 0.25 }, A[3] = { 1, 0.3, 0.1 };
    for (int k = 0; k < 3; k++) {
        ph_t a, b;
        ph_init(&a, f * RATIO[k], T60[k], A[k], 0);
        ph_init(&b, f * RATIO[k] * cents(3), T60[k], A[k] * 0.8, 1.3);
        for (int i = 0; i < n; i++) { L[i] += (float)ph_step(&a); R[i] += (float)ph_step(&b); }
    }
    bq_t lp; bq_set(&lp, 0, 2500, 0.7, 0);
    for (int i = 0; i < (int)(0.01 * SR); i++) { double x = 0.15 * bq(&lp, rnd()) * (1 - i / (0.01 * SR)); L[i] += (float)x; R[i] += (float)x; }
}

/* Pads: slow layers built from the other models, rendered into scratch buffers and blended. */
static void pad(const inst_def_t *d, double f, int note, float *L, float *R, int n) {
    int flavour = (int)d->p[0];
    float *aL = calloc((size_t)n, sizeof(float)), *aR = calloc((size_t)n, sizeof(float));
    if (flavour == 0) {
        inst_def_t s = { 0, 0, K_STRINGS, 0, 0, 0, 1, d->len, d->loop, { 8, 10, 10, 10, 1.8, 3800 } };
        inst_def_t c = { 0, 0, K_CHOIR, 0, 0, 0, 1, d->len, d->loop, { 6, 1, 1.6, 0 } };
        strings(&s, f, L, R, n);
        choir(&c, f, note, aL, aR, n);
        for (int i = 0; i < n; i++) { L[i] = 0.55f * L[i] + 0.8f * aL[i]; R[i] = 0.55f * R[i] + 0.8f * aR[i]; }
    } else if (flavour == 1) {   /* six detuned saws, a slow low-pass sweep that repeats once per loop, a sine sub */
        osc_t o[6];
        svf_t fl = {0}, fr = {0};
        double ph = 0, period = d->len - d->loop;
        for (int k = 0; k < 6; k++) o[k].ph = k / 6.0;
        for (int i = 0; i < n; i++) {
            double t = i / SR, env = fmin(1, t / 1.6);
            double sweep = 0.5 - 0.5 * cos(2 * PI * fmax(0, t - d->loop) / period);
            double l = 0, r = 0;
            for (int k = 0; k < 6; k++) {
                double y = saw(&o[k], f * cents((k - 2.5) * 5.5));
                if (k & 1) r += y; else l += y;
            }
            double fc = 300 + f * (1.5 + 3.5 * sweep);
            double sb = 0.5 * sin(ph);
            ph += 2 * PI * f / 2 / SR; if (ph > 2 * PI) ph -= 2 * PI;
            L[i] = (float)((svf_lp(&fl, fc, 0.3, l) * 0.4 + sb) * env);
            R[i] = (float)((svf_lp(&fr, fc * 1.04, 0.3, r) * 0.4 + sb) * env);
        }
    } else {
        inst_def_t s = { 0, 0, K_STRINGS, 0, 0, 0, 1, d->len, d->loop, { 10, 12, 14, 14, 1.5, 4200 } };
        strings(&s, f, L, R, n);
        glass(f * 2, aL, aR, n);
        for (int i = 0; i < n; i++) {
            double t = i / SR, sh = 0.18 * fmin(1, t / 2.5);   /* the shimmer creeps in behind */
            L[i] = (float)(L[i] + sh * aL[i]); R[i] = (float)(R[i] + sh * aR[i]);
        }
    }
    free(aL); free(aR);
    bq_t lp[2]; for (int c = 0; c < 2; c++) bq_set(&lp[c], 0, 5200, 0.6, 0);
    for (int i = 0; i < n; i++) { L[i] = (float)bq(&lp[0], L[i]); R[i] = (float)bq(&lp[1], R[i]); }
}

/* Leads: monophonic voices for melodies, each with its own vibrato that grows in after the attack. */
static void lead(const inst_def_t *d, double f, float *L, float *R, int n) {
    int flavour = (int)d->p[0];
    osc_t o[4] = { {0}, {0.25}, {0.5}, {0.75} };
    svf_t fl = {0}, fr = {0};
    bq_t bp, hp; bq_set(&bp, 2, f * 1.0, 3, 0); bq_set(&hp, 1, 2000, 0.7, 0);
    double ph = 0, wob = 0, wt = 0;
    for (int i = 0; i < n; i++) {
        double t = i / SR, yl = 0, yr = 0;
        if ((i & 2047) == 0) wt = rnd() * 6;
        wob += 0.0004 * (wt - wob);   /* a slow, uneven drift, like an old instrument */
        if (flavour == 0) {   /* theremin: a near-sine with a soft second harmonic, a wide slow vibrato, a scoop into the note */
            double vib = 38 * fmin(1, fmax(0, t - 0.25) / 0.7) * sin(2 * PI * 5.7 * t) + wob - 60 * exp(-t * 9);
            double fr_ = f * cents(vib);
            double y = sin(ph) + 0.18 * sin(2 * ph + 0.4) + 0.05 * sin(3 * ph);
            ph += 2 * PI * fr_ / SR; if (ph > 2 * PI) ph -= 2 * PI;
            y *= fmin(1, t / 0.09);
            yl = yr = y * 0.8;
        } else if (flavour == 1) {   /* a filtered saw, a little square underneath, a soft resonant low-pass */
            double fr_ = f * cents(14 * fmin(1, fmax(0, t - 0.4) / 0.6) * sin(2 * PI * 5.2 * t) + wob);
            double x = saw(&o[0], fr_) + 0.3 * square(&o[1], fr_ * cents(-6));
            double fc = f * (2.2 + 2.5 * exp(-t * 5));
            yl = yr = svf_lp(&fl, fc, 0.5, x) * fmin(1, t / 0.01) * 0.7;
        } else if (flavour == 2) {   /* whistle: a pure tone, a hint of third harmonic, breath through a band-pass at the pitch */
            double vib = 24 * fmin(1, fmax(0, t - 0.3) / 0.5) * sin(2 * PI * 5.4 * t) + wob - 40 * exp(-t * 14);
            double fr_ = f * cents(vib);
            double y = sin(ph) + 0.05 * sin(3 * ph) + 0.35 * bq(&bp, rnd());
            ph += 2 * PI * fr_ / SR; if (ph > 2 * PI) ph -= 2 * PI;
            yl = yr = y * fmin(1, t / 0.05) * 0.8;
        } else if (flavour == 3) {   /* a 30% pulse (two saws a phase apart) and a second one 8 cents up, hollow and dark */
            double fr_ = f * cents(10 * fmin(1, fmax(0, t - 0.5) / 0.6) * sin(2 * PI * 4.8 * t) + wob);
            double p1 = saw(&o[0], fr_) - saw(&o[1], fr_), p2 = saw(&o[2], fr_ * cents(8)) - saw(&o[3], fr_ * cents(8));
            yl = svf_lp(&fl, f * 3.2, 0.25, p1 + 0.6 * p2) * 0.5;
            yr = svf_lp(&fr, f * 3.0, 0.25, p2 + 0.6 * p1) * 0.5;
            double env = fmin(1, t / 0.015); yl *= env; yr *= env;
        } else if (flavour == 4) {   /* three detuned saws and a square an octave down, a snarling resonant filter, driven */
            double fr_ = f * cents(wob);
            double s1 = saw(&o[0], fr_ * cents(-12)), s2 = saw(&o[1], fr_), s3 = saw(&o[2], fr_ * cents(9)), sq = square(&o[3], fr_ / 2);
            double fc = f * (2.0 + 4.0 * exp(-t * 3.5));
            yl = tanh(1.6 * svf_lp(&fl, fc, 0.65, s1 + s2 + 0.6 * sq)) * 0.6;
            yr = tanh(1.6 * svf_lp(&fr, fc * 1.03, 0.65, s3 + s2 + 0.6 * sq)) * 0.6;
            double env = fmin(1, t / 0.006); yl *= env; yr *= env;
        } else {   /* flute: a sine and its octave, breath above 2 kHz, a short chiff at the start */
            double fr_ = f * cents(16 * fmin(1, fmax(0, t - 0.35) / 0.5) * sin(2 * PI * 5.0 * t) + wob);
            double y = sin(ph) + 0.28 * sin(2 * ph) + 0.06 * sin(3 * ph);
            ph += 2 * PI * fr_ / SR; if (ph > 2 * PI) ph -= 2 * PI;
            double breath = bq(&hp, rnd()) * (0.07 + 0.5 * exp(-t * 30));
            y = y * fmin(1, t / 0.06) + breath;
            yl = yr = y * 0.7;
        }
        L[i] = (float)yl; R[i] = (float)yr;
    }
}

static void render_note(const inst_def_t *d, int note, float *L, float *R, int n) {
    double f = hz(note);
    switch (d->kind) {
    case K_PIANO: piano(d, f, note, L, R, n); break;
    case K_EP: ep(d, f, note, L, R, n, 0); break;
    case K_WURLI: ep(d, f, note, L, R, n, 1); break;
    case K_ORGAN: organ(d, f, L, R, n); break;
    case K_STRINGS: strings(d, f, L, R, n); break;
    case K_PIZZ: pizz(f, L, R, n); break;
    case K_CHOIR: choir(d, f, note, L, R, n); break;
    case K_BRASS: brass(d, f, L, R, n); break;
    case K_MUTE: muted(f, L, R, n); break;
    case K_SAX: sax(f, L, R, n); break;
    case K_DRONE: drone(d, f, L, R, n); break;
    case K_SUB: sub(f, L, R, n); break;
    case K_BASS: bass(f, L, R, n); break;
    case K_MUSICBOX: musicbox(f, L, R, n); break;
    case K_GLASS: glass(f, L, R, n); break;
    case K_VIBES: vibes(f, L, R, n); break;
    case K_PAD: pad(d, f, note, L, R, n); break;
    case K_LEAD: lead(d, f, L, R, n); break;
    }
}

/* ---- loop crossfade and WAV writing ---- */
static void make_loop(float *L, float *R, int ls, int le) {   /* equal-power crossfade of the loop's end into what precedes its start */
    int xf = (le - ls) / 3;
    if (xf > (int)(0.3 * SR)) xf = (int)(0.3 * SR);
    if (xf > ls) xf = ls;
    for (int i = 0; i < xf; i++) {
        double t = (double)i / xf, a = cos(t * PI / 2), b = sin(t * PI / 2);
        L[le - xf + i] = (float)(L[le - xf + i] * a + L[ls - xf + i] * b);
        R[le - xf + i] = (float)(R[le - xf + i] * a + R[ls - xf + i] * b);
    }
}

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xffff); w16(f, v >> 16); }

static int write_wav(const char *path, const float *L, const float *R, int n, int stereo, int ls, int le, int root, double gain) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ch = stereo ? 2 : 1, data = n * ch * 2, smpl = le > 0 ? 60 : 0;
    fwrite("RIFF", 1, 4, f); w32(f, (unsigned)(4 + 24 + 8 + data + (smpl ? 8 + smpl : 0))); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(f, 16); w16(f, 1); w16(f, (unsigned)ch); w32(f, 44100); w32(f, (unsigned)(44100 * ch * 2)); w16(f, (unsigned)(ch * 2)); w16(f, 16);
    if (smpl) {
        fwrite("smpl", 1, 4, f); w32(f, (unsigned)smpl);
        w32(f, 0); w32(f, 0); w32(f, 22676); w32(f, (unsigned)root); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 1); w32(f, 0);
        w32(f, 0); w32(f, 0); w32(f, (unsigned)ls); w32(f, (unsigned)(le - 1)); w32(f, 0); w32(f, 0);
    }
    fwrite("data", 1, 4, f); w32(f, (unsigned)data);
    for (int i = 0; i < n; i++) {
        double l = L[i] * gain, r = R[i] * gain;
        if (!stereo) l = 0.7 * (l + r);
        int a = (int)lrint(fmax(-1, fmin(1, l)) * 32767), b = (int)lrint(fmax(-1, fmin(1, r)) * 32767);
        w16(f, (unsigned)(a & 0xffff));
        if (stereo) w16(f, (unsigned)(b & 0xffff));
    }
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: render_rom <out dir> [catalogue.json]\n"); return 2; }
    if ((int)(sizeof DESC / sizeof DESC[0]) != NINST) { fprintf(stderr, "render_rom: DESC needs one line per sound\n"); return 2; }
    mkdir(argv[1], 0755);
    FILE *cat = argc > 2 ? fopen(argv[2], "w") : NULL;
    if (cat) fprintf(cat, "{\n \"sounds\": [\n");
    long total = 0;
    for (int k = 0; k < NINST; k++) {
        const inst_def_t *d = &INST[k];
        char dir[512], path[700];
        snprintf(dir, sizeof dir, "%s/%s", argv[1], d->folder);
        mkdir(dir, 0755);
        snprintf(path, sizeof path, "%s/info.txt", dir);
        FILE *inf = fopen(path, "w");
        if (inf) { fprintf(inf, "modes=%s\ndesc=%s\n", d->modes, DESC[k]); fclose(inf); }
        const char *name = strstr(d->folder, " - ") ? strstr(d->folder, " - ") + 3 : d->folder;
        const char *disp = d->folder; while (*disp >= '0' && *disp <= '9') disp++; while (*disp == ' ') disp++;
        if (cat) fprintf(cat, "  {\"folder\": \"%s\", \"name\": \"%s\", \"modes\": \"%s\"}%s\n", d->folder, disp, d->modes, k < NINST - 1 ? "," : "");
        for (int note = d->lo; note <= d->hi; note += d->step) {
            int n = (int)(d->len * SR);
            if (d->kind == K_PIANO || d->kind == K_MUSICBOX || d->kind == K_VIBES || d->kind == K_EP || d->kind == K_WURLI || d->kind == K_PIZZ) {
                double sh = fmax(0.45, 1 - (note - d->lo) / 110.0);   /* high notes die sooner: shorter files */
                n = (int)(n * sh);
            }
            float *L = calloc((size_t)n, sizeof(float)), *R = calloc((size_t)n, sizeof(float));
            rs = 0x1234567u + (uint32_t)(k * 131 + note * 7919);
            render_note(d, note, L, R, n);
            int ls = 0, le = 0;
            if (d->loop > 0) { ls = (int)(d->loop * SR); le = n; make_loop(L, R, ls, le); }
            double heat = d->kind == K_BRASS || d->kind == K_PIANO ? 0.6 : 0.4;
            print_chain(L, R, n, heat);
            double peak = 1e-9;
            for (int i = 0; i < n; i++) {
                if (!isfinite(L[i]) || !isfinite(R[i])) { fprintf(stderr, "render_rom: %s note %d: non-finite sample at %d\n", d->folder, note, i); return 1; }
                peak = fmax(peak, d->stereo ? fmax(fabs(L[i]), fabs(R[i])) : fabs(0.7 * (L[i] + R[i])));   /* as write_wav writes it */
            }
            snprintf(path, sizeof path, "%s/%s %03d.wav", dir, name, note);
            if (!write_wav(path, L, R, n, d->stereo, ls, le, note, 0.79 / peak)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            total += (long)n * (d->stereo ? 4 : 2);
            free(L); free(R);
        }
        printf("%-34s %s\n", d->folder, d->modes);
        fflush(stdout);
    }
    if (cat) { fprintf(cat, " ]\n}\n"); fclose(cat); }
    printf("%d sounds, %.1f MB\n", NINST, total / 1048576.0);
    return 0;
}
