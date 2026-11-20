#ifndef ZSTATE_H
#define ZSTATE_H

#include "zforward.h"
#include <time.h>

#include "zmem.h"
#include "zconfig.h"
#include "ztok.h"
#include "znode.h"
#include "zmap.h"
#include "zvec.h"
#include "zstr.h"

typedef enum {
    Z_LTO_OFF = 0,
    Z_LTO_THIN,
    Z_LTO_FULL
} ZLTOMode;

typedef enum {
    Z_EMIT_EXE,
    Z_EMIT_OBJ,
    Z_EMIT_IR,
    Z_EMIT_ASM
} ZEmitMode;

static char sep = '/';

typedef enum {
    Z_ERROR,
    Z_WARNING,
    Z_INFO,
    Z_DEBUG
} ZLogLevel;

typedef enum {
    Z_PHASE_LEXICAL,
    Z_PHASE_SYNTAX,
    Z_PHASE_SEMANTIC,
    Z_PHASE_GENERATE
} ZPhase;

/* Stable diagnostic codes, generated from the catalog in zdiag.def.
 * Z_DIAG_NONE (0) marks a log created without a catalog entry. */
typedef enum {
    Z_DIAG_NONE = 0,

    #define DIAG(code, level, msg, hint) code,
    #include "zdiag.def"
    #undef DIAG

    Z_DIAG_COUNT
} ZDiagCode;

/* A secondary, located annotation attached to a diagnostic: the "cause"
 * chain ("previously defined here", "required by this bound", ...). Each note
 * carries its own optional span so it can point at a different token. */
typedef struct {
    char            *message;
    ZToken          *token;
} ZLogNote;

typedef struct {
    char            *filename;
    char            *message;
    ZToken          *token;
    ZLogLevel       level;
    ZPhase          phase;

    /* Catalog code (Z_DIAG_NONE when emitted via the raw error()/warning()
     * path). Drives `--explain` and stable, message-agnostic tests. */
    ZDiagCode       code;

    /* Single actionable suggestion. May be a static catalog default or a
     * formatted string produced by emitHint(). NULL when absent. */
    char            *hint;

    /* Cause chain, appended by emitNote(). vec (NULL == empty). */
    ZLogNote        **notes;

    const char      *src_file;
    int             src_line;
} ZLog;

typedef struct {
    ZType *none;
    ZType *u0Type;
    ZType *charType;
    ZType *u1Type;
    ZType *u64Type;
    ZType *modType;
    ZType *strType;
    ZType *interpType;
    ZType *typeType;
    // ZRegion *rawRegion;

    bool initialized;
} ZConsts;

#define consts(state, var) ((state)->consts.var)

typedef struct {
    /* Options the frontend (lexer/parser/sem) reads: shared by every mode. */
    bool            debug;

    bool            unusedVar;
    bool            unusedFunc;
    bool            unusedStruct;

    bool            noInject;
    bool            noColor;

#if Z_COMPILER
    char            *output;
    char            **argv;

    bool            skipLLVMValidation;
    bool            verbose;
    bool            dumpAst;
    bool            dumpTokens;

    bool            nostdlib;

    char            optimizationLevel;
    ZLTOMode        ltoMode;

    char            *targetTriple;
    char            *targetCPU;
    char            *targetFeatures;
    ZEmitMode       emit;
    ZMode           mode;

    /* Extra arguments should be passed in the linker. */
    char            **extraArgs;
#endif
} ZCliOptions;

typedef struct {
    const char      *code;
    const char      *message;
    const char      *hint;
    const ZLogLevel level;
} ZDiagnostic;

typedef struct {
    ZNode       *module;
    Allocator   *allocator;
} ZModuleAllocator;

typedef ZMap(const char *, int) ZKeywordMap;

typedef struct {
    const char  *filename;
    zstr        content;
    ZKeywordMap words;
} ZFile;

typedef vec(ZFile) ZFileTable;

struct ZState {
    Allocator       *allocator;
    ZFileTable      fileTable;
    ZCliOptions     cli;
    ZConsts         consts;
    ZLog            **logs;
    ZPhase          currentPhase;
    char            *currentPath;
    char            *filename;
    char            *homePath;

    char            **pathFiles;
    char            **visitedFiles;
    bool            canAdvance;

#if Z_COMPILER
    /* Size of the pointer in bytes (used for usize/isize).
     * Initialized inside the code generator after creating the target.
     * */
    usize           pointerSize;


    /* opaque LLVMTargetMachineRef. */
    void            *targetMachine;

    /* Data layout string. */
    char            *dataLayout;

    /* Triple used for codegen. */
    char            *resolvedTriple;
#endif

    ZNode           *root;

    /* Each module carries its own arena allocator for thread-safety. */
    ZModuleAllocator **modules;

    /* Every module is parsed exactly once; every later import reuses the parsed
     * body from here. Shared across the per-file parser instances (each file
     * gets its own ZParser), so a re-import from any file resolves its cached
     * body regardless of which file first parsed it. */
    struct ZParserModule **cachedModules;

#if Z_COMPILER
    /* Indicates the start time of the current phase to calculate the diagnostics. */
    struct timespec phaseTime;
#endif

    /* Save every 'here' call token such that the code generator build a
     * 'SourceLocation' struct and zinc can use the location to show diagnostics.
     * */
    ZToken **sourceLocations;

    ZNode **langItems;
    ZType **reflected;
};

#define ENTRY_IMPORT_FILE "lib"

ZState *makestate();
void freestate(ZState *);
bool canAdvance(ZState *);
void initPrimitiveTypes(ZState *);

char *readfile(Allocator *, char *);
char *zstrdup(Allocator *, char *);
char *zstrndup(Allocator *, char *, usize);

char *mangler(char *[]);
char *manglerA(char *[]);

char *resolveImport(ZState *, const char *);
bool visit(ZState *, char *);
void undoVisit(ZState *);

void _error  (ZState *, ZToken *, const char *, int, const char *, ...);
void _warning(ZState *, ZToken *, const char *, int, const char *, ...);
void _info   (ZState *, ZToken *, const char *, int, const char *, ...);
void _debug  (ZState *, ZToken *, const char *, int, const char *, ...);

#define error(state, tok, ...)   _error  (state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define warning(state, tok, ...) _warning(state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define info(state, tok, ...)    _info   (state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define debug(state, tok, ...)   _debug  (state, tok, __FILE__, __LINE__, __VA_ARGS__)

ZLog *_log(ZState *, ZToken *, ZDiagCode, const char *, int, ...);
ZLog *emitHint  (ZLog *, const char *, ...);
ZLog *emitNote  (ZLog *, ZToken *, const char *, ...);

#define zlog(state, tok, code, ...) \
    _log(state, tok, code, __FILE__, __LINE__, ##__VA_ARGS__)

ZDiagnostic get_diagnostics(ZDiagCode code);
void printLogs(ZState *);

#endif // ZSTATE_H
