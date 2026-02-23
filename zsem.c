/* This file is the Semantic analyzer.
 *
 * Now what is a semantic analyzer?
 * In this part of the compiler/interpreter
 * we have already built the AST (Abstract Syntax Tree) and
 * we want to make sure that all the types are correctly.
 * for example this expression 1230 + "hello"
 * is not valid because i cannot add a number to a string
 * but the parser parse it correctly. so we need to check every single expression/statement
 * that all types are correctly.
 * In this phase we also care about the scope of functions/variables.
 *
 * How do we handle the scope?
 *
 * We have a global scope for the entire project,
 * then a child scope for the current file and then a child for blocks like functions, loops etc..
 * */
#include "zinc.h"

static void analyzeStmt(ZSemantic *, ZNode *);
static void analyzeBlock(ZSemantic *, ZNode *, bool);
static ZType *resolveType(ZSemantic *, ZNode *);
static ZType *resolveTypeRef(ZSemantic *, ZType *);

/* ================== Scope / Symbol helpers ================== */

static ZScope *makescope(ZScope *parent) {
	ZScope *self = zalloc(ZScope);
	self->depth   = parent ? parent->depth + 1 : 0;
	self->parent  = parent;
	self->symbols = NULL;
	return self;
}

static ZSymbol *makesymbol(ZSymType kind) {
	ZSymbol *self = zalloc(ZSymbol);
	self->kind = kind;
	return self;
}

static ZSymTable *makesymtable(void) {
	ZSymTable *self  	= zalloc(ZSymTable);
	self->global     	= makescope(NULL);
	self->current    	= self->global;
	self->temp 				= NULL;
	self->funcs  			= NULL;
	return self;
}

static ZSemantic *makesemantic(ZState *state, ZNode *root) {
	ZSemantic *self       = zalloc(ZSemantic);
	self->root            = root;
	self->currentFuncRet  = NULL;
	self->loopDepth       = 0;
	self->state           = state;
	self->table           = makesymtable();
	self->scopes 					= NULL;
	
	return self;
}

static void putSymbol(ZSemantic *semantic, ZSymbol *symbol) {
	ZScope *scope = semantic->table->current;
	if (symbol->isPublic) scope = semantic->table->global;

	vecpush(scope->symbols, symbol);
}

static void putReceiverFunc(ZSemantic *semantic, ZNode *node) {
	let receiver = node->funcDef.receiver;
	let funcs = semantic->table->funcs;
	for (usize i = 0; i < veclen(funcs); i++) {
		if (typesEqual(funcs[i]->receiver, receiver->field.type)) {
			vecpush(funcs[i]->funcDef, node);
			return;
		}
	}

	ZFuncTable *func = zalloc(ZFuncTable);
	func->receiver = receiver->field.type;
	func->funcDef = NULL;
	vecpush(func->funcDef, node);
	vecpush(semantic->table->funcs, func);

}

static void putFunc(ZSemantic *semantic, ZNode *node) {
	if (node->funcDef.receiver) {
		putReceiverFunc(semantic, node);
		return;
	}

	ZSymbol *symbol   = makesymbol(Z_SYM_FUNC);
	symbol->name      = node->funcDef.ident->str;
	symbol->type      = node->funcDef.ret;
	symbol->node      = node;
	symbol->isPublic  = node->funcDef.pub;
	putSymbol(semantic, symbol);
}

static void putStruct(ZSemantic *semantic, ZNode *node) {
	ZSymbol *symbol   = makesymbol(Z_SYM_STRUCT);
	symbol->name      = node->structDef.ident->str;
	symbol->node      = node;
	symbol->isPublic  = node->structDef.pub;

	ZType *type        = maketype(Z_TYPE_STRUCT);
	type->strct.name   = node->structDef.ident;
	type->strct.fields = node->structDef.fields;
	type->strct.generics = NULL;
	symbol->type       = type;

	putSymbol(semantic, symbol);
}

static void registerModule(ZSemantic *semantic, ZNode *module) {
	ZScope *scope = NULL;
	for (usize i = 0; i < veclen(semantic->scopes); i++) {
		if (semantic->scopes[i]->module == module) {
			scope = semantic->scopes[i]->scope;
			goto setScope;
		}
	}

	ZScopeTable *table = zalloc(ZScopeTable);
	table->module = module;
	table->scope = makescope(semantic->table->global);

	vecpush(semantic->scopes, table);
	scope = table->scope;

setScope:
	semantic->table->temp = semantic->table->current;
	semantic->table->current = scope;
}

