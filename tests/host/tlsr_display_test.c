/* tlsr_display_test.c -- targets/telink-tlsr's image path, driver included, down to the wire.
 *
 * Runs od_xfer_tlsr.c's hooks over the imported Firmware_NRF52 drivers (UC81xx.c, SSD16xx.c,
 * epd_models.c) and the bit-banged epd_io.c, against a fake tlsr_port that decodes CS/SCLK/MOSI/DC
 * back into (command | data, byte) records. So a pass means the bytes a panel controller would
 * clock in are the right ones: every image byte, in the right plane, after the right RAM command,
 * followed by a refresh and deep sleep.
 *
 * What it cannot prove: the TLSR825x GPIO registers, bit timing against a real controller, or the
 * busy line's real polarity on a given tag. Those need hardware. */

#include "od_xfer_app.h"

#include "EPD_driver.h"
#include "hanshow_qn.h"
#include "od_check.h"
#include "od_boot_screen.h"
#include "od_boot_app.h"
#include "od_config.h"
#include "od_tlsr.h"
#include "od_txq.h"
#include "od_zlib_inflate.h"
#include "od_zlib_pump.h"
#include "epd_port.h"
#include "tlsr_port.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --------------------------------------------------------------- fake SPI wire decoder --- */

enum { P_MOSI = 1, P_SCLK = 2, P_CS = 3, P_DC = 4, P_RST = 5, P_BUSY = 6, P_PWR = 7, P_CS2 = 8 };

#define LOG_MAX 200000u
static struct { uint8_t dc, b, cs; } s_log[LOG_MAX];   /* cs: bit 0 = P_CS low, bit 1 = P_CS2 low */
static uint32_t s_log_n;
static uint8_t  s_level[32];
static uint8_t  s_shift, s_bits;
static bool     s_busy_idle_level;     /* level the busy line reads when the panel is idle */
static bool     s_busy_stuck;          /* never go idle: exercises the timeout path */
static int      s_pwr_at_first_byte;   /* P_PWR level when the first SPI byte completed, -1 = none */
static uint64_t s_clock_us;
static uint32_t s_services;
static uint32_t s_tag;
static bool s_connected;
static bool s_disconnect_on_refresh;
static bool s_connect_on_refresh;
static uint32_t s_refresh_hold_ms;
static uint32_t s_busy_until_ms;
static bool     s_pwr_driven;          /* P_PWR is an output (not released to float) */
static bool s_mosi_input;
static bool s_qn_sensor;
static uint8_t s_qn_revision[3], s_qn_temperature[2];
static const uint8_t *s_sensor_read;
static size_t s_sensor_read_bytes, s_sensor_read_bit;
static bool s_conversion_issued, s_conversion_never_assert, s_conversion_stuck;
static bool s_disconnect_on_conversion;
static uint32_t s_conversion_start_ms, s_conversion_delay_ms, s_conversion_hold_ms;

void tlsr_port_gpio_output(uint8_t pin, bool level)
{
    if (pin == P_PWR) s_pwr_driven = true;
    if (pin == P_MOSI) s_mosi_input = false;
    tlsr_port_gpio_write(pin, level);
}
void tlsr_port_gpio_input(uint8_t pin, uint8_t pull)
{
    (void)pull;
    if (pin == P_PWR) s_pwr_driven = false;
    if (pin == P_MOSI) s_mosi_input = true;
}
void tlsr_port_gpio_release(uint8_t pin) { if (pin == P_PWR) s_pwr_driven = false; }

void tlsr_port_gpio_write(uint8_t pin, bool level)
{
    if (pin >= 32u) return;
    if (pin == P_RST && !level) s_conversion_issued = false;
    if ((pin == P_CS || pin == P_CS2) && level && s_bits != 0u) {
        s_bits = 0u;                               /* partial byte discarded at CS high */
    }
    if (pin == P_SCLK && level && !s_level[P_SCLK] && !s_mosi_input && (!s_level[P_CS] || !s_level[P_CS2])) {
        s_shift = (uint8_t)((s_shift << 1) | (s_level[P_MOSI] ? 1u : 0u));
        if (++s_bits == 8u) {
            if (s_log_n == 0u) s_pwr_at_first_byte = s_level[P_PWR];
            if (s_log_n < LOG_MAX) {
                s_log[s_log_n].dc = s_level[P_DC];
                s_log[s_log_n].b = s_shift;
                s_log[s_log_n].cs = (uint8_t)((s_level[P_CS] ? 0u : 1u) | (s_level[P_CS2] ? 0u : 2u));
                s_log_n++;
                if (s_qn_sensor && s_level[P_DC] == 0u && (s_shift == 0x70u || s_shift == 0x40u)) {
                    s_sensor_read = s_shift == 0x70u ? s_qn_revision : s_qn_temperature;
                    s_sensor_read_bytes = s_shift == 0x70u ? sizeof(s_qn_revision) : sizeof(s_qn_temperature);
                    s_sensor_read_bit = 0u;
                    if (s_shift == 0x40u) {
                        s_conversion_issued = true;
                        s_conversion_start_ms = (uint32_t)(s_clock_us / 1000u);
                        if (s_disconnect_on_conversion) { s_connected = false; ++s_tag; }
                    }
                }
                if (s_level[P_DC] == 0u && s_shift == 0x12u) {
                    s_busy_until_ms = (uint32_t)(s_clock_us / 1000u) + s_refresh_hold_ms;
                    if (s_disconnect_on_refresh) { s_connected = false; ++s_tag; }
                    if (s_connect_on_refresh) { s_connected = true; ++s_tag; }
                }
            }
            s_bits = 0u;
        }
    }
    s_level[pin] = level ? 1u : 0u;
}

/* The port's fast SPI path, replayed as the same edges so the decoder above sees every bit. */
void tlsr_port_spi_out(uint8_t mosi, uint8_t sclk, const uint8_t *buf, uint32_t n)
{
    while (n-- != 0u) {
        uint8_t b = *buf++, bit;
        for (bit = 0x80u; bit != 0u; bit >>= 1) {
            tlsr_port_gpio_write(mosi, (b & bit) != 0u);
            tlsr_port_gpio_write(sclk, true);
            tlsr_port_gpio_write(sclk, false);
        }
    }
}

/* The panel's temperature register, clocked out MSB first on MOSI when the driver reads. */
static uint8_t s_temp_byte = 23u;
static uint8_t s_read_bit;

bool tlsr_port_gpio_read(uint8_t pin)
{
    if (pin == P_BUSY && s_conversion_issued && !s_conversion_never_assert) {
        uint32_t elapsed = (uint32_t)(s_clock_us / 1000u) - s_conversion_start_ms;
        if (elapsed >= s_conversion_delay_ms &&
            (s_conversion_stuck || elapsed < s_conversion_delay_ms + s_conversion_hold_ms)) return false;
    }
    if (pin == P_BUSY && (uint32_t)(s_clock_us / 1000u) < s_busy_until_ms) return !s_busy_idle_level;
    if (pin == P_BUSY) return s_busy_stuck ? !s_busy_idle_level : s_busy_idle_level;
    if (pin == P_MOSI) {
        if (s_qn_sensor && s_sensor_read != NULL) {
            size_t bit = s_sensor_read_bit++;
            return bit / 8u < s_sensor_read_bytes &&
                   ((s_sensor_read[bit / 8u] >> (7u - bit % 8u)) & 1u) != 0u;
        }
        return ((s_temp_byte >> (7u - (s_read_bit++ & 7u))) & 1u) != 0u;
    }
    return false;
}

