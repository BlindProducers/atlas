/* The OpenB3 reference renderer: the untouched Beatrix engine from the opt-in
 * third_party/OpenB3 submodule (setBfree's tonegen.c, vibrato.c, whirl.c and
 * eqcomp.c), driven as OpenB3 drives it: no configuration file, 64-sample
 * fragments. AtlasOpenB3Tests runs it and compares Atlas against what it
 * writes. Commands:
 *
 *   wiring OUT                      keyContrib for upper manual keys 0-60
 *   tables RATE OUT                 wheel frequencies (per temperament), the
 *                                   loop frequencies at RATE and key compression
 *   gains RATE DRAWBARS PERC KEYS OUT
 *                                   per wheel sumSwell and sumPercn after the keys
 *   tonegen RATE DRAWBARS PERC FRAGMENTS EVENTS OUT ENVELOPES [RELEASE]
 *                                   oscGenerateFragment's output, and the attack
 *                                   then release envelopes; RELEASE "click" picks
 *                                   osc.release.model click (default linear)
 *   scanner RATE SELECT HZ IN OUT   vibratoProc over IN
 *   whirl RATE EVENTS IN OUT        whirlProc3 over IN (left, right interleaved)
 *
 * DRAWBARS: nine digits, 16' to 1'. PERC: four digits, enabled soft fast first
 * (setPercussionFirst: 1 = second harmonic). KEYS: comma-separated key
 * numbers. EVENTS: comma-separated FRAGMENT:on|off:KEY for tonegen, or
 * FRAGMENT:SPEED for whirl (setRevSelect: 0 slow, 1 stop, 2 fast). Floats are
 * written as 32-bit samples, numbers in tables as %a.
 */
#include "global_inst.h"
#include "global_definitions.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The MIDI and configuration hooks the engine calls; nothing here uses them. */
void useMIDIControlFunction (void* m, const char* cfname, void (*f) (void*, unsigned char), void* d) { }
void notifyControlChangeByName (void* mcfg, const char* cfname, unsigned char val) { }
void notifyControlChangeById (void* mcfg, int id, unsigned char val) { }
void showConfigfileContext (ConfigContext* cfg, const char* msg) { }
void configIntOutOfRange (ConfigContext* cfg, int min, int max) { }
void configDoubleUnparsable (ConfigContext* cfg) { }
const char* getConfigValue (ConfigContext* cfg) { return ""; }
int getConfigParameter_d (const char* par, ConfigContext* cfg, double* dp) { return 0; }
int getConfigParameter_i (const char* par, ConfigContext* cfg, int* ip) { return 0; }
int getConfigParameter_ir (const char* par, ConfigContext* cfg, int* ip, int lowest, int highest) { return 0; }
int getConfigParameter_dr (const char* par, ConfigContext* cfg, double* dp, double lowest, double highest) { return 0; }

static FILE* openOut (const char* path, const char* mode) {
	FILE* f = fopen (path, mode);
	if (!f) {
		perror (path);
		exit (1);
	}
	return f;
}

static float* readFloats (const char* path, long* count) {
	FILE* f = openOut (path, "rb");
	fseek (f, 0, SEEK_END);
	*count = ftell (f) / (long)sizeof (float);
	fseek (f, 0, SEEK_SET);
	float* data = (float*)calloc ((size_t)*count + BUFFER_SIZE_SAMPLES, sizeof (float));
	if (fread (data, sizeof (float), (size_t)*count, f) != (size_t)*count) {
		fprintf (stderr, "short read: %s\n", path);
		exit (1);
	}
	fclose (f);
	return data;
}

/* An upper manual set up as OpenB3's plugin sets it. */
static struct b_tonegen* makeTonegenWith (double rate, const char* drawbars, const char* perc, int release) {
	SampleRateD = rate;
	srand (1);
	struct b_tonegen* t = allocTonegen ();
	setEnvReleaseModel (t, release);
	initToneGenerator (t, NULL);
	b_instance inst;
	memset (&inst, 0, sizeof (inst));
	inst.synth = t;
	unsigned int setting[9];
	for (int i = 0; i < 9; ++i)
		setting[i] = (unsigned int)(drawbars[i] - '0');
	setDrawBars (&inst, UPPER_MANUAL, setting);
	setPercussionFirst (t, perc[3] == '1');
	setPercussionVolume (t, perc[1] == '1');
	setPercussionFast (t, perc[2] == '1');
	setPercussionEnabled (t, perc[0] == '1');
	return t;
}

