/* Independently authored register sequence from recovered hardware observations.
 * Images arrive in controller component order; the MCU retains no image plane. */
#include "hanshow_qn.h"
#include "EPD_driver.h"
#include "od_hal_time.h"
#include "opendisplay_structs.h"
#include "tlsr_port.h"

static struct {
    const struct hanshow_qn_waveforms *wave;
    bool (*keep_running)(void);
    uint32_t offset;
    uint32_t component_bytes;
    uint16_t width;
    uint16_t height;
    bool active;
    bool powered;
} s_qn;

__attribute__((weak))
const struct hanshow_qn_waveforms *hanshow_qn_waveforms_for(uint16_t panel_id)
{
    (void)panel_id;
    return NULL;
}

uint32_t hanshow_qn_wire_size(uint16_t panel_id)
{
    if (panel_id == OD_PANEL_IC_HANSHOW_PRO154QN) return 20000u;
    if (panel_id == OD_PANEL_IC_HANSHOW_PRO266QN) return 22496u;
    return 0u;
}

static bool valid_pair(uint16_t panel_id, const struct hanshow_qn_waveforms *wave)
{
    if (hanshow_qn_wire_size(panel_id) == 0u || wave == NULL ||
        wave->panel_id != panel_id || wave->pass[0] == NULL || wave->pass[1] == NULL ||
        wave->min_centi > wave->max_centi) return false;
    if (wave->family == HANSHOW_QN_FAMILY_0ENSPQ1YQ) {
        return panel_id == OD_PANEL_IC_HANSHOW_PRO266QN && wave->band == 5u &&
               wave->initializer[0] != NULL && wave->initializer[1] != NULL;
    }
    return wave->family == HANSHOW_QN_FAMILY_LEGACY &&
           (wave->band == 6u || (panel_id == OD_PANEL_IC_HANSHOW_PRO154QN && wave->band == 7u));
}

bool hanshow_qn_available(uint16_t panel_id)
{
    return valid_pair(panel_id, hanshow_qn_waveforms_for(panel_id));
}

bool hanshow_qn_active(void)
{
    return s_qn.active;
}

void hanshow_qn_abort(void)
{
    if (s_qn.powered) EPD_GPIO_Uninit();
    s_qn.powered = false;
    s_qn.active = false;
    s_qn.wave = NULL;
    s_qn.keep_running = NULL;
}

static bool service(void)
{
    tlsr_port_service_stack();
    (void)od_hal_uptime_ms();
    return s_qn.keep_running == NULL || s_qn.keep_running();
}

static bool pause_ms(uint32_t duration)
{
    uint32_t start = od_hal_uptime_ms();
    do {
        tlsr_port_delay_us(1000u);
        if (!service()) return false;
    } while ((uint32_t)(od_hal_uptime_ms() - start) < duration);
    return true;
}

/* BUSY is high when ready. Minimum settle time prevents sampling the pre-command
 * ready level before the controller asserts BUSY. Unsigned uptime survives wrap. */
static bool ready(uint32_t settle_ms, uint32_t deadline_ms)
{
    uint32_t start = od_hal_uptime_ms();
    if (!pause_ms(settle_ms)) return false;
    while (!EPD_ReadBusy()) {
        if ((uint32_t)(od_hal_uptime_ms() - start) >= deadline_ms) return false;
        if (!pause_ms(1u)) return false;
    }
    return service();
}

static void reg8(uint8_t command, uint8_t value)
{
    EPD_WriteCmd(command);
    EPD_WriteByte(value);
}

static bool reset_panel(uint32_t power_settle_ms)
{
    EPD_GPIO_Init();
    s_qn.powered = true;
    if (!pause_ms(power_settle_ms)) return false;
    epd_io_reset_write(true);
    if (!pause_ms(2u)) return false;
    epd_io_reset_write(false);
    if (!pause_ms(20u)) return false;
    epd_io_reset_write(true);
    if (!pause_ms(2u)) return false;
    epd_io_reset_write(false);
    if (!pause_ms(20u)) return false;
    epd_io_reset_write(true);
    return ready(2u, 400u);
}

bool hanshow_qn_read_temperature(uint16_t panel_id, int16_t *centi_c,
                                 bool (*keep_running)(void))
{
    uint8_t revision[3], raw[2];
    uint32_t start;
    bool asserted = false, result = false;
    int16_t temperature;
    if (s_qn.active || centi_c == NULL || hanshow_qn_wire_size(panel_id) == 0u) return false;
    s_qn.active = true;
    s_qn.keep_running = keep_running;
    if (!service() || !reset_panel(110u)) goto cleanup;
    EPD_WriteCmd(0x70u);
    EPD_ReadData(revision, sizeof(revision));
    /* Recognized readback is a profile check, not a controller part identity. */
    if (revision[1] != 0x02u || revision[2] != 0x01u ||
        (panel_id == OD_PANEL_IC_HANSHOW_PRO154QN ? revision[0] != 0x08u :
         (revision[0] != 0x03u && revision[0] != 0x83u))) goto cleanup;
    EPD_WriteCmd(0x40u);
    start = od_hal_uptime_ms();
    for (;;) {
        bool idle = EPD_ReadBusy();
        uint32_t elapsed = (uint32_t)(od_hal_uptime_ms() - start);
        if (!idle) asserted = true;
        if (asserted && idle && elapsed <= 400u) break;
        if (elapsed >= 400u || !pause_ms(1u)) goto cleanup;
    }
    EPD_ReadData(raw, sizeof(raw));
    if (!service()) goto cleanup;
    temperature = (int16_t)((raw[0] < 128u ? (int16_t)raw[0] : (int16_t)raw[0] - 256) * 100 +
                            (raw[1] >> 6) * 25);
    if (temperature < -2500 || temperature > 5000) goto cleanup;
    *centi_c = temperature;
    result = true;
cleanup:
    hanshow_qn_abort();
    return result;
}

