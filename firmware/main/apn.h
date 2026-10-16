/* APN from the SIM's home network (MCC + MNC from the IMSI). Pure C, host-tested (tests/test_apn.c). */
#pragma once
#include <stdbool.h>

typedef struct {
	const char *mccmnc;   /* "45204" */
	const char *operator_name;
	const char *apn;
	bool verified;        /* confirmed on the operator's own site (docs/APN.md); all false on 16 Oct 2026 */
} apn_entry_t;

/* IMSI digits ("452040123456789") -> the table entry, NULL when unknown */
const apn_entry_t *apn_from_imsi(const char *imsi);