void tlsr_port_delay_us(uint32_t us) { s_clock_us += us; }
void tlsr_port_mac(uint8_t out[6]) { static const uint8_t m[6] = {0x92, 0xa9, 0x80, 1, 2, 3}; memcpy(out, m, 6); }
void tlsr_port_service_stack(void) { ++s_services; }
bool tlsr_port_connected(void) { return s_connected; }
uint32_t od_tlsr_link_tag(void) { return s_tag; }
void od_core_reset(void) { od_xfer_app_abort(OD_XFER_ABORT_RESET); }
static int s_awake_depth;              /* the panel's hold must always be released */
void tlsr_port_stay_awake(uint8_t holder, bool on)
{
    if (holder == TLSR_PORT_AWAKE_PANEL) s_awake_depth = on ? 1 : 0;
}

/* ------------------------------------------------------------------ other link stubs --- */

static struct od_config s_cfg;
static int s_temp_reported = -128;
void od_tlsr_set_temperature(int8_t c) { s_temp_reported = c; }
float od_tlsr_battery_volts(void) { return -1.0f; }
float od_tlsr_temperature_c(void) { return -1000.0f; }
const struct od_config *od_tlsr_config(void) { return &s_cfg; }
uint32_t od_hal_uptime_ms(void) { return (uint32_t)(s_clock_us / 1000u); }
od_txq_status_t od_txq_flush(uint32_t now_ms, uint32_t deadline_ms)
{
    (void)now_ms; (void)deadline_ms;
    return OD_TXQ_OK;
}
/* ----------------------------------------------------------------------------- helpers --- */

static void set_panel(uint16_t ic, uint16_t w, uint16_t h, uint8_t scheme)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.display_count = 1u;
    s_cfg.displays[0].panel_ic_type = ic;
    s_cfg.displays[0].pixel_width = w;
    s_cfg.displays[0].pixel_height = h;
    s_cfg.displays[0].color_scheme = scheme;
    s_cfg.displays[0].data_pin = P_MOSI;
    s_cfg.displays[0].clk_pin = P_SCLK;
    s_cfg.displays[0].cs_pin = P_CS;
    s_cfg.displays[0].dc_pin = P_DC;
    s_cfg.displays[0].reset_pin = P_RST;
    s_cfg.displays[0].busy_pin = P_BUSY;
    s_cfg.system_config.pwr_pin = 0xFFu;
    memset(s_level, 0, sizeof(s_level));
    s_level[P_CS] = 1u;
    s_level[P_CS2] = 1u;
    s_log_n = 0u;
    s_bits = 0u;
    s_busy_stuck = false;
    s_clock_us = 0u; s_services = 0u; s_busy_until_ms = 0u;
    s_refresh_hold_ms = 0u; s_disconnect_on_refresh = false; s_connect_on_refresh = false; s_connected = false;
    s_pwr_at_first_byte = -1;
    s_pwr_driven = false;
    s_mosi_input = false;
    s_qn_sensor = false; s_sensor_read = NULL;
    s_conversion_issued = false; s_conversion_never_assert = false; s_conversion_stuck = false;
    s_disconnect_on_conversion = false;
    s_conversion_delay_ms = 2u; s_conversion_hold_ms = 3u;
    s_temp_reported = -128;
}

/* Index of the n-th (0-based) command byte `cmd` in the log, or -1. */
static long find_cmd(uint8_t cmd, unsigned nth)
{
    uint32_t i;
    for (i = 0; i < s_log_n; ++i) {
        if (s_log[i].dc == 0u && s_log[i].b == cmd && nth-- == 0u) return (long)i;
    }
    return -1;
}

/* True when command `cmd` occurs anywhere after log index `after` (the refresh after the image;
 * SSD panels also issue an activation before it, to load the temperature). */
static bool cmd_after(uint8_t cmd, long after)
{
    unsigned nth;
    long at;
    for (nth = 0; (at = find_cmd(cmd, nth)) >= 0; ++nth) {
        if (at > after) return true;
    }
    return false;
}

/* The data bytes following the command at `at`, up to the next command. */
static uint32_t data_after(long at, const uint8_t **unused, uint8_t *out, uint32_t cap)
{
    uint32_t n = 0u, i;
    (void)unused;
    for (i = (uint32_t)at + 1u; i < s_log_n && s_log[i].dc == 1u; ++i) {
        if (n < cap) out[n] = s_log[i].b;
        n++;
    }
    return n;
}

static uint8_t s_img[170000];
static uint8_t s_got[170000];

static void fill(uint32_t n, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n; ++i) s_img[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
}

/* Stream `n` bytes as od_xfer would: chunks of `chunk`, running offset. */
static bool stream(uint32_t n, uint32_t chunk)
{
    uint32_t off = 0u;
    while (off < n) {
        uint32_t k = (n - off) < chunk ? (n - off) : chunk;
        if (od_xfer_app_write(off, od_span_make(s_img + off, k)) != k) return false;
        off += k;
    }
    return true;
}

static bool begin(void)
{
    od_xfer_panel_info_t info;
    if (!od_xfer_app_panel_info(&info)) return false;
    od_xfer_app_prepare_start();
    return od_xfer_app_begin_full(&info.geometry);
}

/* ------------------------------------------------------------------------------ cases --- */

static void test_uc8176_bwr(void)
{
    bool completed = false;
    long dtm1, dtm2;

    CASE("UC8176 4.2 BWR: both planes, refresh, sleep");
    set_panel(1002u, 400u, 300u, OD_COLOR_SCHEME_BWR);
    s_busy_idle_level = true;                        /* UC81xx: busy is active low */
    fill(30000u, 7u);
    CHECK(begin());
    CHECK(stream(30000u, 244u));
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(completed);

    dtm1 = find_cmd(UC81xx_DTM1, 0u);
    dtm2 = find_cmd(UC81xx_DTM2, 0u);
    CHECK(dtm1 >= 0 && dtm2 > dtm1);
    CHECK(data_after(dtm1, NULL, s_got, sizeof(s_got)) == 15000u);
    CHECK(memcmp(s_got, s_img, 15000u) == 0);
    CHECK(data_after(dtm2, NULL, s_got, sizeof(s_got)) == 15000u);
    CHECK(memcmp(s_got, s_img + 15000u, 15000u) == 0);
    CHECK(find_cmd(UC81xx_DRF, 0u) > dtm2);           /* refresh after the image */
    CHECK(find_cmd(UC81xx_DSLP, 0u) > find_cmd(UC81xx_DRF, 0u));
    CHECK(find_cmd(UC81xx_DTM1, 1u) < 0);             /* each plane opened exactly once */
    CHECK(find_cmd(UC81xx_DTM2, 1u) < 0);
}

