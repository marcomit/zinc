#ifndef ZMIR_BUILDER
#define ZMIR_BUILDER

#include "zinc.h"

typedef struct ZMirBuilder ZMirBuilder;

typedef struct ZMirInstruction ZMirInstruction;
typedef ZMirInstruction *ZMirOpaque;

typedef enum {
    #define INSTR(builder, category, emitted) ZMir##builder,
    #define ZMIR_INSTRUCTIONS

    #include "zllvm.def"

    #undef ZMIR_INSTRUCTIONS
    #undef INSTR
} ZMirInstructionType;

typedef enum {
    #define ICMP(builder, category) ZMirInt##builder,
    #define ZMIR_ICMP

    #include "zllvm.def"

    #undef ZMIR_ICMP
    #undef ICMP
} ZMirIntPredicate;

typedef enum {
    #define FCMP(builder, category) ZMirFloat##builder,
    #define ZMIR_FCMP

    #include "zllvm.def"

    #undef ZMIR_FCMP
    #undef FCMP
} ZMirFloatPredicate;

typedef enum {
    ZMirI8,
    // ZMirU8,
    ZMirI16,
    // ZMirU16,
    ZMirI32,
    // ZMirU32,
    ZMirI64,
    // ZMirU64,
} ZMirInt;

struct ZMirInstruction {
    ZMirInstructionType type;
    union {
        struct {
            ZMirIntPredicate predicate;
            ZMirOpaque *left;
            ZMirOpaque *right;
        } icmp;

        struct {
            ZMirFloatPredicate predicate;
            ZMirOpaque *left;
            ZMirOpaque *right;
        } fcmp;

        struct {
            ZMirInstruction *left;
            ZMirInstruction *right;
        } binary;

        struct {
            ZMirInt inttype;
            u64 value;
        } constint;

        struct {
            u32 align;

            union {
                ZType *alloca;

                struct {
                    ZMirInstruction *val;
                    ZMirInstruction *ptr;
                } store;

                struct {
                    ZType           *type;
                    ZMirInstruction *value;
                } load;

                struct {
                    ZMirInstruction *ptr;
                    ZMirInstruction *val;
                    ZMirInstruction *len;
                } mmset;
            };
        };

    };
};

typedef struct ZMirCursor {
    ZMirOpaque      *currentBlock;
    ZMirInstruction **instructions;
} ZMirCursor;

struct ZMirBuilder {
    ZState      *state;
    ZMirCursor  *cursor;
    ZMirOpaque  **instructions;
};

#endif
