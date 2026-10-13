// Captures the engine's UE_LOG output (FMsg::Logf_Internal) into our log, since shipping writes no log file.
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

#define ADDR_LOGF_INTERNAL VA(0x142411DB0ull) // FMsg::Logf_Internal(File, Line, const FName& Cat, Verbosity, Fmt, ...)
static const uint8_t SIG_LOGF[] = {0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0xb8,0x78,0x24,0x00,0x00};

typedef void (*LogfFn)(const char *file, int line, const FName *cat, uint8_t verb, const wchar_t *fmt, ...);
static LogfFn orig_logf;
static const char *VERB[] = {"?", "Fatal", "Error", "Warning", "Display", "Log", "Verbose", "VeryVerbose"};

static void logf_detour(const char *file, int line, const FName *cat, uint8_t verb, const wchar_t *fmt, ...) {
    static __thread wchar_t wbuf[4096];
    static __thread char buf[4096], cname[128];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(wbuf, 4095, fmt, ap);
    va_end(ap);
    wbuf[4095] = 0;
    size_t i = 0;
    for (; wbuf[i] && i < sizeof buf - 1; i++) buf[i] = wbuf[i] < 128 ? (char)wbuf[i] : '?';
    buf[i] = 0;
    ue_name(*cat, cname, sizeof cname);
    if (strstr(buf, "b4bcoopaddons=")) log_redact_addons(buf);   // login URLs of older clients: their add-ons (#35)
    LOG("UE %s %s: %s", cname, VERB[(verb & 0xF) < 8 ? verb & 0xF : 0], buf);
    if ((verb & 0xF) == 2 /*Error*/ && !strcmp(cname, "LogDTLSHandler")) { travel_on_handshake_failed(); cmds_join_failed("DTLS handshake"); }
    if (!strcmp(cname, "LogDTLSHandler") && strstr(buf, "Handshaking completed")) cmds_join_answered();
    steamnet_on_log(cname, buf);
    if (!strcmp(cname, "LogNet") && strstr(buf, "NetworkFailure: PendingConnectionFailure") && strstr(buf, "Server full."))
        cmds_auto_join_backoff(60);
    if (!strcmp(cname, "LogNet") && !strncmp(buf, "NetworkFailure: ", 16)) cmds_join_failed(buf + 16);   // before chat.c's stop/backoff
    // an established connection to our host is gone (it crashed or its quit didn't reach us; joins and follows only ever
    // fail as PendingConnectionFailure): host hand-off (handoff.c)
    if (!strcmp(cname, "LogNet") && (strstr(buf, "FailureType = ConnectionLost") || strstr(buf, "FailureType = ConnectionTimeout")))
        handoff_host_lost("connection lost");
    if (!strcmp(cname, "LogNet") && strstr(buf, "NetworkFailure: PendingConnectionFailure")) chat_on_join_failed(buf);
    orig_logf(file, line, cat, verb, L"%s", wbuf);
}

int uelog_init(void) {
    if (memcmp((void *)ADDR_LOGF_INTERNAL, SIG_LOGF, sizeof SIG_LOGF)) { LOG("uelog: signature mismatch"); return -1; }
    if (MH_CreateHook((void *)ADDR_LOGF_INTERNAL, (void *)logf_detour, (void **)&orig_logf) != MH_OK ||
        MH_EnableHook((void *)ADDR_LOGF_INTERNAL) != MH_OK) { LOG("uelog: hook failed"); return -1; }
    LOG("uelog: engine log hooked");
    return 0;
}
