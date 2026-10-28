/*
 * time.c - "time:u" (also time:a, time:s): clocks and time zones.
 *
 *   ISystemClock     wall-clock time, POSIX seconds (user / network / local clocks)
 *   ISteadyClock     seconds since boot, never goes backwards
 *   ITimeZoneService POSIX time <-> calendar date, in the device's own time zone (the C library's
 *                    localtime/mktime: on Android the system setting), so no Switch time-zone
 *                    files are needed. (UTC showed save times hours off.)
 *
 * switchbrew.org/wiki/Glue_services#time:a.2C_time:s.2C_time:u
 */
#define _DEFAULT_SOURCE          /* tm_zone, tm_gmtoff */
#include "services.h"
#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Which steady clock a time point came from: any fixed 16 bytes */
static const uint8_t clock_source_id[16] = { 'L', 'A', 'A', 'R', 'e', 'c', 'o', 'm', 'p', 1, 2, 3, 4, 5, 6, 7 };

static int64_t steady_seconds(void) { return (int64_t)(recomp_cntpct() / 19200000); }

/* SteadyClockTimePoint: i64 seconds, 16-byte clock source id */
static void out_time_point(IpcCall *call) {
    ipc_out64(call, (uint64_t)steady_seconds());
    ipc_out_bytes(call, clock_source_id, 16);
}

/* ---- the shared page the game reads clocks from (time:u cmd 20) ----
 * Each value is a "lock-free atomic": u32 counter, then two copies; the reader uses copy
 * (counter & 1). We write both copies once and never change them: the clocks keep moving
 * because the game adds the current tick count to these offsets itself. */
static KSharedMemory *time_shm;

static void put_system_clock_context(uint8_t *p) {   /* counter, then 2 x SystemClockContext */
    int64_t offset = (int64_t)time(NULL) - steady_seconds(), point = steady_seconds();
    for (int k = 0; k < 2; k++) {
        memcpy(p + 8 + 0x20 * k, &offset, 8);
        memcpy(p + 16 + 0x20 * k, &point, 8);
        memcpy(p + 24 + 0x20 * k, clock_source_id, 16);
    }
}

static KSharedMemory *time_shared_memory(void) {
    if (time_shm) return time_shm;
    time_shm = kshm_new(0x1000);
    uint8_t *p = time_shm->host;
    for (int k = 0; k < 2; k++)                /* 0x00: steady clock: 0 ns offset + clock id */
        memcpy(p + 8 + 0x18 * k + 8, clock_source_id, 16);
    put_system_clock_context(p + 0x38);        /* 0x38: local system clock */
    put_system_clock_context(p + 0x80);        /* 0x80: network system clock */
    return time_shm;                           /* 0xC8: automatic correction: off (zeros) */
}