static void endModule(ZSemantic *semantic) {
	if (!semantic || !semantic->table || !semantic->table->temp) return;
	semantic->table->current = semantic->table->temp;
}

static void beginScope(ZSemantic *semantic) {
	ZScope *scope         = makescope(semantic->table->current);
	semantic->table->current = scope;
}

static void endScope(ZSemantic *semantic) {
	if (!semantic->table->current || !semantic->table->current->parent) {
		printf("Cannot end scope: already at the top\n");
		return;
	}
	semantic->table->current = semantic->table->current->parent;
}

/* ================== Type arithmetic ================== */

static u8 typeRank(ZTokenType t) {
	switch (t) {
	case TOK_CHAR:              return 0;
	case TOK_I8:  case TOK_U8:  return 1;
	case TOK_I16: case TOK_U16: return 2;
	case TOK_I32: case TOK_U32: return 3;
	case TOK_I64: case TOK_U64: return 4;
	case TOK_F32:               return 5;
	case TOK_F64:               return 6;
	default:                    return 0;
	}
}

static inline bool isUnsigned(ZTokenType t) { return (bool)(t & TOK_UNSIGNED); }
static inline bool isSigned  (ZTokenType t) { return (bool)(t & TOK_SIGNED);   }
static inline bool isFloat   (ZTokenType t) { return (bool)(t & TOK_FLOAT);    }
static inline bool isInteger (ZTokenType t) { return isSigned(t) || isUnsigned(t); }

static ZTokenType toSigned(u8 rank) {
	switch (rank) {
	case 1: return TOK_I8;
	case 2: return TOK_I16;
	case 3: return TOK_I32;
	case 4: return TOK_I64;
	default: return TOK_I32;
	}
}

/* An implementation note:
 * if a or b is a float the return type is always a float.
 * if a and b are both signed or unsigned return the type with the highest rank.
 * if they are unsigned vs signed and the unsigned is u64 can't promote to i128
 * so in this case the compiler shows a warning (explicit casting requested).
 * */
ZType *typesCompatible(ZState *state, ZType *a, ZType *b) {
	if (!a || !b) return NULL;
	if (typesEqual(a, b)) return a;

	if (a->kind != Z_TYPE_PRIMITIVE || b->kind != Z_TYPE_PRIMITIVE)
		return NULL;

	ZTokenType ta = a->primitive.token->type;
	ZTokenType tb = b->primitive.token->type;

	if (ta == TOK_VOID || tb == TOK_VOID) return NULL;

	u8 ra = typeRank(ta);
	u8 rb = typeRank(tb);

	if (isFloat(ta) || isFloat(tb)) {
		u8 minRank = min(ra, rb);
		if (minRank <= 2)
			return ra > rb ? a : b;
		ZType *promoted = maketype(Z_TYPE_PRIMITIVE);
		promoted->primitive.token = maketoken(TOK_F64);
		return promoted;
	}

	if ((isSigned(ta) && isSigned(tb)) ||
	    (isUnsigned(ta) && isUnsigned(tb)))
		return ra > rb ? a : b;

	/* signed vs unsigned */
	u8    signedRank   = isSigned(ta) ? ra : rb;
	u8    unsignedRank = isSigned(ta) ? rb : ra;
	ZType *signedType  = isSigned(ta) ? a  : b;

	if (signedRank > unsignedRank) return signedType;

	if (signedRank == 4) {
		warning(state, signedType->primitive.token,
		        "Cannot promote an i64, try with an explicit cast");
	}

	ZType *promoted = maketype(Z_TYPE_PRIMITIVE);
	promoted->primitive.token = maketoken(toSigned(signedRank + 1));
	return promoted;
}

