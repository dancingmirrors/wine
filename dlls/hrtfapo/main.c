/*
 * Copyright (C) 2023 Mohamad Al-Jaf
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* clang-format off */
#define COBJMACROS
#include "initguid.h"
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "mmreg.h"
#include "ks.h"
#include "ksmedia.h"
#include "hrtfapoapi.h"
/* clang-format on */

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(xaudio2);

DEFINE_GUID(CLSID_hrtf_apo, 0x1d5de651, 0x6523, 0x4745, 0xa2, 0x65, 0x1b, 0x86, 0xbc, 0xea, 0x2f, 0x83);

#define HRTF_PI 3.14159265358979323846f

#define HEAD_RADIUS 0.0875f
#define SPEED_OF_SOUND 343.0f
#define SHADOW_ALPHA_MIN 0.1f
#define SHADOW_THETA_MIN (150.0f * HRTF_PI / 180.0f)
#define REAR_LOWPASS_HZ 4000.0f
#define REAR_LOWPASS_MIX 0.8f

enum sample_format {
    SAMPLE_FLOAT32,
    SAMPLE_PCM16,
};

struct hrtf_params {
    HrtfPosition position;
    HrtfOrientation orientation;
    float gain;
    HrtfEnvironment environment;
};

struct ear_target {
    float delay;
    float alpha;
    float rear;
    float gain;
};

struct ear_state {
    struct ear_target target;
    float shadow_x1, shadow_y1;
    float lowpass_y1;
};

struct hrtf_apo {
    IXAPO IXAPO_iface;
    IXAPOHrtfParameters IXAPOHrtfParameters_iface;
    LONG ref;

    HrtfDistanceDecay decay;
    HrtfDirectivity directivity;
    float cardioid_order;
    float cone_inner, cone_outer;

    CRITICAL_SECTION cs;
    struct hrtf_params params;

    BOOL locked;
    UINT32 sample_rate;
    UINT32 in_channels;
    float beta;
    float shadow_norm;
    float lowpass_coeff;
    float *delay_buffer;
    UINT32 delay_mask;
    void *scratch;
    UINT32 scratch_size;
    unsigned int dump_buffers;
    UINT32 delay_pos;
    UINT32 tail_frames;
    UINT32 silent_frames;
    BOOL snap;
    BOOL clean;
    struct hrtf_params render_params;
    struct ear_state ears[2];
};

static inline struct hrtf_apo *impl_from_IXAPO(IXAPO *iface) {
    return CONTAINING_RECORD(iface, struct hrtf_apo, IXAPO_iface);
}

static inline struct hrtf_apo *impl_from_IXAPOHrtfParameters(IXAPOHrtfParameters *iface) {
    return CONTAINING_RECORD(iface, struct hrtf_apo, IXAPOHrtfParameters_iface);
}

static inline float clampf(float value, float min, float max) {
    return value < min ? min : value > max ? max
                                           : value;
}

static inline float db_to_linear(float db) {
    return powf(10.0f, db / 20.0f);
}

