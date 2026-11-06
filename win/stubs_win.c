/* What the Windows build does not have yet: the in-game updater (switch/update.c: libnx + curl,
 * replaces the .nro). */
#include <stdio.h>
#include <string.h>
#include "../runtime/rt.h"
#include "../switch/update.h"
#include "mkdd_version.h"

void update_init(const char *nro_path) { (void)nro_path; }
const char *update_version(void) { return MKDD_VERSION; }
void update_status(UpdateStatus *out) {
    memset(out, 0, sizeof *out);
    out->state = UPD_ERROR;
    snprintf(out->msg, sizeof out->msg, "NOT IN THE WINDOWS BUILD YET");
}
void update_start(void) {}

