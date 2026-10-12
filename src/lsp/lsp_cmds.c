#include "zlsp.h"
#include "zinc.h"
#include "zvisit.h"

#include <ctype.h>
#include <string.h>

Json *range_from_token(Allocator *a, ZToken *tok) {
    Json *start = JsonMap(a, NULL);
    Json *end   = JsonMap(a, NULL);
    Json *range = JsonMap(a, NULL);

    int line    = tok ? (int)tok->row - 1 : 0;
    int col     = tok ? (int)(tok->start - tok->sourceLinePtr) : 0;
    int row     = tok ? (int)(tok->end - tok->start) : 0;

    JsonSet(a, start,  "line",         JsonNumber(a, line));
    JsonSet(a, start,  "character",    JsonNumber(a, col));

    JsonSet(a, end,    "line",         JsonNumber(a, line));
    JsonSet(a, end,    "character",    JsonNumber(a, col + row));

    JsonSet(a, range,  "start",        start);
    JsonSet(a, range,  "end",          end);

    return range;
}

LspCompletionKind completion_kind_from_symbol(ZSymType kind) {
    switch (kind) {
    case Z_SYM_ENUM:     return Z_LSP_ENUM;
    case Z_SYM_FACET:    return Z_LSP_INTERFACE;
    case Z_SYM_FOREIGN:  return Z_LSP_CLASS;
    case Z_SYM_FUNC:     return Z_LSP_FUNCTION;
    case Z_SYM_GENERIC:  return Z_LSP_TYPE_PARAM;
    case Z_SYM_IMPORT:   return Z_LSP_MODULE;
    case Z_SYM_STRUCT:   return Z_LSP_STRUCT;
    case Z_SYM_TYPEDEF:  return Z_LSP_TYPE_PARAM;
    case Z_SYM_VAR:      return Z_LSP_VARIABLE;
    }
    return Z_LSP_TEXT;
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

        if (list[i].detail)
            JsonSet(a, item, "detail",      JsonString(a, list[i].detail));
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
    { "textDocument/definition",    lsp_definition          },
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
    ZNode **path;
    ZScope *scope;
    Allocator *allocator;
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
    vecpushwith(res->path, node, res->allocator);
    if (node->type == NODE_BLOCK && node->scope) res->scope = node->scope;
#endif

    return Z_CONTINUE;
}

ZScope *scope_from_position(LspModule *mod, LspPosition pos) {
    ZPositionLookup res = {
        .pos = pos, .path = NULL, .scope = NULL, mod->allocator
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
        .pos = pos, .path = NULL, .scope = NULL, .allocator = mod->allocator
    };
    ZVisitor v = {
        .visit = visit_position,
        .clientData = &res
    };

    visitNode(&v, mod->root);

    return veclen(res.path) ? veclast(res.path) : NULL;
}

/* Index of the token under the cursor, or of the last token before it. */
static isize token_index_from_position(LspModule *mod, LspPosition pos) {
    usize len = veclen(mod->tokens);

    if (len == 0) return -1;
    usize start = 0, end = len;

    ZToken **toks = mod->tokens;
    while (start < end) {
        usize mid = start + (end - start) / 2;

        int comp = compare_token_position(toks[mid], pos);
        if (comp == 0) return (isize)mid;
        else if (comp < 0) start = mid + 1;
        else end = mid;
    }

    return (isize)start - 1;
}

ZToken *token_from_position(LspModule *mod, LspPosition pos) {
    isize i = token_index_from_position(mod, pos);
    return i >= 0 ? mod->tokens[i] : NULL;
}

typedef struct {
    LspPosition pos;
    ZNode       *member;
} ZMemberLookup;

static ZVisitorState visit_member(ZNode *node, ZClientData data) {
    ZMemberLookup *res = data;
    if (node->type != NODE_MEMBER) return Z_CONTINUE;

    ZToken *field = node->memberAccess.field;
    if (!field || !field->start || !field->end) return Z_CONTINUE;

    int len = (int)(field->end - field->start);
    int end = (int)field->col;
    if ((int)field->row - 1 == res->pos.line &&
        end - len <= res->pos.character && res->pos.character <= end) {
        res->member = node;
        return Z_STOP;
    }
    return Z_CONTINUE;
}

