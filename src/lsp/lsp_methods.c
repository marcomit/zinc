#include "json.c/json.h"
#include "zlsp.h"
#include "zinc.h"
#include "zmem.h"
#include "zvec.h"
#include <stdio.h>


static LspResponse *lsp_response(int id, Json *response) {
    LspResponse *self   = zalloc(heapAllocator, LspResponse);
    self->response      = response;
    self->id            = id;
    return self;
}

LspResponse *lsp_reply(LspContext *ctx, Json *result) {
    if (!ctx) return NULL;
    Allocator *a = ctx->allocator;
    Json *reqId     = JsonGet(ctx->root, "id");
    Json *response  = JsonMap(a, NULL);
    Json *id        = JsonType(reqId) == JSON_STRING
                    ? JsonString(a, JsonAsString(reqId))
                    : JsonNumber(a, JsonAsNum(reqId));

    JsonSet(a, response, "jsonrpc",    JsonString(a, "2.0"));
    JsonSet(a, response, "id",         id);
    JsonSet(a, response, "result",     result);

    return lsp_response((int)JsonAsNum(reqId), response);
}

void lsp_send(Json *message) {
    usize len = JsonStrlen(message);
    fprintf(g_out, "Content-Length: %zu\r\n\r\n", len);
    JsonFileWrite(message, g_out);
    fflush(g_out);
    log_json("SENT", message);
}

void lsp_notify(Allocator *allocator, const char *method, Json *params) {
    Json *msg = JsonMap(allocator, NULL);

    JsonSet(allocator, msg, "jsonrpc", JsonString(allocator, "2.0"));
    JsonSet(allocator, msg, "method", JsonString(allocator, (char *)method));
    JsonSet(allocator, msg, "params", params);

    lsp_send(msg);
    JsonFree(allocator, msg);
}

void publish_diagnostics(LspContext *ctx, const char *uri, int version) {
    Allocator *a = getTempAllocator();
    Json *diags = JsonList(a, NULL);
    ZState *state = ctx->state;

    const char *filename = strncmp(uri, "file://", 7) == 0 ? uri + 7 : uri;

    for (usize i = 0; i < veclen(state->logs); i++) {
        ZLog *log = state->logs[i];
        if (log->level != Z_ERROR && log->level != Z_WARNING) continue;
        if (!log->filename || strcmp(log->filename, filename) != 0) continue;

        Json *d = JsonMap(a, NULL);
        JsonSet(a, d, "range",      range_from_token(a, log->token));
        JsonSet(a, d, "severity",   JsonNumber(a, 1 + (int)log->level));
        JsonSet(a, d, "source",     JsonString(a, "zinc"));
        JsonSet(a, d, "message",    JsonString(a, log->message ? log->message : ""));

        // Logs from the untyped error()/warning() path have no diagnostic code.
        const char *code = get_diagnostics(log->code).code;
        if (code) JsonSet(a, d, "code", JsonString(a, (char *)code));

        JsonPush(a, diags, d);
    }

    Json *params = JsonMap(a, NULL);
    JsonSet(a, params, "uri", JsonString(a, (char *)uri));
    JsonSet(a, params, "version", JsonNumber(a, version));
    JsonSet(a, params, "diagnostics", diags);

    lsp_notify(a, "textDocument/publishDiagnostics", params);
    aclose(a);
}

LspResponse *lsp_open_document(LspContext *ctx) {
    if (!ctx) return NULL;
    Json *doc   = JsonGetFmt(ctx->root, "params.textDocument");
    char *uri   = JsonAsString(JsonGet(doc, "uri"));
    char *src   = JsonAsString(JsonGet(doc, "text"));
    int version = (int)JsonAsNum(JsonGet(doc, "version"));

    if (!src) return NULL;

    lsp_analyze(ctx, uri, src, version);

    return NULL;
}

