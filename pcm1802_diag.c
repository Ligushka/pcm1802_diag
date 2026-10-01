/*
 * PCM1802 slot diagnostic for the RP2350B DualADC/DualPCM board
 * (https://github.com/Sev5000/RP2350B_DualADC_DualPCM), used with the
 * hsdaoh-rp2350 dual_external_adc firmware.
 *
 * Drives the same 40 MHz clock the production firmware puts on GP20..GP22
 * (ADC1 clock / PCM1802 SCKI for both slots / ADC2 clock), then measures
 * what each PCM1802 slot sends back and prints a report over USB serial
 * once per second:
 *
 *   PCM1802 #1: DOUT GP0, BCK GP1, LRCK GP2  (hsdaoh_file -3)
 *   PCM1802 #2: DOUT GP3, BCK GP4, LRCK GP5  (hsdaoh_file -2)
 *
 * With MODE0 high / MODE1 low the PCM1802 is clock master at 512 fs, so a
 * working module outputs LRCK = 40 MHz / 512 = 78.125 kHz and BCK = 5 MHz.
 * Lines that do not toggle are classified as floating, held low or held
 * high by flipping the RP2350's internal pull resistors.
 *
 * Serial keys: 'd' cycles the GP21 pad drive, 'f' cycles the SCK frequency.
 * See README.md for building, usage and the module repair checklist.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"

#include "pcm1802_diag.pio.h"

#define DIAG_VERSION	"1.0"

#define DIAG_SYS_CLK_KHZ	240000		/* 40 MHz clock = sys_clk / 6 */

#define CLK_PIN_BASE	20		/* GP20 ADC1 clk, GP21 PCM1802 SCKI, GP22 ADC2 clk */
#define SCK_PIN		21
#define GATE_US		200000		/* frequency measurement window */
#define REPORT_MS	1000

/* GP21 pad setting at boot, index into sck_modes[]. Press 'd' to cycle. */
#define SCK_MODE_DEFAULT	0

/* SCK frequency at boot, index into sck_freqs[]. Press 'f' to cycle. */
#define SCK_FREQ_DEFAULT	0

/*
 * clockgen toggles every PIO cycle, so SCK = sys_clk / (2 * clkdiv).
 * Only integer dividers of 240 MHz, to keep the clock jitter-free.
 * 40 MHz is what the production firmware uses (fs = 78.125 kHz at 512 fs).
 */
static const double sck_freqs[] = {
	40000000.0,
	30000000.0,
	24000000.0,
	20000000.0,
	12000000.0,
};

static unsigned int sck_freq;

/*
 * The production firmware leaves GP20..GP22 at the pad reset default
 * (4 mA, slow slew); hsdaoh_init() only configures the HSTX pins GP12..19.
 */
static const struct {
	enum gpio_drive_strength drive;
	enum gpio_slew_rate slew;
	const char *name;
} sck_modes[] = {
	{ GPIO_DRIVE_STRENGTH_4MA,  GPIO_SLEW_RATE_SLOW, "4 mA, slow slew (production default)" },
	{ GPIO_DRIVE_STRENGTH_4MA,  GPIO_SLEW_RATE_FAST, "4 mA, fast slew" },
	{ GPIO_DRIVE_STRENGTH_8MA,  GPIO_SLEW_RATE_FAST, "8 mA, fast slew" },
	{ GPIO_DRIVE_STRENGTH_12MA, GPIO_SLEW_RATE_FAST, "12 mA, fast slew" },
};

static unsigned int sck_mode;
static uint clk_sm;

enum level {
	LVL_ACTIVE,
	LVL_FLOAT,
	LVL_LOW,
	LVL_HIGH,
	LVL_ODD,
};

static const char *level_str[] = {
	[LVL_ACTIVE] = "toggling",
	[LVL_FLOAT]  = "static, FLOATING (nothing drives it)",
	[LVL_LOW]    = "static, held LOW",
	[LVL_HIGH]   = "static, held HIGH",
	[LVL_ODD]    = "static, unclear (HIGH with pull-down, LOW with pull-up)",
};

struct sig {
	const char *name;
	uint pin;
	PIO pio;
	uint sm;
	double freq;
	enum level level;
};

enum { SCK, S1_DOUT, S1_BCK, S1_LRCK, S2_DOUT, S2_BCK, S2_LRCK, NUM_SIGS };

