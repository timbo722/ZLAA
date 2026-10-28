/*
 * misc.c - small services that only need to say "nothing to see here".
 *
 *   pctl   parental controls: no restrictions are set
 *   aoc:u  add-on content (DLC): none installed
 *   mm:u   multimedia clock requests (video decoding): accepted, nothing to change
 *   prepo  play reports (usage statistics for Nintendo): accepted and dropped. The pause menu
 *          sends one when its tabs change: unimplemented, that ended the game.
 *
 * switchbrew.org/wiki/Parental_Control_services, switchbrew.org/wiki/NS_Services#aoc:u
 */
#include "services.h"
#include "kernel.h"

/* ---- pctl ---- */
static void pctl_service_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 1:                     /* Initialize */
    case 1001:                  /* CheckFreeCommunicationPermission: allowed */
    case 1002:                  /* ConfirmLaunchApplicationPermission */
    case 1006:                  /* IsFreeCommunicationAvailable: yes (success) */
    case 1013:                  /* ConfirmStereoVisionPermission */
    case 1064:                  /* ResetConfirmedStereoVisionPermission */
        break;
    case 1031:                  /* IsRestrictionEnabled -> u8: no */
    case 1032:                  /* GetSafetyLevel -> u32: none */
    case 1403:                  /* IsPairingActive -> u8: no */
        ipc_out32(call, 0);
        break;
    case 1065:                  /* IsStereoVisionPermitted -> u8: yes */
        ipc_out32(call, 1);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType pctl_service_type = { "pctl:IParentalControlService", pctl_service_dispatch, NULL };

static void pctl_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* CreateService(pid) */
    case 1:                     /* CreateServiceWithoutInitialize(pid) */
        ipc_out_object(call, ipc_new_object(&pctl_service_type, NULL));
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType pctl_type = { "pctl", pctl_dispatch, NULL };

ServiceObject *service_pctl_create(void) { return ipc_new_object(&pctl_type, NULL); }

/* ---- aoc:u ---- */
static void aoc_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 2:                     /* CountAddOnContent(pid) -> u32 */
    case 3:                     /* ListAddOnContent(offset, count, pid) -> u32 count written */
        ipc_out32(call, 0);
        break;
    case 5:                     /* GetAddOnContentBaseId(pid) -> u64: DLC ids are program id + 0x1000 */
        ipc_out64(call, (kproc.program_id & ~0xFFFull) + 0x1000);
        break;
    case 7:                     /* PrepareAddOnContent */
        break;
    case 8:                     /* GetAddOnContentListChangedEvent */
        ipc_out_handle(call, kevent_readable_handle(kevent_new("aoc: list changed")), 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType aoc_type = { "aoc:u", aoc_dispatch, NULL };

ServiceObject *service_aoc_create(void) { return ipc_new_object(&aoc_type, NULL); }

/* ---- mm:u ---- */
static void mm_dispatch(ServiceObject *self, IpcCall *call) {
    static uint32_t next_id = 1;
    switch (call->cmd) {
    case 0: case 1: case 2: case 5: case 6:   /* InitializeOld, FinalizeOld, SetAndWaitOld, Finalize, SetAndWait */
        break;
    case 3: case 7:                           /* GetOld, Get -> current clock setting */
        ipc_out32(call, 0);
        break;
    case 4:                                   /* Initialize(module, priority, clear mode) -> request id */
        ipc_out32(call, next_id++);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType mm_type = { "mm:u", mm_dispatch, NULL };

ServiceObject *service_mm_create(void) { return ipc_new_object(&mm_type, NULL); }

/* ---- prepo:u / a / s / m ---- */
static void prepo_dispatch(ServiceObject *self, IpcCall *call) {
    (void)self;
    switch (call->cmd) {
    case 10300:                 /* GetTransmissionStatus -> u32: idle */
        ipc_out32(call, 0);
        break;
    case 10400:                 /* GetSystemSessionId -> u64 */
        ipc_out64(call, 0);
        break;
    default:                    /* SaveReport(WithUser)(Old/Old2), RequestImmediateTransmission, ...: dropped */
        break;
    }
}
static const ServiceType prepo_type = { "prepo", prepo_dispatch, NULL };

ServiceObject *service_prepo_create(void) { return ipc_new_object(&prepo_type, NULL); }
