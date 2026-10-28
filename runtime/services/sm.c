/*
 * sm.c - "sm:", the service manager: hands out sessions to the other services by name.
 * switchbrew.org/wiki/Services_API#sm:
 */
#include "services.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- services that exist but aren't written yet ---- */
typedef struct { char name[9]; } StubState;

static void stub_dispatch(ServiceObject *self, IpcCall *call) {
    StubState *st = self->state;
    fprintf(stderr, "\nipc: service \"%s\" is not implemented yet (command %u)\n", st->name, call->cmd);
    ipc_unknown_command(self, call);
}
static void stub_destroy(ServiceObject *self) { free(self->state); }
static const ServiceType stub_type = { "(unimplemented)", stub_dispatch, stub_destroy };

/* name -> function that makes a new object of that service */
static const struct { const char *name; ServiceObject *(*create)(void); } registry[] = {
    { "lm", service_lm_create },
    { "appletOE", service_applet_oe_create },
    { "apm", service_apm_create },
    { "apm:p", service_apm_create },
    { "pctl", service_pctl_create },
    { "pctl:a", service_pctl_create },
    { "pctl:s", service_pctl_create },
    { "pctl:r", service_pctl_create },
    { "aoc:u", service_aoc_create },
    { "mm:u", service_mm_create },
    { "prepo:u", service_prepo_create },
    { "prepo:a", service_prepo_create },
    { "prepo:s", service_prepo_create },
    { "prepo:m", service_prepo_create },
    { "nvdrv", service_nvdrv_create },
    { "time:u", service_time_create },
    { "fsp-srv", service_fsp_create },
    { "acc:u0", service_acc_create },
    { "set", service_set_create },
    { "set:sys", service_set_sys_create },
    { "hid", service_hid_create },
    { "vi:u", service_vi_create },
    { "vi:s", service_vi_create },
    { "vi:m", service_vi_create },
    { "pl:u", service_pl_create },
    { "audren:u", service_audren_create },
    { "audout:u", service_audout_create },
    { "acc:u1", service_acc_create },
    { "time:a", service_time_create },
    { "time:s", service_time_create },
    { "nvdrv:a", service_nvdrv_create },
    { "nvdrv:s", service_nvdrv_create },
    { "nvdrv:t", service_nvdrv_create },
    { NULL, NULL },
};

ServiceObject *service_open(const char *name) {
    for (int k = 0; registry[k].name; k++)
        if (strcmp(registry[k].name, name) == 0) return registry[k].create();
    StubState *st = calloc(1, sizeof *st);
    if (!st) exit(1);
    snprintf(st->name, sizeof st->name, "%s", name);
    return ipc_new_object(&stub_type, st);
}

/* ---- sm: itself ---- */
static void sm_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* RegisterClient(pid): "this process is using sm" */
        break;
    case 1: {                   /* GetServiceHandle(name: 8 chars, zero padded) -> session */
        char name[9] = { 0 };
        uint64_t raw = ipc_in64(call, 0);
        memcpy(name, &raw, 8);
        if (ipc_trace) fprintf(stderr, "sm: GetServiceHandle(\"%s\")\n", name);
        ipc_out_object(call, service_open(name));
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}

static const ServiceType sm_type = { "sm:", sm_dispatch, NULL };

ServiceObject *service_sm_create(void) { return ipc_new_object(&sm_type, NULL); }