static bool initialize(unsigned component)
{
    const uint8_t *wave = s_qn.wave->pass[component];
    const struct hanshow_qn_initializer *override = s_qn.wave->initializer[component];
    bool small = s_qn.width == 200u;
    if (!reset_panel(5u)) return false;

    if (!small) {
        reg8(0x4du, 0x78u);
        reg8(0xaeu, 0x0fu);
        reg8(0xb6u, 0x0fu);
        reg8(0xbau, 0x2au);
    }
    EPD_Write(0x00u, 0x07u, 0xa8u);
    EPD_WriteCmd(0x01u);
    EPD_WriteByte(0x07u);
    for (unsigned i = 0u; i < 5u; ++i) EPD_WriteByte(wave[i]);
    EPD_Write(0x03u, 0x10u, 0x54u, 0x44u);
    if (override != NULL) {
        EPD_WriteCmd(0x06u);
        for (unsigned i = 0u; i < sizeof(override->booster); ++i) EPD_WriteByte(override->booster[i]);
    } else {
        EPD_Write(0x06u, 0x0fu, 0x0au, 0x2fu, 0x25u, 0x22u, small ? 0x2bu : 0x2eu, 0x21u);
    }
    reg8(0x30u, wave[6]);
    reg8(0x50u, s_qn.wave->family == HANSHOW_QN_FAMILY_0ENSPQ1YQ && component == 1u ? 0x97u : 0x37u);
    reg8(0xf0u, small ? 0x7du : 0x5fu);
    if (override != NULL) {
        EPD_WriteCmd(0x60u);
        for (unsigned i = 0u; i < sizeof(override->timing); ++i) EPD_WriteByte(override->timing[i]);
    } else {
        EPD_Write(0x60u, 0x02u, 0x02u);
    }
    EPD_Write(0x61u, (uint8_t)(s_qn.width >> 8), (uint8_t)(s_qn.width & 0xffu),
              (uint8_t)(s_qn.height >> 8), (uint8_t)(s_qn.height & 0xffu));
    if (override != NULL) {
        EPD_WriteCmd(0x65u);
        for (unsigned i = 0u; i < sizeof(override->gate_start); ++i) EPD_WriteByte(override->gate_start[i]);
    } else {
        EPD_Write(0x65u, 0u, 0u, 0u, 0u);
    }
    reg8(0x82u, wave[5] ^ 0x80u);
    reg8(0xe3u, 0x22u);
    reg8(0xe7u, 0x1cu);
    reg8(0xe9u, 0x01u);
    EPD_WriteCmd(0x20u);
    for (unsigned i = 0u; i < HANSHOW_QN_WAVE_BYTES; ++i) {
        EPD_WriteByte(wave[i]);
        if ((i & 63u) == 63u && !service()) return false;
    }
    EPD_WriteCmd(0x10u);
    return service();
}

static bool refresh_and_down(void)
{
    EPD_WriteCmd(0x04u);
    if (!ready(1u, 2000u)) return false;
    reg8(0x12u, 0u);
    if (!ready(10u, 200000u)) return false;
    if (s_qn.wave->family == HANSHOW_QN_FAMILY_0ENSPQ1YQ) EPD_Write(0x00u, 0x27u, 0xa9u);
    reg8(0x02u, 0u);
    if (!ready(1u, 400u)) return false;
    reg8(0x07u, 0xa5u);
    EPD_GPIO_Uninit();
    s_qn.powered = false;
    return true;
}

bool hanshow_qn_begin(uint16_t panel_id, bool (*keep_running)(void))
{
    const struct hanshow_qn_waveforms *wave = hanshow_qn_waveforms_for(panel_id);
    bool result = false;
    if (s_qn.active || !valid_pair(panel_id, wave)) return false;
    s_qn.wave = wave;
    s_qn.keep_running = keep_running;
    s_qn.offset = 0u;
    s_qn.component_bytes = hanshow_qn_wire_size(panel_id) / 2u;
    s_qn.width = panel_id == OD_PANEL_IC_HANSHOW_PRO154QN ? 200u : 152u;
    s_qn.height = panel_id == OD_PANEL_IC_HANSHOW_PRO154QN ? 200u : 296u;
    s_qn.active = true;
    if (!service() || !initialize(0u)) goto cleanup;
    result = true;
cleanup:
    if (!result) hanshow_qn_abort();
    return result;
}

bool hanshow_qn_write(uint32_t offset, od_span_t data)
{
    bool result = false;
    uint32_t total = 2u * s_qn.component_bytes;
    if (!s_qn.active) return false;
    if (!od_span_valid(data) || offset != s_qn.offset ||
        offset > total || data.n > total - offset) goto cleanup;
    if (!service()) goto cleanup;
    for (size_t i = 0u; i < data.n; ++i) {
        EPD_WriteByte(data.p[i]);
        ++s_qn.offset;
        if (s_qn.offset == s_qn.component_bytes) {
            if (!refresh_and_down() || !initialize(1u)) goto cleanup;
        }
        if ((i & 63u) == 63u && !service()) goto cleanup;
    }
    if (!service()) goto cleanup;
    result = true;
cleanup:
    if (!result) hanshow_qn_abort();
    return result;
}

bool hanshow_qn_finish(void)
{
    bool result = false;
    if (!s_qn.active) return false;
    if (s_qn.offset != 2u * s_qn.component_bytes || !service()) goto cleanup;
    result = refresh_and_down();
cleanup:
    hanshow_qn_abort();
    return result;
}