/* ---- ISystemClock ---- */
static void system_clock_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* GetCurrentTime -> i64 POSIX seconds */
        ipc_out64(call, (uint64_t)time(NULL));
        break;
    case 1: case 3:             /* SetCurrentTime, SetSystemClockContext: ignore */
        break;
    case 2:                     /* GetSystemClockContext -> offset, time point (now = steady + offset) */
        ipc_out64(call, (uint64_t)((int64_t)time(NULL) - steady_seconds()));
        out_time_point(call);
        break;
    case 4:                     /* GetOperationEventReadableHandle */
        ipc_out_handle(call, kevent_readable_handle(kevent_new("time: clock operation")), 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType system_clock_type = { "time:ISystemClock", system_clock_dispatch, NULL };

/* ---- ISteadyClock ---- */
static void steady_clock_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                     /* GetCurrentTimePoint */
        out_time_point(call);
        break;
    case 2: case 200:           /* GetTestOffset, GetInternalOffset -> i64 */
        ipc_out64(call, 0);
        break;
    case 100:                   /* GetRtcValue -> i64 */
        ipc_out64(call, (uint64_t)time(NULL));
        break;
    case 101:                   /* IsRtcResetDetected -> u8 */
    case 102:                   /* GetSetupResultValue -> result */
        ipc_out32(call, 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType steady_clock_type = { "time:ISteadyClock", steady_clock_dispatch, NULL };

/* ---- ITimeZoneService ---- */
/* CalendarTime: s16 year, s8 month (1-12), day, hour, minute, second, pad
 * CalendarAdditionalInfo: u32 day of week, u32 day of year, char zone[8], u32 is_dst, s32 utc offset */
static void out_calendar(IpcCall *call, int64_t posix) {
    time_t t = (time_t)posix;
    struct tm tm;
    localtime_r(&t, &tm);
    uint8_t cal[8] = { 0 };
    int16_t year = (int16_t)(tm.tm_year + 1900);
    memcpy(cal, &year, 2);
    cal[2] = (uint8_t)(tm.tm_mon + 1);
    cal[3] = (uint8_t)tm.tm_mday;
    cal[4] = (uint8_t)tm.tm_hour;
    cal[5] = (uint8_t)tm.tm_min;
    cal[6] = (uint8_t)tm.tm_sec;
    ipc_out_bytes(call, cal, 8);
    ipc_out32(call, (uint32_t)tm.tm_wday);
    ipc_out32(call, (uint32_t)tm.tm_yday);
    char zone[8] = { 0 };
    snprintf(zone, sizeof zone, "%s", tm.tm_zone ? tm.tm_zone : "UTC");
    ipc_out_bytes(call, zone, 8);
    ipc_out32(call, tm.tm_isdst > 0);
    ipc_out32(call, (uint32_t)(int32_t)tm.tm_gmtoff);
}

static void timezone_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                   /* GetDeviceLocationName -> 0x24-byte name */
        char name[0x24] = "UTC";
        ipc_out_bytes(call, name, sizeof name);
        break;
    }
    case 1:                     /* SetDeviceLocationName */
        break;
    case 2:                     /* GetTotalLocationNameCount -> u32 */
        ipc_out32(call, 1);
        break;
    case 4: {                   /* LoadTimeZoneRule(name) -> rule in B buffer: we only have UTC */
        IpcBuffer b = ipc_out_buffer(call, 0);
        void *p = ipc_guest_ptr(b.addr, b.size);
        if (p) memset(p, 0, b.size);
        break;
    }
    case 100: case 101:         /* ToCalendarTime(time, rule) / ToCalendarTimeWithMyRule(time) */
        out_calendar(call, (int64_t)ipc_in64(call, 0));
        break;
    case 201: case 202: {       /* ToPosixTime(calendar, rule) -> count, times in B buffer */
        int16_t year;
        memcpy(&year, call->in, 2);
        struct tm tm = { 0 };
        tm.tm_year = year - 1900;
        tm.tm_mon = call->in[2] - 1;
        tm.tm_mday = call->in[3];
        tm.tm_hour = call->in[4];
        tm.tm_min = call->in[5];
        tm.tm_sec = call->in[6];
        tm.tm_isdst = -1;                             /* (the C library works out daylight saving) */
        int64_t t = (int64_t)mktime(&tm);
        IpcBuffer b = ipc_out_buffer(call, 0);
        void *p = ipc_guest_ptr(b.addr, 8);
        if (p && b.size >= 8) memcpy(p, &t, 8);
        ipc_out32(call, 1);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType timezone_type = { "time:ITimeZoneService", timezone_dispatch, NULL };

/* ---- time:u ---- */
static void time_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: case 1: case 4: case 5:   /* Get{User,Network,Local,EphemeralNetwork}SystemClock */
        ipc_out_object(call, ipc_new_object(&system_clock_type, NULL));
        break;
    case 2:                           /* GetStandardSteadyClock */
        ipc_out_object(call, ipc_new_object(&steady_clock_type, NULL));
        break;
    case 3:                           /* GetTimeZoneService */
        ipc_out_object(call, ipc_new_object(&timezone_type, NULL));
        break;
    case 20:                          /* GetSharedMemoryNativeHandle */
        ipc_out_handle(call, kshm_handle(time_shared_memory()), 0);
        break;
    case 100:                         /* IsStandardUserSystemClockAutomaticCorrectionEnabled -> u8 */
    case 200:                         /* IsStandardNetworkSystemClockAccuracySufficient -> u8 */
        ipc_out32(call, 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType time_type = { "time:u", time_dispatch, NULL };

ServiceObject *service_time_create(void) { return ipc_new_object(&time_type, NULL); }
