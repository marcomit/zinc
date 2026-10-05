#include "lsp.h"
#include <unistd.h>

static FILE *g_log = NULL;
FILE *g_out = NULL;

static void log_open(void) {
    const char *path = getenv("EMPTY_LSP_LOG");
    if (!path || !*path) path = "/tmp/empty_lsp.log";

    g_log = fopen(path, "a");
    if (!g_log) g_log = stderr;

    // Unbuffered
    setvbuf(g_log, NULL, _IONBF, 0);
}

void log_msg(const char *tag, const char *body, size_t len) {
    time_t t = time(NULL);
    struct tm tm;
    char ts[32];
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(g_log, "===== %s @ %s (%zu bytes) =====\n", tag, ts, len);
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

int main(void) {
    init_allocators();

    g_out = fdopen(dup(STDOUT_FILENO), "w");
    dup2(STDERR_FILENO, STDOUT_FILENO);

    log_open();
    log_fmt("START", "zinc-lsp started");

    LspContext ctx = { 0 };

    for (;;) {
        size_t len = 0;
        char *body = read_message(&len);
        if (!body) break;

        log_msg("RECV", body, len);
        ctx.root = JsonDecode(body);
        LspResponse *result = handle_message(&ctx);

        if (result) lsp_send(result->response);
        JsonFree(ctx.root);
    }

    return 0;
}