static void test_ssd1619_bw_odd_chunks(void)
{
    bool completed = false;
    long ram;

    CASE("SSD1619 4.2 BW: one plane, odd chunking");
    set_panel(1003u, 400u, 300u, OD_COLOR_SCHEME_MONO);
    s_busy_idle_level = false;                       /* SSD16xx: busy is active high */
    fill(15000u, 99u);
    CHECK(begin());
    CHECK(stream(15000u, 97u));                      /* crosses every 255-byte driver chunk */
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(completed);

    ram = find_cmd(SSD16xx_WRITE_RAM1, 0u);
    CHECK(ram >= 0);
    CHECK(data_after(ram, NULL, s_got, sizeof(s_got)) == 15000u);
    CHECK(memcmp(s_got, s_img, 15000u) == 0);
    CHECK(find_cmd(SSD16xx_WRITE_RAM1, 1u) < 0);
    CHECK(find_cmd(SSD16xx_WRITE_RAM2, 0u) < 0);
    CHECK(cmd_after(SSD16xx_MASTER_ACTIVATE, ram));
}

static void test_plane_boundary_in_one_write(void)
{
    long dtm2;

    CASE("one write spanning the plane boundary is split at it");
    set_panel(1002u, 400u, 300u, OD_COLOR_SCHEME_BWR);
    s_busy_idle_level = true;
    fill(30000u, 3u);
    CHECK(begin());
    CHECK(stream(14990u, 244u));
    CHECK(od_xfer_app_write(14990u, od_span_make(s_img + 14990u, 20u)) == 20u);
    CHECK(stream(0u, 1u));
    dtm2 = find_cmd(UC81xx_DTM2, 0u);
    CHECK(dtm2 >= 0);
    CHECK(data_after(dtm2, NULL, s_got, sizeof(s_got)) == 10u);
    CHECK(memcmp(s_got, s_img + 15000u, 10u) == 0);
    od_xfer_app_abort(OD_XFER_ABORT_RESET);
}

static void test_active_low_panel_power(void)
{
    bool completed = false;

    CASE("panel power: parked off, asserted LOW while driving, driven off (not floated) after");
    set_panel(1022u, 296u, 152u, OD_COLOR_SCHEME_BWR);
    s_cfg.system_config.pwr_pin = P_PWR;
    s_busy_idle_level = false;
    epd_io_park_power(P_PWR);
    CHECK(s_pwr_driven && s_level[P_PWR] == 1u);       /* off before any transfer */
    fill(2u * (296u / 8u) * 152u, 11u);                 /* two 37-byte x 152-row planes */
    CHECK(begin());
    CHECK(stream(2u * (296u / 8u) * 152u, 244u));
    CHECK(s_pwr_at_first_byte == 0);                    /* powered while bytes were clocked in */
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(completed);
    CHECK(s_pwr_driven && s_level[P_PWR] == 1u);       /* off, and still an output */
}

/* Hanshow 2.66" (ATC type 9): 152 sources x 296 gates. The Solum 2.6" model has the same size
 * transposed; streaming 296-pixel rows into this glass garbled it on hardware. */
static void test_hanshow_266_window(void)
{
    bool completed = false;
    long xpos, ram1, ram2;
    uint32_t plane = (152u / 8u) * 296u;

    CASE("Hanshow 2.66 BWR: 19-byte source window at offset 1, both planes");
    set_panel(1031u, 152u, 296u, OD_COLOR_SCHEME_BWR);
    s_temp_reported = -128;
    s_read_bit = 0u;
    s_busy_idle_level = false;
    fill(2u * plane, 5u);
    CHECK(begin());
    CHECK(stream(2u * plane, 244u));
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(completed);
    CHECK(s_temp_reported == 23);                   /* controller temperature read and reported */

    xpos = find_cmd(SSD16xx_RAM_XPOS, 0u);
    CHECK(xpos >= 0 && s_log[xpos + 1].b == 0x01u && s_log[xpos + 2].b == 0x13u);
    /* Verified on the tag: Y increments (else the landscape image is mirrored) and the B/W RAM is
     * not inverted (else black and white swap; ATC reports black_invert for this glass). */
    CHECK(find_cmd(SSD16xx_ENTRY_MODE, 0u) >= 0 && s_log[find_cmd(SSD16xx_ENTRY_MODE, 0u) + 1].b == 0x03u);
    CHECK(find_cmd(SSD16xx_DISP_CTRL1, 0u) >= 0 && s_log[find_cmd(SSD16xx_DISP_CTRL1, 0u) + 1].b == 0x00u);
    ram1 = find_cmd(SSD16xx_WRITE_RAM1, 0u);
    ram2 = find_cmd(SSD16xx_WRITE_RAM2, 0u);
    CHECK(ram1 >= 0 && ram2 > ram1);
    CHECK(data_after(ram1, NULL, s_got, sizeof(s_got)) == plane);
    CHECK(memcmp(s_got, s_img, plane) == 0);
    CHECK(data_after(ram2, NULL, s_got, sizeof(s_got)) == plane);
    CHECK(memcmp(s_got, s_img + plane, plane) == 0);

    CASE("the Solum 2.6 orientation is refused for this panel number");
    set_panel(1031u, 296u, 152u, OD_COLOR_SCHEME_BWR);
    CHECK(!begin());
}

/* Hanshow BWY (ATC type 5): 200 sources x 152 gates, no offset, yellow in the second plane. ATC
 * reports it as 152x200; driven that way, only the 152x152 overlap of the two layouts reached glass. */
static void test_hanshow_200_bwy_window(void)
{
    bool completed = false;
    long xpos, ram1, ram2;
    uint32_t plane = (200u / 8u) * 152u;

    CASE("Hanshow BWY: 25-byte source window at offset 0, yellow as the second plane");
    set_panel(1032u, 200u, 152u, OD_COLOR_SCHEME_BWY);
    s_busy_idle_level = false;
    fill(2u * plane, 7u);
    CHECK(begin());
    CHECK(stream(2u * plane, 244u));
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(completed);

    xpos = find_cmd(SSD16xx_RAM_XPOS, 0u);
    CHECK(xpos >= 0 && s_log[xpos + 1].b == 0x00u && s_log[xpos + 2].b == 0x18u);
    CHECK(find_cmd(SSD16xx_GDO_CTR, 0u) >= 0 && s_log[find_cmd(SSD16xx_GDO_CTR, 0u) + 1].b == 152u);
    /* Verified on the tag: Y increments (else mirrored); B/W polarity as on the 2.66". */
    CHECK(find_cmd(SSD16xx_ENTRY_MODE, 0u) >= 0 && s_log[find_cmd(SSD16xx_ENTRY_MODE, 0u) + 1].b == 0x03u);
    CHECK(find_cmd(SSD16xx_DISP_CTRL1, 0u) >= 0 && s_log[find_cmd(SSD16xx_DISP_CTRL1, 0u) + 1].b == 0x00u);
    ram1 = find_cmd(SSD16xx_WRITE_RAM1, 0u);
    ram2 = find_cmd(SSD16xx_WRITE_RAM2, 0u);
    CHECK(ram1 >= 0 && ram2 > ram1);
    CHECK(data_after(ram1, NULL, s_got, sizeof(s_got)) == plane);
    CHECK(memcmp(s_got, s_img, plane) == 0);
    CHECK(data_after(ram2, NULL, s_got, sizeof(s_got)) == plane);
    CHECK(memcmp(s_got, s_img + plane, plane) == 0);

    CASE("the same glass declared BWR is still accepted (both are two-plane)");
    set_panel(1032u, 200u, 152u, OD_COLOR_SCHEME_BWR);
    CHECK(begin());

    CASE("ATC's transposed 152x200 is refused");
    set_panel(1032u, 152u, 200u, OD_COLOR_SCHEME_BWY);
    CHECK(!begin());
}

