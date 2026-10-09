#include "lsp.h"
#include "zinc.h"
#include "zvisit.h"

LspResponse *lsp_initialize(LspContext *ctx) {
    if (!ctx) return NULL;
    Allocator *a = ctx->allocator;
    Json *result        = JsonMap(a, NULL);
    Json *capabilities  = JsonMap(a, NULL);
    Json *serverInfo    = JsonMap(a, NULL);
    Json *workspace     = JsonMap(a, NULL);
    Json *workspaceRT   = JsonMap(a, NULL);

    Json *JsonTrue = JsonBool(a, true);

    JsonSet(a, result,         "capabilities",             capabilities);
    JsonSet(a, result,         "serverInfo",               serverInfo);

    JsonSet(a, capabilities,   "textDocumentSync",         JsonNumber(a, 1));
    JsonSet(a, capabilities,   "hoverProvider",            JsonTrue);
    JsonSet(a, capabilities,   "definitionProvider",       JsonTrue);
    JsonSet(a, capabilities,   "completionProvider",       JsonMap(a, NULL));
    JsonSet(a, capabilities,   "workspace",                workspace);

    JsonSet(a, serverInfo,     "name",                     JsonString(a, "zinc-lsp"));
    JsonSet(a, serverInfo,     "version",                  JsonString(a, "0.2.0"));

    JsonSet(a, workspace,      "workspaceFolders",         JsonTrue);
    JsonSet(a, workspace,      "configuration",            JsonTrue);
    JsonSet(a, workspace,      "didChangeWatchedFiles",    workspaceRT);

    JsonSet(a, workspaceRT,    "dynamicRegistration",      JsonTrue);

    return lsp_reply(ctx, result);
}

static LspResponse *lsp_shutdown(LspContext *ctx) {
    freestate(ctx->state);
    ctx->state = NULL;
    aclose(ctx->allocator);
    ctx->allocator = NULL;

    return NULL;
}

static Json *get_completions(Allocator *allocator, LspCompletionItem *list) {
    Json *items = JsonList(allocator, NULL);
    for (usize i = 0; i < veclen(list); i++) {
        Json *item = JsonMap(allocator, NULL);
        JsonSet(allocator, item, "label", JsonString(allocator, list[i].label));
        JsonSet(allocator, item, "kind", JsonNumber(allocator, list[i].kind));
        JsonPush(allocator, items, item);
    }
    return items;
}

LspResponse *lsp_completion(LspContext *ctx) {
    LspCompletionItem *items = vecnew(ctx->state->allocator, LspCompletionItem);

    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_CLASS}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_FUNCTION}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_PROPERTY}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_REFERENCE}));

    Json *completions = get_completions(ctx->allocator, items);
    return lsp_reply(ctx, completions);
}

LspResponse *lsp_change_document(LspContext *ctx) {
    Json *params    = JsonGet(ctx->root, "params");
    Json *textDoc   = JsonGet(params, "textDocument");
    Json *content   = JsonGetFmt(params, "contentChanges.0");
    if (!content) return NULL;

    char *text  = JsonAsString(JsonGet(content, "text"));
    char *uri   = JsonAsString(JsonGet(textDoc, "uri"));
    int version = (int)JsonAsNum(JsonGet(textDoc, "version"));

    lsp_analyze(ctx, uri, text, version);

    return NULL;
}

static LspResponse *lsp_workspace_configure(LspContext *ctx) {
    return NULL;
}

static LspHandler Handlers[] = {
    { "initialize",                 lsp_initialize          },
    { "shutdown",                   lsp_shutdown            },
    { "textDocument/didOpen",       lsp_open_document       },
    { "textDocument/didChange",     lsp_change_document     },
    { "textDocument/completion",    lsp_completion          },
    { "textDocument/hover",         lsp_hover               },
    { "workspace/configure",        lsp_workspace_configure },
    { NULL,                         NULL                    }
};

LspResponse *handle_message(LspContext *ctx) {
    const char *method = JsonAsString(JsonGet(ctx->root, "method"));
    if (!method) return NULL;

    LspHandler *handler = Handlers;

    while (handler->name) {
        if (strcmp(method, handler->name) == 0) {
            return handler->callback(ctx);
        }
        handler++;
    }

    return NULL;
}

void get_token(LspContext *ctx, LspModule *current, LspPosition pos) {
}

inline int get_module(LspContext *ctx, const char *uri) {
    LspModule **modules = ctx->modules;
    for (usize i = 0; i < veclen(modules); i++)
        if (strcmp(modules[i]->uri, uri) == 0)
            return (int)i;
    return -1;
}

inline void put_module(LspContext *ctx, LspModule *mod) {
    int cached = get_module(ctx, mod->uri);
    if (cached != -1) {
        memcpy(ctx->modules + cached, mod, sizeof(LspModule));
    } else {
        mod = copy(ctx->allocator, LspModule, *mod);
        vecpush(ctx->modules, mod);
    }
}

void lsp_analyze(LspContext *ctx, const char *uri, char *text, int version) {
    char *path = strncmp(uri, "file://", 7) == 0 ? (char *)uri + 7 : (char *)uri;
    ZState *state = makestate();
    useAllocator(state->allocator);
    visit(state, path);
    initPrimitiveTypes(state);

    ZToken **tokens = ztokenizeSource(state, text);
    ZNode *root = zparse(state, tokens);
    if (canAdvance(state)) {
        zanalyze(state, root);
    }
    freestate(ctx->state);
    ctx->state = state;

    put_module(ctx, &(LspModule){
        .allocator = state->allocator,
        .uri = uri, .src = text,
        .root = root,
        .tokens = tokens
    });
    publish_diagnostics(ctx, uri, version);
}

typedef struct {
    LspPosition pos;
    ZNode *node;
} ZPositionLookup;

static inline int compare_token_position(ZToken *tok, LspPosition pos) {
    int line = (int)tok->row - 1;
    if (line != pos.line) return line - pos.line;

    usize len = tok->end - tok->start;
    usize end = tok->col;

    if (end <= (usize)pos.character) return -1;
    if (end - len > (usize)pos.character) return 1;
    return 0;
}

static ZVisitorState visit_position(ZNode *node, ZClientData data) {
    ZPositionLookup *res = data;
#if Z_LSP
    if (!node->start || !node->end) return  Z_CONTINUE;

    LspPosition pos = res->pos;

    if (compare_token_position(node->start, pos) < 0 ||
        compare_token_position(node->end,   pos) > 0) {
        return Z_BREAK;
    }
    res->node = node;
#endif

    return Z_CONTINUE;
}

ZNode *node_from_position(LspModule *mod, LspPosition pos) {
    ZPositionLookup res = {
        .pos = pos, .node = NULL
    };
    ZVisitor v = {
        .visit = visit_position,
        .clientData = &res
    };

    visitNode(&v, mod->root);

    return res.node;
}

ZToken *token_from_position(LspModule *mod, LspPosition pos) {
    usize len = veclen(mod->tokens);

    if (len == 0) return NULL;
    usize start = 0, end = len;

    ZToken **toks = mod->tokens;
    while (start <= end) {
        usize mid = start + (end - start) / 2;

        int comp = compare_token_position(toks[mid], pos);
        if (comp == 0) return toks[mid];
        else if (comp > 0) start = mid + 1;
        else end = mid;
    }

    return start > 0 ? toks[start - 1] : NULL;
}