bool typesEqual(ZType *a, ZType *b) {
	if (!a || !b) return false;

	if (a->kind != b->kind) return false;

	switch (a->kind) {
	case Z_TYPE_PRIMITIVE:
		return a->primitive.token->type == b->primitive.token->type;
	case Z_TYPE_POINTER:
		return typesEqual(a->base, b->base);
	case Z_TYPE_ARRAY:
		return typesEqual(a->array.base, b->array.base);
	case Z_TYPE_STRUCT:
	case Z_TYPE_FUNCTION:
		return a == b;
	case Z_TYPE_TUPLE: {
		if (veclen(a->tuple) != veclen(b->tuple)) return false;
		for (usize i = 0; i < veclen(a->tuple); i++)
			if (!typesEqual(a->tuple[i], b->tuple[i])) return false;
		return true;
	}
	case Z_TYPE_GENERIC: {
		if (strcmp(a->generic.name->str, b->generic.name->str) != 0) return false;
		if (veclen(a->generic.args) != veclen(b->generic.args)) return false;
		for (usize i = 0; i < veclen(a->generic.args); i++)
			if (!typesEqual(a->generic.args[i], b->generic.args[i])) return false;
		return true;
	}
	default:
		return false;
	}
}

/* ================== Symbol lookup ================== */

static ZSymbol *resolve(ZSemantic *semantic, ZToken *ident) {
	ZScope *curr = semantic->table->current;
	while (curr) {
		for (usize i = 0; i < veclen(curr->symbols); i++) {
			if (strcmp(curr->symbols[i]->name, ident->str) == 0) {
				return curr->symbols[i];
			}
		}
		curr = curr->parent;
	}
	return NULL;
}

/* ================== Type resolution ================== */

static ZType *derefType(ZType *t) {
	while (t && t->kind == Z_TYPE_POINTER) t = t->base;
	return t;
}

static ZType *resolveLiteralType(ZNode *curr) {
	ZType *t = maketype(Z_TYPE_PRIMITIVE);
	switch (curr->literalTok->type) {
	case TOK_INT_LIT: {
		t->primitive.token = maketoken(TOK_I32);
		return t;
	}
	case TOK_FLOAT_LIT: {
		t->primitive.token = maketoken(TOK_F64);
		return t;
	}
	case TOK_BOOL_LIT: {
		t->primitive.token = maketoken(TOK_BOOL);
		return t;
	}
	case TOK_STR_LIT: {
		/* String literals are *char */
		ZType *base = maketype(Z_TYPE_PRIMITIVE);
		base->primitive.token = maketoken(TOK_CHAR);
		ZType *ptr  = maketype(Z_TYPE_POINTER);
		ptr->base   = base;
		return ptr;
	}
	default: {
		t->primitive.token = maketoken(TOK_VOID);
		return t;
	}
	}
}

/*
 * Resolve a ZType that may contain named references (user-defined types).
 *
 * When the parser sees `MyStruct x`, the type is stored as a Z_TYPE_PRIMITIVE
 * with primitive.token->type == TOK_IDENT and primitive.token->str == "MyStruct".
 * This function looks that name up in the symbol table and returns the actual
 * ZType registered by putStruct() / a typedef entry.
 *
 * For compound types (pointer, array, function, tuple) it recurses into their
 * sub-types so that e.g. `*MyStruct` gets fully resolved.
 */
static ZType *resolveTypeRef(ZSemantic *semantic, ZType *type) {
	if (!type) return NULL;

	switch (type->kind) {
	case Z_TYPE_PRIMITIVE: {
		if (type->primitive.token->type != TOK_IDENT) return type;
		ZSymbol *sym = resolve(semantic, type->primitive.token);
		if (!sym) {
			error(semantic->state, type->primitive.token,
			      "Unknown type '%s'", type->primitive.token->str);
			return NULL;
		}
		return sym->type;
	}
	case Z_TYPE_POINTER:
		type->base = resolveTypeRef(semantic, type->base);
		return type;
	case Z_TYPE_ARRAY:
		type->array.base = resolveTypeRef(semantic, type->array.base);
		return type;
	case Z_TYPE_FUNCTION:
		type->func.ret = resolveTypeRef(semantic, type->func.ret);
		for (usize i = 0; i < veclen(type->func.args); i++)
			type->func.args[i] = resolveTypeRef(semantic, type->func.args[i]);
		return type;
	case Z_TYPE_TUPLE:
		for (usize i = 0; i < veclen(type->tuple); i++)
			type->tuple[i] = resolveTypeRef(semantic, type->tuple[i]);
		return type;
	default:
		return type;
	}
}

static ZType *resolveMemberAccess(ZSemantic *, ZNode *);
static ZType *resolveArrSubscript(ZSemantic *, ZNode *);

/*
 * Resolve the type of any expression node and cache the result in node->resolved.
 * Returns the resolved ZType* or NULL on error.
 */
