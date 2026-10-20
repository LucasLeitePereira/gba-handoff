/* Glue between jni_bridge.c (owns the core) and link_jni.c (owns the phone_link engine). */
#ifndef APP_CORE_H
#define APP_CORE_H

#include "core_host.h"
#include "phone_link.h"

/* jni_bridge.c */
core_host_ctx *appcore_ctx(void);

/* link_jni.c (no-ops until LinkClient.nativeInit created the engine) */
phone_link *link_jni_engine(void);
void link_jni_game_loaded(void);
void link_jni_game_unloaded(void);
void link_jni_running(int running);

#endif
