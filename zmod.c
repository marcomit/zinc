#include "zinc.h"
#include "zvec.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define indent(t) for (u8 i = 0; i < (t); i++) printf("  ");

static char *nodeLabels[] = {
    "BLOCK",    "IF",           "WHILE",        "FOR",          "RETURN",
    "VAR_DECL", "BINARY",       "UNARY",        "CALL",         "FUNC",
    "LITERAL",  "IDENTIFIER",   "STRUCT",       "SUBSCRIPT",    "MEMBER",
    "MODULE",   "UNION",        "FIELD",        "TYPEDEF",      "FOREIGN",
    "DEFER",    "STRUCT_LIT",   "TUPLE_LIT",    "ARRAY_LIT",    "MACRO",
    "GOTO",     "LABEL",        "TYPE",         "ENUM",         "BREAK",
    "CONTINUE", "ENUM_FIELD",   "CAST",         "SIZEOF",       "STATIC_CALL"

};

static char *levels[] = {
    "error",
    "warning",
    "info"
};
static char *colors[] = {
    "\033[38;2;220;53;69m",   // Error   (red)
    "\033[38;2;255;193;7m",   // Warning (yellow/orange)
    "\033[38;2;23;162;184m",  // Info    (cyan/blue)
};

static void printLog(ZState *, ZLog *);

char *stoken(ZToken *token) {
    if (!token) return "(null)";
    char *tok = allocator.alloc(32);
    bool istype = token->type & TOK_TYPES_MASK;

    if (istype) {
        sprintf(tok, "type(");
    }

    
    switch(token->type) {
    case TOK_INT_LIT:   sprintf(tok, "%llu", token->integer);                   break;
    case TOK_FLOAT_LIT: sprintf(tok, "%g", token->floating);                    break;
    case TOK_STR_LIT:   sprintf(tok, "%s", token->str);                         break;
    case TOK_BOOL_LIT:  sprintf(tok, "%s", token->boolean ? "true" : "false");  break;
    case TOK_IDENT:     sprintf(tok, "%s", token->str);                         break;
    #define DEF(id, str, _) case id: sprintf(tok, str);                         break;

    #define TOK_FLOWS
    #define TOK_TYPES
    #define TOK_SYMBOLS

    #include "ztok.h"

    #undef TOK_SYMBOLS
    #undef TOK_TYPES
    #undef TOK_FLOWS

    #undef DEF
    default:
        break;
    }

    return tok;
}

char *tokname(ZTokenType type) {
    char *tok = allocator.alloc(32);

    switch (type) {
#define DEF(id, str, _) case id: sprintf(tok, str); break; 

    #define TOK_FLOWS
    #define TOK_TYPES
    #define TOK_SYMBOLS

    #include "ztok.h"

    #undef TOK_SYMBOLS
    #undef TOK_TYPES
    #undef TOK_FLOWS

    #undef DEF
        default: break;
    }
    return tok;
}

void printToken(ZToken *token) {
    char *tok = stoken(token);
    printf("%s", tok);
}


void printTokens(ZToken **tokens) {
    printf("==== Tokens: %zu ====\n", veclen(tokens));
    for (usize i = 0; i < veclen(tokens); i++) {
        if (tokens[i]->newlineBefore) printf("\n");
        else printf(" ");
        printToken(tokens[i]);
    }
    printf("\n==== End tokens ====\n");
}

