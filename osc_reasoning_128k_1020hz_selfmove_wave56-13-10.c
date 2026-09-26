#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OR_CELL_BITS        (128u * 1024u)
#define OR_CELL_BYTES       (OR_CELL_BITS / 8u)     /* 16384 bytes / 16 KiB */
#define OR_NODE_COUNT       32u
#define OR_WAVE_SAMPLES     4096u                   /* 8192 bytes */
#define OR_HISTORY_COUNT    128u
#define OR_PI               3.14159265358979323846f
#define OR_TWO_PI           (2.0f * OR_PI)
#define OR_ENTROPY_DECOMPRESS_MAX_FRAMES 4096u

/* Node status */
enum {
    OR_NODE_DECAYING  = 0,
    OR_NODE_EXCITED   = 1,
    OR_NODE_LOCKED    = 2,
    OR_NODE_CONTESTED = 3
};

/* Cell flags */
enum {
    OR_FLAG_CONVERGED = 1u << 0,
    OR_FLAG_CONTESTED = 1u << 1,
    OR_FLAG_VALID     = 1u << 2,
    OR_FLAG_SATURATED = 1u << 3
};

typedef struct {
    int16_t sample[OR_WAVE_SAMPLES];
} ORWaveRegion; /* 8192 B = 64 Kibit */

typedef struct {
    int16_t coupling_q15[OR_NODE_COUNT][OR_NODE_COUNT]; /* 2048 B */
    int16_t evidence_q15[OR_NODE_COUNT];                /*   64 B */
    int16_t bias_q15[OR_NODE_COUNT];                    /*   64 B */
    int16_t phase_target_q15[OR_NODE_COUNT];            /*   64 B */
    int16_t cadence_q15[OR_NODE_COUNT];                 /*   64 B */
    uint8_t reserved[1792];                             /* 1792 B */
} ORDeltaRegion; /* 4096 B = 32 Kibit */

typedef struct {
    uint32_t tick;
    uint16_t dominant;
    uint16_t flags;
    float coherence;
    float confidence;
} ORSnapshot; /* 16 B */

typedef struct {
    ORSnapshot item[OR_HISTORY_COUNT];
} ORHistoryRegion; /* 2048 B = 16 Kibit */

typedef struct {
    float amplitude;     /* hypothesis support: 0..1 */
    float phase;         /* oscillator phase */
    float omega;         /* base update cadence, rad/s */
    float confidence;    /* evidence-backed confidence: 0..1 */
    float memory;        /* long-term low-pass state */
    float evidence;      /* current normalized evidence: -1..1 */
    float stability;     /* recent state velocity; lower is steadier */
    uint32_t status;
} ORNode; /* 32 B */

typedef struct {
    ORNode node[OR_NODE_COUNT]; /* 1024 B */

    float learning_rate;
    float damping;
    float phase_pull;
    float phase_anchor;
    float evidence_gain;
    float bias_gain;
    float cadence_gain;
    float coherence_threshold;
    float evidence_threshold;
    float stability_threshold;
    float contradiction_threshold;
    float saturation_threshold;

    uint32_t generation;
    uint32_t tick;
    uint32_t history_head;
    uint32_t converged_count;
    uint32_t checksum;
    uint32_t flags;
    uint32_t saturation_count;
    uint32_t reserved_word;

    uint8_t reserved[944];
} ORGuardRegion; /* 2048 B = 16 Kibit */

typedef struct {
    ORWaveRegion wave;       /* 64 Kibit */
    ORDeltaRegion delta;     /* 32 Kibit */
    ORHistoryRegion history; /* 16 Kibit */
    ORGuardRegion guard;     /* 16 Kibit */
} ORCell;                    /* 128 Kibit */

_Static_assert(sizeof(ORWaveRegion) == 8192, "wave region must be 8192 bytes");
_Static_assert(sizeof(ORDeltaRegion) == 4096, "delta region must be 4096 bytes");
_Static_assert(sizeof(ORSnapshot) == 16, "snapshot must be 16 bytes");
_Static_assert(sizeof(ORHistoryRegion) == 2048, "history region must be 2048 bytes");
_Static_assert(sizeof(ORNode) == 32, "node must be 32 bytes");
_Static_assert(sizeof(ORGuardRegion) == 2048, "guard region must be 2048 bytes");
_Static_assert(sizeof(ORCell) == OR_CELL_BYTES, "cell must be exactly 128 Kibit");

static float or_clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static float or_wrap_phase(float p) {
    while (p >= OR_TWO_PI) p -= OR_TWO_PI;
    while (p < 0.0f) p += OR_TWO_PI;
    return p;
}

static float or_shortest_phase_delta(float target, float current) {
    float d = target - current;
    while (d > OR_PI) d -= OR_TWO_PI;
    while (d < -OR_PI) d += OR_TWO_PI;
    return d;
}

static int16_t or_float_to_q15(float x) {
    x = or_clampf(x, -1.0f, 1.0f);
    return (int16_t)lrintf(x * 32767.0f);
}

static float or_q15_to_float(int16_t q) {
    return (float)q / 32767.0f;
}

static uint32_t or_fnv1a32(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Hash the whole cell while treating checksum itself as zero. */
static uint32_t or_compute_checksum(const ORCell *src) {
    ORCell temp;
    memcpy(&temp, src, sizeof(temp));
    temp.guard.checksum = 0;
    return or_fnv1a32(&temp, sizeof(temp));
}

static void or_refresh_checksum(ORCell *c) {
    c->guard.checksum = 0;
    c->guard.checksum = or_compute_checksum(c);
}

static int or_validate(const ORCell *c) {
    return (c->guard.flags & OR_FLAG_VALID) &&
           c->guard.checksum == or_compute_checksum(c);
}

void or_init(ORCell *c) {
    memset(c, 0, sizeof(*c));

    c->guard.learning_rate = 0.10f;
    c->guard.damping = 0.075f;
    c->guard.phase_pull = 0.38f;
    c->guard.phase_anchor = 0.08f;
    c->guard.evidence_gain = 0.82f;
    c->guard.bias_gain = 0.18f;
    c->guard.cadence_gain = 0.50f;
    c->guard.coherence_threshold = 0.80f;
    c->guard.evidence_threshold = 0.55f;
    c->guard.stability_threshold = 0.020f;
    c->guard.contradiction_threshold = 0.28f;
    c->guard.saturation_threshold = 0.95f;
    c->guard.generation = 1;
    c->guard.flags = OR_FLAG_VALID;

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        ORNode *n = &c->guard.node[i];
        n->phase = OR_TWO_PI * (float)i / (float)OR_NODE_COUNT;
        n->omega = 1.0f;
        n->status = OR_NODE_DECAYING;
        c->delta.phase_target_q15[i] = or_float_to_q15(
            2.0f * ((float)i / (float)(OR_NODE_COUNT - 1u)) - 1.0f);
    }

    /* High-resolution default carrier. */
    for (uint32_t i = 0; i < OR_WAVE_SAMPLES; ++i) {
        float p = OR_TWO_PI * (float)i / (float)OR_WAVE_SAMPLES;
        c->wave.sample[i] = (int16_t)lrintf(sinf(p) * 32767.0f);
    }

    or_refresh_checksum(c);
}

void or_set_coupling(ORCell *c, uint32_t from, uint32_t to, float weight) {
    if (from >= OR_NODE_COUNT || to >= OR_NODE_COUNT) return;
    c->delta.coupling_q15[from][to] = or_float_to_q15(weight);
}

void or_set_node(ORCell *c, uint32_t id, float amplitude, float omega) {
    if (id >= OR_NODE_COUNT) return;
    ORNode *n = &c->guard.node[id];
    n->amplitude = or_clampf(amplitude, 0.0f, 1.0f);
    n->omega = omega;
    n->memory = n->amplitude;
}

void or_inject_evidence(ORCell *c, uint32_t id, float evidence) {
    if (id >= OR_NODE_COUNT) return;
    c->delta.evidence_q15[id] = or_float_to_q15(evidence);
}

void or_set_bias(ORCell *c, uint32_t id, float bias) {
    if (id >= OR_NODE_COUNT) return;
    c->delta.bias_q15[id] = or_float_to_q15(bias);
}

void or_set_cadence_bias(ORCell *c, uint32_t id, float cadence) {
    if (id >= OR_NODE_COUNT) return;
    c->delta.cadence_q15[id] = or_float_to_q15(cadence);
}

/* normalized_target_phase: -1..+1 maps to -pi..+pi */
void or_set_phase_target(ORCell *c, uint32_t id, float normalized_target_phase) {
    if (id >= OR_NODE_COUNT) return;
    c->delta.phase_target_q15[id] = or_float_to_q15(normalized_target_phase);
}

static float or_global_coherence(const ORCell *c) {
    float x = 0.0f, y = 0.0f, mass = 0.0f;
    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        const ORNode *n = &c->guard.node[i];
        x += n->amplitude * cosf(n->phase);
        y += n->amplitude * sinf(n->phase);
        mass += n->amplitude;
    }
    if (mass < 1e-6f) return 0.0f;
    return or_clampf(sqrtf(x*x + y*y) / mass, 0.0f, 1.0f);
}

static float or_contradiction(const ORCell *c) {
    float opposed = 0.0f;
    float total = 0.0f;

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        for (uint32_t j = i + 1; j < OR_NODE_COUNT; ++j) {
            float wij = fabsf(or_q15_to_float(c->delta.coupling_q15[i][j]));
            float wji = fabsf(or_q15_to_float(c->delta.coupling_q15[j][i]));
            float w = 0.5f * (wij + wji);
            if (w <= 0.0f) continue;

            float strength = w * c->guard.node[i].amplitude * c->guard.node[j].amplitude;
            float agreement = cosf(c->guard.node[i].phase - c->guard.node[j].phase);
            total += strength;
            if (agreement < 0.0f)
                opposed += strength * (-agreement);
        }
    }

    return total > 1e-6f ? or_clampf(opposed / total, 0.0f, 1.0f) : 0.0f;
}

static uint32_t or_dominant_node(const ORCell *c) {
    uint32_t best = 0;
    float score = -1.0f;
    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        const ORNode *n = &c->guard.node[i];
        float s = n->amplitude * (0.30f + 0.70f * n->confidence);
        if (s > score) {
            score = s;
            best = i;
        }
    }
    return best;
}

static void or_record_history(ORCell *c, float coherence, uint32_t dominant) {
    uint32_t idx = c->guard.history_head % OR_HISTORY_COUNT;
    ORSnapshot *s = &c->history.item[idx];
    s->tick = c->guard.tick;
    s->dominant = (uint16_t)dominant;
    s->flags = (uint16_t)c->guard.flags;
    s->coherence = coherence;
    s->confidence = c->guard.node[dominant].confidence;
    c->guard.history_head = (idx + 1u) % OR_HISTORY_COUNT;
}

void or_step(ORCell *c, float dt) {
    float old_amp[OR_NODE_COUNT];
    float old_phase[OR_NODE_COUNT];
    float next_amp[OR_NODE_COUNT];
    float next_phase[OR_NODE_COUNT];
    float next_conf[OR_NODE_COUNT];

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        old_amp[i] = c->guard.node[i].amplitude;
        old_phase[i] = c->guard.node[i].phase;
        c->guard.node[i].evidence = or_q15_to_float(c->delta.evidence_q15[i]);
    }

    uint32_t saturated_nodes = 0;

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        ORNode *ni = &c->guard.node[i];
        float support = 0.0f;
        float phase_force = 0.0f;
        float norm = 0.0f;

        for (uint32_t j = 0; j < OR_NODE_COUNT; ++j) {
            if (i == j) continue;
            float w = or_q15_to_float(c->delta.coupling_q15[j][i]);
            if (fabsf(w) < 1e-6f) continue;

            float dphi = old_phase[j] - old_phase[i];
            support += w * old_amp[j] * cosf(dphi);
            phase_force += w * old_amp[j] * sinf(dphi);
            norm += fabsf(w);
        }

        if (norm > 1.0f) {
            support /= norm;
            phase_force /= norm;
        }

        float evidence = ni->evidence;
        float bias = or_q15_to_float(c->delta.bias_q15[i]);
        float cadence_bias = or_q15_to_float(c->delta.cadence_q15[i]);
        float target_phase = OR_PI * or_q15_to_float(c->delta.phase_target_q15[i]);
        float anchor_force = or_shortest_phase_delta(target_phase, old_phase[i]);

        float growth = support
                     + c->guard.evidence_gain * evidence
                     + c->guard.bias_gain * bias
                     - c->guard.damping * old_amp[i];

        next_amp[i] = or_clampf(
            old_amp[i] + c->guard.learning_rate * growth,
            0.0f, 1.0f);

        float cadence = ni->omega * (1.0f + c->guard.cadence_gain * cadence_bias);
        if (cadence < 0.0f) cadence = 0.0f;

        next_phase[i] = or_wrap_phase(
            old_phase[i] + dt * (cadence
                               + c->guard.phase_pull * phase_force
                               + c->guard.phase_anchor * anchor_force));

        /* Confidence is evidence-gated: resonance alone cannot manufacture certainty. */
        float evidence_support = fmaxf(0.0f, evidence);
        float evidence_attack  = fmaxf(0.0f, -evidence);
        float conf_delta = 0.085f * evidence_support
                         - 0.125f * evidence_attack
                         - 0.010f * (evidence_support < 0.02f);

        next_conf[i] = or_clampf(ni->confidence + conf_delta, 0.0f, 1.0f);

        if (next_amp[i] >= c->guard.saturation_threshold)
            saturated_nodes++;
    }

    float avg_stability = 0.0f;
    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        ORNode *n = &c->guard.node[i];
        n->stability = fabsf(next_amp[i] - old_amp[i]);
        avg_stability += n->stability;
        n->amplitude = next_amp[i];
        n->phase = next_phase[i];
        n->confidence = next_conf[i];
        n->memory = 0.990f * n->memory + 0.010f * n->amplitude;
    }
    avg_stability /= (float)OR_NODE_COUNT;

    float coherence = or_global_coherence(c);
    float contradiction = or_contradiction(c);
    uint32_t dominant = or_dominant_node(c);
    ORNode *winner = &c->guard.node[dominant];

    int stable = avg_stability <= c->guard.stability_threshold;
    int evidence_ok = winner->evidence >= c->guard.evidence_threshold;
    int coherent = coherence >= c->guard.coherence_threshold;
    int contradiction_ok = contradiction <= c->guard.contradiction_threshold;

    if (stable && evidence_ok && coherent && contradiction_ok)
        c->guard.converged_count++;
    else
        c->guard.converged_count = 0;

    c->guard.flags &= ~(uint32_t)(OR_FLAG_CONVERGED | OR_FLAG_CONTESTED | OR_FLAG_SATURATED);
    if (c->guard.converged_count >= 12u)
        c->guard.flags |= OR_FLAG_CONVERGED;
    if (contradiction > c->guard.contradiction_threshold)
        c->guard.flags |= OR_FLAG_CONTESTED;

    if (saturated_nodes > OR_NODE_COUNT / 2u) {
        c->guard.flags |= OR_FLAG_SATURATED;
        c->guard.saturation_count++;
    } else {
        c->guard.saturation_count = 0;
    }

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        ORNode *n = &c->guard.node[i];
        if ((c->guard.flags & OR_FLAG_CONTESTED) && n->amplitude > 0.35f)
            n->status = OR_NODE_CONTESTED;
        else if ((c->guard.flags & OR_FLAG_CONVERGED) && i == dominant)
            n->status = OR_NODE_LOCKED;
        else if (n->amplitude > n->memory + 0.008f || n->evidence > 0.05f)
            n->status = OR_NODE_EXCITED;
        else
            n->status = OR_NODE_DECAYING;
    }

    c->guard.tick++;
    or_record_history(c, coherence, dominant);
    or_refresh_checksum(c);
}

/* Safe memory-manager update: edit a shadow, validate it, then copy to active. */
void or_shadow_begin(const ORCell *active, ORCell *shadow) {
    memcpy(shadow, active, sizeof(*shadow));
    shadow->guard.generation = active->guard.generation + 1u;
    shadow->guard.flags |= OR_FLAG_VALID;
    or_refresh_checksum(shadow);
}

int or_shadow_commit(ORCell *active, ORCell *shadow) {
    or_refresh_checksum(shadow);
    if (!or_validate(shadow)) return 0;
    memcpy(active, shadow, sizeof(*active));
    return 1;
}

/* Optional waveform output: the reasoning state modulates a stored carrier. */
float or_wave_sample(const ORCell *c, uint32_t phase32) {
    uint64_t scaled = (uint64_t)phase32 * OR_WAVE_SAMPLES;
    uint32_t index = (uint32_t)(scaled >> 32);
    uint32_t frac = (uint32_t)scaled;
    uint32_t next = (index + 1u) % OR_WAVE_SAMPLES;
    float t = (float)((double)frac / 4294967296.0);
    float a = c->wave.sample[index] / 32768.0f;
    float b = c->wave.sample[next] / 32768.0f;
    return a + (b - a) * t;
}


/* -------------------------------------------------------------------------
 * 1020 Hz temporal convolutor
 * -------------------------------------------------------------------------
 * Runs the reasoning cell at exactly 1020 update ticks/second.
 * The FIR is external to ORCell, so ORCell remains exactly 128 Kibit.
 *
 * At 1020 Hz:
 *   tick period = 1 / 1020 s = 980.392156862745 us
 *   Nyquist     = 510 Hz
 *
 * A 33-tap symmetric windowed-sinc low-pass is used by default.  The cutoff
 * is configurable below Nyquist.  Each reasoning node receives its own
 * amplitude and evidence history, so channels remain independent until the
 * normal ORCell coupling stage mixes them.
 */
#define OR_CONV_RATE_HZ       1020.0f
#define OR_CONV_DT            (1.0f / OR_CONV_RATE_HZ)
#define OR_CONV_TAPS          33u
#define OR_CONV_NYQUIST_HZ    (OR_CONV_RATE_HZ * 0.5f)

typedef struct {
    float coeff[OR_CONV_TAPS];
    int16_t amplitude_history[OR_NODE_COUNT][OR_CONV_TAPS];
    int16_t evidence_history[OR_NODE_COUNT][OR_CONV_TAPS];
    float filtered_amplitude[OR_NODE_COUNT];
    float filtered_evidence[OR_NODE_COUNT];
    float cutoff_hz;
    float amplitude_mix;      /* 0=bypass amplitude convolution, 1=full */
    float evidence_mix;       /* 0=bypass evidence convolution, 1=full */
    float amplitude_slew;     /* maximum amplitude correction per 1020-Hz tick */
    float evidence_slew;      /* maximum evidence correction per tick */
    uint32_t head;
    uint32_t tick;
    uint32_t primed;
} ORConvolutor1020;

static float or_conv1020_sinc(float x) {
    if (fabsf(x) < 1e-7f) return 1.0f;
    return sinf(OR_PI * x) / (OR_PI * x);
}

/* Design a normalized Hamming-windowed low-pass FIR. */
int or_conv1020_design_lowpass(ORConvolutor1020 *cv, float cutoff_hz) {
    if (!cv) return 0;
    if (!(cutoff_hz > 0.0f && cutoff_hz < OR_CONV_NYQUIST_HZ)) return 0;

    const float fc = cutoff_hz / OR_CONV_RATE_HZ; /* cycles/sample, 0..0.5 */
    const float mid = ((float)OR_CONV_TAPS - 1.0f) * 0.5f;
    float sum = 0.0f;

    for (uint32_t k = 0; k < OR_CONV_TAPS; ++k) {
        const float m = (float)k - mid;
        const float ideal = 2.0f * fc * or_conv1020_sinc(2.0f * fc * m);
        const float window = 0.54f - 0.46f * cosf(
            OR_TWO_PI * (float)k / ((float)OR_CONV_TAPS - 1.0f));
        cv->coeff[k] = ideal * window;
        sum += cv->coeff[k];
    }

    if (fabsf(sum) < 1e-12f) return 0;
    for (uint32_t k = 0; k < OR_CONV_TAPS; ++k)
        cv->coeff[k] /= sum;

    cv->cutoff_hz = cutoff_hz;
    return 1;
}

void or_conv1020_init(ORConvolutor1020 *cv, float cutoff_hz) {
    if (!cv) return;
    memset(cv, 0, sizeof(*cv));

    cv->amplitude_mix = 0.35f;
    cv->evidence_mix = 1.00f;
    cv->amplitude_slew = 0.020f;
    cv->evidence_slew = 0.080f;

    if (!or_conv1020_design_lowpass(cv, cutoff_hz))
        (void)or_conv1020_design_lowpass(cv, 120.0f);
}

void or_conv1020_set_mix(ORConvolutor1020 *cv,
                         float amplitude_mix,
                         float evidence_mix) {
    if (!cv) return;
    cv->amplitude_mix = or_clampf(amplitude_mix, 0.0f, 1.0f);
    cv->evidence_mix = or_clampf(evidence_mix, 0.0f, 1.0f);
}

void or_conv1020_set_slew(ORConvolutor1020 *cv,
                          float amplitude_per_tick,
                          float evidence_per_tick) {
    if (!cv) return;
    cv->amplitude_slew = fmaxf(0.0f, amplitude_per_tick);
    cv->evidence_slew = fmaxf(0.0f, evidence_per_tick);
}

static float or_conv1020_limit_delta(float current, float target, float max_delta) {
    float d = target - current;
    if (d >  max_delta) d =  max_delta;
    if (d < -max_delta) d = -max_delta;
    return current + d;
}

static float or_conv1020_filter_q15(const int16_t history[OR_CONV_TAPS],
                                    const float coeff[OR_CONV_TAPS],
                                    uint32_t newest) {
    float y = 0.0f;
    uint32_t p = newest;
    for (uint32_t k = 0; k < OR_CONV_TAPS; ++k) {
        y += coeff[k] * or_q15_to_float(history[p]);
        p = (p == 0u) ? (OR_CONV_TAPS - 1u) : (p - 1u);
    }
    return y;
}

/* Seed all FIR delay elements with the current state to avoid startup transients. */
void or_conv1020_prime(ORConvolutor1020 *cv, const ORCell *c) {
    if (!cv || !c) return;

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        int16_t aq = or_float_to_q15(c->guard.node[i].amplitude);
        int16_t eq = c->delta.evidence_q15[i];
        for (uint32_t k = 0; k < OR_CONV_TAPS; ++k) {
            cv->amplitude_history[i][k] = aq;
            cv->evidence_history[i][k] = eq;
        }
        cv->filtered_amplitude[i] = c->guard.node[i].amplitude;
        cv->filtered_evidence[i] = or_q15_to_float(eq);
    }

    cv->head = 0u;
    cv->primed = 1u;
}

/*
 * One complete convolution/reasoning quantum at 1020 Hz.
 *
 * 1. Capture current amplitude + raw evidence into delay lines.
 * 2. FIR-filter each node independently.
 * 3. Apply bounded convolution correction.
 * 4. Run the normal coupled reasoning dynamics at dt = 1/1020.
 */
void or_conv1020_tick(ORConvolutor1020 *cv, ORCell *c) {
    if (!cv || !c) return;
    if (!cv->primed) or_conv1020_prime(cv, c);

    const uint32_t slot = cv->head;

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        cv->amplitude_history[i][slot] =
            or_float_to_q15(c->guard.node[i].amplitude);
        cv->evidence_history[i][slot] = c->delta.evidence_q15[i];
    }

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        const float raw_a = c->guard.node[i].amplitude;
        const float raw_e = or_q15_to_float(c->delta.evidence_q15[i]);

        float fa = or_conv1020_filter_q15(
            cv->amplitude_history[i], cv->coeff, slot);
        float fe = or_conv1020_filter_q15(
            cv->evidence_history[i], cv->coeff, slot);

        fa = or_clampf(fa, 0.0f, 1.0f);
        fe = or_clampf(fe, -1.0f, 1.0f);

        cv->filtered_amplitude[i] = fa;
        cv->filtered_evidence[i] = fe;

        float target_a = raw_a + cv->amplitude_mix * (fa - raw_a);
        float target_e = raw_e + cv->evidence_mix * (fe - raw_e);

        c->guard.node[i].amplitude = or_clampf(
            or_conv1020_limit_delta(raw_a, target_a, cv->amplitude_slew),
            0.0f, 1.0f);

        float bounded_e = or_clampf(
            or_conv1020_limit_delta(raw_e, target_e, cv->evidence_slew),
            -1.0f, 1.0f);
        c->delta.evidence_q15[i] = or_float_to_q15(bounded_e);
    }

    cv->head = (slot + 1u) % OR_CONV_TAPS;
    cv->tick++;

    or_step(c, OR_CONV_DT);
}

/* Exact integer tick accounting for host loops. */
uint64_t or_conv1020_ticks_to_ns(uint64_t ticks) {
    /* Rounded rational conversion: ticks * 1e9 / 1020. */
    return (ticks * 1000000000ull + 510ull) / 1020ull;
}

float or_conv1020_tick_period_us(void) {
    return 1000000.0f / OR_CONV_RATE_HZ;
}


/* -------------------------------------------------------------------------
 * Self-moving oscillatory controller
 * -------------------------------------------------------------------------
 * This layer gives every reasoning node a bounded endogenous motion source.
 * It does NOT create evidence and therefore cannot increase confidence by
 * itself. Instead it moves cadence bias, phase targets, and a small reasoning
 * bias. The 128-Kibit ORCell remains unchanged; self-motion state is external.
 */
#define OR_SELF_MOVE_RATE_HZ OR_CONV_RATE_HZ
#define OR_SELF_MOVE_DT      OR_CONV_DT

typedef struct {
    float drive_phase[OR_NODE_COUNT];
    float velocity[OR_NODE_COUNT];
    float bias_state[OR_NODE_COUNT];
    float cadence_state[OR_NODE_COUNT];
    float phase_state[OR_NODE_COUNT];

    float drive_hz;             /* endogenous movement cycle */
    float drive_spread;         /* per-node detuning */
    float thrust;               /* amplitude of internal motion */
    float damping;              /* velocity damping */
    float memory_pull;          /* return toward remembered amplitude */
    float evidence_steer;       /* evidence may steer, never create confidence */
    float coherence_steer;      /* global coherence affects motion speed */
    float cadence_span;         /* max cadence_q15 magnitude */
    float phase_span;           /* max normalized phase target magnitude */
    float bias_span;            /* max endogenous reasoning bias */
    float max_velocity;
    float max_slew;

    uint32_t tick;
    uint32_t enabled;
} ORSelfMover1020;

void or_self_move_init(ORSelfMover1020 *sm, const ORCell *c) {
    if (!sm) return;
    memset(sm, 0, sizeof(*sm));

    sm->drive_hz = 2.0f;
    sm->drive_spread = 0.28f;
    sm->thrust = 0.85f;
    sm->damping = 3.2f;
    sm->memory_pull = 1.10f;
    sm->evidence_steer = 0.35f;
    sm->coherence_steer = 0.40f;
    sm->cadence_span = 0.32f;
    sm->phase_span = 0.70f;
    sm->bias_span = 0.12f;
    sm->max_velocity = 2.5f;
    sm->max_slew = 0.025f;
    sm->enabled = 1u;

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        sm->drive_phase[i] = OR_TWO_PI * (float)i / (float)OR_NODE_COUNT;
        if (c) {
            sm->bias_state[i] = or_q15_to_float(c->delta.bias_q15[i]);
            sm->cadence_state[i] = or_q15_to_float(c->delta.cadence_q15[i]);
            sm->phase_state[i] = or_q15_to_float(c->delta.phase_target_q15[i]);
        }
    }
}

void or_self_move_enable(ORSelfMover1020 *sm, int enabled) {
    if (!sm) return;
    sm->enabled = enabled ? 1u : 0u;
}

void or_self_move_set_drive(ORSelfMover1020 *sm,
                            float drive_hz,
                            float thrust,
                            float damping) {
    if (!sm) return;
    sm->drive_hz = or_clampf(drive_hz, 0.01f, 100.0f);
    sm->thrust = or_clampf(thrust, 0.0f, 4.0f);
    sm->damping = or_clampf(damping, 0.0f, 40.0f);
}

void or_self_move_set_spans(ORSelfMover1020 *sm,
                            float cadence_span,
                            float phase_span,
                            float bias_span) {
    if (!sm) return;
    sm->cadence_span = or_clampf(cadence_span, 0.0f, 1.0f);
    sm->phase_span = or_clampf(phase_span, 0.0f, 1.0f);
    sm->bias_span = or_clampf(bias_span, 0.0f, 1.0f);
}

static float or_self_move_slew(float current, float target, float max_delta) {
    float d = target - current;
    if (d >  max_delta) d =  max_delta;
    if (d < -max_delta) d = -max_delta;
    return current + d;
}

/*
 * Advance internal motion one 1020-Hz quantum.
 *
 * The mover changes only bias/cadence/phase-target channels. Raw evidence is
 * untouched; this preserves the evidence-gated confidence invariant.
 */
void or_self_move_prepare(ORSelfMover1020 *sm, ORCell *c) {
    if (!sm || !c || !sm->enabled) return;

    const float coherence = or_global_coherence(c);

    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        ORNode *n = &c->guard.node[i];
        const float evidence = or_q15_to_float(c->delta.evidence_q15[i]);
        const float node_detune = ((float)i / (float)(OR_NODE_COUNT - 1u)) - 0.5f;

        /* Internal drive slows slightly when the whole matrix is coherent. */
        float local_hz = sm->drive_hz *
            (1.0f + sm->drive_spread * node_detune) *
            (1.0f - 0.18f * sm->coherence_steer * coherence);
        local_hz = fmaxf(0.01f, local_hz);

        /* A bounded second-order locomotion state. */
        const float memory_error = n->memory - n->amplitude;
        const float wave_drive = sinf(sm->drive_phase[i]);
        const float steering = sm->thrust * wave_drive
                             + sm->memory_pull * memory_error
                             + sm->evidence_steer * evidence;

        sm->velocity[i] += OR_SELF_MOVE_DT *
            (steering - sm->damping * sm->velocity[i]);
        sm->velocity[i] = or_clampf(
            sm->velocity[i], -sm->max_velocity, sm->max_velocity);

        /* Velocity bends the endogenous oscillator rather than teleporting it. */
        const float phase_rate = OR_TWO_PI * local_hz + sm->velocity[i];
        sm->drive_phase[i] = or_wrap_phase(
            sm->drive_phase[i] + OR_SELF_MOVE_DT * phase_rate);

        const float orbit = sinf(sm->drive_phase[i]);
        const float quadrature = cosf(sm->drive_phase[i]);

        float target_phase = sm->phase_span * orbit;
        float target_cadence = sm->cadence_span *
            or_clampf(0.72f * quadrature + 0.28f * sm->velocity[i], -1.0f, 1.0f);

        /* Endogenous bias moves support, but is intentionally small. */
        float target_bias = sm->bias_span *
            or_clampf(0.65f * orbit + 0.35f * memory_error, -1.0f, 1.0f);

        /* When saturated, fold motion back toward neutral. */
        if (c->guard.flags & OR_FLAG_SATURATED) {
            target_cadence *= 0.45f;
            target_bias *= 0.35f;
        }

        sm->phase_state[i] = or_self_move_slew(
            sm->phase_state[i], target_phase, sm->max_slew);
        sm->cadence_state[i] = or_self_move_slew(
            sm->cadence_state[i], target_cadence, sm->max_slew);
        sm->bias_state[i] = or_self_move_slew(
            sm->bias_state[i], target_bias, sm->max_slew);

        c->delta.phase_target_q15[i] = or_float_to_q15(sm->phase_state[i]);
        c->delta.cadence_q15[i] = or_float_to_q15(sm->cadence_state[i]);
        c->delta.bias_q15[i] = or_float_to_q15(sm->bias_state[i]);
    }

    sm->tick++;
}

