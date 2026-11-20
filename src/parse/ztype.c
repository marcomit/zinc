#include "zparse.h"

/* Parses a list of types delimited by left and right.
 * It assumes that the elements are separated by a TOK_COMMA.
 *
 * Example: [u8, char, [u8]]
 * You can parse this by using this function
 * by calling with TOK_LSBRACKET and TOK_RSBRACKET.
 * */
ZType **parseTypeList(ZParser *parser, ZTokenType left, ZTokenType right) {
    expect(parser, left);
    ZType **args = NULL;
    do {
        ZType *curr = tryParse(parser, parseType(parser));
        if (!curr) {
            zlog(parser->state, peek(parser), Z200B);
            return NULL;
        }
        vecpush(args, curr);
        if (!match(parser, TOK_COMMA)) break;
        if (check(parser, right)) break;
    } while (true);
    if (!match(parser, right)) {
        zlog(parser->state, peek(parser), Z200A, tokname(right), stoken(peek(parser)));
        return NULL;
    }
    return args;
}

ZType *parseTypeArray(ZParser *parser) {
    expect(parser, TOK_LSBRACKET);

    usize size      = 0;
    bool dynamic    = false;
    if (check(parser, TOK_INT_LIT)) {
        size    = consume(parser)->integer;
    } else if (match(parser, TOK_DOUBLE_DOT)) {
        dynamic = true;
    }

    expect(parser, TOK_RSBRACKET);

    ZType *type = parseBaseType(parser);
    ensure(type, "Expected a type after [] brackets");

    ZType *arr = maketype(parser->allocator, Z_TYPE_ARRAY);
    arr->array.base     = type;
    arr->array.size     = size;
    arr->array.dynamic  = dynamic;
    return arr;
}

/* A tuple must have at least 2 types */
static ZType *parseTypeTuple(ZParser *parser) {
    ZType **types = parseTypeList(parser, TOK_LPAREN, TOK_RPAREN);

    if (!types) {
        zlog(parser->state, peek(parser), Z200C);
        return NULL;
    } else if (veclen(types) == 1) {
        return types[0];
    } else if (veclen(types) < 2) {
        zlog(parser->state, peek(parser), Z200D, veclen(types));
        return NULL;
    }

    ZType *type = maketype(parser->allocator, Z_TYPE_TUPLE);
    type->tuple = types;
    return type;
}

static ZType *parseTypeOptional(ZParser *parser) {
    ZToken *tok     = consume(parser);
    ZType *base     = parseType(parser);
    if (!base) return NULL;
    ZType *opt      = maketype(parser->allocator, Z_TYPE_OPTIONAL);
    opt->optional   = base;
    opt->tok        = tok;
    return opt;
}

static ZType *parseIdentType(ZParser *parser) {
    ZType *base = NULL, *prev = NULL;
    while (check(parser, TOK_IDENT)) {
        base                    = maketype(parser->allocator, Z_TYPE_PRIMITIVE);
        base->tok               = peek(parser);
        ZToken *t               = consume(parser);
        base->primitive.token   = t;
        base->primitive.base    = prev;

        if (!prev && parser->implBase && strcmp(t->str, "Self") == 0)
            base->primitive.token = parser->implBase->primitive.token;

        if (!match(parser, TOK_DOT)) break;
        prev = base;
    }
    return base;
}

ZType *parseFuncType(ZParser *parser) {
    ZToken *start   = peek(parser);

    expect(parser, TOK_FN);

    ZType **generics = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser);
    }

    expect(parser, TOK_LPAREN);

    ZType **args    = NULL;
    ZType *arg      = NULL;
    bool variadic   = false;

    do {
        if (match(parser, TOK_DOUBLE_DOT)) {
            variadic = true;
            // break;
        } else {
            arg = tryParse(parser, parseType(parser));
            if (arg) vecpush(args, arg);
        }
    } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));

    expect(parser, TOK_RPAREN);

    ZType *ret              = tryParse(parser, parseType(parser));
    guard(ret);
    ZNode **capabilities    = parseCapabilityList(parser);
    ZType *func             = maketype(parser->allocator, Z_TYPE_FUNCTION);
    func->func.ret          = ret;
    func->func.args         = args;
    func->func.generics     = generics;
    func->func.variadic     = variadic;
    func->func.capabilities = NULL;
    func->tok               = start;

    for (usize i = 0; i < veclen(capabilities); i++) {
        vecpush(func->func.capabilities, capabilities[i]->resolved);
    }
    return func;
}