static struct sig sigs[NUM_SIGS] = {
	[SCK]     = { "SCK",  21 },
	[S1_DOUT] = { "DOUT",  0 },
	[S1_BCK]  = { "BCK",   1 },
	[S1_LRCK] = { "LRCK",  2 },
	[S2_DOUT] = { "DOUT",  3 },
	[S2_BCK]  = { "BCK",   4 },
	[S2_LRCK] = { "LRCK",  5 },
};

struct slot {
	const char *label;	/* silkscreen on the carrier PCB */
	int stream;		/* hsdaoh_file option recording this slot */
	int dout, bck, lrck;	/* indices into sigs[] */
};

/* production firmware: data pin GP3 -> stream 2, data pin GP0 -> stream 3 */
static const struct slot slots[] = {
	{ "PCM1802 #1", 3, S1_DOUT, S1_BCK, S1_LRCK },
	{ "PCM1802 #2", 2, S2_DOUT, S2_BCK, S2_LRCK },
};

static bool near(double value, double target, double tol)
{
	return value > target * (1 - tol) && value < target * (1 + tol);
}

static void start_clock(void)
{
	PIO pio = pio0;
	uint offset = pio_add_program(pio, &clockgen_program);
	uint sm = pio_claim_unused_sm(pio, true);
	pio_sm_config c = clockgen_program_get_default_config(offset);

	sm_config_set_sideset_pins(&c, CLK_PIN_BASE);

	for (uint i = 0; i < 3; i++)
		pio_gpio_init(pio, CLK_PIN_BASE + i);
	pio_sm_set_consecutive_pindirs(pio, sm, CLK_PIN_BASE, 3, true);

	/* left disabled, set_clock() configures and starts it */
	pio_sm_init(pio, sm, offset, &c);
	clk_sm = sm;
}

/*
 * Apply GP21 pad drive and SCK frequency. The clock is halted (low) for
 * 20 ms around the change, so a PCM1802 that got confused by a bad clock
 * sees a clean restart.
 */
static void set_clock(unsigned int mode, unsigned int freq)
{
	sck_mode = mode % count_of(sck_modes);
	sck_freq = freq % count_of(sck_freqs);

	pio_sm_set_enabled(pio0, clk_sm, false);
	pio_sm_set_pins_with_mask(pio0, clk_sm, 0, 7u << CLK_PIN_BASE);

	gpio_set_drive_strength(SCK_PIN, sck_modes[sck_mode].drive);
	gpio_set_slew_rate(SCK_PIN, sck_modes[sck_mode].slew);
	pio_sm_set_clkdiv(pio0, clk_sm,
			  (float)(clock_get_hz(clk_sys) / (2 * sck_freqs[sck_freq])));
	sleep_ms(20);

	pio_sm_clkdiv_restart(pio0, clk_sm);
	pio_sm_set_enabled(pio0, clk_sm, true);
}

/* PIO inputs read the pad directly, so the counters never touch pin
 * functions or directions (important for GP21, driven by clockgen). */
static void start_counter(struct sig *s, PIO pio, uint offset)
{
	pio_sm_config c = edge_counter_program_get_default_config(offset);

	s->pio = pio;
	s->sm = pio_claim_unused_sm(pio, true);
	sm_config_set_in_pins(&c, s->pin);
	sm_config_set_clkdiv(&c, 1.f);
	pio_sm_init(pio, s->sm, offset, &c);
	pio_sm_set_enabled(pio, s->sm, true);
}

/*
 * Snapshot X of a counter SM. Returns false if the SM does not answer in
 * time; that only happens for a static input, so callers treat it as
 * "no edges".
 */
static bool read_count(const struct sig *s, uint32_t *x)
{
	while (!pio_sm_is_rx_fifo_empty(s->pio, s->sm))
		pio_sm_get(s->pio, s->sm);

	pio_sm_exec(s->pio, s->sm, pio_encode_mov(pio_isr, pio_x));
	pio_sm_exec(s->pio, s->sm, pio_encode_push(false, false));

	absolute_time_t timeout = make_timeout_time_us(1000);
	while (pio_sm_is_rx_fifo_empty(s->pio, s->sm))
		if (time_reached(timeout))
			return false;

	*x = pio_sm_get(s->pio, s->sm);
	return true;
}