static void _stype(ZType *type, char **buff) {
    if (!type) {
        vecunion(*buff, "unknown", 7);
        return;
    }

    switch (type->kind) {
    case Z_TYPE_POINTER:
        vecpush(*buff, '*');
        _stype(type->base, buff);
        break;
    case Z_TYPE_PRIMITIVE: {
        char *str = stoken(type->primitive.token);
        vecunion(*buff, str, strlen(str));
        break;
    }
    case Z_TYPE_FUNCTION:
        _stype(type->func.ret, buff);
        vecpush(*buff, '(');
        for (usize i = 0; i < veclen(type->func.args); i++) {
            _stype(type->func.args[i], buff);
            if (i < veclen(type->func.args) - 1) vecunion(*buff, ", ", 2);
        }
        vecpush(*buff, ')');
        break;
    case Z_TYPE_STRUCT:
        vecunion(*buff, type->strct.name->str, strlen(type->strct.name->str));
        break;
    case Z_TYPE_ARRAY:
        vecpush(*buff, '[');
        _stype(type->array.base, buff);
        vecpush(*buff, ']');
        break;
    case Z_TYPE_TUPLE:
        vecpush(*buff, '(');
        for (usize i = 0; i < veclen(type->tuple); i++) {
            _stype(type->tuple[i], buff);
            if (i < veclen(type->tuple) - 1) vecpush(*buff, ',');
        }
        vecpush(*buff, ')');
        break;
    case Z_TYPE_GENERIC:
        vecunion(*buff, type->generic.name->str, strlen(type->generic.name->str));
        vecpush(*buff, '[');

        for (usize i = 0; i < veclen(type->generic.args); i++) {
            _stype(type->generic.args[i], buff);
            if (i < veclen(type->generic.args) - 1) vecpush(*buff, ',');
        }
        vecpush(*buff, ']');
        break;
    default:
        break;
    }
}

void stype(ZType *type, char **buff) {
    _stype(type, buff);
    vecpush(*buff, '\0');
}

void printType(ZType *type) {
    if (!type) {
        printf("unknown");
        return;
    }

    if (type->constant) printf("const ");

    switch(type->kind) {
    case Z_TYPE_POINTER:
        printf("*");
        printType(type->base);
        break;
    case Z_TYPE_PRIMITIVE:
        printToken(type->primitive.token);
        break;
    case Z_TYPE_FUNCTION:
        printType(type->func.ret);
        printf("(");
        for (usize i = 0; i < veclen(type->func.args); i++) {
            printType(type->func.args[i]);
            if (i < veclen(type->func.args) - 1) printf(", ");
        }
        printf(")");
        break;
    case Z_TYPE_STRUCT:
        printf("struct %s", type->strct.name->str);
        break;
    case Z_TYPE_ARRAY:
        printf("[");
        printType(type->array.base);
        printf("; %zu]", type->array.size);
        break;
    case Z_TYPE_TUPLE:
        printf("(");
        for (usize i = 0; i < veclen(type->tuple); i++) {
            printType(type->tuple[i]);
            if (i < veclen(type->tuple) - 1) printf(", ");
        }
        printf(")");
        break;
    case Z_TYPE_GENERIC:
        printf("%s[", type->generic.name->str);
        for (usize i = 0; i < veclen(type->generic.args); i++) {
            printType(type->generic.args[i]);
            if (i < veclen(type->generic.args) - 1) printf(", ");
        }
        printf("]");
        break;
    default:
        printf("(details not implemented for type %d)", type->kind);
        break;
    }
}

static void printMacroPattern(ZMacroPattern *pattern, u8 depth) {
    indent(depth);
    switch (pattern->kind) {
    case Z_MACRO_IDENT:
        printf("i(%s)\n", pattern->ident->str);
        break;
    case Z_MACRO_KEY:
        printf("key(%s)\n", pattern->ident->str);
        break;
    case Z_MACRO_TYPE:
        printf("type(%s)\n", pattern->ident->str);
        break;
    case Z_MACRO_EXPR:
        printf("expr(%s)\n", pattern->ident->str);
        break;
    case Z_MACRO_ZM:
        printf("$(\n");
        printMacroPattern(pattern->zeroOrMore, depth + 1);
        indent(depth);
        printf(")*\n");
        break;
    case Z_MACRO_OM:
        printf("$(\n");
        printMacroPattern(pattern->oneOrMore, depth + 1);
        indent(depth);
        printf(")+\n");
        break;
    case Z_MACRO_SEQ:
        printf("seq(\n");
        for (usize i = 0; i < veclen(pattern->sequence); i++) {
            printMacroPattern(pattern->sequence[i], depth + 1);
        }
        indent(depth);
        printf(")\n");
        break;
    default:
        printf("Invalid macro type\n");
        break;
    }
}

