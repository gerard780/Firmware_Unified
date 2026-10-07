/* Native two-pass Hanshow Q-N panel profiles; configured pins use epd_io. */
#ifndef HANSHOW_QN_H
#define HANSHOW_QN_H

#include <stdbool.h>
#include <stdint.h>
#include "od_span.h"

#define HANSHOW_QN_WAVE_BYTES 535u
#define HANSHOW_QN_FAMILY_LEGACY 0u
#define HANSHOW_QN_FAMILY_0ENSPQ1YQ 1u

/* Captured values replace these registers at their normal initialization positions. */
struct hanshow_qn_initializer {
    uint8_t booster[7];
    uint8_t timing[2];
    uint8_t gate_start[4];
};

/* A fixed owner-selected pair. Temperatures describe provenance, not a live sensor
 * reading or automatic thermal qualification. Keep the pair fixed for both passes. */
struct hanshow_qn_waveforms {
    uint16_t panel_id;
    uint8_t band;
    int16_t min_centi;
    int16_t max_centi;
    const uint8_t *pass[2];
    uint8_t family;
    const struct hanshow_qn_initializer *initializer[2];
};

/* Generated private provider overrides the driver's weak, unavailable default. */
const struct hanshow_qn_waveforms *hanshow_qn_waveforms_for(uint16_t panel_id);

bool hanshow_qn_available(uint16_t panel_id);
uint32_t hanshow_qn_wire_size(uint16_t panel_id);
/* Low-voltage sensor probe; no waveform, PON or refresh. Caller configures epd_io
 * and owns its stay-awake hold. Output is unchanged if validation fails. */
bool hanshow_qn_read_temperature(uint16_t panel_id, int16_t *centi_c,
                                 bool (*keep_running)(void));
/* Caller configures epd_io and owns its stay-awake hold. Driver owns GPIO init/
 * uninit through both passes. NULL keep_running permits disconnected boot output. */
bool hanshow_qn_begin(uint16_t panel_id, bool (*keep_running)(void));
bool hanshow_qn_write(uint32_t offset, od_span_t data);
bool hanshow_qn_finish(void);
void hanshow_qn_abort(void);
bool hanshow_qn_active(void);

#endif
