#ifndef ZPARSE_H
#define ZPARSE_H

#include "zinc/zinc.h"
#include "zvisit.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

typedef enum {
    Z_MACRO_KEY,        // Captured keyword
    Z_MACRO_EXPR,       // Captured expression
    Z_MACRO_IDENT,      // Captured identifier
    Z_MACRO_TYPE,       // Captured type
    Z_MACRO_ZM,         // Zero or more
    Z_MACRO_OM,         // One or more
    Z_MACRO_SEQ         // Sequence
} ZMacroType;

typedef struct ZMacroPattern {
    ZMacroType kind;
    union {
        /* Used for keyword, expression and identifier*/
        ZToken *ident;
        struct ZMacroPattern *zeroOrMore;
        struct ZMacroPattern *oneOrMore;
        struct ZMacroPattern **sequence;
    };
} ZMacroPattern;

typedef struct ZMacroVar {
    ZToken *name;
    usize startIndex;   // Start index into source token array
    usize endIndex;     // End index (exclusive)
    ZNode *captured;    // The parsed AST node for this captured variable
    usize useCount;     // Count how many times the variable is used in the body
} ZMacroVar;

typedef struct ZMacroParser {
    ZNode             **macros;

    /* Set when it parses the body of a macro. */
    ZNode             *currentMacro;


    /* Stack of macros currently being expanded (for cycle detection) */
    ZNode             **expandingMacros;

    /* Current pattern list. */
    ZMacroPattern    *currentList;

    /* Used for list expansion. */
    usize             currentIndex;
} ZMacroParser;

typedef struct ZParserSnapshot {
    ZTokenStream    *stream;
    usize           streamIndex;
    usize           tokenIndex;
    ZToken          *last;
} ZParserSnapshot;

typedef struct ZParser {
    Allocator       *allocator;
    ZState          *state;
    ZTokenStream    *source;
    ZFile           *file;
    usize           tokenIndex;

    /* Used to track temporary errors and find a valid path. */
    usize           *errstack;
    ZLog            **logs;

    ZMacroParser    macroParser;

    u8              depth;

    /* When true, parseExpr will not attempt to parse a struct literal.
     * Set by condition-parsing sites (if, for, while) to prevent `ident {`
     * from being mistaken for a struct literal instead of a block. */
    bool            noStructLit;

    /* When true, parseStmt will not attempt to parse a return statement.
     * Set by parseDefer so that `defer return ...` will be rejected by the parser.
     * */
    bool            noReturnStmt;


    /* Inside a struct or enum declaration it's very useful creating
     * anonymous structs. But anonymous structs can be parsed only inside a struct/enum
     * declaration and not everywhere (e.g. inside a function or as a return type). */
    bool            declAsType;

    /* When true, parseFuncDecl tries to parse the first parameter as a type.
     * It will be set true by parseImpl before calling parseFuncDecl and reset at the end.
     */
    bool            inImplBlock;

    /* Must set only inside an impl block, it captures the base of the the impl. */
    ZType           *implBase;

    /* Last consumed token: where the node being parsed ends (see setspan). */
    ZToken          *last;
} ZParser;

ZNode *makenode(Allocator *, ZNodeType);
ZInterpolation *makeinterpolation(Allocator *, ZInterpolationType, void *);
ZNode *makenodevar(Allocator *, ZVarDestructPattern *, ZType *, ZNode *);

bool canPeek(ZParser *);
bool check(ZParser *, ZTokenType);
bool checkMask(ZParser *, u32);
ZToken *peek(ZParser *);
bool checkAhead(ZParser *, ZTokenType, usize);
ZToken *peekAhead(ZParser *, usize);
ZToken *consume(ZParser *);
bool match(ZParser *, ZTokenType);
ZNode *setspan(ZParser *, ZNode *, ZToken *);
ZNode *setspanfrom(ZParser *, ZNode *, ZNode *);
ZParserSnapshot *store(ZParser *);
void undo(ZParser *, ZParserSnapshot *);
void pushErrorCheckpoint(ZParser *);
void commitErrors(ZParser *);
void rollbackErrors(ZParser *);
bool isValidToken(ZParser *, ZTokenType *, size_t);

static inline void sinchronize(ZParser *parser, ZTokenType opening, ZTokenType closing) {
    usize level = 1;
    while (canPeek(parser) && level > 0) {
        if (check(parser, opening)) level++;
        else if (check(parser, closing)) level--;
        consume(parser);
    }
}

#define arrlen(arr) (sizeof(arr) / sizeof((arr)[0]))

