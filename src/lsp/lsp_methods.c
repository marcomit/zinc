#include "json.c/json.h"
#include "lsp.h"
#include "zinc.h"
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

void lsp_send(Json *message) {
    const char *json = JsonEncode(message);
    size_t len = strlen(json);
    fprintf(g_out, "Content-Length: %zu\r\n\r\n%s", len, json);
    fflush(g_out);
    log_msg("SENT", json, len);
}

void lsp_notify(const char *method, Json *params) {
    Json *msg = JsonMap(NULL);

    JsonSet(msg, "jsonrpc", JsonString("2.0"));
    JsonSet(msg, "method", JsonString((char *)method));
    JsonSet(msg, "params", params);

    lsp_send(msg);
    JsonFree(msg);
}

static LspResponse *lsp_initialize(LspContext *ctx) {
    Json *result        = JsonMap(NULL);
    Json *capabilities  = JsonMap(NULL);
    Json *serverInfo    = JsonMap(NULL);
    Json *workspace     = JsonMap(NULL);
    Json *workspaceRT   = JsonMap(NULL);

    JsonSet(result,         "capabilities",             capabilities);
    JsonSet(result,         "serverInfo",               serverInfo);

    JsonSet(capabilities,   "textDocumentSync",         JsonNumber(1));
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

LspResponse *lsp_completion(LspContext *ctx) {
    LspCompletionItem *items = NULL;

    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_CLASS}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_FUNCTION}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_PROPERTY}));
    vecpush(items, ((LspCompletionItem){"PRRR", Z_LSP_REFERENCE}));

    return lsp_reply(ctx, get_completions(items));
}

static Json *range_from_token(ZToken *tok) {
    Json *start = JsonMap(NULL);
    Json *end   = JsonMap(NULL);
    Json *range = JsonMap(NULL);

    int line    = tok ? (int)tok->row - 1 : 0;
    int col     = tok ? (int)(tok->start - tok->sourceLinePtr) : 0;
    int row     = tok ? (int)(tok->end - tok->start) : 0;

    JsonSet(start,  "line",         JsonNumber(line));
    JsonSet(start,  "character",    JsonNumber(col));

    JsonSet(end,    "line",         JsonNumber(line));
    JsonSet(end,    "character",    JsonNumber(col + row));

    JsonSet(range,  "start",        start);
    JsonSet(range,  "end",          end);

    return range;
}

static void publish_diagnostics(const char *uri, int version, ZState *state) {
    Json *diags = JsonList(NULL);

    for (usize i = 0; i < veclen(state->logs); i++) {
        ZLog *log = state->logs[i];
        if (log->level != Z_ERROR && log->level != Z_WARNING) continue;

        Json *d = JsonMap(NULL);
        JsonSet(d, "range", range_from_token(log->token));
        JsonSet(d, "severity", JsonNumber(1 + (int)log->level));
        JsonSet(d, "source", JsonString("zinc"));
        JsonSet(d, "message", JsonString(log->message));

        JsonPush(diags, d);
    }

    Json *params = JsonMap(NULL);
    JsonSet(params, "uri", JsonString((char *)uri));
    JsonSet(params, "version", JsonNumber(version));
    JsonSet(params, "diagnostics", diags);

    lsp_notify("textDocument/publishDiagnostics", params);
}

LspResponse *lsp_open_document(LspContext *ctx) {
    Json *doc   = JsonGetFmt(ctx->root, "params.textDocument");
    char *uri   = JsonAsString(JsonGet(doc, "uri"));
    char *src   = JsonAsString(JsonGet(doc, "text"));
    int version = (int)JsonAsNum(JsonGet(doc, "version"));

    char *path = strncmp(uri, "file://", 7) == 0 ? uri + 7 : uri;

    if (!src) return NULL;

    ZState *state   = makestate();
    visit(state, &path, false);
    initPrimitiveTypes();
    ZToken **tokens = ztokenizeSource(state, src);

    ZNode *root     = zparse(state, tokens);

    publish_diagnostics(uri, version, state);
    return NULL;
}

LspResponse *lsp_change_document(LspContext *ctx) {
    return NULL;
}

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
