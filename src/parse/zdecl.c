#include "zinc/zstate.h"
#include "zparse.h"

static ZNode *parseImpl(ZParser *parser, ZAnnotation **implAnnotations, bool public) {
    ZToken *start = peek(parser);
    ZType *type = NULL;

    if (!check(parser, TOK_IDENT) &&
        !checkMask(parser, TOK_TYPES_MASK))
        return NULL;

    type = maketype(parser->allocator, Z_TYPE_PRIMITIVE);
    type->tok = start;
    type->primitive.token = consume(parser);

    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_IMPL);

    /* Declare facets this block must implement. */
    ZType **facets = NULL;
    if (match(parser, TOK_LPAREN)) {
        do {
            ZType *facet            = maketype(parser->allocator, Z_TYPE_PRIMITIVE);
            facet->primitive.token  = consume(parser);
            facet->tok              = facet->primitive.token;
            vecpush(facets, facet);
        } while (   !check(parser, TOK_RPAREN)      &&
                     match(parser, TOK_PLUS)        );
        expect(parser, TOK_RPAREN);
    }

    /* Declare generics that every function in this block inherit. */
    ZType **generics = NULL;
    if (match(parser, TOK_WHERE)) {
        generics = parseGenericsDecl(parser);

        if (!generics) {
            zlog(parser->state, peek(parser), Z2038);
        }
    }

    ZNode **capabilities = parseCapabilityList(parser);

    expect(parser, TOK_LBRACKET);

    ZNode *func                 = NULL;

    ZNode *block                = makenode(parser->allocator, NODE_IMPL);
    block->impl.base            = type;
    // block->impl.self            = NULL;
    block->impl.funcs           = NULL;
    block->impl.facets          = facets;
    block->impl.generics        = generics;
    block->impl.capabilities    = capabilities;
    block->impl.pub             = public;
    block->annotations          = implAnnotations;

    ZAnnotation **annotations   = NULL;
    parser->inImplBlock         = true;
    parser->implBase            = type;
    do {
        annotations = NULL;
        if (check(parser, TOK_HASHTAG)) {
            annotations = parseAnnotations(parser);
        }
        public = match(parser, TOK_PUB);
        func = parseFuncDecl(parser, annotations, public);
        if (!func) {
            if (public) {
                parser->inImplBlock = false;
                parser->implBase    = NULL;
                zlog(parser->state, peek(parser), Z2039);
                return NULL;
            }
            break;
        }

        for (usize i = 0; i < veclen(capabilities); i++) {
            vecpush(func->funcDef.capabilities, capabilities[i]);
            vecpush(func->resolved->func.capabilities, capabilities[i]->field.type);
        }
        vecpush(block->impl.funcs, func);
    } while (!check(parser, TOK_RBRACKET));

    parser->inImplBlock = false;
    expect(parser, TOK_RBRACKET);

    setspan(parser, block, start);

    usize len = veclen(block->impl.funcs);

    if (len == 0) return block;

    /* For static mangling we key on the primitive type name (matching what the
     * call-site parser emits for `Type.method`). Strip pointer levels to get
     * the base primitive token. */
    ZType *baseType = type;
    while (baseType->kind == Z_TYPE_POINTER) baseType = baseType->base;
    ZToken *typeNameTok = baseType->primitive.token;

    for (usize i = 0; i < len; i++) {
        ZNode *f = block->impl.funcs[i];
        char *mangled = NULL;
        if (f->funcDef.receiver) {
            mangled = manglerM(type, f->funcDef.name);

        } else {
            mangled = mangler((const char *[]) {
                typeNameTok->str,
                f->funcDef.name->str,
                NULL
            });
            f->funcDef.base = type;
        }
        f->funcDef.mangled = mangled;
    }

    return block;
}

static ZVisitorState checkReceiverType(ZType *type, ZClientData cd) {
    switch (type->kind) {
    case Z_TYPE_PRIMITIVE: {
        const char *s = stoken(type->primitive.token);
        return strcmp(s, (const char *)cd) == 0 ? Z_BREAK : Z_STOP;
    }
    case Z_TYPE_POINTER:
    case Z_TYPE_ARRAY:
    case Z_TYPE_OPTIONAL:
        return Z_CONTINUE;
    default: return Z_STOP;
    }
}