static ZNode *parseStructField(ZParser *parser) {
    ZAnnotation **annotations = NULL;
    if (check(parser, TOK_HASHTAG))
        annotations = parseAnnotations(parser);

    ZToken *start = peek(parser);
    ZNode *result = NULL;
    if (match(parser, TOK_DOUBLE_DOT)) {
        ZType *type = parseType(parser);
        if (!type) {
            zlog(parser->state, peek(parser), Z2017);
            return NULL;
        }
        ZNode *node             = makenode(parser->allocator, NODE_EMBED_FIELD);
        node->resolved          = type;
        node->field.type        = type;
        node->field.identifier  = NULL;
        result = setspan(parser, node, start);
    } else {
        result = parseField(parser);
    }

    if (result) {
        result->annotations = annotations;

        if (match(parser, TOK_EQ))
            result->field.defaultValue = parseExpr(parser);
    }

    return result;
}

ZType *parseAnonStruct(ZParser *parser, ZAnnotation **annotations) {
    ZToken *start = peek(parser);
    expect(parser, TOK_STRUCT);

    ZType **generics = NULL;

    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser);
        if (!generics) {
            zlog(parser->state, peek(parser), Z201B);
        }
    }

    ZToken **regions = NULL;
    // if (check(parser, TOK_AT)) {
    //     regions = parseRegions(parser);
    // }

    ZNode **fields = parseGenericList(parser,
            TOK_LBRACKET, TOK_RBRACKET,
            parseStructField, false);

    ZType *type                 = maketype(parser->allocator, Z_TYPE_STRUCT);
    type->annotations           = annotations;
    type->strct.name            = start;
    type->strct.generics        = generics;
    type->strct.fields          = fields;
    type->strct.regions         = regions;
    return type;
}



static ZType *parseAtom(ZParser *parser) {
    ZToken *start = peek(parser);
    if (!start) return NULL;

    switch (start->type) {
    case TOK_CAP:
    case TOK_STAR: {
        consume(parser);
        ZType *type = maketype(parser->allocator, Z_TYPE_POINTER);
        type->tok   = start;
        type->base  = parseAtom(parser);
        return type;
    }
    // case TOK_TYPEDEF:
    //     consume(parser);
    //     return consts(parser->state, typeType);
    case TOK_FN:
        return parseFuncType(parser);
    case TOK_LSBRACKET:
        return parseTypeArray(parser);
    case TOK_LPAREN:
        return parseTypeTuple(parser);
    case TOK_OPT:
        return parseTypeOptional(parser);
    case TOK_STRUCT:
        if (!parser->declAsType) {
            zlog(parser->state, peek(parser), Z200E);
            return NULL;
        }
        return parseAnonStruct(parser, NULL);
    case TOK_ENUM:
        if (!parser->declAsType) {
            zlog(parser->state, peek(parser), Z200F);
            return NULL;
        }
        return parseAnonEnum(parser, NULL);
    default:
        if (checkMask(parser, TOK_TYPES_MASK)) {
            ZType *base = maketype(parser->allocator, Z_TYPE_PRIMITIVE);
            base->primitive.token   = consume(parser);
            base->primitive.base    = NULL;
            return base;
        }

        if (!check(parser, TOK_IDENT)) return NULL;
        return parseIdentType(parser);
    }
}

ZType *parseBaseType(ZParser *parser) {
    ZToken *start   = peek(parser);

    ZType *base     = tryParse(parser, parseAtom(parser));
    ensure(base, "Failed to parse atom type");

    if (check(parser, TOK_LSBRACKET)) {
        // Generic type instantiation like List[int] or Map[str, int]
        ZType **generics = tryParse(parser, parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET));
        base->primitive.generics = generics;
    } else if (check(parser, TOK_ESCL)) {
        ZToken *tok = consume(parser);
        ZType *error = parseBaseType(parser);
        if (!error) return NULL;

        ZType *result           = maketype(parser->allocator, Z_TYPE_RESULT);
        result->result.success  = base;
        result->result.error    = error;
        result->tok             = tok;
        base = result;
    }

    if (base) base->tok = start;
    return base;
}

