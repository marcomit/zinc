#include "json.c/json.h"
#include "lsp.h"
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

static Json *range_from_token(Allocator *a, ZToken *tok) {
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

void publish_diagnostics(LspContext *ctx, const char *uri, int version) {
    Allocator *a = getTempAllocator();
    Json *diags = JsonList(a, NULL);
    ZState *state = ctx->state;

    for (usize i = 0; i < veclen(state->logs); i++) {
        ZLog *log = state->logs[i];
        if (log->level != Z_ERROR && log->level != Z_WARNING) continue;

        Json *d = JsonMap(a, NULL);
        JsonSet(a, d, "range", range_from_token(a, log->token));
        JsonSet(a, d, "severity", JsonNumber(a, 1 + (int)log->level));
        JsonSet(a, d, "source", JsonString(a, "zinc"));
        JsonSet(a, d, "message", JsonString(a, log->message));

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
    Json *doc   = JsonGetFmt(ctx->root, "params.textDocument");
    char *uri   = JsonAsString(JsonGet(doc, "uri"));
    char *src   = JsonAsString(JsonGet(doc, "text"));
    int version = (int)JsonAsNum(JsonGet(doc, "version"));

    char *path = strncmp(uri, "file://", 7) == 0 ? uri + 7 : uri;

    if (!src) return NULL;
    if (!ctx || !ctx->state) return NULL;

    lsp_analyze(ctx, uri, src, version);

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