static inline bool isImplicitSelfField(ZType *type) {
    return visitType(&((ZTypeVisitor){ checkReceiverType, "self" }), type);
}

static inline bool isSelfField(ZParser *parser, ZType *type) {
    if (!parser->implBase) return false;
    return visitType(&((ZTypeVisitor){
        checkReceiverType, parser->implBase->primitive.token
    }), type);
}

static inline ZType *receiverLeaf(ZType *t) {
    for (;;) switch (t->kind) {
        case Z_TYPE_POINTER:    t = t->base;        break;
        case Z_TYPE_OPTIONAL:   t = t->optional;    break;
        case Z_TYPE_ARRAY:      t = t->array.base;  break;
        default:                return t;
    }
}


static ZNode *parseFuncField(ZParser *parser) {
    if (check(parser, TOK_IDENT) && checkAhead(parser, TOK_COLON, 1)) {
        ZToken *name = peek(parser);
        ZNode *field = parseField(parser);

        if (strcmp(stoken(name), "self") == 0 &&
            isSelfField(parser, field->field.type))
            field->type = NODE_FUNC_FIELD;

        return field;
    }

    ZToken *start = peek(parser);
    ZType *type = parseType(parser);

    ZNode *node             = makenode(parser->allocator, NODE_FUNC_FIELD);

    if (!isImplicitSelfField(type)) {
        zlog(parser->state, start, Z203F, stype(type));
    } else {
        ZType *leaf = receiverLeaf(type);
        node->field.identifier  = leaf->primitive.token;
        leaf->primitive.token   = parser->implBase->primitive.token;
    }

    node->resolved          = type;
    node->tok               = start;
    node->field.type        = type;
    return setspan(parser, node, start);
}

static ZType *parseFuncMultiReturn(ZParser *parser) {
    ZToken *start   = peek(parser);
    ZType **list    = NULL;
    ZType *ret      = NULL;

    do {
        ret = tryParse(parser, parseType(parser));
        if (!ret) break;
        vecpush(list, ret);
    } while (   !check(parser, TOK_WITH)        &&
                !check(parser, TOK_LBRACKET)    &&
                match(parser, TOK_COMMA)        );

    if (veclen(list) > 1) {
        ret         = maketype(parser->allocator, Z_TYPE_TUPLE);
        ret->tuple  = list;
        ret->tok    = start;
    }

    if (!ret) return consts(parser->state, u0Type);
    return ret;
}

static inline void checkOptFieldsAtEnd(ZParser *parser, ZNode **fields) {
    bool opt = false;
    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *dv = fields[i]->field.defaultValue;
        if (dv) opt = true;
        else if (opt) zlog(parser->state, fields[i]->tok, Z2040);
    }
}

/* The caller must handle:
 * - the mangling name.
 * - the receiver node if it is a receiver function.
 * - the base type if it is a static function.
 *   */