static BOOL get_sample_format(const WAVEFORMATEX *format, enum sample_format *sample_format) {
    WORD tag = format->wFormatTag;

    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;

        if (format->cbSize < sizeof(*extensible) - sizeof(*format)) {
            return FALSE;
        }
        if (IsEqualGUID(&extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(&extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        } else {
            return FALSE;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        *sample_format = SAMPLE_FLOAT32;
    } else if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) {
        *sample_format = SAMPLE_PCM16;
    } else {
        return FALSE;
    }
    return TRUE;
}

static inline unsigned int sample_size(enum sample_format format) {
    return format == SAMPLE_PCM16 ? sizeof(short) : sizeof(float);
}

#define DATA_SAMPLE_SIZE sizeof(float)

static BOOL is_rate_supported(const WAVEFORMATEX *format, const WAVEFORMATEX *other) {
    if (format->nSamplesPerSec < XAPO_MIN_FRAMERATE || format->nSamplesPerSec > XAPO_MAX_FRAMERATE) {
        return FALSE;
    }
    return !other || other->nSamplesPerSec == format->nSamplesPerSec;
}

static BOOL is_input_supported(const WAVEFORMATEX *input, const WAVEFORMATEX *output) {
    enum sample_format sample_format;

    return get_sample_format(input, &sample_format) && (input->nChannels == 1 || input->nChannels == 2) && is_rate_supported(input, output);
}

static BOOL is_output_supported(const WAVEFORMATEX *output, const WAVEFORMATEX *input) {
    enum sample_format sample_format;

    return get_sample_format(output, &sample_format) && output->nChannels == 2 && is_rate_supported(output, input);
}

static WAVEFORMATEX *alloc_format(WORD channels, const WAVEFORMATEX *other) {
    enum sample_format sample_format;
    DWORD rate = other->nSamplesPerSec;
    WAVEFORMATEX *format;

    if (rate < XAPO_MIN_FRAMERATE || rate > XAPO_MAX_FRAMERATE) {
        rate = 48000;
    }
    if (!get_sample_format(other, &sample_format)) {
        sample_format = SAMPLE_FLOAT32;
    }
    if (!(format = CoTaskMemAlloc(sizeof(*format)))) {
        return NULL;
    }
    format->wFormatTag = sample_format == SAMPLE_PCM16 ? WAVE_FORMAT_PCM : WAVE_FORMAT_IEEE_FLOAT;
    format->nChannels = channels;
    format->nSamplesPerSec = rate;
    format->wBitsPerSample = sample_size(sample_format) * 8;
    format->nBlockAlign = channels * sample_size(sample_format);
    format->nAvgBytesPerSec = rate * format->nBlockAlign;
    format->cbSize = 0;
    return format;
}

static void reset_state(struct hrtf_apo *apo) {
    if (apo->clean) {
        return;
    }
    memset(apo->ears, 0, sizeof(apo->ears));
    if (apo->delay_buffer) {
        memset(apo->delay_buffer, 0, (apo->delay_mask + 1) * sizeof(float));
    }
    apo->delay_pos = 0;
    apo->silent_frames = apo->tail_frames;
    apo->snap = TRUE;
    apo->clean = TRUE;
}

static UINT32 decay_frames(float pole) {
    pole = fabsf(pole);
    if (pole < 1e-3f) {
        return 1;
    }
    if (pole > 0.9999f) {
        pole = 0.9999f;
    }
    return (UINT32)ceilf(-13.8155f / logf(pole));
}

static void update_render_params(struct hrtf_apo *apo) {
    if (!TryEnterCriticalSection(&apo->cs)) {
        return;
    }
    apo->render_params = apo->params;
    LeaveCriticalSection(&apo->cs);
}

static float distance_gain(const struct hrtf_apo *apo, float distance) {
    float db;

    if (apo->decay.type == CustomDecay) {
        db = apo->render_params.gain;
    } else {
        if (distance >= apo->decay.cutoffDistance) {
            return 0.0f;
        }
        db = 20.0f * log10f(apo->decay.unityGainDistance / clampf(distance, 1e-4f, FLT_MAX));
    }

    return db_to_linear(clampf(db, apo->decay.minGain, apo->decay.maxGain));
}

static float directivity_gain(const struct hrtf_apo *apo, const float *direction) {
    const float *m = apo->render_params.orientation.element;
    float forward[3], length, cos_angle, angle, inner, outer, pattern;

    if (apo->directivity.type == OmniDirectional) {
        return 1.0f;
    }

    forward[0] = -m[6];
    forward[1] = -m[7];
    forward[2] = -m[8];
    length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (length < 1e-6f) {
        return 1.0f;
    }

    cos_angle = -(forward[0] * direction[0] + forward[1] * direction[1] + forward[2] * direction[2]) / length;
    cos_angle = clampf(cos_angle, -1.0f, 1.0f);

    if (apo->directivity.type == Cardioid) {
        pattern = powf((1.0f + cos_angle) * 0.5f, apo->cardioid_order);
        return 1.0f - apo->directivity.scaling * (1.0f - pattern);
    }

    angle = acosf(cos_angle);
    inner = apo->cone_inner * 0.5f;
    outer = apo->cone_outer * 0.5f;
    if (angle <= inner) {
        pattern = 1.0f;
    } else if (angle >= outer) {
        pattern = 0.0f;
    } else {
        pattern = 1.0f - (angle - inner) / (outer - inner);
    }
    return 1.0f - apo->directivity.scaling * (1.0f - pattern);
}

static void compute_ear_target(const struct hrtf_apo *apo, const float *direction, float gain,
                               unsigned int ear, struct ear_target *target) {
    const float radius_delay = HEAD_RADIUS / SPEED_OF_SOUND;
    float theta = acosf(clampf(ear ? direction[0] : -direction[0], -1.0f, 1.0f));
    float delay;

    if (theta < HRTF_PI / 2) {
        delay = radius_delay * (1.0f - cosf(theta));
    } else {
        delay = radius_delay * (1.0f + theta - HRTF_PI / 2);
    }

    target->delay = delay * apo->sample_rate;
    target->alpha = (1.0f + SHADOW_ALPHA_MIN / 2) + (1.0f - SHADOW_ALPHA_MIN / 2) * cosf(theta * HRTF_PI / SHADOW_THETA_MIN);
    target->rear = REAR_LOWPASS_MIX * clampf(direction[2], 0.0f, 1.0f);
    target->gain = gain;
}

static void compute_targets(struct hrtf_apo *apo, struct ear_target targets[2]) {
    const HrtfPosition *position = &apo->render_params.position;
    float direction[3], distance, gain;

    distance = sqrtf(position->x * position->x + position->y * position->y + position->z * position->z);
    if (distance > 1e-5f) {
        direction[0] = position->x / distance;
        direction[1] = position->y / distance;
        direction[2] = position->z / distance;
    } else {
        direction[0] = 0.0f;
        direction[1] = 0.0f;
        direction[2] = -1.0f;
        distance = 0.0f;
    }

    gain = distance_gain(apo, distance) * directivity_gain(apo, direction);
    compute_ear_target(apo, direction, gain, 0, &targets[0]);
    compute_ear_target(apo, direction, gain, 1, &targets[1]);
}

static inline float read_channel(const struct hrtf_apo *apo, const void *input, UINT32 frame, unsigned int channel) {
    return ((const float *)input)[frame * apo->in_channels + channel];
}

static inline float input_sample(const struct hrtf_apo *apo, const void *input, UINT32 frame) {
    if (apo->in_channels == 1) {
        return read_channel(apo, input, frame, 0);
    }
    return 0.5f * (read_channel(apo, input, frame, 0) + read_channel(apo, input, frame, 1));
}

static inline void write_sample(void *output, UINT32 index, float value) {
    ((float *)output)[index] = value;
}

static BOOL input_overlaps_output(const struct hrtf_apo *apo, const void *input, const void *output, UINT32 frames) {
    const char *in_start = input, *out_start = output;
    const char *in_end = in_start + (size_t)frames * apo->in_channels * DATA_SAMPLE_SIZE;
    const char *out_end = out_start + (size_t)frames * 2 * DATA_SAMPLE_SIZE;

    return in_start < out_end && out_start < in_end;
}

static BOOL describe_input(const struct hrtf_apo *apo, const void *input, UINT32 frames) {
    UINT32 bytes_count = frames * apo->in_channels * DATA_SAMPLE_SIZE, i, first_nonzero = 0;
    const unsigned char *bytes = input;
    char hex[3 * 32 + 1] = "";
    unsigned int floats_in_range = 0, floats_checked = 0;
    int pcm_peak = 0;

    for (i = 0; i < bytes_count / sizeof(short); ++i) {
        int value = abs(((const short *)input)[i]);
        if (value > pcm_peak) {
            pcm_peak = value;
        }
    }
    if (!pcm_peak) {
        return FALSE;
    }

    for (i = 0; i < bytes_count && !bytes[i]; ++i)
        ;
    first_nonzero = i & ~(UINT32)3;
    for (i = 0; i < 32 && first_nonzero + i < bytes_count; ++i) {
        sprintf(hex + 3 * i, "%02x ", bytes[first_nonzero + i]);
    }

    for (i = first_nonzero / sizeof(float); i < bytes_count / sizeof(float) && floats_checked < 64; ++i, ++floats_checked) {
        float value = ((const float *)input)[i];
        if (isfinite(value) && fabsf(value) <= 1.0f) {
            floats_in_range++;
        }
    }

    TRACE("%u frames, bytes from offset %u: %s\n", frames, first_nonzero, hex);
    TRACE("as 16-bit PCM peak %d, as float %u of %u in [-1, 1].\n", pcm_peak, floats_in_range, floats_checked);
    return TRUE;
}

static HRESULT WINAPI xapo_QueryInterface(IXAPO *iface, REFIID riid, void **out) {
    struct hrtf_apo *apo = impl_from_IXAPO(iface);

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IXAPO) || IsEqualGUID(riid, &IID_IXAPO27)) {
        *out = &apo->IXAPO_iface;
    } else if (IsEqualGUID(riid, &IID_IXAPOHrtfParameters)) {
        *out = &apo->IXAPOHrtfParameters_iface;
    } else {
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI xapo_AddRef(IXAPO *iface) {
    struct hrtf_apo *apo = impl_from_IXAPO(iface);
    ULONG refcount = InterlockedIncrement(&apo->ref);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI xapo_Release(IXAPO *iface) {
    struct hrtf_apo *apo = impl_from_IXAPO(iface);
    ULONG refcount = InterlockedDecrement(&apo->ref);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount) {
        apo->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&apo->cs);
        free(apo->delay_buffer);
        free(apo->scratch);
        free(apo);
    }

    return refcount;
}

