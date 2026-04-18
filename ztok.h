/*
 * Definition of tokens using the X macros trick.
 */

#ifndef TOK_MASKS
#define TOK_MASKS

#define TOK_BASE_MASK 16

#define TOK_FLOWS_MASK                  (1 << (TOK_BASE_MASK))
#define TOK_TYPES_MASK                  (1 << (TOK_BASE_MASK + 1))
#define TOK_DYN_MASK                    (1 << (TOK_BASE_MASK + 2))
#define TOK_SYMBOLS_MASK                (1 << (TOK_BASE_MASK + 3))
#define TOK_OPERATOR                    (1 << (TOK_BASE_MASK + 4))
#define TOK_TYPES_SIGNATURE_MASK        (1 << (TOK_BASE_MASK + 5))
#define TOK_LITERAL                     (1 << (TOK_BASE_MASK + 6))
#define TOK_SIGNED                      (1 << (TOK_BASE_MASK + 7))
#define TOK_UNSIGNED                    (1 << (TOK_BASE_MASK + 8))
#define TOK_FLOAT                       (1 << (TOK_BASE_MASK + 9))
#define TOK_OVERRIDABLE                 (1 << (TOK_BASE_MASK + 10))
#endif                                  
                                        
#ifdef TOK_FLOWS                        
DEF(TOK_IF,         "if",               TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x00)
DEF(TOK_ELSE,       "else",             TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x01)
DEF(TOK_FOR,        "for",              TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x02)
DEF(TOK_CONTINUE,   "continue",         TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x03)
DEF(TOK_BREAK,      "break",            TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x04)
DEF(TOK_RETURN,     "return",           TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x05)
DEF(TOK_GOTO,       "goto",             TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x06)
DEF(TOK_SWITCH,     "switch",           TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x07)
DEF(TOK_CASE,       "case",             TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x08)
DEF(TOK_MODULE,     "use",              TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x09)
DEF(TOK_FOREIGN,    "foreign",          TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x0A)
DEF(TOK_DEFER,      "defer",            TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x0B)
DEF(TOK_IN,         "in",               TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x0C)
DEF(TOK_MATCH,      "match",            TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x0D)
DEF(TOK_MACRO,      "macro",            TOK_FLOWS_MASK | 0x0E)
DEF(TOK_SNOT,       "not",              TOK_FLOWS_MASK | 0x0F)
DEF(TOK_SOR,        "or",               TOK_FLOWS_MASK | 0x10)
DEF(TOK_SAND,       "and",              TOK_FLOWS_MASK | 0x11)
DEF(TOK_NONE,       "none",             TOK_FLOWS_MASK | 0x12)
DEF(TOK_TRUE,       "true",             TOK_FLOWS_MASK | TOK_LITERAL | 0x13)
DEF(TOK_FALSE,      "false",            TOK_FLOWS_MASK | TOK_LITERAL | 0x14)
DEF(TOK_CAST,       "as",               TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x15)
DEF(TOK_SIZEOF,     "sizeof",           TOK_FLOWS_MASK | TOK_OVERRIDABLE | 0x16)
DEF(TOK_MOD,        "mod",              TOK_FLOWS_MASK | 0x72)
#endif

#ifdef TOK_TYPES
DEF(TOK_VOID,       "u0",               TOK_TYPES_MASK                              | 0x20)
DEF(TOK_BOOL,       "u1",               TOK_TYPES_MASK | TOK_SIGNED                 | 0x21)
DEF(TOK_CHAR,       "char",             TOK_TYPES_MASK                              | 0x22)
DEF(TOK_F32,        "f32",              TOK_TYPES_MASK | TOK_FLOAT                  | 0x23)
DEF(TOK_F64,        "f64",              TOK_TYPES_MASK | TOK_FLOAT                  | 0x24)
DEF(TOK_I8,         "i8",               TOK_TYPES_MASK | TOK_SIGNED                 | 0x25)
DEF(TOK_I16,        "i16",              TOK_TYPES_MASK | TOK_SIGNED                 | 0x26)
DEF(TOK_I32,        "i32",              TOK_TYPES_MASK | TOK_SIGNED                 | 0x27)
DEF(TOK_I64,        "i64",              TOK_TYPES_MASK | TOK_SIGNED                 | 0x28)
DEF(TOK_U8,         "u8",               TOK_TYPES_MASK | TOK_UNSIGNED               | 0x29)
DEF(TOK_U16,        "u16",              TOK_TYPES_MASK | TOK_UNSIGNED               | 0x2A)
DEF(TOK_U32,        "u32",              TOK_TYPES_MASK | TOK_UNSIGNED               | 0x2B)
DEF(TOK_U64,        "u64",              TOK_TYPES_MASK | TOK_UNSIGNED               | 0x2C)
DEF(TOK_STRUCT,     "struct",           TOK_TYPES_SIGNATURE_MASK | TOK_OVERRIDABLE  | 0x2D)
DEF(TOK_TYPEDEF,    "type",             TOK_TYPES_SIGNATURE_MASK                    | 0x2F)
DEF(TOK_ENUM,       "enum",             TOK_TYPES_SIGNATURE_MASK | TOK_OVERRIDABLE  | 0x30)
DEF(TOK_CONST,      "const",            TOK_TYPES_SIGNATURE_MASK                    | 0x31)
DEF(TOK_PUB,        "pub",              TOK_TYPES_SIGNATURE_MASK                    | 0x32)
#endif

