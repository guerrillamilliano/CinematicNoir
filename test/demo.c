/* Offline audition and load test: loads each engine preset (src/presets.h) into a fresh engine, waits for its sound to
 * load from rom/ next to this binary (so build it into build/), plays a minor progression and writes
 * build/demo/<Category> - <name>.wav. Checks the right sound loaded, the category followed, and the display readouts.
 *   build/demo_run [all]      (default: each sound's first preset) */
#include "../src/noir.c"
#include <unistd.h>

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xffff); w16(f, v >> 16); }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* renders n blocks of a held C minor chord then silence; returns the RMS of blocks [from, to) (in 128-frame blocks) and the
 * share of samples near full scale */
static double play_rms(noir_t *e, int on_blocks, int total, int from, int to, double *hot) {
    static const uint8_t CH[3] = { 48, 51, 55 };
    int16_t blk[256]; double acc = 0; long n = 0, h = 0;
    for (int b = 0; b < total; b++) {
        for (int k = 0; k < 3; k++) {
            uint8_t m[3] = { (uint8_t)(b == on_blocks ? 0x80 : 0x90), CH[k], 100 };
            if (b == 0 || b == on_blocks) midi(e, m, 3);
        }
        render(e, blk, 128);
        for (int i = 0; i < 256; i++) {
            if (b >= from && b < to) { acc += (double)blk[i] * blk[i]; n++; }
            if (abs(blk[i]) > 32000) h++;
        }
    }
    if (hot) *hot = (double)h / (total * 256.0);
    return n ? sqrt(acc / n) : 0;
}

/* a held chord (1 s) then 1.5 s: s[0] rms while held, s[1] brightness (rms of the first difference / rms) while held,
 * s[2] width (side rms / mid rms) while held, s[3] rms 0.3 .. 1.5 s after the release (the tail; the voices end in ~0.1 s) */
static void fx_stats(noir_t *e, double *s) {
    static const uint8_t CH[3] = { 48, 51, 55 };
    int16_t blk[256]; double a = 0, dsum = 0, mid = 0, side = 0, tail = 0; long n = 0, nt = 0; int pl = 0;
    for (int b = 0; b < 860; b++) {
        for (int k = 0; k < 3; k++) {
            uint8_t m[3] = { (uint8_t)(b == 345 ? 0x80 : 0x90), CH[k], 100 };
            if (b == 0 || b == 345) midi(e, m, 3);
        }
        render(e, blk, 128);
        for (int i = 0; i < 128; i++) {
            int l = blk[2 * i], r = blk[2 * i + 1];
            if (b >= 100 && b < 340) {
                a += 0.5 * ((double)l * l + (double)r * r); dsum += (double)(l - pl) * (l - pl);
                mid += 0.25 * (double)(l + r) * (l + r); side += 0.25 * (double)(l - r) * (l - r); n++;
            }
            if (b >= 448 && b < 860) { tail += 0.5 * ((double)l * l + (double)r * r); nt++; }
            pl = l;
        }
    }
    s[0] = sqrt(a / n); s[1] = sqrt(dsum / n) / (s[0] + 1e-9); s[2] = sqrt(side / n) / (sqrt(mid / n) + 1e-9); s[3] = sqrt(tail / nt);
}

static noir_t *loaded(const char *state) {
    noir_t *e = create(NULL);
    set_param(e, "state", state);
    int16_t blk[256];
    for (int k = 0; k < 2500; k++) {   /* wait for the sound (5 s at most) */
        render(e, blk, 128);
        pthread_mutex_lock(&e->mu);
        int ready = e->cur != e->fallback && !e->pending && e->req_gen == e->done_gen;
        pthread_mutex_unlock(&e->mu);
        if (ready) break;
        usleep(2000);
    }
    return e;
}

