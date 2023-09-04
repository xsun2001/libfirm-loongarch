/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    The main loongarch64 backend driver file.
 */
#include "be2addr.h"
#include "be_t.h"
#include "beirg.h"
#include "bemodule.h"
#include "benode.h"
#include "bera.h"
#include "besched.h"
#include "bespillslots.h"
#include "bestack.h"
#include "betranshlp.h"
#include "debug.h"
#include "gen_loongarch64_regalloc_if.h"
#include "iredges_t.h"
#include "irgwalk.h"
#include "irprog_t.h"
#include "isas.h"
#include "loongarch64_emitter.h"
#include "loongarch64_new_nodes.h"
#include "loongarch64_transform.h"
#include "lower_builtins.h"
#include "lower_calls.h"
#include "panic.h"
#include "target_t.h"

/**
 * Transforms the standard firm graph into a loongarch64 firm graph
 */
static void loongarch64_select_instructions(ir_graph *irg) {
    /* transform nodes into assembler instructions */
    be_timer_push(T_CODEGEN);
    loongarch64_transform_graph(irg);
    be_timer_pop(T_CODEGEN);
    be_dump(DUMP_BE, irg, "code-selection");

    place_code(irg);
    be_dump(DUMP_BE, irg, "place");
}

static ir_node *loongarch64_new_spill(ir_node *value, ir_node *after) {
    ir_mode *const mode = get_irn_mode(value);
    if (be_mode_needs_gp_reg(mode)) {
        ir_node *const  block = get_block(after);
        ir_graph *const irg   = get_irn_irg(after);
        ir_node *const  nomem = get_irg_no_mem(irg);
        ir_node *const  frame = get_irg_frame(irg);
        ir_node *const  store = new_bd_loongarch64_st_d(NULL, block, nomem, frame, value, NULL, 0);
        sched_add_after(after, store);
        return store;
    }
    TODO(value);
}

static ir_node *loongarch64_new_reload(ir_node *value, ir_node *spill, ir_node *before) {
    ir_mode *const mode = get_irn_mode(value);
    if (be_mode_needs_gp_reg(mode)) {
        ir_node *const  block = get_block(before);
        ir_graph *const irg   = get_irn_irg(before);
        ir_node *const  frame = get_irg_frame(irg);
        ir_node *const  load  = new_bd_loongarch64_ld_d(NULL, block, spill, frame, NULL, 0);
        sched_add_before(before, load);
        return be_new_Proj(load, pn_loongarch64_ld_d_res);
    }
    TODO(value);
}

static const regalloc_if_t loongarch64_regalloc_if = {
    .spill_cost  = 1,
    .reload_cost = 1,
    .new_spill   = loongarch64_new_spill,
    .new_reload  = loongarch64_new_reload,
};

static void loongarch64_collect_frame_entity_nodes(ir_node *const node, void *const data) {
    be_fec_env_t *const env = (be_fec_env_t *)data;

    if (is_loongarch64_ld_w(node) || is_loongarch64_ld_h(node) || is_loongarch64_ld_b(node) ||
        is_loongarch64_ld_wu(node) || is_loongarch64_ld_hu(node) || is_loongarch64_ld_bu(node) ||
        is_loongarch64_ld_d(node)) {
        ir_node *const  base  = get_irn_n(node, 1);
        ir_graph *const irg   = get_irn_irg(node);
        ir_node *const  frame = get_irg_frame(irg);
        if (base == frame) {
            loongarch64_immediate_attr_t const *const attr = get_loongarch64_immediate_attr_const(node);
            if (!attr->ent) {
                be_load_needs_frame_entity(env, node, 16, 4);
            }
        }
    }
}

static void loongarch64_set_frame_entity(ir_node *const node, ir_entity *const entity, unsigned const size,
                                         unsigned const po2align) {
    (void)size, (void)po2align;

    loongarch64_immediate_attr_t *const imm = get_loongarch64_immediate_attr(node);
    imm->ent                                = entity;
}

static void loongarch64_assign_spill_slots(ir_graph *const irg) {
    be_fec_env_t *const fec_env = be_new_frame_entity_coalescer(irg);
    irg_walk_graph(irg, NULL, loongarch64_collect_frame_entity_nodes, fec_env);
    be_assign_entities(fec_env, loongarch64_set_frame_entity, true);
    be_free_frame_entity_coalescer(fec_env);
}

static void loongarch64_introduce_prologue(ir_graph *const irg, unsigned const size) {
    ir_node *const start    = get_irg_start(irg);
    ir_node *const block    = get_nodes_block(start);
    ir_node *const start_sp = be_get_Start_proj(irg, &loongarch64_registers[REG_SP]);
    ir_node *const inc_sp   = be_new_IncSP(block, start_sp, size, 0);
    sched_add_after(start, inc_sp);
    edges_reroute_except(start_sp, inc_sp, inc_sp);
}

static void loongarch64_introduce_epilogue(ir_node *const ret, unsigned const size) {
    ir_node *const block  = get_nodes_block(ret);
    ir_node *const ret_sp = get_irn_n(ret, n_loongarch64_return_stack);
    ir_node *const inc_sp = be_new_IncSP(block, ret_sp, -(int) size, 0);
    sched_add_before(ret, inc_sp);
    set_irn_n(ret, n_loongarch64_return_stack, inc_sp);
}