static HRESULT WINAPI xapo_GetRegistrationProperties(IXAPO *iface, XAPO_REGISTRATION_PROPERTIES **props) {
    static const WCHAR name[] = L"HRTF APO";
    static const WCHAR copyright[] = L"Copyright (C) the Wine project";
    XAPO_REGISTRATION_PROPERTIES *properties;

    TRACE("iface %p, props %p.\n", iface, props);

    if (!props) {
        return E_INVALIDARG;
    }
    if (!(properties = CoTaskMemAlloc(sizeof(*properties)))) {
        return E_OUTOFMEMORY;
    }

    memset(properties, 0, sizeof(*properties));
    properties->clsid = CLSID_hrtf_apo;
    memcpy(properties->FriendlyName, name, sizeof(name));
    memcpy(properties->CopyrightInfo, copyright, sizeof(copyright));
    properties->MajorVersion = 1;
    properties->MinorVersion = 0;
    properties->Flags = XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH | XAPO_FLAG_BUFFERCOUNT_MUST_MATCH;
    properties->MinInputBufferCount = 1;
    properties->MaxInputBufferCount = 1;
    properties->MinOutputBufferCount = 1;
    properties->MaxOutputBufferCount = 1;

    *props = properties;
    return S_OK;
}

static HRESULT WINAPI xapo_IsInputFormatSupported(IXAPO *iface, const WAVEFORMATEX *output_fmt,
                                                  const WAVEFORMATEX *input_fmt, WAVEFORMATEX **supported_fmt) {
    TRACE("iface %p, output_fmt %p, input_fmt %p, supported_fmt %p.\n", iface, output_fmt, input_fmt, supported_fmt);

    if (supported_fmt) {
        *supported_fmt = NULL;
    }
    if (!output_fmt || !input_fmt) {
        return E_INVALIDARG;
    }

    if (is_input_supported(input_fmt, output_fmt)) {
        return S_OK;
    }

    if (supported_fmt && !(*supported_fmt = alloc_format(1, output_fmt))) {
        return E_OUTOFMEMORY;
    }
    return XAPO_E_FORMAT_UNSUPPORTED;
}