/* Cracked Vinyl FOLLOW stops with the notes; the MOOD effects change the sound; the ghost and echo loops stay bounded */
static int fx_tests(void) {
    int fails = 0;
    const char *dry = "cutoff=0.85;space=0;echo_mix=0;shadow=0;shimmer=0;drift=0;tape_highcut=1;sname=Keys - Dark Grand;";
    char st[512];
    /* 1 s chord, released, then 3 s: blocks 345 .. 690 lie 0.6 s .. 1.6 s after the release (released voices fade in ~0.3 s) */
    snprintf(st, sizeof st, "%svinyl=90;vinyl_follow=0;", dry);
    noir_t *e = loaded(st);
    double held = play_rms(e, 345, 1380, 100, 340, NULL);
    destroy(e);
    e = loaded(st);
    double after = play_rms(e, 345, 1380, 550, 1380, NULL);
    printf("     after release: %d voices, vinyl gate %.4f\n", e->nactive, e->vy_gate);
    destroy(e);
    snprintf(st, sizeof st, "%svinyl=0;", dry);
    e = loaded(st);
    printf("     the same with no vinyl: rms %.1f after release\n", play_rms(e, 345, 1380, 550, 1380, NULL));
    destroy(e);
    snprintf(st, sizeof st, "%svinyl=90;vinyl_follow=0;", dry);
    snprintf(st, sizeof st, "%svinyl=90;vinyl_follow=1;", dry);
    e = loaded(st);
    double always = play_rms(e, 345, 1380, 550, 1380, NULL);
    destroy(e);
    int ok = held > 300 && after < 5 && always > 20;
    fails += !ok;
    printf("%s vinyl FOLLOW: while playing rms %.0f, 0.6-3 s after release %.1f (silent); ALWAYS keeps going (%.1f)\n",
           ok ? "ok  " : "FAIL", held, after, always);
    /* each MOOD effect does what it says, measured where it acts: loudness, brightness (the first difference against the
     * signal), width (side against mid), and the tail after the release */
    static const struct { const char *set, *what; int stat, dir; } FX[] = {
        { "tone_tilt=0;", "darker", 1, -1 }, { "tone_tilt=100;", "brighter", 1, +1 }, { "tone_warmth=100;", "fuller", 0, +1 },
        { "tone_air=100;", "brighter", 1, +1 }, { "shadow=100;", "a tail", 3, +1 }, { "shimmer=100;", "a tail", 3, +1 },
        { "echo_mix=100;echo_feedback=60;", "a tail", 3, +1 }, { "ensemble=100;", "wider", 2, +1 },
        { "pulse_depth=100;", "pulsing (quieter on average)", 0, -1 } };
    double flat[4];
    snprintf(st, sizeof st, "%svinyl=0;", dry);
    e = loaded(st); fx_stats(e, flat); destroy(e);
    {   /* Air lifts what is above 4.5 kHz: the dark piano has almost none, so it is measured on bright strings */
        const char *bright = "cutoff=1;space=0;echo_mix=0;shadow=0;shimmer=0;drift=0;tape_highcut=1;vinyl=0;mode=0;sname=Strings - Tremolo Panic;";
        double f2[4], a2[4];
        e = loaded(bright); fx_stats(e, f2); destroy(e);
        snprintf(st, sizeof st, "%stone_air=100;", bright);
        e = loaded(st); fx_stats(e, a2); destroy(e);
        int ok3 = a2[1] > f2[1] * 1.05;
        fails += !ok3;
        printf("%s tone_air=100 (bright strings)     brighter: brightness %.3g (flat %.3g)\n", ok3 ? "ok  " : "FAIL", a2[1], f2[1]);
    }
    for (int k = 0; k < (int)(sizeof FX / sizeof FX[0]); k++) {
        if (!strcmp(FX[k].set, "tone_air=100;")) continue;   /* above */
        snprintf(st, sizeof st, "%svinyl=0;%s", dry, FX[k].set);
        e = loaded(st);
        double s[4]; fx_stats(e, s);
        destroy(e);
        double ref = flat[FX[k].stat], v = s[FX[k].stat];
        int ok2 = FX[k].stat == 3 ? v > ref * 3 + 20 : (v - ref) * FX[k].dir > 0.05 * ref;
        fails += !ok2;
        static const char *const STAT[4] = { "rms", "brightness", "width", "tail rms" };
        printf("%s %-32s %s: %s %.3g (flat %.3g)\n", ok2 ? "ok  " : "FAIL", FX[k].set, FX[k].what, STAT[FX[k].stat], v, ref);
    }
    /* everything at the top: the feedback loops must settle, not run away or pin the output */
    snprintf(st, sizeof st, "%svinyl=0;space=100;space_type=2;shadow=100;shimmer=100;ghost_tail=100;echo_mix=100;echo_feedback=95;", dry);
    e = loaded(st);
    double hot, tail = play_rms(e, 690, 3445, 2800, 3445, &hot);   /* 2 s on, 8 s of tails: the last 1.8 s */
    int sane = isfinite(e->ghost_fb) && hot < 0.02 && tail < 12000;
    destroy(e);
    fails += !sane;
    printf("%s extremes: %.2f%% of samples near full scale, tail rms %.0f after 8 s (bounded)\n", sane ? "ok  " : "FAIL", 100 * hot, tail);
    return fails;
}

