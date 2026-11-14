#pragma once

#include "cJSON.h"

#define PROBE_FW_VERSION "0.1.0"

/*
 * Every scan takes the request's "args" object (may be NULL) and returns a
 * cJSON value owned by the caller. On failure it returns NULL and points *err
 * at a static string.
 */
cJSON *wifi_scan(const cJSON *args, const char **err);
cJSON *ble_scan(const cJSON *args, const char **err);

/* Thread scans exist only on chips with an 802.15.4 radio. */
void thread_init(void);
cJSON *thread_discover(const cJSON *args, const char **err);
cJSON *thread_energy(const cJSON *args, const char **err);

/* Integer argument with a default and clamping. */
int probe_arg_int(const cJSON *args, const char *name, int def, int min, int max);