static HRESULT WINAPI xapo_IsOutputFormatSupported(IXAPO *iface, const WAVEFORMATEX *input_fmt,
                                                   const WAVEFORMATEX *output_fmt, WAVEFORMATEX **supported_fmt) {
    TRACE("iface %p, input_fmt %p, output_fmt %p, supported_fmt %p.\n", iface, input_fmt, output_fmt, supported_fmt);

    if (supported_fmt) {
        *supported_fmt = NULL;
    }
    if (!output_fmt || !input_fmt) {
        return E_INVALIDARG;
    }

    if (is_output_supported(output_fmt, input_fmt)) {
        return S_OK;
    }

    if (supported_fmt && !(*supported_fmt = alloc_format(2, input_fmt))) {
        return E_OUTOFMEMORY;
    }
    return XAPO_E_FORMAT_UNSUPPORTED;
}

static HRESULT WINAPI xapo_Initialize(IXAPO *iface, const void *data, UINT32 data_len) {
    TRACE("iface %p, data %p, data_len %u.\n", iface, data, data_len);

    if (data && data_len) {
        FIXME("Ignoring %u bytes of initialization data.\n", data_len);
    }

    return S_OK;
}

static void WINAPI xapo_Reset(IXAPO *iface) {
    struct hrtf_apo *apo = impl_from_IXAPO(iface);

    TRACE("iface %p.\n", iface);

    reset_state(apo);
}