/*
 * Classify a static line by flipping the internal pull resistor. Before
 * the pull-down read the input buffer is briefly disabled: RP2350 erratum
 * E9 can otherwise keep a floating pin latched at ~2 V.
 */
static enum level probe_level(uint pin)
{
	gpio_set_pulls(pin, false, true);
	gpio_set_input_enabled(pin, false);
	sleep_us(200);
	gpio_set_input_enabled(pin, true);
	sleep_us(10);
	bool with_pd = gpio_get(pin);

	gpio_set_pulls(pin, true, false);
	sleep_us(200);
	bool with_pu = gpio_get(pin);

	/* back to the production default: pull-down */
	gpio_set_pulls(pin, false, true);
	gpio_set_input_enabled(pin, false);
	sleep_us(200);
	gpio_set_input_enabled(pin, true);

	if (!with_pd && with_pu)
		return LVL_FLOAT;
	if (!with_pd && !with_pu)
		return LVL_LOW;
	if (with_pd && with_pu)
		return LVL_HIGH;
	return LVL_ODD;
}

static void measure(void)
{
	uint32_t x0[NUM_SIGS], x1[NUM_SIGS];
	bool ok0[NUM_SIGS], ok1[NUM_SIGS];

	uint64_t t0 = time_us_64();
	for (int i = 0; i < NUM_SIGS; i++)
		ok0[i] = read_count(&sigs[i], &x0[i]);

	sleep_us(GATE_US);

	uint64_t t1 = time_us_64();
	for (int i = 0; i < NUM_SIGS; i++)
		ok1[i] = read_count(&sigs[i], &x1[i]);

	double secs = (t1 - t0) / 1e6;

	for (int i = 0; i < NUM_SIGS; i++) {
		/* X counts down, so the number of periods is x0 - x1 */
		uint32_t periods = (ok0[i] && ok1[i]) ? x0[i] - x1[i] : 0;

		sigs[i].freq = periods / secs;
		if (periods)
			sigs[i].level = LVL_ACTIVE;
		else if (i == SCK)	/* output pin, don't touch its pulls */
			sigs[i].level = gpio_get(sigs[i].pin) ? LVL_HIGH : LVL_LOW;
		else
			sigs[i].level = probe_level(sigs[i].pin);
	}
}

static void fmt_freq(char *buf, size_t len, double hz)
{
	if (hz >= 1e6)
		snprintf(buf, len, "%8.3f MHz", hz / 1e6);
	else if (hz >= 1e3)
		snprintf(buf, len, "%8.3f kHz", hz / 1e3);
	else
		snprintf(buf, len, "%8.0f Hz ", hz);
}

static void print_sig(const struct sig *s, const char *expect)
{
	char f[24];

	fmt_freq(f, sizeof(f), s->freq);
	printf("  %-4s GP%-2u %s   expect %-10s  %s\n", s->name, s->pin, f, expect,
	       s->level == LVL_ACTIVE ? "" : level_str[s->level]);
}