ZNode *member_from_position(LspModule *mod, LspPosition pos) {
    ZMemberLookup res = { .pos = pos, .member = NULL };
    ZVisitor v = {
        .visit = visit_member,
        .clientData = &res
    };

    visitNode(&v, mod->root);

    return res.member;
}

LspPositionContext get_position_context(LspModule *mod, LspPosition pos) {
    ZPositionLookup res = {
        .pos = pos, .path = NULL, .scope = NULL, .allocator = mod->allocator
    };
    ZVisitor v = {
        .visit = visit_position,
        .clientData = &res
    };

    visitNode(&v, mod->root);

    ZSymbol *symbol = NULL;
    if (res.scope && veclen(res.path) > 0) {
        for (usize i = 0; i < veclen(res.scope->symbols); i++) {
            if (res.scope->symbols[i]->node == veclast(res.path)) {
                symbol = res.scope->symbols[i];
                break;
            }
        }
    }

    return (LspPositionContext){
        .token  = token_from_position(mod, pos),
        .node   = veclen(res.path) ? veclast(res.path) : NULL,
        .symbol = symbol,
        .scope  = res.scope,
    };
}

static void add_item(char *name, ZType *type, ZNode *node, LspCompletionKind kind, void *ud) {
    LspCompletionItem **items = ud;

    char *sig = type ? stype(type) : NULL;
    vecpush(*items, ((LspCompletionItem){
        .label = name,
        .detail = "",
        .label_details = { NULL, sig },
        .kind = kind, .docs = { NULL, NULL }
    }));
}

static void list_scope(LspCompletionItem **items, ZScope *scope, int mask) {
    for (; scope; scope = scope->parent) {
        for (usize i = 0; i < veclen(scope->symbols); i++) {
            ZSymbol *sym = scope->symbols[i];
            if (!sym || !(sym->kind & mask)) continue;
            char *type = sym->node && sym->node->resolved ?
                stype(sym->node->resolved) : NULL;
            vecpush(*items, ((LspCompletionItem){
                .kind = completion_kind_from_symbol(sym->kind),
                .label = stoken(sym->name),
                .detail = type,
                .label_details = { type, type },
                .docs = { NULL, NULL }
            }));
        }
    }
}

static void list_keywords(LspCompletionItem **items, int mask) {
    struct { char *name; int mask; } keywords[] = {
#define TOK_FLOWS
#define TOK_TYPES
#define DEF(type, name, mask) { name, mask },
#include "ztok.def"
#undef DEF
#undef TOK_TYPES
#undef TOK_FLOWS
    };

    for (usize i = 0; i < (sizeof(keywords)/sizeof(keywords[0])); i++) {
        if (!(keywords[i].mask & mask)) continue;
        vecpush(*items, ((LspCompletionItem){
            .kind = Z_LSP_KEYWORD,
            .label = keywords[i].name,
            .detail = NULL,
            .label_details = { NULL, NULL },
            .docs = { NULL, NULL }
        }));
    }
}

static void list_struct_fields(LspCompletionItem **items, ZNode *lit, ZToken *exclude) {
    if (!lit || lit->type != NODE_STRUCT_LIT) return;
    ZType *type = lit->resolved;
    if (!type || type->kind != Z_TYPE_STRUCT) return;
    hashset_t seen = NULL;

    for (usize i = 0; i < veclen(lit->structlit.fields); i++) {
        ZNode *field = lit->structlit.fields[i];
        if (field->tok == exclude) continue;
        hashset_insert(&seen, stoken(field->tok));
    }

    ZNode **fields = type->strct.fields;
    for (usize i = 0; i < veclen(fields); i++) {
        char *field = stoken(fields[i]->tok);
        if (hashset_has(seen, field)) continue;
        char *detail = fields[i]->resolved ? stype(fields[i]->resolved) : NULL;
        vecpush(*items, ((LspCompletionItem){
            .kind = Z_LSP_FIELD,
            .label = field,
            .detail = detail,
            .label_details = { NULL, NULL },
            .docs = { NULL, NULL }
        }));
    }
}