/* One autonomous movement + convolution + reasoning tick. */
void or_self_move_tick(ORSelfMover1020 *sm,
                       ORConvolutor1020 *cv,
                       ORCell *c) {
    if (!sm || !cv || !c) return;
    or_self_move_prepare(sm, c);
    or_conv1020_tick(cv, c);
}


#ifdef OSC_REASONING_SELF_MOVE_DEMO
int main(void) {
    ORCell cell;
    ORConvolutor1020 conv;
    ORSelfMover1020 mover;

    or_init(&cell);
    or_conv1020_init(&conv, 120.0f);
    or_self_move_init(&mover, &cell);

    /* Six mutually-coupled reasoning oscillators. */
    for (uint32_t i = 0; i < 6u; ++i) {
        or_set_node(&cell, i, 0.22f + 0.012f * (float)i, 7.0f);
        for (uint32_t j = 0; j < 6u; ++j)
            if (i != j) or_set_coupling(&cell, i, j, 0.60f);
    }

    /* Real evidence stays external to self-motion. */
    for (uint32_t i = 0; i < 6u; ++i)
        or_inject_evidence(&cell, i, 0.62f - 0.025f * (float)i);

    or_refresh_checksum(&cell);
    or_conv1020_prime(&conv, &cell);

    /* Two seconds of autonomous locomotion at exactly 1020 Hz. */
    for (uint32_t k = 0; k < 2040u; ++k) {
        if (k == 1020u)
            or_inject_evidence(&cell, 0u, 0.18f);
        or_self_move_tick(&mover, &conv, &cell);
    }

    uint32_t d = or_dominant_node(&cell);
    printf("cell=%zu bytes (%u Kibit)\n",
           sizeof(cell), OR_CELL_BITS / 1024u);
    printf("convolutor=%zu bytes self_mover=%zu bytes rate=%.1f Hz\n",
           sizeof(conv), sizeof(mover), OR_CONV_RATE_HZ);
    printf("ticks=%u mover_ticks=%u time_ns=%llu\n",
           conv.tick, mover.tick,
           (unsigned long long)or_conv1020_ticks_to_ns(conv.tick));
    printf("dominant=%u amp=%.3f conf=%.3f evidence=%.3f phase=%.3f cadence_bias=%.3f flags=0x%X\n",
           d,
           cell.guard.node[d].amplitude,
           cell.guard.node[d].confidence,
           cell.guard.node[d].evidence,
           cell.guard.node[d].phase,
           or_q15_to_float(cell.delta.cadence_q15[d]),
           cell.guard.flags);
    return 0;
}
#endif

/* ------------------------------------------------------------------------- */
/* 56-Kibit transformer storage and control contract                        */
/* ------------------------------------------------------------------------- */

#define OR_WT56_BITS          (56u * 1024u)
#define OR_WT56_BYTES         (OR_WT56_BITS / 8u)
#define OR_WT56_MAP_SAMPLES   2048u
#define OR_WT56_HISTORY       1024u
#define OR_WT56_MOD_SAMPLES    512u
#define OR_WT56_MAX_HZ        (OR_CONV_RATE_HZ * 0.49f)

typedef struct {
    int16_t map_q15[OR_WT56_MAP_SAMPLES];
    int16_t history_q15[OR_WT56_HISTORY];
    int16_t mod_q15[OR_WT56_MOD_SAMPLES];
} ORWaveTransformer56K;

typedef struct {
    uint32_t phase;
    uint32_t phase_step;
    uint32_t sample_rate;
    uint32_t history_head;
    uint32_t generation;
    float drive;
    float morph;
    float feedback;
    float phase_warp;
    float slew;
    float output_gain;
    float previous_output;
    float carrier_hz;
    float energy;
    float energy_limit;
} ORWaveTransformCtl;

_Static_assert(sizeof(ORWaveTransformer56K) == OR_WT56_BYTES,
               "ORWaveTransformer56K must be exactly 56 Kibit");
_Static_assert(sizeof(ORWaveTransformCtl) == 60,
               "wave-transform controller layout changed");

int or_wt56_validate_ctl(const ORWaveTransformCtl *ctl) {
    if (!ctl || ctl->sample_rate == 0u || ctl->sample_rate > 192000u ||
        ctl->history_head >= OR_WT56_HISTORY ||
        !isfinite(ctl->carrier_hz) || ctl->carrier_hz < 0.0f ||
        ctl->carrier_hz > (float)ctl->sample_rate * 0.49f ||
        ctl->phase_step != (uint32_t)((double)ctl->carrier_hz /
                                      (double)ctl->sample_rate * 4294967296.0))
        return 0;
    return isfinite(ctl->drive) && ctl->drive >= 0.0f && ctl->drive <= 1.0f &&
           isfinite(ctl->morph) && ctl->morph >= 0.0f && ctl->morph <= 1.0f &&
           isfinite(ctl->feedback) && ctl->feedback >= 0.0f && ctl->feedback <= 0.5f &&
           isfinite(ctl->phase_warp) && ctl->phase_warp >= 0.0f && ctl->phase_warp <= 1.0f &&
           isfinite(ctl->slew) && ctl->slew >= 0.0f && ctl->slew <= 1.0f &&
           isfinite(ctl->output_gain) && ctl->output_gain >= 0.0f &&
               ctl->output_gain <= 1.0f &&
           isfinite(ctl->previous_output) && ctl->previous_output >= -1.0f &&
               ctl->previous_output <= 1.0f &&
           isfinite(ctl->energy) && ctl->energy >= 0.0f && ctl->energy <= 1.0f &&
           isfinite(ctl->energy_limit) && ctl->energy_limit > 0.0f &&
               ctl->energy_limit <= 1.0f;
}

/* ------------------------------------------------------------------------- */
/* DoubleX reasoning-driven stereo synthesizer                              */
/* ------------------------------------------------------------------------- */

#define OR_DOUBLEX_VOICES              4u
#define OR_DOUBLEX_CHECKPOINT_CREW     4u
#define OR_DOUBLEX_CHECKPOINT_INTERVAL 256u
#define OR_DOUBLE_DIFFUSER_SLOTS       2u
#define OR_DOUBLE_DIFFUSER_CHANNELS    2u
#define OR_DOUBLE_DIFFUSER_RING        64u
#define OR_MULTI_REASONER_SLOTS        2u
#define OR_DOUBLE_SLIT_PATHS           2u
#define OR_DOUBLE_SLIT_CHANNELS        2u
#define OR_DOUBLE_SLIT_RING            64u
#define OR_WAVEGEN_OSCILLATORS         4u
#define OR_WAVEGEN_CHECKPOINTS         4u

enum {
    OR_WAVEGEN_SINE = 0u,
    OR_WAVEGEN_TRIANGLE,
    OR_WAVEGEN_SAW,
    OR_WAVEGEN_PULSE,
    OR_WAVEGEN_WAVEFORM_COUNT
};

typedef struct {
    float hz;
    float phase;
    float gain;
    float pulse_width;
    uint32_t waveform;
    uint32_t generation;
} ORWaveGenOscillator;

typedef struct {
    uint32_t sequence;
    float phase[OR_WAVEGEN_OSCILLATORS];
    uint32_t generation[OR_WAVEGEN_OSCILLATORS];
    uint32_t config_checksum;
    uint32_t checksum;
} ORWaveGenCheckpoint;

typedef struct {
    ORWaveGenOscillator oscillator[OR_WAVEGEN_OSCILLATORS];
    ORWaveGenCheckpoint checkpoint[OR_WAVEGEN_CHECKPOINTS];
    uint32_t sample_rate;
    uint32_t checkpoint_head;
    uint32_t checkpoint_count;
    uint32_t checkpoint_sequence;
    uint32_t frames_since_checkpoint;
    uint32_t frame_generation;
    uint32_t config_checksum;
} ORWaveGeneratorSet;

