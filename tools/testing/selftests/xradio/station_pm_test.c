// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <string.h>

#include "station-command.h"
#include "station-ps.h"

static int failures;

static void expect(int condition, const char *name)
{
	if (condition)
		return;
	fprintf(stderr, "FAIL: %s\n", name);
	failures++;
}

static void test_encoding_and_allowlist(void)
{
	u8 frame[8] = { 0 };
	u8 wire[4];

	xrs_ps_encode(wire, false, 0);
	memcpy(frame + 4, wire, sizeof(wire));
	expect(xrs_command_allowed(0x0010, frame, sizeof(frame), 1),
	       "off encoding allowed");

	xrs_ps_encode(wire, true, 0);
	memcpy(frame + 4, wire, sizeof(wire));
	expect(!memcmp(wire, (u8[]) { 1, 0, 200, 0 }, sizeof(wire)),
	       "normal PS wire encoding");
	expect(xrs_command_allowed(0x0010, frame, sizeof(frame), 1),
	       "normal PS allowed");

	xrs_ps_encode(wire, true, 100);
	memcpy(frame + 4, wire, sizeof(wire));
	expect(!memcmp(wire, (u8[]) { 0x81, 200, 200, 0 }, sizeof(wire)),
	       "fast PS wire encoding");
	expect(xrs_command_allowed(0x0010, frame, sizeof(frame), 1),
	       "fast PS allowed");

	frame[7] = 1;
	expect(!xrs_command_allowed(0x0010, frame, sizeof(frame), 1),
	       "reserved byte rejected");
	frame[7] = 0;
	frame[4] = 2;
	expect(!xrs_command_allowed(0x0010, frame, sizeof(frame), 1),
	       "unknown mode rejected");
	frame[4] = 1;
	frame[5] = 1;
	expect(!xrs_command_allowed(0x0010, frame, sizeof(frame), 1),
	       "normal PS idle byte rejected");
	expect(!xrs_command_allowed(0x0010, frame, sizeof(frame), 0),
	       "PM rejected outside station mode");
	expect(!xrs_command_allowed(0x0010, frame, sizeof(frame) - 1, 1),
	       "short PM command rejected");
}

static void test_indication_state_machine(void)
{
	struct xrs_ps_state state = { 0 };
	u8 wire[4];

	xrs_ps_encode(wire, true, 0);
	expect(xrs_ps_begin(&state, wire) == 1, "transition begins");
	expect(xrs_ps_finish(&state, 0) == -ETIMEDOUT,
	       "confirmation without indication rejected");
	expect(xrs_ps_begin(&state, wire) == -EIO,
	       "ambiguous transition poisons epoch");

	memset(&state, 0, sizeof(state));
	expect(xrs_ps_begin(&state, wire) == 1, "fresh transition begins");
	expect(xrs_ps_indicate(&state), "first indication accepted");
	expect(!xrs_ps_indicate(&state), "duplicate indication rejected");
	expect(xrs_ps_finish(&state, 0) == 0, "indicated transition succeeds");
	expect(xrs_ps_begin(&state, wire) == 0, "applied mode is idempotent");
}

static void test_scan_allowlist(void)
{
	u8 frame[32] = { 0 };

	/* One passive channel record with a bounded dwell interval. */
	frame[13] = 1;
	frame[16] = 1;
	frame[20] = 10;
	frame[24] = 20;
	expect(xrs_command_allowed(7, frame, sizeof(frame), 0),
	       "foreground scan allowed before JOIN");

	frame[5] = 0x41;
	expect(!xrs_command_allowed(7, frame, sizeof(frame), 0),
	       "forced background scan rejected before JOIN");
	expect(xrs_command_allowed(7, frame, sizeof(frame), 1),
	       "forced background scan allowed after JOIN");

	frame[5] = 0x40;
	expect(!xrs_command_allowed(7, frame, sizeof(frame), 1),
	       "background scan without force rejected");
	frame[5] = 0x01;
	expect(!xrs_command_allowed(7, frame, sizeof(frame), 1),
	       "foreground force flag rejected");
}

int main(void)
{
	test_encoding_and_allowlist();
	test_indication_state_machine();
	test_scan_allowlist();
	if (failures)
		return 1;
	puts("PASS: XR829 PM encoding, allowlist and indication state");
	return 0;
}