ZNode *parseFuncDecl(ZParser *parser,
    ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);
    ZNode *receiver = NULL;

    if (!check(parser, TOK_IDENT)) return NULL;
    ZToken *name = consume(parser);

    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_FN);

    ZType **generics = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser);
    }

    ZNode **args = parseGenericList(parser,
        TOK_LPAREN,         TOK_RPAREN,
        parseFuncField,  true
    );

    if (veclen(args) && args[0]->type == NODE_FUNC_FIELD) {
        receiver = args[0];
        for (usize i = 0; i < veclen(args) - 1; i++)
            args[i] = args[i + 1];
        vecpop(args);
    }

    checkOptFieldsAtEnd(parser, args);

    ZType *ret = parseFuncMultiReturn(parser);

    if (!ret && !check(parser, TOK_WITH) && !check(parser, TOK_LBRACKET)) {
        zlog(parser->state, start, Z2022);
        return NULL;
    }

    if (!ret) return NULL;

    ZNode **capabilities = parseCapabilityList(parser);

    if (match(parser, TOK_WHERE)) {
        generics = parseGenericsDecl(parser);
        if (!generics) {
            zlog(parser->state, peek(parser), Z2023);
        }
    }

    ZNode *body     = NULL;
    ZToken *arrow   = peek(parser);

    if (match(parser, TOK_ARROW)) {
        ZNode *expr = tryParse(parser, parseExpr(parser));
        if (expr) {
            ZNode *ret = makenode(parser->allocator, NODE_RETURN);
            ret->returnStmt.expr = expr;
            setspanfrom(parser, ret, expr);
            body = makenode(parser->allocator, NODE_BLOCK);
            vecpush(body->block, ret);
            setspan(parser, body, arrow);
        } else {
            zlog(parser->state, peek(parser), Z2024);
            return NULL;
        }
    } else if (check(parser, TOK_LBRACKET)) {
        body = tryParse(parser, parseBlock(parser));
        if (body) body->annotations = annotations;
    } else {
        zlog(parser->state, peek(parser), Z2015);
    }

    if (!body) {
        zlog(parser->state, peek(parser), Z2025);
    }

    ZType *func                 = maketype(parser->allocator, Z_TYPE_FUNCTION);
    func->func.ret              = ret;
    func->func.args             = NULL;
    func->func.capabilities     = NULL;

    for (usize i = 0; i < veclen(args); i++)
        vecpush(func->func.args, args[i]->field.type);
    for (usize i = 0; i < veclen(capabilities); i++)
        vecpush(func->func.capabilities, capabilities[i]->field.type);

    char *mangled = strcmp(name->str, "main") == 0 ? name->str :
        mangler((const char *[]){
            tokfilename(parser->state, name),
            name->str,
            NULL
        });

    ZNode *node                 = makenode(parser->allocator, NODE_FUNC);
    node->tok                   = name;
    node->resolved              = func;
    node->funcDef.name          = name;
    node->funcDef.args          = args;
    node->funcDef.body          = body;
    node->funcDef.pub           = public;
    node->funcDef.generics      = generics;
    node->funcDef.base          = NULL;
    node->funcDef.receiver      = receiver;
    node->funcDef.mangled       = mangled;
    node->funcDef.annotations   = annotations;
    node->funcDef.capabilities  = capabilities;

    return setspan(parser, node, start);
}

ZNode *parseFacet(ZParser *parser, ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);

    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_FACET);


    ZType **generics = NULL;

    if (match(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser);
        expect(parser, TOK_RSBRACKET);
    }

    expect(parser, TOK_LBRACKET);

    ZNode *facet                = makenode(parser->allocator, NODE_FACET);
    facet->facet.pub            = public;
    facet->facet.name           = start;
    facet->facet.funcs          = NULL;
    facet->facet.generics       = generics;
    facet->annotations          = annotations;

    do {
        if (!check(parser, TOK_IDENT)) break;
        ZToken *name = consume(parser);

        if (!match(parser, TOK_COLON)) break;

        ZType *func = parseFuncType(parser);

        ZNode *field = makenode(parser->allocator, NODE_FIELD);

        field->field.identifier = name;
        field->field.type       = func;
        field->resolved         = func;
        setspan(parser, field, name);

        vecpush(facet->facet.funcs, field);
    } while (!check(parser, TOK_RBRACKET));

    expect(parser, TOK_RBRACKET);

    if (veclen(facet->facet.funcs) == 0) {
        zlog(parser->state, start, Z203A);
    }

    ZType *type         = maketype(parser->allocator, Z_TYPE_FACET);
    type->tok           = start;
    type->facet.name    = start;
    type->facet.funcs   = facet->facet.funcs;
    facet->resolved     = type;
    facet->tok          = start;

    return setspan(parser, facet, start);
}

