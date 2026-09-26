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