static void list_import_paths(LspCompletionItem **items, char *prefix) {

}

/* ================== Completion context ================== */

static bool token_touches(ZToken *tok, LspPosition pos) {
    if (!tok || (int)tok->row - 1 != pos.line) return false;
    int end     = (int)tok->col;
    int start   = end - (int)(tok->end - tok->start);
    return start <= pos.character && pos.character <= end;
}


static const char *line_prefix(LspModule *mod, LspPosition pos, usize *len) {
    const char *s = mod->src;
    if (!s) return NULL;

    for (int line = 0; line < pos.line; line++) {
        s = strchr(s, '\n');
        if (!s) return NULL;
        s++;
    }

    usize n = 0;
    while (s[n] && s[n] != '\n' && n < (usize)pos.character) n++;
    *len = n;
    return s;
}

static bool in_line_comment(const char *line, usize len) {
    bool str = false;
    for (usize i = 0; i < len; i++) {
        if (str && line[i] == '\\')     { i++; continue; }
        if (line[i] == '"')             { str = !str; continue; }
        if (!str && line[i] == '/' && i + 1 < len && line[i + 1] == '/')
            return true;
    }
    return false;
}

static ZNode **path_from_position(LspModule *mod, LspPosition pos, ZScope **scope) {
    ZPositionLookup res = {
        .pos = pos, .path = NULL, .scope = NULL, .allocator = mod->allocator
    };
    ZVisitor v = {
        .visit = visit_position,
        .clientData = &res
    };

    visitNode(&v, mod->root);

    *scope = res.scope;
    return res.path;
}

static isize innermost(ZNode **path, ZNodeType type) {
    for (isize i = (isize)veclen(path) - 1; i >= 0; i--)
        if (path[i]->type == type) return i;
    return -1;
}

static ZScope *module_scope(LspModule *mod) {
    ZSemantic *sem = mod->semantic;
    if (!sem) return NULL;
    for (usize i = 0; i < veclen(sem->scopes); i++)
        if (sem->scopes[i]->module == mod->root)
            return sem->scopes[i]->ctx->global;
    return NULL;
}

LspCompletionContext get_completion_context(LspModule *mod, LspPosition pos) {
    LspCompletionContext cc = { .kind = LSP_COMPLETION_SCOPE };

    usize len           = 0;
    const char *line    = line_prefix(mod, pos, &len);
    if (line && in_line_comment(line, len)) {
        cc.kind = LSP_COMPLETION_NONE;
        return cc;
    }

    isize idx   = token_index_from_position(mod, pos);
    ZToken *tok = idx >= 0 ? mod->tokens[idx] : NULL;
    cc.token    = tok;

#if Z_LSP
    if (tok && compare_token_position(tok, pos) == 0 &&
        (tok->type == TOK_STR_LIT || tok->type == TOK_COMMENT)) {
        cc.kind = LSP_COMPLETION_NONE;
        return cc;
    }
#endif


    ZToken *word    = NULL;
    ZToken *anchor  = tok;
    if (tok && tok->type == TOK_IDENT && token_touches(tok, pos)) {
        word    = tok;
        anchor  = idx > 0 ? mod->tokens[idx - 1] : NULL;
    }

    ZScope *scope   = NULL;
    cc.path         = path_from_position(mod, pos, &scope);
    cc.scope        = scope ? scope : module_scope(mod);

    if (anchor && anchor->type == TOK_DOT) {
        cc.target = member_from_position(mod, pos);
        if (!cc.target) {
            isize i = innermost(cc.path, NODE_MEMBER);
            if (i >= 0) cc.target = cc.path[i];
        }
        if (!cc.target) {
            cc.kind = LSP_COMPLETION_NONE;
            return cc;
        }
        ZNode *obj = cc.target->memberAccess.object;
        cc.kind = obj && obj->type == NODE_IDENTIFIER &&
            strcmp(stoken(obj->tok), ".") == 0 ?
            LSP_COMPLETION_STATIC : LSP_COMPLETION_MEMBER;
        return cc;
    }

    if (anchor && (anchor->type == TOK_COLON || anchor->type == TOK_CAST)) {
        cc.kind = LSP_COMPLETION_TYPE;
        return cc;
    }

    isize lit = innermost(cc.path, NODE_STRUCT_LIT);
    if (lit >= 0 && anchor &&
        (anchor->type == TOK_LBRACKET || anchor->type == TOK_COMMA)) {
        usize depth     = veclen(cc.path) - 1 - (usize)lit;
        ZNode *field    = depth > 0 ? cc.path[lit + 1] : NULL;
        if (depth == 0 ||
            (word && field->type == NODE_VAR_DECL && field->tok == word)) {
            cc.kind     = LSP_COMPLETION_STRUCT_LIT;
            cc.target   = cc.path[lit];
            return cc;
        }
    }

    bool toplevel = true;
    for (usize i = 0; i < veclen(cc.path); i++) {
        if (cc.path[i]->type != NODE_MODULE) {
            toplevel = false;
            break;
        }
    }
    if (toplevel) cc.kind = LSP_COMPLETION_TOPLEVEL;

    return cc;
}

