// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "base.h"
#include "zinc.h"
#include "zmem.h"
#include "zvec.h"
#include "zcolors.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define indent(t) for (u8 i = 0; i < (t); i++) printf("  ");

#define MANGLER_DEFAULT_PREFIX  "_ZN"
#define MANGLER_TYPE_PREFIX     "_ZNM"
#define MANGLER_ANON_PREFIX     "_ZNA"

static char *nodeLabels[] = {
    "BLOCK",        "IF",           "WHILE",        "RETURN",
    "VAR_DECL",     "BINARY",       "UNARY",        "CALL",         "FUNC",
    "LITERAL",      "IDENTIFIER",   "STRUCT",       "SUBSCRIPT",    "MEMBER",
    "MODULE",       "FIELD",        "EMBED",        "TYPEDEF",      "FOREIGN",
    "DEFER",        "STRUCT_LIT",   "TUPLE_LIT",    "ARRAY_LIT",    "ARRAY_INIT",
    "MACRO",        "TYPE",         "ENUM",         "BREAK",        "CONTINUE",
    "ENUM_FIELD",   "CAST",         "SIZEOF",       "NAMESPACE",    "SLICE",
    "CAPABILITY",   "MATCH",        "MATCH_ARM",    "ENUM_LIT",     "FACET",
    "IMPL",         "FOR_IN"
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

static char *getHomePath();

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <libgen.h>
#include <pwd.h>

static char *getHomePath() {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return NULL;
    return strdup(home);
}

#elif _WIN32
#include <windows.h>

static char *getHomePath() {
    const char *home = getenv("USERPROFILE");
    if (!home || !*home) home = getenv("HOME");
    if (!home) return NULL;

    char *out = strdup(home);
    // Normalize to forward slashes to match the rest of the path handling.
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    return out;
}

#endif

char *stoken(ZToken *token) {
    if (!token) return "(null)";
    if (token->type == TOK_STR_LIT) {

    }
    switch(token->type) {
    case TOK_STR_LIT:
    case TOK_IDENT:     return token->str;
    case TOK_INT_LIT:   return strndup(token->start, token->end - token->start);
    case TOK_FLOAT_LIT: return strndup(token->start, token->end - token->start);
    #define DEF(id, str, _) case id: return str;

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

    return "(not found)";
}

char *tokname(ZTokenType type) {

    switch (type) {
#define DEF(id, str, _) case id: return str;

    #define TOK_FLOWS
    #define TOK_TYPES
    #define TOK_SYMBOLS

    #include "ztok.h"

    #undef TOK_SYMBOLS
    #undef TOK_TYPES
    #undef TOK_FLOWS

    #undef DEF
        default: return NULL;
    }
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

        if (type->array.dynamic) {
            vecunion(*buff, "dyn", 3);
        } else {
            usize len = type->array.size;
            while (len > 0) {
                vecpush(*buff, 48 + len % 10);
                len /= 10;
            }
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
    case Z_TYPE_SUM:
        vecpush(*buff, '(');
        for (usize i = 0; i < veclen(type->sumType); i++) {
            _stype(type->sumType[i], buff);
            if (i != veclen(type->sumType) - 1) {
                vecpush(*buff, ' ');
                vecpush(*buff, '|');
                vecpush(*buff, ' ');
            }
        }
        vecpush(*buff, ')');
        break;
    case Z_TYPE_FACET:
        vecunion(*buff, "facet ", 6);
        vecunion(*buff, type->facet.name->str, strlen(type->facet.name->str));
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

static const u64 KIND_PRIME[] = {
    0xE142EA7D17BE3111ULL,  // Z_TYPE_PRIMITIVE
    0x9064005C3985C3CFULL,  // Z_TYPE_POINTER
    0xBF87E362CF8D446BULL,  // Z_TYPE_STRUCT
    0xC8A639D015B52909ULL,  // Z_TYPE_ARRAY
    0xC797B2C957207247ULL,  // Z_TYPE_FUNCTION
    0xFDE31A516694C343ULL,  // Z_TYPE_TUPLE
    0xC4007D5AE88DA719ULL,  // Z_TYPE_GENERIC
    0xFF9D3E64C1A6423BULL,  // Z_TYPE_FACET
    0x8CF2B69B0577AEA9ULL,  // Z_TYPE_ENUM
    0xE92AC1391F4A8CA1ULL,  // Z_TYPE_NONE
    0xA7AA7CBC23377BBDULL,  // Z_TYPE_NAMESPACE
    0xAD78DC4BFB9E8DDBULL,  // Z_TYPE_SUM
    0x9E3779B185EBCA87ULL,  // Z_TYPE_OPTIONAL
    0xC2B2AE3D27D4EB4FULL,  // Z_TYPE_RESULT
};

inline u32 hashtoken(ZToken *tok) {
    char *str = stoken(tok);
    return hashStr(str, strlen(str));
}

u32 hashNode(ZNode *node) {
    return node->type;
}

NOSANITIZE("unsigned-integer-overflow")
static inline u32 hashMix(u32 h, u32 x) {
    return h ^ (x + 0x9e3779b9u + (h << 6) + (h >> 2));
}

static inline u32 hashTypeList(u32 h, ZType **types) {
    h = hashMix(h, (u32)veclen(types));
    for (usize i = 0; i < veclen(types); i++) {
        h = hashMix(h, hashType(types[i]));
    }
    return h;
}

NOSANITIZE("unsigned-integer-overflow")
u32 hashType(ZType *type) {
    if (!type) return 0;

    u32 h = (u32)KIND_PRIME[type->kind];

    switch (type->kind) {
    case Z_TYPE_PRIMITIVE:
        h = hashMix(h, hashtoken(type->primitive.token));
        break;
    case Z_TYPE_POINTER:
        h = hashMix(h, hashType(type->base));
        break;
    case Z_TYPE_STRUCT:
        if (type->strct.name) h = hashMix(h, hashtoken(type->strct.name));
        break;
    case Z_TYPE_ARRAY:
        h = hashMix(h, hashType(type->array.base));
        break;
    case Z_TYPE_FUNCTION:
        h = hashMix(h, hashType(type->func.ret));
        h = hashTypeList(h, type->func.args);
        h = hashTypeList(h, type->func.capabilities);
        h = hashMix(h, (u32)veclen(type->func.generics));
        break;
    case Z_TYPE_TUPLE:
        h = hashTypeList(h, type->tuple);
        break;
    case Z_TYPE_GENERIC:
        if (type->generic.name) h = hashMix(h, hashtoken(type->generic.name));
        break;
    case Z_TYPE_FACET:
        if (type->facet.name) h = hashMix(h, hashtoken(type->facet.name));
        h = hashMix(h, (u32)veclen(type->facet.funcs));
        for (usize i = 0; i < veclen(type->facet.funcs); i++) {
            h = hashMix(h, hashType(type->facet.funcs[i]->resolved));
        }
        break;
    case Z_TYPE_ENUM:
        if (type->enm.name) h = hashMix(h, hashtoken(type->enm.name));
        break;
    case Z_TYPE_SUM: {
        u32 acc = 0;
        for (usize i = 0; i < veclen(type->sumType); i++) {
            acc += hashType(type->sumType[i]);
        }
        h = hashMix(h, (u32)veclen(type->sumType));
        h ^= acc;
        break;
    }
    case Z_TYPE_OPTIONAL:
        h = hashMix(h, hashType(type->optional));
        break;
    case Z_TYPE_RESULT:
        h = hashMix(h, hashType(type->result.success));
        h = hashMix(h, hashType(type->result.error));
        break;
    case Z_TYPE_NAMESPACE:
    case Z_TYPE_NONE:
        // No structural payload compared by typesEqual; the kind seed suffices.
        break;
    }

    return h;
}

void printDestructedVar(ZVarDestructPattern *pattern, u8 depth) {
    indent(depth);

    switch (pattern->type) {
    case Z_VAR_LIT:
    case Z_VAR_IDENT:
        printf("%s\n", stoken(pattern->tok));
        break;
    case Z_VAR_PAIR:
        printf("%s:\n", pattern->key->str);
        printDestructedVar(pattern->value, depth + 1);
        break;
    case Z_VAR_ENUM:
        printf("%s::%s\n", pattern->base->str, pattern->prop->str);
        for (usize i = 0; i < veclen(pattern->args); i++)
            printDestructedVar(pattern->args[i], depth + 1);
        break;
    case Z_VAR_SUM:
        printf("%s(\n", stype(pattern->sum.type));
        printDestructedVar(pattern->sum.child, depth + 1);
        indent(depth);
        printf(")\n");
        break;
    default: {
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
        break;
    }
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

static void printAnnotation(ZAnnotation *arg) {
    switch (arg->kind) {
    case Z_ANN_IDENT:   printToken(arg->ident);     break;
    case Z_ANN_LIT:     printToken(arg->literal);   break;
    case Z_ANN_NESTED:
        printf("%s(", stoken(arg->tok));
        for (usize i = 0; i < veclen(arg->nested); i++) {
            printAnnotation(arg->nested[i]);
            if (i != veclen(arg->nested) - 1) printf(", ");
        }
        printf(")");
        break;
    case Z_ANN_ASSIGN:
        printf("%s = ", stoken(arg->assign.name));
        printAnnotation(arg->assign.value);
        break;
    }
}

void printNode(ZNode *node, u8 depth) {
    if (node == NULL) {
        printf("unknown");
        return;
    }

    indent(depth);

    printf("[%s %s] ", nodeLabels[node->type], stype(node->resolved));

    depth++;
    switch (node->type) {
    case NODE_BREAK:
        if (node->breakStmt.expr) {
            printf("\n");
            printNode(node->breakStmt.expr, depth);
        }
    case NODE_CONTINUE:
    case NODE_ARRAY_INIT:
        break;
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
        if (node->binary.overload) printf(" overloaded");
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
            printf("Receiver: %s", stype(node->funcDef.receiver->field.type));
            printf(" ");
            printToken(node->funcDef.receiver->field.identifier);
            printf(" ");
        } else if (node->funcDef.base) {
            printf("%s::", node->funcDef.base->primitive.token->str);
        }
        printf("%s, Type: %s", stoken(node->funcDef.name), stype(node->funcDef.ret));
        printf("\n");
        for (usize i = 0; i < veclen(node->funcDef.generics); i++) {
            indent(depth);
            printf("%s", stype(node->funcDef.generics[i]));
            printf("\n");
        }
        indent(depth);
        for (usize i = 0; i < veclen(node->funcDef.annotations); i++) {
            printAnnotation(node->funcDef.annotations[i]);
            if (i != veclen(node->funcDef.annotations) - 1) printf(", ");
        }
        printf("\n");
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
    case NODE_EMBED_FIELD:
        if (node->resolved) printf("%s\n", stype(node->resolved));
        break;
    case NODE_FIELD:
        if (node->resolved) printf("%s: ", stype(node->resolved));
        if (node->field.identifier) printf("%s" , node->field.identifier->str);
        break;
    case NODE_STRUCT:
        if (node->structDef.pub) printf("pub ");
        printf("%s[", node->structDef.ident->str);
        for (usize i = 0; i < veclen(node->structDef.generics); i++) {
            printf("%s", stype(node->structDef.generics[i]));
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
        printf("Name: %s: %s\n", stoken(node->module.name), node->module.filename);
        for (usize i = 0; i < veclen(node->module.root); i++) {
            printNode(node->module.root[i], depth);
        }
        break;

    case NODE_MEMBER: {
        for (usize i = 0; i < veclen(node->memberAccess.path); i++) {
            printf("%d ", node->memberAccess.path[i]);
        }
        printf("\n");
        printNode(node->memberAccess.object, depth);
        indent(depth);
        printToken(node->memberAccess.field);
        break;
    }
    case NODE_TYPEDEF:
        printf(" %s alias for %s",
            node->typeDef.alias->str,
            stype(node->typeDef.type));
        break;
    case NODE_FOREIGN:
        printf("%s\n", stoken(node->foreignDecl.name));
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
        printToken(veclast(node->structlit.chain));
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
    case NODE_CAST:
        printf("\n");
        printNode(node->castExpr.expr, depth);
        indent(depth + 1);
        printf("as %s", stype(node->castExpr.toType));
        break;
    case NODE_SIZEOF:
        printf("%s", stype(node->sizeofExpr.type));
        break;

    case NODE_ENUM:
        printf("%s\n", stoken(node->enumDef.name));

        for (usize i = 0; i < veclen(node->enumDef.fields); i++) {
            printNode(node->enumDef.fields[i], depth);
        }
        break;

    case NODE_ENUM_FIELD:
        printf("%s\n", stoken(node->enumField.name));
        break;

    case NODE_NAMESPACE:
        printf("%s\n", node->tok->str);
        for (usize i = 0; i < veclen(node->block); i++) {
            printNode(node->block[i], depth);
        }
        break;

    case NODE_SLICE:
        printf("\n");
        indent(depth);
        printf("base\n");
        printNode(node->slice.base, depth);
        if (node->slice.start) {
            indent(depth);
            printf("start\n");
            printNode(node->slice.start, depth);
        }
        if (node->slice.end) {
            indent(depth);
            printf("end\n");
            printNode(node->slice.end, depth);
        }
        break;
    case NODE_CAPABILITY:
        printf("\n");
        printNode(node->capability.capability, depth);
        printNode(node->capability.block, depth);
        break;
    case NODE_MATCH:
        printf("\n");
        printNode(node->match.cond, depth);
        for (usize i = 0; i < veclen(node->match.arms); i++) {
            printNode(node->match.arms[i], depth);
        }
        break;
    case NODE_ENUM_LIT:
        printf("\n");
        printNode(node->call.callee, depth);
        for (usize i = 0; i < veclen(node->call.args); i++) {
            printNode(node->call.args[i], depth);
        }
        break;
    case NODE_MATCH_ARM:
        printf("\n");
        printDestructedVar(node->matchArm.pattern, depth);
        printNode(node->matchArm.expr, depth);
        break;
    case NODE_FACET:
        printf("%s\n", stoken(node->facet.name));
        for (usize i = 0; i < veclen(node->facet.funcs); i++) {
            printNode(node->facet.funcs[i], depth);
        }
        break;
    case NODE_IMPL:
        printf("%s", stype(node->impl.base));
        printf("\n");

        for (usize i = 0; i < veclen(node->impl.funcs); i++) {
            printNode(node->impl.funcs[i], depth);
        }
        break;
    case NODE_FORIN:
        printf("\n");
        printDestructedVar(node->forin.binding, depth);
        printNode(node->forin.iter, depth);
        printNode(node->forin.body, depth);
        break;

    default:
            printf("(details not implemented in printer for node %d)",
                    node->type);
            break;
    }
    printf("\n");
}

static inline void sanitize(char *source, char **buff) {
    while (*source) {
        if (isalnum(*source)) {
            vecpush(*buff, *source);
        } else {
            vecpush(*buff, '_');
            vecpush(*buff, '_');
        }
        source++;
    }
}

static char *_mangler(char *segments[], const char *prefix) {
    char *mangled = NULL;

    vecunion(mangled, prefix, strlen(prefix));
    for (usize i = 0; segments[i] != NULL; i++) {
        int len = strlen(segments[i]);
        int tmp = len;
        while (tmp) {
            vecpush(mangled, ('0' + tmp % 10));
            tmp /= 10;
        }
        sanitize(segments[i], &mangled);
    }
    vecpush(mangled, '\0');
    return mangled;
}

char *manglerA(char *segments[]) { return _mangler(segments, MANGLER_ANON_PREFIX); }
char *mangler(char *segments[]) { return _mangler(segments, MANGLER_DEFAULT_PREFIX); }

/* Encode a ZType into a mangled name buffer.
 * Pointer types get a 'P' prefix; primitives get a length-prefixed name.
 * e.g. String -> "6String", *String -> "P6String" */
void encodeType(ZType *type, char **buf) {
    switch (type->kind) {
    case Z_TYPE_POINTER:
        vecpush(*buf, 'P');
        encodeType(type->base, buf);
        break;
    case Z_TYPE_TUPLE:
        vecpush(*buf, 'T');
        for (usize i = 0; i < veclen(type->tuple); i++) {
            encodeType(type->tuple[i], buf);
        }
        break;
    case Z_TYPE_PRIMITIVE: {
        const char *name = type->primitive.token->str;
        usize len = strlen(name);
        int tmp = len;
        while (tmp) {
            vecpush(*buf, '0' + tmp % 10);
            tmp /= 10;
        }
        vecunion(*buf, name, len);
        break;
    }
    case Z_TYPE_ARRAY:
        vecpush(*buf, 'A');
        encodeType(type->array.base, buf);
        break;
    case Z_TYPE_FUNCTION:
        vecpush(*buf, 'F');
        encodeType(type->func.ret, buf);
        for (usize i = 0; i < veclen(type->func.args); i++) {
            encodeType(type->func.args[i], buf);
        }
        break;
    case Z_TYPE_STRUCT: {
        vecpush(*buf, 'S');
        const char *name = type->strct.name->str;
        usize len = strlen(name);
        int tmp = len;
        while (tmp) {
            vecpush(*buf, '0' + tmp % 10);
            tmp /= 10;
        }
        vecunion(*buf, name, len);
        break;
    }
    case Z_TYPE_FACET:
        vecpush(*buf, 'I');
        break;
    default: break;
    }
}

/* Mangle a receiver (non-static) method using the _ZNM prefix so it never
 * collides with a static function of the same name on the same type.
 * The full receiver type is encoded, so `for String self` and
 * `for *String self` produce distinct names. */
char *manglerM(ZType *recvType, ZToken *funcName) {
    char *mangled = NULL;
    vecunion(mangled, MANGLER_TYPE_PREFIX, strlen(MANGLER_TYPE_PREFIX));
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
        printf("%s", stype(symbol->type));
        break;
    case Z_SYM_FUNC:
        printf("Func(%s)", symbol->name->str);
        printf("%s", stype(symbol->type));
        break;
    case Z_SYM_STRUCT:
        printf("Struct(%s)", symbol->name->str);
        printf("%s", stype(symbol->type));
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

ZState *makestate() {
    ZState *self                = zalloc(ZState);
    *self                       = (ZState){ 0 };

    self->currentPhase          = Z_PHASE_LEXICAL;
    self->homePath              = getHomePath();
    self->canAdvance            = true;

    self->emit                  = Z_EMIT_EXE;
    self->optimizationLevel     = '2';
    self->ltoMode               = Z_LTO_OFF;
    self->mode                  = Z_MODE_RELEASE;

    return self;
}

char *readfile(char *filename) {
    FILE *fd = fopen(filename, "rb");

    if (!fd) return NULL;

    fseek(fd, 0, SEEK_END);
    i64 flen = ftell(fd);
    fseek(fd, 0, SEEK_SET);
    char *buff = allocator.alloc(flen + 1);
    fread(buff, flen, 1, fd);

    buff[flen] = 0;
    fclose(fd);
    return buff;
}

ZLog *vmakelog( ZState *state,
                ZLogLevel level,
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
    log->phase = state->currentPhase;
    vecpush(state->logs, log);

    return log;
}

static pthread_mutex_t logLock = PTHREAD_MUTEX_INITIALIZER;

#define LOG_FUNC(name, level)                                                   \
void name(ZState *state, ZToken *tok, const char *src_file,                     \
            int src_line, const char *fmt, ...) {                               \
    pthread_mutex_lock(&logLock);                                               \
    va_list args;                                                               \
    va_start(args, fmt);                                                        \
                                                                                \
    vmakelog(state, level,                                                      \
        tok ? tok->filename : NULL,                                             \
        tok,                                                                    \
        src_file,                                                               \
        src_line,                                                               \
        fmt,                                                                    \
        args);                                                                  \
                                                                                \
    va_end(args);                                                               \
    pthread_mutex_unlock(&logLock);                                             \
}

LOG_FUNC(_error,    Z_ERROR)
LOG_FUNC(_warning,  Z_WARNING)
LOG_FUNC(_info,     Z_INFO)
LOG_FUNC(_debug,    Z_DEBUG)

static char *resolvePath(ZState *state, char *filename) {
    if (!state->filename) return filename;
    if (filename[0] == sep) return filename;
    // Windows absolute path: "C:/..." or "C:\..."
    if (filename[0] && filename[1] == ':' && (filename[2] == '/' || filename[2] == '\\')) return filename;


    usize len = strlen(state->filename);
    char *path = znalloc(char, len+1);
    strncpy(path, state->filename, len);
    path[len] = '\0';

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

static bool fileExists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

#define ENTRY_MODULE "/lib.zn"

static char *resolveModuleFile(char *filename) {
    if (fileExists(filename)) return filename;

    usize n = strlen(filename);
    if (n < 3 || strcmp(filename + n - 3, ".zn") != 0) return filename;

    usize baseLen = n - 3;
    char *alt = malloc(baseLen + sizeof(ENTRY_MODULE));
    memcpy(alt, filename, baseLen);
    memcpy(alt + baseLen, ENTRY_MODULE, sizeof(ENTRY_MODULE));

    if (fileExists(alt)) return alt;
    free(alt);
    return filename;
}

/* Stores the filename in the global state of visited files.
 * if external is true means that the compiler try to find it in the package directory.
 * It returns the resolved file path or null if already visited.
 * */
bool visit(ZState *state, char **filename, bool external) {
    *filename = resolvePath(state, *filename);
    *filename = resolveModuleFile(*filename);
    for (usize i = 0; i < veclen(state->visitedFiles); i++) {
        if (strcmp(state->visitedFiles[i], *filename) == 0) return false;
    }

    if (!external) {
        printf("  " COLOR_BOLD COLOR_GREEN "Building" COLOR_RESET " %s\n", *filename);
    }
    vecpush(state->visitedFiles,    *filename);
    vecpush(state->pathFiles,       *filename);
    state->filename = *filename;
    return true;
}

void undoVisit(ZState *state) {
    vecpop(state->pathFiles);
    state->filename = veclen(state->pathFiles) > 0 ? veclast(state->pathFiles) : NULL;
}

static void printLineHighlight(ZToken *tok, const char *color) {
    if (!tok || !tok->start || !tok->sourceLinePtr) return;
    char *lineStart = tok->sourceLinePtr;

    char num[32];

    snprintf(num, sizeof(num), "%zu", tok->row);
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
    if (log->filename) printf("  %s", log->filename);
    if (state->debug) {
        printf("[%s:%d]", log->src_file, log->src_line);
    }
    printf(":");

    if (log->token) printf("%zu:%zu: ", log->token->row, log->token->col);

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
