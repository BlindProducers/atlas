/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "atlas_openevv.h"

#include <pthread.h>
#include <string.h>

#include "klatt_rates.h"
#include "klatt_state.h"

/* OpenEVV's start-up, which its library runs from a constructor. */
void evv_port_start(void);
void evvRunStaticInitialisers(void);

/* The rate the voices may run at besides IBM's 11,025, and its two tables. */
#define WIDE_RATE 22050
static int16_t wide_ex[KLATT_EX_COUNT];
static int16_t wide_co[KLATT_CO_COUNT];

static pthread_once_t started = PTHREAD_ONCE_INIT;

static volatile AtlasFrameTap tap;
static void *volatile tap_user;

typedef char state_fits[sizeof(klatt_state) <= ATLAS_KLATT_STATE_BYTES ? 1 : -1];

/* A voice's synthesiser carries this as its error reporter, which tells it
   from the engine's in atlas_openevv_klatt_frame. */
static void voice_error(void *user, const char *tag, const char *message)
{
    (void)user;
    (void)tag;
    (void)message;
}

static void start_once(void)
{
    unsigned char probe[ATLAS_KLATT_STATE_BYTES];

    evv_port_start();
    evvRunStaticInitialisers();
    klatt_buildRateTables(WIDE_RATE, wide_ex, wide_co);
    /* The noise filter reads its environment switch on first use; do that
       here rather than on the audio thread. */
    atlas_klatt_open(probe, 11025, 0, 100, 0, 0);
    atlas_klatt_open(probe, WIDE_RATE, 0, 100, 0, 0);
}

void atlas_openevv_start(void)
{
    pthread_once(&started, start_once);
}

void atlas_openevv_set_tap(AtlasFrameTap new_tap, void *user)
{
    tap_user = user;
    tap = new_tap;
}

/* Called by the patched KlattSynth with every frame, before it is synthesised. */
void atlas_openevv_klatt_frame(const void *handle, const int32_t *parms)
{
    const klatt_state *k = handle;
    AtlasFrameTap current = tap;
    AtlasKlattVoice voice;

    if (current == 0 || k->cp.error_fn == (klatt_error_fn)voice_error)
        return;
    /* Only the engine's 11,025 hertz synthesiser, not a wideband companion. */
    if (k->cp.sample_rate != 11025 || k->pace_rate != 0)
        return;
    voice.add_20 = k->cp.unknown_20;
    voice.add_24 = k->cp.unknown_24;
    voice.add_28 = k->cp.unknown_28;
    voice.add_2c = k->cp.unknown_2c;
    voice.n_formants = k->cp.n_formants;
    current(tap_user, parms, &voice, k->volume);
}

typedef struct {
    AtlasKlattSink sink;
    void *user;
} sink_target;

/* KlattSynth hands its samples to a callback with the state's user pointer;
   the sink and its user are kept in the bytes after the state. */
static sink_target *target_of(void *state)
{
    return (sink_target *)((unsigned char *)state + ATLAS_KLATT_STATE_BYTES - sizeof(sink_target));
}

typedef char target_fits[sizeof(klatt_state) + sizeof(sink_target) <= ATLAS_KLATT_STATE_BYTES ? 1 : -1];

static int deliver(void *user, KlattSamplesStruct *s)
{
    sink_target *target = user;

    if (target->sink != 0 && s->count > 0)
        target->sink(target->user, s->samples, s->count);
    return 1;
}

int atlas_klatt_open(void *state, int32_t rate, const AtlasKlattVoice *voice, int32_t volume,
                     AtlasKlattSink sink, void *user)
{
    klatt_state *k = state;
    sink_target *target = target_of(state);
    KlattConstParms cp;

    if (rate != 11025 && rate != WIDE_RATE)
        return 0;

    /* What klatt_new does, in the voice's own memory. */
    memset(k, 0, sizeof *k);
    k->version = KlattVersionString;
    k->user = target;
    k->output_samples = 1;
    target->sink = sink;
    target->user = user;

    if (rate == WIDE_RATE)
        KlattSetRateTables(k, wide_ex, wide_co);

    /* What makeSound builds in src/klatt/klatt_run.c. */
    memset(&cp, 0, sizeof cp);
    cp.unknown_00 = 100;
    cp.sample_rate = rate;
    cp.unknown_08 = 16;
    cp.n_formants = 5;
    cp.unknown_10 = 8;
    cp.unknown_14 = 1;
    if (voice != 0) {
        cp.unknown_20 = voice->add_20;
        cp.unknown_24 = voice->add_24;
        cp.unknown_28 = voice->add_28;
        cp.unknown_2c = voice->add_2c;
        if (voice->n_formants > 0)
            cp.n_formants = voice->n_formants;
    }
    cp.error_fn = (klatt_error_fn)voice_error;
    cp.callback_mode = 2;
    cp.samples_fn = deliver;
    KlattSetConstParms(k, cp);
    klattSetVolumeMultiplier(k, volume);
    return KlattOpen(k);
}

void atlas_klatt_set_sink(void *state, AtlasKlattSink sink, void *user)
{
    klatt_state *k = state;
    sink_target *target = target_of(state);

    k->user = target;
    target->sink = sink;
    target->user = user;
}

void atlas_klatt_set_volume(void *state, int32_t volume)
{
    klattSetVolumeMultiplier(state, volume);
}

int atlas_klatt_synth(void *state, const int32_t *parms)
{
    return KlattSynth(state, parms);
}

int32_t atlas_klatt_frame_samples(const void *state, const int32_t *parms)
{
    const klatt_state *k = state;

    /* KlattSynth's own count, with no pacing partner. */
    return (int32_t)((int64_t)parms[0] * k->cp.unknown_00 * k->cp.sample_rate / 100000);
}