/* A mode tap (as from the screen's segment) in every category, from every mode to every other: it must load a preset of
 * the new mode in the same category with a different sound, and report the moved parameters to the host. */
static int mode_tests(void) {
    int fails = 0, checks = 0;
    for (int start = 0; start < NOIR_NPRESETS; start++)   /* from every preset */
            for (int to = 0; to < 3; to++) {
                int cat = NOIR_PRESETS[start].cat, from = NOIR_PRESETS[start].mode;
                if (from == to) continue;
                noir_t *e = create(NULL);
                char v[16]; snprintf(v, sizeof v, "%d", start);
                set_param(e, "preset", v);
                char key[64]; while (mpc_engine_changed(e, key, sizeof key)) {}   /* the load's own reports */
                char before[64]; snprintf(before, sizeof before, "%s", e->sname);
                float cut0 = e->val[P_CUTOFF], drive0 = e->val[P_DRIVE], space0 = e->val[P_SPACE];
                snprintf(v, sizeof v, "%d", to);
                set_param(e, "mode", v);
                int p = opt(e, P_PRESET), reported = 0;
                while (mpc_engine_changed(e, key, sizeof key)) reported++;
                int ok = NOIR_PRESETS[p].mode == to && opt(e, P_MODE) == to && NOIR_PRESETS[p].cat == cat && opt(e, P_CATEGORY) == cat
                         && strcmp(e->sname, before) && reported > 20;
                checks++;
                if (!ok) {
                    fails++;
                    printf("FAIL mode %s -> %s in category %d: preset \"%s\" (mode %d, cat %d), sound %s -> %s, %d reported\n",
                           OPT_MODE[from], OPT_MODE[to], cat, NOIR_PRESETS[p].name, NOIR_PRESETS[p].mode, NOIR_PRESETS[p].cat,
                           before, e->sname, reported);
                } else if (cat == 0 && start < 7)
                    printf("ok   Keys %s -> %s: \"%s\" -> \"%s\" (%s), cutoff %.2f->%.2f drive %.0f->%.0f space %.0f->%.0f\n",
                           OPT_MODE[from], OPT_MODE[to], NOIR_PRESETS[start].name, NOIR_PRESETS[p].name, e->sname,
                           cut0, e->val[P_CUTOFF], drive0, e->val[P_DRIVE], space0, e->val[P_SPACE]);
                destroy(e);
            }
    printf("%s mode switches: %d of %d load a new sound of the new mode in the same category\n", fails ? "FAIL" : "ok  ", checks - fails, checks);
    return fails;
}