static ZType *resolveType(ZSemantic *semantic, ZNode *curr) {
	if (!curr)           return NULL;
	if (curr->resolved)  return curr->resolved;

	ZType *result = NULL;

	switch (curr->type) {

	case NODE_LITERAL:
		result = resolveLiteralType(curr);
		break;

	case NODE_IDENTIFIER: {
		ZSymbol *sym = resolve(semantic, curr->identTok);
		if (!sym) {
			error(semantic->state, curr->identTok,
			      "Undefined variable '%s'", curr->identTok->str);
			return NULL;
		}
		result = sym->type;
		break;
	}

	case NODE_BINARY: {
		ZTokenType op    = curr->binary.op->type;
		ZType     *left  = resolveType(semantic, curr->binary.left);
		ZType     *right = resolveType(semantic, curr->binary.right);

		/* Comparison / logical operators always produce a bool. */
		if (op == TOK_EQEQ || op == TOK_NOTEQ ||
		    op == TOK_LT   || op == TOK_GT    ||
		    op == TOK_LTE  || op == TOK_GTE   ||
		    op == TOK_AND  || op == TOK_OR    ||
		    op == TOK_SAND || op == TOK_SOR) {
			ZType *boolType = maketype(Z_TYPE_PRIMITIVE);
			boolType->primitive.token = maketoken(TOK_BOOL);
			result = boolType;
		} else if (op == TOK_EQ) {
			/* Assignment yields the type of the left-hand side. */
			result = left;
		} else {
			result = typesCompatible(semantic->state, left, right);
			if (!result) {
				error(semantic->state, curr->binary.op,
				      "Incompatible types in binary expression");
			}
		}
		break;
	}

	case NODE_UNARY: {
		ZType     *operand = resolveType(semantic, curr->unary.operand);
		ZTokenType op      = curr->unary.operat->type;

		if (op == TOK_REF) {
			/* &expr => *T */
			ZType *ptr = maketype(Z_TYPE_POINTER);
			ptr->base  = operand;
			result     = ptr;
		} else if (op == TOK_STAR) {
			/* *ptr => T */
			if (operand && operand->kind == Z_TYPE_POINTER) {
				result = operand->base;
			} else {
				error(semantic->state, curr->unary.operat,
				      "Cannot dereference a non-pointer type");
				result = operand;
			}
		} else {
			/* -, !, not — type is unchanged */
			result = operand;
		}
		break;
	}

	case NODE_CALL: {
		ZNode *callee = curr->call.callee;

		if (callee->type == NODE_IDENTIFIER) {
			ZSymbol *sym = resolve(semantic, callee->identTok);
			if (!sym) {
				error(semantic->state, callee->identTok,
				      "Undefined function '%s'", callee->identTok->str);
				return NULL;
			}
			if (sym->kind != Z_SYM_FUNC) {
				error(semantic->state, callee->identTok,
				      "'%s' is not callable", callee->identTok->str);
				return NULL;
			}
			/* sym->type is the raw parsed return type — resolve it so that named
		 * types (e.g. "Vec2" → Z_TYPE_STRUCT) are expanded before the
		 * result is used downstream (e.g. for member access on return value). */
			result = resolveTypeRef(semantic, sym->type);
		} else if (callee->type == NODE_MEMBER) {

		} else {
			/* Expression call: resolve callee type and extract return type. */
			ZType *calleeType = resolveType(semantic, callee);
			if (calleeType && calleeType->kind == Z_TYPE_FUNCTION)
				result = resolveTypeRef(semantic, calleeType->func.ret);
		}

		break;
	}

	case NODE_MEMBER:
		result = resolveMemberAccess(semantic, curr);
		break;

	case NODE_SUBSCRIPT:
		result = resolveArrSubscript(semantic, curr);
		break;

	case NODE_VAR_DECL:
		/* Used when a var-decl appears as a sub-expression (unusual but safe). */
		if (curr->varDecl.type)
			result = resolveTypeRef(semantic, curr->varDecl.type);
		else if (curr->varDecl.rvalue)
			result = resolveType(semantic, curr->varDecl.rvalue);
		break;

	case NODE_STRUCT_LIT: {
		for (usize i = 0; i < veclen(curr->structlit.fields); i++) {
			let field = curr->structlit.fields[i]->varDecl;
			ZType *type = resolveType(semantic, field.rvalue);
			if (!typesCompatible(semantic->state, field.type, type)) {
				error(semantic->state, field.ident->identTok, "Mismatch types");
			}
		}
		let resolved = resolve(semantic, curr->structlit.ident);

		if (!resolved) {
			error(semantic->state, curr->structlit.ident, "Unknown struct literal");
		} else {
			if (resolved->kind != Z_SYM_STRUCT) {
				error(semantic->state, curr->structlit.ident, "This is not a struct literal");
			}
			result = resolved->type;
		}

		break;
	}

	default:
		break;
	}

	curr->resolved = result;
	return result;
}

