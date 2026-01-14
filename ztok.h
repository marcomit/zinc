/*
 * Definition of tokens using the X macros trick.
 */

#ifndef TOK_MASKS
#define TOK_MASKS

#define TOK_BASE_MASK 16

#define TOK_FLOWS_MASK           (1 << (TOK_BASE_MASK))
#define TOK_TYPES_MASK           (1 << (TOK_BASE_MASK + 1))
#define TOK_DYN_MASK             (1 << (TOK_BASE_MASK + 2))
#define TOK_SYMBOLS_MASK         (1 << (TOK_BASE_MASK + 3))
#define TOK_OPERATOR             (1 << (TOK_BASE_MASK + 4))
#define TOK_TYPES_SIGNATURE_MASK (1 << (TOK_BASE_MASK + 5))
#define TOK_LITERAL              (1 << (TOK_BASE_MASK + 6))
#endif

#ifdef TOK_FLOWS
DEF(TOK_IF,       "if",       TOK_FLOWS_MASK | 0x00)
DEF(TOK_ELSE,     "else",     TOK_FLOWS_MASK | 0x01)
DEF(TOK_WHILE,    "while",    TOK_FLOWS_MASK | 0x02)
DEF(TOK_FOR,      "for",      TOK_FLOWS_MASK | 0x03)
DEF(TOK_DO,       "do",       TOK_FLOWS_MASK | 0x04)
DEF(TOK_CONTINUE, "continue", TOK_FLOWS_MASK | 0x05)
DEF(TOK_BREAK,    "break",    TOK_FLOWS_MASK | 0x06)
DEF(TOK_RETURN,   "return",   TOK_FLOWS_MASK | 0x07)
DEF(TOK_GOTO,     "goto",     TOK_FLOWS_MASK | 0x08)
DEF(TOK_SWITCH,   "switch",   TOK_FLOWS_MASK | 0x09)
DEF(TOK_CASE,     "case",     TOK_FLOWS_MASK | 0x0A)
DEF(TOK_MODULE,   "use",      TOK_FLOWS_MASK | 0x0B)
#endif

#ifdef TOK_TYPES
DEF(TOK_VOID,    	"void",   	TOK_TYPES_MASK | 0x10)
DEF(TOK_CHAR,    	"char",   	TOK_TYPES_MASK | 0x11)
DEF(TOK_F32,     	"f32",    	TOK_TYPES_MASK | 0x12)
DEF(TOK_F64,     	"f64",    	TOK_TYPES_MASK | 0x13)
DEF(TOK_I8,      	"i8",     	TOK_TYPES_MASK | 0x14)
DEF(TOK_I16,     	"i16",    	TOK_TYPES_MASK | 0x15)
DEF(TOK_I32,     	"i32",    	TOK_TYPES_MASK | 0x16)
DEF(TOK_I64,     	"i64",    	TOK_TYPES_MASK | 0x17)
DEF(TOK_U8,      	"u8",     	TOK_TYPES_MASK | 0x18)
DEF(TOK_U16,     	"u16",    	TOK_TYPES_MASK | 0x19)
DEF(TOK_U32,     	"u32",    	TOK_TYPES_MASK | 0x1A)
DEF(TOK_U64,     	"u64",    	TOK_TYPES_MASK | 0x1B)
DEF(TOK_STRUCT,  	"struct", 	TOK_TYPES_SIGNATURE_MASK | 0x1C)
DEF(TOK_UNION,   	"union",  	TOK_TYPES_SIGNATURE_MASK | 0x1D)
DEF(TOK_TYPEDEF, 	"type",  		TOK_TYPES_SIGNATURE_MASK | 0x1E)
DEF(TOK_ENUM,    	"enum",   	TOK_TYPES_SIGNATURE_MASK | 0x1F)
DEF(TOK_CONST,   	"const",  	TOK_TYPES_SIGNATURE_MASK | 0x20)
DEF(TOK_VAR,   		"var",		 	TOK_TYPES_SIGNATURE_MASK | 0x20)
#endif

#ifdef TOK_DYN
DEF(TOK_STR_LIT,  "string literal",  TOK_LITERAL | TOK_DYN_MASK | 0x30)
DEF(TOK_INT_LIT,  "int literal",     TOK_LITERAL | TOK_DYN_MASK | 0x31)
DEF(TOK_BOOL_LIT, "boolean literal", TOK_LITERAL | TOK_DYN_MASK | 0x32)
DEF(TOK_IDENT,    "identifier",      TOK_DYN_MASK | 0x33)
#endif

#ifdef TOK_SYMBOLS
DEF(TOK_ARROW,     "->",  TOK_SYMBOLS_MASK | 0x40)
DEF(TOK_EQEQ,      "==",  TOK_SYMBOLS_MASK | 0x41)
DEF(TOK_NOTEQ,     "!=",  TOK_SYMBOLS_MASK | 0x42)
DEF(TOK_LPAREN,    "(",   TOK_SYMBOLS_MASK | 0x43)
DEF(TOK_RPAREN,    ")",   TOK_SYMBOLS_MASK | 0x44)
DEF(TOK_LBRACKET,  "{",   TOK_SYMBOLS_MASK | 0x45)
DEF(TOK_RBRACKET,  "}",   TOK_SYMBOLS_MASK | 0x46)
DEF(TOK_LSBRACKET, "[",   TOK_SYMBOLS_MASK | 0x47)
DEF(TOK_RSBRACKET, "]",   TOK_SYMBOLS_MASK | 0x48)
DEF(TOK_REF,       "&",   TOK_SYMBOLS_MASK | 0x49)
DEF(TOK_STAR,      "*",   TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x4A)
DEF(TOK_PLUS,      "+",   TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x4B)
DEF(TOK_MINUS,     "-",   TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x4C)
DEF(TOK_DIV,       "/",   TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x4D)
DEF(TOK_NOT,       "!",   TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x4E)
DEF(TOK_COMMA,     ",",   TOK_SYMBOLS_MASK | 0x4F)
DEF(TOK_EQ,        "=",   TOK_SYMBOLS_MASK | 0x50)
DEF(TOK_DOT,       ".",   TOK_SYMBOLS_MASK | 0x51)
DEF(TOK_GTE,       ">=",  TOK_SYMBOLS_MASK | 0x52)
DEF(TOK_LTE,       "<=",  TOK_SYMBOLS_MASK | 0x53)
DEF(TOK_GT,        ">",   TOK_SYMBOLS_MASK | 0x54)
DEF(TOK_LT,        "<",   TOK_SYMBOLS_MASK | 0x55)
DEF(TOK_AND,       "and", TOK_SYMBOLS_MASK | 0x56)
DEF(TOK_OR,        "or",  TOK_SYMBOLS_MASK | 0x57)
#endif