static ZNode *parseConst(ZParser *parser, bool public) {
    ZToken *start = peek(parser);

    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);

    ZNode *expr = parseExpr(parser);

    ZVarDestructPattern *pattern = makeDestructIdent(parser->allocator, start);
    ZNode *var = setspan(parser, makenodevar(parser->allocator, pattern, NULL, expr), start);
    var->varDecl.pub = public;
    return var;
}

static ZNode *getModuleByName(
    ZParser *parser, char *module, ZToken *tok, bool public) {
    char *from = tok ? (char *)tokfilename(parser->state, tok) : (char *)zfile_name(parser->file);
    char *filename = resolveImport(parser->allocator, parser->state, from, module);
    if (!filename) {
        zlog(parser->state, tok, Z203C, module);
        return NULL;
    }

    bool created = false;
    ZFile *f                    = ft_insert(
        parser->allocator, parser->state->fileTable, filename, &created
    );

    if (!f) {
        zlog(parser->state, tok, Z1009, filename, strerror(errno));
        return NULL;
    }

    if (created) parseFile(parser->allocator, parser->state, f);

    ZNode *node         = makenode(parser->allocator, NODE_MODULE);
    node->module.file   = f;
    node->module.pub    = public;
    return node;
}

ZNode *parseImport(ZParser *parser, bool public) {
    ZToken *start   = peek(parser);
    ZToken *alias   = NULL;
    if (check(parser, TOK_IDENT) && checkAhead(parser, TOK_DOUBLE_COLON, 1)) {
        alias = consume(parser);
        consume(parser);
    }
    expect(parser, TOK_IMPORT);
    if (!check(parser, TOK_STR_LIT))
        return NULL;

    ZToken *name    = consume(parser);
    ZNode *node     = getModuleByName(parser, stoken(name), name, public);

    if (node) node->module.name = alias;
    return setspan(parser, node, start);
}

static ZNode *parseDecl(ZParser *parser, ZAnnotation **a, bool p) {
    ZToken *start   = peek(parser);
    ZNode *res      = NULL;
    if (checkMask(parser, TOK_TYPES_MASK)) {
        res = parseImpl(parser, a, p);
        goto end;
    }

    if (!check(parser, TOK_IDENT)) return NULL;
    if (checkAhead(parser, TOK_COLON, 1)    ||
        checkAhead(parser, TOK_ASSIGN, 1)   ||
        checkAhead(parser, TOK_COMMA, 1)) {
        res = parseVarDef(parser);
        goto end;
    }

    if (!checkAhead(parser, TOK_DOUBLE_COLON, 1))
        return NULL;

    ZToken *kind = peekAhead(parser, 2);
    if (!kind) return NULL;
    switch (kind->type) {
    case TOK_STRUCT:    res = parseStructDecl(parser, a, p);    break;
    case TOK_ENUM:      res = parseEnumDecl(parser, a, p);      break;
    case TOK_FOREIGN:   res = parseForeign(parser, a, p);       break;
    case TOK_FACET:     res = parseFacet(parser, a, p);         break;
    case TOK_IMPL:      res = parseImpl(parser, a, p);          break;
    case TOK_FN:        res = parseFuncDecl(parser, a, p);      break;
    case TOK_TYPEDEF:   res = parseTypedef(parser, a, p);       break;
    case TOK_IMPORT:    res = parseImport(parser, p);           break;
    default:            res = parseConst(parser, p);            break;
    }

end:
    return setspan(parser, res, start);
}

ZNode *parseTypedef(ZParser *parser, ZAnnotation **annotations, bool public) {
    ZToken *alias = peek(parser);
    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_TYPEDEF);

    ZType *type = tryParse(parser, parseType(parser));

    ensure(type, "Invalid type");

    ZNode *node                 = makenode(parser->allocator, NODE_TYPEDEF);
    node->typeDef.alias         = alias;
    node->typeDef.type          = type;
    node->typeDef.pub           = public;
    node->annotations           = annotations;
    node->tok                   = alias;
    return setspan(parser, node, alias);
}

