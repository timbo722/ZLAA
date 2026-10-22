/*
 * apm.c - "apm", performance management: CPU/GPU clock settings the game asks for.
 * We have no clocks to change, so we just remember the setting.
 * switchbrew.org/wiki/PPC_services#apm
 */
#include "services.h"

static uint32_t configuration[2] = { 0x00010000, 0x00020000 };   /* per mode: normal, boost */

static void session_dispatch(ServiceObject *self, IpcCall *call) {
    uint32_t mode = ipc_in32(call, 0) & 1;
    switch (call->cmd) {
    case 0:                     /* SetPerformanceConfiguration(mode, config) */
        configuration[mode] = ipc_in32(call, 4);
        break;
    case 1:                     /* GetPerformanceConfiguration(mode) -> config */
        ipc_out32(call, configuration[mode]);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType session_type = { "apm:ISession", session_dispatch, NULL };

static void apm_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* OpenSession -> ISession */
        ipc_out_object(call, ipc_new_object(&session_type, NULL));
        break;
    case 1:                     /* GetPerformanceMode -> 0 normal (handheld), 1 boost (docked) */
        ipc_out32(call, applet_handheld() ? 0 : 1);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType apm_type = { "apm", apm_dispatch, NULL };

ServiceObject *service_apm_create(void) { return ipc_new_object(&apm_type, NULL); }