static ZType *resolveReceiverCall(ZSemantic *semantic, ZType *caller, ZToken *name) {
	ZFuncTable **table = semantic->table->funcs;
	ZNode **funcs = NULL;

	for (usize i = 0; i < veclen(table) && !funcs; i++) {
		ZType *receiverType = resolveTypeRef(semantic, table[i]->receiver);
		table[i]->receiver = receiverType;
		if (typesEqual(receiverType, caller)) {
			funcs = table[i]->funcDef;
		}
	}
	if (!funcs) return NULL;

	for (usize i = 0; i < veclen(funcs); i++) {
		if (tokeneq(funcs[i]->funcDef.ident, name)) {
			return funcs[i]->resolved;
		}
	}

	return NULL;
}

static ZType *resolveMemberAccess(ZSemantic *semantic, ZNode *curr) {
	ZType *objType = resolveType(semantic, curr->memberAccess.object);
	if (!objType) {
		error(semantic->state, curr->tok,
		      "Cannot resolve object type in member access");
		return NULL;
	}

	ZType *base = derefType(objType);
	if (!base || base->kind != Z_TYPE_STRUCT) {
		error(semantic->state, curr->tok,
		      "Expected a struct type for '.' access");
		return NULL;
	}

	ZNode **fields = base->strct.fields;
	for (usize i = 0; i < veclen(fields); i++) {
		if (tokeneq(fields[i]->field.identifier, curr->memberAccess.field))
			return resolveTypeRef(semantic, fields[i]->field.type);
	}

	ZType *result = resolveReceiverCall(semantic, objType, curr->memberAccess.field);
	if (result) return result;

	error(semantic->state, curr->memberAccess.field,
	      "Member '%s' not found in struct", curr->memberAccess.field->str);
	return NULL;
}

static ZType *resolveArrSubscript(ZSemantic *semantic, ZNode *curr) {
	ZType *arrType   = resolveType(semantic, curr->subscript.arr);
	ZType *indexType = resolveType(semantic, curr->subscript.index);

	printf("Array: %d ", arrType->kind);
	printType(arrType->array.base);
	printf("\n");

	if (!arrType ||
			(arrType->kind != Z_TYPE_ARRAY &&
			arrType->kind != Z_TYPE_POINTER)) {
		error(semantic->state, curr->tok,
		      "Expected an array type for subscript");
		return NULL;
	}

	if (!indexType || indexType->kind != Z_TYPE_PRIMITIVE ||
	    !isInteger(indexType->primitive.token->type)) {
		error(semantic->state, curr->tok,
		      "Array index must be an integer");
		return NULL;
	}


	return arrType->array.base;
}

/* ================== Statement analysis ================== */

static void analyzeVar(ZSemantic *semantic, ZNode *curr, bool isGlobal) {
	ZToken *var = curr->varDecl.ident->identTok;
	if (resolve(semantic, var)) {
		error(semantic->state, var,
					"Redefinition of variable %s",
					stoken(var));
	}

	ZType *rvalueType   = NULL;
	ZType *declaredType = NULL;

	if (curr->varDecl.rvalue) {
		rvalueType = resolveType(semantic, curr->varDecl.rvalue);
	}

	if (curr->varDecl.type) {
		declaredType = resolveTypeRef(semantic, curr->varDecl.type);
		if (rvalueType &&
		    !typesCompatible(semantic->state, declaredType, rvalueType)) {
			error(semantic->state, curr->tok,
			      "Type mismatch: cannot assign value to variable of this type");
		}
	} else {
		/* Inferred type (:= syntax) */
		if (!rvalueType) {
			error(semantic->state, curr->tok,
			      "Cannot infer type without an initializer");
		}
		declaredType         = rvalueType;
		curr->varDecl.type   = rvalueType;
	}

	curr->resolved = declaredType;

	ZSymbol *symbol   = makesymbol(Z_SYM_VAR);
	symbol->name      = var->str;
	symbol->type      = declaredType;
	symbol->node      = curr;
	symbol->isPublic  = isGlobal;
	putSymbol(semantic, symbol);
}