/* Hanshow Nebular 350Y-N (ATC type 1): UC8151-class, 184 x 384, yellow in the second plane. */
static void test_hanshow_350_bwy(void)
{
    bool completed = false;
    long tres, dtm1, dtm2;
    uint32_t plane = (184u / 8u) * 384u;

    CASE("Hanshow 350 BWY: UC8151 resolution 184 x 384, both planes");
    set_panel(1033u, 184u, 384u, OD_COLOR_SCHEME_BWY);
    s_busy_idle_level = true;
    fill(2u * plane, 11u);
    CHECK(begin());
    CHECK(stream(2u * plane, 244u));
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(completed);
    tres = find_cmd(UC81xx_TRES, 0u);
    CHECK(tres >= 0 && s_log[tres + 1].b == 184u && s_log[tres + 2].b == 0x01u && s_log[tres + 3].b == 0x80u);
    dtm1 = find_cmd(UC81xx_DTM1, 0u);
    dtm2 = find_cmd(UC81xx_DTM2, 0u);
    CHECK(dtm1 >= 0 && dtm2 > dtm1);
    CHECK(data_after(dtm1, NULL, s_got, sizeof(s_got)) == plane);
    CHECK(memcmp(s_got, s_img, plane) == 0);
    CHECK(data_after(dtm2, NULL, s_got, sizeof(s_got)) == plane);
    CHECK(memcmp(s_got, s_img + plane, plane) == 0);
    CHECK(cmd_after(UC81xx_DRF, dtm2));
}

/* 9.7" dual-controller BWR (ATC type 14): each 120-byte row splits 60 / 60 across the two
 * chip-selects; plane 0 is inverted (the controller takes 1 = black), plane 1 goes as is. */
static void check_ti_plane(uint8_t cmd, const uint8_t *src, bool invert, bool reversed)
{
    const uint32_t plane = 120u * 672u;
    long at = find_cmd(cmd, 0u);
    uint32_t i, n = 0u, bad = 0u;

    CHECK(at >= 0 && s_log[at].cs == 3u);                  /* command to both halves */
    for (i = (uint32_t)at + 1u; i < s_log_n && s_log[i].dc == 1u && n < plane; ++i, ++n) {
        uint32_t col = n % 120u;
        uint8_t want = invert ? (uint8_t)~src[n] : src[n];
        if (reversed) {
            uint8_t r = 0u, b;
            for (b = 0u; b < 8u; b++) if (want & (1u << b)) r |= (uint8_t)(0x80u >> b);
            want = r;
        }
        if (s_log[i].b != want || s_log[i].cs != (col < 60u ? 1u : 2u)) bad++;
    }
    CHECK(n == plane);
    CHECK(bad == 0u);
}

static void test_ti_970(void)
{
    bool completed = false;
    const uint32_t plane = 120u * 672u;

    CASE("TI 9.7 BWR: OTP read, rows split 60 / 60 across cs and cs2, both planes");
    set_panel(1034u, 960u, 672u, OD_COLOR_SCHEME_BWR);
    s_cfg.displays[0].cs_pin_2 = P_CS2;
    s_cfg.system_config.pwr_pin_2 = 0xFFu;
    s_busy_idle_level = true;                          /* busy is low while refreshing */
    s_temp_byte = 0x23u;                               /* OTP reads back non-blank */
    s_read_bit = 0u;
    fill(2u * plane, 13u);
    CHECK(begin());
    CHECK(find_cmd(0xB9, 0u) >= 0 && s_log[find_cmd(0xB9, 0u)].cs == 1u);   /* OTP from the master */
    CHECK(find_cmd(0x13, 0u) >= 0 && s_log[find_cmd(0x13, 0u) + 1].b == 0x23u);   /* OTP values used */
    CHECK(stream(2u * plane, 244u));
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(completed);
    check_ti_plane(0x10, s_img, true, false);
    check_ti_plane(0x11, s_img + plane, false, false);
    CHECK(cmd_after(0x15, find_cmd(0x11, 0u)));        /* refresh started after the image */
    CHECK(s_level[P_CS] == 0u && s_level[P_CS2] == 0u); /* powered down, lines parked low */

    CASE("TI 9.7: blank OTP falls back to ATC's 9.7 values and bit-reversed data");
    set_panel(1034u, 960u, 672u, OD_COLOR_SCHEME_BWR);
    s_cfg.displays[0].cs_pin_2 = P_CS2;
    s_cfg.system_config.pwr_pin_2 = 0xFFu;
    s_busy_idle_level = true;
    s_temp_byte = 0xFFu;
    s_read_bit = 0u;
    CHECK(begin());
    CHECK(find_cmd(0x13, 0u) >= 0 && s_log[find_cmd(0x13, 0u) + 6].b == 0x02u);   /* 0x1A default */
    CHECK(stream(2u * plane, 244u));
    CHECK(od_xfer_app_refresh(0u, &completed));
    check_ti_plane(0x10, s_img, true, true);
    s_temp_byte = 23u;

    CASE("cs_pin_2 is ignored for single-controller panels (0 there is PA0, not a pin)");
    set_panel(1031u, 152u, 296u, OD_COLOR_SCHEME_BWR);
    s_level[0] = 7u;                                   /* sentinel: never written */
    CHECK(begin());
    od_xfer_app_abort(OD_XFER_ABORT_REPLY_FAILED);
    CHECK(s_level[0] == 7u);
}

/* The boot screen through the real hooks, drivers and SPI, on the Hanshow 2.66" config. */
static void test_boot_screen(void)
{
    static uint8_t row[256], qr[256];
    struct od_boot_bufs bufs = { row, sizeof(row), qr, sizeof(qr) };
    uint32_t plane = (152u / 8u) * 296u, i, black = 0u;
    long ram1, ram2;

    CASE("boot screen renders both planes and refreshes");
    set_panel(1031u, 152u, 296u, OD_COLOR_SCHEME_BWR);
    s_busy_idle_level = false;
    CHECK(od_boot_screen_render(&s_cfg, NULL, &bufs));
    ram1 = find_cmd(SSD16xx_WRITE_RAM1, 0u);
    ram2 = find_cmd(SSD16xx_WRITE_RAM2, 0u);
    CHECK(ram1 >= 0 && ram2 > ram1);
    CHECK(data_after(ram1, NULL, s_got, sizeof(s_got)) == plane);
    for (i = 0; i < plane; i++) black += (uint32_t)(s_got[i] != 0xFFu);
    CHECK(black > plane / 20u);                     /* text and a QR code, not a blank frame */
    CHECK(black < plane);                           /* and not an all-black one */
    CHECK(data_after(ram2, NULL, s_got, sizeof(s_got)) == plane);
    CHECK(cmd_after(SSD16xx_MASTER_ACTIVATE, ram2));
    CHECK(find_cmd(SSD16xx_SLEEP_MODE, 0u) > ram2);  /* powered down afterwards */
    CHECK(s_awake_depth == 0);                       /* and suspend allowed again */

    CASE("a model mismatch refuses before drawing anything");
    set_panel(1031u, 296u, 152u, OD_COLOR_SCHEME_BWR);
    CHECK(!od_boot_screen_render(&s_cfg, NULL, &bufs));
    CHECK(find_cmd(SSD16xx_WRITE_RAM1, 0u) < 0);
}

