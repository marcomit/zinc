/*
 * Definition of tokens using the X macros trick.
 * To categorize tokens we use bit flags
 * in order to check if a token is of a specific category.
 */

#ifndef TOK_MASKS
#define TOK_MASKS

#define TOK_BASE_MASK 16

#define TOK_FLOWS_MASK 		(1 << (TOK_BASE_MASK))
#define TOK_TYPES_MASK 		(1 << (TOK_BASE_MASK + 1))
#define TOK_DYN_MASK 			(1 << (TOK_BASE_MASK + 2))
#define TOK_SYMBOLS_MASK 	(1 << (TOK_BASE_MASK + 3))
#define TOK_OPERATOR			(1 << (TOK_BASE_MASK + 4))
#endif

#ifdef TOK_FLOWS
DEF(TOK_IF, 				"if", 			TOK_FLOWS_MASK | 0x00)
DEF(TOK_ELSE, 			"else", 		TOK_FLOWS_MASK | 0x01)
DEF(TOK_WHILE, 			"while", 		TOK_FLOWS_MASK | 0x02)
DEF(TOK_FOR, 				"for", 			TOK_FLOWS_MASK | 0x03)
DEF(TOK_DO, 				"do", 			TOK_FLOWS_MASK | 0x04)
DEF(TOK_CONTINUE, 	"continue", TOK_FLOWS_MASK | 0x05)
DEF(TOK_BREAK, 			"break", 		TOK_FLOWS_MASK | 0x06)
DEF(TOK_RETURN, 		"return", 	TOK_FLOWS_MASK | 0x07)
DEF(TOK_GOTO, 			"goto", 		TOK_FLOWS_MASK | 0x08)
DEF(TOK_SWITCH, 		"switch", 	TOK_FLOWS_MASK | 0x09)
DEF(TOK_CASE, 			"case", 		TOK_FLOWS_MASK | 0x0A)
DEF(TOK_MODULE,			"module", 	0x32)
#endif

#ifdef TOK_TYPES
DEF(TOK_VOID, 			"void", 		TOK_TYPES_MASK | 0x0B)
DEF(TOK_CHAR, 			"char",			TOK_TYPES_MASK | 0x0C)
DEF(TOK_INT, 				"int", 			TOK_TYPES_MASK | 0x0D)
DEF(TOK_FLOAT, 			"float", 		TOK_TYPES_MASK | 0x0E)
DEF(TOK_DOUBLE, 		"double", 	TOK_TYPES_MASK | 0x0F)
DEF(TOK_SHORT, 			"short", 		TOK_TYPES_MASK | 0x10)
DEF(TOK_LONG, 			"long", 		TOK_TYPES_MASK | 0x11)
DEF(TOK_STRUCT, 		"struct", 	TOK_TYPES_MASK | 0x12)
DEF(TOK_UNION, 			"union", 		TOK_TYPES_MASK | 0x13)
DEF(TOK_TYPEDEF, 		"typedef", 	TOK_TYPES_MASK | 0x14)
DEF(TOK_ENUM, 			"enum", 		TOK_TYPES_MASK | 0x15)
#endif

#ifdef TOK_DYN
DEF(TOK_STR_LIT, 		"string literal", 	TOK_DYN_MASK | 0x16)
DEF(TOK_INT_LIT, 		"int literal", 			TOK_DYN_MASK | 0x17)
DEF(TOK_BOOL_LIT, 	"boolean literal", 	TOK_DYN_MASK | 0x18)
DEF(TOK_IDENT,			"identifier",				TOK_DYN_MASK | 0x19)
#endif

#ifdef TOK_SYMBOLS

DEF(TOK_ARROW,			"->", 			TOK_SYMBOLS_MASK | 0x1A)
DEF(TOK_EQEQ,				"==", 			TOK_SYMBOLS_MASK | 0x1B)
DEF(TOK_NOTEQ,			"!=", 			TOK_SYMBOLS_MASK | 0x1C)
DEF(TOK_LPAREN,			"(", 				TOK_SYMBOLS_MASK | 0x1D)
DEF(TOK_RPAREN,			")", 				TOK_SYMBOLS_MASK | 0x1E)
DEF(TOK_LBRACKET,		"{", 				TOK_SYMBOLS_MASK | 0x1F)
DEF(TOK_RBRACKET,		"}", 				TOK_SYMBOLS_MASK | 0x20)
DEF(TOK_LSBRACKET,	"[", 				TOK_SYMBOLS_MASK | 0x21)
DEF(TOK_RSBRACKET,	"]", 				TOK_SYMBOLS_MASK | 0x22)
DEF(TOK_REF, 				"&", 				TOK_SYMBOLS_MASK | 0x23)
DEF(TOK_STAR,				"*", 				TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x24)
DEF(TOK_PLUS,				"+", 				TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x25)
DEF(TOK_MINUS,			"-", 				TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x26)
DEF(TOK_DIV,				"/", 				TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x27)
DEF(TOK_NOT,				"!", 				TOK_OPERATOR | TOK_SYMBOLS_MASK | 0x28)
DEF(TOK_COMMA, 			",", 				TOK_SYMBOLS_MASK | 0x29)
DEF(TOK_EQ, 				"=", 				TOK_SYMBOLS_MASK | 0x2A)
DEF(TOK_DOT,				".", 				TOK_SYMBOLS_MASK | 0x2B)
DEF(TOK_GTE,				">=", 			TOK_SYMBOLS_MASK | 0x2C)
DEF(TOK_LTE,				"<=", 			TOK_SYMBOLS_MASK | 0x2D)
DEF(TOK_GT,					">", 				TOK_SYMBOLS_MASK | 0x2E)
DEF(TOK_LT,					"<", 				TOK_SYMBOLS_MASK | 0x2F)
DEF(TOK_AND,				"and", 			TOK_SYMBOLS_MASK | 0x30)
DEF(TOK_OR,					"or", 			TOK_SYMBOLS_MASK | 0x31)

#endif
