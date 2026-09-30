#ifndef LSP_H
#define LSP_H

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#include "zinc.h"
#include "json.c/json.h"

typedef struct {
    Json *root;
} LspContext;

typedef struct {
    Json *response;
    int id;
} LspResponse;

LspResponse *handle_message(LspContext *);

typedef enum {
    Z_LSP_TEXT,
    Z_LSP_METHOD,
    Z_LSP_FUNCTION,
    Z_LSP_CONSTRUCTOR,
    Z_LSP_FIELD,
    Z_LSP_VARIABLE,
    Z_LSP_CLASS,
    Z_LSP_INTERFACE,
    Z_LSP_MODULE,
    Z_LSP_PROPERTY,
    Z_LSP_UNIT,
    Z_LSP_VALUE,
    Z_LSP_ENUM,
    Z_LSP_KEYWORD,
    Z_LSP_SNIPPET,
    Z_LSP_COLOR,
    Z_LSP_FILE,
    Z_LSP_REFERENCE,
    Z_LSP_FOLDER,
    Z_LSP_ENUM_MEMBER,
    Z_LSP_CONSTANT,
    Z_LSP_STRUCT,
    Z_LSP_EVENT,
    Z_LSP_OPERATOR,
    Z_LSP_TYPE_PARAM,
} LspCompletionKind;

typedef struct {
    char                *label;
    LspCompletionKind   kind;
} LspCompletionItem;

#endif //!LSP_H