LspCompletionItem *get_completions(LspContext *ctx, LspModule *module, LspCompletionContext cc) {
    LspCompletionItem *items = vecnew(ctx->allocator, LspCompletionItem);
    switch (cc.kind) {
    case LSP_COMPLETION_NONE:
        break;
    case LSP_COMPLETION_SCOPE:
        list_scope(&items, cc.scope, ~0);
        list_keywords(&items, ~0);
        break;
    case LSP_COMPLETION_IMPORT_PATH:
        list_import_paths(&items, cc.prefix);
        break;
    case LSP_COMPLETION_STRUCT_LIT:
        list_struct_fields(&items, cc.target, cc.token);
        break;
    case LSP_COMPLETION_TOPLEVEL:
        list_keywords(&items, ~0);
        break;
    case LSP_COMPLETION_TYPE:
        list_scope(
            &items, cc.scope,
            Z_SYM_STRUCT | Z_SYM_ENUM | Z_SYM_TYPEDEF | Z_SYM_GENERIC | Z_SYM_IMPORT
        );
        list_keywords(&items, TOK_TYPES_MASK);
        break;
    case LSP_COMPLETION_MEMBER:
    case LSP_COMPLETION_STATIC:
        zforEachMember(module->semantic, module->root, cc.target, add_item, &items);
        break;
    }
    return items;
}

Json *lsp_get_definition(LspModule *mod, LspPositionContext p) {
    if (!p.node || !p.node->resolved) return NULL;
    ZToken *source = NULL;

    if (p.node) switch (p.node->type) {
    case NODE_IDENTIFIER:
        if (p.node->identNode.ref) source = p.node->identNode.ref->tok;
    case NODE_MEMBER:
        if (p.node->memberAccess.func) source = p.node->memberAccess.func->tok;
    default: break;
    }

    if (!source && p.token && p.token->type == TOK_IDENT) {
        char *name = stoken(p.token);

        ZScope *scope = p.scope ? p.scope : module_scope(mod);
        for (; scope && !source; scope = scope->parent) {
            if (!hashset_has(scope->seen, name)) continue;
            for (usize i = 0; i < veclen(scope->symbols) && !source; i++)
                if (strcmp(name, scope->symbols[i]->name->str))
                    source = scope->symbols[i]->name;
        }
    }

    if (!source) return NULL;

    Allocator *a = mod->allocator;
    Json *response = JsonMap(a, NULL);
    JsonSet(a, response, "uri", JsonStringFmt(a, "file://%s", source->filename));
    JsonSet(a, response, "range", range_from_token(a, source));
    return response;
}