static uint32_t or_wavegen_config_checksum(const ORWaveGeneratorSet *set) {
    struct {
        float hz[OR_WAVEGEN_OSCILLATORS];
        float gain[OR_WAVEGEN_OSCILLATORS];
        float pulse_width[OR_WAVEGEN_OSCILLATORS];
        uint32_t waveform[OR_WAVEGEN_OSCILLATORS];
        uint32_t sample_rate;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.sample_rate = set->sample_rate;
    for (uint32_t i = 0; i < OR_WAVEGEN_OSCILLATORS; ++i) {
        contract.hz[i] = set->oscillator[i].hz;
        contract.gain[i] = set->oscillator[i].gain;
        contract.pulse_width[i] = set->oscillator[i].pulse_width;
        contract.waveform[i] = set->oscillator[i].waveform;
    }
    return or_fnv1a32(&contract, sizeof(contract));
}

static uint32_t or_wavegen_checkpoint_checksum(const ORWaveGenCheckpoint *cp) {
    ORWaveGenCheckpoint copy = *cp;
    copy.checksum = 0u;
    return or_fnv1a32(&copy, sizeof(copy));
}

static void or_wavegen_init(ORWaveGeneratorSet *set, uint32_t sample_rate) {
    memset(set, 0, sizeof(*set));
    set->sample_rate = sample_rate;
    static const float hz[OR_WAVEGEN_OSCILLATORS] = {110.0f, 220.0f, 330.0f, 440.0f};
    static const float gain[OR_WAVEGEN_OSCILLATORS] = {0.050f, 0.035f, 0.025f, 0.020f};
    for (uint32_t i = 0; i < OR_WAVEGEN_OSCILLATORS; ++i) {
        set->oscillator[i].hz = hz[i];
        set->oscillator[i].gain = gain[i];
        set->oscillator[i].pulse_width = 0.5f;
        set->oscillator[i].waveform = i;
    }
    set->config_checksum = or_wavegen_config_checksum(set);
}

static int or_wavegen_validate(const ORWaveGeneratorSet *set,
                               uint32_t sample_rate) {
    if (!set || set->sample_rate != sample_rate || sample_rate < 8000u ||
        sample_rate > 192000u || set->checkpoint_head >= OR_WAVEGEN_CHECKPOINTS ||
        set->checkpoint_count > OR_WAVEGEN_CHECKPOINTS ||
        set->config_checksum != or_wavegen_config_checksum(set))
        return 0;
    float total_gain = 0.0f;
    for (uint32_t i = 0; i < OR_WAVEGEN_OSCILLATORS; ++i) {
        const ORWaveGenOscillator *osc = &set->oscillator[i];
        if (!isfinite(osc->hz) || osc->hz <= 0.0f ||
            osc->hz >= (float)sample_rate * 0.45f ||
            !isfinite(osc->phase) || osc->phase < 0.0f || osc->phase >= 1.0f ||
            !isfinite(osc->gain) || osc->gain < 0.0f || osc->gain > 0.15f ||
            !isfinite(osc->pulse_width) || osc->pulse_width < 0.05f ||
            osc->pulse_width > 0.95f || osc->waveform >= OR_WAVEGEN_WAVEFORM_COUNT ||
            osc->generation == UINT32_MAX)
            return 0;
        total_gain += osc->gain;
    }
    return total_gain <= 0.30f;
}

int or_wavegen_set_oscillator(ORWaveGeneratorSet *set,
                              uint32_t index,
                              uint32_t waveform,
                              float hz,
                              float gain,
                              float pulse_width) {
    if (!set || index >= OR_WAVEGEN_OSCILLATORS ||
        waveform >= OR_WAVEGEN_WAVEFORM_COUNT || !isfinite(hz) ||
        !isfinite(gain) || !isfinite(pulse_width)) return 0;
    ORWaveGeneratorSet candidate = *set;
    ORWaveGenOscillator *osc = &candidate.oscillator[index];
    osc->waveform = waveform;
    osc->hz = hz;
    osc->gain = gain;
    osc->pulse_width = pulse_width;
    candidate.config_checksum = or_wavegen_config_checksum(&candidate);
    if (!or_wavegen_validate(&candidate, candidate.sample_rate)) return 0;
    *set = candidate;
    return 1;
}

static float or_wavegen_sample(uint32_t waveform, float phase, float pulse_width) {
    switch (waveform) {
        case OR_WAVEGEN_SINE: return sinf(OR_TWO_PI * phase);
        case OR_WAVEGEN_TRIANGLE: return 1.0f - 4.0f * fabsf(phase - 0.5f);
        case OR_WAVEGEN_SAW: return 2.0f * phase - 1.0f;
        case OR_WAVEGEN_PULSE: return phase < pulse_width ? 1.0f : -1.0f;
        default: return 0.0f;
    }
}

static void or_wavegen_checkpoint(ORWaveGeneratorSet *set) {
    ORWaveGenCheckpoint *cp = &set->checkpoint[set->checkpoint_head];
    memset(cp, 0, sizeof(*cp));
    cp->sequence = set->checkpoint_sequence == UINT32_MAX
                 ? UINT32_MAX : set->checkpoint_sequence + 1u;
    if (cp->sequence == 0u) cp->sequence = 1u;
    cp->config_checksum = set->config_checksum;
    for (uint32_t i = 0; i < OR_WAVEGEN_OSCILLATORS; ++i) {
        cp->phase[i] = set->oscillator[i].phase;
        cp->generation[i] = set->oscillator[i].generation;
    }
    cp->checksum = or_wavegen_checkpoint_checksum(cp);
    set->checkpoint_sequence = cp->sequence;
    set->checkpoint_head = (set->checkpoint_head + 1u) % OR_WAVEGEN_CHECKPOINTS;
    if (set->checkpoint_count < OR_WAVEGEN_CHECKPOINTS) set->checkpoint_count++;
    set->frames_since_checkpoint = 0u;
}

int or_wavegen_restore_latest(ORWaveGeneratorSet *set) {
    if (!or_wavegen_validate(set, set ? set->sample_rate : 0u) ||
        set->checkpoint_count == 0u) return 0;
    for (uint32_t offset = 0; offset < OR_WAVEGEN_CHECKPOINTS; ++offset) {
        const uint32_t index = (set->checkpoint_head + OR_WAVEGEN_CHECKPOINTS -
                                1u - offset) % OR_WAVEGEN_CHECKPOINTS;
        const ORWaveGenCheckpoint *cp = &set->checkpoint[index];
        if (!cp->sequence || cp->config_checksum != set->config_checksum ||
            cp->checksum != or_wavegen_checkpoint_checksum(cp)) continue;
        for (uint32_t i = 0; i < OR_WAVEGEN_OSCILLATORS; ++i) {
            set->oscillator[i].phase = cp->phase[i];
            set->oscillator[i].generation = cp->generation[i];
        }
        set->frames_since_checkpoint = 0u;
        set->checkpoint_sequence = cp->sequence;
        return 1;
    }
    return 0;
}

static void or_wavegen_render(ORWaveGeneratorSet *set,
                              float *left,
                              float *right,
                              size_t frame_count) {
    static const float pan[OR_WAVEGEN_OSCILLATORS] = {-0.6f, -0.2f, 0.2f, 0.6f};
    for (size_t frame = 0; frame < frame_count; ++frame) {
        float sum_l = left[frame];
        float sum_r = right[frame];
        for (uint32_t i = 0; i < OR_WAVEGEN_OSCILLATORS; ++i) {
            ORWaveGenOscillator *osc = &set->oscillator[i];
            const float sample = or_wavegen_sample(osc->waveform, osc->phase,
                                                    osc->pulse_width) * osc->gain;
            sum_l += sample * sqrtf(0.5f * (1.0f - pan[i]));
            sum_r += sample * sqrtf(0.5f * (1.0f + pan[i]));
            osc->phase += osc->hz / (float)set->sample_rate;
            osc->phase -= floorf(osc->phase);
            if (osc->generation != UINT32_MAX) osc->generation++;
        }
        left[frame] = or_clampf(sum_l, -1.0f, 1.0f);
        right[frame] = or_clampf(sum_r, -1.0f, 1.0f);
        if (set->frame_generation != UINT32_MAX) set->frame_generation++;
        set->frames_since_checkpoint++;
        if (set->frames_since_checkpoint >= OR_DOUBLEX_CHECKPOINT_INTERVAL)
            or_wavegen_checkpoint(set);
    }
}

/* Independent wave modulation module. Connect it to any stereo renderer. */
#define OR_WAVE_MOD_STAGES 2u
#define OR_WAVE_MOD_CHANNELS 2u
#define OR_WAVE_MOD_RING 128u

enum {
    OR_WAVE_CANCEL_OFF = 0u,
    OR_WAVE_CANCEL_SLOW,
    OR_WAVE_CANCEL_MEDIUM,
    OR_WAVE_CANCEL_FAST,
    OR_WAVE_CANCEL_MENU_COUNT
};

typedef struct {
    float history[OR_WAVE_MOD_CHANNELS][OR_WAVE_MOD_RING];
    uint32_t head;
    uint32_t window_samples;
    float scan_hz;
    float loyalty;           /* 1 preserves input, 0 selects scanned signal. */
    float cancel_depth;
    uint32_t cancel_menu;
    float scan_phase;
    float cancel_phase;
    uint32_t generation;
} ORWaveModulationStage;

typedef struct {
    ORWaveModulationStage stage[OR_WAVE_MOD_STAGES];
    uint32_t sample_rate;
    uint32_t config_checksum;
} ORWaveModulationStack;

float or_wave_mod_cancel_rate_hz(uint32_t menu) {
    static const float hz[OR_WAVE_CANCEL_MENU_COUNT] =
        {0.0f, 0.5f, 2.0f, 8.0f};
    return menu < OR_WAVE_CANCEL_MENU_COUNT ? hz[menu] : -1.0f;
}

static uint32_t or_wave_mod_config_checksum(const ORWaveModulationStack *stack) {
    struct {
        uint32_t sample_rate;
        uint32_t window[OR_WAVE_MOD_STAGES];
        float scan_hz[OR_WAVE_MOD_STAGES];
        float loyalty[OR_WAVE_MOD_STAGES];
        float depth[OR_WAVE_MOD_STAGES];
        uint32_t menu[OR_WAVE_MOD_STAGES];
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.sample_rate = stack->sample_rate;
    for (uint32_t i = 0; i < OR_WAVE_MOD_STAGES; ++i) {
        contract.window[i] = stack->stage[i].window_samples;
        contract.scan_hz[i] = stack->stage[i].scan_hz;
        contract.loyalty[i] = stack->stage[i].loyalty;
        contract.depth[i] = stack->stage[i].cancel_depth;
        contract.menu[i] = stack->stage[i].cancel_menu;
    }
    return or_fnv1a32(&contract, sizeof(contract));
}

int or_wave_mod_stack_init(ORWaveModulationStack *stack, uint32_t sample_rate) {
    if (!stack || sample_rate < 8000u || sample_rate > 192000u) return 0;
    memset(stack, 0, sizeof(*stack));
    stack->sample_rate = sample_rate;
    for (uint32_t i = 0; i < OR_WAVE_MOD_STAGES; ++i) {
        stack->stage[i].window_samples = i ? 48u : 32u;
        stack->stage[i].scan_hz = i ? 0.67f : 0.43f;
        stack->stage[i].loyalty = 0.7f;
        stack->stage[i].cancel_depth = 0.3f;
        stack->stage[i].cancel_menu = OR_WAVE_CANCEL_SLOW;
    }
    stack->config_checksum = or_wave_mod_config_checksum(stack);
    return 1;
}

int or_wave_mod_stack_validate(const ORWaveModulationStack *stack) {
    if (!stack || stack->sample_rate < 8000u ||
        stack->sample_rate > 192000u ||
        stack->config_checksum != or_wave_mod_config_checksum(stack)) return 0;
    for (uint32_t i = 0; i < OR_WAVE_MOD_STAGES; ++i) {
        const ORWaveModulationStage *stage = &stack->stage[i];
        if (stage->head >= OR_WAVE_MOD_RING ||
            stage->window_samples < 8u || stage->window_samples > 96u ||
            !isfinite(stage->scan_hz) || stage->scan_hz < 0.0f ||
            stage->scan_hz > 20.0f ||
            !isfinite(stage->loyalty) || stage->loyalty < 0.0f ||
            stage->loyalty > 1.0f ||
            !isfinite(stage->cancel_depth) || stage->cancel_depth < 0.0f ||
            stage->cancel_depth > 0.8f ||
            stage->cancel_menu >= OR_WAVE_CANCEL_MENU_COUNT ||
            !isfinite(stage->scan_phase) || stage->scan_phase < 0.0f ||
            stage->scan_phase >= 1.0f ||
            !isfinite(stage->cancel_phase) || stage->cancel_phase < 0.0f ||
            stage->cancel_phase >= 1.0f)
            return 0;
        for (uint32_t ch = 0; ch < OR_WAVE_MOD_CHANNELS; ++ch)
            for (uint32_t n = 0; n < OR_WAVE_MOD_RING; ++n)
                if (!isfinite(stage->history[ch][n]) ||
                    fabsf(stage->history[ch][n]) > 1.0f) return 0;
    }
    return 1;
}

int or_wave_mod_stack_set_stage(ORWaveModulationStack *stack,
                                uint32_t stage_index, uint32_t window_samples,
                                float scan_hz, float loyalty,
                                uint32_t cancel_menu, float cancel_depth) {
    if (!or_wave_mod_stack_validate(stack) ||
        stage_index >= OR_WAVE_MOD_STAGES || !isfinite(scan_hz) ||
        !isfinite(loyalty) || !isfinite(cancel_depth)) return 0;
    ORWaveModulationStack candidate = *stack;
    ORWaveModulationStage *stage = &candidate.stage[stage_index];
    stage->window_samples = window_samples;
    stage->scan_hz = scan_hz;
    stage->loyalty = loyalty;
    stage->cancel_menu = cancel_menu;
    stage->cancel_depth = cancel_depth;
    candidate.config_checksum = or_wave_mod_config_checksum(&candidate);
    if (!or_wave_mod_stack_validate(&candidate)) return 0;
    *stack = candidate;
    return 1;
}

static float or_wave_mod_tap(const ORWaveModulationStage *stage,
                             uint32_t ch, float phase) {
    const float delay = 1.0f + phase * (float)(stage->window_samples - 2u);
    const uint32_t offset = (uint32_t)delay;
    const float frac = delay - (float)offset;
    const uint32_t a = (stage->head + OR_WAVE_MOD_RING - offset) % OR_WAVE_MOD_RING;
    const uint32_t b = (a + OR_WAVE_MOD_RING - 1u) % OR_WAVE_MOD_RING;
    return stage->history[ch][a] * (1.0f - frac) +
           stage->history[ch][b] * frac;
}

/* Two Hann windows, half a scan apart, overlap to a constant unit weight. */
int or_wave_mod_stack_process(ORWaveModulationStack *stack,
                              float *left, float *right, size_t frame_count) {
    if (!or_wave_mod_stack_validate(stack) ||
        (frame_count && (!left || !right))) return 0;
    for (size_t frame = 0; frame < frame_count; ++frame)
        if (!isfinite(left[frame]) || fabsf(left[frame]) > 1.0f ||
            !isfinite(right[frame]) || fabsf(right[frame]) > 1.0f) return 0;
    for (size_t frame = 0; frame < frame_count; ++frame) {
        float signal[OR_WAVE_MOD_CHANNELS] = {left[frame], right[frame]};
        for (uint32_t i = 0; i < OR_WAVE_MOD_STAGES; ++i) {
            ORWaveModulationStage *stage = &stack->stage[i];
            const float w = 0.5f - 0.5f * cosf(OR_TWO_PI * stage->scan_phase);
            float opposite = stage->scan_phase + 0.5f;
            if (opposite >= 1.0f) opposite -= 1.0f;
            const float cancellation = stage->cancel_menu == OR_WAVE_CANCEL_OFF
                ? 0.0f : stage->cancel_depth *
                  (0.5f + 0.5f * sinf(OR_TWO_PI * stage->cancel_phase));
            for (uint32_t ch = 0; ch < OR_WAVE_MOD_CHANNELS; ++ch) {
                stage->history[ch][stage->head] = signal[ch];
                const float scanned = w * or_wave_mod_tap(stage, ch, stage->scan_phase) +
                    (1.0f - w) * or_wave_mod_tap(stage, ch, opposite);
                const float modulated = scanned - cancellation * signal[ch];
                signal[ch] = or_clampf(stage->loyalty * signal[ch] +
                    (1.0f - stage->loyalty) * modulated, -1.0f, 1.0f);
            }
            stage->head = (stage->head + 1u) % OR_WAVE_MOD_RING;
            stage->scan_phase += stage->scan_hz / (float)stack->sample_rate;
            if (stage->scan_phase >= 1.0f) stage->scan_phase -= 1.0f;
            stage->cancel_phase += or_wave_mod_cancel_rate_hz(stage->cancel_menu) /
                                   (float)stack->sample_rate;
            if (stage->cancel_phase >= 1.0f) stage->cancel_phase -= 1.0f;
            if (stage->generation != UINT32_MAX) stage->generation++;
        }
        left[frame] = signal[0];
        right[frame] = signal[1];
    }
    return 1;
}

typedef struct {
    float delay[OR_DOUBLE_SLIT_PATHS]
               [OR_DOUBLE_SLIT_CHANNELS][OR_DOUBLE_SLIT_RING];
    uint32_t head[OR_DOUBLE_SLIT_PATHS];
    uint32_t delay_length[OR_DOUBLE_SLIT_PATHS][OR_DOUBLE_SLIT_CHANNELS];
    float aperture[OR_DOUBLE_SLIT_PATHS];
    float coherence;
    float wet;
    float phase;
    float phase_hz;
    uint32_t generation;
} ORDoubleSlitGroup;

static void or_double_slit_init(ORDoubleSlitGroup *slit) {
    memset(slit, 0, sizeof(*slit));
    slit->delay_length[0][0] = 11u;
    slit->delay_length[0][1] = 13u;
    slit->delay_length[1][0] = 23u;
    slit->delay_length[1][1] = 29u;
    slit->aperture[0] = 0.72f;
    slit->aperture[1] = 0.68f;
    slit->coherence = 0.82f;
    slit->wet = 0.62f;
    slit->phase_hz = 0.35f;
}

static int or_double_slit_validate(const ORDoubleSlitGroup *slit,
                                   uint32_t sample_rate) {
    if (!slit || sample_rate < 8000u || sample_rate > 192000u ||
        !isfinite(slit->coherence) || slit->coherence < 0.0f ||
        slit->coherence > 1.0f || !isfinite(slit->wet) || slit->wet < 0.0f ||
        slit->wet > 1.0f || !isfinite(slit->phase) || slit->phase < 0.0f ||
        slit->phase >= OR_TWO_PI || !isfinite(slit->phase_hz) ||
        slit->phase_hz < 0.0f || slit->phase_hz > 20.0f)
        return 0;
    for (uint32_t path = 0; path < OR_DOUBLE_SLIT_PATHS; ++path) {
        if (slit->head[path] >= OR_DOUBLE_SLIT_RING ||
            !isfinite(slit->aperture[path]) || slit->aperture[path] < 0.0f ||
            slit->aperture[path] > 1.0f)
            return 0;
        for (uint32_t channel = 0; channel < OR_DOUBLE_SLIT_CHANNELS; ++channel) {
            if (slit->delay_length[path][channel] == 0u ||
                slit->delay_length[path][channel] >= OR_DOUBLE_SLIT_RING)
                return 0;
            for (uint32_t i = 0; i < OR_DOUBLE_SLIT_RING; ++i)
                if (!isfinite(slit->delay[path][channel][i]) ||
                    slit->delay[path][channel][i] < -1.0f ||
                    slit->delay[path][channel][i] > 1.0f)
                    return 0;
        }
    }
    return 1;
}

typedef struct {
    const ORCell *cell;
    float weight;
    uint32_t expected_checksum;
    uint32_t expected_generation;
} ORMultiReasonerSlot;

typedef struct {
    ORMultiReasonerSlot slot[OR_MULTI_REASONER_SLOTS];
    ORWaveGeneratorSet wavegen;
    ORDoubleSlitGroup slit;
    uint32_t sample_rate;
    uint32_t bound_count;
    uint32_t contract_checksum;
} ORMultiReasonerSet;

int or_multi_reasoner_validate(const ORMultiReasonerSet *set);

static uint32_t or_multi_reasoner_contract(const ORMultiReasonerSet *set) {
    struct {
        float weight[OR_MULTI_REASONER_SLOTS];
        uint32_t checksum[OR_MULTI_REASONER_SLOTS];
        uint32_t generation[OR_MULTI_REASONER_SLOTS];
        uint32_t slit_delay[OR_DOUBLE_SLIT_PATHS][OR_DOUBLE_SLIT_CHANNELS];
        float slit_aperture[OR_DOUBLE_SLIT_PATHS];
        float slit_coherence;
        float slit_wet;
        float slit_phase_hz;
        uint32_t sample_rate;
        uint32_t wavegen_checksum;
        uint32_t bound_count;
    } contract;
    memset(&contract, 0, sizeof(contract));
    for (uint32_t i = 0; i < OR_MULTI_REASONER_SLOTS; ++i) {
        contract.weight[i] = set->slot[i].weight;
        contract.checksum[i] = set->slot[i].expected_checksum;
        contract.generation[i] = set->slot[i].expected_generation;
    }
    memcpy(contract.slit_delay, set->slit.delay_length,
           sizeof(contract.slit_delay));
    memcpy(contract.slit_aperture, set->slit.aperture,
           sizeof(contract.slit_aperture));
    contract.slit_coherence = set->slit.coherence;
    contract.slit_wet = set->slit.wet;
    contract.slit_phase_hz = set->slit.phase_hz;
    contract.sample_rate = set->sample_rate;
    contract.wavegen_checksum = set->wavegen.config_checksum;
    contract.bound_count = set->bound_count;
    return or_fnv1a32(&contract, sizeof(contract));
}

void or_multi_reasoner_init(ORMultiReasonerSet *set) {
    if (!set) return;
    memset(set, 0, sizeof(*set));
    set->sample_rate = 48000u;
    or_double_slit_init(&set->slit);
    or_wavegen_init(&set->wavegen, set->sample_rate);
    set->contract_checksum = or_multi_reasoner_contract(set);
}

int or_multi_reasoner_set_double_slit(ORMultiReasonerSet *set,
                                      uint32_t separation_samples,
                                      float coherence,
                                      float wet,
                                      float phase_hz) {
    if (!set || separation_samples == 0u || separation_samples > 24u ||
        !isfinite(coherence) || !isfinite(wet) || !isfinite(phase_hz))
        return 0;
    ORMultiReasonerSet candidate = *set;
    candidate.slit.delay_length[1][0] =
        candidate.slit.delay_length[0][0] + separation_samples;
    candidate.slit.delay_length[1][1] =
        candidate.slit.delay_length[0][1] + separation_samples;
    candidate.slit.coherence = coherence;
    candidate.slit.wet = wet;
    candidate.slit.phase_hz = phase_hz;
    if (!or_double_slit_validate(&candidate.slit, candidate.sample_rate)) return 0;
    candidate.contract_checksum = or_multi_reasoner_contract(&candidate);
    *set = candidate;
    return 1;
}

int or_multi_reasoner_set_sample_rate(ORMultiReasonerSet *set,
                                      uint32_t sample_rate) {
    if (!set || sample_rate < 8000u || sample_rate > 192000u) return 0;
    ORMultiReasonerSet candidate = *set;
    candidate.sample_rate = sample_rate;
    candidate.wavegen.sample_rate = sample_rate;
    candidate.wavegen.config_checksum =
        or_wavegen_config_checksum(&candidate.wavegen);
    if (!or_double_slit_validate(&candidate.slit, sample_rate)) return 0;
    if (!or_wavegen_validate(&candidate.wavegen, sample_rate)) return 0;
    candidate.contract_checksum = or_multi_reasoner_contract(&candidate);
    *set = candidate;
    return 1;
}

int or_multi_reasoner_set_wavegen(ORMultiReasonerSet *set,
                                  uint32_t oscillator,
                                  uint32_t waveform,
                                  float hz,
                                  float gain,
                                  float pulse_width) {
    if (!set || !or_multi_reasoner_validate(set)) return 0;
    ORMultiReasonerSet candidate = *set;
    if (!or_wavegen_set_oscillator(&candidate.wavegen, oscillator,
                                   waveform, hz, gain, pulse_width)) return 0;
    candidate.contract_checksum = or_multi_reasoner_contract(&candidate);
    *set = candidate;
    return 1;
}

int or_multi_reasoner_restore_wavegen(ORMultiReasonerSet *set) {
    if (!set || !or_multi_reasoner_validate(set) ||
        !or_wavegen_restore_latest(&set->wavegen)) return 0;
    set->contract_checksum = or_multi_reasoner_contract(set);
    return 1;
}

static void or_double_slit_process(ORDoubleSlitGroup *slit,
                                   uint32_t sample_rate,
                                   float *left,
                                   float *right,
                                   size_t frame_count) {
    float *channel[OR_DOUBLE_SLIT_CHANNELS] = {left, right};
    for (size_t frame = 0; frame < frame_count; ++frame) {
        const float phase_weight = slit->coherence * cosf(slit->phase);
        for (uint32_t ch = 0; ch < OR_DOUBLE_SLIT_CHANNELS; ++ch) {
            const float input = channel[ch][frame];
            float path_signal[OR_DOUBLE_SLIT_PATHS];
            for (uint32_t path = 0; path < OR_DOUBLE_SLIT_PATHS; ++path) {
                const uint32_t read =
                    (slit->head[path] + OR_DOUBLE_SLIT_RING -
                     slit->delay_length[path][ch]) % OR_DOUBLE_SLIT_RING;
                path_signal[path] = slit->delay[path][ch][read] *
                                    slit->aperture[path];
                slit->delay[path][ch][slit->head[path]] = input;
            }
            const float interference =
                (path_signal[0] + phase_weight * path_signal[1]) /
                (1.0f + slit->coherence);
            channel[ch][frame] = or_clampf(input * (1.0f - slit->wet) +
                                           interference * slit->wet,
                                           -1.0f, 1.0f);
        }
        for (uint32_t path = 0; path < OR_DOUBLE_SLIT_PATHS; ++path)
            slit->head[path] = (slit->head[path] + 1u) % OR_DOUBLE_SLIT_RING;
        slit->phase = or_wrap_phase(slit->phase +
            OR_TWO_PI * slit->phase_hz / (float)sample_rate);
        if (slit->generation != UINT32_MAX) slit->generation++;
    }
}

int or_multi_reasoner_bind(ORMultiReasonerSet *set,
                           uint32_t slot,
                           const ORCell *cell,
                           float weight) {
    if (!set || slot >= OR_MULTI_REASONER_SLOTS || !cell ||
        !or_validate(cell) || !isfinite(weight) || weight <= 0.0f ||
        weight > 1.0f)
        return 0;
    ORMultiReasonerSet candidate = *set;
    if (!candidate.slot[slot].cell) candidate.bound_count++;
    candidate.slot[slot].cell = cell;
    candidate.slot[slot].weight = weight;
    candidate.slot[slot].expected_checksum = cell->guard.checksum;
    candidate.slot[slot].expected_generation = cell->guard.generation;
    candidate.contract_checksum = or_multi_reasoner_contract(&candidate);
    *set = candidate;
    return 1;
}

int or_multi_reasoner_validate(const ORMultiReasonerSet *set) {
    if (!set || set->bound_count != OR_MULTI_REASONER_SLOTS ||
        !or_double_slit_validate(&set->slit, set->sample_rate) ||
        !or_wavegen_validate(&set->wavegen, set->sample_rate) ||
        set->contract_checksum != or_multi_reasoner_contract(set))
        return 0;
    float total_weight = 0.0f;
    for (uint32_t i = 0; i < OR_MULTI_REASONER_SLOTS; ++i) {
        const ORMultiReasonerSlot *slot = &set->slot[i];
        if (!slot->cell || !isfinite(slot->weight) || slot->weight <= 0.0f ||
            slot->weight > 1.0f || !or_validate(slot->cell) ||
            slot->expected_checksum != slot->cell->guard.checksum ||
            slot->expected_generation != slot->cell->guard.generation)
            return 0;
        total_weight += slot->weight;
    }
    return isfinite(total_weight) && total_weight > 0.0f;
}

/* ------------------------------------------------------------------------- */
/* Modulation + entropy category group                                       */
/* ------------------------------------------------------------------------- */

/*
 * The category group is a connector, not a third reasoner slot.  It reads
 * the verified two-slot contract, measures disagreement as reasoner entropy,
 * and combines that measure with stereo/temporal entropy after modulation.
 *
 * "Derelict" has a precise runtime meaning here: persistent entropy below
 * entropy_floor accumulates derelict_amount.  Recovery is automatic when the
 * signal becomes informative again.  No noise is invented to fake entropy.
 */
enum {
    OR_MOD_ENTROPY_BYPASS = 0u,
    OR_MOD_ENTROPY_MODULATION,
    OR_MOD_ENTROPY_ENTROPY,
    OR_MOD_ENTROPY_CONJUGATION_ENTROPIES,
    OR_MOD_ENTROPY_MODE_COUNT
};

typedef struct {
    uint32_t mode;
    float entropy_floor;
    float entropy_drive;
    float conjugation_depth;
    float response_hz;
    float recovery_hz;

    float signal_entropy;
    float reasoner_entropy;
    float temporal_entropy;
    float combined_entropy;
    float derelict_amount;
    float previous_mid;
    float previous_side;
    uint32_t generation;
} OREntropyConjugationState;

typedef struct {
    ORWaveModulationStack modulation;
    OREntropyConjugationState entropy;
    uint32_t sample_rate;
    uint32_t config_checksum;
} ORModEntropyCategoryGroup;

/* Two independent signed-delta lanes; one int8 code reconstructs one sample. */
typedef struct {
    float predictor[2];
    float step;
    uint32_t generation;
    uint32_t config_checksum;
} ORDoubleEntropyDecompressor;

enum {
    OR_DOUBLE_DECOMPRESS_CATEGORY_REASONER_SLOTS = 1u << 0,
    OR_DOUBLE_DECOMPRESS_CATEGORY_MODULATION     = 1u << 1,
    OR_DOUBLE_DECOMPRESS_CATEGORY_ENTROPY        = 1u << 2,
    OR_DOUBLE_DECOMPRESS_CATEGORY_DOUBLE_DELTA   = 1u << 3,
    OR_DOUBLE_DECOMPRESS_CATEGORY_PIR_VOLITION   = 1u << 4,
    OR_DOUBLE_DECOMPRESS_CATEGORY_ALL =
        OR_DOUBLE_DECOMPRESS_CATEGORY_REASONER_SLOTS |
        OR_DOUBLE_DECOMPRESS_CATEGORY_MODULATION |
        OR_DOUBLE_DECOMPRESS_CATEGORY_ENTROPY |
        OR_DOUBLE_DECOMPRESS_CATEGORY_DOUBLE_DELTA |
        OR_DOUBLE_DECOMPRESS_CATEGORY_PIR_VOLITION
};

typedef struct {
    float kp;
    float ki;
    float kd;
    float target_entropy;
    float integral;
    float integral_limit;
    float previous_error;
    float volition;
    float cadence_hz;
    float delta_depth;
    uint32_t initialized;
    uint32_t generation;
} ORPIRVolitionController;

typedef struct {
    ORModEntropyCategoryGroup categories;
    ORDoubleEntropyDecompressor decompressor;
    ORPIRVolitionController pir;
    uint32_t category_mask;
    uint32_t sample_rate;
    uint32_t config_checksum;
} ORDoubleDecompressorGroup;

enum {
    OR_WAVE_DUPLEXOR_CATEGORY_DOUBLE_DECOMPRESSOR = 1u << 0,
    OR_WAVE_DUPLEXOR_CATEGORY_LANE_EDITOR        = 1u << 1,
    OR_WAVE_DUPLEXOR_CATEGORY_MEET               = 1u << 2,
    OR_WAVE_DUPLEXOR_CATEGORY_SEPARATE           = 1u << 3,
    OR_WAVE_DUPLEXOR_CATEGORY_ALL =
        OR_WAVE_DUPLEXOR_CATEGORY_DOUBLE_DECOMPRESSOR |
        OR_WAVE_DUPLEXOR_CATEGORY_LANE_EDITOR |
        OR_WAVE_DUPLEXOR_CATEGORY_MEET |
        OR_WAVE_DUPLEXOR_CATEGORY_SEPARATE
};

typedef struct {
    float lane_gain[2];
    float lane_bias[2];
    float meet;
    float separate;
    float output_gain;
    uint32_t generation;
    uint32_t config_checksum;
} ORWaveDuplexorEditor;

typedef struct {
    ORDoubleDecompressorGroup decompressor_group;
    ORWaveDuplexorEditor duplexor_editor;
    uint32_t category_mask;
    uint32_t sample_rate;
    uint32_t config_checksum;
} ORDoubleDecompressorWaveDuplexorGroup;

/* Juplex is this project's name for a governed current/prior editor pair. */
enum {
    OR_JUPLEX_CURRENT = 0u,
    OR_JUPLEX_PRIOR,
    OR_JUPLEX_ACCRUED_BLEND,
    OR_JUPLEX_TANGO_MINISTRY,
    OR_JUPLEX_MODE_COUNT
};

enum {
    OR_JUPLEX_GUARANTEE_REASONERS = 1u << 0,
    OR_JUPLEX_GUARANTEE_CONCORD   = 1u << 1,
    OR_JUPLEX_GUARANTEE_ENTROPY   = 1u << 2,
    OR_JUPLEX_GUARANTEE_OUTPUT    = 1u << 3,
    OR_JUPLEX_GUARANTEE_COMMIT    = 1u << 4,
    OR_JUPLEX_GUARANTEE_ALL =
        OR_JUPLEX_GUARANTEE_REASONERS |
        OR_JUPLEX_GUARANTEE_CONCORD |
        OR_JUPLEX_GUARANTEE_ENTROPY |
        OR_JUPLEX_GUARANTEE_OUTPUT |
        OR_JUPLEX_GUARANTEE_COMMIT
};

typedef struct {
    float entropy_response_hz;
    float entropy_meet_depth;
    float entropy_separate_depth;
    float tango_hz;
    float tango_depth;
    float prior_authority;
    float output_ceiling;
} ORJuplexConcord;

typedef struct {
    ORDoubleDecompressorWaveDuplexorGroup current;
    ORWaveDuplexorEditor prior_editor;
    ORJuplexConcord concord;
    uint32_t prior_valid;
    uint32_t mode;
    float target_prior_mix;
    float transition_hz;
    float prior_mix;
    float accrued_energy[2]; /* [0] current, [1] prior; EWMA of edited output. */
    float entropy_modulation;
    float tango_phase;
    float guaranteed_peak;
    uint32_t guarantee_flags;
    uint32_t generation;
    uint32_t config_checksum;
} ORJuplexRegime;

typedef struct {
    uint32_t mode;
    uint32_t prior_valid;
    uint32_t guarantee_flags;
    uint32_t current_editor_generation;
    uint32_t prior_editor_generation;
    float prior_mix;
    float entropy_modulation;
    float tango_phase;
    float current_accrued_energy;
    float prior_accrued_energy;
    float guaranteed_peak;
    float output_ceiling;
} ORJuplexIntegrationReport;

enum {
    OR_TRANSVIXOR_ENVELOPE_EMPTY = 0u,
    OR_TRANSVIXOR_ENVELOPE_DORMANT,
    OR_TRANSVIXOR_ENVELOPE_ENACTED,
    OR_TRANSVIXOR_ENVELOPE_ABORTED
};

enum {
    OR_TRANSVIXOR_MOAT_IDENTITY = 1u << 0,
    OR_TRANSVIXOR_MOAT_FRESHNESS = 1u << 1,
    OR_TRANSVIXOR_MOAT_BOUNDS = 1u << 2,
    OR_TRANSVIXOR_MOAT_INTEGRITY = 1u << 3,
    OR_TRANSVIXOR_MOAT_ATOMIC = 1u << 4,
    OR_TRANSVIXOR_MOAT_ALL =
        OR_TRANSVIXOR_MOAT_IDENTITY |
        OR_TRANSVIXOR_MOAT_FRESHNESS |
        OR_TRANSVIXOR_MOAT_BOUNDS |
        OR_TRANSVIXOR_MOAT_INTEGRITY |
        OR_TRANSVIXOR_MOAT_ATOMIC
};

typedef struct {
    ORWaveDuplexorEditor proposed_editor;
    ORJuplexConcord proposed_concord;
    uint32_t entity_decree;
    uint32_t mode;
    float prior_mix;
    float transition_hz;
    uint32_t base_config_checksum;
    uint32_t base_generation;
    uint32_t sequence;
    uint32_t state;
    uint32_t checksum;
} ORJuplexPriorEnvelope;

typedef struct {
    uint32_t entity_decree;
    float max_gain_delta;
    float max_bias_delta;
    float max_topology_delta;
    float max_output_delta;
    float max_rate_delta;
    uint32_t next_sequence;
    uint32_t enacted_sequence;
    uint32_t aborted_sequence;
    uint32_t moat_flags;
    uint32_t config_checksum;
} ORJuplexTransvixor;

enum {
    OR_REJUKER_GUARANTEE_ENVELOPE = 1u << 0,
    OR_REJUKER_GUARANTEE_PRIOR    = 1u << 1,
    OR_REJUKER_GUARANTEE_COMPASS  = 1u << 2,
    OR_REJUKER_GUARANTEE_OUTPUT   = 1u << 3,
    OR_REJUKER_GUARANTEE_FORWARD  = 1u << 4,
    OR_REJUKER_GUARANTEE_ALL =
        OR_REJUKER_GUARANTEE_ENVELOPE |
        OR_REJUKER_GUARANTEE_PRIOR |
        OR_REJUKER_GUARANTEE_COMPASS |
        OR_REJUKER_GUARANTEE_OUTPUT |
        OR_REJUKER_GUARANTEE_FORWARD
};

typedef struct {
    float compass_turns; /* 0..1, one complete two-lane routing rotation. */
    float juke_depth;
    float forward_gain;
    float slew_limit;
    float output_ceiling;
    float previous_output[2];
    uint32_t generation;
    uint32_t config_checksum;
} ORRejukerKernel;

typedef struct {
    ORRejukerKernel kernel;
    uint32_t entity_decree;
    uint32_t envelope_sequence;
    uint32_t minimum_priority;
    uint32_t guarantee_flags;
    uint64_t forwarded_frames;
    float guaranteed_peak;
    uint32_t config_checksum;
} ORRejukerPriorGuarantee;

enum {
    OR_FORESHADOW_PRIORITY_BACKGROUND = 0u,
    OR_FORESHADOW_PRIORITY_NORMAL,
    OR_FORESHADOW_PRIORITY_FORWARD,
    OR_FORESHADOW_PRIORITY_DECREE,
    OR_FORESHADOW_PRIORITY_COUNT
};

enum {
    OR_FORESHADOW_EMPTY = 0u,
    OR_FORESHADOW_READY,
    OR_FORESHADOW_CONSUMED,
    OR_FORESHADOW_ABORTED
};

enum {
    OR_FORESHADOW_GUARANTEE_IDENTITY = 1u << 0,
    OR_FORESHADOW_GUARANTEE_PRIORITY = 1u << 1,
    OR_FORESHADOW_GUARANTEE_FRESHNESS = 1u << 2,
    OR_FORESHADOW_GUARANTEE_PREDICTION = 1u << 3,
    OR_FORESHADOW_GUARANTEE_NO_MUTATION = 1u << 4,
    OR_FORESHADOW_GUARANTEE_ALL =
        OR_FORESHADOW_GUARANTEE_IDENTITY |
        OR_FORESHADOW_GUARANTEE_PRIORITY |
        OR_FORESHADOW_GUARANTEE_FRESHNESS |
        OR_FORESHADOW_GUARANTEE_PREDICTION |
        OR_FORESHADOW_GUARANTEE_NO_MUTATION
};

typedef struct {
    uint32_t entity_decree;
    uint32_t envelope_sequence;
    uint32_t priority;
    uint32_t state;
    uint32_t frame_count;
    uint32_t base_regime_config_checksum;
    uint32_t base_regime_generation;
    uint32_t base_rejuker_config_checksum;
    uint32_t base_kernel_generation;
    uint64_t base_forwarded_frames;
    uint32_t input_checksum;
    uint32_t predicted_output_checksum;
    float predicted_peak;
    uint32_t guarantee_flags;
    uint32_t checksum;
} ORRejukerForeshadow;

enum {
    OR_REJUKER_MARKOV_CURRENT = 0u,
    OR_REJUKER_MARKOV_PRIOR,
    OR_REJUKER_MARKOV_FORESHADOW,
    OR_REJUKER_MARKOV_FORWARD,
    OR_REJUKER_MARKOV_STATE_COUNT
};

enum {
    OR_REJUKER_PROPERTY_CHECKSUM = 1u << 0,
    OR_REJUKER_PROPERTY_APRIORI  = 1u << 1,
    OR_REJUKER_PROPERTY_MARKOV   = 1u << 2,
    OR_REJUKER_PROPERTY_PRIOR    = 1u << 3,
    OR_REJUKER_PROPERTY_OUTPUT   = 1u << 4,
    OR_REJUKER_PROPERTY_ALL =
        OR_REJUKER_PROPERTY_CHECKSUM |
        OR_REJUKER_PROPERTY_APRIORI |
        OR_REJUKER_PROPERTY_MARKOV |
        OR_REJUKER_PROPERTY_PRIOR |
        OR_REJUKER_PROPERTY_OUTPUT
};

/* The immutable transition/a-priori contract and its per-tick posterior. */
typedef struct {
    float transition[OR_REJUKER_MARKOV_STATE_COUNT]
                    [OR_REJUKER_MARKOV_STATE_COUNT];
    float apriori[OR_REJUKER_MARKOV_STATE_COUNT];
    float current[OR_REJUKER_MARKOV_STATE_COUNT];
    uint64_t tick;
    uint32_t contract_checksum;
    uint32_t state_checksum;
} ORRejukerMarkovChain;

/* Function-regime carcass: identity, Markov state, properties, and checksum. */
typedef struct {
    ORRejukerMarkovChain markov;
    uint32_t entity_decree;
    uint32_t envelope_sequence;
    uint32_t property_flags;
    uint32_t generation;
    uint32_t config_checksum;
    uint32_t checksum;
} ORRejukerFunctionRegimeClass;

typedef struct {
    uint32_t property_flags;
    uint32_t dominant_state;
    uint32_t generation;
    uint64_t markov_tick;
    float apriori[OR_REJUKER_MARKOV_STATE_COUNT];
    float posterior[OR_REJUKER_MARKOV_STATE_COUNT];
    float prior_mix;
    float forward_peak;
    uint64_t forwarded_frames;
    uint32_t minimum_priority;
    uint32_t contract_checksum;
    uint32_t state_checksum;
    int guaranteed;
} ORRejukerPropertyGlass;

const char *or_mod_entropy_mode_name(uint32_t mode) {
    static const char *const name[OR_MOD_ENTROPY_MODE_COUNT] = {
        "Bypass", "Modulation", "Entropy", "Conjugation + Entropies"
    };
    return mode < OR_MOD_ENTROPY_MODE_COUNT ? name[mode] : "Invalid";
}

const char *or_juplex_mode_name(uint32_t mode) {
    static const char *const name[OR_JUPLEX_MODE_COUNT] = {
        "Current", "Prior", "Accrued Blend", "Tango Ministry"
    };
    return mode < OR_JUPLEX_MODE_COUNT ? name[mode] : "Invalid";
}

static float or_binary_entropy(float p) {
    p = or_clampf(p, 0.0f, 1.0f);
    if (p <= 0.000001f || p >= 0.999999f) return 0.0f;
    return -(p * log2f(p) + (1.0f - p) * log2f(1.0f - p));
}

static uint32_t or_mod_entropy_config_checksum(
    const ORModEntropyCategoryGroup *group) {
    struct {
        uint32_t sample_rate;
        uint32_t mode;
        uint32_t modulation_checksum;
        float entropy_floor;
        float entropy_drive;
        float conjugation_depth;
        float response_hz;
        float recovery_hz;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.sample_rate = group->sample_rate;
    contract.mode = group->entropy.mode;
    contract.modulation_checksum = group->modulation.config_checksum;
    contract.entropy_floor = group->entropy.entropy_floor;
    contract.entropy_drive = group->entropy.entropy_drive;
    contract.conjugation_depth = group->entropy.conjugation_depth;
    contract.response_hz = group->entropy.response_hz;
    contract.recovery_hz = group->entropy.recovery_hz;
    return or_fnv1a32(&contract, sizeof(contract));
}

int or_mod_entropy_group_init(ORModEntropyCategoryGroup *group,
                              uint32_t sample_rate) {
    if (!group || sample_rate < 8000u || sample_rate > 192000u) return 0;
    memset(group, 0, sizeof(*group));
    if (!or_wave_mod_stack_init(&group->modulation, sample_rate)) return 0;
    group->sample_rate = sample_rate;
    group->entropy.mode = OR_MOD_ENTROPY_CONJUGATION_ENTROPIES;
    group->entropy.entropy_floor = 0.34f;
    group->entropy.entropy_drive = 0.72f;
    group->entropy.conjugation_depth = 0.58f;
    group->entropy.response_hz = 18.0f;
    group->entropy.recovery_hz = 2.0f;
    group->config_checksum = or_mod_entropy_config_checksum(group);
    return 1;
}

int or_mod_entropy_group_validate(const ORModEntropyCategoryGroup *group) {
    if (!group || group->sample_rate < 8000u ||
        group->sample_rate > 192000u ||
        group->modulation.sample_rate != group->sample_rate ||
        !or_wave_mod_stack_validate(&group->modulation) ||
        group->config_checksum != or_mod_entropy_config_checksum(group))
        return 0;
    const OREntropyConjugationState *e = &group->entropy;
    if (e->mode >= OR_MOD_ENTROPY_MODE_COUNT ||
        !isfinite(e->entropy_floor) || e->entropy_floor < 0.05f ||
        e->entropy_floor > 0.95f || !isfinite(e->entropy_drive) ||
        e->entropy_drive < 0.0f || e->entropy_drive > 1.0f ||
        !isfinite(e->conjugation_depth) || e->conjugation_depth < 0.0f ||
        e->conjugation_depth > 1.0f || !isfinite(e->response_hz) ||
        e->response_hz < 0.1f || e->response_hz > 100.0f ||
        !isfinite(e->recovery_hz) || e->recovery_hz < 0.1f ||
        e->recovery_hz > 20.0f || !isfinite(e->signal_entropy) ||
        e->signal_entropy < 0.0f || e->signal_entropy > 1.0f ||
        !isfinite(e->reasoner_entropy) || e->reasoner_entropy < 0.0f ||
        e->reasoner_entropy > 1.0f || !isfinite(e->temporal_entropy) ||
        e->temporal_entropy < 0.0f || e->temporal_entropy > 1.0f ||
        !isfinite(e->combined_entropy) || e->combined_entropy < 0.0f ||
        e->combined_entropy > 1.0f || !isfinite(e->derelict_amount) ||
        e->derelict_amount < 0.0f || e->derelict_amount > 1.0f ||
        !isfinite(e->previous_mid) || fabsf(e->previous_mid) > 1.0f ||
        !isfinite(e->previous_side) || fabsf(e->previous_side) > 1.0f)
        return 0;
    return 1;
}

int or_mod_entropy_group_set_mode(ORModEntropyCategoryGroup *group,
                                  uint32_t mode,
                                  float entropy_floor,
                                  float entropy_drive,
                                  float conjugation_depth) {
    if (!or_mod_entropy_group_validate(group) ||
        mode >= OR_MOD_ENTROPY_MODE_COUNT || !isfinite(entropy_floor) ||
        !isfinite(entropy_drive) || !isfinite(conjugation_depth)) return 0;
    ORModEntropyCategoryGroup candidate = *group;
    candidate.entropy.mode = mode;
    candidate.entropy.entropy_floor = entropy_floor;
    candidate.entropy.entropy_drive = entropy_drive;
    candidate.entropy.conjugation_depth = conjugation_depth;
    candidate.config_checksum = or_mod_entropy_config_checksum(&candidate);
    if (!or_mod_entropy_group_validate(&candidate)) return 0;
    *group = candidate;
    return 1;
}

int or_mod_entropy_group_set_stage(ORModEntropyCategoryGroup *group,
                                   uint32_t stage_index,
                                   uint32_t window_samples,
                                   float scan_hz,
                                   float loyalty,
                                   uint32_t cancel_menu,
                                   float cancel_depth) {
    if (!or_mod_entropy_group_validate(group)) return 0;
    ORModEntropyCategoryGroup candidate = *group;
    if (!or_wave_mod_stack_set_stage(&candidate.modulation, stage_index,
                                     window_samples, scan_hz, loyalty,
                                     cancel_menu, cancel_depth)) return 0;
    candidate.config_checksum = or_mod_entropy_config_checksum(&candidate);
    if (!or_mod_entropy_group_validate(&candidate)) return 0;
    *group = candidate;
    return 1;
}

/* Entropy of the two verified reasoner slots: ownership plus state divergence. */
float or_multi_reasoner_entropy(const ORMultiReasonerSet *set) {
    if (!or_multi_reasoner_validate(set)) return -1.0f;
    const float total = set->slot[0].weight + set->slot[1].weight;
    const float ownership = or_binary_entropy(set->slot[0].weight / total);
    float divergence = 0.0f;
    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        const ORNode *a = &set->slot[0].cell->guard.node[i];
        const ORNode *b = &set->slot[1].cell->guard.node[i];
        const float amplitude_gap = fabsf(a->amplitude - b->amplitude);
        const float phase_gap = fabsf(or_shortest_phase_delta(a->phase,
                                                               b->phase)) / OR_PI;
        divergence += 0.60f * amplitude_gap + 0.40f * phase_gap;
    }
    divergence /= (float)OR_NODE_COUNT;
    return or_clampf(0.35f * ownership + 0.65f * divergence, 0.0f, 1.0f);
}

int or_mod_entropy_group_process(ORModEntropyCategoryGroup *group,
                                 float reasoner_entropy,
                                 float *left,
                                 float *right,
                                 size_t frame_count) {
    if (!or_mod_entropy_group_validate(group) ||
        !isfinite(reasoner_entropy) || reasoner_entropy < 0.0f ||
        reasoner_entropy > 1.0f || (frame_count && (!left || !right))) return 0;
    for (size_t frame = 0; frame < frame_count; ++frame)
        if (!isfinite(left[frame]) || fabsf(left[frame]) > 1.0f ||
            !isfinite(right[frame]) || fabsf(right[frame]) > 1.0f) return 0;

    const uint32_t mode = group->entropy.mode;
    if ((mode == OR_MOD_ENTROPY_MODULATION ||
         mode == OR_MOD_ENTROPY_CONJUGATION_ENTROPIES) &&
        !or_wave_mod_stack_process(&group->modulation, left, right,
                                   frame_count)) return 0;

    OREntropyConjugationState *e = &group->entropy;
    e->reasoner_entropy = reasoner_entropy;
    const float response = 1.0f - expf(-OR_TWO_PI * e->response_hz /
                                      (float)group->sample_rate);
    const float recovery = 1.0f - expf(-OR_TWO_PI * e->recovery_hz /
                                      (float)group->sample_rate);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        const float input_l = left[frame];
        const float input_r = right[frame];
        const float energy_l = input_l * input_l;
        const float energy_r = input_r * input_r;
        const float energy_sum = energy_l + energy_r;
        const float signal_entropy = energy_sum > 1.0e-12f
            ? or_binary_entropy(energy_l / energy_sum) : 0.0f;
        const float mid = 0.5f * (input_l + input_r);
        const float side = 0.5f * (input_l - input_r);
        const float temporal = or_clampf(
            2.0f * (fabsf(mid - e->previous_mid) +
                    fabsf(side - e->previous_side)), 0.0f, 1.0f);
        const float combined = or_clampf(0.44f * signal_entropy +
            0.34f * temporal + 0.22f * reasoner_entropy, 0.0f, 1.0f);

        e->signal_entropy += response * (signal_entropy - e->signal_entropy);
        e->temporal_entropy += response * (temporal - e->temporal_entropy);
        e->combined_entropy += response * (combined - e->combined_entropy);

        const float deficit = or_clampf(
            (e->entropy_floor - e->combined_entropy) / e->entropy_floor,
            0.0f, 1.0f);
        const float derelict_rate = deficit > e->derelict_amount
            ? response : recovery;
        e->derelict_amount += derelict_rate *
                              (deficit - e->derelict_amount);

        if (mode == OR_MOD_ENTROPY_ENTROPY ||
            mode == OR_MOD_ENTROPY_CONJUGATION_ENTROPIES) {
            const float active_entropy = or_clampf(
                e->entropy_drive * e->combined_entropy +
                (1.0f - e->entropy_drive) * reasoner_entropy, 0.0f, 1.0f);
            const float amount = e->conjugation_depth * active_entropy *
                                 (1.0f - 0.75f * e->derelict_amount);
            /* Stereo side is the imaginary axis; negating it is conjugation. */
            const float conjugate_side = -e->previous_side;
            const float governed_side = side * (1.0f - amount) +
                                        conjugate_side * amount;
            /* The mid axis is conserved, so the operation cannot add DC mass. */
            left[frame] = or_clampf(mid + governed_side, -1.0f, 1.0f);
            right[frame] = or_clampf(mid - governed_side, -1.0f, 1.0f);
        }
        e->previous_mid = mid;
        e->previous_side = side;
        if (e->generation != UINT32_MAX) e->generation++;
    }
    return or_mod_entropy_group_validate(group);
}

static uint32_t or_double_entropy_decompressor_checksum(
    const ORDoubleEntropyDecompressor *decoder) {
    return or_fnv1a32(&decoder->step, sizeof(decoder->step));
}

int or_double_entropy_decompressor_init(ORDoubleEntropyDecompressor *decoder,
                                       float step) {
    if (!decoder || !isfinite(step) || step <= 0.0f || step > 1.0f / 32.0f)
        return 0;
    memset(decoder, 0, sizeof(*decoder));
    decoder->step = step;
    decoder->config_checksum =
        or_double_entropy_decompressor_checksum(decoder);
    return 1;
}

int or_double_entropy_decompressor_validate(
    const ORDoubleEntropyDecompressor *decoder) {
    return decoder && isfinite(decoder->step) && decoder->step > 0.0f &&
           decoder->step <= 1.0f / 32.0f &&
           isfinite(decoder->predictor[0]) &&
           fabsf(decoder->predictor[0]) <= 1.0f &&
           isfinite(decoder->predictor[1]) &&
           fabsf(decoder->predictor[1]) <= 1.0f &&
           decoder->config_checksum ==
               or_double_entropy_decompressor_checksum(decoder);
}

/*
 * Decode and route a stereo block through modulation + entropy.  The two
 * byte streams are independent DPCM lanes.  A whole block is checked and
 * processed on candidate state first; failure leaves decoder, category, and
 * destination buffers unchanged.  frame_count is bounded to cap stack use.
 */
int or_mod_entropy_double_decompress(
    ORModEntropyCategoryGroup *group,
    ORDoubleEntropyDecompressor *decoder,
    float reasoner_entropy,
    const int8_t *left_codes,
    const int8_t *right_codes,
    float *left_output,
    float *right_output,
    size_t frame_count) {
    if (!or_mod_entropy_group_validate(group) ||
        !or_double_entropy_decompressor_validate(decoder) ||
        !isfinite(reasoner_entropy) || reasoner_entropy < 0.0f ||
        reasoner_entropy > 1.0f || frame_count > OR_ENTROPY_DECOMPRESS_MAX_FRAMES ||
        (frame_count && (!left_codes || !right_codes ||
                         !left_output || !right_output))) return 0;

    ORModEntropyCategoryGroup group_candidate = *group;
    ORDoubleEntropyDecompressor decoder_candidate = *decoder;
    float left[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float right[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    for (size_t i = 0; i < frame_count; ++i) {
        decoder_candidate.predictor[0] = or_clampf(
            decoder_candidate.predictor[0] +
                (float)left_codes[i] * decoder_candidate.step,
            -1.0f, 1.0f);
        decoder_candidate.predictor[1] = or_clampf(
            decoder_candidate.predictor[1] +
                (float)right_codes[i] * decoder_candidate.step,
            -1.0f, 1.0f);
        left[i] = decoder_candidate.predictor[0];
        right[i] = decoder_candidate.predictor[1];
        if (decoder_candidate.generation != UINT32_MAX)
            decoder_candidate.generation++;
    }

    if (!or_mod_entropy_group_process(&group_candidate, reasoner_entropy,
                                      left, right, frame_count) ||
        !or_double_entropy_decompressor_validate(&decoder_candidate))
        return 0;

    if (frame_count) {
        memcpy(left_output, left, frame_count * sizeof(float));
        memcpy(right_output, right, frame_count * sizeof(float));
    }
    *group = group_candidate;
    *decoder = decoder_candidate;
    return 1;
}

static uint32_t or_double_decompressor_group_checksum(
    const ORDoubleDecompressorGroup *group) {
    struct {
        uint32_t category_mask;
        uint32_t sample_rate;
        uint32_t categories_checksum;
        uint32_t decoder_checksum;
        float kp, ki, kd, target_entropy, integral_limit;
        float cadence_hz, delta_depth;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.category_mask = group->category_mask;
    contract.sample_rate = group->sample_rate;
    contract.categories_checksum = group->categories.config_checksum;
    contract.decoder_checksum = group->decompressor.config_checksum;
    contract.kp = group->pir.kp;
    contract.ki = group->pir.ki;
    contract.kd = group->pir.kd;
    contract.target_entropy = group->pir.target_entropy;
    contract.integral_limit = group->pir.integral_limit;
    contract.cadence_hz = group->pir.cadence_hz;
    contract.delta_depth = group->pir.delta_depth;
    return or_fnv1a32(&contract, sizeof(contract));
}

int or_double_decompressor_group_init(ORDoubleDecompressorGroup *group,
                                     uint32_t sample_rate) {
    if (!group || sample_rate < 8000u || sample_rate > 192000u) return 0;
    memset(group, 0, sizeof(*group));
    if (!or_mod_entropy_group_init(&group->categories, sample_rate) ||
        !or_double_entropy_decompressor_init(&group->decompressor, 0.0025f))
        return 0;
    group->sample_rate = sample_rate;
    group->category_mask = OR_DOUBLE_DECOMPRESS_CATEGORY_ALL;
    group->pir.kp = 0.82f;
    group->pir.ki = 0.18f;
    group->pir.kd = 0.025f;
    group->pir.target_entropy = 0.46f;
    group->pir.integral_limit = 2.0f;
    group->pir.cadence_hz = 6.0f;
    group->pir.delta_depth = 0.45f;
    group->config_checksum = or_double_decompressor_group_checksum(group);
    return 1;
}

int or_double_decompressor_group_validate(
    const ORDoubleDecompressorGroup *group) {
    if (!group || group->sample_rate < 8000u ||
        group->sample_rate > 192000u ||
        group->categories.sample_rate != group->sample_rate ||
        group->category_mask != OR_DOUBLE_DECOMPRESS_CATEGORY_ALL ||
        !or_mod_entropy_group_validate(&group->categories) ||
        !or_double_entropy_decompressor_validate(&group->decompressor) ||
        group->config_checksum != or_double_decompressor_group_checksum(group))
        return 0;
    const ORPIRVolitionController *p = &group->pir;
    return isfinite(p->kp) && p->kp >= 0.0f && p->kp <= 4.0f &&
           isfinite(p->ki) && p->ki >= 0.0f && p->ki <= 2.0f &&
           isfinite(p->kd) && p->kd >= 0.0f && p->kd <= 0.5f &&
           isfinite(p->target_entropy) && p->target_entropy >= 0.0f &&
           p->target_entropy <= 1.0f && isfinite(p->integral) &&
           fabsf(p->integral) <= p->integral_limit &&
           isfinite(p->integral_limit) && p->integral_limit > 0.0f &&
           p->integral_limit <= 8.0f && isfinite(p->previous_error) &&
           fabsf(p->previous_error) <= 1.0f && isfinite(p->volition) &&
           p->volition >= -1.0f && p->volition <= 1.0f &&
           isfinite(p->cadence_hz) && p->cadence_hz >= 0.1f &&
           p->cadence_hz <= 20.0f && isfinite(p->delta_depth) &&
           p->delta_depth >= 0.0f && p->delta_depth <= 0.75f &&
           p->initialized <= 1u;
}

int or_double_decompressor_group_set_pir(
    ORDoubleDecompressorGroup *group, float kp, float ki, float kd,
    float target_entropy, float cadence_hz, float delta_depth) {
    if (!or_double_decompressor_group_validate(group) || !isfinite(kp) ||
        !isfinite(ki) || !isfinite(kd) || !isfinite(target_entropy) ||
        !isfinite(cadence_hz) || !isfinite(delta_depth)) return 0;
    ORDoubleDecompressorGroup candidate = *group;
    candidate.pir.kp = kp;
    candidate.pir.ki = ki;
    candidate.pir.kd = kd;
    candidate.pir.target_entropy = target_entropy;
    candidate.pir.cadence_hz = cadence_hz;
    candidate.pir.delta_depth = delta_depth;
    candidate.config_checksum = or_double_decompressor_group_checksum(&candidate);
    if (!or_double_decompressor_group_validate(&candidate)) return 0;
    *group = candidate;
    return 1;
}

/*
 * Compiled category regime: reasoner slots -> modulation/entropy -> two
 * DPCM lanes -> P/I/D volition.  Current volition scales this block's delta
 * step; the newly measured P/I/D response governs the next block.
 */
int or_double_decompressor_group_process(
    ORDoubleDecompressorGroup *group,
    float reasoner_entropy,
    const int8_t *left_codes,
    const int8_t *right_codes,
    float *left_output,
    float *right_output,
    size_t frame_count) {
    if (!or_double_decompressor_group_validate(group) ||
        !isfinite(reasoner_entropy) || reasoner_entropy < 0.0f ||
        reasoner_entropy > 1.0f ||
        frame_count > OR_ENTROPY_DECOMPRESS_MAX_FRAMES ||
        (frame_count && (!left_codes || !right_codes ||
                         !left_output || !right_output))) return 0;

    ORDoubleDecompressorGroup candidate = *group;
    const float scale = or_clampf(
        1.0f + candidate.pir.delta_depth * candidate.pir.volition,
        0.25f, 1.75f);
    candidate.decompressor.step = or_clampf(
        candidate.decompressor.step * scale, 1.0e-6f, 1.0f / 32.0f);
    candidate.decompressor.config_checksum =
        or_double_entropy_decompressor_checksum(&candidate.decompressor);

    float left[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float right[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    if (!or_mod_entropy_double_decompress(
            &candidate.categories, &candidate.decompressor, reasoner_entropy,
            left_codes, right_codes, left, right, frame_count)) return 0;

    /* Restore the nominal code step while retaining both updated predictors. */
    candidate.decompressor.step = group->decompressor.step;
    candidate.decompressor.config_checksum =
        or_double_entropy_decompressor_checksum(&candidate.decompressor);

    if (frame_count) {
        ORPIRVolitionController *p = &candidate.pir;
        const float dt = (float)frame_count / (float)candidate.sample_rate;
        const float error = p->target_entropy -
                            candidate.categories.entropy.combined_entropy;
        const float derivative = p->initialized && dt > 0.0f
            ? (error - p->previous_error) / dt : 0.0f;
        p->integral = or_clampf(p->integral + error * dt,
                                -p->integral_limit, p->integral_limit);
        const float demand = or_clampf(
            p->kp * error + p->ki * p->integral + p->kd * derivative,
            -1.0f, 1.0f);
        const float response = 1.0f - expf(-OR_TWO_PI * p->cadence_hz * dt);
        p->volition = or_clampf(p->volition +
                                response * (demand - p->volition),
                                -1.0f, 1.0f);
        p->previous_error = error;
        p->initialized = 1u;
        if (p->generation != UINT32_MAX) p->generation++;
    }

    if (!or_double_decompressor_group_validate(&candidate)) return 0;
    if (frame_count) {
        memcpy(left_output, left, frame_count * sizeof(float));
        memcpy(right_output, right, frame_count * sizeof(float));
    }
    *group = candidate;
    return 1;
}

/* End-to-end connector: obtain entropy only from the verified two-slot set. */
int or_multi_reasoner_double_decompress(
    const ORMultiReasonerSet *reasoners,
    ORDoubleDecompressorGroup *group,
    const int8_t *left_codes,
    const int8_t *right_codes,
    float *left_output,
    float *right_output,
    size_t frame_count) {
    if (!reasoners || !or_multi_reasoner_validate(reasoners) ||
        !group || group->sample_rate != reasoners->sample_rate) return 0;
    const float entropy = or_multi_reasoner_entropy(reasoners);
    if (!isfinite(entropy) || entropy < 0.0f) return 0;
    return or_double_decompressor_group_process(
        group, entropy, left_codes, right_codes,
        left_output, right_output, frame_count);
}

static uint32_t or_wave_duplexor_editor_checksum(
    const ORWaveDuplexorEditor *editor) {
    struct {
        float lane_gain[2];
        float lane_bias[2];
        float meet;
        float separate;
        float output_gain;
    } contract;
    memset(&contract, 0, sizeof(contract));
    memcpy(contract.lane_gain, editor->lane_gain, sizeof(contract.lane_gain));
    memcpy(contract.lane_bias, editor->lane_bias, sizeof(contract.lane_bias));
    contract.meet = editor->meet;
    contract.separate = editor->separate;
    contract.output_gain = editor->output_gain;
    return or_fnv1a32(&contract, sizeof(contract));
}

static uint32_t or_wave_duplexor_group_checksum(
    const ORDoubleDecompressorWaveDuplexorGroup *group) {
    struct {
        uint32_t category_mask;
        uint32_t sample_rate;
        uint32_t decompressor_checksum;
        uint32_t editor_checksum;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.category_mask = group->category_mask;
    contract.sample_rate = group->sample_rate;
    contract.decompressor_checksum =
        group->decompressor_group.config_checksum;
    contract.editor_checksum = group->duplexor_editor.config_checksum;
    return or_fnv1a32(&contract, sizeof(contract));
}

int or_wave_duplexor_editor_init(ORWaveDuplexorEditor *editor) {
    if (!editor) return 0;
    memset(editor, 0, sizeof(*editor));
    editor->lane_gain[0] = editor->lane_gain[1] = 1.0f;
    editor->meet = 0.20f;
    editor->separate = 0.25f;
    editor->output_gain = 1.0f;
    editor->config_checksum = or_wave_duplexor_editor_checksum(editor);
    return 1;
}

int or_wave_duplexor_editor_validate(const ORWaveDuplexorEditor *editor) {
    if (!editor || editor->generation == UINT32_MAX ||
        editor->config_checksum != or_wave_duplexor_editor_checksum(editor) ||
        !isfinite(editor->meet) || editor->meet < 0.0f || editor->meet > 1.0f ||
        !isfinite(editor->separate) || editor->separate < 0.0f ||
        editor->separate > 1.0f || !isfinite(editor->output_gain) ||
        editor->output_gain < 0.0f || editor->output_gain > 1.5f) return 0;
    for (uint32_t lane = 0; lane < 2u; ++lane)
        if (!isfinite(editor->lane_gain[lane]) || editor->lane_gain[lane] < 0.0f ||
            editor->lane_gain[lane] > 2.0f ||
            !isfinite(editor->lane_bias[lane]) ||
            editor->lane_bias[lane] < -0.25f || editor->lane_bias[lane] > 0.25f)
            return 0;
    return 1;
}

int or_wave_duplexor_editor_set(ORWaveDuplexorEditor *editor,
                                float left_gain, float right_gain,
                                float left_bias, float right_bias,
                                float meet, float separate,
                                float output_gain) {
    if (!or_wave_duplexor_editor_validate(editor) || !isfinite(left_gain) ||
        !isfinite(right_gain) || !isfinite(left_bias) ||
        !isfinite(right_bias) || !isfinite(meet) || !isfinite(separate) ||
        !isfinite(output_gain)) return 0;
    ORWaveDuplexorEditor candidate = *editor;
    candidate.lane_gain[0] = left_gain;
    candidate.lane_gain[1] = right_gain;
    candidate.lane_bias[0] = left_bias;
    candidate.lane_bias[1] = right_bias;
    candidate.meet = meet;
    candidate.separate = separate;
    candidate.output_gain = output_gain;
    candidate.config_checksum = or_wave_duplexor_editor_checksum(&candidate);
    if (!or_wave_duplexor_editor_validate(&candidate)) return 0;
    *editor = candidate;
    return 1;
}

int or_wave_duplexor_editor_process(ORWaveDuplexorEditor *editor,
                                   float *left, float *right,
                                   size_t frame_count) {
    if (!or_wave_duplexor_editor_validate(editor) ||
        frame_count > OR_ENTROPY_DECOMPRESS_MAX_FRAMES ||
        (frame_count && (!left || !right))) return 0;
    float edited_left[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float edited_right[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    for (size_t i = 0; i < frame_count; ++i) {
        if (!isfinite(left[i]) || fabsf(left[i]) > 1.0f ||
            !isfinite(right[i]) || fabsf(right[i]) > 1.0f) return 0;
        const float lane_l = or_clampf(
            left[i] * editor->lane_gain[0] + editor->lane_bias[0], -1.0f, 1.0f);
        const float lane_r = or_clampf(
            right[i] * editor->lane_gain[1] + editor->lane_bias[1], -1.0f, 1.0f);
        const float mid = 0.5f * (lane_l + lane_r);
        const float side = 0.5f * (lane_l - lane_r);
        const float duplex_side = side * (1.0f - editor->meet) *
                                  (1.0f + 2.0f * editor->separate);
        edited_left[i] = or_clampf((mid + duplex_side) * editor->output_gain,
                                   -1.0f, 1.0f);
        edited_right[i] = or_clampf((mid - duplex_side) * editor->output_gain,
                                    -1.0f, 1.0f);
    }
    if (frame_count) {
        memcpy(left, edited_left, frame_count * sizeof(float));
        memcpy(right, edited_right, frame_count * sizeof(float));
    }
    if (editor->generation != UINT32_MAX) editor->generation++;
    return 1;
}

int or_double_decompressor_wave_duplexor_group_init(
    ORDoubleDecompressorWaveDuplexorGroup *group, uint32_t sample_rate) {
    if (!group || !or_double_decompressor_group_init(
                      &group->decompressor_group, sample_rate) ||
        !or_wave_duplexor_editor_init(&group->duplexor_editor)) return 0;
    group->category_mask = OR_WAVE_DUPLEXOR_CATEGORY_ALL;
    group->sample_rate = sample_rate;
    group->config_checksum = or_wave_duplexor_group_checksum(group);
    return 1;
}

int or_double_decompressor_wave_duplexor_group_validate(
    const ORDoubleDecompressorWaveDuplexorGroup *group) {
    return group && group->sample_rate >= 8000u &&
           group->sample_rate <= 192000u &&
           group->category_mask == OR_WAVE_DUPLEXOR_CATEGORY_ALL &&
           group->decompressor_group.sample_rate == group->sample_rate &&
           or_double_decompressor_group_validate(&group->decompressor_group) &&
           or_wave_duplexor_editor_validate(&group->duplexor_editor) &&
           group->config_checksum == or_wave_duplexor_group_checksum(group);
}

int or_double_decompressor_wave_duplexor_group_set_editor(
    ORDoubleDecompressorWaveDuplexorGroup *group,
    float left_gain, float right_gain, float left_bias, float right_bias,
    float meet, float separate, float output_gain) {
    if (!or_double_decompressor_wave_duplexor_group_validate(group)) return 0;
    ORDoubleDecompressorWaveDuplexorGroup candidate = *group;
    if (!or_wave_duplexor_editor_set(&candidate.duplexor_editor,
                                     left_gain, right_gain, left_bias, right_bias,
                                     meet, separate, output_gain)) return 0;
    candidate.config_checksum = or_wave_duplexor_group_checksum(&candidate);
    if (!or_double_decompressor_wave_duplexor_group_validate(&candidate)) return 0;
    *group = candidate;
    return 1;
}

/* Two delta lanes stay independently editable until this governed meet/split. */
int or_multi_reasoner_wave_duplexor_edit(
    const ORMultiReasonerSet *reasoners,
    ORDoubleDecompressorWaveDuplexorGroup *group,
    const int8_t *left_codes,
    const int8_t *right_codes,
    float *left_output,
    float *right_output,
    size_t frame_count) {
    if (!reasoners || !or_multi_reasoner_validate(reasoners) ||
        !or_double_decompressor_wave_duplexor_group_validate(group) ||
        reasoners->sample_rate != group->sample_rate ||
        frame_count > OR_ENTROPY_DECOMPRESS_MAX_FRAMES ||
        (frame_count && (!left_codes || !right_codes ||
                         !left_output || !right_output))) return 0;
    ORDoubleDecompressorWaveDuplexorGroup candidate = *group;
    float left[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float right[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    if (!or_multi_reasoner_double_decompress(
            reasoners, &candidate.decompressor_group,
            left_codes, right_codes, left, right, frame_count) ||
        !or_wave_duplexor_editor_process(&candidate.duplexor_editor,
                                         left, right, frame_count)) return 0;
    if (!or_double_decompressor_wave_duplexor_group_validate(&candidate)) return 0;
    if (frame_count) {
        memcpy(left_output, left, frame_count * sizeof(float));
        memcpy(right_output, right, frame_count * sizeof(float));
    }
    *group = candidate;
    return 1;
}

static uint32_t or_juplex_config_checksum(const ORJuplexRegime *regime) {
    struct {
        uint32_t current_checksum;
        uint32_t prior_checksum;
        uint32_t prior_valid;
        uint32_t mode;
        float target_prior_mix;
        float transition_hz;
        ORJuplexConcord concord;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.current_checksum = regime->current.config_checksum;
    contract.prior_checksum = regime->prior_valid
        ? regime->prior_editor.config_checksum : 0u;
    contract.prior_valid = regime->prior_valid;
    contract.mode = regime->mode;
    contract.target_prior_mix = regime->target_prior_mix;
    contract.transition_hz = regime->transition_hz;
    contract.concord = regime->concord;
    return or_fnv1a32(&contract, sizeof(contract));
}

int or_juplex_init(ORJuplexRegime *regime, uint32_t sample_rate) {
    if (!regime || sample_rate < 8000u || sample_rate > 192000u) return 0;
    memset(regime, 0, sizeof(*regime));
    if (!or_double_decompressor_wave_duplexor_group_init(
            &regime->current, sample_rate)) return 0;
    regime->mode = OR_JUPLEX_CURRENT;
    regime->transition_hz = 3.0f;
    regime->concord.entropy_response_hz = 8.0f;
    regime->concord.entropy_meet_depth = 0.24f;
    regime->concord.entropy_separate_depth = 0.28f;
    regime->concord.tango_hz = 0.75f;
    regime->concord.tango_depth = 0.35f;
    regime->concord.prior_authority = 1.0f;
    regime->concord.output_ceiling = 0.95f;
    regime->config_checksum = or_juplex_config_checksum(regime);
    return 1;
}

int or_juplex_validate(const ORJuplexRegime *regime) {
    if (!regime ||
        !or_double_decompressor_wave_duplexor_group_validate(&regime->current) ||
        regime->prior_valid > 1u || regime->mode >= OR_JUPLEX_MODE_COUNT ||
        (regime->mode != OR_JUPLEX_CURRENT && !regime->prior_valid) ||
        (regime->prior_valid &&
         !or_wave_duplexor_editor_validate(&regime->prior_editor)) ||
        !isfinite(regime->target_prior_mix) ||
        regime->target_prior_mix < 0.0f || regime->target_prior_mix > 1.0f ||
        !isfinite(regime->transition_hz) || regime->transition_hz < 0.1f ||
        regime->transition_hz > 50.0f || !isfinite(regime->prior_mix) ||
        regime->prior_mix < 0.0f || regime->prior_mix > 1.0f ||
        !isfinite(regime->concord.entropy_response_hz) ||
        regime->concord.entropy_response_hz < 0.1f ||
        regime->concord.entropy_response_hz > 50.0f ||
        !isfinite(regime->concord.entropy_meet_depth) ||
        regime->concord.entropy_meet_depth < 0.0f ||
        regime->concord.entropy_meet_depth > 1.0f ||
        !isfinite(regime->concord.entropy_separate_depth) ||
        regime->concord.entropy_separate_depth < 0.0f ||
        regime->concord.entropy_separate_depth > 1.0f ||
        !isfinite(regime->concord.tango_hz) ||
        regime->concord.tango_hz < 0.0f || regime->concord.tango_hz > 20.0f ||
        !isfinite(regime->concord.tango_depth) ||
        regime->concord.tango_depth < 0.0f ||
        regime->concord.tango_depth > 1.0f ||
        !isfinite(regime->concord.prior_authority) ||
        regime->concord.prior_authority < 0.0f ||
        regime->concord.prior_authority > 1.0f ||
        regime->prior_mix > regime->concord.prior_authority ||
        !isfinite(regime->concord.output_ceiling) ||
        regime->concord.output_ceiling < 0.1f ||
        regime->concord.output_ceiling > 1.0f ||
        !isfinite(regime->entropy_modulation) ||
        regime->entropy_modulation < 0.0f || regime->entropy_modulation > 1.0f ||
        !isfinite(regime->tango_phase) || regime->tango_phase < 0.0f ||
        regime->tango_phase >= 1.0f || !isfinite(regime->guaranteed_peak) ||
        regime->guaranteed_peak < 0.0f ||
        regime->guaranteed_peak > regime->concord.output_ceiling ||
        (regime->guarantee_flags & ~OR_JUPLEX_GUARANTEE_ALL) != 0u ||
        regime->config_checksum != or_juplex_config_checksum(regime)) return 0;
    for (uint32_t i = 0; i < 2u; ++i)
        if (!isfinite(regime->accrued_energy[i]) ||
            regime->accrued_energy[i] < 0.0f ||
            regime->accrued_energy[i] > 1.0f) return 0;
    return 1;
}

int or_juplex_set_concord(ORJuplexRegime *regime,
                          float entropy_response_hz,
                          float entropy_meet_depth,
                          float entropy_separate_depth,
                          float tango_hz,
                          float tango_depth,
                          float prior_authority,
                          float output_ceiling) {
    if (!or_juplex_validate(regime) || !isfinite(entropy_response_hz) ||
        !isfinite(entropy_meet_depth) ||
        !isfinite(entropy_separate_depth) || !isfinite(tango_hz) ||
        !isfinite(tango_depth) || !isfinite(prior_authority) ||
        !isfinite(output_ceiling)) return 0;
    ORJuplexRegime candidate = *regime;
    candidate.concord.entropy_response_hz = entropy_response_hz;
    candidate.concord.entropy_meet_depth = entropy_meet_depth;
    candidate.concord.entropy_separate_depth = entropy_separate_depth;
    candidate.concord.tango_hz = tango_hz;
    candidate.concord.tango_depth = tango_depth;
    candidate.concord.prior_authority = prior_authority;
    candidate.concord.output_ceiling = output_ceiling;
    candidate.prior_mix = fminf(candidate.prior_mix, prior_authority);
    candidate.guaranteed_peak = fminf(candidate.guaranteed_peak, output_ceiling);
    candidate.guarantee_flags = 0u;
    candidate.config_checksum = or_juplex_config_checksum(&candidate);
    if (!or_juplex_validate(&candidate)) return 0;
    *regime = candidate;
    return 1;
}

int or_juplex_guaranteed(const ORJuplexRegime *regime) {
    return or_juplex_validate(regime) && regime->generation > 0u &&
           regime->guarantee_flags == OR_JUPLEX_GUARANTEE_ALL &&
           regime->guaranteed_peak <= regime->concord.output_ceiling;
}

int or_juplex_review_integration(const ORJuplexRegime *regime,
                                  ORJuplexIntegrationReport *report) {
    if (!or_juplex_validate(regime) || !report) return 0;
    memset(report, 0, sizeof(*report));
    report->mode = regime->mode;
    report->prior_valid = regime->prior_valid;
    report->guarantee_flags = regime->guarantee_flags;
    report->current_editor_generation =
        regime->current.duplexor_editor.generation;
    report->prior_editor_generation = regime->prior_valid
        ? regime->prior_editor.generation : 0u;
    report->prior_mix = regime->prior_mix;
    report->entropy_modulation = regime->entropy_modulation;
    report->tango_phase = regime->tango_phase;
    report->current_accrued_energy = regime->accrued_energy[0];
    report->prior_accrued_energy = regime->accrued_energy[1];
    report->guaranteed_peak = regime->guaranteed_peak;
    report->output_ceiling = regime->concord.output_ceiling;
    return 1;
}

/* An editor change first keeps the old current notion as the prior notion. */
int or_juplex_edit_current(ORJuplexRegime *regime,
                           float left_gain, float right_gain,
                           float left_bias, float right_bias,
                           float meet, float separate, float output_gain) {
    if (!or_juplex_validate(regime)) return 0;
    ORJuplexRegime candidate = *regime;
    const ORWaveDuplexorEditor previous = candidate.current.duplexor_editor;
    if (!or_double_decompressor_wave_duplexor_group_set_editor(
            &candidate.current, left_gain, right_gain,
            left_bias, right_bias, meet, separate, output_gain)) return 0;
    candidate.prior_editor = previous;
    candidate.prior_valid = 1u;
    candidate.accrued_energy[1] = candidate.accrued_energy[0];
    candidate.accrued_energy[0] = 0.0f;
    candidate.guarantee_flags = 0u;
    candidate.config_checksum = or_juplex_config_checksum(&candidate);
    if (!or_juplex_validate(&candidate)) return 0;
    *regime = candidate;
    return 1;
}

int or_juplex_set_mode(ORJuplexRegime *regime, uint32_t mode,
                        float prior_mix, float transition_hz) {
    if (!or_juplex_validate(regime) || mode >= OR_JUPLEX_MODE_COUNT ||
        (mode != OR_JUPLEX_CURRENT && !regime->prior_valid) ||
        !isfinite(prior_mix) || !isfinite(transition_hz)) return 0;
    ORJuplexRegime candidate = *regime;
    candidate.mode = mode;
    candidate.target_prior_mix = mode == OR_JUPLEX_CURRENT ? 0.0f :
        (mode == OR_JUPLEX_PRIOR ? 1.0f : prior_mix);
    candidate.transition_hz = transition_hz;
    candidate.guarantee_flags = 0u;
    candidate.config_checksum = or_juplex_config_checksum(&candidate);
    if (!or_juplex_validate(&candidate)) return 0;
    *regime = candidate;
    return 1;
}

/* Recall swaps editor profiles; the displaced current remains recoverable. */
int or_juplex_recall_prior(ORJuplexRegime *regime) {
    if (!or_juplex_validate(regime) || !regime->prior_valid) return 0;
    ORJuplexRegime candidate = *regime;
    const ORWaveDuplexorEditor displaced = candidate.current.duplexor_editor;
    candidate.current.duplexor_editor = candidate.prior_editor;
    candidate.prior_editor = displaced;
    const float energy = candidate.accrued_energy[0];
    candidate.accrued_energy[0] = candidate.accrued_energy[1];
    candidate.accrued_energy[1] = energy;
    candidate.current.config_checksum =
        or_wave_duplexor_group_checksum(&candidate.current);
    candidate.guarantee_flags = 0u;
    candidate.config_checksum = or_juplex_config_checksum(&candidate);
    if (!or_juplex_validate(&candidate)) return 0;
    *regime = candidate;
    return 1;
}

/* Decode once; apply two independently governed editors to identical lanes. */
int or_juplex_process(const ORMultiReasonerSet *reasoners,
                       ORJuplexRegime *regime,
                       const int8_t *left_codes,
                       const int8_t *right_codes,
                       float *left_output, float *right_output,
                       size_t frame_count) {
    if (!reasoners || !or_multi_reasoner_validate(reasoners) ||
        !or_juplex_validate(regime) ||
        reasoners->sample_rate != regime->current.sample_rate ||
        frame_count > OR_ENTROPY_DECOMPRESS_MAX_FRAMES ||
        (frame_count && (!left_codes || !right_codes ||
                         !left_output || !right_output))) return 0;
    if (frame_count == 0u) return 1;

    ORJuplexRegime candidate = *regime;
    float current_l[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float current_r[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float prior_l[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float prior_r[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    if (!or_multi_reasoner_double_decompress(
            reasoners, &candidate.current.decompressor_group,
            left_codes, right_codes, current_l, current_r, frame_count)) return 0;

    if (candidate.prior_valid) {
        memcpy(prior_l, current_l, frame_count * sizeof(float));
        memcpy(prior_r, current_r, frame_count * sizeof(float));
    }
    const float block_dt = (float)frame_count /
                           (float)candidate.current.sample_rate;
    const float entropy_response = 1.0f - expf(
        -OR_TWO_PI * candidate.concord.entropy_response_hz * block_dt);
    const float measured_entropy = or_clampf(
        candidate.current.decompressor_group.categories.entropy.combined_entropy,
        0.0f, 1.0f);
    candidate.entropy_modulation = or_clampf(
        candidate.entropy_modulation + entropy_response *
        (measured_entropy - candidate.entropy_modulation), 0.0f, 1.0f);

    ORWaveDuplexorEditor current_editor =
        candidate.current.duplexor_editor;
    current_editor.meet = or_clampf(current_editor.meet +
        candidate.concord.entropy_meet_depth *
        (1.0f - candidate.entropy_modulation), 0.0f, 1.0f);
    current_editor.separate = or_clampf(current_editor.separate +
        candidate.concord.entropy_separate_depth *
        candidate.entropy_modulation, 0.0f, 1.0f);
    current_editor.config_checksum =
        or_wave_duplexor_editor_checksum(&current_editor);

    ORWaveDuplexorEditor prior_editor = candidate.prior_editor;
    if (candidate.prior_valid) {
        /* The prior partner responds oppositely, forming the tango concord. */
        prior_editor.meet = or_clampf(prior_editor.meet +
            candidate.concord.entropy_meet_depth *
            candidate.entropy_modulation, 0.0f, 1.0f);
        prior_editor.separate = or_clampf(prior_editor.separate +
            candidate.concord.entropy_separate_depth *
            (1.0f - candidate.entropy_modulation), 0.0f, 1.0f);
        prior_editor.config_checksum =
            or_wave_duplexor_editor_checksum(&prior_editor);
    }
    if (!or_wave_duplexor_editor_process(&current_editor,
                                          current_l, current_r, frame_count) ||
        (candidate.prior_valid &&
         !or_wave_duplexor_editor_process(&prior_editor,
                                           prior_l, prior_r, frame_count))) return 0;
    candidate.current.duplexor_editor.generation = current_editor.generation;
    if (candidate.prior_valid)
        candidate.prior_editor.generation = prior_editor.generation;

    const float base_target = candidate.mode == OR_JUPLEX_CURRENT ? 0.0f :
        (candidate.mode == OR_JUPLEX_PRIOR ? 1.0f : candidate.target_prior_mix);
    const float transition = 1.0f - expf(-OR_TWO_PI * candidate.transition_hz /
                                         (float)candidate.current.sample_rate);
    const float accrual = 1.0f - expf(-OR_TWO_PI * 2.0f /
                                      (float)candidate.current.sample_rate);
    float guaranteed_peak = 0.0f;
    for (size_t i = 0; i < frame_count; ++i) {
        const float current_power = 0.5f *
            (current_l[i] * current_l[i] + current_r[i] * current_r[i]);
        candidate.accrued_energy[0] += accrual *
            (current_power - candidate.accrued_energy[0]);
        if (candidate.prior_valid) {
            const float prior_power = 0.5f *
                (prior_l[i] * prior_l[i] + prior_r[i] * prior_r[i]);
            candidate.accrued_energy[1] += accrual *
                (prior_power - candidate.accrued_energy[1]);
            const float tango = 0.5f + 0.5f *
                sinf(OR_TWO_PI * candidate.tango_phase);
            const float tango_span = candidate.concord.tango_depth *
                (0.25f + 0.75f * candidate.entropy_modulation);
            const float tango_target = candidate.mode == OR_JUPLEX_TANGO_MINISTRY
                ? or_clampf(base_target +
                    (2.0f * tango - 1.0f) * tango_span,
                    0.0f, candidate.concord.prior_authority)
                : or_clampf(base_target, 0.0f,
                            candidate.concord.prior_authority);
            candidate.prior_mix = or_clampf(candidate.prior_mix +
                transition * (tango_target - candidate.prior_mix),
                0.0f, candidate.concord.prior_authority);
            candidate.tango_phase += candidate.concord.tango_hz /
                                     (float)candidate.current.sample_rate;
            if (candidate.tango_phase >= 1.0f) candidate.tango_phase -= 1.0f;
        }
        const float keep = 1.0f - candidate.prior_mix;
        current_l[i] = or_clampf(keep * current_l[i] +
            (candidate.prior_valid ? candidate.prior_mix * prior_l[i] : 0.0f),
            -candidate.concord.output_ceiling,
             candidate.concord.output_ceiling);
        current_r[i] = or_clampf(keep * current_r[i] +
            (candidate.prior_valid ? candidate.prior_mix * prior_r[i] : 0.0f),
            -candidate.concord.output_ceiling,
             candidate.concord.output_ceiling);
        guaranteed_peak = fmaxf(guaranteed_peak, fabsf(current_l[i]));
        guaranteed_peak = fmaxf(guaranteed_peak, fabsf(current_r[i]));
    }
    candidate.guaranteed_peak = guaranteed_peak;
    candidate.guarantee_flags = OR_JUPLEX_GUARANTEE_ALL;
    if (candidate.generation != UINT32_MAX) candidate.generation++;
    if (!or_juplex_validate(&candidate)) return 0;
    memcpy(left_output, current_l, frame_count * sizeof(float));
    memcpy(right_output, current_r, frame_count * sizeof(float));
    *regime = candidate;
    return 1;
}

static uint32_t or_juplex_prior_envelope_checksum(
    const ORJuplexPriorEnvelope *envelope) {
    ORJuplexPriorEnvelope copy = *envelope;
    copy.checksum = 0u;
    return or_fnv1a32(&copy, sizeof(copy));
}

static uint32_t or_juplex_transvixor_checksum(
    const ORJuplexTransvixor *transvixor) {
    struct {
        uint32_t entity_decree;
        float max_gain_delta;
        float max_bias_delta;
        float max_topology_delta;
        float max_output_delta;
        float max_rate_delta;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.entity_decree = transvixor->entity_decree;
    contract.max_gain_delta = transvixor->max_gain_delta;
    contract.max_bias_delta = transvixor->max_bias_delta;
    contract.max_topology_delta = transvixor->max_topology_delta;
    contract.max_output_delta = transvixor->max_output_delta;
    contract.max_rate_delta = transvixor->max_rate_delta;
    return or_fnv1a32(&contract, sizeof(contract));
}

static int or_juplex_concord_values_valid(const ORJuplexConcord *concord) {
    return concord && isfinite(concord->entropy_response_hz) &&
           concord->entropy_response_hz >= 0.1f &&
           concord->entropy_response_hz <= 50.0f &&
           isfinite(concord->entropy_meet_depth) &&
           concord->entropy_meet_depth >= 0.0f &&
           concord->entropy_meet_depth <= 1.0f &&
           isfinite(concord->entropy_separate_depth) &&
           concord->entropy_separate_depth >= 0.0f &&
           concord->entropy_separate_depth <= 1.0f &&
           isfinite(concord->tango_hz) && concord->tango_hz >= 0.0f &&
           concord->tango_hz <= 20.0f && isfinite(concord->tango_depth) &&
           concord->tango_depth >= 0.0f && concord->tango_depth <= 1.0f &&
           isfinite(concord->prior_authority) &&
           concord->prior_authority >= 0.0f &&
           concord->prior_authority <= 1.0f &&
           isfinite(concord->output_ceiling) &&
           concord->output_ceiling >= 0.1f &&
           concord->output_ceiling <= 1.0f;
}

int or_juplex_prior_envelope_validate(
    const ORJuplexPriorEnvelope *envelope) {
    return envelope && envelope->entity_decree != 0u &&
           envelope->mode < OR_JUPLEX_MODE_COUNT &&
           isfinite(envelope->prior_mix) && envelope->prior_mix >= 0.0f &&
           envelope->prior_mix <= 1.0f &&
           isfinite(envelope->transition_hz) &&
           envelope->transition_hz >= 0.1f && envelope->transition_hz <= 50.0f &&
           envelope->sequence != 0u &&
           (envelope->state == OR_TRANSVIXOR_ENVELOPE_DORMANT ||
            envelope->state == OR_TRANSVIXOR_ENVELOPE_ENACTED ||
            envelope->state == OR_TRANSVIXOR_ENVELOPE_ABORTED) &&
           or_wave_duplexor_editor_validate(&envelope->proposed_editor) &&
           or_juplex_concord_values_valid(&envelope->proposed_concord) &&
           envelope->checksum == or_juplex_prior_envelope_checksum(envelope);
}

int or_juplex_transvixor_init(ORJuplexTransvixor *transvixor,
                              uint32_t entity_decree) {
    if (!transvixor || entity_decree == 0u) return 0;
    memset(transvixor, 0, sizeof(*transvixor));
    transvixor->entity_decree = entity_decree;
    transvixor->max_gain_delta = 0.50f;
    transvixor->max_bias_delta = 0.125f;
    transvixor->max_topology_delta = 0.50f;
    transvixor->max_output_delta = 0.35f;
    transvixor->max_rate_delta = 10.0f;
    transvixor->next_sequence = 1u;
    transvixor->config_checksum = or_juplex_transvixor_checksum(transvixor);
    return 1;
}

int or_juplex_transvixor_validate(const ORJuplexTransvixor *transvixor) {
    return transvixor && transvixor->entity_decree != 0u &&
           isfinite(transvixor->max_gain_delta) &&
           transvixor->max_gain_delta > 0.0f &&
           transvixor->max_gain_delta <= 2.0f &&
           isfinite(transvixor->max_bias_delta) &&
           transvixor->max_bias_delta > 0.0f &&
           transvixor->max_bias_delta <= 0.25f &&
           isfinite(transvixor->max_topology_delta) &&
           transvixor->max_topology_delta > 0.0f &&
           transvixor->max_topology_delta <= 1.0f &&
           isfinite(transvixor->max_output_delta) &&
           transvixor->max_output_delta > 0.0f &&
           transvixor->max_output_delta <= 1.0f &&
           isfinite(transvixor->max_rate_delta) &&
           transvixor->max_rate_delta > 0.0f &&
           transvixor->max_rate_delta <= 50.0f &&
           transvixor->next_sequence != 0u &&
           transvixor->enacted_sequence < transvixor->next_sequence &&
           transvixor->aborted_sequence < transvixor->next_sequence &&
           (transvixor->moat_flags & ~OR_TRANSVIXOR_MOAT_ALL) == 0u &&
           transvixor->config_checksum ==
               or_juplex_transvixor_checksum(transvixor);
}

static int or_juplex_transvixor_within_moat(
    const ORJuplexTransvixor *transvixor,
    const ORJuplexRegime *regime,
    const ORWaveDuplexorEditor *editor,
    const ORJuplexConcord *concord) {
    const ORWaveDuplexorEditor *active =
        &regime->current.duplexor_editor;
    if (fabsf(editor->lane_gain[0] - active->lane_gain[0]) >
            transvixor->max_gain_delta ||
        fabsf(editor->lane_gain[1] - active->lane_gain[1]) >
            transvixor->max_gain_delta ||
        fabsf(editor->lane_bias[0] - active->lane_bias[0]) >
            transvixor->max_bias_delta ||
        fabsf(editor->lane_bias[1] - active->lane_bias[1]) >
            transvixor->max_bias_delta ||
        fabsf(editor->meet - active->meet) >
            transvixor->max_topology_delta ||
        fabsf(editor->separate - active->separate) >
            transvixor->max_topology_delta ||
        fabsf(editor->output_gain - active->output_gain) >
            transvixor->max_output_delta ||
        fabsf(concord->entropy_meet_depth -
              regime->concord.entropy_meet_depth) >
            transvixor->max_topology_delta ||
        fabsf(concord->entropy_separate_depth -
              regime->concord.entropy_separate_depth) >
            transvixor->max_topology_delta ||
        fabsf(concord->tango_depth - regime->concord.tango_depth) >
            transvixor->max_topology_delta ||
        fabsf(concord->prior_authority - regime->concord.prior_authority) >
            transvixor->max_topology_delta ||
        fabsf(concord->output_ceiling - regime->concord.output_ceiling) >
            transvixor->max_output_delta ||
        fabsf(concord->entropy_response_hz -
              regime->concord.entropy_response_hz) >
            transvixor->max_rate_delta ||
        fabsf(concord->tango_hz - regime->concord.tango_hz) >
            transvixor->max_rate_delta) return 0;
    return 1;
}

/* Staging changes only the Transvixor and dormant envelope, never the entity. */
int or_juplex_transvixor_stage(
    ORJuplexTransvixor *transvixor,
    const ORJuplexRegime *regime,
    uint32_t entity_decree,
    const ORWaveDuplexorEditor *proposed_editor,
    const ORJuplexConcord *proposed_concord,
    uint32_t mode, float prior_mix, float transition_hz,
    ORJuplexPriorEnvelope *envelope) {
    if (!or_juplex_transvixor_validate(transvixor) ||
        !or_juplex_validate(regime) || !envelope ||
        entity_decree != transvixor->entity_decree ||
        !or_wave_duplexor_editor_validate(proposed_editor) ||
        !or_juplex_concord_values_valid(proposed_concord) ||
        mode >= OR_JUPLEX_MODE_COUNT || !isfinite(prior_mix) ||
        prior_mix < 0.0f || prior_mix > 1.0f ||
        !isfinite(transition_hz) || transition_hz < 0.1f ||
        transition_hz > 50.0f ||
        !or_juplex_transvixor_within_moat(
            transvixor, regime, proposed_editor, proposed_concord) ||
        transvixor->next_sequence == UINT32_MAX) return 0;

    ORJuplexPriorEnvelope candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.proposed_editor = *proposed_editor;
    candidate.proposed_concord = *proposed_concord;
    candidate.entity_decree = entity_decree;
    candidate.mode = mode;
    candidate.prior_mix = prior_mix;
    candidate.transition_hz = transition_hz;
    candidate.base_config_checksum = regime->config_checksum;
    candidate.base_generation = regime->generation;
    candidate.sequence = transvixor->next_sequence;
    candidate.state = OR_TRANSVIXOR_ENVELOPE_DORMANT;
    candidate.checksum = or_juplex_prior_envelope_checksum(&candidate);
    if (!or_juplex_prior_envelope_validate(&candidate)) return 0;

    ORJuplexTransvixor trans_candidate = *transvixor;
    trans_candidate.next_sequence++;
    trans_candidate.moat_flags = 0u;
    if (!or_juplex_transvixor_validate(&trans_candidate)) return 0;
    *envelope = candidate;
    *transvixor = trans_candidate;
    return 1;
}

/* Atomic enactment: the old current editor becomes the new prior editor. */
int or_juplex_transvixor_enact(ORJuplexTransvixor *transvixor,
                               ORJuplexRegime *regime,
                               ORJuplexPriorEnvelope *envelope) {
    if (!or_juplex_transvixor_validate(transvixor) ||
        !or_juplex_validate(regime) ||
        !or_juplex_prior_envelope_validate(envelope) ||
        envelope->state != OR_TRANSVIXOR_ENVELOPE_DORMANT ||
        envelope->entity_decree != transvixor->entity_decree ||
        envelope->sequence + 1u != transvixor->next_sequence ||
        envelope->base_config_checksum != regime->config_checksum ||
        envelope->base_generation != regime->generation ||
        !or_juplex_transvixor_within_moat(
            transvixor, regime, &envelope->proposed_editor,
            &envelope->proposed_concord) ||
        regime->generation == UINT32_MAX) return 0;

    ORJuplexRegime regime_candidate = *regime;
    if (!or_juplex_edit_current(
            &regime_candidate,
            envelope->proposed_editor.lane_gain[0],
            envelope->proposed_editor.lane_gain[1],
            envelope->proposed_editor.lane_bias[0],
            envelope->proposed_editor.lane_bias[1],
            envelope->proposed_editor.meet,
            envelope->proposed_editor.separate,
            envelope->proposed_editor.output_gain) ||
        !or_juplex_set_concord(
            &regime_candidate,
            envelope->proposed_concord.entropy_response_hz,
            envelope->proposed_concord.entropy_meet_depth,
            envelope->proposed_concord.entropy_separate_depth,
            envelope->proposed_concord.tango_hz,
            envelope->proposed_concord.tango_depth,
            envelope->proposed_concord.prior_authority,
            envelope->proposed_concord.output_ceiling) ||
        !or_juplex_set_mode(&regime_candidate, envelope->mode,
                            envelope->prior_mix,
                            envelope->transition_hz)) return 0;
    regime_candidate.generation++;
    if (!or_juplex_validate(&regime_candidate)) return 0;

    ORJuplexPriorEnvelope envelope_candidate = *envelope;
    envelope_candidate.state = OR_TRANSVIXOR_ENVELOPE_ENACTED;
    envelope_candidate.checksum =
        or_juplex_prior_envelope_checksum(&envelope_candidate);
    ORJuplexTransvixor trans_candidate = *transvixor;
    trans_candidate.enacted_sequence = envelope_candidate.sequence;
    trans_candidate.moat_flags = OR_TRANSVIXOR_MOAT_ALL;
    if (!or_juplex_prior_envelope_validate(&envelope_candidate) ||
        !or_juplex_transvixor_validate(&trans_candidate)) return 0;
    *regime = regime_candidate;
    *envelope = envelope_candidate;
    *transvixor = trans_candidate;
    return 1;
}

int or_juplex_transvixor_abort(ORJuplexTransvixor *transvixor,
                               ORJuplexPriorEnvelope *envelope) {
    if (!or_juplex_transvixor_validate(transvixor) ||
        !or_juplex_prior_envelope_validate(envelope) ||
        envelope->state != OR_TRANSVIXOR_ENVELOPE_DORMANT ||
        envelope->entity_decree != transvixor->entity_decree ||
        envelope->sequence + 1u != transvixor->next_sequence) return 0;
    ORJuplexPriorEnvelope envelope_candidate = *envelope;
    envelope_candidate.state = OR_TRANSVIXOR_ENVELOPE_ABORTED;
    envelope_candidate.checksum =
        or_juplex_prior_envelope_checksum(&envelope_candidate);
    ORJuplexTransvixor trans_candidate = *transvixor;
    trans_candidate.aborted_sequence = envelope_candidate.sequence;
    trans_candidate.moat_flags = 0u;
    if (!or_juplex_prior_envelope_validate(&envelope_candidate) ||
        !or_juplex_transvixor_validate(&trans_candidate)) return 0;
    *envelope = envelope_candidate;
    *transvixor = trans_candidate;
    return 1;
}

int or_juplex_transvixor_guaranteed(
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime) {
    return or_juplex_transvixor_validate(transvixor) &&
           or_juplex_prior_envelope_validate(envelope) &&
           or_juplex_validate(regime) &&
           envelope->state == OR_TRANSVIXOR_ENVELOPE_ENACTED &&
           envelope->entity_decree == transvixor->entity_decree &&
           transvixor->enacted_sequence == envelope->sequence &&
           transvixor->moat_flags == OR_TRANSVIXOR_MOAT_ALL &&
           regime->current.duplexor_editor.config_checksum ==
               envelope->proposed_editor.config_checksum &&
           memcmp(&regime->concord, &envelope->proposed_concord,
                  sizeof(regime->concord)) == 0 &&
           regime->mode == envelope->mode &&
           regime->transition_hz == envelope->transition_hz;
}

static uint32_t or_rejuker_kernel_checksum(const ORRejukerKernel *kernel) {
    struct {
        float compass_turns;
        float juke_depth;
        float forward_gain;
        float slew_limit;
        float output_ceiling;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.compass_turns = kernel->compass_turns;
    contract.juke_depth = kernel->juke_depth;
    contract.forward_gain = kernel->forward_gain;
    contract.slew_limit = kernel->slew_limit;
    contract.output_ceiling = kernel->output_ceiling;
    return or_fnv1a32(&contract, sizeof(contract));
}

static uint32_t or_rejuker_config_checksum(
    const ORRejukerPriorGuarantee *rejuker) {
    struct {
        uint32_t kernel_checksum;
        uint32_t entity_decree;
        uint32_t envelope_sequence;
        uint32_t minimum_priority;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.kernel_checksum = rejuker->kernel.config_checksum;
    contract.entity_decree = rejuker->entity_decree;
    contract.envelope_sequence = rejuker->envelope_sequence;
    contract.minimum_priority = rejuker->minimum_priority;
    return or_fnv1a32(&contract, sizeof(contract));
}

int or_rejuker_kernel_validate(const ORRejukerKernel *kernel) {
    return kernel && isfinite(kernel->compass_turns) &&
           kernel->compass_turns >= 0.0f && kernel->compass_turns < 1.0f &&
           isfinite(kernel->juke_depth) && kernel->juke_depth >= 0.0f &&
           kernel->juke_depth <= 1.0f && isfinite(kernel->forward_gain) &&
           kernel->forward_gain >= 0.0f && kernel->forward_gain <= 1.5f &&
           isfinite(kernel->slew_limit) && kernel->slew_limit > 0.0f &&
           kernel->slew_limit <= 1.0f && isfinite(kernel->output_ceiling) &&
           kernel->output_ceiling >= 0.1f && kernel->output_ceiling <= 1.0f &&
           isfinite(kernel->previous_output[0]) &&
           fabsf(kernel->previous_output[0]) <= kernel->output_ceiling &&
           isfinite(kernel->previous_output[1]) &&
           fabsf(kernel->previous_output[1]) <= kernel->output_ceiling &&
           kernel->generation != UINT32_MAX &&
           kernel->config_checksum == or_rejuker_kernel_checksum(kernel);
}

int or_rejuker_validate(const ORRejukerPriorGuarantee *rejuker) {
    return rejuker && rejuker->entity_decree != 0u &&
           rejuker->envelope_sequence != 0u &&
           rejuker->minimum_priority < OR_FORESHADOW_PRIORITY_COUNT &&
           or_rejuker_kernel_validate(&rejuker->kernel) &&
           (rejuker->guarantee_flags & ~OR_REJUKER_GUARANTEE_ALL) == 0u &&
           isfinite(rejuker->guaranteed_peak) &&
           rejuker->guaranteed_peak >= 0.0f &&
           rejuker->guaranteed_peak <= rejuker->kernel.output_ceiling &&
           rejuker->config_checksum == or_rejuker_config_checksum(rejuker);
}

int or_rejuker_init(ORRejukerPriorGuarantee *rejuker,
                     const ORJuplexTransvixor *transvixor,
                     const ORJuplexPriorEnvelope *envelope,
                     const ORJuplexRegime *regime) {
    if (!rejuker || !or_juplex_transvixor_guaranteed(
            transvixor, envelope, regime) || !regime->prior_valid) return 0;
    memset(rejuker, 0, sizeof(*rejuker));
    rejuker->kernel.compass_turns = 0.0f;
    rejuker->kernel.juke_depth = 0.45f;
    rejuker->kernel.forward_gain = 0.92f;
    rejuker->kernel.slew_limit = 0.18f;
    rejuker->kernel.output_ceiling = fminf(
        0.90f, regime->concord.output_ceiling);
    rejuker->kernel.config_checksum =
        or_rejuker_kernel_checksum(&rejuker->kernel);
    rejuker->entity_decree = envelope->entity_decree;
    rejuker->envelope_sequence = envelope->sequence;
    rejuker->minimum_priority = OR_FORESHADOW_PRIORITY_NORMAL;
    rejuker->config_checksum = or_rejuker_config_checksum(rejuker);
    return or_rejuker_validate(rejuker);
}

int or_rejuker_set_compass(ORRejukerPriorGuarantee *rejuker,
                            float compass_turns, float juke_depth,
                            float forward_gain, float slew_limit,
                            float output_ceiling) {
    if (!or_rejuker_validate(rejuker) || !isfinite(compass_turns) ||
        !isfinite(juke_depth) || !isfinite(forward_gain) ||
        !isfinite(slew_limit) || !isfinite(output_ceiling)) return 0;
    ORRejukerPriorGuarantee candidate = *rejuker;
    candidate.kernel.compass_turns = compass_turns;
    candidate.kernel.juke_depth = juke_depth;
    candidate.kernel.forward_gain = forward_gain;
    candidate.kernel.slew_limit = slew_limit;
    candidate.kernel.output_ceiling = output_ceiling;
    candidate.kernel.previous_output[0] = or_clampf(
        candidate.kernel.previous_output[0], -output_ceiling, output_ceiling);
    candidate.kernel.previous_output[1] = or_clampf(
        candidate.kernel.previous_output[1], -output_ceiling, output_ceiling);
    candidate.kernel.config_checksum =
        or_rejuker_kernel_checksum(&candidate.kernel);
    candidate.guarantee_flags = 0u;
    candidate.guaranteed_peak = fminf(candidate.guaranteed_peak,
                                      output_ceiling);
    candidate.config_checksum = or_rejuker_config_checksum(&candidate);
    if (!or_rejuker_validate(&candidate)) return 0;
    *rejuker = candidate;
    return 1;
}

int or_rejuker_set_minimum_priority(ORRejukerPriorGuarantee *rejuker,
                                    uint32_t minimum_priority) {
    if (!or_rejuker_validate(rejuker) ||
        minimum_priority >= OR_FORESHADOW_PRIORITY_COUNT) return 0;
    ORRejukerPriorGuarantee candidate = *rejuker;
    candidate.minimum_priority = minimum_priority;
    candidate.guarantee_flags = 0u;
    candidate.config_checksum = or_rejuker_config_checksum(&candidate);
    if (!or_rejuker_validate(&candidate)) return 0;
    *rejuker = candidate;
    return 1;
}

/* Juplexor -> prior envelope -> compass kernel -> guaranteed forward output. */
int or_rejuker_forward(const ORMultiReasonerSet *reasoners,
                        const ORJuplexTransvixor *transvixor,
                        const ORJuplexPriorEnvelope *envelope,
                        ORJuplexRegime *regime,
                        ORRejukerPriorGuarantee *rejuker,
                        const int8_t *left_codes,
                        const int8_t *right_codes,
                        float *left_output, float *right_output,
                        size_t frame_count) {
    if (!reasoners || !or_multi_reasoner_validate(reasoners) ||
        !or_juplex_transvixor_guaranteed(transvixor, envelope, regime) ||
        !or_rejuker_validate(rejuker) || !regime->prior_valid ||
        rejuker->entity_decree != envelope->entity_decree ||
        rejuker->envelope_sequence != envelope->sequence ||
        frame_count > OR_ENTROPY_DECOMPRESS_MAX_FRAMES ||
        UINT64_MAX - rejuker->forwarded_frames < frame_count ||
        (frame_count && (!left_codes || !right_codes ||
                         !left_output || !right_output))) return 0;
    if (frame_count == 0u) return 1;

    ORJuplexRegime regime_candidate = *regime;
    ORRejukerPriorGuarantee rejuker_candidate = *rejuker;
    float left[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float right[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    if (!or_juplex_process(reasoners, &regime_candidate,
                            left_codes, right_codes,
                            left, right, frame_count) ||
        !or_juplex_guaranteed(&regime_candidate)) return 0;

    ORRejukerKernel *kernel = &rejuker_candidate.kernel;
    float effective_turns = kernel->compass_turns +
                            0.25f * regime_candidate.prior_mix;
    effective_turns -= floorf(effective_turns);
    const float c = cosf(OR_TWO_PI * effective_turns);
    const float s = sinf(OR_TWO_PI * effective_turns);
    float peak = 0.0f;
    for (size_t i = 0; i < frame_count; ++i) {
        const float routed_l = c * left[i] - s * right[i];
        const float routed_r = s * left[i] + c * right[i];
        const float target_l = kernel->forward_gain *
            ((1.0f - kernel->juke_depth) * left[i] +
             kernel->juke_depth * routed_l);
        const float target_r = kernel->forward_gain *
            ((1.0f - kernel->juke_depth) * right[i] +
             kernel->juke_depth * routed_r);
        const float next_l = kernel->previous_output[0] + or_clampf(
            target_l - kernel->previous_output[0],
            -kernel->slew_limit, kernel->slew_limit);
        const float next_r = kernel->previous_output[1] + or_clampf(
            target_r - kernel->previous_output[1],
            -kernel->slew_limit, kernel->slew_limit);
        left[i] = or_clampf(next_l, -kernel->output_ceiling,
                            kernel->output_ceiling);
        right[i] = or_clampf(next_r, -kernel->output_ceiling,
                             kernel->output_ceiling);
        kernel->previous_output[0] = left[i];
        kernel->previous_output[1] = right[i];
        peak = fmaxf(peak, fabsf(left[i]));
        peak = fmaxf(peak, fabsf(right[i]));
        if (kernel->generation != UINT32_MAX) kernel->generation++;
    }
    rejuker_candidate.forwarded_frames += frame_count;
    rejuker_candidate.guaranteed_peak = peak;
    rejuker_candidate.guarantee_flags = OR_REJUKER_GUARANTEE_ALL;
    if (!or_rejuker_validate(&rejuker_candidate)) return 0;
    memcpy(left_output, left, frame_count * sizeof(float));
    memcpy(right_output, right, frame_count * sizeof(float));
    *regime = regime_candidate;
    *rejuker = rejuker_candidate;
    return 1;
}

int or_rejuker_guaranteed(
    const ORRejukerPriorGuarantee *rejuker,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime) {
    return or_rejuker_validate(rejuker) &&
           or_juplex_transvixor_guaranteed(transvixor, envelope, regime) &&
           or_juplex_guaranteed(regime) && regime->prior_valid &&
           rejuker->entity_decree == envelope->entity_decree &&
           rejuker->envelope_sequence == envelope->sequence &&
           rejuker->guarantee_flags == OR_REJUKER_GUARANTEE_ALL &&
           rejuker->guaranteed_peak <= rejuker->kernel.output_ceiling;
}

static uint32_t or_rejuker_foreshadow_checksum(
    const ORRejukerForeshadow *foreshadow) {
    ORRejukerForeshadow copy = *foreshadow;
    copy.checksum = 0u;
    return or_fnv1a32(&copy, sizeof(copy));
}

static uint32_t or_rejuker_input_checksum(const int8_t *left_codes,
                                           const int8_t *right_codes,
                                           size_t frame_count) {
    struct {
        uint32_t frame_count;
        uint32_t left_checksum;
        uint32_t right_checksum;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.frame_count = (uint32_t)frame_count;
    contract.left_checksum = or_fnv1a32(left_codes, frame_count);
    contract.right_checksum = or_fnv1a32(right_codes, frame_count);
    return or_fnv1a32(&contract, sizeof(contract));
}

static uint32_t or_rejuker_output_checksum(const float *left,
                                            const float *right,
                                            size_t frame_count) {
    struct {
        uint32_t frame_count;
        uint32_t left_checksum;
        uint32_t right_checksum;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.frame_count = (uint32_t)frame_count;
    contract.left_checksum = or_fnv1a32(
        left, frame_count * sizeof(float));
    contract.right_checksum = or_fnv1a32(
        right, frame_count * sizeof(float));
    return or_fnv1a32(&contract, sizeof(contract));
}

int or_rejuker_foreshadow_validate(const ORRejukerForeshadow *foreshadow) {
    return foreshadow && foreshadow->entity_decree != 0u &&
           foreshadow->envelope_sequence != 0u &&
           foreshadow->priority < OR_FORESHADOW_PRIORITY_COUNT &&
           (foreshadow->state == OR_FORESHADOW_READY ||
            foreshadow->state == OR_FORESHADOW_CONSUMED ||
            foreshadow->state == OR_FORESHADOW_ABORTED) &&
           foreshadow->frame_count > 0u &&
           foreshadow->frame_count <= OR_ENTROPY_DECOMPRESS_MAX_FRAMES &&
           isfinite(foreshadow->predicted_peak) &&
           foreshadow->predicted_peak >= 0.0f &&
           foreshadow->predicted_peak <= 1.0f &&
           foreshadow->guarantee_flags == OR_FORESHADOW_GUARANTEE_ALL &&
           foreshadow->checksum ==
               or_rejuker_foreshadow_checksum(foreshadow);
}

/* Predict the exact forward block on copied state; live state is const. */
int or_rejuker_foreshadow(
    const ORMultiReasonerSet *reasoners,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime,
    const ORRejukerPriorGuarantee *rejuker,
    uint32_t priority,
    const int8_t *left_codes,
    const int8_t *right_codes,
    float *preview_left, float *preview_right,
    size_t frame_count,
    ORRejukerForeshadow *foreshadow) {
    if (!reasoners || !or_multi_reasoner_validate(reasoners) ||
        !or_juplex_transvixor_guaranteed(transvixor, envelope, regime) ||
        !or_rejuker_validate(rejuker) || !foreshadow ||
        priority >= OR_FORESHADOW_PRIORITY_COUNT ||
        priority < rejuker->minimum_priority || frame_count == 0u ||
        frame_count > OR_ENTROPY_DECOMPRESS_MAX_FRAMES ||
        !left_codes || !right_codes || !preview_left || !preview_right)
        return 0;

    ORJuplexRegime regime_candidate = *regime;
    ORRejukerPriorGuarantee rejuker_candidate = *rejuker;
    float left[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float right[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    if (!or_rejuker_forward(reasoners, transvixor, envelope,
                             &regime_candidate, &rejuker_candidate,
                             left_codes, right_codes,
                             left, right, frame_count)) return 0;

    ORRejukerForeshadow ticket;
    memset(&ticket, 0, sizeof(ticket));
    ticket.entity_decree = envelope->entity_decree;
    ticket.envelope_sequence = envelope->sequence;
    ticket.priority = priority;
    ticket.state = OR_FORESHADOW_READY;
    ticket.frame_count = (uint32_t)frame_count;
    ticket.base_regime_config_checksum = regime->config_checksum;
    ticket.base_regime_generation = regime->generation;
    ticket.base_rejuker_config_checksum = rejuker->config_checksum;
    ticket.base_kernel_generation = rejuker->kernel.generation;
    ticket.base_forwarded_frames = rejuker->forwarded_frames;
    ticket.input_checksum = or_rejuker_input_checksum(
        left_codes, right_codes, frame_count);
    ticket.predicted_output_checksum = or_rejuker_output_checksum(
        left, right, frame_count);
    ticket.predicted_peak = rejuker_candidate.guaranteed_peak;
    ticket.guarantee_flags = OR_FORESHADOW_GUARANTEE_ALL;
    ticket.checksum = or_rejuker_foreshadow_checksum(&ticket);
    if (!or_rejuker_foreshadow_validate(&ticket)) return 0;
    memcpy(preview_left, left, frame_count * sizeof(float));
    memcpy(preview_right, right, frame_count * sizeof(float));
    *foreshadow = ticket;
    return 1;
}

int or_rejuker_foreshadow_is_current(
    const ORRejukerForeshadow *foreshadow,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime,
    const ORRejukerPriorGuarantee *rejuker) {
    return or_rejuker_foreshadow_validate(foreshadow) &&
           foreshadow->state == OR_FORESHADOW_READY &&
           envelope && regime && rejuker &&
           foreshadow->entity_decree == envelope->entity_decree &&
           foreshadow->envelope_sequence == envelope->sequence &&
           foreshadow->priority >= rejuker->minimum_priority &&
           foreshadow->base_regime_config_checksum == regime->config_checksum &&
           foreshadow->base_regime_generation == regime->generation &&
           foreshadow->base_rejuker_config_checksum == rejuker->config_checksum &&
           foreshadow->base_kernel_generation == rejuker->kernel.generation &&
           foreshadow->base_forwarded_frames == rejuker->forwarded_frames;
}

/* Commit only when the live replay exactly matches the foreshadowed digest. */
int or_rejuker_forward_foreshadowed(
    const ORMultiReasonerSet *reasoners,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    ORJuplexRegime *regime,
    ORRejukerPriorGuarantee *rejuker,
    const int8_t *left_codes,
    const int8_t *right_codes,
    float *left_output, float *right_output,
    size_t frame_count,
    ORRejukerForeshadow *foreshadow) {
    if (!reasoners || !or_multi_reasoner_validate(reasoners) ||
        !or_juplex_transvixor_guaranteed(transvixor, envelope, regime) ||
        !or_rejuker_validate(rejuker) ||
        !or_rejuker_foreshadow_is_current(
            foreshadow, envelope, regime, rejuker) ||
        frame_count != foreshadow->frame_count || !left_codes ||
        !right_codes || !left_output || !right_output ||
        or_rejuker_input_checksum(left_codes, right_codes, frame_count) !=
            foreshadow->input_checksum) return 0;

    ORJuplexRegime regime_candidate = *regime;
    ORRejukerPriorGuarantee rejuker_candidate = *rejuker;
    float left[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    float right[OR_ENTROPY_DECOMPRESS_MAX_FRAMES];
    if (!or_rejuker_forward(reasoners, transvixor, envelope,
                             &regime_candidate, &rejuker_candidate,
                             left_codes, right_codes,
                             left, right, frame_count) ||
        or_rejuker_output_checksum(left, right, frame_count) !=
            foreshadow->predicted_output_checksum ||
        rejuker_candidate.guaranteed_peak != foreshadow->predicted_peak)
        return 0;

    ORRejukerForeshadow ticket = *foreshadow;
    ticket.state = OR_FORESHADOW_CONSUMED;
    ticket.checksum = or_rejuker_foreshadow_checksum(&ticket);
    if (!or_rejuker_foreshadow_validate(&ticket)) return 0;
    memcpy(left_output, left, frame_count * sizeof(float));
    memcpy(right_output, right, frame_count * sizeof(float));
    *regime = regime_candidate;
    *rejuker = rejuker_candidate;
    *foreshadow = ticket;
    return 1;
}

int or_rejuker_foreshadow_abort(ORRejukerForeshadow *foreshadow) {
    if (!or_rejuker_foreshadow_validate(foreshadow) ||
        foreshadow->state != OR_FORESHADOW_READY) return 0;
    ORRejukerForeshadow ticket = *foreshadow;
    ticket.state = OR_FORESHADOW_ABORTED;
    ticket.checksum = or_rejuker_foreshadow_checksum(&ticket);
    if (!or_rejuker_foreshadow_validate(&ticket)) return 0;
    *foreshadow = ticket;
    return 1;
}

static uint32_t or_rejuker_markov_contract_checksum(
    const ORRejukerMarkovChain *markov) {
    struct {
        float transition[OR_REJUKER_MARKOV_STATE_COUNT]
                        [OR_REJUKER_MARKOV_STATE_COUNT];
        float apriori[OR_REJUKER_MARKOV_STATE_COUNT];
    } contract;
    memset(&contract, 0, sizeof(contract));
    memcpy(contract.transition, markov->transition,
           sizeof(contract.transition));
    memcpy(contract.apriori, markov->apriori, sizeof(contract.apriori));
    return or_fnv1a32(&contract, sizeof(contract));
}

static uint32_t or_rejuker_markov_state_checksum(
    const ORRejukerMarkovChain *markov) {
    struct {
        float current[OR_REJUKER_MARKOV_STATE_COUNT];
        uint64_t tick;
        uint32_t contract_checksum;
    } state;
    memset(&state, 0, sizeof(state));
    memcpy(state.current, markov->current, sizeof(state.current));
    state.tick = markov->tick;
    state.contract_checksum = markov->contract_checksum;
    return or_fnv1a32(&state, sizeof(state));
}

static uint32_t or_rejuker_regime_config_checksum(
    const ORRejukerFunctionRegimeClass *function_class) {
    struct {
        uint32_t entity_decree;
        uint32_t envelope_sequence;
        uint32_t markov_contract_checksum;
    } contract;
    memset(&contract, 0, sizeof(contract));
    contract.entity_decree = function_class->entity_decree;
    contract.envelope_sequence = function_class->envelope_sequence;
    contract.markov_contract_checksum =
        function_class->markov.contract_checksum;
    return or_fnv1a32(&contract, sizeof(contract));
}

static uint32_t or_rejuker_regime_state_checksum(
    const ORRejukerFunctionRegimeClass *function_class) {
    ORRejukerFunctionRegimeClass copy = *function_class;
    copy.checksum = 0u;
    return or_fnv1a32(&copy, sizeof(copy));
}

static int or_rejuker_probability_vector_validate(
    const float probability[OR_REJUKER_MARKOV_STATE_COUNT]) {
    float sum = 0.0f;
    for (uint32_t state = 0u;
         state < OR_REJUKER_MARKOV_STATE_COUNT; ++state) {
        if (!isfinite(probability[state]) || probability[state] < 0.0f ||
            probability[state] > 1.0f) return 0;
        sum += probability[state];
    }
    return fabsf(sum - 1.0f) <= 0.00001f;
}

int or_rejuker_markov_validate(const ORRejukerMarkovChain *markov) {
    if (!markov || markov->tick == UINT64_MAX ||
        markov->contract_checksum !=
            or_rejuker_markov_contract_checksum(markov) ||
        markov->state_checksum != or_rejuker_markov_state_checksum(markov) ||
        !or_rejuker_probability_vector_validate(markov->apriori) ||
        !or_rejuker_probability_vector_validate(markov->current)) return 0;
    for (uint32_t row = 0u; row < OR_REJUKER_MARKOV_STATE_COUNT; ++row)
        if (!or_rejuker_probability_vector_validate(markov->transition[row]))
            return 0;
    return 1;
}

int or_rejuker_regime_checksum_validate(
    const ORRejukerFunctionRegimeClass *function_class) {
    return function_class &&
           function_class->markov.contract_checksum ==
               or_rejuker_markov_contract_checksum(&function_class->markov) &&
           function_class->markov.state_checksum ==
               or_rejuker_markov_state_checksum(&function_class->markov) &&
           function_class->config_checksum ==
               or_rejuker_regime_config_checksum(function_class) &&
           function_class->checksum ==
               or_rejuker_regime_state_checksum(function_class);
}

int or_rejuker_regime_class_validate(
    const ORRejukerFunctionRegimeClass *function_class) {
    return function_class && function_class->entity_decree != 0u &&
           function_class->envelope_sequence != 0u &&
           function_class->generation != UINT32_MAX &&
           (function_class->property_flags & ~OR_REJUKER_PROPERTY_ALL) == 0u &&
           or_rejuker_markov_validate(&function_class->markov) &&
           or_rejuker_regime_checksum_validate(function_class);
}

int or_rejuker_regime_class_init(
    ORRejukerFunctionRegimeClass *function_class,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime,
    const ORRejukerPriorGuarantee *rejuker) {
    static const float transition[OR_REJUKER_MARKOV_STATE_COUNT]
                                 [OR_REJUKER_MARKOV_STATE_COUNT] = {
        {0.70f, 0.15f, 0.05f, 0.10f},
        {0.15f, 0.65f, 0.10f, 0.10f},
        {0.10f, 0.10f, 0.60f, 0.20f},
        {0.10f, 0.10f, 0.10f, 0.70f}
    };
    static const float apriori[OR_REJUKER_MARKOV_STATE_COUNT] = {
        0.55f, 0.25f, 0.10f, 0.10f
    };
    if (!function_class ||
        !or_rejuker_guaranteed(rejuker, transvixor, envelope, regime))
        return 0;
    memset(function_class, 0, sizeof(*function_class));
    memcpy(function_class->markov.transition, transition,
           sizeof(transition));
    memcpy(function_class->markov.apriori, apriori, sizeof(apriori));
    memcpy(function_class->markov.current, apriori, sizeof(apriori));
    function_class->markov.contract_checksum =
        or_rejuker_markov_contract_checksum(&function_class->markov);
    function_class->markov.state_checksum =
        or_rejuker_markov_state_checksum(&function_class->markov);
    function_class->entity_decree = envelope->entity_decree;
    function_class->envelope_sequence = envelope->sequence;
    function_class->config_checksum =
        or_rejuker_regime_config_checksum(function_class);
    function_class->checksum =
        or_rejuker_regime_state_checksum(function_class);
    return or_rejuker_regime_class_validate(function_class);
}

int or_rejuker_regime_class_guaranteed(
    const ORRejukerFunctionRegimeClass *function_class,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime,
    const ORRejukerPriorGuarantee *rejuker) {
    return or_rejuker_regime_class_validate(function_class) &&
           or_rejuker_guaranteed(rejuker, transvixor, envelope, regime) &&
           function_class->entity_decree == envelope->entity_decree &&
           function_class->envelope_sequence == envelope->sequence &&
           function_class->markov.tick > 0u &&
           function_class->property_flags == OR_REJUKER_PROPERTY_ALL;
}

/* Advance on a copy, normalize the posterior, then atomically commit it. */
int or_rejuker_regime_class_tick(
    ORRejukerFunctionRegimeClass *function_class,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime,
    const ORRejukerPriorGuarantee *rejuker,
    const ORRejukerForeshadow *foreshadow) {
    if (!or_rejuker_regime_class_validate(function_class) ||
        !or_rejuker_guaranteed(rejuker, transvixor, envelope, regime) ||
        function_class->entity_decree != envelope->entity_decree ||
        function_class->envelope_sequence != envelope->sequence ||
        function_class->markov.tick == UINT64_MAX - 1u ||
        function_class->generation == UINT32_MAX - 1u ||
        (foreshadow &&
         (!or_rejuker_foreshadow_validate(foreshadow) ||
          foreshadow->entity_decree != envelope->entity_decree ||
          foreshadow->envelope_sequence != envelope->sequence))) return 0;

    ORRejukerFunctionRegimeClass candidate = *function_class;
    float predicted[OR_REJUKER_MARKOV_STATE_COUNT] = {0.0f};
    for (uint32_t destination = 0u;
         destination < OR_REJUKER_MARKOV_STATE_COUNT; ++destination)
        for (uint32_t source = 0u;
             source < OR_REJUKER_MARKOV_STATE_COUNT; ++source)
            predicted[destination] += candidate.markov.current[source] *
                candidate.markov.transition[source][destination];

    const float ceiling = fmaxf(rejuker->kernel.output_ceiling, 0.000001f);
    const float observation[OR_REJUKER_MARKOV_STATE_COUNT] = {
        fmaxf(0.01f, 1.0f - regime->prior_mix),
        fmaxf(0.01f, regime->prior_mix),
        foreshadow && (foreshadow->state == OR_FORESHADOW_READY ||
                       foreshadow->state == OR_FORESHADOW_CONSUMED)
            ? 0.90f : 0.10f,
        0.25f + 0.75f * or_clampf(
            rejuker->guaranteed_peak / ceiling, 0.0f, 1.0f)
    };
    float normalizer = 0.0f;
    for (uint32_t state = 0u;
         state < OR_REJUKER_MARKOV_STATE_COUNT; ++state) {
        candidate.markov.current[state] = predicted[state] *
                                           observation[state];
        normalizer += candidate.markov.current[state];
    }
    if (!isfinite(normalizer) || normalizer <= 0.000001f) return 0;
    for (uint32_t state = 0u;
         state < OR_REJUKER_MARKOV_STATE_COUNT; ++state)
        candidate.markov.current[state] /= normalizer;

    candidate.markov.tick++;
    candidate.generation++;
    candidate.property_flags = OR_REJUKER_PROPERTY_ALL;
    candidate.markov.state_checksum =
        or_rejuker_markov_state_checksum(&candidate.markov);
    candidate.checksum = or_rejuker_regime_state_checksum(&candidate);
    if (!or_rejuker_regime_class_guaranteed(
            &candidate, transvixor, envelope, regime, rejuker)) return 0;
    *function_class = candidate;
    return 1;
}

int or_rejuker_property_glass(
    const ORRejukerFunctionRegimeClass *function_class,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime,
    const ORRejukerPriorGuarantee *rejuker,
    ORRejukerPropertyGlass *glass) {
    if (!glass || !or_rejuker_regime_class_validate(function_class) ||
        !or_rejuker_validate(rejuker) || !or_juplex_validate(regime))
        return 0;
    memset(glass, 0, sizeof(*glass));
    glass->property_flags = function_class->property_flags;
    glass->generation = function_class->generation;
    glass->markov_tick = function_class->markov.tick;
    memcpy(glass->apriori, function_class->markov.apriori,
           sizeof(glass->apriori));
    memcpy(glass->posterior, function_class->markov.current,
           sizeof(glass->posterior));
    for (uint32_t state = 1u;
         state < OR_REJUKER_MARKOV_STATE_COUNT; ++state)
        if (glass->posterior[state] >
            glass->posterior[glass->dominant_state])
            glass->dominant_state = state;
    glass->prior_mix = regime->prior_mix;
    glass->forward_peak = rejuker->guaranteed_peak;
    glass->forwarded_frames = rejuker->forwarded_frames;
    glass->minimum_priority = rejuker->minimum_priority;
    glass->contract_checksum = function_class->markov.contract_checksum;
    glass->state_checksum = function_class->markov.state_checksum;
    glass->guaranteed = or_rejuker_regime_class_guaranteed(
        function_class, transvixor, envelope, regime, rejuker);
    return 1;
}

/* Prior self-test: exercise one future tick without advancing live state. */
int or_rejuker_test_prior(
    const ORRejukerFunctionRegimeClass *function_class,
    const ORJuplexTransvixor *transvixor,
    const ORJuplexPriorEnvelope *envelope,
    const ORJuplexRegime *regime,
    const ORRejukerPriorGuarantee *rejuker,
    const ORRejukerForeshadow *foreshadow,
    ORRejukerPropertyGlass *glass) {
    if (!or_rejuker_regime_class_validate(function_class)) return 0;
    const ORRejukerFunctionRegimeClass before = *function_class;
    ORRejukerFunctionRegimeClass candidate = before;
    if (!or_rejuker_regime_class_tick(
            &candidate, transvixor, envelope, regime, rejuker,
            foreshadow) ||
        candidate.markov.tick != before.markov.tick + 1u ||
        candidate.generation != before.generation + 1u ||
        memcmp(candidate.markov.apriori, before.markov.apriori,
               sizeof(before.markov.apriori)) != 0 ||
        memcmp(function_class, &before, sizeof(before)) != 0 ||
        (glass && !or_rejuker_property_glass(
            &candidate, transvixor, envelope, regime, rejuker, glass)))
        return 0;
    return 1;
}

static int or_multi_reasoner_fuse(const ORMultiReasonerSet *set,
                                  ORCell *fused) {
    if (!or_multi_reasoner_validate(set) || !fused) return 0;
    ORCell snapshot[OR_MULTI_REASONER_SLOTS];
    for (uint32_t i = 0; i < OR_MULTI_REASONER_SLOTS; ++i) {
        memcpy(&snapshot[i], set->slot[i].cell, sizeof(snapshot[i]));
        if (!or_validate(&snapshot[i]) ||
            snapshot[i].guard.checksum != set->slot[i].expected_checksum ||
            snapshot[i].guard.generation != set->slot[i].expected_generation)
            return 0;
    }
    const float total = set->slot[0].weight + set->slot[1].weight;
    const float w0 = set->slot[0].weight / total;
    const float w1 = set->slot[1].weight / total;
    *fused = snapshot[0];
    for (uint32_t i = 0; i < OR_WAVE_SAMPLES; ++i) {
        const float mixed = w0 * (float)snapshot[0].wave.sample[i] +
                            w1 * (float)snapshot[1].wave.sample[i];
        fused->wave.sample[i] = (int16_t)lrintf(or_clampf(mixed,
                                                          -32768.0f, 32767.0f));
    }
    for (uint32_t i = 0; i < OR_NODE_COUNT; ++i) {
        ORNode *out = &fused->guard.node[i];
        const ORNode *a = &snapshot[0].guard.node[i];
        const ORNode *b = &snapshot[1].guard.node[i];
        out->amplitude = or_clampf(w0 * a->amplitude + w1 * b->amplitude, 0.0f, 1.0f);
        out->confidence = or_clampf(w0 * a->confidence + w1 * b->confidence, 0.0f, 1.0f);
        out->memory = or_clampf(w0 * a->memory + w1 * b->memory, 0.0f, 1.0f);
        out->evidence = or_clampf(w0 * a->evidence + w1 * b->evidence, -1.0f, 1.0f);
        out->stability = fmaxf(0.0f, w0 * a->stability + w1 * b->stability);
        const float px = w0 * cosf(a->phase) + w1 * cosf(b->phase);
        const float py = w0 * sinf(a->phase) + w1 * sinf(b->phase);
        out->phase = or_wrap_phase(atan2f(py, px));
        out->omega = fmaxf(0.0f, w0 * a->omega + w1 * b->omega);
        out->status = out->amplitude >= 0.5f ? OR_NODE_EXCITED : OR_NODE_DECAYING;
    }
    fused->guard.generation = snapshot[0].guard.generation > snapshot[1].guard.generation
                            ? snapshot[0].guard.generation : snapshot[1].guard.generation;
    fused->guard.flags = OR_FLAG_VALID;
    or_refresh_checksum(fused);
    return or_validate(fused);
}

/*
 * Project-local invention name: Terrence Terahertz Neck.
 * This is a numerical control oscillator and down-converter, not a physical
 * terahertz emitter. It produces an audio-rate neck signal from virtual THz
 * carrier differences while keeping the actual renderer inside Nyquist.
 * Project-originated experimental arrangement; no third-party implementation
 * is copied here and no license grant is implied by this source comment.
 */
#define OR_TERRENCE_INVENTION_NAME "Terrence Terahertz Neck"
#define OR_TERRENCE_SOURCE_CATEGORY "C source / Terrence"
enum {
    OR_SOURCE_CATEGORY_C_SOURCE = 1u,
    OR_SOURCE_CATEGORY_TERRENCE = 2u
};

enum {
    OR_SCREENER_SOURCE_OK  = 1u << 0,
    OR_SCREENER_THz_OK     = 1u << 1,
    OR_SCREENER_NECK_OK    = 1u << 2,
    OR_SCREENER_CURRENT_OK = 1u << 3
};

typedef struct {
    uint32_t source_category;
    uint32_t flags;
    double carrier_a_hz;
    double carrier_b_hz;
    float neck_hz;
    float neck_depth;
    double phase_a;
    double phase_b;
    uint32_t generation;
} ORTerrenceNeck;

typedef struct {
    uint32_t accepted_flags;
    uint32_t rejected_flags;
    float audio_nyquist_hz;
    float neck_hz;
} ORTerrenceScreenReport;

static int or_terrence_screen(const ORTerrenceNeck *neck,
                              uint32_t sample_rate,
                              ORTerrenceScreenReport *report) {
    uint32_t accepted = 0u;
    if (neck && (neck->source_category == OR_SOURCE_CATEGORY_C_SOURCE ||
                 neck->source_category == OR_SOURCE_CATEGORY_TERRENCE))
        accepted |= OR_SCREENER_SOURCE_OK;
    if (neck && isfinite(neck->carrier_a_hz) && isfinite(neck->carrier_b_hz) &&
        neck->carrier_a_hz >= 1.0e11 && neck->carrier_a_hz <= 1.0e13 &&
        neck->carrier_b_hz >= 1.0e11 && neck->carrier_b_hz <= 1.0e13)
        accepted |= OR_SCREENER_THz_OK;
    if (neck && isfinite(neck->neck_hz) && neck->neck_hz > 0.0f &&
        sample_rate >= 8000u && sample_rate <= 192000u &&
        neck->neck_hz < (float)sample_rate * 0.45f &&
        isfinite(neck->neck_depth) && neck->neck_depth >= 0.0f &&
        neck->neck_depth <= 1.0f && isfinite(neck->phase_a) &&
        isfinite(neck->phase_b) && neck->phase_a >= 0.0 &&
        neck->phase_a < 1.0 && neck->phase_b >= 0.0 && neck->phase_b < 1.0)
        accepted |= OR_SCREENER_NECK_OK;
    if (neck && neck->generation != UINT32_MAX)
        accepted |= OR_SCREENER_CURRENT_OK;

    if (report) {
        report->accepted_flags = accepted;
        report->rejected_flags =
            (OR_SCREENER_SOURCE_OK | OR_SCREENER_THz_OK |
             OR_SCREENER_NECK_OK | OR_SCREENER_CURRENT_OK) & ~accepted;
        report->audio_nyquist_hz = sample_rate > 0u ? 0.5f * (float)sample_rate : 0.0f;
        report->neck_hz = neck ? neck->neck_hz : 0.0f;
    }
    return accepted == (OR_SCREENER_SOURCE_OK | OR_SCREENER_THz_OK |
                        OR_SCREENER_NECK_OK | OR_SCREENER_CURRENT_OK);
}

static void or_terrence_init(ORTerrenceNeck *neck) {
    memset(neck, 0, sizeof(*neck));
    neck->source_category = OR_SOURCE_CATEGORY_TERRENCE;
    neck->carrier_a_hz = 1.020000000000e12;
    neck->carrier_b_hz = 1.019999999780e12;
    neck->neck_hz = 220.0f;
    neck->neck_depth = 0.18f;
    neck->flags = OR_SCREENER_SOURCE_OK | OR_SCREENER_THz_OK |
                  OR_SCREENER_NECK_OK | OR_SCREENER_CURRENT_OK;
}

static float or_terrence_tick(ORTerrenceNeck *neck, uint32_t sample_rate) {
    ORTerrenceScreenReport report;
    if (!or_terrence_screen(neck, sample_rate, &report)) return 0.0f;
    const double da = fmod(neck->carrier_a_hz, (double)sample_rate) /
                      (double)sample_rate;
    const double db = fmod(neck->carrier_b_hz, (double)sample_rate) /
                      (double)sample_rate;
    neck->phase_a += da;
    neck->phase_b += db;
    neck->phase_a -= floor(neck->phase_a);
    neck->phase_b -= floor(neck->phase_b);
    const double beat_phase = neck->phase_a - neck->phase_b;
    if (neck->generation != UINT32_MAX) neck->generation++;
    return (float)sin(OR_TWO_PI * beat_phase) * neck->neck_depth;
}

/* Double Down: two audio-rate pathways exposed through a Hertz menu. */
enum {
    OR_HERTZ_MENU_UNISON = 0u,
    OR_HERTZ_MENU_INTERVAL,
    OR_HERTZ_MENU_CONTRARY,
    OR_HERTZ_MENU_TERRENCE,
    OR_HERTZ_MENU_COUNT
};

enum {
    OR_CHECKSUM_ALPHA_PROFUSE = 0u,
    OR_CHECKSUM_ALPHA_OBTEASE,
    OR_CHECKSUM_ALPHA_DERELICT,
    OR_CHECKSUM_ALPHA_DIALECT_COUNT
};

typedef struct {
    uint32_t member_id;
    uint32_t approved;
    uint32_t seal;
} ORCrewCord;

typedef struct {
    float hz[2];
    double phase[2];
    float matrix[2][2];
    float depth;
    uint32_t menu;
    uint32_t enabled;
    uint32_t generation;
    uint32_t alpha_dialect;
    uint32_t alpha_checksum;
    ORCrewCord crew[3];
} ORDoubleDownPathway;

const char *or_checksum_alpha_dialect_name(uint32_t dialect) {
    static const char *const name[OR_CHECKSUM_ALPHA_DIALECT_COUNT] = {
        "Profuse", "Obtease", "Derelict"
    };
    return dialect < OR_CHECKSUM_ALPHA_DIALECT_COUNT ? name[dialect] : "Invalid";
}

static uint32_t or_checksum_alpha_compute(const ORDoubleDownPathway *path) {
    if (!path || path->alpha_dialect >= OR_CHECKSUM_ALPHA_DIALECT_COUNT)
        return 0u;
    if (path->alpha_dialect == OR_CHECKSUM_ALPHA_PROFUSE) {
        struct {
            float hz[2];
            float matrix[2][2];
            float depth;
            uint32_t menu;
            uint32_t enabled;
            uint32_t dialect;
        } payload;
        memset(&payload, 0, sizeof(payload));
        memcpy(payload.hz, path->hz, sizeof(payload.hz));
        memcpy(payload.matrix, path->matrix, sizeof(payload.matrix));
        payload.depth = path->depth;
        payload.menu = path->menu;
        payload.enabled = path->enabled;
        payload.dialect = path->alpha_dialect;
        return or_fnv1a32(&payload, sizeof(payload));
    }
    if (path->alpha_dialect == OR_CHECKSUM_ALPHA_OBTEASE) {
        struct {
            float determinant;
            float row_energy[2];
            float frequency_ratio;
            uint32_t menu;
        } payload;
        memset(&payload, 0, sizeof(payload));
        payload.determinant = path->matrix[0][0] * path->matrix[1][1] -
                              path->matrix[0][1] * path->matrix[1][0];
        payload.row_energy[0] = fabsf(path->matrix[0][0]) +
                                fabsf(path->matrix[0][1]);
        payload.row_energy[1] = fabsf(path->matrix[1][0]) +
                                fabsf(path->matrix[1][1]);
        payload.frequency_ratio = path->hz[1] / fmaxf(path->hz[0], 1.0e-9f);
        payload.menu = path->menu;
        return or_fnv1a32(&payload, sizeof(payload));
    }
    struct {
        double phase[2];
        uint32_t generation;
        uint32_t enabled;
    } payload;
    memset(&payload, 0, sizeof(payload));
    memcpy(payload.phase, path->phase, sizeof(payload.phase));
    payload.generation = path->generation;
    payload.enabled = path->enabled;
    return or_fnv1a32(&payload, sizeof(payload));
}

static void or_checksum_alpha_refresh(ORDoubleDownPathway *path) {
    if (path) path->alpha_checksum = or_checksum_alpha_compute(path);
}

static uint32_t or_double_down_seal(const ORDoubleDownPathway *path,
                                    uint32_t member_id) {
    struct {
        float hz[2];
        float matrix[2][2];
        float depth;
        uint32_t menu;
        uint32_t enabled;
        uint32_t alpha_dialect;
        uint32_t member_id;
    } contract;
    memset(&contract, 0, sizeof(contract));
    memcpy(contract.hz, path->hz, sizeof(contract.hz));
    memcpy(contract.matrix, path->matrix, sizeof(contract.matrix));
    contract.depth = path->depth;
    contract.menu = path->menu;
    contract.enabled = path->enabled;
    contract.alpha_dialect = path->alpha_dialect;
    contract.member_id = member_id;
    return or_fnv1a32(&contract, sizeof(contract));
}

static int or_double_down_verify_crew(ORDoubleDownPathway *path,
                                      uint32_t sample_rate) {
    if (!path || sample_rate < 8000u || sample_rate > 192000u) return 0;
    const int range_ok = isfinite(path->hz[0]) && isfinite(path->hz[1]) &&
        path->hz[0] > 0.0f && path->hz[1] > 0.0f &&
        path->hz[0] < (float)sample_rate * 0.45f &&
        path->hz[1] < (float)sample_rate * 0.45f &&
        isfinite(path->depth) && path->depth >= 0.0f && path->depth <= 0.5f;
    int matrix_ok = 1;
    for (uint32_t row = 0; row < 2u; ++row) {
        float row_energy = 0.0f;
        for (uint32_t column = 0; column < 2u; ++column) {
            const float value = path->matrix[row][column];
            if (!isfinite(value) || value < -1.0f || value > 1.0f)
                matrix_ok = 0;
            row_energy += fabsf(value);
        }
        if (row_energy > 1.00001f) matrix_ok = 0;
    }
    const float determinant = path->matrix[0][0] * path->matrix[1][1] -
                              path->matrix[0][1] * path->matrix[1][0];
    if (!isfinite(determinant) || fabsf(determinant) < 0.05f) matrix_ok = 0;

    const int alpha_ok = path->alpha_dialect < OR_CHECKSUM_ALPHA_DIALECT_COUNT &&
                         path->alpha_checksum == or_checksum_alpha_compute(path);
    const int vote[3] = {range_ok, matrix_ok,
                         range_ok && matrix_ok && alpha_ok &&
                         path->menu < OR_HERTZ_MENU_COUNT};
    for (uint32_t member = 0; member < 3u; ++member) {
        path->crew[member].member_id = member + 1u;
        path->crew[member].approved = vote[member] ? 1u : 0u;
        path->crew[member].seal = or_double_down_seal(path, member + 1u);
    }
    return vote[0] && vote[1] && vote[2];
}

int or_double_down_set_checksum_alpha(ORDoubleDownPathway *path,
                                      uint32_t dialect,
                                      uint32_t sample_rate) {
    if (!path || dialect >= OR_CHECKSUM_ALPHA_DIALECT_COUNT) return 0;
    ORDoubleDownPathway candidate = *path;
    candidate.alpha_dialect = dialect;
    or_checksum_alpha_refresh(&candidate);
    if (!or_double_down_verify_crew(&candidate, sample_rate)) return 0;
    *path = candidate;
    return 1;
}

int or_double_down_set_menu(ORDoubleDownPathway *path,
                            uint32_t menu,
                            float anchor_hz,
                            float terrence_neck_hz,
                            uint32_t sample_rate) {
    if (!path || menu >= OR_HERTZ_MENU_COUNT || !isfinite(anchor_hz) ||
        !isfinite(terrence_neck_hz) || anchor_hz <= 0.0f ||
        terrence_neck_hz <= 0.0f || sample_rate < 8000u ||
        sample_rate > 192000u)
        return 0;
    ORDoubleDownPathway candidate = *path;
    candidate.menu = menu;
    switch (menu) {
        case OR_HERTZ_MENU_UNISON:
            candidate.hz[0] = anchor_hz;
            candidate.hz[1] = anchor_hz * 1.005f;
            candidate.matrix[0][0] = 0.70f; candidate.matrix[0][1] = 0.30f;
            candidate.matrix[1][0] = 0.30f; candidate.matrix[1][1] = 0.70f;
            break;
        case OR_HERTZ_MENU_INTERVAL:
            candidate.hz[0] = anchor_hz;
            candidate.hz[1] = anchor_hz * 1.5f;
            candidate.matrix[0][0] = 0.80f; candidate.matrix[0][1] = 0.20f;
            candidate.matrix[1][0] = -0.20f; candidate.matrix[1][1] = 0.80f;
            break;
        case OR_HERTZ_MENU_CONTRARY:
            candidate.hz[0] = anchor_hz * 0.5f;
            candidate.hz[1] = anchor_hz * 2.0f;
            candidate.matrix[0][0] = 0.65f; candidate.matrix[0][1] = -0.35f;
            candidate.matrix[1][0] = 0.35f; candidate.matrix[1][1] = 0.65f;
            break;
        case OR_HERTZ_MENU_TERRENCE:
            candidate.hz[0] = terrence_neck_hz;
            candidate.hz[1] = 0.5f * (anchor_hz + terrence_neck_hz);
            candidate.matrix[0][0] = 0.50f; candidate.matrix[0][1] = 0.50f;
            candidate.matrix[1][0] = -0.50f; candidate.matrix[1][1] = 0.50f;
            break;
        default:
            return 0;
    }
    candidate.enabled = 1u;
    candidate.depth = 0.14f;
    if (candidate.alpha_dialect >= OR_CHECKSUM_ALPHA_DIALECT_COUNT)
        candidate.alpha_dialect = OR_CHECKSUM_ALPHA_PROFUSE;
    or_checksum_alpha_refresh(&candidate);
    if (!or_double_down_verify_crew(&candidate, sample_rate)) return 0;
    *path = candidate;
    return 1;
}

static int or_double_down_validate(const ORDoubleDownPathway *path,
                                   uint32_t sample_rate) {
    return path && path->menu < OR_HERTZ_MENU_COUNT && path->enabled <= 1u &&
           isfinite(path->hz[0]) && isfinite(path->hz[1]) &&
           path->hz[0] > 0.0f && path->hz[1] > 0.0f &&
           path->hz[0] < (float)sample_rate * 0.45f &&
           path->hz[1] < (float)sample_rate * 0.45f &&
           isfinite(path->phase[0]) && isfinite(path->phase[1]) &&
           path->phase[0] >= 0.0 && path->phase[0] < 1.0 &&
           path->phase[1] >= 0.0 && path->phase[1] < 1.0 &&
           isfinite(path->depth) && path->depth >= 0.0f &&
           path->depth <= 0.5f && path->generation != UINT32_MAX &&
           path->alpha_dialect < OR_CHECKSUM_ALPHA_DIALECT_COUNT &&
           path->alpha_checksum == or_checksum_alpha_compute(path) &&
           path->crew[0].approved == 1u && path->crew[1].approved == 1u &&
           path->crew[2].approved == 1u &&
           path->crew[0].seal == or_double_down_seal(path, 1u) &&
           path->crew[1].seal == or_double_down_seal(path, 2u) &&
           path->crew[2].seal == or_double_down_seal(path, 3u);
}

static float or_double_down_tick(ORDoubleDownPathway *path,
                                 uint32_t sample_rate) {
    if (!or_double_down_validate(path, sample_rate) || !path->enabled)
        return 0.0f;
    float lane_signal[2];
    for (uint32_t lane = 0; lane < 2u; ++lane) {
        path->phase[lane] += (double)path->hz[lane] / (double)sample_rate;
        path->phase[lane] -= floor(path->phase[lane]);
        lane_signal[lane] = (float)sin(OR_TWO_PI * path->phase[lane]);
    }
    const float routed_a = path->matrix[0][0] * lane_signal[0] +
                           path->matrix[0][1] * lane_signal[1];
    const float routed_b = path->matrix[1][0] * lane_signal[0] +
                           path->matrix[1][1] * lane_signal[1];
    const float signal = 0.56f * routed_a + 0.44f * routed_b;
    path->generation++;
    or_checksum_alpha_refresh(path);
    return signal * path->depth;
}

void or_wt56_set_sample_rate(ORWaveTransformCtl *ctl, uint32_t sample_rate);
float or_wt56_tick(ORWaveTransformer56K *wt,
                   ORWaveTransformCtl *ctl,
                   float input,
                   float reasoning);

typedef struct {
    float delay[OR_DOUBLE_DIFFUSER_SLOTS]
               [OR_DOUBLE_DIFFUSER_CHANNELS][OR_DOUBLE_DIFFUSER_RING];
    uint32_t head[OR_DOUBLE_DIFFUSER_SLOTS];
    uint32_t delay_length[OR_DOUBLE_DIFFUSER_SLOTS]
                         [OR_DOUBLE_DIFFUSER_CHANNELS];
    float coefficient;
    float wet;
    uint32_t generation;
} ORDoubleDiffuser;

_Static_assert(sizeof(ORDoubleDiffuser) == 1060,
               "double-slot diffuser layout changed");

static void or_double_diffuser_init(ORDoubleDiffuser *df) {
    memset(df, 0, sizeof(*df));
    df->delay_length[0][0] = 43u;
    df->delay_length[0][1] = 47u;
    df->delay_length[1][0] = 59u;
    df->delay_length[1][1] = 61u;
    df->coefficient = 0.58f;
    df->wet = 0.72f;
}

static int or_double_diffuser_validate(const ORDoubleDiffuser *df) {
    if (!df || !isfinite(df->coefficient) || df->coefficient < 0.0f ||
        df->coefficient > 0.75f || !isfinite(df->wet) ||
        df->wet < 0.0f || df->wet > 1.0f)
        return 0;
    for (uint32_t slot = 0; slot < OR_DOUBLE_DIFFUSER_SLOTS; ++slot) {
        if (df->head[slot] >= OR_DOUBLE_DIFFUSER_RING) return 0;
        for (uint32_t channel = 0; channel < OR_DOUBLE_DIFFUSER_CHANNELS; ++channel) {
            if (df->delay_length[slot][channel] == 0u ||
                df->delay_length[slot][channel] >= OR_DOUBLE_DIFFUSER_RING)
                return 0;
            for (uint32_t i = 0; i < OR_DOUBLE_DIFFUSER_RING; ++i) {
                const float sample = df->delay[slot][channel][i];
                if (!isfinite(sample) || sample < -2.0f || sample > 2.0f)
                    return 0;
            }
        }
    }
    return 1;
}

static void or_double_diffuser_tick(ORDoubleDiffuser *df,
                                    float in_l,
                                    float in_r,
                                    float *out_l,
                                    float *out_r) {
    float channel_input[OR_DOUBLE_DIFFUSER_CHANNELS] = {in_l, in_r};
    float channel_output[OR_DOUBLE_DIFFUSER_CHANNELS];
    for (uint32_t channel = 0; channel < OR_DOUBLE_DIFFUSER_CHANNELS; ++channel) {
        float x = channel_input[channel];
        for (uint32_t slot = 0; slot < OR_DOUBLE_DIFFUSER_SLOTS; ++slot) {
            const uint32_t read =
                (df->head[slot] + OR_DOUBLE_DIFFUSER_RING -
                 df->delay_length[slot][channel]) % OR_DOUBLE_DIFFUSER_RING;
            const float delayed = df->delay[slot][channel][read];
            const float y = delayed - df->coefficient * x;
            df->delay[slot][channel][df->head[slot]] = x + df->coefficient * y;
            x = y;
        }
        channel_output[channel] = x;
    }
    for (uint32_t slot = 0; slot < OR_DOUBLE_DIFFUSER_SLOTS; ++slot)
        df->head[slot] = (df->head[slot] + 1u) % OR_DOUBLE_DIFFUSER_RING;
    *out_l = or_clampf(in_l * (1.0f - df->wet) +
                       channel_output[0] * df->wet, -1.0f, 1.0f);
    *out_r = or_clampf(in_r * (1.0f - df->wet) +
                       channel_output[1] * df->wet, -1.0f, 1.0f);
    if (df->generation != UINT32_MAX) df->generation++;
}

/* A checkpoint records the entire small synthesis state plus its source epoch. */
typedef struct {
    uint32_t sequence;
    uint32_t phase[OR_DOUBLEX_VOICES];
    uint32_t sample_rate;
    uint64_t rendered_frames;
    uint32_t source_checksum;
    uint32_t source_generation;
    float fundamental_hz;
    float master_gain;
    float envelope;
    ORWaveTransformCtl transformer_ctl;
    int16_t transformer_history[OR_WT56_HISTORY];
    ORDoubleDiffuser diffuser;
    ORTerrenceNeck terrence;
    ORDoubleDownPathway pathway;
    uint32_t checksum;
} ORDoubleXCheckpoint;

typedef struct {
    uint32_t sample_rate;
    uint32_t checkpoint_head;
    uint32_t checkpoint_count;
    uint32_t checkpoint_sequence;
    uint32_t phase[OR_DOUBLEX_VOICES];
    uint64_t rendered_frames;
    uint32_t frames_since_checkpoint;
    float fundamental_hz;
    float master_gain;
    float envelope;
    ORDoubleDiffuser diffuser;
    ORTerrenceNeck terrence;
    ORDoubleDownPathway pathway;
    ORDoubleXCheckpoint checkpoint[OR_DOUBLEX_CHECKPOINT_CREW];
} ORDoubleXSynth;

int or_doublex_validate(const ORDoubleXSynth *sx);

const char *or_doublex_hertz_menu_name(uint32_t menu) {
    static const char *const name[OR_HERTZ_MENU_COUNT] = {
        "Unison Double", "Interval Double", "Contrary Double", "Terrence Neck"
    };
    return menu < OR_HERTZ_MENU_COUNT ? name[menu] : "Invalid";
}

int or_doublex_select_hertz_menu(ORDoubleXSynth *sx, uint32_t menu) {
    if (!sx || !or_doublex_validate(sx)) return 0;
    return or_double_down_set_menu(&sx->pathway, menu,
                                   sx->fundamental_hz,
                                   sx->terrence.neck_hz,
                                   sx->sample_rate);
}

int or_doublex_select_checksum_alpha(ORDoubleXSynth *sx, uint32_t dialect) {
    if (!sx || !or_doublex_validate(sx)) return 0;
    return or_double_down_set_checksum_alpha(&sx->pathway, dialect,
                                              sx->sample_rate);
}

int or_doublex_verify_matrix_crew(ORDoubleXSynth *sx) {
    if (!sx) return 0;
    return or_double_down_verify_crew(&sx->pathway, sx->sample_rate) &&
           or_double_down_validate(&sx->pathway, sx->sample_rate);
}

static uint32_t or_doublex_checkpoint_checksum(const ORDoubleXCheckpoint *src) {
    ORDoubleXCheckpoint copy = *src;
    copy.checksum = 0u;
    return or_fnv1a32(&copy, sizeof(copy));
}

static int or_doublex_checkpoint_valid(const ORDoubleXCheckpoint *cp) {
    return cp && cp->sequence != 0u &&
           cp->sample_rate >= 8000u && cp->sample_rate <= 192000u &&
           isfinite(cp->fundamental_hz) && cp->fundamental_hz > 0.0f &&
           cp->fundamental_hz <= (float)cp->sample_rate * 0.1125f &&
           isfinite(cp->master_gain) && cp->master_gain >= 0.0f &&
           cp->master_gain <= 1.0f && isfinite(cp->envelope) &&
           cp->envelope >= 0.0f && cp->envelope <= 1.0f &&
           or_double_diffuser_validate(&cp->diffuser) &&
           or_terrence_screen(&cp->terrence, cp->sample_rate, NULL) &&
           or_double_down_validate(&cp->pathway, cp->sample_rate) &&
           or_wt56_validate_ctl(&cp->transformer_ctl) &&
           cp->checksum == or_doublex_checkpoint_checksum(cp);
}

int or_doublex_validate(const ORDoubleXSynth *sx) {
    if (!sx || sx->sample_rate < 8000u || sx->sample_rate > 192000u ||
        sx->checkpoint_head >= OR_DOUBLEX_CHECKPOINT_CREW ||
        sx->checkpoint_count > OR_DOUBLEX_CHECKPOINT_CREW ||
        !isfinite(sx->fundamental_hz) || sx->fundamental_hz <= 0.0f ||
        sx->fundamental_hz > (float)sx->sample_rate * 0.1125f ||
        !isfinite(sx->master_gain) || sx->master_gain < 0.0f ||
        sx->master_gain > 1.0f || !isfinite(sx->envelope) ||
        sx->envelope < 0.0f || sx->envelope > 1.0f)
        return 0;
    return or_double_diffuser_validate(&sx->diffuser) &&
           or_terrence_screen(&sx->terrence, sx->sample_rate, NULL) &&
           or_double_down_validate(&sx->pathway, sx->sample_rate);
}

/* Two paired oscillator layers: a harmonic pair and a color pair. */
int or_doublex_init(ORDoubleXSynth *sx,
                    uint32_t sample_rate,
                    float fundamental_hz) {
    if (!sx || sample_rate < 8000u || sample_rate > 192000u ||
        !isfinite(fundamental_hz) || fundamental_hz <= 0.0f)
        return 0;
    memset(sx, 0, sizeof(*sx));
    sx->sample_rate = sample_rate;
    sx->fundamental_hz = or_clampf(fundamental_hz, 1.0f,
                                    (float)sample_rate * 0.1125f);
    sx->master_gain = 0.72f;
    or_double_diffuser_init(&sx->diffuser);
    or_terrence_init(&sx->terrence);
    sx->terrence.neck_hz = sx->fundamental_hz;
    if (!or_double_down_set_menu(&sx->pathway, OR_HERTZ_MENU_UNISON,
                                 sx->fundamental_hz, sx->terrence.neck_hz,
                                 sx->sample_rate)) return 0;
    return 1;
}

static void or_doublex_checkpoint(ORDoubleXSynth *sx,
                                 ORWaveTransformer56K *wt,
                                 ORWaveTransformCtl *wtctl,
                                 const ORCell *cell) {
    ORDoubleXCheckpoint *cp = &sx->checkpoint[sx->checkpoint_head];
    memset(cp, 0, sizeof(*cp));
    cp->sequence = sx->checkpoint_sequence == UINT32_MAX
                 ? UINT32_MAX : sx->checkpoint_sequence + 1u;
    if (cp->sequence == 0u) cp->sequence = 1u;
    memcpy(cp->phase, sx->phase, sizeof(cp->phase));
    cp->sample_rate = sx->sample_rate;
    cp->rendered_frames = sx->rendered_frames;
    cp->source_checksum = cell->guard.checksum;
    cp->source_generation = cell->guard.generation;
    cp->fundamental_hz = sx->fundamental_hz;
    cp->master_gain = sx->master_gain;
    cp->envelope = sx->envelope;
    cp->transformer_ctl = *wtctl;
    memcpy(cp->transformer_history, wt->history_q15,
           sizeof(cp->transformer_history));
    cp->diffuser = sx->diffuser;
    cp->terrence = sx->terrence;
    cp->pathway = sx->pathway;
    cp->checksum = or_doublex_checkpoint_checksum(cp);

    sx->checkpoint_sequence = cp->sequence;
    sx->checkpoint_head = (sx->checkpoint_head + 1u) % OR_DOUBLEX_CHECKPOINT_CREW;
    if (sx->checkpoint_count < OR_DOUBLEX_CHECKPOINT_CREW)
        sx->checkpoint_count++;
    sx->frames_since_checkpoint = 0u;
}

/* Restore newest valid crew checkpoint; report whether its reasoner epoch matches. */
int or_doublex_restore_latest(ORDoubleXSynth *sx,
                             ORWaveTransformer56K *wt,
                             ORWaveTransformCtl *wtctl,
                             const ORCell *cell,
                             int *source_epoch_matches) {
    if (!or_doublex_validate(sx) || !wt || !or_wt56_validate_ctl(wtctl) ||
        !cell || !or_validate(cell) ||
        sx->checkpoint_count == 0u)
        return 0;

    for (uint32_t offset = 0; offset < OR_DOUBLEX_CHECKPOINT_CREW; ++offset) {
        const uint32_t index =
            (sx->checkpoint_head + OR_DOUBLEX_CHECKPOINT_CREW - 1u - offset) %
            OR_DOUBLEX_CHECKPOINT_CREW;
        const ORDoubleXCheckpoint *cp = &sx->checkpoint[index];
        if (!or_doublex_checkpoint_valid(cp)) continue;
        if (cp->sample_rate != cp->transformer_ctl.sample_rate) continue;

        memcpy(sx->phase, cp->phase, sizeof(sx->phase));
        sx->sample_rate = cp->sample_rate;
        sx->rendered_frames = cp->rendered_frames;
        sx->fundamental_hz = cp->fundamental_hz;
        sx->master_gain = cp->master_gain;
        sx->envelope = cp->envelope;
        *wtctl = cp->transformer_ctl;
        memcpy(wt->history_q15, cp->transformer_history,
               sizeof(cp->transformer_history));
        sx->diffuser = cp->diffuser;
        sx->terrence = cp->terrence;
        sx->pathway = cp->pathway;
        sx->frames_since_checkpoint = 0u;
        sx->checkpoint_sequence = cp->sequence;
        if (source_epoch_matches)
            *source_epoch_matches =
                cp->source_checksum == cell->guard.checksum &&
                cp->source_generation == cell->guard.generation;
        return 1;
    }
    return 0;
}

/*
 * Render a stereo block. The reasoner cell must remain unchanged during this
 * call. Output arrays are written only after all inputs and state validate.
 * Checkpoints are made automatically every 256 output frames, with four slots
 * retained as a rolling recovery crew.
 */
int or_doublex_render_block(ORDoubleXSynth *sx,
                            ORWaveTransformer56K *wt,
                            ORWaveTransformCtl *wtctl,
                            const ORCell *cell,
                            float *left,
                            float *right,
                            size_t frame_count) {
    static const float ratio[OR_DOUBLEX_VOICES] = {1.0f, 1.5f, 1.25f, 2.0f};
    static const float level[OR_DOUBLEX_VOICES] = {0.40f, 0.28f, 0.20f, 0.12f};
    static const float pan[OR_DOUBLEX_VOICES] = {-0.65f, -0.18f, 0.18f, 0.65f};
    if (!sx || !wt || !or_doublex_validate(sx) ||
        !or_wt56_validate_ctl(wtctl) || !cell ||
        !left || !right || frame_count == 0u || !or_validate(cell))
        return 0;

    or_wt56_set_sample_rate(wtctl, sx->sample_rate);
    if (!or_wt56_validate_ctl(wtctl)) return 0;

    const uint32_t dominant = or_dominant_node(cell);
    const ORNode *node = &cell->guard.node[dominant];
    const float support = or_clampf(node->amplitude * node->confidence,
                                    0.0f, 1.0f);
    const float attack = 1.0f - expf(-1.0f / (0.012f * (float)sx->sample_rate));
    const float release = 1.0f - expf(-1.0f / (0.090f * (float)sx->sample_rate));
    const double phase_scale = 4294967296.0 / (double)sx->sample_rate;

    for (size_t frame = 0; frame < frame_count; ++frame) {
        const float coefficient = support > sx->envelope ? attack : release;
        sx->envelope += coefficient * (support - sx->envelope);

        const float source = or_wave_sample(cell, sx->phase[0]);
        const float transformed = or_wt56_tick(wt, wtctl, source,
                                                2.0f * support - 1.0f);
        const float neck = or_terrence_tick(&sx->terrence, sx->sample_rate);
        const float pathway = or_double_down_tick(&sx->pathway, sx->sample_rate);
        float mix_l = 0.0f;
        float mix_r = 0.0f;
        for (uint32_t voice = 0; voice < OR_DOUBLEX_VOICES; ++voice) {
            const double hz = (double)sx->fundamental_hz * ratio[voice];
            const uint32_t step = (uint32_t)(hz * phase_scale);
            const float phase = (float)((double)sx->phase[voice] /
                                        4294967296.0);
            const float oscillator = sinf(OR_TWO_PI * phase);
            const float signal = 0.64f * oscillator + 0.24f * transformed +
                                 0.06f * neck + 0.06f * pathway;
            const float voice_sample = signal * level[voice] * sx->envelope;
            const float left_pan = sqrtf(0.5f * (1.0f - pan[voice]));
            const float right_pan = sqrtf(0.5f * (1.0f + pan[voice]));
            mix_l += voice_sample * left_pan;
            mix_r += voice_sample * right_pan;
            sx->phase[voice] += step;
        }

        const float gained_l = or_clampf(mix_l * sx->master_gain, -1.0f, 1.0f);
        const float gained_r = or_clampf(mix_r * sx->master_gain, -1.0f, 1.0f);
        or_double_diffuser_tick(&sx->diffuser, gained_l, gained_r,
                                &left[frame], &right[frame]);
        sx->rendered_frames++;
        sx->frames_since_checkpoint++;
        if (sx->frames_since_checkpoint >= OR_DOUBLEX_CHECKPOINT_INTERVAL)
            or_doublex_checkpoint(sx, wt, wtctl, cell);
    }
    return 1;
}

/* Render from two independently verified reasoner slots through one fused view. */
int or_doublex_render_multi_reasoner(ORDoubleXSynth *sx,
                                     ORWaveTransformer56K *wt,
                                     ORWaveTransformCtl *wtctl,
                                     ORMultiReasonerSet *set,
                                     float *left,
                                     float *right,
                                     size_t frame_count) {
    ORCell fused;
    if (!sx || !set || set->sample_rate != sx->sample_rate ||
        !or_multi_reasoner_fuse(set, &fused)) return 0;
    if (!or_doublex_render_block(sx, wt, wtctl, &fused,
                                 left, right, frame_count)) return 0;
    or_wavegen_render(&set->wavegen, left, right, frame_count);
    or_double_slit_process(&set->slit, set->sample_rate,
                           left, right, frame_count);
    return 1;
}

/* Explicit connector: the modulation stack is owned outside the reasoner set. */
int or_doublex_render_wave_modulated(ORDoubleXSynth *sx,
                                     ORWaveTransformer56K *wt,
                                     ORWaveTransformCtl *wtctl,
                                     ORMultiReasonerSet *set,
                                     ORWaveModulationStack *stack,
                                     float *left, float *right,
                                     size_t frame_count) {
    if (!sx || !set || !stack || stack->sample_rate != sx->sample_rate ||
        !or_wave_mod_stack_validate(stack) ||
        !or_multi_reasoner_validate(set)) return 0;
    if (!or_doublex_render_multi_reasoner(sx, wt, wtctl, set,
                                          left, right, frame_count)) return 0;
    return or_wave_mod_stack_process(stack, left, right, frame_count);
}

/*
 * Full category connector for the two slot machines.  Rendering remains
 * transactional: invalid slots, category configuration, or buffers fail
 * before the category changes any samples.
 */
int or_doublex_render_mod_entropy(ORDoubleXSynth *sx,
                                  ORWaveTransformer56K *wt,
                                  ORWaveTransformCtl *wtctl,
                                  ORMultiReasonerSet *set,
                                  ORModEntropyCategoryGroup *group,
                                  float *left,
                                  float *right,
                                  size_t frame_count) {
    if (!sx || !set || !group || set->sample_rate != sx->sample_rate ||
        group->sample_rate != sx->sample_rate ||
        !or_multi_reasoner_validate(set) ||
        !or_mod_entropy_group_validate(group) ||
        (frame_count && (!left || !right))) return 0;
    const float reasoner_entropy = or_multi_reasoner_entropy(set);
    if (reasoner_entropy < 0.0f) return 0;
    if (!or_doublex_render_multi_reasoner(sx, wt, wtctl, set,
                                          left, right, frame_count)) return 0;
    return or_mod_entropy_group_process(group, reasoner_entropy,
                                        left, right, frame_count);
}

/* ------------------------------------------------------------------------- */
/* 56-Kibit Wave Transformer                                                */
/* ------------------------------------------------------------------------- */

/*
 * Exact 56-Kibit transform memory:
 *   map_q15     : 2048 * 16 = 32768 bits
 *   history_q15 : 1024 * 16 = 16384 bits
 *   mod_q15     :  512 * 16 =  8192 bits
 *                              ---------
 *                               57344 bits = 56 Kibit
 *
 * Control metadata lives outside this cell so the memory contract stays exact.
 */
static float or_wt56_q15f(int16_t v) {
    return (float)v / 32768.0f;
}

static int16_t or_wt56_fq15(float x) {
    x = or_clampf(x, -1.0f, 0.999969482421875f);
    return (int16_t)lrintf(x * 32768.0f);
}

static float or_wt56_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static float or_wt56_lookup_q15(const int16_t *table,
                                uint32_t count,
                                uint32_t phase) {
    const uint64_t scaled = (uint64_t)phase * (uint64_t)count;
    uint32_t i0 = (uint32_t)(scaled >> 32);
    uint32_t frac = (uint32_t)scaled;
    uint32_t i1 = i0 + 1u;
    if (i1 >= count) i1 = 0u;
    const float t = (float)((double)frac / 4294967296.0);
    return or_wt56_lerp(or_wt56_q15f(table[i0]),
                        or_wt56_q15f(table[i1]), t);
}

void or_wt56_init(ORWaveTransformer56K *wt,
                  ORWaveTransformCtl *ctl,
                  float carrier_hz) {
    if (!wt || !ctl) return;
    memset(wt, 0, sizeof(*wt));
    memset(ctl, 0, sizeof(*ctl));

    /* Identity-like map with a gentle third-harmonic bend. */
    for (uint32_t i = 0; i < OR_WT56_MAP_SAMPLES; ++i) {
        const float p = OR_TWO_PI * (float)i / (float)OR_WT56_MAP_SAMPLES;
        const float v = 0.86f * sinf(p) + 0.14f * sinf(3.0f * p);
        wt->map_q15[i] = or_wt56_fq15(v);
    }

    /* Modulation bank: quadrature-rich bounded shape. */
    for (uint32_t i = 0; i < OR_WT56_MOD_SAMPLES; ++i) {
        const float p = OR_TWO_PI * (float)i / (float)OR_WT56_MOD_SAMPLES;
        const float v = 0.70f * cosf(p) + 0.20f * sinf(2.0f * p)
                                      + 0.10f * cosf(5.0f * p);
        wt->mod_q15[i] = or_wt56_fq15(v);
    }

    if (!isfinite(carrier_hz)) carrier_hz = 0.0f;
    carrier_hz = or_clampf(carrier_hz, 0.0f, OR_WT56_MAX_HZ);
    ctl->sample_rate = (uint32_t)OR_CONV_RATE_HZ;
    ctl->carrier_hz = carrier_hz;
    ctl->phase_step = (uint32_t)((double)carrier_hz /
                      (double)OR_CONV_RATE_HZ * 4294967296.0);
    ctl->drive = 0.75f;
    ctl->morph = 0.35f;
    ctl->feedback = 0.12f;
    ctl->phase_warp = 0.20f;
    ctl->slew = 0.10f;
    ctl->output_gain = 0.85f;
    ctl->energy_limit = 0.85f;
    ctl->generation = 1u;
}

void or_wt56_set_rate(ORWaveTransformCtl *ctl, float carrier_hz) {
    if (!ctl || !isfinite(carrier_hz) || ctl->sample_rate == 0u) return;
    carrier_hz = or_clampf(carrier_hz, 0.0f,
                           (float)ctl->sample_rate * 0.49f);
    ctl->carrier_hz = carrier_hz;
    ctl->phase_step = (uint32_t)((double)carrier_hz /
                      (double)ctl->sample_rate * 4294967296.0);
}

void or_wt56_set_sample_rate(ORWaveTransformCtl *ctl, uint32_t sample_rate) {
    if (!ctl || sample_rate == 0u || sample_rate > 192000u ||
        !isfinite(ctl->carrier_hz)) return;
    ctl->sample_rate = sample_rate;
    ctl->carrier_hz = or_clampf(ctl->carrier_hz, 0.0f,
                                (float)sample_rate * 0.49f);
    ctl->phase_step = (uint32_t)((double)ctl->carrier_hz /
                      (double)sample_rate * 4294967296.0);
}

/*
 * One guarded transform tick.
 *
 * input       : upstream normalized sample [-1,1]
 * reasoning   : bounded steering term, typically dominant-node amplitude/bias
 * returns     : transformed normalized sample [-1,1]
 *
 * This stage never creates evidence/confidence; it only reshapes the wave path.
 */
float or_wt56_tick(ORWaveTransformer56K *wt,
                   ORWaveTransformCtl *ctl,
                   float input,
                   float reasoning) {
    if (!wt || !or_wt56_validate_ctl(ctl) ||
        !isfinite(input) || !isfinite(reasoning)) return 0.0f;

    input = or_clampf(input, -1.0f, 1.0f);
    reasoning = or_clampf(reasoning, -1.0f, 1.0f);

    const float map = or_wt56_lookup_q15(
        wt->map_q15, OR_WT56_MAP_SAMPLES, ctl->phase);
    const float mod = or_wt56_lookup_q15(
        wt->mod_q15, OR_WT56_MOD_SAMPLES,
        ctl->phase + (uint32_t)(ctl->phase_warp * 1073741824.0f));

    /* Read delayed history for a small, bounded temporal transform. */
    const uint32_t delay = 31u;
    const uint32_t read_index =
        (ctl->history_head + OR_WT56_HISTORY - delay) % OR_WT56_HISTORY;
    const float delayed = or_wt56_q15f(wt->history_q15[read_index]);

    const float shaped =
        input * (1.0f - ctl->morph)
        + map * ctl->morph
        + mod * ctl->drive * (0.18f + 0.22f * fabsf(reasoning))
        + delayed * ctl->feedback;

    /* Reasoning may steer shape slightly, but cannot overwhelm the signal. */
    float target = shaped + 0.10f * reasoning * mod;
    target = or_clampf(target, -1.0f, 1.0f);

    /* Output slew guard. */
    float delta = target - ctl->previous_output;
    delta = or_clampf(delta, -ctl->slew, ctl->slew);
    float out = ctl->previous_output + delta;

    /* Measure the emitted signal, including gain and any energy attenuation. */
    out = or_clampf(out * ctl->output_gain, -1.0f, 1.0f);
    const float projected_energy = 0.995f * ctl->energy + 0.005f * (out * out);
    if (projected_energy > ctl->energy_limit) {
        const float scale = sqrtf(ctl->energy_limit / projected_energy);
        out *= or_clampf(scale, 0.25f, 1.0f);
    }
    ctl->energy = 0.995f * ctl->energy + 0.005f * (out * out);
    ctl->previous_output = out;

    wt->history_q15[ctl->history_head] = or_wt56_fq15(out);
    ctl->history_head = (ctl->history_head + 1u) % OR_WT56_HISTORY;
    ctl->phase += ctl->phase_step;
    if (ctl->generation != UINT32_MAX) ctl->generation++;

    return out;
}

/* Transform the 4096-point carrier of the 128-Kibit reasoning cell in-place. */
void or_wt56_transform_carrier(ORWaveTransformer56K *wt,
                               ORWaveTransformCtl *ctl,
                               ORCell *cell) {
    if (!wt || !cell || !or_wt56_validate_ctl(ctl)) return;

    const uint32_t dominant = or_dominant_node(cell);
    const float reason = or_clampf(
        cell->guard.node[dominant].amplitude *
        (0.5f + 0.5f * cell->guard.node[dominant].confidence),
        0.0f, 1.0f);

    for (uint32_t i = 0; i < OR_WAVE_SAMPLES; ++i) {
        const float x = (float)cell->wave.sample[i] / 32768.0f;
        const float y = or_wt56_tick(wt, ctl, x, reason);
        cell->wave.sample[i] = or_wt56_fq15(y);
    }

    /* The carrier changed; refresh the fixed cell integrity checksum. */
    or_refresh_checksum(cell);
}

#ifdef OSC_REASONING_WAVE56_DEMO
int main(void) {
    ORCell cell;
    ORConvolutor1020 conv;
    ORSelfMover1020 mover;
    ORWaveTransformer56K wt;
    ORWaveTransformCtl wtctl;

    or_init(&cell);
    or_conv1020_init(&conv, 120.0f);
    or_self_move_init(&mover, &cell);
    or_wt56_init(&wt, &wtctl, 48.0f);

    for (uint32_t i = 0; i < 6u; ++i) {
        or_set_node(&cell, i, 0.24f + 0.01f * (float)i, 7.0f);
        or_inject_evidence(&cell, i, 0.58f - 0.02f * (float)i);
        for (uint32_t j = 0; j < 6u; ++j)
            if (i != j) or_set_coupling(&cell, i, j, 0.56f);
    }

    or_refresh_checksum(&cell);
    or_conv1020_prime(&conv, &cell);

    /* One second of self-moving reasoning at 1020 Hz. */
    for (uint32_t k = 0; k < 1020u; ++k)
        or_self_move_tick(&mover, &conv, &cell);

    /* Finish by reshaping the carrier through the fixed 56-Kibit transformer. */
    or_wt56_transform_carrier(&wt, &wtctl, &cell);

    const uint32_t d = or_dominant_node(&cell);
    printf("reasoning_cell=%zu bytes (%u Kibit)\n",
           sizeof(cell), OR_CELL_BITS / 1024u);
    printf("wave_transformer=%zu bytes (%u Kibit)\n",
           sizeof(wt), OR_WT56_BITS / 1024u);
    printf("rate=%.1f Hz ticks=%u dominant=%u amp=%.3f conf=%.3f\n",
           OR_CONV_RATE_HZ, conv.tick, d,
           cell.guard.node[d].amplitude,
           cell.guard.node[d].confidence);
    printf("transform_generation=%u energy=%.6f first_samples=%d,%d,%d,%d\n",
           wtctl.generation, wtctl.energy,
           cell.wave.sample[0], cell.wave.sample[1],
           cell.wave.sample[2], cell.wave.sample[3]);
    return 0;
}
#endif

#ifdef OSC_REASONING_DOUBLEX_DEMO
int main(void) {
    ORCell cell;
    ORWaveTransformer56K wt;
    ORWaveTransformCtl wtctl;
    ORDoubleXSynth synth;
    float left[512];
    float right[512];
    float peak = 0.0f;

    or_init(&cell);
    or_set_node(&cell, 0u, 0.82f, 7.0f);
    or_inject_evidence(&cell, 0u, 0.78f);
    or_refresh_checksum(&cell);
    for (uint32_t i = 0; i < 20u; ++i)
        or_step(&cell, OR_CONV_DT);
    or_wt56_init(&wt, &wtctl, 48.0f);
    if (!or_doublex_init(&synth, 48000u, 220.0f)) return 1;
    if (!or_doublex_render_block(&synth, &wt, &wtctl, &cell,
                                 left, right, 512u)) return 2;

    for (uint32_t i = 0; i < 512u; ++i) {
        peak = fmaxf(peak, fabsf(left[i]));
        peak = fmaxf(peak, fabsf(right[i]));
    }
    printf("DoubleX: rate=%u Hz frames=%llu voices=%u checkpoints=%u peak=%.4f\n",
           synth.sample_rate, (unsigned long long)synth.rendered_frames,
           OR_DOUBLEX_VOICES, synth.checkpoint_count, peak);
    return 0;
}
#endif

#ifdef OSC_REASONING_MOD_ENTROPY_DEMO
int main(void) {
    ORCell cell_a;
    ORCell cell_b;
    ORMultiReasonerSet reasoners;
    ORModEntropyCategoryGroup category;
    ORModEntropyCategoryGroup decoded_category;
    ORDoubleEntropyDecompressor decoder;
    ORWaveTransformer56K transformer;
    ORWaveTransformCtl transformer_ctl;
    ORDoubleXSynth synth;
    float left[1024];
    float right[1024];
    const int8_t left_codes[16] =
        {12, 8, 4, 0, -4, -8, -12, -8, -4, 0, 4, 8, 12, 8, 4, 0};
    const int8_t right_codes[16] =
        {0, 4, 8, 12, 8, 4, 0, -4, -8, -12, -8, -4, 0, 4, 8, 12};
    float decoded_left[16];
    float decoded_right[16];
    float peak = 0.0f;

    or_init(&cell_a);
    or_init(&cell_b);
    or_set_node(&cell_a, 0u, 0.84f, 7.0f);
    or_set_node(&cell_b, 0u, 0.28f, 5.0f);
    or_set_node(&cell_b, 1u, 0.76f, 9.0f);
    or_inject_evidence(&cell_a, 0u, 0.74f);
    or_inject_evidence(&cell_b, 1u, 0.68f);
    or_refresh_checksum(&cell_a);
    or_refresh_checksum(&cell_b);

    or_multi_reasoner_init(&reasoners);
    if (!or_multi_reasoner_bind(&reasoners, 0u, &cell_a, 0.55f) ||
        !or_multi_reasoner_bind(&reasoners, 1u, &cell_b, 0.45f)) return 1;
    if (!or_mod_entropy_group_init(&category, 48000u) ||
        !or_mod_entropy_group_set_mode(
            &category, OR_MOD_ENTROPY_CONJUGATION_ENTROPIES,
            0.34f, 0.72f, 0.58f)) return 2;
    or_wt56_init(&transformer, &transformer_ctl, 48.0f);
    or_wt56_set_sample_rate(&transformer_ctl, 48000u);
    if (!or_doublex_init(&synth, 48000u, 220.0f)) return 3;
    if (!or_doublex_render_mod_entropy(
            &synth, &transformer, &transformer_ctl, &reasoners, &category,
            left, right, 1024u)) return 4;

    for (uint32_t i = 0; i < 1024u; ++i) {
        peak = fmaxf(peak, fabsf(left[i]));
        peak = fmaxf(peak, fabsf(right[i]));
    }
    if (!or_mod_entropy_group_init(&decoded_category, 48000u) ||
        !or_mod_entropy_group_set_mode(
            &decoded_category, OR_MOD_ENTROPY_CONJUGATION_ENTROPIES,
            0.34f, 0.72f, 0.58f) ||
        !or_double_entropy_decompressor_init(&decoder, 0.0025f) ||
        !or_mod_entropy_double_decompress(
            &decoded_category, &decoder, category.entropy.reasoner_entropy,
            left_codes, right_codes, decoded_left, decoded_right, 16u)) return 5;
    ORDoubleDecompressorGroup decompressor_group;
    if (!or_double_decompressor_group_init(&decompressor_group, 48000u) ||
        !or_multi_reasoner_double_decompress(
            &reasoners, &decompressor_group,
            left_codes, right_codes, decoded_left, decoded_right, 16u)) return 6;
    ORDoubleDecompressorWaveDuplexorGroup duplexor_group;
    if (!or_double_decompressor_wave_duplexor_group_init(
            &duplexor_group, 48000u) ||
        !or_double_decompressor_wave_duplexor_group_set_editor(
            &duplexor_group, 1.0f, 1.0f, 0.0f, 0.0f,
            0.30f, 0.40f, 1.0f) ||
        !or_multi_reasoner_wave_duplexor_edit(
            &reasoners, &duplexor_group, left_codes, right_codes,
            decoded_left, decoded_right, 16u)) return 7;
    ORJuplexRegime juplex;
    if (!or_juplex_init(&juplex, 48000u) ||
        or_juplex_set_mode(&juplex, OR_JUPLEX_PRIOR, 1.0f, 4.0f) ||
        !or_juplex_edit_current(&juplex, 0.95f, 1.10f,
                                 0.0f, 0.0f, 0.60f, 0.50f, 1.0f) ||
        !or_juplex_set_concord(&juplex, 10.0f, 0.25f, 0.30f,
                                0.80f, 0.40f, 0.90f, 0.92f) ||
        !or_juplex_set_mode(&juplex, OR_JUPLEX_TANGO_MINISTRY, 0.45f, 4.0f) ||
        !or_juplex_process(&reasoners, &juplex,
                            left_codes, right_codes,
                            decoded_left, decoded_right, 16u) ||
        !or_juplex_guaranteed(&juplex)) return 8;
    for (uint32_t block = 0; block < 3000u; ++block)
        if (!or_juplex_process(&reasoners, &juplex,
                                left_codes, right_codes,
                                decoded_left, decoded_right, 16u) ||
            !or_juplex_guaranteed(&juplex)) return 8;
    ORJuplexIntegrationReport juplex_report;
    if (!or_juplex_review_integration(&juplex, &juplex_report)) return 8;
    const int juplex_was_guaranteed = or_juplex_guaranteed(&juplex);
    if (!or_juplex_recall_prior(&juplex) || or_juplex_guaranteed(&juplex))
        return 9;
    const ORJuplexRegime juplex_before_rejected_block = juplex;
    const float output_before_rejected_block = decoded_left[0];
    if (or_juplex_process(&reasoners, &juplex,
                           left_codes, right_codes,
                           decoded_left, decoded_right,
                           OR_ENTROPY_DECOMPRESS_MAX_FRAMES + 1u) ||
        memcmp(&juplex, &juplex_before_rejected_block, sizeof(juplex)) != 0 ||
        decoded_left[0] != output_before_rejected_block) return 10;

    ORJuplexRegime moat_regime;
    ORJuplexTransvixor transvixor;
    ORJuplexPriorEnvelope envelope;
    ORWaveDuplexorEditor proposed_editor;
    ORJuplexConcord proposed_concord;
    if (!or_juplex_init(&moat_regime, 48000u) ||
        !or_juplex_transvixor_init(&transvixor, 0x4a55504cu)) return 11;
    proposed_editor = moat_regime.current.duplexor_editor;
    if (!or_wave_duplexor_editor_set(&proposed_editor,
                                      0.92f, 1.08f, 0.02f, -0.02f,
                                      0.38f, 0.42f, 0.92f)) return 12;
    proposed_concord = moat_regime.concord;
    proposed_concord.entropy_response_hz = 9.0f;
    proposed_concord.entropy_meet_depth = 0.30f;
    proposed_concord.entropy_separate_depth = 0.34f;
    proposed_concord.tango_hz = 0.90f;
    proposed_concord.tango_depth = 0.42f;
    proposed_concord.prior_authority = 0.88f;
    proposed_concord.output_ceiling = 0.90f;
    const ORJuplexRegime regime_before_stage = moat_regime;
    if (!or_juplex_transvixor_stage(
            &transvixor, &moat_regime, 0x4a55504cu,
            &proposed_editor, &proposed_concord,
            OR_JUPLEX_TANGO_MINISTRY, 0.46f, 4.5f, &envelope) ||
        envelope.state != OR_TRANSVIXOR_ENVELOPE_DORMANT ||
        memcmp(&moat_regime, &regime_before_stage, sizeof(moat_regime)) != 0 ||
        !or_juplex_transvixor_enact(&transvixor, &moat_regime, &envelope) ||
        !or_juplex_transvixor_guaranteed(
            &transvixor, &envelope, &moat_regime) ||
        !moat_regime.prior_valid ||
        moat_regime.prior_editor.config_checksum !=
            regime_before_stage.current.duplexor_editor.config_checksum)
        return 13;
    const int transvixor_was_guaranteed = or_juplex_transvixor_guaranteed(
        &transvixor, &envelope, &moat_regime);
    const uint32_t enacted_moat_flags = transvixor.moat_flags;
    ORRejukerPriorGuarantee rejuker;
    if (!or_rejuker_init(&rejuker, &transvixor, &envelope, &moat_regime) ||
        !or_rejuker_set_compass(&rejuker, 0.125f, 0.58f,
                                 0.94f, 0.14f, 0.84f) ||
        !or_rejuker_forward(&reasoners, &transvixor, &envelope,
                             &moat_regime, &rejuker,
                             left_codes, right_codes,
                             decoded_left, decoded_right, 16u) ||
        !or_rejuker_guaranteed(
            &rejuker, &transvixor, &envelope, &moat_regime)) return 14;
    const ORJuplexRegime regime_before_rejuker_rejection = moat_regime;
    const ORRejukerPriorGuarantee rejuker_before_rejection = rejuker;
    const float rejuker_output_before_rejection = decoded_left[0];
    if (or_rejuker_forward(&reasoners, &transvixor, &envelope,
                            &moat_regime, &rejuker,
                            left_codes, right_codes,
                            decoded_left, decoded_right,
                            OR_ENTROPY_DECOMPRESS_MAX_FRAMES + 1u) ||
        memcmp(&moat_regime, &regime_before_rejuker_rejection,
               sizeof(moat_regime)) != 0 ||
        memcmp(&rejuker, &rejuker_before_rejection,
               sizeof(rejuker)) != 0 ||
        decoded_left[0] != rejuker_output_before_rejection) return 15;
    if (!or_rejuker_set_minimum_priority(
            &rejuker, OR_FORESHADOW_PRIORITY_FORWARD)) return 16;
    float preview_left[16];
    float preview_right[16];
    ORRejukerForeshadow foreshadow;
    const ORJuplexRegime regime_before_foreshadow = moat_regime;
    const ORRejukerPriorGuarantee rejuker_before_foreshadow = rejuker;
    if (or_rejuker_foreshadow(
            &reasoners, &transvixor, &envelope,
            &moat_regime, &rejuker, OR_FORESHADOW_PRIORITY_NORMAL,
            left_codes, right_codes, preview_left, preview_right, 16u,
            &foreshadow) ||
        !or_rejuker_foreshadow(
            &reasoners, &transvixor, &envelope,
            &moat_regime, &rejuker, OR_FORESHADOW_PRIORITY_FORWARD,
            left_codes, right_codes, preview_left, preview_right, 16u,
            &foreshadow) ||
        !or_rejuker_foreshadow_is_current(
            &foreshadow, &envelope, &moat_regime, &rejuker) ||
        memcmp(&moat_regime, &regime_before_foreshadow,
               sizeof(moat_regime)) != 0 ||
        memcmp(&rejuker, &rejuker_before_foreshadow,
               sizeof(rejuker)) != 0) return 17;
    const uint32_t foreshadow_guarantees = foreshadow.guarantee_flags;
    int8_t tampered_left_codes[16];
    memcpy(tampered_left_codes, left_codes, sizeof(tampered_left_codes));
    tampered_left_codes[0]++;
    if (or_rejuker_forward_foreshadowed(
            &reasoners, &transvixor, &envelope,
            &moat_regime, &rejuker,
            tampered_left_codes, right_codes,
            decoded_left, decoded_right, 16u, &foreshadow) ||
        foreshadow.state != OR_FORESHADOW_READY ||
        memcmp(&moat_regime, &regime_before_foreshadow,
               sizeof(moat_regime)) != 0 ||
        memcmp(&rejuker, &rejuker_before_foreshadow,
               sizeof(rejuker)) != 0) return 18;
    if (!or_rejuker_forward_foreshadowed(
            &reasoners, &transvixor, &envelope,
            &moat_regime, &rejuker,
            left_codes, right_codes, decoded_left, decoded_right, 16u,
            &foreshadow) || foreshadow.state != OR_FORESHADOW_CONSUMED ||
        memcmp(decoded_left, preview_left, sizeof(preview_left)) != 0 ||
        memcmp(decoded_right, preview_right, sizeof(preview_right)) != 0 ||
        !or_rejuker_guaranteed(
            &rejuker, &transvixor, &envelope, &moat_regime)) return 19;
    const ORJuplexRegime regime_before_replay = moat_regime;
    const ORRejukerPriorGuarantee rejuker_before_replay = rejuker;
    if (or_rejuker_forward_foreshadowed(
            &reasoners, &transvixor, &envelope,
            &moat_regime, &rejuker,
            left_codes, right_codes, decoded_left, decoded_right, 16u,
            &foreshadow) ||
        memcmp(&moat_regime, &regime_before_replay,
               sizeof(moat_regime)) != 0 ||
        memcmp(&rejuker, &rejuker_before_replay,
               sizeof(rejuker)) != 0) return 20;

    ORRejukerFunctionRegimeClass function_regime;
    ORRejukerPropertyGlass prior_test_glass;
    ORRejukerPropertyGlass property_glass;
    if (!or_rejuker_regime_class_init(
            &function_regime, &transvixor, &envelope,
            &moat_regime, &rejuker)) return 21;
    const ORRejukerFunctionRegimeClass function_regime_before_test =
        function_regime;
    if (!or_rejuker_test_prior(
            &function_regime, &transvixor, &envelope,
            &moat_regime, &rejuker, &foreshadow, &prior_test_glass) ||
        !prior_test_glass.guaranteed ||
        memcmp(&function_regime, &function_regime_before_test,
               sizeof(function_regime)) != 0 ||
        !or_rejuker_regime_class_tick(
            &function_regime, &transvixor, &envelope,
            &moat_regime, &rejuker, &foreshadow) ||
        !or_rejuker_property_glass(
            &function_regime, &transvixor, &envelope,
            &moat_regime, &rejuker, &property_glass) ||
        !property_glass.guaranteed ||
        property_glass.property_flags != OR_REJUKER_PROPERTY_ALL ||
        !or_rejuker_regime_checksum_validate(&function_regime)) return 22;
    ORRejukerFunctionRegimeClass corrupt_function_regime = function_regime;
    corrupt_function_regime.markov.current[OR_REJUKER_MARKOV_CURRENT] +=
        0.01f;
    if (or_rejuker_regime_checksum_validate(&corrupt_function_regime) ||
        !or_rejuker_regime_checksum_validate(&function_regime)) return 23;

    ORJuplexPriorEnvelope stale_envelope;
    proposed_editor = moat_regime.current.duplexor_editor;
    if (!or_wave_duplexor_editor_set(&proposed_editor,
                                      0.98f, 1.02f, 0.01f, -0.01f,
                                      0.44f, 0.36f, 0.94f) ||
        !or_juplex_transvixor_stage(
            &transvixor, &moat_regime, 0x4a55504cu,
            &proposed_editor, &moat_regime.concord,
            OR_JUPLEX_ACCRUED_BLEND, 0.40f, 4.0f,
            &stale_envelope) ||
        !or_juplex_process(&reasoners, &moat_regime,
                            left_codes, right_codes,
                            decoded_left, decoded_right, 16u)) return 24;
    const ORJuplexRegime regime_before_stale_enact = moat_regime;
    if (or_juplex_transvixor_enact(
            &transvixor, &moat_regime, &stale_envelope) ||
        memcmp(&moat_regime, &regime_before_stale_enact,
               sizeof(moat_regime)) != 0 ||
        stale_envelope.state != OR_TRANSVIXOR_ENVELOPE_DORMANT ||
        !or_juplex_transvixor_abort(&transvixor, &stale_envelope) ||
        stale_envelope.state != OR_TRANSVIXOR_ENVELOPE_ABORTED) return 25;

    proposed_editor = moat_regime.current.duplexor_editor;
    if (!or_wave_duplexor_editor_set(&proposed_editor,
                                      1.80f, 0.20f, 0.0f, 0.0f,
                                      0.44f, 0.36f, 0.94f)) return 26;
    const ORJuplexTransvixor trans_before_rejection = transvixor;
    if (or_juplex_transvixor_stage(
            &transvixor, &moat_regime, 0x4a55504cu,
            &proposed_editor, &moat_regime.concord,
            OR_JUPLEX_CURRENT, 0.0f, 4.0f, &stale_envelope) ||
        memcmp(&transvixor, &trans_before_rejection,
               sizeof(transvixor)) != 0) return 27;
    printf("category=%s reasoner_entropy=%.4f signal_entropy=%.4f "
           "temporal_entropy=%.4f combined_entropy=%.4f "
           "derelict=%.4f peak=%.4f decompressed_frames=%u "
           "pir_volition=%.4f category_mask=0x%02x "
           "duplex_meet=%.2f separate=%.2f edits=%u "
           "juplex=%s prior=%u mix=%.5f entropy_mod=%.5f tango=%.5f "
           "guaranteed=%d peak=%.5f current_accrued=%.6f "
           "transvixor_guaranteed=%d moat=0x%02x enacted=%u aborted=%u "
           "rejuker_frames=%llu compass=%.3f forward_peak=%.5f "
           "foreshadow_priority=%u guarantee=0x%02x state=%u "
           "markov_state=%u markov_tick=%llu apriori_prior=%.5f "
           "posterior_prior=%.5f properties=0x%02x "
           "class_guaranteed=%d prior_test_guaranteed=%d\n",
           or_mod_entropy_mode_name(category.entropy.mode),
           category.entropy.reasoner_entropy,
           category.entropy.signal_entropy,
           category.entropy.temporal_entropy,
           category.entropy.combined_entropy,
           category.entropy.derelict_amount, peak, decoder.generation,
           decompressor_group.pir.volition,
           decompressor_group.category_mask,
           duplexor_group.duplexor_editor.meet,
           duplexor_group.duplexor_editor.separate,
           duplexor_group.duplexor_editor.generation,
           or_juplex_mode_name(juplex_report.mode),
           juplex_report.prior_valid, juplex_report.prior_mix,
           juplex_report.entropy_modulation, juplex_report.tango_phase,
           juplex_was_guaranteed, juplex_report.guaranteed_peak,
           juplex_report.current_accrued_energy,
           transvixor_was_guaranteed, enacted_moat_flags,
           transvixor.enacted_sequence,
           transvixor.aborted_sequence,
           (unsigned long long)rejuker.forwarded_frames,
           rejuker.kernel.compass_turns, rejuker.guaranteed_peak,
           rejuker.minimum_priority, foreshadow_guarantees,
           foreshadow.state, property_glass.dominant_state,
           (unsigned long long)property_glass.markov_tick,
           property_glass.apriori[OR_REJUKER_MARKOV_PRIOR],
           property_glass.posterior[OR_REJUKER_MARKOV_PRIOR],
           property_glass.property_flags, property_glass.guaranteed,
           prior_test_glass.guaranteed);
    return 0;
}
#endif