ZNode *parseForeignBlock(ZParser *parser, ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);
    ZToken *lib = start;

    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_FOREIGN);

    ZType **capabilities = NULL;
    if (match(parser, TOK_WITH)) {
        do {
            ZType *capability = parseType(parser);
            if (!capability) break;
            vecpush(capabilities, capability);
        } while (!check(parser, TOK_LSBRACKET) && match(parser, TOK_COMMA));
    }

    expect(parser, TOK_LBRACKET);

    ZType *type             = NULL;
    ZNode *namespace        = makenode(parser->allocator, NODE_NAMESPACE);
    namespace->tok          = lib;
    namespace->block        = NULL;
    namespace->pub          = public;
    namespace->annotations  = annotations;
    namespace->capabilities = capabilities;
    ZNode *node             = NULL;


    ZToken *name = NULL;
    while (!check(parser, TOK_RBRACKET)) {
        public = match(parser, TOK_PUB);
        name = peek(parser);

        expect(parser, TOK_IDENT);
        expect(parser, TOK_COLON);

        type = parseType(parser);
        if (!type) {
            zlog(parser->state, peek(parser), Z2031);
            return NULL;
        }

        if (type->kind == Z_TYPE_FUNCTION)
            for (usize i = 0; i < veclen(capabilities); i++)
                vecpush(type->func.capabilities, capabilities[i]);

        node                    = makenode(parser->allocator, NODE_FOREIGN);
        node->foreignDecl.name  = name;
        node->foreignDecl.pub   = public;
        node->tok               = name;
        node->resolved          = type;
        setspan(parser, node, name);

        vecpush(namespace->block, node);
    }
    expect(parser, TOK_RBRACKET);

    return setspan(parser, namespace, start);
}

ZNode *parseForeignInlineDecl(ZParser *parser, ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);

    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_FOREIGN);

    ZType *type = parseType(parser);
    guard(type);

    ZNode *node = makenode(parser->allocator, NODE_FOREIGN);
    node->foreignDecl.name          = start;
    node->foreignDecl.pub           = public;
    node->annotations               = annotations;
    node->tok                       = start;
    node->resolved                  = type;
    return setspan(parser, node, start);
}

ZNode *parseForeign(ZParser *parser, ZAnnotation **annotations, bool public) {
    (void)public;
    if (!checkAhead(parser, TOK_IDENT,          0)) return NULL;
    if (!checkAhead(parser, TOK_DOUBLE_COLON,   1)) return NULL;
    if (!checkAhead(parser, TOK_FOREIGN,        2)) return NULL;
    /* '{' starts a block; 'with' introduces the block's capability list. */
    if (checkAhead(parser, TOK_LBRACKET,        3) ||
        checkAhead(parser, TOK_WITH,            3))
        return parseForeignBlock(parser, annotations, public);
    return parseForeignInlineDecl(parser, annotations, public);
}

ZNode *parseField(ZParser *parser) {
    ZAnnotation **annotations =
        check(parser, TOK_HASHTAG) ?
        parseAnnotations(parser) :
        NULL;

    ensure(check(parser, TOK_IDENT), "Expected an identifier");
    ZToken *ident = consume(parser);

    expect(parser, TOK_COLON);

    ZType *type = tryParse(parser, parseType(parser));
    guard(type);

    ZNode *defaultValue = match(parser, TOK_EQ) ? parseExpr(parser) : NULL;

    ZNode *node             = makenode(parser->allocator, NODE_FIELD);
    node->field.type        = type;
    node->field.identifier  = ident;
    node->field.defaultValue= defaultValue;

    node->resolved          = type;
    node->tok               = ident;
    node->annotations       = annotations;
    return setspan(parser, node, ident);
}


static ZNode *parseFieldOptName(ZParser *parser) {
    ZToken *start = peek(parser);
    ZToken *ident = NULL;
    if (check(parser, TOK_IDENT) && checkAhead(parser, TOK_COLON, 1)) {
        ident = consume(parser);
        expect(parser, TOK_COLON);
    } else {
        ZToken *tok = peek(parser);
        if (!tok) return NULL;
        u32 file_id = zfile_id(parser->file);
        ident = makeident(parser->allocator, "_", file_id, 0);
    }

    ZType *type = tryParse(parser, parseType(parser));
    guard(type);

    ZNode *defaultValue = match(parser, TOK_EQ) ? parseExpr(parser) : NULL;
    ZNode *node             = makenode(parser->allocator, NODE_FIELD);
    node->field.type        = type;
    node->field.identifier  = ident;
    node->field.defaultValue= defaultValue;
    node->resolved          = type;
    node->tok               = ident;
    return setspan(parser, node, start);
}