static void analyzeIf(ZSemantic *semantic, ZNode *curr) {
	ZType *cond = resolveType(semantic, curr->ifStmt.cond);
	
	if (!cond) {
		error(semantic->state, curr->ifStmt.cond->tok, "Unknown type condition");
	} else if (!(cond->kind & Z_TYPE_COMPARABLE_MASK)) {
		error(semantic->state, curr->ifStmt.cond->tok, "Condition must be a comparable value");
	}

	analyzeBlock(semantic, curr->ifStmt.body, true);

	if (curr->ifStmt.elseBranch) {
		ZNode *el = curr->ifStmt.elseBranch;
		if (el->type == NODE_IF)
			analyzeIf(semantic, el);
		else
			analyzeBlock(semantic, el, true);
	}
}

static void analyzeWhile(ZSemantic *semantic, ZNode *curr) {
	resolveType(semantic, curr->whileStmt.cond);
	semantic->loopDepth++;
	analyzeBlock(semantic, curr->whileStmt.branch, true);
	semantic->loopDepth--;
}

static void analyzeFor(ZSemantic *semantic, ZNode *curr) {
	beginScope(semantic);
	let f = curr->forStmt;
	analyzeVar(semantic, f.var, false);

	ZType *cond = resolveType(semantic, f.cond);

	if (!cond || cond->kind != Z_TYPE_PRIMITIVE) {

	}

	semantic->loopDepth++;
	analyzeBlock(semantic, curr->forStmt.block, false);
	semantic->loopDepth--;

	endScope(semantic);
}

static void analyzeFunc(ZSemantic *semantic, ZNode *curr) {
	beginScope(semantic);

	for (usize i = 0; i < veclen(curr->funcDef.args); i++) {
		ZNode  *arg     = curr->funcDef.args[i];
		ZType  *argType = resolveTypeRef(semantic, arg->field.type);

		if (!argType) {
			error(semantic->state, curr->tok, "Unknown type");
		}

		ZSymbol *sym  = makesymbol(Z_SYM_VAR);
		sym->name     = arg->field.identifier->str;
		sym->type     = argType;
		sym->node     = arg;
		sym->isPublic = false;
		putSymbol(semantic, sym);
	}

	if (curr->funcDef.receiver) {
		ZNode *receiver = curr->funcDef.receiver;
		ZType *recType = resolveTypeRef(semantic, receiver->field.type);

		ZSymbol *sym = makesymbol(Z_SYM_VAR);
		sym->name 		= receiver->field.identifier->str;
		sym->type 		= recType;
		sym->node 		= curr->funcDef.receiver;
		sym->isPublic = false;
		putSymbol(semantic, sym);
	}

	ZType *savedRet          = semantic->currentFuncRet;
	semantic->currentFuncRet = resolveTypeRef(semantic, curr->funcDef.ret);

	analyzeBlock(semantic, curr->funcDef.body, false);

	semantic->currentFuncRet = savedRet;
	endScope(semantic);
}

