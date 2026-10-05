#include "lsp.h"

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
