#include "lsp.h"
#include "zinc.h"

LspResponse *lsp_initialize(LspContext *ctx) {
    ctx->state = makestate();
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

    vecsetlen(ctx->state->logs, 0);
    ZToken **tokens = ztokenizeSource(ctx->state, text);
    ZNode *root     = zparse(ctx->state, tokens);
    zanalyze(ctx->state, root);

    publish_diagnostics(ctx, uri, version);

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
