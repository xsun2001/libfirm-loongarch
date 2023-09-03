/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   emit assembler for a backend graph
 */
#include "loongarch64_emitter.h"

#include "bearch.h"
#include "beblocksched.h"
#include "beemithlp.h"
#include "beemitter.h"
#include "begnuas.h"
#include "benode.h"
#include "besched.h"
#include "gen_loongarch64_emitter.h"
#include "irgwalk.h"
#include "loongarch64_new_nodes.h"
#include "panic.h"
#include "util.h"

static void loongarch64_emit_immediate(const ir_node *node) {
    loongarch64_immediate_attr_t const *const attr = get_loongarch64_immediate_attr_const(node);
    ir_entity *const                          ent  = attr->ent;
    int64_t                                   val  = attr->val;
    if (ent) {
        be_emit_irprintf("&%s", get_entity_ld_name(ent));
        be_emit_irprintf("%d", val);
    } else {
        be_emit_irprintf("%d", val);
    }
}

static void emit_register(const arch_register_t *reg) {
    be_emit_char('$');
    be_emit_string(reg->name);
}

static void loongarch64_emit_source_register(const ir_node *node, int pos) {
    const arch_register_t *reg = arch_get_irn_register_in(node, pos);
    emit_register(reg);
}

static void loongarch64_emit_dest_register(const ir_node *node, int pos) {
    const arch_register_t *reg = arch_get_irn_register_out(node, pos);
    emit_register(reg);
}

void loongarch64_emitf(const ir_node *node, const char *format, ...) {
    BE_EMITF(node, format, ap, false) {
        switch (*format++) {
        case 'S': {
            if (!is_digit(*format))
                goto unknown;
            unsigned const pos = *format++ - '0';
            loongarch64_emit_source_register(node, pos);
            break;
        }

        case 'D': {
            if (!is_digit(*format))
                goto unknown;
            unsigned const pos = *format++ - '0';
            loongarch64_emit_dest_register(node, pos);
            break;
        }

        case 'I': {
            loongarch64_emit_immediate(node);
            break;
        }

        case 'X': {
            int num = va_arg(ap, int);
            be_emit_irprintf("%X", num);
            break;
        }

        case 'A': {
            loongarch64_emit_source_register(node, 1);
            be_emit_char(',');
            be_emit_char('\t');
            loongarch64_emit_immediate(node);
            break;
        }

        case 'G': {
            loongarch64_immediate_attr_t const *const attr = get_loongarch64_immediate_attr_const(node);
            ir_entity *const                          ent  = attr->ent;
            if (ent) {
                be_emit_irprintf("%s", get_entity_ld_name(ent));
            } else {
                panic("no entity for global address");
            }
            break;
        }

        default:
        unknown:
            panic("unknown format conversion");
        }
    }
}

/**
 * Emits code for a unconditional jump.
 */
static void emit_loongarch64_Jmp(const ir_node *node) { loongarch64_emitf(node, "jmp %L", node); }

static void emit_be_Copy(ir_node const *const node) {
    ir_node *const               op  = be_get_Copy_op(node);
    arch_register_t const *const in  = arch_get_irn_register(op);
    arch_register_t const *const out = arch_get_irn_register(node);
    if (in == out)
        return;

    loongarch64_emitf(node, "ori\t%D0,\t%S0,\t0");
}

static void emit_be_IncSP(const ir_node *node) {
    int offset = be_get_IncSP_offset(node);
    if (offset != 0) {
        loongarch64_emitf(node, "addi.d\t%D0,\t%S0,\t%d", -offset);
    }
}

static void emit_Return(const ir_node *node) {
    ir_graph *irg        = get_irn_irg(node);
    ir_type  *frame_type = get_irg_frame_type(irg);
    unsigned  size       = get_type_size(frame_type);

    /* emit function epilog here */

    /* deallocate stackframe */
    if (size > 0) {
        loongarch64_emitf(node, "add %%sp, %u, %%sp", size);
    }

    /* return */
    unsigned const    n_res = get_irn_arity(node) - n_loongarch64_return_first_result;
    char const *const fmt   = n_res == 0 ? "ret" : n_res == 1 ? "ret %S2" : "ret %S2, ...";
    loongarch64_emitf(node, fmt);
}

/**
 * Enters the emitter functions for handled nodes into the generic
 * pointer of an opcode.
 */
static void loongarch64_register_emitters(void) {
    be_init_emitters();

    /* register all emitter functions defined in spec */
    loongarch64_register_spec_emitters();

    /* custom emitters not provided by the spec */
    be_set_emitter(op_be_Copy, emit_be_Copy);
    be_set_emitter(op_be_IncSP, emit_be_IncSP);
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void loongarch64_emit_block(ir_node *block) {
    be_gas_begin_block(block);

    sched_foreach(block, node) { be_emit_node(node); }
}

void loongarch64_emit_function(ir_graph *irg) {
    /* register all emitter functions */
    loongarch64_register_emitters();

    /* create the block schedule */
    ir_node **block_schedule = be_create_block_schedule(irg);

    /* emit assembler prolog */
    ir_entity *entity = get_irg_entity(irg);
    be_gas_emit_function_prolog(entity, 4, NULL);

    /* populate jump link fields with their destinations */
    ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);

    be_emit_init_cf_links(block_schedule);

    for (size_t i = 0, n = ARR_LEN(block_schedule); i < n; ++i) {
        ir_node *block = block_schedule[i];
        loongarch64_emit_block(block);
    }
    ir_free_resources(irg, IR_RESOURCE_IRN_LINK);

    be_gas_emit_function_epilog(entity);
}