static ZType *parseSumType(ZParser *parser) {
#if Z_LSP
    ZToken *start   = peek(parser);
#endif
    ZType *curr     = parseBaseType(parser);
    ZType **types   = NULL;
    vecpush(types, curr);
    while (match(parser, TOK_BITOR)) {
        curr = tryParse(parser, parseBaseType(parser));

        if (!curr) break;
        vecpush(types, curr);
    }

    ZType *sumType = NULL;
    if (veclen(types) == 1) {
        sumType = types[0];
    } else {
        sumType      = maketype(parser->allocator, Z_TYPE_SUM);
        typesSort(types);
        sumType->sumType    = types;
    }

#if Z_LSP
    if (sumType) {
        sumType->lsp_start  = start;
        sumType->lsp_end    = peek(parser);
    }
#endif

    return sumType;
}

ZType *parseType(ZParser *parser) {
    ZType *type = parseSumType(parser);
    if (!type) return NULL;

    ZToken *region = NULL;
    if (match(parser, TOK_AT)) {
        if (!check(parser, TOK_IDENT)) {
            zlog(parser->state, peek(parser), Z2015);
        } else region = consume(parser);
    }

    // if (region) {
    //     type->region = makeregion(parser->allocator, stoken(region), 0);
    // } else {
    //     type->region = consts(parser->state, rawRegion);
    // }

    return type;
}


static ZToken **parseRegions(ZParser *parser) {
    ZToken **list = NULL;
    if (!match(parser, TOK_AT)) return NULL;

    if (check(parser, TOK_IDENT)) {
        vecpush(list, consume(parser));
    } else if (match(parser, TOK_LPAREN)) {
        do {
            if (!check(parser, TOK_IDENT)) break;
            vecpush(list, consume(parser));

        } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));

        if (!match(parser, TOK_RPAREN)) {
            zlog(parser->state, peek(parser), Z2015);
        } else if (veclen(list) == 0) {
            zlog(parser->state, peek(parser), Z2015);
        }
    } else {
        zlog(parser->state, peek(parser), Z2015);
    }

    return list;
}


static ZNode *parseEnumVariantField(ZParser *parser) {
    ZNode *field = makenode(parser->allocator, NODE_FIELD);
    field->tok = peek(parser);
    if (check(parser, TOK_IDENT) && checkAhead(parser, TOK_COLON, 1)) {
        field->field.identifier = consume(parser);
        consume(parser);
    }

    ZType *type = tryParse(parser, parseType(parser));
    if (!type) return NULL;

    field->resolved     = type;
    field->field.type   = type;
    return setspan(parser, field, field->tok);
}


static ZNode *parseEnumField(ZParser *parser) {
    ZAnnotation **annotations = NULL;
    if (check(parser, TOK_HASHTAG)) {
        annotations = parseAnnotations(parser);
        if (!annotations) {
            zlog(parser->state, peek(parser), Z2018);
        }
    }
    if (!check(parser, TOK_IDENT)) {
        zlog(parser->state, peek(parser), Z2018);
        return NULL;
    }
    ZToken *name = consume(parser);

    ZNode **fields = NULL;


    ZNode *node                 = makenode(parser->allocator, NODE_ENUM_FIELD);
    node->annotations           = annotations;
    node->enumField.name        = name;
    node->tok                   = name;

    ZType *enm                  = maketype(parser->allocator, Z_TYPE_STRUCT);
    enm->strct.name             = name;
    enm->strct.fields           = NULL;

    /* Prepend the flag type. */
    ZNode *field                = makenode(parser->allocator, NODE_FIELD);
    field->field.identifier     = NULL;

    ZType *flag                 = maketype(parser->allocator, Z_TYPE_PRIMITIVE);
    flag->primitive.token       = maketoken(parser->allocator, TOK_U8, NULL, NULL);
    field->field.type           = flag;

    vecpush(enm->strct.fields, field);

    if (check(parser, TOK_LPAREN)) {
        fields = parseGenericList(
            parser, TOK_LPAREN, TOK_RPAREN, parseEnumVariantField, true
        );
        if (!fields) {
            zlog(parser->state, peek(parser), Z2019);
            return NULL;
        }

        for (usize i = 0; i < veclen(fields); i++) {
            vecpush(enm->strct.fields, fields[i]);
        }
    }

    node->resolved = enm;
    return setspan(parser, node, name);
}


