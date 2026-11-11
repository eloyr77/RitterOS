/*
 * The interpreter's instructions (27o): OP(name, operand bytes, stack effect).
 * A stack effect of 99 is worked out by the compiler from the operands (calls,
 * arrays). Operands follow the opcode: u16 registers, hops and counts, u32
 * constants and atoms (as indexes into the function's constants), i32 jumps
 * relative to the end of the instruction.
 */
OP(NOP, 0, 0)
OP(UNDEF, 0, 1)
OP(NULL, 0, 1)
OP(TRUE, 0, 1)
OP(FALSE, 0, 1)
OP(EMPTY, 0, 1)
OP(INT, 4, 1)                   /* i32 */
OP(CONST, 4, 1)                 /* u32 constant */
OP(THIS_FRAME, 0, 1)            /* the frame's this (EMPTY in a derived constructor) */
OP(NEW_TARGET_FRAME, 0, 1)
OP(CALLEE, 0, 1)
OP(HOME_FRAME, 0, 1)            /* the callee's home object */
OP(GLOBAL_THIS, 0, 1)
OP(ARG, 2, 1)                   /* u16 index */
OP(REST, 2, 1)                  /* u16 first */
OP(ARGUMENTS, 2, 1)             /* u16 n mapped, then n u16 slots */
OP(GET_REG, 2, 1)
OP(SET_REG, 2, 0)
OP(PUT_REG, 2, -1)
OP(GET_REG_CHECK, 6, 1)         /* u16 reg, u32 name: ReferenceError if EMPTY */
OP(SET_REG_CHECK, 6, 0)
OP(GET_SCOPE, 4, 1)             /* u16 hops, u16 index */
OP(SET_SCOPE, 4, 0)
OP(PUT_SCOPE, 4, -1)
OP(GET_SCOPE_CHECK, 8, 1)       /* hops, index, u32 name */
OP(SET_SCOPE_CHECK, 8, 0)
OP(SET_THIS_ONCE_REG, 2, 0)     /* super(): this may be bound once */
OP(SET_THIS_ONCE_SCOPE, 4, 0)
OP(THROW_CONST, 4, 0)           /* u32 name: assignment to a constant */
OP(PUSH_SCOPE, 2, 0)            /* u16 slots */
OP(POP_SCOPE, 0, 0)
OP(COPY_SCOPE, 0, 0)
OP(GET_GLOBAL, 4, 1)            /* u32 name */
OP(GET_GLOBAL_TYPEOF, 4, 1)
OP(SET_GLOBAL, 5, 0)            /* u32 name, u8 strict */
OP(DEFINE_GLOBAL_VAR, 4, 0)
OP(DEFINE_GLOBAL_FUNC, 4, -1)
OP(DECLARE_GLOBAL_LEX, 5, 0)    /* u32 name, u8 const */
OP(INIT_GLOBAL_LEX, 4, -1)
OP(GET_PROP, 4, 0)              /* u32 name: obj -> value */
OP(GET_METHOD, 4, 1)            /* obj -> f obj */
OP(SET_PROP, 5, -1)             /* u32 name, u8 strict: obj value -> value */
OP(GET_ELEM, 0, -1)             /* obj key -> value */
OP(GET_METHOD_ELEM, 0, 0)       /* obj key -> f obj */
OP(SET_ELEM, 1, -2)             /* u8 strict: obj key value -> value */
OP(GET_SUPER, 4, -1)            /* home this -> value */
OP(SET_SUPER, 5, -2)            /* home this value -> value */
OP(GET_SUPER_ELEM, 0, -2)       /* home this key -> value */
OP(SET_SUPER_ELEM, 1, -3)       /* home this key value -> value */
OP(DELETE_PROP, 5, 0)           /* u32 name, u8 strict: obj -> bool */
OP(DELETE_ELEM, 1, -1)          /* obj key -> bool */
OP(DELETE_GLOBAL, 4, 1)         /* u32 name: -> bool */
OP(TO_PROPKEY, 0, 0)
OP(IN, 0, -1)                   /* key obj -> bool */
OP(INSTANCEOF, 0, -1)
OP(TYPEOF, 0, 0)
OP(OBJECT, 0, 1)
OP(ARRAY, 0, 1)                 /* an empty array */
OP(ARRAY_APPEND, 0, -1)         /* arr value -> arr */
OP(ARRAY_HOLE, 0, 0)
OP(ARRAY_SPREAD, 0, -1)         /* arr iterable -> arr */
OP(DEFINE_FIELD, 4, -1)         /* u32 name: obj value -> obj */
OP(DEFINE_ELEM, 0, -2)          /* obj key value -> obj */
OP(DEFINE_METHOD, 6, -1)        /* u32 name, u8 kind, u8 enumerable: obj f -> obj */
OP(DEFINE_METHOD_ELEM, 2, -2)   /* u8 kind, u8 enumerable: obj key f -> obj */
OP(SET_PROTO_LIT, 0, -1)        /* obj value -> obj */
OP(COPY_DATA_PROPS, 0, -1)      /* obj source -> obj */
OP(OBJECT_REST, 2, 99)          /* u16 n: source key1..keyn -> rest */
OP(CHECK_COERCIBLE, 0, 0)
OP(CLOSURE, 4, 1)               /* u32 constant (a funcdef) */
OP(CLASS, 5, 0)                 /* u32 name, u8 has heritage: [heritage] ctor -> ctor proto */
OP(SET_FIELDS, 0, -1)           /* ctor proto init -> ctor proto */
OP(CALL_STATIC_INIT, 0, -1)     /* ctor init -> ctor */
OP(SET_NAME_ELEM, 1, 0)         /* u8 prefix kind: key f -> key f */
OP(CALL, 2, 99)                 /* u16 argc: f this args -> result */
OP(CALL_SPREAD, 0, -2)          /* f this array -> result */
OP(NEW, 2, 99)                  /* u16 argc: f newtarget args -> result */
OP(NEW_SPREAD, 0, -2)
OP(SUPER_CALL, 2, 99)           /* u16 argc: func newtarget args -> this */
OP(SUPER_CALL_SPREAD, 0, -2)
OP(INIT_FIELDS, 0, -2)          /* this func -> */
OP(RETURN, 0, -1)
OP(RETURN_UNDEF, 0, 0)
OP(CHECK_DERIVED_RETURN, 0, -1) /* value this -> value */
OP(ADD, 0, -1)
OP(SUB, 0, -1)
OP(MUL, 0, -1)
OP(DIV, 0, -1)
OP(MOD, 0, -1)
OP(POW, 0, -1)
OP(BAND, 0, -1)
OP(BOR, 0, -1)
OP(BXOR, 0, -1)
OP(SHL, 0, -1)
OP(SAR, 0, -1)
OP(SHR, 0, -1)
OP(NEG, 0, 0)
OP(PLUS, 0, 0)
OP(NOT, 0, 0)
OP(BNOT, 0, 0)
OP(INC, 0, 0)
OP(DEC, 0, 0)
OP(TO_NUMERIC, 0, 0)
OP(EQ, 0, -1)
OP(NE, 0, -1)
OP(SEQ, 0, -1)
OP(SNE, 0, -1)
OP(LT, 0, -1)
OP(LE, 0, -1)
OP(GT, 0, -1)
OP(GE, 0, -1)
OP(JMP, 4, 0)
OP(JMP_FALSE, 4, -1)
OP(JMP_TRUE, 4, -1)
OP(JMP_FALSE_KEEP, 4, -1)       /* falsy: keep it and jump; else pop it (the effect: falling through) */
OP(JMP_TRUE_KEEP, 4, -1)
OP(JMP_NOT_NULLISH_KEEP, 4, -1) /* ?? */
OP(JMP_NULLISH_UNDEF, 4, 0)     /* ?.: nullish -> undefined and jump */
OP(JMP_NULLISH_CALLEE, 4, 0)    /* ?.(): f this with f nullish -> undefined and jump */
OP(JMP_NOT_UNDEF, 4, 0)         /* (defaults) not undefined: jump, keeping it */
OP(DUP, 0, 1)
OP(DUP2, 0, 2)
OP(POP, 0, -1)
OP(SWAP, 0, 0)
OP(ROT3R, 0, 0)                 /* a b c -> c a b */
OP(ROT4R, 0, 0)                 /* a b c d -> d a b c */
OP(THROW, 0, -1)
OP(THROW_ERROR, 5, 0)           /* u8 kind (0 Type, 1 Reference, 2 Syntax), u32 message */
OP(TRY, 4, 0)                   /* i32 handler */
OP(END_TRY, 0, 0)
OP(GET_ITER, 0, 2)              /* v -> it next at */
OP(ITER_NEXT, 4, 1)             /* i32 done: it next at -> it next at value (done: jump, no value) */
OP(ITER_STEP, 0, 1)             /* it next at -> it next at value-or-undefined (done remembered) */
OP(ITER_REST, 0, 1)             /* it next at -> it next at array */
OP(ITER_CLOSE, 0, -3)           /* it next at -> (return() unless done) */
OP(ITER_CLOSE_EXC, 0, -3)       /* it next at exc -> exc (return() and its errors ignored) */
OP(FOR_IN_START, 0, 0)          /* obj -> walk */
OP(FOR_IN_NEXT, 4, 1)           /* i32 done: walk -> walk key */
OP(TEMPLATE_OBJECT, 4, 1)       /* u32 constant: the site's strings (an array of [cooked..., raw...]) */
OP(REGEXP, 4, 1)                /* u32 constant: body, the next one flags */
OP(TO_STRING, 0, 0)
OP(PRIVATE_NAME, 4, 1)          /* u32 description */
OP(GET_PRIVATE, 0, -1)          /* obj sym -> value */
OP(SET_PRIVATE, 0, -2)          /* obj sym value -> value */
OP(DEFINE_PRIVATE, 0, -2)       /* obj sym value -> obj */
OP(PRIVATE_IN, 0, -1)           /* sym obj -> bool */
OP(DEBUGGER, 0, 0)
OP(UNSUPPORTED, 4, 0)           /* u32 message: a SyntaxError at run time (generators, async) */
OP(INITIAL_YIELD, 1, 0)         /* (27r) u8 async: the generator object made, the call returns (async: runs on) */
OP(YIELD, 0, 1)                 /* (27r) value -> (resumed) received, kind */
OP(AWAIT, 0, 1)                 /* (27r) value -> (resumed) received, kind */
OP(RESUME_SWITCH, 4, -1)        /* (27r) received kind -> received; i32: where a return goes (0: none) */
OP(DELEGATE_START, 0, 3)        /* (27r) yield*: iterable -> iterator, next, received (undefined), kind (next) */
OP(DELEGATE_STEP, 8, 99)        /* (27r) i32 done, i32 return: passes received on (see interp.c) */
OP(YIELD_RAW, 0, 1)             /* (27r) yield*'s: the inner result object as it is -> received, kind */
