#include "json.c/json.h"
#include "lsp.h"
#include "zmem.h"


static LspResponse *lsp_response(int id, Json *response) {
    LspResponse *self   = zalloc(LspResponse);
    self->response      = response;
    self->id            = id;
    return self;
}

LspResponse *lsp_reply(LspContext *ctx, Json *result) {
    Json *reqId     = JsonGet(ctx->root, "id");
    Json *response  = JsonMap(NULL);
    Json *id        = JsonType(reqId) == JSON_STRING
                    ? JsonString(JsonAsString(reqId))
                    : JsonNumber(JsonAsNum(reqId));

    JsonSet(response, "jsonrpc",    JsonString("2.0"));
    JsonSet(response, "id",         id);
    JsonSet(response, "result",     result);

    return lsp_response((int)JsonAsNum(reqId), response);
}

static LspResponse *lsp_initialize(LspContext *ctx) {
    Json *result        = JsonMap(NULL);
    Json *capabilities  = JsonMap(NULL);
    Json *serverInfo    = JsonMap(NULL);
    Json *workspace     = JsonMap(NULL);
    Json *workspaceRT   = JsonMap(NULL);

    JsonSet(result,         "capabilities",             capabilities);
    JsonSet(result,         "serverInfo",               serverInfo);

    JsonSet(capabilities,   "textDocumentSync",         JsonNumber(2));
    JsonSet(capabilities,   "hoverProvider",            JsonBool(true));
    JsonSet(capabilities,   "definitionProvider",       JsonBool(true));
    JsonSet(capabilities,   "completionProvider",       JsonMap(NULL));
    JsonSet(capabilities,   "workspace",                workspace);

    JsonSet(serverInfo,     "name",                     JsonString("zinc-lsp"));
    JsonSet(serverInfo,     "version",                  JsonString("0.2.0"));

    JsonSet(workspace,      "workspaceFolders",         JsonBool(true));
    JsonSet(workspace,      "configuration",            JsonBool(true));
    JsonSet(workspace,      "didChangeWatchedFiles",    workspaceRT);

    JsonSet(workspaceRT,    "dynamicRegistration",      JsonBool(true));

    return lsp_reply(ctx, result);
}

static LspResponse *lsp_shutdown(LspContext *ctx) {
    return NULL;
}

static Json *get_completions(LspCompletionItem *list) {
    Json *items = JsonList(NULL);
    for (usize i = 0; i < veclen(list); i++) {
        Json *item = JsonMap(NULL);
        JsonSet(item, "label", JsonString(list[i].label));
        JsonSet(item, "kind", JsonNumber(list[i].kind));
        JsonPush(items, item);
    }
    return items;
}

static LspResponse *lsp_completion(LspContext *ctx) {
    LspCompletionItem *items = NULL;

    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_CLASS}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_FUNCTION}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_PROPERTY}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_REFERENCE}));

    return lsp_reply(ctx, get_completions(items));
}

static LspResponse *lsp_open_document(LspContext *ctx) {
    return NULL;
}

static LspResponse *lsp_change_document(LspContext *ctx) {
    return lsp_reply(ctx, JsonNull());
}

static LspHandler Handlers[] = {
    { "initialize",                 lsp_initialize      },
    { "shutdown",                   lsp_shutdown        },
    { "textDocument/didOpen",       lsp_open_document   },
    { "textDocument/didChange",     lsp_change_document },
    { "textDocument/completion",    lsp_completion      },
    { NULL,                         NULL                }
};

static bool format_id(Json *id, char *buf, size_t buf_size) {
    if (JsonType(id) == JSON_NUMBER) {
        snprintf(buf, buf_size, "%.0f", JsonAsNum(id));
        return true;
    }

    if (JsonType(id) == JSON_STRING) {
        snprintf(buf, buf_size, "\"%s\"", JsonAsString(id));
        return true;
    }
    return false;
}

LspResponse *handle_message(LspContext *ctx) {
    const char *method = JsonAsString(JsonGet(ctx->root, "method"));
    if (!method) return NULL;

    LspHandler *handler = Handlers;
    char buf[256] = "";
    bool has_id = format_id(JsonGet(ctx->root, "id"), buf, sizeof buf);

    while (handler->name) {
        if (strcmp(method, handler->name) == 0) {
            return handler->callback(ctx);
        }
        handler++;
    }

    return NULL;
}