static HRESULT WINAPI xapo_LockForProcess(IXAPO *iface, UINT32 in_count,
                                          const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *in_params, UINT32 out_count,
                                          const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *out_params) {
    struct hrtf_apo *apo = impl_from_IXAPO(iface);
    const WAVEFORMATEX *input, *output;
    float beta, lowpass_coeff, max_delay;
    UINT32 size;

    TRACE("iface %p, in_count %u, in_params %p, out_count %u, out_params %p.\n",
          iface, in_count, in_params, out_count, out_params);

    apo->locked = FALSE;

    if (in_count != 1 || out_count != 1 || !in_params || !out_params) {
        return E_INVALIDARG;
    }

    input = in_params->pFormat;
    output = out_params->pFormat;
    if (!input || !output) {
        return E_INVALIDARG;
    }

    TRACE("input tag %#x, %u channels, %lu Hz, %u bits, align %u, %lu B/s, cbSize %u, max %u frames.\n",
          input->wFormatTag, input->nChannels, input->nSamplesPerSec, input->wBitsPerSample,
          input->nBlockAlign, input->nAvgBytesPerSec, input->cbSize, in_params->MaxFrameCount);
    TRACE("output tag %#x, %u channels, %lu Hz, %u bits, align %u, %lu B/s, cbSize %u, max %u frames.\n",
          output->wFormatTag, output->nChannels, output->nSamplesPerSec, output->wBitsPerSample,
          output->nBlockAlign, output->nAvgBytesPerSec, output->cbSize, out_params->MaxFrameCount);

    if (!is_input_supported(input, output) || !is_output_supported(output, input)) {
        FIXME("Unsupported formats, input tag %#x, %u channels, %lu Hz, %u bits; output tag %#x, %u channels, %lu Hz, %u bits.\n",
              input->wFormatTag, input->nChannels, input->nSamplesPerSec, input->wBitsPerSample,
              output->wFormatTag, output->nChannels, output->nSamplesPerSec, output->wBitsPerSample);
        return XAPO_E_FORMAT_UNSUPPORTED;
    }

    beta = SPEED_OF_SOUND / HEAD_RADIUS / input->nSamplesPerSec;
    lowpass_coeff = 1.0f - expf(-2.0f * HRTF_PI * REAR_LOWPASS_HZ / input->nSamplesPerSec);

    max_delay = HEAD_RADIUS / SPEED_OF_SOUND * (1.0f + HRTF_PI / 2) * input->nSamplesPerSec;
    size = 4;
    while (size < max_delay + 2.0f) {
        size *= 2;
    }
    if (!apo->delay_buffer || apo->delay_mask != size - 1) {
        float *buffer;

        if (!(buffer = malloc(size * sizeof(float)))) {
            return E_OUTOFMEMORY;
        }
        free(apo->delay_buffer);
        apo->delay_buffer = buffer;
        apo->delay_mask = size - 1;
    }

    apo->sample_rate = input->nSamplesPerSec;
    apo->in_channels = input->nChannels;
    apo->beta = beta;
    apo->shadow_norm = 1.0f / (1.0f + beta);
    apo->lowpass_coeff = lowpass_coeff;
    apo->tail_frames = size + max(decay_frames((1.0f - beta) / (1.0f + beta)), decay_frames(1.0f - lowpass_coeff));

    apo->dump_buffers = 4;
    apo->clean = FALSE;
    reset_state(apo);
    apo->locked = TRUE;
    return S_OK;
}

static void WINAPI xapo_UnlockForProcess(IXAPO *iface) {
    struct hrtf_apo *apo = impl_from_IXAPO(iface);

    TRACE("iface %p.\n", iface);

    apo->locked = FALSE;
}