int main(int argc, char **argv) {
    static const int CH[4][4] = { { 48, 51, 55, 58 }, { 44, 48, 51, 55 }, { 41, 44, 48, 51 }, { 43, 47, 50, 53 } };   /* Cm7 Ab F m G7 */
    int all = argc > 1 && !strcmp(argv[1], "all");
    double render_s = 0, audio_s = 0;
    int fails = fx_tests() + mode_tests();
    char last_sound[64] = "";
    mkdir("build/demo", 0755);
    for (int p = 0; p < NOIR_NPRESETS; p++) {
        const char *sn = strstr(NOIR_PRESETS[p].state, "sname=") + 6;
        char want[64]; snprintf(want, sizeof want, "%.*s", (int)strcspn(sn, ";"), sn);
        if (!all && !strcmp(want, last_sound)) continue;
        snprintf(last_sound, sizeof last_sound, "%s", want);
        noir_t *e = create(NULL);
        char pv[16]; snprintf(pv, sizeof pv, "%d", p);
        set_param(e, "preset", pv);
        int16_t blk[256];
        double t0 = now();
        while (now() - t0 < 5) {   /* wait for the worker: the sound is current once nothing is pending */
            render(e, blk, 128);
            pthread_mutex_lock(&e->mu);
            int ready = e->cur != e->fallback && !e->pending && e->req_gen == e->done_gen;
            pthread_mutex_unlock(&e->mu);
            if (ready) break;
            usleep(2000);
        }
        double load = now() - t0;
        char name[96], path[700], info[128], disp[128];
        snprintf(pv, sizeof pv, "preset:%d", p);
        get_param(e, pv, name, sizeof name);
        snprintf(path, sizeof path, "build/demo/%s.wav", name);
        for (char *c = path + 11; *c; c++) if (*c == ':') *c = ' ';
        FILE *f = fopen(path, "wb");
        int frames = (int)(9.0 * SR) / 128 * 128;
        fwrite("RIFF", 1, 4, f); w32(f, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, f); w32(f, 16); w16(f, 1); w16(f, 2);
        w32(f, 44100); w32(f, 44100 * 4); w16(f, 4); w16(f, 16); fwrite("data", 1, 4, f); w32(f, frames * 4);
        double r0 = now();
        long peak = 0;
        char playing[128] = "";
        int wave_ok = 0, head0 = 0, head_moved = 0;
        for (int b = 0; b < frames / 128; b++) {
            int t = b * 128;
            for (int c = 0; c < 4; c++) {
                int on = (int)(c * 1.6 * SR), off = on + (int)(1.4 * SR);
                for (int k = 0; k < 4; k++) {
                    uint8_t m[3] = { 0x90, (uint8_t)CH[c][k], 96 };
                    if (on >= t && on < t + 128) midi(e, m, 3);
                    m[0] = 0x80;
                    if (off >= t && off < t + 128) midi(e, m, 3);
                }
            }
            render(e, blk, 128);
            if (b == 20) {   /* 58 ms in: even a chopped stab still sounds */
                get_param(e, "disp_play", playing, sizeof playing);
                char rv[16]; get_param(e, "display_rev", rv, sizeof rv);
                int shape = 0;
                for (int c = 0; c < NWCOL; c++) shape += e->vis[c];
                /* a waveform (a rom sound: its drawn picture, lit while playing, and no bars; otherwise the bars), a playhead, the lamp lit */
                int pic = e->vis[NWCOL + 2];
                wave_ok = (e->cur->rom_idx >= 0 ? pic == e->cur->rom_idx + 1 + WAVE_IMGS && shape == 0 : pic == 0 && shape > NWCOL)
                          && e->vis[NWCOL] >= 1 && e->vis[NWCOL + 1] == 1;
                head0 = e->vis[NWCOL];
            }
            if (b == 120) {   /* Stutter sends the playhead back to the start each step, so it may read the same column */
                char rv[16]; get_param(e, "display_rev", rv, sizeof rv);
                head_moved = e->vis[NWCOL] != head0 || e->vis[NWCOL] == 0 || opt(e, P_STUTTER) > 0;
            }
            for (int i = 0; i < 256; i++) { w16(f, (unsigned)(blk[i] & 0xffff)); if (labs(blk[i]) > peak) peak = labs(blk[i]); }
        }
        render_s += now() - r0; audio_s += frames / SR;
        fclose(f);
        get_param(e, "disp_info", info, sizeof info);
        get_param(e, "preset_display", disp, sizeof disp);
        int ok = e->cur != e->fallback && !strcmp(e->cur->name, want) && peak > 300 && opt(e, P_CATEGORY) == NOIR_PRESETS[p].cat
                 && !strcmp(disp, NOIR_PRESETS[p].name)
                 && strstr(playing, opt(e, P_VOICE_MODE) ? " 1 VOICE" : "4 VOICES")   /* MONO plays a chord as one voice */
                 && wave_ok && head_moved;
        if (!ok) fails++;
        char wave[NWCOL + 1];
        for (int c = 0; c < NWCOL; c++) wave[c] = " .:-=+*#%@@@"[e->vis_inst ? ((const inst_t *)e->vis_inst)->wave[c] : 0];
        wave[NWCOL] = 0;
        printf("%s %-30s load %4.0f ms  picture %2d [%s]  %s | %s\n", ok ? "ok  " : "FAIL", name, load * 1000,
               e->vis[NWCOL + 2], wave, info, playing);
        if (!wave_ok || !head_moved) printf("     waveform %s, playhead %s\n", wave_ok ? "ok" : "MISSING", head_moved ? "moves" : "STUCK");
        destroy(e);
    }
    printf("render speed: %.1fx real time on this machine\n", audio_s / (render_s > 0 ? render_s : 1));
    printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails != 0;
}
