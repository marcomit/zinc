#ifndef ZPARSE_H
#define ZPARSE_H

#include "zmem.h"
#include "zconfig.h"
#include "zforward.h"
#include "ztok.h"
#include "ztype.h"
#include "znode.h"
#include "zstate.h"

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

typedef struct ZParserModule {
    char    *name;
    ZNode   **node;
} ZParserModule;

typedef struct ZParser {
    Allocator       *allocator;
    ZState          *state;
    ZTokenStream    *source;
    usize           tokenIndex;

    /* Used to track temporary errors and find a valid path. */
    usize           *errstack;

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

ZNode *zparse(ZState *, ZToken **);
ZNode *parseExpr(ZParser *);
ZNode *parseStmt(ZParser *);
ZType *parseType(ZParser *);

bool canPeek(ZParser *);
bool check(ZParser *, ZTokenType);
bool checkMask(ZParser *, u32);
ZToken *peek(ZParser *);
ZToken *consume(ZParser *);

ZNode *setspan(ZParser *, ZNode *, ZToken *);
ZNode *setspanfrom(ZParser *, ZNode *, ZNode *);

ZNode *getMacroVar(ZNode *, ZToken *);
ZNode *getMacroCapturedVar(ZNode *, ZToken *);
ZNode *expandMacro(ZParser *);
bool macroeq(ZNode *, ZNode *);
bool macropatterneq(ZMacroPattern *, ZMacroPattern *);

ZNode *convertHeaderToZNode(ZState *, ZToken *);

#endif // ZPARSE_H