static struct b_tonegen* makeTonegen (double rate, const char* drawbars, const char* perc) {
	return makeTonegenWith (rate, drawbars, perc, ENV_LINEAR);
}

static int wiring (const char* out) {
	struct b_tonegen* t = makeTonegen (48000.0, "886000000", "0011");
	FILE* f = openOut (out, "w");
	for (int k = 0; k <= 60; ++k) {
		for (ListElement* lep = t->keyContrib[k]; lep != NULL; lep = lep->next)
			fprintf (f, "%d %d %d %a\n", k, lep->u.ssf.sa, lep->u.ssf.sb, lep->u.ssf.fc);
	}
	fclose (f);
	return 0;
}

static int tables (double rate, const char* out) {
	FILE* f = openOut (out, "w");
	const int tunings[3] = { 1, 2, 0 }; /* gear60, gear50, equal */
	for (int i = 0; i < 3; ++i) {
		SampleRateD = rate;
		struct b_tonegen* t = allocTonegen ();
		t->gearTuning = tunings[i];
		initToneGenerator (t, NULL);
		for (int w = 1; w <= NOF_WHEELS; ++w) {
			const struct _oscillator* o = &t->oscillators[w];
			const double waves = rint (o->frequency * (double)o->lengthSamples / rate);
			fprintf (f, "frequency %d %d %a %a\n", i, w, o->frequency, waves * rate / (double)o->lengthSamples);
		}
		if (i == 0) {
			for (int k = 0; k < MAXKEYS; ++k)
				fprintf (f, "keycomp %d %a\n", k, (double)t->keyCompTable[k]);
		}
		freeToneGenerator (t);
	}
	fclose (f);
	return 0;
}

static void parseKeys (const char* keys, int* list, int* count) {
	*count = 0;
	const char* s = keys;
	while (s && *s) {
		list[(*count)++] = atoi (s);
		s = strchr (s, ',');
		if (s)
			++s;
	}
}

static int gains (double rate, const char* drawbars, const char* perc, const char* keys, const char* out) {
	struct b_tonegen* t = makeTonegen (rate, drawbars, perc);
	float buffer[BUFFER_SIZE_SAMPLES];
	oscGenerateFragment (t, buffer, BUFFER_SIZE_SAMPLES);
	int list[128], count;
	parseKeys (keys, list, &count);
	for (int i = 0; i < count; ++i)
		oscKeyOn (t, (unsigned char)list[i], (unsigned char)list[i]);
	oscGenerateFragment (t, buffer, BUFFER_SIZE_SAMPLES);
	FILE* f = openOut (out, "w");
	for (int w = 1; w <= NOF_WHEELS; ++w) {
		if (t->aot[w].refCount > 0)
			fprintf (f, "%d %a %a\n", w, (double)t->aot[w].sumSwell, (double)t->aot[w].sumPercn);
	}
	fclose (f);
	return 0;
}

static int tonegen (double rate, const char* drawbars, const char* perc, int fragments, const char* events,
                    const char* out, const char* envelopes, const char* release) {
	struct b_tonegen* t = makeTonegenWith (rate, drawbars, perc,
	                                       release && !strcmp (release, "click") ? ENV_CLICK : ENV_LINEAR);
	FILE* e = openOut (envelopes, "wb");
	for (int b = 0; b < 8; ++b)
		fwrite (t->attackEnv[b], sizeof (float), BUFFER_SIZE_SAMPLES, e);
	for (int b = 0; b < 8; ++b)
		fwrite (t->releaseEnv[b], sizeof (float), BUFFER_SIZE_SAMPLES, e);
	fclose (e);
	FILE* f = openOut (out, "wb");
	float buffer[BUFFER_SIZE_SAMPLES];
	for (int fragment = 0; fragment < fragments; ++fragment) {
		const char* s = events;
		while (s && *s) {
			int at = 0, key = 0;
			char kind[8] = { 0 };
			if (sscanf (s, "%d:%7[^:]:%d", &at, kind, &key) == 3 && at == fragment) {
				if (!strcmp (kind, "on"))
					oscKeyOn (t, (unsigned char)key, (unsigned char)key);
				else
					oscKeyOff (t, (unsigned char)key, (unsigned char)key);
			}
			s = strchr (s, ',');
			if (s)
				++s;
		}
		oscGenerateFragment (t, buffer, BUFFER_SIZE_SAMPLES);
		fwrite (buffer, sizeof (float), BUFFER_SIZE_SAMPLES, f);
	}
	fclose (f);
	return 0;
}