static void WINAPI xapo_Process(IXAPO *iface, UINT32 in_count, const XAPO_PROCESS_BUFFER_PARAMETERS *in_params,
                                UINT32 out_count, XAPO_PROCESS_BUFFER_PARAMETERS *out_params, BOOL enabled) {
    struct hrtf_apo *apo = impl_from_IXAPO(iface);
    struct ear_target target[2], step[2];
    const void *input;
    void *output;
    UINT32 frames, i;
    unsigned int ear;
    BOOL silent;

    TRACE("iface %p, in_count %u, in_params %p (%u frames, flags %#x), out_count %u, out_params %p, enabled %d.\n",
          iface, in_count, in_params, in_count && in_params ? in_params->ValidFrameCount : 0,
          in_count && in_params ? in_params->BufferFlags : 0, out_count, out_params, enabled);

    if (out_count != 1 || !out_params || !out_params->pBuffer) {
        WARN("Unexpected output buffers.\n");
        return;
    }
    output = out_params->pBuffer;

    if (in_count != 1 || !in_params || !apo->locked) {
        WARN("Unexpected input buffers, or not locked.\n");
        out_params->ValidFrameCount = 0;
        out_params->BufferFlags = XAPO_BUFFER_SILENT;
        return;
    }

    frames = in_params->ValidFrameCount;
    input = in_params->pBuffer;
    silent = in_params->BufferFlags == XAPO_BUFFER_SILENT || !input;
    out_params->ValidFrameCount = frames;
    out_params->BufferFlags = silent ? XAPO_BUFFER_SILENT : XAPO_BUFFER_VALID;
    if (!frames) {
        return;
    }

    if (!silent && apo->dump_buffers && TRACE_ON(xaudio2) && describe_input(apo, input, frames)) {
        apo->dump_buffers--;
    }

    if (!silent && input_overlaps_output(apo, input, output, frames)) {
        UINT32 size = frames * apo->in_channels * DATA_SAMPLE_SIZE;

        TRACE("Overlapping buffers, input %p, output %p.\n", input, output);
        if (size > apo->scratch_size) {
            void *scratch;

            if (!(scratch = malloc(size))) {
                WARN("Out of memory for the in-place copy.\n");
                memset(output, 0, frames * 2 * DATA_SAMPLE_SIZE);
                out_params->BufferFlags = XAPO_BUFFER_SILENT;
                return;
            }
            free(apo->scratch);
            apo->scratch = scratch;
            apo->scratch_size = size;
        }
        memcpy(apo->scratch, input, size);
        input = apo->scratch;
    }

    if (!enabled) {
        reset_state(apo);
        if (silent) {
            memset(output, 0, frames * 2 * DATA_SAMPLE_SIZE);
        } else {
            for (i = 0; i < frames; ++i) {
                write_sample(output, 2 * i, read_channel(apo, input, i, 0));
                write_sample(output, 2 * i + 1, read_channel(apo, input, i, apo->in_channels - 1));
            }
        }
        return;
    }

    if (silent) {
        if (apo->silent_frames >= apo->tail_frames) {
            memset(output, 0, frames * 2 * DATA_SAMPLE_SIZE);
            reset_state(apo);
            return;
        }
        apo->silent_frames += frames;
    } else {
        apo->silent_frames = 0;
    }
    out_params->BufferFlags = XAPO_BUFFER_VALID;

    update_render_params(apo);
    compute_targets(apo, target);

    for (ear = 0; ear < 2; ++ear) {
        struct ear_state *state = &apo->ears[ear];

        if (apo->snap) {
            state->target = target[ear];
        }
        step[ear].delay = (target[ear].delay - state->target.delay) / frames;
        step[ear].alpha = (target[ear].alpha - state->target.alpha) / frames;
        step[ear].rear = (target[ear].rear - state->target.rear) / frames;
        step[ear].gain = (target[ear].gain - state->target.gain) / frames;
    }
    apo->snap = FALSE;
    apo->clean = FALSE;

    for (i = 0; i < frames; ++i) {
        float x = silent ? 0.0f : input_sample(apo, input, i);

        apo->delay_buffer[apo->delay_pos] = x;

        for (ear = 0; ear < 2; ++ear) {
            struct ear_state *state = &apo->ears[ear];
            struct ear_target *current = &state->target;
            float b0, b1, a1, frac, s0, s1, s, y;
            UINT32 whole;

            current->delay += step[ear].delay;
            current->alpha += step[ear].alpha;
            current->rear += step[ear].rear;
            current->gain += step[ear].gain;

            if (current->delay < 0.0f) {
                current->delay = 0.0f;
            }
            whole = (UINT32)current->delay;
            frac = current->delay - whole;
            s0 = apo->delay_buffer[(apo->delay_pos - whole) & apo->delay_mask];
            s1 = apo->delay_buffer[(apo->delay_pos - whole - 1) & apo->delay_mask];
            s = s0 + (s1 - s0) * frac;

            b0 = (current->alpha + apo->beta) * apo->shadow_norm;
            b1 = (apo->beta - current->alpha) * apo->shadow_norm;
            a1 = (1.0f - apo->beta) * apo->shadow_norm;
            y = b0 * s + b1 * state->shadow_x1 + a1 * state->shadow_y1;
            state->shadow_x1 = s;
            state->shadow_y1 = y;

            state->lowpass_y1 += apo->lowpass_coeff * (y - state->lowpass_y1);
            y += current->rear * (state->lowpass_y1 - y);

            write_sample(output, 2 * i + ear, y * current->gain);
        }

        apo->delay_pos = (apo->delay_pos + 1) & apo->delay_mask;
    }

    for (ear = 0; ear < 2; ++ear) {
        struct ear_state *state = &apo->ears[ear];

        state->target = target[ear];
        if (fabsf(state->shadow_x1) < 1e-20f) {
            state->shadow_x1 = 0.0f;
        }
        if (fabsf(state->shadow_y1) < 1e-20f) {
            state->shadow_y1 = 0.0f;
        }
        if (fabsf(state->lowpass_y1) < 1e-20f) {
            state->lowpass_y1 = 0.0f;
        }
    }
}

static UINT32 WINAPI xapo_CalcInputFrames(IXAPO *iface, UINT32 output_frames) {
    TRACE("iface %p, output_frames %u.\n", iface, output_frames);
    return output_frames;
}

static UINT32 WINAPI xapo_CalcOutputFrames(IXAPO *iface, UINT32 input_frames) {
    TRACE("iface %p, input_frames %u.\n", iface, input_frames);
    return input_frames;
}

static const IXAPOVtbl xapo_vtbl =
    {
        xapo_QueryInterface,
        xapo_AddRef,
        xapo_Release,
        xapo_GetRegistrationProperties,
        xapo_IsInputFormatSupported,
        xapo_IsOutputFormatSupported,
        xapo_Initialize,
        xapo_Reset,
        xapo_LockForProcess,
        xapo_UnlockForProcess,
        xapo_Process,
        xapo_CalcInputFrames,
        xapo_CalcOutputFrames,
};

