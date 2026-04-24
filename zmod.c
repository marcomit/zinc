#include "zinc.h"
#include "zmem.h"
#include "zvec.h"
#include "zcolors.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <libgen.h>

#define indent(t) for (u8 i = 0; i < (t); i++) printf("  ");

static char *nodeLabels[] = {
    "BLOCK",        "IF",           "WHILE",        "FOR",          "RETURN",
    "VAR_DECL",     "BINARY",       "UNARY",        "CALL",         "FUNC",
    "LITERAL",      "IDENTIFIER",   "STRUCT",       "SUBSCRIPT",    "MEMBER",
    "MODULE",       "FIELD",        "EMBED",        "TYPEDEF",      "FOREIGN",
    "DEFER",        "STRUCT_LIT",   "TUPLE_LIT",    "ARRAY_LIT",    "ARRAY_INIT",
    "MACRO",        "GOTO",         "LABEL",        "TYPE",         "ENUM",
    "BREAK",        "CONTINUE",     "ENUM_FIELD",   "CAST",         "SIZEOF",
    "STATIC_ACCESS"
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

static char *getCompilerPath();

#ifdef __linux__
#include <unistd.h>
#include <linux/limits.h>

static char *getCompilerPath() {
    char *buf[PATH_MAX];
    usize len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len == -1) return NULL;
    buf[len] = '\0';
    return strdup(dirname(buf));
}

#elif __APPLE__
#include <mach-o/dyld.h>
#include <sys/syslimits.h>

static char *getCompilerPath() {
    char buf[PATH_MAX];
    u32 size = sizeof(buf);

    if (_NSGetExecutablePath(buf, &size) != 0)  return NULL;

    char *real = realpath(buf, NULL);
    if (!real) return NULL;

    char *dir = strdup(dirname(real));

    free(real);
    return dir;
}

#elif _WIN32
#include <windows.h>

static char *getCompilerPath() {
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);

    if (len == 0) return NULL;

    char *last = strrchr(buf, '\\');
    if (last) *last = '\0';
    return strdup(last);
}

#endif

char *stoken(ZToken *token) {
    if (!token) return "(null)";
    char *tok = allocator.alloc(32);
    bool istype = token->type & TOK_TYPES_MASK;

    if (istype) {
        sprintf(tok, "type(");
    }

    
    switch(token->type) {
    case TOK_STR_LIT:
    case TOK_IDENT:     sprintf(tok, "%s", token->str);                         break;
    case TOK_INT_LIT:   sprintf(tok, "%lld", (long long)token->integer);                   break;
    case TOK_FLOAT_LIT: sprintf(tok, "%g", token->floating);                    break;
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
        vecunion(*buff, "struct ", 7);
        vecunion(*buff, type->strct.name->str, strlen(type->strct.name->str));
        break;
    case Z_TYPE_ARRAY: {
        vecpush(*buff, '[');
        
        usize len = type->array.size;

        while (len > 0) {
            vecpush(*buff, 48 + len % 10);
            len /= 10;
        }

        vecpush(*buff, ']');
        _stype(type->array.base, buff);
        break;
    }
    case Z_TYPE_ENUM:
        vecunion(*buff, "enum ", 5);
        vecunion(*buff, type->enm.name->str, strlen(type->enm.name->str));
        break;
    case Z_TYPE_TUPLE:
        vecpush(*buff, '(');
        for (usize i = 0; i < veclen(type->tuple); i++) {
            _stype(type->tuple[i], buff);
            if (i < veclen(type->tuple) - 1) vecpush(*buff, ',');
        }
        vecpush(*buff, ')');
        break;
    // case Z_TYPE_GENERIC:
    //     vecunion(*buff, type->generic.name->str, strlen(type->generic.name->str));
    //     vecpush(*buff, '[');
    //
    //     for (usize i = 0; i < veclen(type->generic.args); i++) {
    //         _stype(type->generic.args[i], buff);
    //         if (i < veclen(type->generic.args) - 1) vecpush(*buff, ',');
    //     }
    //     vecpush(*buff, ']');
    //     break;
    default:
        break;
    }
}

char *stype(ZType *type) {
    char *buff = NULL;
    _stype(type, &buff);
    vecpush(buff, '\0');
    return buff;
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
        printf("[%zu]", type->array.size);
        printType(type->array.base);
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
        printf("%s", type->generic.name->str);
        if (veclen(type->generic.extensions) > 0) {
            printf(": ");
        }
        for (usize i = 0; i < veclen(type->generic.extensions); i++) {
            printType(type->generic.extensions[i]);
            printf(" ");
        }
        break;
    case Z_TYPE_ENUM:
        printf("enum %s\n", type->enm.name->str);
        break;
    default:
        printf("(details not implemented for type %d)", type->kind);
        break;
    }
}