void printNode(ZNode *node, u8 depth) {
    if (node == NULL) {
        printf("unknown");
        return;
    }

    // Helper to print indentation
    indent(depth);

    printf("[%s] ", nodeLabels[node->type]);

    depth++;
    switch (node->type) {
    case NODE_LITERAL:
        printf("Value: ");
        printToken(node->literalTok);
        break;

    case NODE_IDENTIFIER:
        printf("Name: %s", node->identNode.tok->str);
        break;

    case NODE_BINARY:
        printf("Op: ");
        printToken(node->binary.op);
        printf("\n");
        printNode(node->binary.left, depth);
        printNode(node->binary.right, depth);
        return; // Return early to avoid the double newline

    case NODE_VAR_DECL:
        printf("Var: %s Type: ", node->varDecl.ident->identNode.tok->str);
        printType(node->varDecl.type);
        if (node->varDecl.rvalue) {
            printf("\n");
            printNode(node->varDecl.rvalue, depth);
        }
        break;

    case NODE_BLOCK:
        printf(" %zu\n", veclen(node->block));
        for (usize i = 0; i < veclen(node->block); i++) {
            printNode(node->block[i], depth);
        }

        return;

    case NODE_FUNC:
        if (node->funcDef.pub) printf("pub ");
        if (node->funcDef.receiver) {
            printf("Receiver: ");
            printType(node->funcDef.receiver->field.type);
            printf(" ");
            printToken(node->funcDef.receiver->field.identifier);
            printf(" ");
        }
        printf("%s, Type: ", node->funcDef.name->str);
        printType(node->funcDef.ret);
        printf("\n");
        for (usize i = 0; i < veclen(node->funcDef.generics); i++) {
            indent(depth);
            printToken(node->funcDef.generics[i]);
            printf("\n");
        }
        printNode(node->funcDef.body, depth);
        return;

    case NODE_CALL:
        printf("\n");
        printNode(node->call.callee, depth);
        for (usize i = 0; i < veclen(node->call.args); i++){
            printNode(node->call.args[i], depth);
        }
        return;

    case NODE_RETURN:
        printf("\n");
        if (node->returnStmt.expr) printNode(node->returnStmt.expr, depth);
        return;

    case NODE_IF:
        printf("Cond: \n");
        printNode(node->ifStmt.cond, depth);
        printNode(node->ifStmt.body, depth);
        if (node->ifStmt.elseBranch) {
            indent(depth - 1);
            printf("[ELSE]\n");
            printNode(node->ifStmt.elseBranch, depth);
        }
        break;
    case NODE_WHILE:
        printf("Cond: \n");
        printNode(node->whileStmt.cond, depth);
        printNode(node->whileStmt.branch, depth);
        break;
    case NODE_FOR:
        printf("\n");
        printNode(node->forStmt.var, depth);
        printf("\n");
        printNode(node->forStmt.cond, depth);
        printf("\n");
        printNode(node->forStmt.incr, depth);
        printf("\n");
        printNode(node->forStmt.block, depth);
        break;
    case NODE_STRUCT:
        if (node->structDef.pub) printf("pub ");
        printf("%s[", node->structDef.ident->str);
        for (usize i = 0; i < veclen(node->structDef.generics); i++) {
                printToken(node->structDef.generics[i]);
        }
        printf("]\n");
        for (usize i = 0; i < veclen(node->structDef.fields); i++) {
            ZNode *field = node->structDef.fields[i];
            indent(depth);
            printType(field->field.type);
            printf(" %s\n", field->field.identifier->str);
        }
        break;
    case NODE_UNARY:
        printf("Op: %s\n", stoken(node->unary.operat));
        printNode(node->unary.operand, depth);
        break;
    case NODE_MODULE:
        printf("Name: %s\n", node->module.name);
        for (usize i = 0; i < veclen(node->module.root); i++) {
            printNode(node->module.root[i], depth);
        }
        break;
    
    case NODE_MEMBER:
        printf("Field: %s\n", node->memberAccess.field->str);
        printNode(node->memberAccess.object, depth);
        break;
    case NODE_TYPEDEF:
        printf(" %s alias for ", node->typeDef.alias->str);
        printType(node->typeDef.type);
        break;
    case NODE_FOREIGN:
        printType(node->foreignFunc.ret);
        printf(" %s(", node->foreignFunc.tok->str);
        for (usize i = 0; i < veclen(node->foreignFunc.args); i++) {
            printType(node->foreignFunc.args[i]);
            if (i < veclen(node->foreignFunc.args) - 1) printf(", ");
        }
        printf(")");
        break;
    case NODE_DEFER:
        printf("\n");
        printNode(node->deferStmt.expr, depth);
        break;
    case NODE_ARRAY_LIT:
        printf("\n");
        for(usize i = 0; i < veclen(node->arraylit); i++) {
            printNode(node->arraylit[i], depth);
        }
        break;
    case NODE_STRUCT_LIT:
        printToken(node->structlit.ident);
        printf("\n");
        for (usize i = 0; i < veclen(node->structlit.fields); i++) {
            printNode(node->structlit.fields[i], depth);
        }
        break;
    case NODE_MACRO:
        printf("PATTERN = \n");
        printMacroPattern(node->macro.pattern, depth);
        break;
    case NODE_SUBSCRIPT:
        printf("\n");
        indent(depth);
        printf("array:\n");
        printNode(node->subscript.arr, depth);
        indent(depth);
        printf("index:\n");
        printNode(node->subscript.index, depth);
        break;
    case NODE_TUPLE_LIT:
        printf("\n");
        ZNode **fields = node->tuplelit;
        for (usize i = 0; i < veclen(fields); i++) {
            printNode(fields[i], depth);
        }
        break;
    case NODE_LABEL:
        printf("Label: %s", stoken(node->gotoLabel));
        break;
    case NODE_CAST:
        printf("\n");
        printNode(node->castExpr.expr, depth);
        printf("as ");
        printType(node->castExpr.toType);
        break;
    case NODE_SIZEOF:
        printType(node->sizeofExpr.type);
        break;

    case NODE_STATIC_ACCESS:
        printf("%s::%s",
                node->staticAccess.base->str, node->staticAccess.prop->str);
        break;
    default:
            printf("(details not implemented in printer for node %d)",
                    node->type);
            break;
    }
    printf("\n");
}