static HRESULT WINAPI hrtf_params_QueryInterface(IXAPOHrtfParameters *iface, REFIID riid, void **out) {
    struct hrtf_apo *apo = impl_from_IXAPOHrtfParameters(iface);
    return IXAPO_QueryInterface(&apo->IXAPO_iface, riid, out);
}

static ULONG WINAPI hrtf_params_AddRef(IXAPOHrtfParameters *iface) {
    struct hrtf_apo *apo = impl_from_IXAPOHrtfParameters(iface);
    return IXAPO_AddRef(&apo->IXAPO_iface);
}

static ULONG WINAPI hrtf_params_Release(IXAPOHrtfParameters *iface) {
    struct hrtf_apo *apo = impl_from_IXAPOHrtfParameters(iface);
    return IXAPO_Release(&apo->IXAPO_iface);
}

static HRESULT WINAPI hrtf_params_SetSourcePosition(IXAPOHrtfParameters *iface, const HrtfPosition *position) {
    struct hrtf_apo *apo = impl_from_IXAPOHrtfParameters(iface);

    if (!position) {
        WARN("iface %p, position NULL.\n", iface);
        return E_INVALIDARG;
    }

    TRACE("iface %p, position (%f, %f, %f).\n", iface, position->x, position->y, position->z);

    if (!isfinite(position->x) || !isfinite(position->y) || !isfinite(position->z)) {
        WARN("Invalid position.\n");
        return E_INVALIDARG;
    }

    EnterCriticalSection(&apo->cs);
    apo->params.position = *position;
    LeaveCriticalSection(&apo->cs);
    return S_OK;
}

static HRESULT WINAPI hrtf_params_SetSourceOrientation(IXAPOHrtfParameters *iface, const HrtfOrientation *orientation) {
    struct hrtf_apo *apo = impl_from_IXAPOHrtfParameters(iface);
    unsigned int i;

    TRACE("iface %p, orientation %p.\n", iface, orientation);

    if (!orientation) {
        return E_INVALIDARG;
    }
    for (i = 0; i < ARRAY_SIZE(orientation->element); ++i) {
        if (!isfinite(orientation->element[i])) {
            WARN("Invalid orientation element %u.\n", i);
            return E_INVALIDARG;
        }
    }

    EnterCriticalSection(&apo->cs);
    apo->params.orientation = *orientation;
    LeaveCriticalSection(&apo->cs);
    return S_OK;
}

static HRESULT WINAPI hrtf_params_SetSourceGain(IXAPOHrtfParameters *iface, float gain) {
    struct hrtf_apo *apo = impl_from_IXAPOHrtfParameters(iface);

    TRACE("iface %p, gain %f.\n", iface, gain);

    if (!isfinite(gain)) {
        return E_INVALIDARG;
    }
    if (apo->decay.type != CustomDecay) {
        WARN("Gain is only used with custom distance decay.\n");
    }

    EnterCriticalSection(&apo->cs);
    apo->params.gain = clampf(gain, HRTF_MIN_GAIN_LIMIT, HRTF_MAX_GAIN_LIMIT);
    LeaveCriticalSection(&apo->cs);
    return S_OK;
}

static HRESULT WINAPI hrtf_params_SetEnvironment(IXAPOHrtfParameters *iface, HrtfEnvironment environment) {
    struct hrtf_apo *apo = impl_from_IXAPOHrtfParameters(iface);
    static LONG once;

    TRACE("iface %p, environment %d.\n", iface, environment);

    if ((unsigned int)environment > Outdoors) {
        return E_INVALIDARG;
    }
    if (!InterlockedExchange(&once, 1)) {
        FIXME("Environment reverberation is not implemented.\n");
    }

    EnterCriticalSection(&apo->cs);
    apo->params.environment = environment;
    LeaveCriticalSection(&apo->cs);
    return S_OK;
}

static const IXAPOHrtfParametersVtbl hrtf_params_vtbl =
    {
        hrtf_params_QueryInterface,
        hrtf_params_AddRef,
        hrtf_params_Release,
        hrtf_params_SetSourcePosition,
        hrtf_params_SetSourceOrientation,
        hrtf_params_SetSourceGain,
        hrtf_params_SetEnvironment,
};