static void report_slot(const struct slot *sl, double f_sck)
{
	const struct sig *lr = &sigs[sl->lrck];
	const struct sig *bk = &sigs[sl->bck];
	const struct sig *dt = &sigs[sl->dout];
	double nominal = sck_freqs[sck_freq];
	char exp_lr[24], exp_bk[24];

	/* 512 fs master mode: LRCK = SCKI / 512, BCK = 64 * LRCK */
	fmt_freq(exp_lr, sizeof(exp_lr), nominal / 512);
	fmt_freq(exp_bk, sizeof(exp_bk), nominal / 8);

	printf("\n[%s]  GP%u-GP%u, recorded by: hsdaoh_file -%d <file>\n",
	       sl->label, dt->pin, lr->pin, sl->stream);
	print_sig(lr, exp_lr);
	print_sig(bk, exp_bk);
	print_sig(dt, "toggling");
	printf("  => ");

	if (lr->level == LVL_ACTIVE && bk->level == LVL_ACTIVE) {
		double sck = near(f_sck, nominal, 0.01) ? f_sck : nominal;
		double ratio = sck / lr->freq;
		double bits = bk->freq / lr->freq;
		bool good = false;

		if (lr->freq > bk->freq) {
			printf("BCK and LRCK look swapped (LRCK faster than BCK) - check wiring.\n");
		} else if (near(ratio, 512, 0.02)) {
			printf("Master mode, SCKI = 512 fs (MODE0 high, MODE1 low), fs = %.0f Hz.\n"
			       "     Correct for the current firmware.\n", lr->freq);
			good = true;
		} else if (near(ratio, 256, 0.02)) {
			printf("Master mode, SCKI = 256 fs: MODE0 AND MODE1 are high.\n"
			       "     Right for the cxadc clockgen and old hsdaoh firmware; current\n"
			       "     hsdaoh firmware wants MODE1 low (remove the MODE1 bridge).\n");
		} else if (near(ratio, 384, 0.02)) {
			printf("Master mode, SCKI = 384 fs: MODE1 high, MODE0 low.\n"
			       "     Current firmware wants MODE0 high, MODE1 low.\n");
		} else {
			printf("Clocks present, but SCKI/LRCK ratio is %.1f (expected 512).\n"
			       "     Intermittent LRCK contact, noisy SCKI, or a marginal chip.\n", ratio);
		}

		if (!near(bits, 64, 0.02)) {
			printf("     BCK/LRCK ratio is %.1f (expected 64): intermittent BCK or LRCK contact?\n", bits);
			good = false;
		}
		if (dt->level == LVL_FLOAT) {
			printf("     DOUT is %s: clocks run but DOUT is not connected\n"
			       "     to GP%u. Check the DOUT contact (header -> IC pin 12).\n",
			       level_str[dt->level], dt->pin);
			good = false;
		} else if (dt->level != LVL_ACTIVE) {
			/* even silence toggles the LSBs; constant data = converter not running */
			printf("     DOUT is %s: the digital side runs, but the converter\n"
			       "     outputs no samples. Most likely the 5V analog supply is missing:\n"
			       "     unpowered, 5V header -> IC pin 5 (VCC) should read ~0 ohm and\n"
			       "     -> pin 4 (VREF2) ~1 kohm. Some boards lack the 5V trace to pin 5;\n"
			       "     bridge the regulator's 5V input leg to the capacitor on pin 5.\n",
			       level_str[dt->level]);
			good = false;
		}
		if (good && sck_freq == 0)
			printf("     This slot should record. If hsdaoh_file still writes 0 KB,\n"
			       "     make sure you used -%d for this slot.\n", sl->stream);
		else if (good)
			printf("     The chip runs with a %.0f MHz SCK. If it does not run at 40 MHz,\n"
			       "     it cannot handle the production firmware's clock.\n", nominal / 1e6);
		return;
	}

	if (lr->level != LVL_ACTIVE && bk->level != LVL_ACTIVE) {
		if (lr->level == LVL_FLOAT && bk->level == LVL_FLOAT && dt->level == LVL_FLOAT)
			printf("Nothing drives LRCK, BCK or DOUT. An empty slot looks exactly like this.\n"
			       "     With a module plugged in: it is not making contact or has no power,\n"
			       "     or the PCM1802 is idle (slave mode / no SCKI) with DOUT released.\n"
			       "     Check +5V, 3.3V, PDW (~3.3V) and SCK (~1.6V) at the module header.\n");
		else if (lr->level == LVL_FLOAT && bk->level == LVL_FLOAT)
			printf("DOUT is driven, so the chip is powered, but it does not drive BCK/LRCK.\n"
			       "     Most likely SLAVE mode: MODE0 not high at the chip. The '+' jumper\n"
			       "     pads are not connected to 3.3V on these boards; bridge MODE0 and\n"
			       "     wire 3.3V to it, then 3.3V header -> IC pin 19 should beep.\n"
			       "     Otherwise no SCKI: SCK header -> IC pin 15 should beep.\n"
			       "     If both beep, the chip ignores its mode pins: likely a bad or\n"
			       "     counterfeit PCM1802.\n");
		else if (lr->level == LVL_LOW && bk->level == LVL_LOW)
			printf("BCK and LRCK are held low: set up as master but not running.\n"
			       "     Unpowered, check SCK header -> IC pin 15, PDW -> pin 7 and\n"
			       "     3.3V -> pin 14 (all beep), and SCK -> GP21 on the carrier\n"
			       "     (PGA2350 header J3 pin 4). If all pass, suspect the chip.\n");
		else if (lr->level == LVL_HIGH && bk->level == LVL_HIGH)
			printf("BCK and LRCK are held high: the chip drives them but is not running.\n"
			       "     Seen on chips in slave mode without clocks. Check MODE0 (3.3V\n"
			       "     header -> IC pin 19 should beep) and SCK -> IC pin 15.\n");
		else
			printf("Unexpected mix of static levels - suspect individual pin contacts\n"
			       "     between the module header and GP%u-GP%u.\n", dt->pin, lr->pin);
		return;
	}

	const struct sig *dead = (lr->level == LVL_ACTIVE) ? bk : lr;
	printf("Only one of BCK/LRCK toggles; %s (GP%u) is %s.\n"
	       "     Contact problem on that line between the module header and GP%u.\n",
	       dead->name, dead->pin, level_str[dead->level], dead->pin);
}