/* Synthetic waveforms exercise register/data boundaries, not optical performance. */
static const uint8_t s_wave1[HANSHOW_QN_WAVE_BYTES] = {1, 2, 3, 4, 5, 6, 7};
static const uint8_t s_wave2[HANSHOW_QN_WAVE_BYTES] = {11, 12, 13, 14, 15, 16, 17};
static bool s_waves_available;
static bool s_new_family;
static const struct hanshow_qn_initializer s_qn_init[2] = {
    {{31, 32, 33, 34, 35, 36, 37}, {41, 42}, {51, 52, 53, 54}},
    {{61, 62, 63, 64, 65, 66, 67}, {71, 72}, {81, 82, 83, 84}}
};
static struct hanshow_qn_waveforms s_qn_new = {
    .panel_id = 1036, .band = 5, .min_centi = 2001, .max_centi = 2500,
    .pass = {s_wave1, s_wave2}, .family = HANSHOW_QN_FAMILY_0ENSPQ1YQ,
    .initializer = {&s_qn_init[0], &s_qn_init[1]}
};
const struct hanshow_qn_waveforms *hanshow_qn_waveforms_for(uint16_t panel)
{
    static const struct hanshow_qn_waveforms small = {
        .panel_id = 1035, .band = 7, .min_centi = 2501, .max_centi = 3000,
        .pass = {s_wave1, s_wave2}
    };
    static const struct hanshow_qn_waveforms large = {
        .panel_id = 1036, .band = 6, .min_centi = 2001, .max_centi = 2500,
        .pass = {s_wave1, s_wave2}
    };
    if (!s_waves_available) return NULL;
    if (s_new_family) return &s_qn_new;
    return panel == 1035u ? &small : panel == 1036u ? &large : NULL;
}

static bool qn_inflate_sink(void *context, od_mut_span_t bytes)
{
    uint32_t *offset = context;
    if (od_xfer_app_write(*offset, od_mut_span_const(bytes)) != bytes.n) return false;
    *offset += (uint32_t)bytes.n;
    return true;
}

static uint8_t asymmetric_drive_code(uint16_t x, uint16_t y)
{
    return (uint8_t)((3u * x + 5u * y + x / 7u) & 15u);
}

static void test_hanshow_boot_component_projection(void)
{
    uint8_t row[128];
    CASE("Q-N boot hooks preserve physical asymmetric pixels across both component streams");
    for (unsigned panel = 0u; panel < 2u; ++panel) {
        uint16_t width = panel == 0u ? 200u : 152u;
        uint16_t height = panel == 0u ? 200u : 296u;
        uint32_t component_bytes = (uint32_t)width * height / 4u;
        set_panel(panel == 0u ? 1035u : 1036u, width, height, OD_COLOR_SCHEME_INDEXED16_2PASS);
        s_busy_idle_level = true;
        s_waves_available = true;
        CHECK(od_boot_app_begin_frame(width, height, 1u) == 0);
        for (unsigned pass = 0u; pass < 2u; ++pass) {
            CHECK(od_boot_app_begin_plane(pass == 0u ? OD_BOOT_PLANE_PRIMARY : OD_BOOT_PLANE_SECOND) == 0);
            for (uint16_t line = 0u; line < height; ++line) {
                memset(row, 0, sizeof(row));
                for (uint16_t pos = 0u; pos < width; ++pos) {
                    uint16_t physical_x = panel == 0u ? line : pos;
                    uint16_t physical_y = panel == 0u ? pos : (uint16_t)(height - 1u - line);
                    row[pos / 2u] |= (uint8_t)(asymmetric_drive_code(physical_x, physical_y) <<
                                              (pos % 2u == 0u ? 4u : 0u));
                }
                CHECK(od_boot_app_write_row(line, 0u, row, width / 2u) == 0);
            }
            CHECK(od_boot_app_end_plane(pass == 0u ? OD_BOOT_PLANE_PRIMARY : OD_BOOT_PLANE_SECOND) == 0);
        }
        CHECK(od_boot_app_end_frame() == 0 && s_awake_depth == 0);
        CHECK(data_after(find_cmd(0x10u, 0u), NULL, s_got, sizeof(s_got)) == component_bytes);
        CHECK(data_after(find_cmd(0x10u, 1u), NULL, s_img, sizeof(s_img)) == component_bytes);
        for (uint16_t y = 0u; y < height; ++y) {
            for (uint16_t x = 0u; x < width; ++x) {
                uint32_t pixel = panel == 0u ? (uint32_t)x * height + y :
                                 (uint32_t)(height - 1u - y) * width + x;
                uint8_t shift = (uint8_t)(6u - 2u * (pixel % 4u));
                uint8_t code = (uint8_t)((((s_got[pixel / 4u] >> shift) & 3u) << 2) |
                                       ((s_img[pixel / 4u] >> shift) & 3u));
                CHECK(code == asymmetric_drive_code(x, y));
            }
        }
    }
    s_waves_available = false;
}

static void sensor_setup(uint16_t panel, uint8_t revision, uint8_t whole, uint8_t fraction)
{
    struct epd_io_pins pins = {P_MOSI, P_SCLK, P_CS, P_DC, P_RST, P_BUSY, P_PWR, 0xff, 0xff};
    set_panel(panel, panel == 1035u ? 200u : 152u, panel == 1035u ? 200u : 296u,
              OD_COLOR_SCHEME_INDEXED16_2PASS);
    s_cfg.system_config.pwr_pin = P_PWR;
    s_busy_idle_level = true;
    s_qn_sensor = true;
    s_qn_revision[0] = revision; s_qn_revision[1] = 2u; s_qn_revision[2] = 1u;
    s_qn_temperature[0] = whole; s_qn_temperature[1] = fraction;
    epd_io_configure(&pins);
}

static bool sensor_keep_connected(void) { return s_connected; }

