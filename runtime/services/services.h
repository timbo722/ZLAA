/*
 * services.h - the OS services we provide (one file each in this folder).
 * Each service is a ServiceType with a dispatch function: switch on call->cmd, read the
 * arguments, write the reply. Command lists: switchbrew.org/wiki/Services_API.
 */
#ifndef SERVICES_H
#define SERVICES_H

#include "ipc.h"

/* The one user profile we pretend exists (a Switch user id is 16 bytes, never all zero) */
#define LAA_USER_ID_LO 0x0000000000000001ull
#define LAA_USER_ID_HI 0x4C41415245434F4Dull     /* "MOCERAAL": just recognisable */

ServiceObject *service_sm_create(void);
ServiceObject *service_lm_create(void);
ServiceObject *service_applet_oe_create(void);
ServiceObject *service_apm_create(void);
ServiceObject *service_pctl_create(void);
ServiceObject *service_aoc_create(void);
ServiceObject *service_mm_create(void);
ServiceObject *service_prepo_create(void);
ServiceObject *service_nvdrv_create(void);
ServiceObject *service_time_create(void);
ServiceObject *service_fsp_create(void);
ServiceObject *service_acc_create(void);
ServiceObject *service_set_create(void);
ServiceObject *service_set_sys_create(void);
ServiceObject *service_hid_create(void);
ServiceObject *service_vi_create(void);
ServiceObject *service_pl_create(void);
ServiceObject *service_audren_create(void);
ServiceObject *service_audout_create(void);

/* Look up a service by the name the game gives sm ("fsp-srv", "hid", ...). Services we
 * haven't written yet still open fine, and stop the program on their first command. */
ServiceObject *service_open(const char *name);

int applet_handheld(void);          /* applet.c: are we a handheld (1280x720) Switch? */
void hid_connect_controllers(void); /* hid.c: (re)connect player 1 / handheld, as the controller applet does */

#endif