ZNode *parseEnumDecl(ZParser *parser,
    ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);

    bool savedDeclAsType        = parser->declAsType;
    parser->declAsType          = true;
    ZType *type                 = parseAnonEnum(parser, annotations);
    parser->declAsType          = savedDeclAsType;
    guard(type);

    /* parseAnonEnum names the type after the 'enum' keyword; a named
     * declaration must carry the declared identifier instead. */
    type->enm.name              = start;
    type->tok                   = start;

    ZNode *node                 = makenode(parser->allocator, NODE_ENUM);
    node->enumDef.name          = start;
    node->enumDef.pub           = public;
    node->enumDef.fields        = type->enm.fields;
    node->annotations           = annotations;
    node->tok                   = node->enumDef.name;
    node->resolved          = type;

    return setspan(parser, node, start);
}


ZNode *parseStructDecl(ZParser *parser,
    ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);

    bool savedDeclAsType        = parser->declAsType;
    parser->declAsType          = true;
    ZType *type                 = parseAnonStruct(parser, annotations);
    parser->declAsType          = savedDeclAsType;
    guard(type);

    /* parseAnonStruct names the type after the 'struct' keyword; a named
     * declaration must carry the declared identifier instead. */
    type->strct.name            = start;
    type->tok                   = start;

    ZNode *node                 = makenode(parser->allocator, NODE_STRUCT);
    node->tok                   = start;
    node->structDef.fields      = type->strct.fields;
    node->structDef.generics    = type->strct.generics;
    node->structDef.ident       = start;
    node->structDef.pub         = public;
    node->annotations           = annotations;

    return setspan(parser, node, start);
}


static ZAnnotation *parseAnnotation(ZParser *parser) {
    ZToken *curr        = peek(parser);
    ZAnnotation *arg    = zalloc(parser->allocator, ZAnnotation);
    arg->tok            = curr;

    if (tokmask(curr, TOK_LITERAL | TOK_OVERLOADABLE)) {
        arg->kind       = Z_ANN_LIT;
        arg->literal    = consume(parser);
        return arg;
    }

    if (curr->type != TOK_IDENT) return NULL;

    curr = consume(parser);

    if (match(parser, TOK_EQ)) {
        ZAnnotation *value = parseAnnotation(parser);
        if (!value) return NULL;
        arg->kind           = Z_ANN_ASSIGN;
        arg->assign.name    = curr;
        arg->assign.value   = value;
        return arg;
    }

    if (match(parser, TOK_LPAREN)) {
        arg->kind   = Z_ANN_NESTED;
        arg->nested = NULL;
        if (match(parser, TOK_RPAREN)) return arg;

        do {
            ZAnnotation *val = parseAnnotation(parser);
            vecpush(arg->nested, val);
        } while(!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));

        if (!match(parser, TOK_RPAREN)) {
            return NULL;
        }
        return arg;
    }

    arg->kind       = Z_ANN_IDENT;
    arg->ident.tok  = curr;
    arg->ident.li   = Z_LANG_NONE;
    return arg;
}


ZAnnotation **parseAnnotations(ZParser *parser) {
    if (!match(parser, TOK_HASHTAG)) return NULL;

    bool group = match(parser, TOK_LSBRACKET);

    ZAnnotation **annotations   = NULL;
    ZAnnotation *annotation     = NULL;

    do {
        annotation = parseAnnotation(parser);
        if (!annotation) {
            zlog(parser->state, peek(parser), Z201D);
            break;
        }
        vecpush(annotations, annotation);
    } while (!check(parser, TOK_RSBRACKET) && match(parser, group ? TOK_COMMA : TOK_HASHTAG));

    if (group) expect(parser, TOK_RSBRACKET);

    return annotations;
}