static int scanner (double rate, int select, double hz, const char* in, const char* out) {
	SampleRateD = rate;
	struct b_tonegen* t = allocTonegen ();
	t->inst_vibrato.vibFqHertz = hz;
	init_vibrato (&t->inst_vibrato);
	setVibrato (t, select);
	long count;
	float* input = readFloats (in, &count);
	float* output = (float*)calloc ((size_t)count + BUFFER_SIZE_SAMPLES, sizeof (float));
	for (long offset = 0; offset < count; offset += BUFFER_SIZE_SAMPLES)
		vibratoProc (&t->inst_vibrato, input + offset, output + offset, BUFFER_SIZE_SAMPLES);
	FILE* f = openOut (out, "wb");
	fwrite (output, sizeof (float), (size_t)count, f);
	fclose (f);
	return 0;
}

static int whirl (double rate, const char* events, const char* in, const char* out) {
	struct b_whirl* w = allocWhirl ();
	initWhirl (w, NULL, rate);
	long count;
	float* input = readFloats (in, &count);
	FILE* f = openOut (out, "wb");
	float left[BUFFER_SIZE_SAMPLES], right[BUFFER_SIZE_SAMPLES], tmp_left[BUFFER_SIZE_SAMPLES],
	    tmp_right[BUFFER_SIZE_SAMPLES], frame[2 * BUFFER_SIZE_SAMPLES];
	int fragment = 0;
	for (long offset = 0; offset < count; offset += BUFFER_SIZE_SAMPLES, ++fragment) {
		const char* s = events;
		while (s && *s) {
			int at = 0, speed = 0;
			if (sscanf (s, "%d:%d", &at, &speed) == 2 && at == fragment)
				setRevSelect (w, speed);
			s = strchr (s, ',');
			if (s)
				++s;
		}
		whirlProc3 (w, input + offset, left, right, tmp_left, tmp_right, BUFFER_SIZE_SAMPLES);
		for (int i = 0; i < BUFFER_SIZE_SAMPLES; ++i) {
			frame[2 * i] = left[i];
			frame[2 * i + 1] = right[i];
		}
		const long remaining = count - offset;
		fwrite (frame, sizeof (float), 2 * (size_t)(remaining < BUFFER_SIZE_SAMPLES ? remaining : BUFFER_SIZE_SAMPLES), f);
	}
	fclose (f);
	return 0;
}

int main (int argc, char** argv) {
	if (argc >= 3 && !strcmp (argv[1], "wiring"))
		return wiring (argv[2]);
	if (argc >= 4 && !strcmp (argv[1], "tables"))
		return tables (atof (argv[2]), argv[3]);
	if (argc >= 7 && !strcmp (argv[1], "gains"))
		return gains (atof (argv[2]), argv[3], argv[4], argv[5], argv[6]);
	if (argc >= 9 && !strcmp (argv[1], "tonegen"))
		return tonegen (atof (argv[2]), argv[3], argv[4], atoi (argv[5]), argv[6], argv[7], argv[8],
		                argc >= 10 ? argv[9] : NULL);
	if (argc >= 7 && !strcmp (argv[1], "scanner"))
		return scanner (atof (argv[2]), (int)strtol (argv[3], NULL, 0), atof (argv[4]), argv[5], argv[6]);
	if (argc >= 6 && !strcmp (argv[1], "whirl"))
		return whirl (atof (argv[2]), argv[3], argv[4], argv[5]);
	fprintf (stderr, "usage: see the comment at the top of reference.c\n");
	return 2;
}
