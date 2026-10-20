/*
 * acc.c - "acc:u0", user accounts. There is exactly one user, LAA_USER_ID, called "Player".
 * switchbrew.org/wiki/Account_services
 */
#include "services.h"
#include "kernel.h"
#include <string.h>

static int is_our_user(const IpcCall *call, uint32_t off) {
    return ipc_in64(call, off) == LAA_USER_ID_LO && ipc_in64(call, off + 8) == LAA_USER_ID_HI;
}

static void out_user_id(IpcCall *call) {
    ipc_out64(call, LAA_USER_ID_LO);
    ipc_out64(call, LAA_USER_ID_HI);
}

/* ProfileBase: user id (16), u64 last edited (POSIX time), nickname (0x20 chars) */
static void out_profile_base(IpcCall *call) {
    char nickname[0x20] = "Player";
    out_user_id(call);
    ipc_out64(call, 0);
    ipc_out_bytes(call, nickname, sizeof nickname);
}

/* ---- IProfile ---- */
static void profile_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                   /* Get -> ProfileBase, UserData (0x80 bytes, B buffer) */
        IpcBuffer b = ipc_out_buffer(call, 0);
        void *p = ipc_guest_ptr(b.addr, b.size);
        if (p) memset(p, 0, b.size);
        out_profile_base(call);
        break;
    }
    case 1:                     /* GetBase -> ProfileBase */
        out_profile_base(call);
        break;
    case 10:                    /* GetImageSize -> u32: no profile picture */
    case 11:                    /* LoadImage -> u32 size */
        ipc_out32(call, 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType profile_type = { "acc:IProfile", profile_dispatch, NULL };

/* ---- IManagerForApplication: online account (we are offline) ---- */
static void manager_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* CheckAvailability */
        break;
    case 1:                     /* GetAccountId -> u64 */
        ipc_out64(call, LAA_USER_ID_LO);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType manager_type = { "acc:IManagerForApplication", manager_dispatch, NULL };

/* ---- acc:u0 ---- */
static void acc_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* GetUserCount -> s32 */
        ipc_out32(call, 1);
        break;
    case 1:                     /* GetUserExistence(user) -> bool */
        ipc_out32(call, (uint32_t)is_our_user(call, 0));
        break;
    case 2: case 3: {           /* ListAllUsers, ListOpenUsers -> user ids in B buffer (up to 8) */
        IpcBuffer b = ipc_out_buffer(call, 0);
        uint8_t *p = ipc_guest_ptr(b.addr, b.size);
        if (p) {
            memset(p, 0, b.size);
            uint64_t uid[2] = { LAA_USER_ID_LO, LAA_USER_ID_HI };
            if (b.size >= 16) memcpy(p, uid, 16);
        }
        break;
    }
    case 4:                     /* GetLastOpenedUser -> user id */
        out_user_id(call);
        break;
    case 5:                     /* GetProfile(user) -> IProfile */
        ipc_out_object(call, ipc_new_object(&profile_type, NULL));
        break;
    case 50:                    /* IsUserRegistrationRequestPermitted -> bool */
    case 150:                   /* IsUserAccountSwitchLocked -> bool */
        ipc_out32(call, 0);
        break;
    case 51:                    /* TrySelectUserWithoutInteraction -> user id */
        out_user_id(call);
        break;
    case 100: case 140:         /* InitializeApplicationInfo(pid) */
    case 110: case 111:         /* StoreSaveDataThumbnail, ClearSaveDataThumbnail */
        break;
    case 101:                   /* GetBaasAccountManagerForApplication(user) */
        ipc_out_object(call, ipc_new_object(&manager_type, NULL));
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType acc_type = { "acc:u0", acc_dispatch, NULL };

ServiceObject *service_acc_create(void) { return ipc_new_object(&acc_type, NULL); }