static void loongarch64_introduce_prologue_epilogue(ir_graph *const irg) {
    ir_type *const frame = get_irg_frame_type(irg);
    unsigned const size  = get_type_size(frame);
    if (size == 0)
        return;

    foreach_irn_in(get_irg_end_block(irg), i, ret) {
        assert(is_loongarch64_return(ret));
        loongarch64_introduce_epilogue(ret, size);
    }

    loongarch64_introduce_prologue(irg, size);
}

static void loongarch64_sp_sim(ir_node *const node, stack_pointer_state_t *const state) {
    if (is_loongarch64_irn(node)) {
        switch ((loongarch64_opcodes)get_loongarch64_irn_opcode(node)) {
        case iro_loongarch64_addu16i_d:
        case iro_loongarch64_ld_d:
        case iro_loongarch64_ld_w:
        case iro_loongarch64_ld_wu:
        case iro_loongarch64_ld_h:
        case iro_loongarch64_ld_hu:
        case iro_loongarch64_ld_b:
        case iro_loongarch64_ld_bu:
        case iro_loongarch64_st_d:
        case iro_loongarch64_st_w:
        case iro_loongarch64_st_h:
        case iro_loongarch64_st_b: {
            loongarch64_immediate_attr_t *const imm = get_loongarch64_immediate_attr(node);
            ir_entity *const                    ent = imm->ent;
            if (ent && is_frame_type(get_entity_owner(ent))) {
                imm->ent = NULL;
                imm->val += state->offset + get_entity_offset(ent);
            }
            break;
        }
        default:
            break;
        }
    }
}

static void loongarch64_generate_code(FILE *output, const char *cup_name) {
    be_begin(output, cup_name);
    unsigned *const sp_is_non_ssa = rbitset_alloca(N_LOONGARCH64_REGISTERS);
    rbitset_set(sp_is_non_ssa, REG_SP);

    foreach_irp_irg(i, irg) {
        if (!be_step_first(irg))
            continue;

        // Skip checking SSA property for `sp` register.
        be_birg_from_irg(irg)->non_ssa_regs = sp_is_non_ssa;
        loongarch64_select_instructions(irg);

        be_step_schedule(irg);

        // Register allocation.
        // 'Load' of spilled valued is set to `ent = NULL, offset = 0`.
        be_step_regalloc(irg, &loongarch64_regalloc_if);
        // Then find all 'Load' nodes with `ent = NULL`.
        // They all require assigning a frame entity.
        loongarch64_assign_spill_slots(irg);

        ir_type *const frame = get_irg_frame_type(irg);
        be_sort_frame_entities(frame, true);
        be_layout_frame_type(frame, 0, 0);

        loongarch64_introduce_prologue_epilogue(irg);

        // Fix `sp` register to be in SSA form.
        be_fix_stack_nodes(irg, &loongarch64_registers[REG_SP]);
        be_birg_from_irg(irg)->non_ssa_regs = NULL;

        // Transform entity information to relative position to `sp`.
        be_sim_stack_pointer(irg, 0, 4, &loongarch64_sp_sim);

        be_handle_2addr(irg, NULL);

        loongarch64_emit_function(irg);
        be_step_last(irg);
    }

    be_finish();
}

static void loongarch64_init(void) {
    loongarch64_register_init();
    loongarch64_create_opcodes();

    ir_target.experimental       = "The loongarch64 backend is highly experimental";
    ir_target.float_int_overflow = ir_overflow_min_max;
}

static void loongarch64_finish(void) { loongarch64_free_opcodes(); }

static void loongarch64_lower_for_target(void) {
    lower_builtins(0, NULL, NULL);
    be_after_irp_transform("lower-builtins");

    /* lower compound param handling */
    lower_calls_with_compounds(LF_RETURN_HIDDEN, lower_aggregates_as_pointers, NULL, lower_aggregates_as_pointers, NULL,
                               reset_stateless_abi);
    be_after_irp_transform("lower-calls");
}

static unsigned loongarch64_get_op_estimated_cost(const ir_node *node) { return 1; }

arch_isa_if_t const loongarch64_isa_if = {
    .name                  = "loongarch64",
    .pointer_size          = 8,
    .modulo_shift          = 32,
    .big_endian            = false,
    .po2_biggest_alignment = 4,
    .pic_supported         = false,
    .register_prefix       = '$',
    .n_registers           = N_LOONGARCH64_REGISTERS,
    .registers             = loongarch64_registers,
    .n_register_classes    = N_LOONGARCH64_CLASSES,
    .register_classes      = loongarch64_reg_classes,
    .init                  = loongarch64_init,
    .finish                = loongarch64_finish,
    .generate_code         = loongarch64_generate_code,
    .lower_for_target      = loongarch64_lower_for_target,
    .get_op_estimated_cost = loongarch64_get_op_estimated_cost,
};

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_loongarch64)
void be_init_arch_loongarch64(void) { loongarch64_init_transform(); }