ZNode **parseCapabilityList(ZParser *parser) {
    if (!match(parser, TOK_WITH)) return NULL;

    ZNode **capabilities = NULL;
    do {
        ZNode *capability = parseFieldOptName(parser);
        if (!capability) break;
        vecpush(capabilities, capability);
    } while (
            !check(parser, TOK_ARROW)       &&
            !check(parser, TOK_LBRACKET)    &&
             match(parser, TOK_COMMA)       );
    return capabilities;
}

/* Parses a module by default and insert it at the first element in every module. */
ZNode *injectPrelude(ZParser *parser) {
    /* Root of the std module: its span points into another file. */
    return setspan(parser, getModuleByName(parser, "std", NULL, false), NULL);
}

static ZNode *parseModule(ZParser *parser) {
    ZToken *start = peek(parser);
    ZNode *root = makenode(parser->allocator, NODE_MODULE);

    root->module.root       = NULL;
    root->module.file       = parser->file;

    if (!parser->state->cli.noInject) {
        ZNode *prelude = injectPrelude(parser);
        vecpush(root->module.root, prelude);
    }

    while (canPeek(parser)) {
        ZNode *child = parse(parser);
        if (!child) break;

        // Block of functions
        if (child->type == NODE_BLOCK) {
            for (usize i = 0; i < veclen(child->block); i++) {
                vecpush(root->module.root, child->block[i]);
            }
        } else {
            vecpush(root->module.root, child);
        }
    }
    return setspan(parser, root, start);
}

ZNode *parse(ZParser *parser) {
    guard(canPeek(parser));
    ZAnnotation **annotations   = NULL;

// #if Z_LSP
//     ZToken *comment = NULL;
//     if (check(parser, TOK_COMMENT)) {
//         comment = consume(parser);
//     }
// #endif

    annotations = parseAnnotations(parser);
    bool public = match(parser, TOK_PUB);

    ZNode *res = NULL;
    if (check(parser, TOK_IMPORT)) {
        res = parseImport(parser, public);
    } else {
        res = parseDecl(parser, annotations, public);
    }

    if (!res) {
        zlog(parser->state, peek(parser), Z203B, stoken(peek(parser)));
        consume(parser);
    }

// #if Z_LSP
//     if (res) res->comment = comment;
// #endif
    return res;
}

static void pool_worker(ZTaskCtx *task_ctx) {
    ZParser *parser     = (ZParser *)task_ctx->ctx;
    parser->allocator   = task_ctx->allocator;
    useAllocator(task_ctx->allocator);
    ZLog ***prevSink    = setLogSink(&parser->logs);

    ZToken **tokens     = ztokenize(
        parser->allocator, parser->state, parser->file
    );
    zfile_set_tokens(parser->file, tokens);
#if Z_LSP
    ZToken **code = NULL;
    for (usize i = 0; i < veclen(tokens); i++)
        if (tokens[i]->type != TOK_COMMENT) vecpush(code, tokens[i]);
    tokens = code;
#endif
    parser->source = maketokstream(parser->allocator, tokens, NULL);

    ZNode *root = NULL;
    if (tokens) {
        root = parseModule(parser);
        if (canPeek(parser))
            zlog(parser->state, peek(parser), Z203B, stoken(peek(parser)));
    } else {
        root = makenode(parser->allocator, NODE_MODULE);
        root->module.file = parser->file;
    }

    zfile_set_root(parser->file, root);

    setLogSink(prevSink);
    flushLogs(parser->state, parser->logs);
}

void parseFile(Allocator *a, ZState *state, ZFile *file) {
    ZParser *parser     = makeparser(a, state, file);
    pool_submit(state->pool, pool_worker, parser);
}

void zparse(ZState *state, ZFile *file) {
    state->currentPhase = Z_PHASE_SYNTAX;
    parseFile(state->allocator, state, file);
    pool_wait(state->pool);
}