#define ensure(c, msg) do {                                                     \
    if (!(c)) {                                                                 \
        return NULL;                                                            \
    }                                                                           \
} while (0)

#define guard(c) if (!(c)) return NULL

#define expect(l, t) if (!match(l, t)) {                                        \
        zlog((l)->state, peek(l),                                               \
                Z0009, tokname(t), stoken(peek(parser)));                       \
        return NULL;                                                            \
    }

#define tryParse(p, func) ({                                                    \
    ZParserSnapshot *saved = store(p);                                          \
    pushErrorCheckpoint(p);                                                     \
    let res = (func);                                                           \
    if (!res) {                                                                 \
        undo(p, saved);                                                         \
        rollbackErrors(p);                                                      \
    } else {                                                                    \
        commitErrors(p);                                                        \
    }                                                                           \
    res;                                                                        \
})

typedef ZNode *(*ZParseFunc)(ZParser *);

void parseFile                      (Allocator *, ZState *, ZFile *);
ZType *parseType                    (ZParser *);
ZNode *parseExpr                    (ZParser *);
ZNode *parseStmt                    (ZParser *);
ZNode *parse                        (ZParser *);
ZNode *parseIf                      (ZParser *);
ZNode *parseBreak                   (ZParser *);
ZNode *parseBlock                   (ZParser *);
ZNode *parseDefer                   (ZParser *);
ZNode *parseMatch                   (ZParser *, bool);
ZNode *parseUnary                   (ZParser *);
ZNode *parseLoops                   (ZParser *);
ZNode *parseReturn                  (ZParser *);
ZNode *parseVarDef                  (ZParser *);
ZNode *parseBinary                  (ZParser *);
ZNode *parsePrimary                 (ZParser *);
ZNode *parseAnonFunc                (ZParser *);
ZNode *parseArrayLit                (ZParser *);
ZNode *parseContinue                (ZParser *);
ZNode *parseTupleLit                (ZParser *);
ZType *parseBaseType                (ZParser *);
ZType *parseFuncType                (ZParser *);
ZType *parseTypeArray               (ZParser *);
ZNode *parseStructLit               (ZParser *);
ZNode *parseVarInferred             (ZParser *);
ZNode *parseVarDefTyped             (ZParser *);
ZNode *parseTypeAsExpr              (ZParser *);
ZNode *parseBlockOrInline           (ZParser *, bool);
ZNode *_parseStructLit              (ZParser *, ZToken **);

/* File-level parsing functions */
ZNode *parseImport                  (ZParser *, bool);
ZNode *parseTypedef                 (ZParser *, ZAnnotation **, bool);
ZNode *parseFacet                   (ZParser *, ZAnnotation **, bool);
ZNode *parseForeign                 (ZParser *, ZAnnotation **, bool);
ZNode *parseFuncDecl                (ZParser *, ZAnnotation **, bool);
ZNode *parseEnumDecl                (ZParser *, ZAnnotation **, bool);
ZNode *parseStructDecl              (ZParser *, ZAnnotation **, bool);
ZNode *parseForeignBlock            (ZParser *, ZAnnotation **, bool);
ZNode *parseForeignInlineDecl       (ZParser *, ZAnnotation **, bool);
ZNode                **parseCapabilityList  (ZParser *);
ZType                **parseGenericsDecl    (ZParser *);
ZVarDestructPattern  *parseMultiDestructVar (ZParser *);
ZVarDestructPattern  *parseDestructVar      (ZParser *, bool);
ZType                *parseAnonStruct       (ZParser *, ZAnnotation **);
ZType                *parseAnonEnum         (ZParser *, ZAnnotation **);
ZAnnotation          **parseAnnotations     (ZParser *);
ZNode *parseField(ZParser *);
ZNode **parseGenericList(ZParser *, ZTokenType, ZTokenType, ZParseFunc, bool);
ZNode **parseCapabilityList(ZParser *);

ZParser             *makeparser             (Allocator *, ZState *, ZFile *);
ZVarDestructPattern *makeVarDestructPattern (Allocator *, int);
ZVarDestructPattern *makeDestructIdent      (Allocator *, ZToken *);
ZType               **parseTypeList         (ZParser *, ZTokenType, ZTokenType);
ZNode               *parseOrGrammar         (ZParser *, ZParseFunc *, usize);
ZNode               *parseUpdate            (ZParser *);
ZNode               *parseMultiExpr         (ZParser *);
ZNode               *parseCapabilityBlock   (ZParser *);

#endif // ZPARSE_H