static HRESULT init_distance_decay(struct hrtf_apo *apo, const HrtfDistanceDecay *decay) {
    if (decay->type != NaturalDecay && decay->type != CustomDecay) {
        WARN("Invalid decay type %d.\n", decay->type);
        return E_INVALIDARG;
    }
    if (!isfinite(decay->maxGain) || !isfinite(decay->minGain) || !isfinite(decay->unityGainDistance) || isnan(decay->cutoffDistance)) {
        WARN("Invalid decay parameters.\n");
        return E_INVALIDARG;
    }

    TRACE("type %d, max gain %f, min gain %f, unity gain distance %f, cutoff distance %f.\n", decay->type,
          decay->maxGain, decay->minGain, decay->unityGainDistance, decay->cutoffDistance);

    apo->decay.type = decay->type;
    apo->decay.maxGain = clampf(decay->maxGain, HRTF_MIN_GAIN_LIMIT, HRTF_MAX_GAIN_LIMIT);
    apo->decay.minGain = clampf(decay->minGain, HRTF_MIN_GAIN_LIMIT, apo->decay.maxGain);
    apo->decay.unityGainDistance = clampf(decay->unityGainDistance, HRTF_MIN_UNITY_GAIN_DISTANCE, FLT_MAX);
    apo->decay.cutoffDistance = decay->cutoffDistance > 0.0f ? decay->cutoffDistance : HRTF_DEFAULT_CUTOFF_DISTANCE;
    return S_OK;
}

static HRESULT init_directivity(struct hrtf_apo *apo, const HrtfDirectivity *directivity) {
    if (!isfinite(directivity->scaling)) {
        WARN("Invalid directivity scaling.\n");
        return E_INVALIDARG;
    }

    TRACE("type %d, scaling %f.\n", directivity->type, directivity->scaling);

    apo->directivity.type = directivity->type;
    apo->directivity.scaling = clampf(directivity->scaling, 0.0f, 1.0f);

    switch (directivity->type) {
    case OmniDirectional:
        break;

    case Cardioid: {
        const HrtfDirectivityCardioid *cardioid = (const HrtfDirectivityCardioid *)directivity;

        if (!isfinite(cardioid->order)) {
            return E_INVALIDARG;
        }
        TRACE("cardioid order %f.\n", cardioid->order);
        apo->cardioid_order = clampf(cardioid->order, 0.0f, 32.0f);
        break;
    }

    case Cone: {
        const HrtfDirectivityCone *cone = (const HrtfDirectivityCone *)directivity;

        if (!isfinite(cone->innerAngle) || !isfinite(cone->outerAngle)) {
            return E_INVALIDARG;
        }
        TRACE("cone inner angle %f, outer angle %f.\n", cone->innerAngle, cone->outerAngle);
        apo->cone_inner = clampf(cone->innerAngle, 0.0f, 2 * HRTF_PI);
        apo->cone_outer = clampf(cone->outerAngle, apo->cone_inner, 2 * HRTF_PI);
        break;
    }

    default:
        WARN("Invalid directivity type %d.\n", directivity->type);
        return E_INVALIDARG;
    }

    return S_OK;
}

HRESULT WINAPI CreateHrtfApo(const HrtfApoInit *init, IXAPO **xapo) {
    struct hrtf_apo *apo;
    HRESULT hr;

    TRACE("init %p, xapo %p.\n", init, xapo);

    if (!xapo) {
        return E_INVALIDARG;
    }
    *xapo = NULL;

    if (!(apo = calloc(1, sizeof(*apo)))) {
        return E_OUTOFMEMORY;
    }

    apo->IXAPO_iface.lpVtbl = &xapo_vtbl;
    apo->IXAPOHrtfParameters_iface.lpVtbl = &hrtf_params_vtbl;
    apo->ref = 1;

    apo->decay.type = NaturalDecay;
    apo->decay.maxGain = HRTF_MAX_GAIN_LIMIT;
    apo->decay.minGain = HRTF_MIN_GAIN_LIMIT;
    apo->decay.unityGainDistance = HRTF_DEFAULT_UNITY_GAIN_DISTANCE;
    apo->decay.cutoffDistance = HRTF_DEFAULT_CUTOFF_DISTANCE;
    apo->directivity.type = OmniDirectional;

    if (init && init->distanceDecay && FAILED(hr = init_distance_decay(apo, init->distanceDecay))) {
        free(apo);
        return hr;
    }
    if (init && init->directivity && FAILED(hr = init_directivity(apo, init->directivity))) {
        free(apo);
        return hr;
    }

    apo->params.orientation.element[0] = 1.0f;
    apo->params.orientation.element[4] = 1.0f;
    apo->params.orientation.element[8] = 1.0f;

    InitializeCriticalSectionEx(&apo->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    apo->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": hrtf_apo.cs");

    TRACE("Created HRTF APO %p.\n", apo);

    *xapo = &apo->IXAPO_iface;
    return S_OK;
}
