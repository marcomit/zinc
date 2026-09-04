#ifndef ZCLI_H
#define ZCLI_H

#include "zinc.h"

typedef enum {
    Z_OK = 0,
    Z_INVALID_COMMAND,
    Z_INVALID_STATE,
    Z_LEXICAL_ERROR,
    Z_SYNTAX_ERROR,
    Z_SEMANTIC_ERROR,
    Z_CODEGEN_ERROR,
} ZErrorCode;

typedef enum {
    Z_CMD_NEEDS_INPUT = 1 << 0x00
} ZCliFlags;

typedef ZErrorCode (*ZCliCallback)(ZState *);
typedef struct ZCliCommand ZCliCommand;
struct ZCliCommand {
    const char      *name;
    const char      *summary;
    struct option   *options;

    int minArgs;
    int maxArgs;

    ZCliCallback    callback;
    ZCliCommand     *subcommand;

    /* The number of arguments this command expect.
     * e.g. the command build needs the file as input. */
    int             argc;
};


bool loadOptions(ZState *, const ZCliCommand *, int, char **);
const ZCliCommand *getCmd(int *, char ***);
void usage(char *);

#endif //!ZCLI_H