static void printDestructedVar(ZVarDestructPattern *pattern, u8 depth) {
    indent(depth);

    if (pattern->type == Z_VAR_IDENT) {
        printf("%s\n", pattern->ident->str);
    } else {
        bool isTuple = pattern->type == Z_VAR_TUPLE;
        ZVarDestructPattern **list = isTuple ?
            pattern->tuple :
            pattern->fields;

        printf("%c\n", isTuple ? '(' : '{');
        for (usize i = 0; i < veclen(list); i++) {
            printDestructedVar(list[i], depth + 1);
        }
        indent(depth);
        printf("%c\n", isTuple ? ')' : '}');
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

    printf("[%s %s] ", nodeLabels[node->type], stype(node->resolved));

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
        printf("\n");
        printDestructedVar(node->varDecl.pattern, depth);
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
        } else if (node->funcDef.base) {
            printf("%s::", node->funcDef.base->primitive.token->str);
        }
        printf("%s, Type: ", node->funcDef.name->str);
        printType(node->funcDef.ret);
        printf("\n");
        for (usize i = 0; i < veclen(node->funcDef.generics); i++) {
            indent(depth);
            printType(node->funcDef.generics[i]);
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
    case NODE_EMBED_FIELD:
        if (node->resolved) printf("%s\n", stype(node->resolved));
        break;
    case NODE_FIELD:
        if (node->resolved) printf("%s: ", stype(node->resolved));
        if (node->field.identifier) printf("%s\n" , node->field.identifier->str);
        break;
    case NODE_STRUCT:
        if (node->structDef.pub) printf("pub ");
        printf("%s[", node->structDef.ident->str);
        for (usize i = 0; i < veclen(node->structDef.generics); i++) {
                printType(node->structDef.generics[i]);
        }
        printf("]\n");
        for (usize i = 0; i < veclen(node->structDef.fields); i++) {
            printNode(node->structDef.fields[i], depth);
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
    
    case NODE_MEMBER: {
        printf("\n");
        printNode(node->memberAccess.object, depth);
        indent(depth);
        printToken(node->memberAccess.field);
        break;
    }
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
        printf(" %zu\n", veclen(node->arraylit));
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
        indent(depth + 1);
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

    case NODE_BREAK:
    case NODE_CONTINUE:
        break;

    case NODE_ENUM:
        printf("%s\n", stoken(node->enumDef.name));

        for (usize i = 0; i < veclen(node->enumDef.fields); i++) {
            printNode(node->enumDef.fields[i], depth);
        }
        break;

    case NODE_ENUM_FIELD:
        printf("%s\n", stoken(node->enumField.name));
        for (usize j = 0; j < veclen(node->enumField.captured); j++) {
            indent(depth);
            printType(node->enumField.captured[j]);
            printf("\n");
        }
        break;

    default:
            printf("(details not implemented in printer for node %d)",
                    node->type);
            break;
    }
    printf("\n");
}

char *mangler(ZToken *segments[]) {
    char *mangled = NULL;
    if (strcmp((*segments)->str, "main") == 0) {
        vecunion(mangled, "main\0", 5);
    }
    vecunion(mangled, "_ZN", 3);
    while (*segments) {
        int len = strlen((*segments)->str);
        int tmp = len;
        while (tmp) {
            vecpush(mangled, ('0' + tmp % 10));
            tmp /= 10;
        }
        vecunion(mangled, (*segments)->str, (usize)len);
        segments++;
    }
    vecpush(mangled, '\0');
    return mangled;
}

/* Encode a ZType into a mangled name buffer.
 * Pointer types get a 'P' prefix; primitives get a length-prefixed name.
 * e.g. String -> "6String", *String -> "P6String" */
static void encodeType(ZType *type, char **buf) {
    if (type->kind == Z_TYPE_POINTER) {
        vecpush(*buf, 'P');
        encodeType(type->base, buf);
    } else if (type->kind == Z_TYPE_PRIMITIVE) {
        const char *name = type->primitive.token->str;
        int len = strlen(name);
        int tmp = len;
        while (tmp) {
            vecpush(*buf, '0' + tmp % 10);
            tmp /= 10;
        }
        vecunion(*buf, name, (usize)len);
    }
}

/* Mangle a receiver (non-static) method using the _ZNM prefix so it never
 * collides with a static function of the same name on the same type.
 * The full receiver type is encoded, so `for String self` and
 * `for *String self` produce distinct names. */
char *manglerM(ZType *recvType, ZToken *funcName) {
    char *mangled = NULL;
    vecunion(mangled, "_ZNM", 4);
    encodeType(recvType, &mangled);
    int len = strlen(funcName->str);
    int tmp = len;
    while (tmp) {
        vecpush(mangled, '0' + tmp % 10);
        tmp /= 10;
    }
    vecunion(mangled, funcName->str, (usize)len);
    vecpush(mangled, '\0');
    return mangled;
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
    ZState *self                = zalloc(ZState);
                                
    self->output                = NULL;
    self->currentPhase          = Z_PHASE_LEXICAL;
    self->filename              = filename;
    self->logs                  = NULL;
    self->verbose               = false;
    self->pathFiles             = NULL;
    self->debug                 = false;
    self->canAdvance            = true;
    self->compilerPath          = getCompilerPath();
    self->currentPath           = NULL;
                                
    self->unusedFunc            = false;
    self->unusedStruct          = false;
    self->unusedVar             = false;
                                
    self->visitedFiles          = NULL;
    self->skipLLVMValidation    = false;
    self->optimizationLevel     = 0;

    return self;
}

char *readfile(char *filename) {
    FILE *fd = fopen(filename, "r");
    
    if (!fd) {
        perror("Error");
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
    vecpush(state->logs, log);
    
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
    vecpush(state->logs, log);

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
    vecpush(state->logs, log);

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
    vecpush(state->logs, log);

    va_end(args);
}

static char *resolvePath(ZState *state, char *filename) {
    if (!state->filename) return filename;
    if (filename[0] == sep) return filename;


    char path[256] = { 0 };
    strncpy(path, state->filename, 256);

    char *dir = dirname(path);
    char *out = NULL;

    while (*dir) {
        vecpush(out, *dir);
        dir++;
    }
    vecpush(out, '/');

    while (*filename) {
        vecpush(out, *filename);
        filename++;
    }

    vecpush(out, '\0');
    return out;
}

bool visit(ZState *state, char *filename) {
    filename = resolvePath(state, filename);
    for (usize i = 0; i < veclen(state->pathFiles); i++) {
        if (strcmp(state->pathFiles[i], filename) == 0) return false;
    }

    printf("  " COLOR_BOLD COLOR_GREEN "Building" COLOR_RESET " %s\n", filename);
    vecpush(state->visitedFiles,     filename);
    vecpush(state->pathFiles,         filename);
    state->filename = filename;
    return true;
}

void undoVisit(ZState *state) {
    vecpop(state->pathFiles);
    state->filename = veclen(state->pathFiles) > 0 ? veclast(state->pathFiles) : NULL;
}

static void printLineHighlight(ZToken *tok, const char *color) {
    if (!tok || !tok->start) return;
    char *lineStart = tok->sourceLinePtr;
    
    char num[32];

    sprintf(num, "%zu", tok->row);
    usize numlen = strlen(num);
    
    u8 padding = 0;
    if (numlen < 6) {
        while(6 - numlen >= padding) {
            putchar(' ');
            padding++;
        }
    } else putchar(' ');

    printf("%s |", num);

    while (*lineStart && *lineStart != '\n') {
            putchar(*lineStart);
            lineStart++;
    }
    lineStart = tok->sourceLinePtr;
    putchar('\n');

    
    padding = numlen < 6 ? 8 : numlen + 1;
    while (padding-- > 0) putchar(' ');
    putchar('|');
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

    printf("  %s", log->filename);
    if (state->debug) {
        printf("[%s:%d]", log->src_file, log->src_line);
    }
    printf(":");

    if (log->token) {
        printf("%zu:%zu: ", log->token->row, log->token->col);
    }
    printf(COLOR_BOLD "\n  %s%s\033[0m: ", colors[log->level], levels[log->level]);
    printf("%s\n", log->message);

    if (log->token) printLineHighlight(log->token, colors[log->level]);
}

bool canAdvance(ZState *state) {
    bool advance = true;
    for (usize i = 0; i < veclen(state->logs) && advance; i++) {
        advance = state->logs[i]->level != Z_ERROR;
    }
    return advance;
}

void printLogs(ZState *state) {
    for (usize i = 0; i < veclen(state->logs); i++) {
        printLog(state, state->logs[i]);
    }
}
