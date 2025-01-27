#include "sidermodule.h"

#include <string.h>
#include <strings.h>

static SiderModuleString *log_key_name;

static const char log_command_name[] = "commandfilter.log";
static const char ping_command_name[] = "commandfilter.ping";
static const char retained_command_name[] = "commandfilter.retained";
static const char unregister_command_name[] = "commandfilter.unregister";
static const char unfiltered_clientid_name[] = "unfilter_clientid";
static int in_log_command = 0;

unsigned long long unfiltered_clientid = 0;

static SiderModuleCommandFilter *filter, *filter1;
static SiderModuleString *retained;

int CommandFilter_UnregisterCommand(SiderModuleCtx *ctx, SiderModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    SiderModule_ReplyWithLongLong(ctx,
            SiderModule_UnregisterCommandFilter(ctx, filter));

    return REDISMODULE_OK;
}

int CommandFilter_PingCommand(SiderModuleCtx *ctx, SiderModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    SiderModuleCallReply *reply = SiderModule_Call(ctx, "ping", "c", "@log");
    if (reply) {
        SiderModule_ReplyWithCallReply(ctx, reply);
        SiderModule_FreeCallReply(reply);
    } else {
        SiderModule_ReplyWithSimpleString(ctx, "Unknown command or invalid arguments");
    }

    return REDISMODULE_OK;
}

int CommandFilter_Retained(SiderModuleCtx *ctx, SiderModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    if (retained) {
        SiderModule_ReplyWithString(ctx, retained);
    } else {
        SiderModule_ReplyWithNull(ctx);
    }

    return REDISMODULE_OK;
}

int CommandFilter_LogCommand(SiderModuleCtx *ctx, SiderModuleString **argv, int argc)
{
    SiderModuleString *s = SiderModule_CreateString(ctx, "", 0);

    int i;
    for (i = 1; i < argc; i++) {
        size_t arglen;
        const char *arg = SiderModule_StringPtrLen(argv[i], &arglen);

        if (i > 1) SiderModule_StringAppendBuffer(ctx, s, " ", 1);
        SiderModule_StringAppendBuffer(ctx, s, arg, arglen);
    }

    SiderModuleKey *log = SiderModule_OpenKey(ctx, log_key_name, REDISMODULE_WRITE|REDISMODULE_READ);
    SiderModule_ListPush(log, REDISMODULE_LIST_HEAD, s);
    SiderModule_CloseKey(log);
    SiderModule_FreeString(ctx, s);

    in_log_command = 1;

    size_t cmdlen;
    const char *cmdname = SiderModule_StringPtrLen(argv[1], &cmdlen);
    SiderModuleCallReply *reply = SiderModule_Call(ctx, cmdname, "v", &argv[2], argc - 2);
    if (reply) {
        SiderModule_ReplyWithCallReply(ctx, reply);
        SiderModule_FreeCallReply(reply);
    } else {
        SiderModule_ReplyWithSimpleString(ctx, "Unknown command or invalid arguments");
    }

    in_log_command = 0;

    return REDISMODULE_OK;
}