void mangler(ZToken *segments[], char **mangled) {
    if (strcmp((*segments)->str, "main") == 0) {
        vecunion(*mangled, "main\0", 5);
    }
    vecunion(*mangled, "_ZN", 3);
    while (*segments) {
        int len = strlen((*segments)->str);
        int tmp = len;
        while (tmp) {
            vecpush(*mangled, ('0' + tmp % 10));
            tmp /= 10;
        }
        vecunion(*mangled, (*segments)->str, (usize)len);
        segments++;
    }
    vecpush(*mangled, '\0');
}

void printSymbol(ZSymbol *symbol) {
    switch (symbol->kind) {
    case Z_SYM_VAR:
        printf("Var(%s) ", symbol->name->str);
        printType(symbol->type);
        break;
    case Z_SYM_FUNC:
        printf("Func(%s)", symbol->name->str);
        printType(symbol->type);
        break;
    case Z_SYM_STRUCT:
        printf("Struct(%s)", symbol->name->str);
        printType(symbol->type);
        break;
    default: return;
    }
    printf("\n");
}

void printScope(ZScope *scope) {
    if (!scope) return;
    printf("\n\n==== Scope(len: %zu, depth: %d) ====\n",
            veclen(scope->symbols), scope->depth);

    for (usize i = 0; i < veclen(scope->symbols); i++) {
        printSymbol(scope->symbols[i]);
    }
    printf("\n==== End scope ====\n");
    printScope(scope->parent);
}

ZState *makestate(char *filename) {
    ZState *self            = zalloc(ZState);

    self->output            = NULL;
    self->currentPhase      = Z_PHASE_LEXICAL;
    self->filename          = filename;
    self->errors            = NULL;
    self->verbose           = false;
    self->pathFiles         = NULL;
    self->debug             = false;

    self->unusedFunc        = false;
    self->unusedStruct      = false;
    self->unusedVar         = false;

    self->visitedFiles      = NULL;
    self->optimizationLevel = 0;

    return self;
}

