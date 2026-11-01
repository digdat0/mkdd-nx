/* uam_c.h on top of uam_api.h. */
#include "uam_c.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "uam_api.h"

extern "C" void uamc_init(void) { uam::init(true); }

extern "C" bool uamc_compile(bool fragment, const char *glsl, uint8_t **dksh, size_t *size, char *log, size_t logn) {
    *dksh = nullptr;
    *size = 0;
    uam::Result r = uam::compile(fragment ? uam::Stage::Fragment : uam::Stage::Vertex, glsl);
    if (log && logn) snprintf(log, logn, "%s", r.log.c_str());
    if (!r.ok || r.dksh.empty()) return false;
    uint8_t *p = (uint8_t *)malloc(r.dksh.size());
    if (!p) return false;
    memcpy(p, r.dksh.data(), r.dksh.size());
    *dksh = p;
    *size = r.dksh.size();
    return true;
}