static void test_hanshow_qn_temperature(void)
{
    int16_t centi_c;
    unsigned i;
    const uint8_t revision[] = {0x08u, 0x03u, 0x83u};
    CASE("Q-N standalone temperature validates revision and fresh BUSY without high voltage");
    for (i = 0u; i < sizeof(revision); ++i) {
        sensor_setup(i == 0u ? 1035u : 1036u, revision[i], 26u, 0x80u);
        CHECK(hanshow_qn_read_temperature(i == 0u ? 1035u : 1036u, &centi_c, NULL));
        CHECK(centi_c == 2650 && s_services > 0u && s_clock_us >= 160000u);
        CHECK(find_cmd(0x70u, 0u) >= 0 && find_cmd(0x40u, 0u) > find_cmd(0x70u, 0u));
        CHECK(find_cmd(0x04u, 0u) < 0 && find_cmd(0x12u, 0u) < 0 && find_cmd(0x20u, 0u) < 0);
        CHECK(find_cmd(0x10u, 0u) < 0 && !hanshow_qn_active());
        CHECK(s_pwr_driven && s_level[P_PWR] == 1u && s_pwr_at_first_byte == 0);
    }
    CASE("Q-N sensor signed quarter degrees and inclusive range preserve raw fractional bits");
    {
        const uint8_t whole[] = {0xe7u, 0xffu, 0u, 50u};
        const uint8_t fractional[] = {0u, 0xc7u, 0u, 0x3fu};
        const int16_t expected[] = {-2500, -25, 0, 5000};
        for (i = 0u; i < sizeof(whole); ++i) {
            sensor_setup(1035u, 8u, whole[i], fractional[i]);
            CHECK(hanshow_qn_read_temperature(1035u, &centi_c, NULL) && centi_c == expected[i]);
        }
    }
    CASE("Q-N sensor refuses malformed revision and implausible temperature without changing output");
    for (i = 0u; i < 7u; ++i) {
        sensor_setup(1035u, 8u, 24u, 0u);
        if (i == 0u) s_qn_revision[0] = 3u;
        if (i == 1u) memset(s_qn_revision, 0xff, sizeof(s_qn_revision));
        if (i == 2u) memset(s_qn_revision, 0, sizeof(s_qn_revision));
        if (i == 3u) s_qn_revision[2] = 2u;
        if (i == 4u) s_qn_temperature[0] = 0xe6u;
        if (i == 5u) s_qn_temperature[0] = 51u;
        if (i == 6u) { s_qn_temperature[0] = 50u; s_qn_temperature[1] = 0x40u; }
        centi_c = 12345;
        CHECK(!hanshow_qn_read_temperature(1035u, &centi_c, NULL) && centi_c == 12345);
        CHECK(!hanshow_qn_active() && s_level[P_PWR] == 1u);
        if (i < 4u) CHECK(find_cmd(0x40u, 0u) < 0);
    }
    CASE("Q-N sensor requires BUSY assertion and completion within its bounded deadline");
    for (i = 0u; i < 3u; ++i) {
        sensor_setup(1035u, 8u, 24u, 0u);
        if (i == 0u) s_conversion_never_assert = true;
        if (i == 1u) s_conversion_stuck = true;
        if (i == 2u) s_busy_stuck = true;
        centi_c = 12345;
        CHECK(!hanshow_qn_read_temperature(1035u, &centi_c, NULL) && centi_c == 12345);
        CHECK(s_clock_us < 600000u && s_services >= 400u && !hanshow_qn_active());
        CHECK(s_level[P_PWR] == 1u);
    }
    CASE("Q-N standalone wrapper updates ordinary telemetry and preserves it on failed probe");
    sensor_setup(1035u, 8u, 26u, 0x80u);
    CHECK(od_tlsr_panel_read_temperature() && s_temp_reported == 26 && s_awake_depth == 0);
    s_qn_revision[0] = 3u;
    CHECK(!od_tlsr_panel_read_temperature() && s_temp_reported == 26 && s_awake_depth == 0);
    CASE("Q-N sensor cancellation during reset or conversion releases every panel resource");
    sensor_setup(1035u, 8u, 24u, 0u);
    centi_c = 12345;
    CHECK(!hanshow_qn_read_temperature(1035u, &centi_c, sensor_keep_connected));
    CHECK(s_log_n == 0u && centi_c == 12345 && !hanshow_qn_active());
    sensor_setup(1035u, 8u, 24u, 0u);
    s_connected = true; s_disconnect_on_conversion = true;
    CHECK(!od_tlsr_panel_read_temperature() && s_temp_reported == -128);
    CHECK(!hanshow_qn_active() && s_awake_depth == 0 && s_level[P_PWR] == 1u);
    CASE("Q-N sensor unknown panel or null output refuses before GPIO access");
    sensor_setup(1035u, 8u, 24u, 0u);
    CHECK(!hanshow_qn_read_temperature(1034u, &centi_c, NULL));
    CHECK(!hanshow_qn_read_temperature(1035u, NULL, NULL));
    CHECK(s_log_n == 0u && !s_pwr_driven);
    CASE("Q-N image startup probes telemetry and refuses sensor reentry during streaming");
    sensor_setup(1035u, 8u, 24u, 0xc0u);
    s_waves_available = true;
    CHECK(begin() && s_temp_reported == 24 && hanshow_qn_active() && s_awake_depth == 1);
    {
        uint32_t logged = s_log_n;
        centi_c = 12345;
        CHECK(!od_tlsr_panel_read_temperature());
        CHECK(!hanshow_qn_read_temperature(1035u, &centi_c, NULL) && centi_c == 12345);
        CHECK(s_log_n == logged && hanshow_qn_active() && s_awake_depth == 1);
    }
    od_xfer_app_abort(OD_XFER_ABORT_RESET);
    CHECK(!hanshow_qn_active() && s_awake_depth == 0 && s_level[P_PWR] == 1u);
    CASE("Q-N invalid sensor response does not block a separately admitted image");
    sensor_setup(1035u, 3u, 24u, 0u);
    CHECK(begin() && s_temp_reported == -128);
    od_xfer_app_abort(OD_XFER_ABORT_RESET);
    CHECK(!hanshow_qn_active() && s_awake_depth == 0);
    s_waves_available = false;
}