static void report(unsigned int n)
{
	const struct sig *sck = &sigs[SCK];
	char f[24];

	double nominal = sck_freqs[sck_freq];
	char exp[24];

	printf("\n==== PCM1802 slot diagnostic v" DIAG_VERSION ", report %u ====\n", n);
	printf("GP21 (SCK) pad: %s   [press 'd' to change]\n", sck_modes[sck_mode].name);
	printf("SCK frequency:  %.0f MHz%s   [press 'f' to change]\n", nominal / 1e6,
	       sck_freq == 0 ? " (production)" : "");
	printf("Expected values assume 512 fs (MODE0 only). With MODE0+MODE1 bridged\n"
	       "(256 fs, cxadc clockgen setup) LRCK and BCK run at twice the rate shown.\n");

	fmt_freq(f, sizeof(f), sck->freq);
	fmt_freq(exp, sizeof(exp), nominal);
	printf("\n[Clock]  SCK GP%u %s   expect %s\n  => ", sck->pin, f, exp);
	if (near(sck->freq, nominal, 0.01))
		printf("OK, the RP2350 drives GP21. (Measured at the RP2350 pin: this does\n"
		       "     not prove the clock reaches the modules.)\n");
	else
		printf("BAD: GP21 is not toggling at the RP2350 pin (%s).\n"
		       "     Shorted to GND/3.3V or to a neighbouring pin?\n", level_str[sck->level]);

	for (unsigned int i = 0; i < count_of(slots); i++)
		report_slot(&slots[i], sck->freq);
}

int main(void)
{
	/* 240 MHz: 40 MHz = sys_clk / 6. Production runs 320 MHz at VREG max. */
	vreg_set_voltage(VREG_VOLTAGE_1_20);
	sleep_ms(10);
	set_sys_clock_khz(DIAG_SYS_CLK_KHZ, true);

	stdio_init_all();

	for (int i = S1_DOUT; i < NUM_SIGS; i++) {
		gpio_init(sigs[i].pin);
		gpio_set_pulls(sigs[i].pin, false, true);	/* reset default, as in production */
	}

	start_clock();
	set_clock(SCK_MODE_DEFAULT, SCK_FREQ_DEFAULT);

	uint off0 = pio_add_program(pio0, &edge_counter_program);
	uint off1 = pio_add_program(pio1, &edge_counter_program);
	uint off2 = pio_add_program(pio2, &edge_counter_program);

	start_counter(&sigs[SCK], pio0, off0);
	for (int i = S1_DOUT; i <= S1_LRCK; i++)
		start_counter(&sigs[i], pio1, off1);
	for (int i = S2_DOUT; i <= S2_LRCK; i++)
		start_counter(&sigs[i], pio2, off2);

	for (unsigned int n = 1; ; n++) {
		measure();
		report(n);

		absolute_time_t next = make_timeout_time_ms(REPORT_MS - GATE_US / 1000);
		while (!time_reached(next)) {
			int ch = getchar_timeout_us(10000);

			if (ch == 'd' || ch == 'D') {
				set_clock(sck_mode + 1, sck_freq);
				printf("\n>>> GP21 (SCK) pad now: %s\n", sck_modes[sck_mode].name);
				break;
			}
			if (ch == 'f' || ch == 'F') {
				set_clock(sck_mode, sck_freq + 1);
				printf("\n>>> SCK now: %.0f MHz\n", sck_freqs[sck_freq] / 1e6);
				break;
			}
		}
	}
}