ZType *parseAnonEnum(ZParser *parser, ZAnnotation **annotations) {
    (void)annotations;
    ZToken *start = peek(parser);
    expect(parser, TOK_ENUM);

    ZToken *integer = NULL;
    if (checkMask(parser, TOK_SIGNED | TOK_UNSIGNED)) {
        integer = consume(parser);
    }

    ZType **generics = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser);
    }

    ZToken **regions = NULL;
    if (check(parser, TOK_AT)) {
        regions = parseRegions(parser);
    }

    ZNode **fields = parseGenericList(parser,
            TOK_LBRACKET, TOK_RBRACKET,
            parseEnumField, false);

    ZType *type                 = maketype(parser->allocator, Z_TYPE_ENUM);
    type->enm.name              = start;
    type->enm.generics          = generics;
    type->enm.fields            = fields;
    type->tok                   = start;
    type->enm.integer           = integer;
    type->enm.regions           = regions;

    if (!fields || veclen(fields) < 2) zlog(parser->state, start, Z201A);

    return type;
}

/* K[V: Display[T] + Drop]
 *
 *
 * generic_arg =
 *              identifier |
 *              identifier ':' generic_decl { '+', generic_decl }
 *
 * generic_decl = ident '[' generic_arg, { ',', generic_arg } ']'
 * */
static ZType *parseGenericDecl(ZParser *);

static ZType *parseGenericArgument(ZParser *parser) {
    ensure(check(parser, TOK_IDENT), "Expected an identifier");

    ZType *generic = maketype(parser->allocator, Z_TYPE_GENERIC);
    generic->generic.name = consume(parser);

    generic->generic.extensions = NULL;

    if (match(parser, TOK_COLON)) {
        ZType *arg = parseGenericDecl(parser);
        if (!arg) {
            zlog(parser->state, peek(parser), Z2015);
            return NULL;
        }

        vecpush(generic->generic.extensions, arg);

        if (!match(parser, TOK_PLUS)) return generic;

        while (true) {
            arg = parseGenericDecl(parser);
            if (!arg) break;
            vecpush(generic->generic.extensions, arg);

            if (!match(parser, TOK_PLUS)) break;
        }
    }

    return generic;
}


static ZType *parseGenericDecl(ZParser *parser) {
    ensure(check(parser, TOK_IDENT), "Expected an identifier");

    ZType *generic              = maketype(parser->allocator, Z_TYPE_GENERIC);
    generic->generic.name       = consume(parser);
    generic->generic.extensions = NULL;

    if (match(parser, TOK_LBRACKET)) {
        ZType *argument = parseGenericArgument(parser);

        if (!argument) {
            zlog(parser->state, peek(parser), Z2020);
        }
        vecpush(generic->generic.extensions, argument);

        while (true) {
            if (!match(parser, TOK_COMMA)) break;
            if (!check(parser, TOK_IDENT)) break;

            argument = parseGenericArgument(parser);
            if (!argument) break;
            vecpush(generic->generic.extensions, argument);
        }
        expect(parser, TOK_RBRACKET);
    }
    return generic;
}

/*
 *  [K, V]
 *  [K: Display + Drop]
 * */
ZType **parseGenericsDecl(ZParser *parser) {
    expect(parser, TOK_LSBRACKET);
    ZType **generics    = NULL;
    ZType *generic      = NULL;
    do {
        if (!check(parser, TOK_IDENT)) break;

        ZToken *ident               = consume(parser);
        generic                     = maketype(parser->allocator, Z_TYPE_GENERIC);
        generic->generic.name       = ident;
        generic->generic.extensions = NULL;

        if (match(parser, TOK_COLON)) {
            if (!check(parser, TOK_IDENT)) {
                zlog(parser->state, peek(parser), Z2021);
                break;
            }
            ZType *extension = maketype(parser->allocator, Z_TYPE_PRIMITIVE);
            extension->primitive.token = consume(parser);
            vecpush(generic->generic.extensions, extension);

            while (match(parser, TOK_PLUS)) {
                if (!check(parser, TOK_IDENT)) {
                    zlog(parser->state, peek(parser), Z2021);
                    break;
                }
                extension = maketype(parser->allocator, Z_TYPE_PRIMITIVE);
                extension->primitive.token = consume(parser);
                vecpush(generic->generic.extensions, extension);
            }
        }

        vecpush(generics, generic);
    } while (!check(parser, TOK_RSBRACKET) && match(parser, TOK_COMMA));

    expect(parser, TOK_RSBRACKET);

    return generics;
}