int CommandFilter_UnfilteredClientId(SiderModuleCtx *ctx, SiderModuleString **argv, int argc)
{
    if (argc < 2)
        return SiderModule_WrongArity(ctx);

    long long id;
    if (SiderModule_StringToLongLong(argv[1], &id) != REDISMODULE_OK) {
        SiderModule_ReplyWithError(ctx, "invalid client id");
        return REDISMODULE_OK;
    }
    if (id < 0) {
        SiderModule_ReplyWithError(ctx, "invalid client id");
        return REDISMODULE_OK;
    }

    unfiltered_clientid = id;
    SiderModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

/* Filter to protect against Bug #11894 reappearing
 *
 * ensures that the filter is only run the first time through, and not on reprocessing
 */
void CommandFilter_BlmoveSwap(SiderModuleCommandFilterCtx *filter)
{
    if (SiderModule_CommandFilterArgsCount(filter) != 6)
        return;

    SiderModuleString *arg = SiderModule_CommandFilterArgGet(filter, 0);
    size_t arg_len;
    const char *arg_str = SiderModule_StringPtrLen(arg, &arg_len);

    if (arg_len != 6 || strncmp(arg_str, "blmove", 6))
        return;

    /*
     * Swapping directional args (right/left) from source and destination.
     * need to hold here, can't push into the ArgReplace func, as it will cause other to freed -> use after free
     */
    SiderModuleString *dir1 = SiderModule_HoldString(NULL, SiderModule_CommandFilterArgGet(filter, 3));
    SiderModuleString *dir2 = SiderModule_HoldString(NULL, SiderModule_CommandFilterArgGet(filter, 4));
    SiderModule_CommandFilterArgReplace(filter, 3, dir2);
    SiderModule_CommandFilterArgReplace(filter, 4, dir1);
}

void CommandFilter_CommandFilter(SiderModuleCommandFilterCtx *filter)
{
    unsigned long long id = SiderModule_CommandFilterGetClientId(filter);
    if (id == unfiltered_clientid) return;

    if (in_log_command) return;  /* don't process our own RM_Call() from CommandFilter_LogCommand() */

    /* Fun manipulations:
     * - Remove @delme
     * - Replace @replaceme
     * - Append @insertbefore or @insertafter
     * - Prefix with Log command if @log encountered
     */
    int log = 0;
    int pos = 0;
    while (pos < SiderModule_CommandFilterArgsCount(filter)) {
        const SiderModuleString *arg = SiderModule_CommandFilterArgGet(filter, pos);
        size_t arg_len;
        const char *arg_str = SiderModule_StringPtrLen(arg, &arg_len);

        if (arg_len == 6 && !memcmp(arg_str, "@delme", 6)) {
            SiderModule_CommandFilterArgDelete(filter, pos);
            continue;
        } 
        if (arg_len == 10 && !memcmp(arg_str, "@replaceme", 10)) {
            SiderModule_CommandFilterArgReplace(filter, pos,
                    SiderModule_CreateString(NULL, "--replaced--", 12));
        } else if (arg_len == 13 && !memcmp(arg_str, "@insertbefore", 13)) {
            SiderModule_CommandFilterArgInsert(filter, pos,
                    SiderModule_CreateString(NULL, "--inserted-before--", 19));
            pos++;
        } else if (arg_len == 12 && !memcmp(arg_str, "@insertafter", 12)) {
            SiderModule_CommandFilterArgInsert(filter, pos + 1,
                    SiderModule_CreateString(NULL, "--inserted-after--", 18));
            pos++;
        } else if (arg_len == 7 && !memcmp(arg_str, "@retain", 7)) {
            if (retained) SiderModule_FreeString(NULL, retained);
            retained = SiderModule_CommandFilterArgGet(filter, pos + 1);
            SiderModule_RetainString(NULL, retained);
            pos++;
        } else if (arg_len == 4 && !memcmp(arg_str, "@log", 4)) {
            log = 1;
        }
        pos++;
    }

    if (log) SiderModule_CommandFilterArgInsert(filter, 0,
            SiderModule_CreateString(NULL, log_command_name, sizeof(log_command_name)-1));
}

int SiderModule_OnLoad(SiderModuleCtx *ctx, SiderModuleString **argv, int argc) {
    if (SiderModule_Init(ctx,"commandfilter",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (argc != 2 && argc != 3) {
        SiderModule_Log(ctx, "warning", "Log key name not specified");
        return REDISMODULE_ERR;
    }

    long long noself = 0;
    log_key_name = SiderModule_CreateStringFromString(ctx, argv[0]);
    SiderModule_StringToLongLong(argv[1], &noself);
    retained = NULL;

    if (SiderModule_CreateCommand(ctx,log_command_name,
                CommandFilter_LogCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (SiderModule_CreateCommand(ctx,ping_command_name,
                CommandFilter_PingCommand,"deny-oom",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (SiderModule_CreateCommand(ctx,retained_command_name,
                CommandFilter_Retained,"readonly",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (SiderModule_CreateCommand(ctx,unregister_command_name,
                CommandFilter_UnregisterCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (SiderModule_CreateCommand(ctx, unfiltered_clientid_name,
                CommandFilter_UnfilteredClientId, "admin", 1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if ((filter = SiderModule_RegisterCommandFilter(ctx, CommandFilter_CommandFilter, 
                    noself ? REDISMODULE_CMDFILTER_NOSELF : 0))
            == NULL) return REDISMODULE_ERR;

    if ((filter1 = SiderModule_RegisterCommandFilter(ctx, CommandFilter_BlmoveSwap, 0)) == NULL)
        return REDISMODULE_ERR;

    if (argc == 3) {
        const char *ptr = SiderModule_StringPtrLen(argv[2], NULL);
        if (!strcasecmp(ptr, "noload")) {
            /* This is a hint that we return ERR at the last moment of OnLoad. */
            SiderModule_FreeString(ctx, log_key_name);
            if (retained) SiderModule_FreeString(NULL, retained);
            return REDISMODULE_ERR;
        }
    }

    return REDISMODULE_OK;
}

int SiderModule_OnUnload(SiderModuleCtx *ctx) {
    SiderModule_FreeString(ctx, log_key_name);
    if (retained) SiderModule_FreeString(NULL, retained);

    return REDISMODULE_OK;
}