static void analyzeStmt(ZSemantic *semantic, ZNode *curr) {
	switch (curr->type) {
	case NODE_VAR_DECL: analyzeVar(semantic, curr, false); break;
	case NODE_IF: analyzeIf(semantic, curr); break;
	case NODE_WHILE: analyzeWhile(semantic, curr); break;
	case NODE_FOR: analyzeFor(semantic, curr); break;
	case NODE_BLOCK: analyzeBlock(semantic, curr, false); break;
	case NODE_DEFER: resolveType(semantic, curr->deferStmt.expr); break;
	default: resolveType(semantic, curr); break;

	case NODE_RETURN: {
		ZType *retType = NULL;
		if (curr->returnStmt.expr)
			retType = resolveType(semantic, curr->returnStmt.expr);

		curr->resolved = retType;

		if (semantic->currentFuncRet) {
			bool isVoidRet  = (retType == NULL);
			bool isVoidFunc = semantic->currentFuncRet->kind == Z_TYPE_PRIMITIVE &&
			                  semantic->currentFuncRet->primitive.token->type == TOK_VOID;

			if (isVoidFunc && !isVoidRet) {
				error(semantic->state, curr->tok,
				      "Unexpected return value in void function");
			} else if (!isVoidFunc && isVoidRet) {
				error(semantic->state, curr->tok,
				      "Expected a return value");
			} else if (!isVoidFunc && !isVoidRet &&
			           !typesCompatible(semantic->state, retType, semantic->currentFuncRet)) {
				error(semantic->state, curr->tok,
				      "Return type does not match function signature");
			}
		}
		break;
	}
	case NODE_CALL: {
		ZType *resolved = resolveType(semantic, curr->call.callee);
		if (!resolved) {
			error(semantic->state, curr->tok, "Unknown function");
			break;
		}

		if (resolved->kind != Z_TYPE_FUNCTION) {
			error(semantic->state, curr->tok, "Must be a function type");
			break;
		}

		usize expected = veclen(resolved->func.args);
		usize got = veclen(curr->call.args);

		if (expected != got) {
			error(semantic->state, curr->tok, "Expected %zu arguments, got %zu", expected, got);
			break;
		}

		for (usize i = 0; i < got; i++) {
				ZType *argType = resolveType(semantic, curr->call.args[i]);
			if (!typesCompatible(semantic->state,
														argType, resolved->func.args[i])) {
				error(semantic->state, curr->tok, "Unexpected type");
			}
		}

		break;
	}
	}
}

static void analyzeBlock(ZSemantic *semantic, ZNode *block, bool scoped) {
	if (scoped) beginScope(semantic);

	ZNode **stmts = block->block;
	for (usize i = 0; i < veclen(stmts); i++)
		analyzeStmt(semantic, stmts[i]);

	if (scoped) endScope(semantic);
}

/* ================== Global scope discovery ================== */

static void discoverGlobalScope(ZSemantic *semantic, ZNode *root) {
	for (usize i = 0; i < veclen(root->module.root); i++) {
		ZNode *child = root->module.root[i];

		switch (child->type) {
		case NODE_FUNC: 	putFunc(semantic, child); 	break;
		case NODE_STRUCT: putStruct(semantic, child); break;

		case NODE_TYPEDEF: {
			/* Register a type alias so named references to it can be resolved. */
			ZSymbol *symbol   = makesymbol(Z_SYM_STRUCT);
			symbol->name      = child->typeDef.alias->str;
			symbol->node      = child;
			symbol->type      = child->typeDef.type;
			symbol->isPublic  = false;
			putSymbol(semantic, symbol);
			break;
		}

		case NODE_FOREIGN: {
			/* Foreign functions are callable like regular functions. */
			ZSymbol *symbol   = makesymbol(Z_SYM_FUNC);
			symbol->name      = child->foreignFunc.tok->str;
			symbol->node      = child;
			symbol->type      = child->foreignFunc.ret;
			symbol->isPublic  = true;
			putSymbol(semantic, symbol);
			break;
		}

		case NODE_MODULE:
			if (child->module.root) {
				registerModule(semantic, child);
				discoverGlobalScope(semantic, child);
				endModule(semantic);
			}
			break;

		default: break;
		}
	}
}

/* ================== Main analysis pass ================== */

static void analyze(ZSemantic *semantic, ZNode *root) {
	for (usize i = 0; i < veclen(root->module.root); i++) {
		ZNode *child = root->module.root[i];

		switch (child->type) {
		case NODE_FUNC: 		analyzeFunc(semantic, child); 			break;
		case NODE_VAR_DECL: analyzeVar(semantic, child, true); 	break;

		case NODE_STRUCT:
			for (usize i = 0; i < veclen(child->structDef.fields); i++) {
				let field = child->structDef.fields[i]->field;
				resolveTypeRef(semantic, field.type);
			}
			break;

		case NODE_UNION:
			warning(semantic->state, root->tok, "'union' not yet analyzed");
			break;

		case NODE_MODULE:
			if (child->module.root) {
				registerModule(semantic, child);
				analyze(semantic, child);
				endModule(semantic);
			}
			break;

		default: break;
		}
	}
}

void zanalyze(ZState *state, ZNode *root) {
	ZSemantic *semantic = makesemantic(state, root);
	registerModule(semantic, root);
	discoverGlobalScope(semantic, root);
	analyze(semantic, root);
}
