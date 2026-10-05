#pragma once
#include <stdarg.h>
#include <pspkerneltypes.h>

/* Logging remains available in every configuration. Guard individual debug-only
 * diagnostics at their call sites; this API does not enforce a logging policy. */

struct logger_t {
    SceUID log_uid;
    const char* path;
    void (*SetPath)(const char* szFileName);
    void (*Write)(const char* message);
    void (*WriteF)(const char* format, ...);
    void (*Close)();
};
extern struct logger_t logger;
