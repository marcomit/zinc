#include "lsp.h"
#include "zinc.h"
#include "zmem.h"
#include <unistd.h>

FILE *g_log = NULL;
FILE *g_out = NULL;

/*
 * TODO: Implement a temporary allocator,
 * It's an arena where every alloc restart from the beginning of the bucket. */
Allocator *tempAllocator = NULL;

static void log_open(void) {
    const char *path = getenv("EMPTY_LSP_LOG");
    if (!path || !*path) path = "/tmp/empty_lsp.log";

    g_log = fopen(path, "a");
    if (!g_log) g_log = stderr;

    // Unbuffered
    setvbuf(g_log, NULL, _IONBF, 0);
}

static inline void log_header(const char *tag) {
    time_t t = time(NULL);
    struct tm tm;
    char ts[32];
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(g_log, "===== %s @ %s =====\n", tag, ts);
}

void log_json(const char *tag, Json *json) {
    log_header(tag);
    JsonFileWrite(json, g_log);
    fputc('\n', g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

void log_msg(const char *tag, const char *body, size_t len) {
    log_header(tag);
    fwrite(body, 1, len, g_log);
    fputc('\n', g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

static void log_fmt(const char *tag, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
    log_msg(tag, buf, (size_t)n);
}

static char *read_message(size_t *out_len) {
    char line[8192];
    long content_length = -1;

    for (;;) {
        if (!fgets(line, sizeof line, stdin)) return NULL;

        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        if (n == 0) break;

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            const char *p = line + 15;
            while (*p == ' ' || *p == '\t')
                p++;
            content_length = strtol(p, NULL, 10);
        }
    }

    if (content_length < 0) return NULL;

    char *body = malloc((size_t)content_length + 1);

    if (!body) return NULL;

    size_t got = fread(body, 1, (size_t)content_length, stdin);
    body[got] = '\0';
    *out_len = got;
    return body;
}

void lsp_analyze(LspContext *ctx, const char *uri, char *text, int version) {
    char *path = strncmp(uri, "file://", 7) == 0 ? (char *)uri + 7 : (char *)uri;
    ZState *state = makestate();
    useAllocator(state->allocator);
    visit(state, path);
    initPrimitiveTypes(state);
    ZNode *root = zparse(state, ztokenizeSource(state, text));
    if (canAdvance(state)) {
        zanalyze(state, root);
    }
    freestate(ctx->state);
    ctx->state = state;
    publish_diagnostics(ctx, uri, version);
}

int main(void) {
    init_allocators();
    tempAllocator = getArenaAllocator();

    g_out = fdopen(dup(STDOUT_FILENO), "w");
    dup2(STDERR_FILENO, STDOUT_FILENO);

    log_open();
    log_fmt("START", "zinc-lsp started");

    LspContext ctx = { 0 };
    Allocator *allocator = getArenaAllocator();
    ctx.allocator = allocator;

    for (;;) {
        size_t len = 0;
        char *body = read_message(&len);
        if (!body) break;

        log_msg("RECV", body, len);
        ctx.root = JsonDecode(allocator, body);
        LspResponse *result = handle_message(&ctx);

        if (result) lsp_send(result->response);
        JsonFree(allocator, ctx.root);
    }

    free(ctx.allocator);

    return 0;
}
