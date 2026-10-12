#ifndef LSP_H
#define LSP_H

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#include "zinc.h"
#include "zvisit.h"
#include "json.c/json.h"

extern Allocator *tempAllocator;
extern FILE *g_out;

typedef struct LspContext LspContext;
typedef struct LspResponse LspResponse;
typedef LspResponse *(*LspMethod)(LspContext *);

typedef struct {
    const char *name;
    LspMethod callback;
} LspHandler;

typedef struct {
    const char  *uri;
    const char  *src;
    ZNode       *root;
    ZToken      **tokens;
    Allocator   *allocator;
    ZSemantic   *semantic;
} LspModule;

struct LspContext {
    Json        *root;
    LspModule   **modules;
    Allocator   *allocator;
    ZState      *state;
};

struct LspResponse {
    Json *response;
    int id;
};

LspResponse *handle_message(LspContext *);

typedef struct {
    char *kind;
    char *value;
} LspDocumentation;

typedef enum {
    Z_LSP_TEXT = 1,
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

typedef void (*ZMemberFn)(
    char *name,
    ZType *type,
    ZNode *decl,
    LspCompletionKind kind,
    void *ud);

void zforEachMember(ZSemantic *ctx,
                    ZNode *module,
                    ZNode *member,
                    ZMemberFn fn,
                    void *ud);

typedef struct {
    char *detail;
    char *description;
} LspCompletionDetails;

typedef struct {
    char                    *label;
    char                    *detail;
    LspCompletionDetails    label_details;
    LspCompletionKind       kind;
    LspDocumentation        docs;
} LspCompletionItem;

typedef struct {
    int line;
    int character;
} LspPosition;

typedef struct {
    int         triggerKind;
    LspPosition position;
    char        *uri;
} LspCompletion;

typedef struct {
    char *text;
    LspPosition start;
    LspPosition end;
} LspContentChange;

typedef struct {
    LspContentChange *changes;
    int version;
    char *uri;
} LspDocumentChange;

typedef struct {
    ZToken  *token;
    ZNode   *node;
    ZSymbol *symbol;
    ZScope  *scope;
} LspPositionContext;

typedef enum {
    LSP_COMPLETION_NONE,        // Inside a string literal or a comment.
    LSP_COMPLETION_SCOPE,       // Bare local variables and keywords
    LSP_COMPLETION_IMPORT_PATH, // Inside an import path
    LSP_COMPLETION_STRUCT_LIT,  // Inside a struct literal MyStruct{ | }
    LSP_COMPLETION_TOPLEVEL,    // File-level
    LSP_COMPLETION_TYPE,        // Type requested, shows only types
    LSP_COMPLETION_MEMBER,      // obj.member
    LSP_COMPLETION_STATIC       // Type.construct
} LspCompletionContextKind;

typedef struct {
    LspCompletionContextKind    kind;
    ZToken                      *token;
    ZNode                       **path;
    ZScope                      *scope;
    ZNode                       *target;
    char                        *prefix;
} LspCompletionContext;

LspResponse *lsp_reply(LspContext *, Json *);
void log_msg(const char *, const char *, size_t);
void log_json(const char *, Json *);
void lsp_send(Json *);
void lsp_notify(Allocator *, const char *, Json *);

void lsp_analyze(LspContext *, const char *, char *, int);

LspResponse *lsp_initialize     (LspContext *);
LspResponse *lsp_shutdown       (LspContext *);
LspResponse *lsp_open_document  (LspContext *);
LspResponse *lsp_change_document(LspContext *);
LspResponse *lsp_close_document (LspContext *);
LspResponse *lsp_completion     (LspContext *);
LspResponse *lsp_hover          (LspContext *);

void publish_diagnostics(LspContext *, const char *, int);

int  get_module(LspContext *, const char *);
void put_module(LspContext *, LspModule *);

ZNode *node_from_position               (LspModule *, LspPosition);
ZToken *token_from_position             (LspModule *, LspPosition);
ZScope *scope_from_position             (LspModule *, LspPosition);
ZNode *member_from_position             (LspModule *, LspPosition);
LspPositionContext get_position_context (LspModule *, LspPosition);

Json *completions2Json(Allocator *, LspCompletionItem *);
LspCompletionKind completion_kind_from_symbol(ZSymType);
LspCompletionContext get_completion_context(LspModule *, LspPosition);
LspCompletionItem *get_completions(LspContext *, LspModule *, LspCompletionContext);

#endif //!LSP_H
