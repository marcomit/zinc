#include "zlsp.h"
#include "zinc.h"
#include "zvisit.h"

LspCompletionKind completion_kind_from_symbol(ZSymType kind) {
    switch (kind) {
    case Z_SYM_ENUM:     return Z_LSP_ENUM;
    case Z_SYM_FACET:    return Z_LSP_INTERFACE;
    case Z_SYM_FOREIGN:  return Z_LSP_CLASS;
    case Z_SYM_FUNC:     return Z_LSP_FUNCTION;
    case Z_SYM_GENERIC:  return Z_LSP_VALUE;
    case Z_SYM_IMPORT:   return Z_LSP_FILE;
    case Z_SYM_STRUCT:   return Z_LSP_CLASS;
    case Z_SYM_TYPEDEF:  return Z_LSP_UNIT;
    case Z_SYM_VAR:      return Z_LSP_VARIABLE;
    }
}

Json *completions2Json(Allocator *a, LspCompletionItem *list) {
    Json *items = JsonList(a, NULL);
    for (usize i = 0; i < veclen(list); i++) {
        Json *item = JsonMap(a, NULL);
        JsonSet(a, item, "label",           JsonString(a, list[i].label));
        JsonSet(a, item, "kind",            JsonNumber(a, list[i].kind));

        if (list[i].docs.kind && list[i].docs.value) {
            Json *docs = JsonMap(a, NULL);
            JsonSet(a, docs, "kind",            JsonString(a, list[i].docs.kind));
            JsonSet(a, docs, "value",           JsonString(a, list[i].docs.value));
            JsonSet(a, item, "documentation",   docs);
        }

        JsonSet(a, item, "detail",          JsonString(a, list[i].detail));
        JsonPush(a, items, item);
    }
    return items;
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
        *ctx->modules[cached] = *mod;
    } else {
        mod = copy(ctx->allocator, LspModule, *mod);
        vecpushwith(ctx->modules, mod, ctx->allocator);
    }
}

void lsp_analyze(LspContext *ctx, const char *uri, char *text, int version) {
    ZState *state   = makestate();
    Allocator *prev = useAllocator(state->allocator);
    uri     = zstrdup(state->allocator, (char *)uri);
    text    = zstrdup(state->allocator, text);
    char *path = strncmp(uri, "file://", 7) == 0 ? (char *)uri + 7 : (char *)uri;
    visit(state, path);
    initPrimitiveTypes(state);

    ZToken **tokens = ztokenizeSource(state, text);
    ZNode *root     = zparse(state, tokens);
    ZSemantic *sem  = NULL;
    if (canAdvance(state)) {
        sem = zanalyze(state, root);
    }

    put_module(ctx, &(LspModule){
        .allocator  = state->allocator,
        .uri        = zstrdup(state->allocator, (char *)uri),
        .src        = zstrdup(state->allocator, text),
        .root       = root,
        .tokens     = tokens,
        .semantic   = sem
    });

    freestate(ctx->state);
    ctx->state = state;
    publish_diagnostics(ctx, uri, version);
    useAllocator(prev);
}

typedef struct {
    LspPosition pos;
    ZNode *node;
    ZScope *scope;
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

    if (compare_token_position(node->start, pos) > 0 ||
        compare_token_position(node->end,   pos) < 0) {
        return Z_BREAK;
    }
    res->node = node;
    if (node->type == NODE_BLOCK && node->scope) res->scope = node->scope;
#endif

    return Z_CONTINUE;
}

ZScope *scope_from_position(LspModule *mod, LspPosition pos) {
    ZPositionLookup res = {
        .pos = pos, .node = NULL, .scope = NULL
    };
    ZVisitor v = {
        .visit = visit_position,
        .clientData = &res
    };

    visitNode(&v, mod->root);

    return res.scope;
}

ZNode *node_from_position(LspModule *mod, LspPosition pos) {
    ZPositionLookup res = {
        .pos = pos, .node = NULL, .scope = NULL
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
    while (start < end) {
        usize mid = start + (end - start) / 2;

        int comp = compare_token_position(toks[mid], pos);
        if (comp == 0) return toks[mid];
        else if (comp < 0) start = mid + 1;
        else end = mid;
    }

    return start > 0 ? toks[start - 1] : NULL;
}

LspPositionContext get_position_context(LspModule *mod, LspPosition pos) {
    ZPositionLookup res = {
        .pos = pos, .node = NULL, .scope = NULL
    };
    ZVisitor v = {
        .visit = visit_position,
        .clientData = &res
    };

    visitNode(&v, mod->root);

    ZSymbol *symbol = NULL;
    if (res.scope) {
        for (usize i = 0; i < veclen(res.scope->symbols); i++) {
            if (res.scope->symbols[i]->node == res.node) {
                symbol = res.scope->symbols[i];
                break;
            }
        }
    }

    return (LspPositionContext){
        .token  = token_from_position(mod, pos),
        .node   = res.node,
        .symbol = symbol,
        .scope  = res.scope,
    };
}

static void add_item(char *name, ZType *type, ZNode *node, LspCompletionKind kind, void *ud) {
    LspCompletionItem **items = ud;

    vecpush(*items, ((LspCompletionItem){
        .label = name,
        .detail = "",
        .label_details = { .detail = "", .description = "" },
        .kind = kind, .docs = { .kind = "markdown", .value = "" }
    }));
}

static void get_field_completions(LspModule *ctx,
                                  LspPositionContext *p,
                                  LspCompletionItem **items) {
    if (!p->node) return;
    zforEachMember(ctx->semantic, ctx->root, p->node, add_item, items);
}

LspCompletionItem *get_completions(LspContext *ctx, LspModule *module, LspPositionContext p) {
    LspCompletionItem *items = vecnew(ctx->allocator, LspCompletionItem);
    ZScope *scope = p.scope;
    while (scope) {
        for (usize i = 0; i < veclen(scope->symbols); i++) {
            ZSymbol *s  = scope->symbols[i];
            char *type  = stype(s->node->resolved);
            vecpush(items, ((LspCompletionItem){
                .label  = stoken(s->name),
                .kind   = completion_kind_from_symbol(s->kind),
                .detail = type,
                .label_details = {
                    .detail         = type,
                    .description    = type
                },
                .docs   = { .kind = "markdown", .value = NULL }
            }));
        }
        scope = scope->parent;
    }
    scope = p.scope;

    get_field_completions(module, &p, &items);

    if (p.node) {
        char buff[24] = { 0 };
        int len = snprintf(buff, 23, "%d", p.node->type);
        log_msg("COMPLETION TOK", buff, len);
    }

    return items;
}