static void test_hanshow_qn(void)
{
    uint16_t ids[] = {1035, 1036};
    unsigned i;
    bool completed;
    for (i = 0; i < 2u; ++i) {
        uint16_t w = i == 0u ? 200u : 152u, h = i == 0u ? 200u : 296u;
        uint32_t total = hanshow_qn_wire_size(ids[i]), part = total / 2u;
        long ram1, ram2, lut1, lut2;
        CASE("Q-N streams two components across an arbitrary chunk boundary");
        set_panel(ids[i], w, h, OD_COLOR_SCHEME_INDEXED16_2PASS);
        s_cfg.system_config.pwr_pin = P_PWR;
        s_busy_idle_level = true;
        s_waves_available = true;
        fill(total, 29u + i);
        CHECK(begin());
        CHECK(stream(total, 230u));
        ram1 = find_cmd(0x10, 0u); ram2 = find_cmd(0x10, 1u);
        CHECK(ram1 >= 0 && ram2 > ram1);
        CHECK(find_cmd(0x12, 0u) > ram1 && find_cmd(0x12, 0u) < ram2);
        CHECK(find_cmd(0x12, 1u) < 0); /* only END triggers the second refresh */
        CHECK(data_after(ram1, NULL, s_got, sizeof(s_got)) == part);
        CHECK(memcmp(s_img, s_got, part) == 0);
        CHECK(data_after(ram2, NULL, s_got, sizeof(s_got)) == part);
        CHECK(memcmp(s_img + part, s_got, part) == 0);
        lut1 = find_cmd(0x20, 0u); lut2 = find_cmd(0x20, 1u);
        CHECK(data_after(lut1, NULL, s_got, sizeof(s_got)) == HANSHOW_QN_WAVE_BYTES);
        CHECK(memcmp(s_got, s_wave1, HANSHOW_QN_WAVE_BYTES) == 0);
        CHECK(data_after(lut2, NULL, s_got, sizeof(s_got)) == HANSHOW_QN_WAVE_BYTES);
        CHECK(memcmp(s_got, s_wave2, HANSHOW_QN_WAVE_BYTES) == 0);
        CHECK((find_cmd(0x4d, 0u) >= 0) == (i == 1u));
        CHECK(data_after(find_cmd(0xf0, 0u), NULL, s_got, sizeof(s_got)) == 1u && s_got[0] == (i == 0u ? 0x7du : 0x5fu));
        CHECK(data_after(find_cmd(0xf0, 1u), NULL, s_got, sizeof(s_got)) == 1u && s_got[0] == (i == 0u ? 0x7du : 0x5fu));
        for (unsigned pass = 0u; pass < 2u; ++pass) {
            const uint8_t booster[] = {0x0fu, 0x0au, 0x2fu, 0x25u, 0x22u, i == 0u ? 0x2bu : 0x2eu, 0x21u};
            const uint8_t dimensions[] = {0u, (uint8_t)w, (uint8_t)(h >> 8), (uint8_t)h};
            CHECK(data_after(find_cmd(0x06, pass), NULL, s_got, sizeof(s_got)) == sizeof(booster));
            CHECK(memcmp(s_got, booster, sizeof(booster)) == 0);
            CHECK(data_after(find_cmd(0x61, pass), NULL, s_got, sizeof(s_got)) == sizeof(dimensions));
            CHECK(memcmp(s_got, dimensions, sizeof(dimensions)) == 0);
        }
        CHECK(find_cmd(0x06, 2u) < 0 && find_cmd(0x60, 2u) < 0 && find_cmd(0x65, 2u) < 0);
        CHECK(data_after(find_cmd(0x50, 1u), NULL, s_got, sizeof(s_got)) == 1u && s_got[0] == 0x37);
        CHECK(od_xfer_app_refresh(0u, &completed) && completed);
        CHECK(find_cmd(0x12, 1u) > ram2);
        CHECK(!hanshow_qn_active() && s_awake_depth == 0);
        CHECK(s_pwr_driven && s_level[P_PWR] == 1u && s_services > 0u);
    }
    CASE("Q-N real inflate pump crosses the refresh boundary within 64-byte scratch");
    {
        static const uint8_t compressed[] = {0x18, 0xd3, 0xed, 0xc1, 0x31, 0x01, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0x94, 0xeb, 0x5f, 0xc5, 0x1a, 0x1e, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x60, 0x13, 0xe0, 0xf2, 0x18};
        uint8_t scratch[64];
        uint32_t offset = 0u, j;
        set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
        s_busy_idle_level = true;
        CHECK(begin());
        od_zlib_pump_reset(20000);
        CHECK(od_zlib_pump_push(od_span_make(compressed, sizeof(compressed)), true,
                               od_mut_span_make(scratch, sizeof(scratch)), qn_inflate_sink, &offset) == OD_ZLIB_PUMP_DONE);
        CHECK(offset == 20000 && od_zlib_pump_output_count() == 20000);
        CHECK(find_cmd(0x12, 0) >= 0 && find_cmd(0x12, 1) < 0);
        for (j = 0; j < 2; ++j) {
            uint32_t k;
            CHECK(data_after(find_cmd(0x10, j), NULL, s_got, sizeof(s_got)) == 10000);
            for (k = 0; k < 10000; ++k) if (s_got[k] != 0x55) break;
            CHECK(k == 10000);
        }
        CHECK(od_xfer_app_refresh(0, &completed) && completed && s_awake_depth == 0);
    }
    CASE("Q-N missing waveforms refuse before pin activity");
    set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
    s_waves_available = false;
    CHECK(!begin() && s_log_n == 0 && s_awake_depth == 0);
    s_waves_available = true;
    CASE("Q-N invalid configuration refuses before pin activity");
    set_panel(1035, 200, 200, OD_COLOR_SCHEME_BWR);
    CHECK(!begin() && s_log_n == 0);
    set_panel(1035, 152, 296, OD_COLOR_SCHEME_INDEXED16_2PASS);
    CHECK(!begin() && s_log_n == 0);
    set_panel(1031, 152, 296, OD_COLOR_SCHEME_INDEXED16_2PASS);
    CHECK(!begin() && s_log_n == 0);

    CASE("Q-N incomplete END and nonsequential writes abort safely");
    set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
    s_busy_idle_level = true;
    CHECK(begin());
    CHECK(od_xfer_app_write(1, od_span_make(s_img, 4)) == 0);
    CHECK(!hanshow_qn_active() && s_awake_depth == 0);
    CHECK(begin());
    CHECK(od_xfer_app_refresh(0u, &completed) && !completed);
    CHECK(!hanshow_qn_active() && s_awake_depth == 0);

    CASE("Q-N first refresh timeout and disconnect release panel resources");
    set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
    s_busy_idle_level = true;
    CHECK(begin());
    s_refresh_hold_ms = 200001u;
    CHECK(od_xfer_app_write(0u, od_span_make(s_img, 10000)) == 0);
    CHECK(!hanshow_qn_active() && s_awake_depth == 0);
    CHECK(find_cmd(0x10, 1u) < 0 && s_services >= 200000u);
    set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
    s_busy_idle_level = true; s_connected = true;
    CHECK(begin());
    s_disconnect_on_refresh = true;
    CHECK(od_xfer_app_write(0u, od_span_make(s_img, 10000)) == 0);
    CHECK(!hanshow_qn_active() && s_awake_depth == 0 && find_cmd(0x10, 1u) < 0);

    CASE("Q-N idle timeout starts after the blocking first refresh");
    set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
    s_busy_idle_level = true;
    CHECK(begin());
    s_refresh_hold_ms = 150000;
    CHECK(od_xfer_app_write(0, od_span_make(s_img, 10000)) == 10000);
    od_tlsr_panel_poll(); CHECK(hanshow_qn_active());
    s_clock_us += 30001000u;
    od_tlsr_panel_poll(); CHECK(!hanshow_qn_active() && s_awake_depth == 0);

    CASE("Q-N boot screen uses a bounded single row across two passes");
    {
        uint8_t row[128], qr[256];
        struct od_boot_bufs bufs = {row, sizeof(row), qr, sizeof(qr)};
        uint32_t x, seen = 0;
        set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
        s_busy_idle_level = true;
        CHECK(od_boot_screen_render(&s_cfg, NULL, &bufs));
        CHECK(data_after(find_cmd(0x10, 0), NULL, s_got, sizeof(s_got)) == 10000);
        CHECK(data_after(find_cmd(0x10, 1), NULL, s_img, sizeof(s_img)) == 10000);
        for (x = 0; x < 40000; ++x) {
            uint8_t shift = (uint8_t)(6u - 2u * (x % 4u));
            unsigned code = (((s_got[x / 4u] >> shift) & 3u) << 2) | ((s_img[x / 4u] >> shift) & 3u);
            seen |= 1u << code;
        }
        CHECK((seen & (1u << 4)) != 0 && (seen & (1u << 5)) != 0);
        CHECK((seen & ~((1u << 4) | (1u << 5) | (1u << 9) | (1u << 13) | (1u << 1) | (1u << 6) | (1u << 7))) == 0);
        CHECK(find_cmd(0x12, 1) >= 0 && s_awake_depth == 0);
    }
    CASE("Q-N boot yields to a newly connected client during refresh");
    {
        uint8_t row[128], qr[256];
        struct od_boot_bufs bufs = {row, sizeof(row), qr, sizeof(qr)};
        set_panel(1035, 200, 200, OD_COLOR_SCHEME_INDEXED16_2PASS);
        s_busy_idle_level = true; s_connect_on_refresh = true;
        CHECK(!od_boot_screen_render(&s_cfg, NULL, &bufs));
        CHECK(!hanshow_qn_active() && s_awake_depth == 0 && find_cmd(0x10, 1u) < 0);
    }
    s_waves_available = false;
}

