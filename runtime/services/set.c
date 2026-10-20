/*
 * set.c - "set" (settings an application may read: languages, region) and "set:sys"
 * (system settings: firmware version, colour theme).
 * switchbrew.org/wiki/Settings_services
 */
#include "services.h"
#include <string.h>

/* Language codes are the language's name, as up to 8 ASCII bytes in a u64. The index in
 * this list is the Switch's language number (0 = Japanese, 1 = American English...). */
static const char *const languages[] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
    "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant",
};
#define N_LANGUAGES_V1 15                      /* GetAvailableLanguageCodes (old) lists 15 */
#define N_LANGUAGES ((int)(sizeof languages / sizeof *languages))
#define SYSTEM_LANGUAGE 1                      /* en-US */
#define REGION_USA 1

static uint64_t language_code(int index) {
    uint64_t v = 0;
    memcpy(&v, languages[index], strlen(languages[index]));
    return v;
}

static void list_languages(IpcCall *call, int max) {
    IpcBuffer b = ipc_out_buffer(call, 0);
    uint8_t *p = ipc_guest_ptr(b.addr, b.size);
    int n = 0;
    for (; p && n < max && (uint64_t)(n + 1) * 8 <= b.size; n++) {
        uint64_t code = language_code(n);
        memcpy(p + 8 * n, &code, 8);
    }
    ipc_out32(call, (uint32_t)n);
}

static void set_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* GetLanguageCode -> u64 */
        ipc_out64(call, language_code(SYSTEM_LANGUAGE));
        break;
    case 1:                     /* GetAvailableLanguageCodes -> count, codes in C buffer */
        list_languages(call, N_LANGUAGES_V1);
        break;
    case 2: {                   /* MakeLanguageCode(index) -> u64 */
        uint32_t k = ipc_in32(call, 0);
        if (k >= (uint32_t)N_LANGUAGES) { call->result = MAKE_RESULT(105, 625); break; }
        ipc_out64(call, language_code((int)k));
        break;
    }
    case 3:                     /* GetAvailableLanguageCodeCount -> s32 */
        ipc_out32(call, N_LANGUAGES_V1);
        break;
    case 4:                     /* GetRegionCode -> s32 */
        ipc_out32(call, REGION_USA);
        break;
    case 5:                     /* GetAvailableLanguageCodes2 -> count, codes in B buffer */
        list_languages(call, N_LANGUAGES);
        break;
    case 6:                     /* GetAvailableLanguageCodeCount2 -> s32 */
        ipc_out32(call, N_LANGUAGES);
        break;
    case 8:                     /* GetQuestFlag -> bool: not a store demo unit */
        ipc_out32(call, 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType set_type = { "set", set_dispatch, NULL };

ServiceObject *service_set_create(void) { return ipc_new_object(&set_type, NULL); }

/* ---- set:sys ---- */
static void set_sys_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 3: case 4: {           /* GetFirmwareVersion(2) -> 0x100-byte FirmwareVersion in C/B buffer */
        IpcBuffer b = ipc_out_buffer(call, 0);
        uint8_t *p = ipc_guest_ptr(b.addr, 0x100);
        if (!p || b.size < 0x100) break;
        memset(p, 0, 0x100);
        p[0] = 8; p[1] = 1; p[2] = 0;          /* 8.1.0: the firmware of the game's era */
        p[4] = 8; p[5] = 1;                    /* revision major/minor */
        strcpy((char *)p + 0x08, "NX");
        strcpy((char *)p + 0x28, "8.1.0");
        strcpy((char *)p + 0x68, "8.1.0");
        strcpy((char *)p + 0x80, "NintendoSDK Firmware for NX 8.1.0");
        break;
    }
    case 23:                    /* GetColorSetId -> s32: 0 = light theme */
        ipc_out32(call, 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType set_sys_type = { "set:sys", set_sys_dispatch, NULL };

ServiceObject *service_set_sys_create(void) { return ipc_new_object(&set_sys_type, NULL); }