#ifdef TOK_DYN
DEF(TOK_STR_LIT,    "string literal",   TOK_DYN_MASK | TOK_LITERAL                      | 0x40)
DEF(TOK_INT_LIT,    "int literal",      TOK_DYN_MASK | TOK_LITERAL                      | 0x41)
DEF(TOK_FLOAT_LIT,  "float literal",    TOK_DYN_MASK | TOK_LITERAL                      | 0x43)
DEF(TOK_IDENT,      "identifier",       TOK_DYN_MASK | TOK_LITERAL | TOK_OVERRIDABLE    | 0x44)
#endif

#ifdef TOK_SYMBOLS
DEF(TOK_ARROW,          "->",           TOK_SYMBOLS_MASK | 0x50)
DEF(TOK_EQEQ,           "==",           TOK_SYMBOLS_MASK | 0x51)
DEF(TOK_NOTEQ,          "!=",           TOK_SYMBOLS_MASK | 0x52)
DEF(TOK_AND,            "&&",           TOK_SYMBOLS_MASK | 0x53)
DEF(TOK_OR,             "||",           TOK_SYMBOLS_MASK | 0x54)
DEF(TOK_ASSIGN,         ":=",           TOK_SYMBOLS_MASK | 0x55)
DEF(TOK_DOUBLE_COLON,   "::",           TOK_SYMBOLS_MASK | 0x71)
DEF(TOK_LPAREN,         "(",            TOK_SYMBOLS_MASK | 0x56)
DEF(TOK_RPAREN,         ")",            TOK_SYMBOLS_MASK | 0x57)
DEF(TOK_LBRACKET,       "{",            TOK_SYMBOLS_MASK | 0x58)
DEF(TOK_RBRACKET,       "}",            TOK_SYMBOLS_MASK | 0x59)
DEF(TOK_LSBRACKET,      "[",            TOK_SYMBOLS_MASK | 0x5A)
DEF(TOK_RSBRACKET,      "]",            TOK_SYMBOLS_MASK | 0x5B)
DEF(TOK_REF,            "&",            TOK_SYMBOLS_MASK | 0x5C)
DEF(TOK_STAR,           "*",            TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x5D)
DEF(TOK_PLUS,           "+",            TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x60)
DEF(TOK_MINUS,          "-",            TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x61)
DEF(TOK_DIV,            "/",            TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x62)
DEF(TOK_NOT,            "!",            TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x63)
DEF(TOK_COMMA,          ",",            TOK_SYMBOLS_MASK | TOK_OVERRIDABLE | 0x64)
DEF(TOK_EQ,             "=",            TOK_SYMBOLS_MASK | TOK_OVERRIDABLE | 0x65)
DEF(TOK_DOT,            ".",            TOK_SYMBOLS_MASK | TOK_OVERRIDABLE | 0x66)
DEF(TOK_GTE,            ">=",           TOK_SYMBOLS_MASK | 0x67)
DEF(TOK_LTE,            "<=",           TOK_SYMBOLS_MASK | 0x68)
DEF(TOK_GT,             ">",            TOK_SYMBOLS_MASK | 0x69)
DEF(TOK_LT,             "<",            TOK_SYMBOLS_MASK | 0x6A)
DEF(TOK_SEMICOLON,      ";",            TOK_SYMBOLS_MASK | TOK_OVERRIDABLE | 0x6B)
DEF(TOK_COLON,          ":",            TOK_SYMBOLS_MASK | TOK_OVERRIDABLE | 0x6C)
DEF(TOK_MACRO_EXPR,     "$",            TOK_SYMBOLS_MASK | 0x6D)
DEF(TOK_MACRO_IDENT,    "@",            TOK_SYMBOLS_MASK | 0x6E)
DEF(TOK_MACRO_TYPE,     "#",            TOK_SYMBOLS_MASK | 0x6F)
DEF(TOK_QUOTE,          "'",            TOK_SYMBOLS_MASK | 0x70)
#endif