LspResponse *lsp_hover(LspContext *ctx) {
    Allocator *a = ctx->allocator;
    char *uri = JsonAsString(JsonGetFmt(ctx->root, "params.textDocument.uri"));
    Json *docPos = JsonGetFmt(ctx->root, "params.position");
    LspPosition pos = {
        .line       = (int)JsonAsNum(JsonGet(docPos, "line")),
        .character  = (int)JsonAsNum(JsonGet(docPos, "character"))
    };

    int cached = get_module(ctx, uri);
    if (cached == -1) goto err;

    LspModule *mod  = ctx->modules[cached];
    LspPositionContext p_ctx = get_position_context(mod, pos);

    if (!p_ctx.token            ||
        !p_ctx.node             ||
        !p_ctx.scope            ||
        // !p_ctx.symbol           ||
        !p_ctx.node->resolved   ) goto err;

    Json *contents  = JsonMap(a, NULL);

    JsonSet(a, contents, "kind", JsonString(a, "markdown"));
    JsonSet(a, contents, "value", JsonStringFmt(a,
        "`%s`",
        stype(p_ctx.node->resolved)
    ));

    Json *hover     = JsonMap(a, NULL);
    JsonSet(a, hover, "contents", contents);
    JsonSet(a, hover, "range", range_from_token(a, p_ctx.token));

    return lsp_reply(ctx, hover);
err:
    return lsp_reply(ctx, JsonNull(a));
}

LspResponse *lsp_completion(LspContext *ctx) {
    Allocator *a = ctx->allocator;
    const char *uri = JsonAsString(JsonGetFmt(ctx->root, "params.textDocument.uri"));
    Json *position  = JsonGetFmt(ctx->root, "params.position");
    LspPosition pos = {
        .line       = (int)JsonAsNum(JsonGet(position, "line")),
        .character  = (int)JsonAsNum(JsonGet(position, "character"))
    };

    int cached = get_module(ctx, uri);
    if (cached == -1) goto err;

    LspModule *mod = ctx->modules[cached];

    LspCompletionContext cc     = get_completion_context(mod, pos);
    LspCompletionItem *items    = get_completions(ctx, mod, cc);
    Json *completions           = completions2Json(ctx->allocator, items);
    return lsp_reply(ctx, completions);
err:
    return lsp_reply(ctx, JsonNull(a));
}

LspResponse *lsp_definition(LspContext *ctx) {
    Allocator *a = ctx->allocator;
    const char *uri = JsonAsString(JsonGetFmt(ctx->root, "params.textDocument.uri"));
    Json *position  = JsonGetFmt(ctx->root, "params.position");
    LspPosition pos = {
        .line       = (int)JsonAsNum(JsonGet(position, "line")),
        .character  = (int)JsonAsNum(JsonGet(position, "character"))
    };

    int cached = get_module(ctx, uri);
    if (cached == -1) goto err;

    LspModule *mod = ctx->modules[cached];


    LspPositionContext pctx = get_position_context(mod, pos);
    Json *response = lsp_get_definition(mod, pctx);
    return lsp_reply(ctx, response);
err:
    return lsp_reply(ctx, JsonNull(a));
}

LspResponse *lsp_initialize(LspContext *ctx) {
    if (!ctx) return NULL;
    Allocator *a = ctx->allocator;
    Json *result                = JsonMap(a, NULL);
    Json *capabilities          = JsonMap(a, NULL);
    Json *serverInfo            = JsonMap(a, NULL);
    Json *workspace             = JsonMap(a, NULL);
    Json *workspaceRT           = JsonMap(a, NULL);
    Json *completion            = JsonMap(a, NULL);
    Json *completion_triggers   = JsonList(a, NULL);

    Json *JsonTrue = JsonBool(a, true);

    JsonPush(a, completion_triggers, JsonString(a, "."));
    JsonSet(a, completion,      "triggerCharacters", completion_triggers);

    JsonSet(a, result,          "capabilities",             capabilities);
    JsonSet(a, result,          "serverInfo",               serverInfo);

    JsonSet(a, capabilities,    "textDocumentSync",         JsonNumber(a, 1));
    JsonSet(a, capabilities,    "hoverProvider",            JsonTrue);
    JsonSet(a, capabilities,    "definitionProvider",       JsonTrue);
    JsonSet(a, capabilities,    "completionProvider",       completion);
    JsonSet(a, capabilities,    "workspace",                workspace);

    JsonSet(a, serverInfo,      "name",                     JsonString(a, "zinc-lsp"));
    JsonSet(a, serverInfo,      "version",                  JsonString(a, "0.2.0"));

    JsonSet(a, workspace,       "workspaceFolders",         JsonTrue);
    JsonSet(a, workspace,       "configuration",            JsonTrue);
    JsonSet(a, workspace,       "didChangeWatchedFiles",    workspaceRT);

    JsonSet(a, workspaceRT,     "dynamicRegistration",      JsonTrue);

    return lsp_reply(ctx, result);
}

LspResponse *lsp_shutdown(LspContext *ctx) {
    freestate(ctx->state);
    ctx->state = NULL;
    aclose(ctx->allocator);
    ctx->allocator = NULL;

    return NULL;
}
