/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * APN auto-detect (CONTRACTS 2a defaults: "4G with APN auto-detect"). Used when the APN setting is empty: the IMSI
 * gives MCC 452 (Vietnam) + a 2-digit MNC, the table gives the operator's default Internet APN. A user APN set in the
 * portal or console always wins. An unknown network (iTel, Wintel ... whose MNC is not confirmed here) gets an empty APN:
 * the LTE default bearer the network assigns at attach usually works. Sources and status: docs/APN.md.
 */
#include <string.h>
#include <ctype.h>
#include "apn.h"

static const apn_entry_t table[] = {
	{ "45201", "MobiFone", "m-wap", false },
	{ "45202", "Vinaphone", "m3-world", false },
	{ "45204", "Viettel", "v-internet", false },
	{ "45205", "Vietnamobile", "internet", false },
	{ "45207", "Gmobile", "internet", false },
};

const apn_entry_t *apn_from_imsi(const char *imsi)
{
	if (!imsi) {
		return NULL;
	}
	while (*imsi && !isdigit((unsigned char)*imsi)) {
		imsi++; /* "+CIMI: 4520..." or a leading newline */
	}
	if (strlen(imsi) < 5) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (!strncmp(imsi, table[i].mccmnc, 5)) {
			return &table[i];
		}
	}
	return NULL;
}
