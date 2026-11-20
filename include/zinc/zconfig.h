#ifndef ZCONFIG_H
#define ZCONFIG_H

#define ZINC_VERSION "0.0.3"

/* The same frontend sources are compiled into two binaries:
 *   ZMODE_COMPILER - the `zinc` CLI (default): full pipeline down to codegen/link.
 *   ZMODE_LSP      - the `zinc-lsp` server: lex/parse/sem only, so everything
 *                    that only the CLI or the code generator needs is compiled out.
 * The layout of ZState differs between the two, so objects built in one mode
 * must never be linked with objects built in the other (the Makefile keeps them
 * in separate build dirs). */
#define ZMODE_COMPILER 1
#define ZMODE_LSP      2

#ifndef ZMODE
#define ZMODE ZMODE_COMPILER
#endif

#define Z_COMPILER (ZMODE == ZMODE_COMPILER)
#define Z_LSP      (ZMODE == ZMODE_LSP)

#if Z_LSP
/* It is the default field when the parser tries to parse a member access
 * but there's no field. Then the parser inserts this virtual field.
 */
#define DEFAULT_MEMBER_ACCESS "__zinc_complete__"
#endif

typedef enum {
    Z_MODE_RELEASE,
    Z_MODE_DEBUG
} ZMode;

#endif // ZCONFIG_H