static void test_hanshow_qn_new_family(void)
{
    const uint8_t commands[] = {0x4d, 0xae, 0xb6, 0xba, 0x00, 0x01, 0x03, 0x06,
                               0x30, 0x50, 0xf0, 0x60, 0x61, 0x65, 0x82, 0xe3,
                               0xe7, 0xe9, 0x20, 0x10};
    bool completed;
    unsigned component;
    uint32_t j;
    CASE("Q-N explicit 404545 family preserves per-pass overrides and register order");
    set_panel(1036, 152, 296, OD_COLOR_SCHEME_INDEXED16_2PASS);
    s_busy_idle_level = true; s_waves_available = true; s_new_family = true;
    fill(22496u, 43u);
    CHECK(begin() && stream(22496u, 230u));
    CHECK(od_xfer_app_refresh(0u, &completed) && completed && s_awake_depth == 0);
    for (component = 0; component < 2u; ++component) {
        long start = find_cmd(0x4d, component);
        unsigned k = 0;
        for (j = (uint32_t)start; j < s_log_n && k < sizeof(commands); ++j) {
            if (s_log[j].dc == 0u) CHECK(s_log[j].b == commands[k++]);
        }
        CHECK(k == sizeof(commands));
        CHECK(data_after(find_cmd(0x06, component), NULL, s_got, sizeof(s_got)) == 7u);
        CHECK(memcmp(s_got, s_qn_init[component].booster, 7u) == 0);
        CHECK(data_after(find_cmd(0x60, component), NULL, s_got, sizeof(s_got)) == 2u);
        CHECK(memcmp(s_got, s_qn_init[component].timing, 2u) == 0);
        CHECK(data_after(find_cmd(0x65, component), NULL, s_got, sizeof(s_got)) == 4u);
        CHECK(memcmp(s_got, s_qn_init[component].gate_start, 4u) == 0);
        CHECK(data_after(find_cmd(0x50, component), NULL, s_got, sizeof(s_got)) == 1u);
        CHECK(s_got[0] == (component == 0u ? 0x37 : 0x97));
        CHECK(data_after(find_cmd(0x01, component), NULL, s_got, sizeof(s_got)) == 6u);
        CHECK(s_got[0] == 7 && memcmp(s_got + 1, component == 0 ? s_wave1 : s_wave2, 5u) == 0);
        CHECK(data_after(find_cmd(0x30, component), NULL, s_got, sizeof(s_got)) == 1u);
        CHECK(s_got[0] == (component == 0u ? 7 : 17));
        CHECK(data_after(find_cmd(0x82, component), NULL, s_got, sizeof(s_got)) == 1u);
        CHECK(s_got[0] == ((component == 0u ? 6u : 16u) ^ 0x80u));
        CHECK(data_after(find_cmd(0x20, component), NULL, s_got, sizeof(s_got)) == HANSHOW_QN_WAVE_BYTES);
        CHECK(memcmp(s_got, component == 0 ? s_wave1 : s_wave2, HANSHOW_QN_WAVE_BYTES) == 0);
        CHECK(data_after(find_cmd(0x10, component), NULL, s_got, sizeof(s_got)) == 11248u);
        CHECK(memcmp(s_got, s_img + component * 11248u, 11248u) == 0);
        {
            long shutdown = find_cmd(0x00, 2u * component + 1u);
            CHECK(shutdown > find_cmd(0x12, component) && shutdown < find_cmd(0x02, component));
            CHECK(data_after(shutdown, NULL, s_got, sizeof(s_got)) == 2u && s_got[0] == 0x27 && s_got[1] == 0xa9);
        }
    }
    CHECK(find_cmd(0x06, 2u) < 0 && find_cmd(0x60, 2u) < 0 && find_cmd(0x65, 2u) < 0);

    CASE("Q-N explicit family refuses missing overrides, wrong band, unknown family and 154");
    for (j = 0; j < 5u; ++j) {
        struct hanshow_qn_waveforms valid = s_qn_new;
        set_panel(j == 4u ? 1035 : 1036, j == 4u ? 200 : 152, j == 4u ? 200 : 296,
                  OD_COLOR_SCHEME_INDEXED16_2PASS);
        if (j < 2u) s_qn_new.initializer[j] = NULL;
        if (j == 2u) s_qn_new.band = 6;
        if (j == 3u) s_qn_new.family = 255;
        if (j == 4u) s_qn_new.panel_id = 1035;
        CHECK(!begin() && s_log_n == 0 && s_awake_depth == 0);
        s_qn_new = valid;
    }
    s_new_family = false; s_waves_available = false;
}

static void test_refusals(void)
{
    od_xfer_panel_info_t info;
    bool completed = true;

    CASE("unknown panel type refused before any pin moves");
    set_panel(1037u, 400u, 300u, OD_COLOR_SCHEME_BWR);
    CHECK(!od_xfer_app_panel_info(&info));
    CHECK(s_log_n == 0u);

    CASE("canonical 1-51 are bb_epaper panels, not legacy model ids");
    set_panel(3u, 400u, 300u, OD_COLOR_SCHEME_BWR);   /* would be UC8176_420_BWR on raw nRF52 */
    CHECK(!od_xfer_app_panel_info(&info));
    CHECK(s_log_n == 0u);

    CASE("config resolution that disagrees with the model table");
    set_panel(1002u, 296u, 128u, OD_COLOR_SCHEME_BWR);
    s_busy_idle_level = true;
    CHECK(!begin());

    CASE("colour scheme the model cannot show");
    set_panel(1000u, 400u, 300u, OD_COLOR_SCHEME_BWRY);  /* UC8176 BW with a 4-colour config */
    CHECK(!begin());

    CASE("no display configured");
    set_panel(1002u, 400u, 300u, OD_COLOR_SCHEME_BWR);
    s_cfg.display_count = 0u;
    CHECK(!od_xfer_app_panel_info(&info));

    CASE("writes past the image and without a transfer are refused");
    set_panel(1002u, 400u, 300u, OD_COLOR_SCHEME_BWR);
    CHECK(od_xfer_app_write(0u, od_span_make(s_img, 10u)) == 0u);   /* nothing begun */
    CHECK(begin());
    CHECK(od_xfer_app_write(29995u, od_span_make(s_img, 10u)) == 0u);
    CHECK(od_xfer_app_write(30001u, od_span_make(s_img, 1u)) == 0u);

    CASE("a busy line that never clears reports an incomplete refresh");
    s_busy_stuck = true;
    CHECK(od_xfer_app_refresh(0u, &completed));
    CHECK(!completed);
    CHECK(!od_xfer_app_refresh(0u, &completed));     /* panel already powered down */
}

int main(void)
{
    test_uc8176_bwr();
    test_ssd1619_bw_odd_chunks();
    test_plane_boundary_in_one_write();
    test_active_low_panel_power();
    test_hanshow_266_window();
    test_hanshow_200_bwy_window();
    test_hanshow_350_bwy();
    test_ti_970();
    test_boot_screen();
    test_hanshow_qn_temperature();
    test_hanshow_boot_component_projection();
    test_hanshow_qn();
    test_hanshow_qn_new_family();
    test_refusals();
    return OD_CHECK_REPORT_NONEMPTY("tlsr_display", 40u);
}