char *readfile(char *filename) {
    FILE *fd = fopen(filename, "r");
    
    if (!fd) {
        fprintf(stderr, "open(%s)", filename);
        perror("");
        return NULL;
    }

    fseek(fd, 0, SEEK_END);
    i64 flen = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    char *buff = allocator.alloc(flen + 1);
    fread(buff, flen, 1, fd);

    buff[flen] = 0;
    fclose(fd);
    return buff;
}

ZLog *vmakelog(ZLogLevel level,
               char *filename,
               ZToken *tok,
               const char *src_file,
               int src_line,
               const char *fmt,
               va_list args) {
    ZLog *log = zalloc(ZLog);

    log->filename = filename;
    log->level = level;
    log->token = tok;
    log->src_file = src_file;
    log->src_line = src_line;

    va_list args_copy;

    va_copy(args_copy, args);

    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    log->message = allocator.alloc((size_t)len + 1);
    if (log->message) {
        vsnprintf(log->message, (size_t)len + 1, fmt, args);
    }

    return log;
}

void _error(ZState *state, ZToken *tok, const char *src_file,
            int src_line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    ZLog *log = vmakelog(Z_ERROR,
            state->filename,
            tok,
            src_file,
            src_line,
            fmt,
            args);
    log->phase = state->currentPhase;
    vecpush(state->errors, log);
    
    va_end(args);
}

void _warning(ZState *state, ZToken *tok, const char *src_file,
            int src_line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    ZLog *log = vmakelog(Z_WARNING,
            state->filename,
            tok, src_file,
            src_line,
            fmt,
            args);
    log->phase = state->currentPhase;
    vecpush(state->errors, log);

    va_end(args);
}

void _info(ZState *state, ZToken *tok, const char *src_file,
            int src_line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    ZLog *log = vmakelog(Z_INFO,
            state->filename,
            tok,
            src_file,
            src_line,
            fmt,
            args);
    log->phase = state->currentPhase;
    vecpush(state->errors, log);

    va_end(args);
}

/* Debug logs are printed directly. */
void _debug(ZState *state, ZToken *tok, const char *src_file,
            int src_line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    ZLog *log = vmakelog(Z_DEBUG,
            state->filename,
            tok,
            src_file,
            src_line,
            fmt,
            args);
    log->phase = state->currentPhase;
    vecpush(state->errors, log);

    va_end(args);
}

bool visit(ZState *state, char *filename) {
    for (usize i = 0; i < veclen(state->pathFiles); i++) {
        if (strcmp(state->pathFiles[i], filename) == 0) return false;
    }

    vecpush(state->visitedFiles,     filename);
    vecpush(state->pathFiles,         filename);
    state->filename = filename;
    return true;
}

void undoVisit(ZState *state) {
    char *filename = vecpop(state->pathFiles);
    state->filename = filename;
}

static void printLineHighlight(ZToken *tok, const char *color) {
    char *lineStart = tok->sourceLinePtr;
    
    while (*lineStart && *lineStart != '\n') {
            putchar(*lineStart);
            lineStart++;
    }
    lineStart = tok->sourceLinePtr;
    putchar('\n');

    
    printf("%s", color);
    u32 i = 1;
    
    while (lineStart++ != tok->start) {
        putchar(' ');
        i++;
    }
    putchar('^');
    i++;

    for (; i <= tok->col; i++) {
        putchar('~');
    }
    
    printf("\033[0m\n");
}

static void printLog(ZState *state, ZLog *log) {

    printf("%s", log->filename);
    if (state->debug) {
        printf("[%s:%d]", log->src_file, log->src_line);
    }
    printf(":");

    if (log->token) {
        printf("%zu:%zu: ", log->token->row, log->token->col);
    }
    printf("%s%s\033[0m: ", colors[log->level], levels[log->level]);
    printf("%s\n", log->message);

    if (log->token) printLineHighlight(log->token, colors[log->level]);
}

void printLogs(ZState *state) {
    printf("\n\n========= Start Logs (%zu) =========\n", veclen(state->errors));
    for (usize i = 0; i < veclen(state->errors); i++) {
        printLog(state, state->errors[i]);
    }
    printf("\n\n========= End Logs =========\n");
}
