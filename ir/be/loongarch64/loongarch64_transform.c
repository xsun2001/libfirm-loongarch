/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   code selection (transform FIRM into loongarch64 FIRM)
 */
#include "loongarch64_transform.h"

#include "beirg.h"
#include "benode.h"
#include "betranshlp.h"
#include "debug.h"
#include "gen_loongarch64_regalloc_if.h"
#include "ircons.h"
#include "iredges_t.h"
#include "irgmod.h"
#include "irgraph_t.h"
#include "irmode_t.h"
#include "irnode_t.h"
#include "iropt_t.h"
#include "loongarch64_bearch_t.h"
#include "loongarch64_new_nodes.h"
#include "loongarch64_nodes_attr.h"
#include "panic.h"
#include "util.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

// ------------------- Helpers -------------------

// Functions types

typedef ir_node *(*new_binop_reg_func)(dbg_info *dbgi, ir_node *block, ir_node *op1, ir_node *op2);
typedef ir_node *(*new_binop_imm_func)(dbg_info *dbgi, ir_node *block, ir_node *op1, ir_entity *const entity,
                                       int64_t value);
typedef ir_node *(*new_uniop_func)(dbg_info *dbgi, ir_node *block, ir_node *op);
typedef ir_node *(*cons_loadop)(dbg_info *, ir_node *, ir_node *, ir_node *, ir_entity *, int64_t);
typedef ir_node *(*cons_storeop)(dbg_info *, ir_node *, ir_node *, ir_node *, ir_node *, ir_entity *, int64_t);

#define TRANS_FUNC(name) static ir_node *gen_##name(ir_node *node)

static bool is_valid_si12(ir_node *node) {
    if (is_Const(node)) {
        long const value = get_Const_long(node);
        return is_simm12(value);
    }
    return false;
}

static ir_node *transform_common_binop(ir_node *node, ir_mode *provide_mode, bool is_commutative,
                                       new_binop_reg_func new_func_w, new_binop_reg_func new_func_d,
                                       new_binop_reg_func new_func_wu, new_binop_reg_func new_func_du,
                                       new_binop_imm_func new_func_wi, new_binop_imm_func new_func_di) {
    ir_mode  *mode  = provide_mode ? provide_mode : get_irn_mode(node);
    ir_node  *block = be_transform_nodes_block(node);
    ir_node  *op1   = get_binop_left(node);
    ir_node  *op2   = get_binop_right(node);
    dbg_info *dbgi  = get_irn_dbg_info(node);
    unsigned  bits  = get_mode_size_bits(mode);

    // 'mode-P' is pointer type, which is equal to 64-bit unsigned int.
    if (mode == mode_P) {
        mode = mode_Lu;
    }

    if (mode_is_int(mode)) {
        // Const folding
        if (new_func_wi && new_func_di) {
            // TODO: Both op1 and op2 are const. Generate a new const node.
            if (is_valid_si12(op2)) {
                op1 = be_transform_node(op1);
                if (bits == 32) {
                    return new_func_wi(dbgi, block, op1, NULL, get_Const_long(op2));
                } else if (bits == 64) {
                    return new_func_di(dbgi, block, op1, NULL, get_Const_long(op2));
                }
            }
            if (is_commutative && is_valid_si12(op1)) {
                op2 = be_transform_node(op2);
                if (bits == 32) {
                    return new_func_wi(dbgi, block, op2, NULL, get_Const_long(op1));
                } else if (bits == 64) {
                    return new_func_di(dbgi, block, op2, NULL, get_Const_long(op1));
                }
            }
        }

        ir_node *new_op1 = be_transform_node(op1);
        ir_node *new_op2 = be_transform_node(op2);
        if (!mode_is_signed(mode)) {
            if (bits == 32 && new_func_wu) {
                return new_func_wu(dbgi, block, new_op1, new_op2);
            } else if (bits == 64 && new_func_du) {
                return new_func_du(dbgi, block, new_op1, new_op2);
            }
        }
        if (bits == 32 && new_func_w) {
            return new_func_w(dbgi, block, new_op1, new_op2);
        } else if (bits == 64 && new_func_d) {
            return new_func_d(dbgi, block, new_op1, new_op2);
        }
    }

    // TODO: float-point support
    TODO(node);
}

#define LA64_WD_INST(name) new_bd_loongarch64_##name##_w, new_bd_loongarch64_##name##_d
#define LA64_WDU_INST(name)                                                                                            \
    new_bd_loongarch64_##name##_w, new_bd_loongarch64_##name##_d, new_bd_loongarch64_##name##_wu,                      \
        new_bd_loongarch64_##name##_du
#define LA64_WD_SAME_INST(name) new_bd_loongarch64_##name, new_bd_loongarch64_##name

static ir_node *transform_const(ir_node *const node, ir_entity *const entity, uint64_t value) {
    ir_node *const  block = be_transform_nodes_block(node);
    dbg_info *const dbgi  = get_irn_dbg_info(node);
    ir_mode *const  mode  = get_irn_mode(node);
    unsigned        bits  = get_mode_size_bits(mode);

    if (value == 0) {
        ir_graph *const irg = get_irn_irg(node);
        return be_get_Start_proj(irg, &loongarch64_registers[REG_ZERO]);
    }
    if (bits == 32) {
        return new_bd_loongarch64_li_w(dbgi, block, entity, value);
    } else if (bits == 64) {
        return new_bd_loongarch64_li_d(dbgi, block, entity, value);
    }
    TODO(node);
}

typedef struct loongarch64_addr {
    ir_node   *base;
    ir_entity *ent;
    int64_t    val;
} loongarch64_addr;

static loongarch64_addr make_addr(ir_node *addr) {
    ir_entity *ent = 0;
    int64_t    val = 0;

    if (is_Add(addr)) {
        ir_node *const r = get_Add_right(addr);
        if (is_Const(r)) {
            long const v = get_Const_long(r);
            if (is_simm12(v)) {
                val  = v;
                addr = get_Add_left(addr);
            }
        }
    }

    if (is_Member(addr)) {
        ent  = get_Member_entity(addr);
        addr = get_Member_ptr(addr);
        assert(is_Proj(addr) && get_Proj_num(addr) == pn_Start_P_frame_base && is_Start(get_Proj_pred(addr)));
    }

    ir_node *const base = be_transform_node(addr);
    return (loongarch64_addr){base, ent, val};
}

static ir_node *get_Start_sp(ir_graph *const irg) { return be_get_Start_proj(irg, &loongarch64_registers[REG_SP]); }

// ------------------- Arithemtic -------------------

TRANS_FUNC(Add) { return transform_common_binop(node, NULL, true, LA64_WD_INST(add), NULL, NULL, LA64_WD_INST(addi)); }

TRANS_FUNC(Sub) { return transform_common_binop(node, NULL, false, LA64_WD_INST(sub), NULL, NULL, NULL, NULL); }

TRANS_FUNC(Mul) { return transform_common_binop(node, NULL, true, LA64_WD_INST(mul), NULL, NULL, NULL, NULL); }

TRANS_FUNC(Mulh) { return transform_common_binop(node, NULL, true, LA64_WDU_INST(mulh), NULL, NULL); }

TRANS_FUNC(Div) { return transform_common_binop(node, get_Div_resmode(node), false, LA64_WDU_INST(div), NULL, NULL); }

TRANS_FUNC(Mod) { return transform_common_binop(node, get_Mod_resmode(node), false, LA64_WDU_INST(mod), NULL, NULL); }

TRANS_FUNC(Minus) {
    dbg_info *const dbgi  = get_irn_dbg_info(node);
    ir_node *const  block = be_transform_nodes_block(node);
    ir_node *const  val   = get_Minus_op(node);
    ir_mode *const  mode  = get_irn_mode(node);
    unsigned        bits  = get_mode_size_bits(mode);

    if (mode_is_int(mode)) {
        ir_graph *const irg   = get_irn_irg(node);
        ir_node *const  new_l = be_get_Start_proj(irg, &loongarch64_registers[REG_ZERO]);
        ir_node *const  new_r = be_transform_node(val);
        if (bits == 32) {
            return new_bd_loongarch64_sub_w(dbgi, block, new_l, new_r);
        } else if (bits == 64) {
            return new_bd_loongarch64_sub_d(dbgi, block, new_l, new_r);
        }
    }

    TODO(node);
}

TRANS_FUNC(Shl) { return transform_common_binop(node, NULL, false, LA64_WD_INST(sll), NULL, NULL, LA64_WD_INST(slli)); }

TRANS_FUNC(Shr) { return transform_common_binop(node, NULL, false, LA64_WD_INST(srl), NULL, NULL, LA64_WD_INST(srli)); }

TRANS_FUNC(Shrs) {
    return transform_common_binop(node, NULL, false, LA64_WD_INST(sra), NULL, NULL, LA64_WD_INST(srai));
}

// ------------------- Bit manipulation -------------------

TRANS_FUNC(And) {
    return transform_common_binop(node, NULL, true, LA64_WD_SAME_INST(and), NULL, NULL, LA64_WD_SAME_INST(andi));
}

TRANS_FUNC(Or) {
    return transform_common_binop(node, NULL, true, LA64_WD_SAME_INST(or), NULL, NULL, LA64_WD_SAME_INST(ori));
}

TRANS_FUNC(Eor) {
    return transform_common_binop(node, NULL, true, LA64_WD_SAME_INST(xor), NULL, NULL, LA64_WD_SAME_INST(xori));
}

TRANS_FUNC(Not) {
    dbg_info *const dbgi  = get_irn_dbg_info(node);
    ir_node *const  block = be_transform_nodes_block(node);
    ir_node *const  val   = get_Minus_op(node);
    ir_mode *const  mode  = get_irn_mode(node);

    if (mode_is_int(mode)) {
        ir_graph *const irg   = get_irn_irg(node);
        ir_node *const  new_l = be_get_Start_proj(irg, &loongarch64_registers[REG_ZERO]);
        ir_node *const  new_r = be_transform_node(val);
        return new_bd_loongarch64_nor(dbgi, block, new_l, new_r);
    }
}

TRANS_FUNC(Const) {
    int64_t value = get_Const_long(node);
    return transform_const(node, NULL, value);
}

// ------------------- Conversion -------------------

// Convert `node` to `target` mode in 64-bits register.
ir_node *convert_value(dbg_info *const dbgi, ir_node *const node, ir_mode *const target) {
    ir_node *const new_op   = be_transform_node(node);
    ir_node *const block    = get_nodes_block(new_op);
    ir_mode *const mode     = get_irn_mode(node);
    unsigned const o_bits   = get_mode_size_bits(mode);
    bool           o_signed = mode_is_signed(mode);
    unsigned const t_bits   = get_mode_size_bits(target);
    bool           t_signed = mode_is_signed(target);
    // only int
    if (!mode_is_int(mode) || !mode_is_int(target)) {
        TODO(node);
    }
    // unsigned int -> long
    if (mode == mode_Iu && t_bits == 64) {
        return new_bd_loongarch64_zext_w(dbgi, block, new_op);
    }
    // narrow to int/unsigned int
    if (o_bits == 64 && t_bits == 32) {
        return new_bd_loongarch64_sext_w(dbgi, block, new_op);
    }
    // narrow conversion
    if (o_bits > t_bits || (o_bits == t_bits && o_signed != t_signed)) {
        if (t_bits == 8) {
            return (t_signed ? new_bd_loongarch64_sext_b : new_bd_loongarch64_zext_b)(dbgi, block, new_op);
        } else if (t_bits == 16) {
            return (t_signed ? new_bd_loongarch64_sext_h : new_bd_loongarch64_zext_h)(dbgi, block, new_op);
        }
    }
    // Don't need to convert
    return new_op;
}

ir_node *extend_value(ir_node *const node) { return convert_value(NULL, node, mode_Ls); }

TRANS_FUNC(Conv) {
    ir_node *const  block = be_transform_nodes_block(node);
    dbg_info *const dbgi  = get_irn_dbg_info(node);
    ir_node *const  op    = get_Conv_op(node);
    ir_mode *const  mode  = get_irn_mode(node);
    return convert_value(dbgi, op, mode);
}

// ------------------- Memory -------------------

TRANS_FUNC(Load) {
    ir_mode *const mode = get_Load_mode(node);
    if (be_mode_needs_gp_reg(mode)) {
        cons_loadop    cons;
        unsigned const size = get_mode_size_bits(mode);
        if (size == 8) {
            cons = mode_is_signed(mode) ? new_bd_loongarch64_ld_b : new_bd_loongarch64_ld_bu;
        } else if (size == 16) {
            cons = mode_is_signed(mode) ? new_bd_loongarch64_ld_h : new_bd_loongarch64_ld_hu;
        } else if (size == 32) {
            cons = mode_is_signed(mode) ? new_bd_loongarch64_ld_w : new_bd_loongarch64_ld_wu;
        } else if (size == 64) {
            cons = new_bd_loongarch64_ld_d;
        } else {
            panic("invalid load");
        }
        dbg_info *const        dbgi  = get_irn_dbg_info(node);
        ir_node *const         block = be_transform_nodes_block(node);
        ir_node *const         mem   = be_transform_node(get_Load_mem(node));
        loongarch64_addr const addr  = make_addr(get_Load_ptr(node));
        return cons(dbgi, block, mem, addr.base, addr.ent, addr.val);
    }
    TODO(node);
}

TRANS_FUNC(Store) {
    ir_node       *old_val = get_Store_value(node);
    ir_mode *const mode    = get_irn_mode(old_val);
    if (be_mode_needs_gp_reg(mode)) {
        cons_storeop   cons;
        unsigned const size = get_mode_size_bits(mode);
        if (size == 8) {
            cons = new_bd_loongarch64_st_b;
        } else if (size == 16) {
            cons = new_bd_loongarch64_st_h;
        } else if (size == 32) {
            cons = new_bd_loongarch64_st_w;
        } else if (size == 64) {
            cons = new_bd_loongarch64_st_d;
        } else {
            panic("invalid store");
        }
        old_val                      = be_skip_downconv(old_val, false);
        dbg_info *const        dbgi  = get_irn_dbg_info(node);
        ir_node *const         block = be_transform_nodes_block(node);
        ir_node *const         mem   = be_transform_node(get_Store_mem(node));
        ir_node *const         val   = be_transform_node(old_val);
        loongarch64_addr const addr  = make_addr(get_Store_ptr(node));
        return cons(dbgi, block, mem, addr.base, val, addr.ent, addr.val);
    }
    TODO(node);
}

TRANS_FUNC(Address) {
    dbg_info *const  dbgi   = get_irn_dbg_info(node);
    ir_node *const   block  = be_transform_nodes_block(node);
    ir_entity *const entity = get_Address_entity(node);
    return new_bd_loongarch64_load_address(dbgi, block, entity, 0);
}

TRANS_FUNC(Member) {
    ir_node *const ptr = get_Member_ptr(node);
    assert(is_Proj(ptr) && get_Proj_num(ptr) == pn_Start_P_frame_base && is_Start(get_Proj_pred(ptr)));
    dbg_info *const  dbgi  = get_irn_dbg_info(node);
    ir_node *const   block = be_transform_nodes_block(node);
    ir_node *const   frame = be_transform_node(ptr);
    ir_entity *const ent   = get_Member_entity(node);
    return new_bd_loongarch64_addi_d(dbgi, block, frame, ent, 0);
}

// ------------------- Compare or Conditional -------------------

TRANS_FUNC(Cmp) { TODO(node); }

TRANS_FUNC(Cond) { TODO(node); }

TRANS_FUNC(Mux) { TODO(node); }

// ------------------- Control Flow -------------------

TRANS_FUNC(IJmp) { TODO(node); }

TRANS_FUNC(Jmp) { TODO(node); }

TRANS_FUNC(Switch) { TODO(node); }

// ------------------- Calling Convention -------------------

static unsigned const reg_params[] = {
    REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7,
};

static unsigned const reg_results[] = {
    REG_A0,
    REG_A1,
};

static unsigned const reg_callee_saves[] = {REG_S0, REG_S1, REG_S2, REG_S3, REG_S4, REG_S5, REG_S6, REG_S7, REG_S8};

static unsigned const reg_caller_saves[] = {
    REG_RA, REG_T0, REG_T1, REG_T2, REG_T3, REG_T4, REG_T5, REG_T6, REG_T7,
    REG_T8, REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7,
};

typedef struct reg_or_slot_t {
    arch_register_t const *reg;
    unsigned               offset;
    ir_entity             *entity;
} reg_or_slot_t;

typedef struct calling_convention_t {
    size_t         n_params;
    reg_or_slot_t *parameters;
    reg_or_slot_t *results;
} calling_convention_t;

static calling_convention_t cconv;
static be_stack_env_t       stack_env;

static void set_allocatable_regs(ir_graph *const irg) {
    be_irg_t       *birg   = be_birg_from_irg(irg);
    struct obstack *obst   = &birg->obst;
    unsigned       *a_regs = rbitset_obstack_alloc(obst, N_LOONGARCH64_REGISTERS);

    for (size_t r = 0, n = ARRAY_SIZE(reg_callee_saves); r < n; ++r) {
        rbitset_set(a_regs, reg_callee_saves[r]);
    }
    for (size_t r = 0, n = ARRAY_SIZE(reg_caller_saves); r < n; ++r) {
        rbitset_set(a_regs, reg_caller_saves[r]);
    }

    birg->allocatable_regs = a_regs;
}

static void setup_calling_convention(calling_convention_t *const cconv, ir_type *const fun_type) {
    size_t const   n_params        = get_method_n_params(fun_type);
    size_t const   n_max_gp_params = ARRAY_SIZE(reg_params);
    reg_or_slot_t *arr             = NULL;
    if (n_params > 0) {
        arr = XMALLOCNZ(reg_or_slot_t, n_params);
        for (size_t i = 0; i != n_params; ++i) {
            ir_type *const param_type = get_method_param_type(fun_type, i);
            ir_mode *const param_mode = get_type_mode(param_type);
            if (!param_mode || mode_is_float(param_mode)) {
                panic("TODO");
            }
            if (i < n_max_gp_params) {
                arr[i].reg    = &loongarch64_registers[reg_params[i]];
                arr[i].offset = 0;
            } else {
                arr[i].offset = (i - n_max_gp_params) * 8;
            }
        }
    }
    cconv->n_params   = n_params;
    cconv->parameters = arr;

    size_t const n_result = get_method_n_ress(fun_type);
    arr                   = NULL;
    if (n_result > 0) {
        arr = XMALLOCNZ(reg_or_slot_t, n_result);
        for (size_t i = 0; i != n_result; ++i) {
            ir_type *const res_type = get_method_res_type(fun_type, i);
            ir_mode *const res_mode = get_type_mode(res_type);
            if (!res_mode || mode_is_float(res_mode)) {
                panic("TODO");
            }
            if (i >= ARRAY_SIZE(reg_results)) {
                panic("Too many gp results");
            }
            arr[i].reg = &loongarch64_registers[reg_results[i]];
        }
    }
    cconv->results = arr;
}

static void free_calling_convention(calling_convention_t *const cconv) {
    free(cconv->parameters);
    free(cconv->results);
}

void layout_parameter_entities(calling_convention_t *const cconv, ir_graph *const irg) {
    ir_entity **const param_map  = be_collect_parameter_entities(irg);
    ir_type *const    frame_type = get_irg_frame_type(irg);
    ir_entity *const  fun_ent    = get_irg_entity(irg);
    ir_type *const    fun_type   = get_entity_type(fun_ent);
    size_t const      n_params   = get_method_n_params(fun_type);

    for (size_t i = 0; i != n_params; ++i) {
        reg_or_slot_t *const param      = &cconv->parameters[i];
        ir_type *const       param_type = get_method_param_type(fun_type, i);
        if (!is_atomic_type(param_type))
            panic("unhandled parameter type");
        ir_entity *param_ent = param_map[i];
        if (!param->reg) {
            if (!param_ent)
                param_ent = new_parameter_entity(frame_type, i, param_type);
            assert(get_entity_offset(param_ent) == INVALID_OFFSET);
            set_entity_offset(param_ent, param->offset);
        }
        param->entity = param_ent;
    }
    free(param_map);
}

TRANS_FUNC(Call) {
    ir_graph *const irg = get_irn_irg(node);

    unsigned                          p        = n_loongarch64_call_first_argument;
    unsigned const                    n_params = get_Call_n_params(node);
    unsigned const                    n_ins    = p + 1 + n_params;
    arch_register_req_t const **const reqs     = be_allocate_in_reqs(irg, n_ins);
    ir_node                          *ins[n_ins];

    // Confirm callee. Global function or function pointer.
    ir_entity     *callee;
    ir_node *const ptr = get_Call_ptr(node);
    if (is_Address(ptr)) {
        callee = get_Address_entity(ptr);
    } else {
        callee  = NULL;
        ins[p]  = be_transform_node(ptr);
        reqs[p] = &loongarch64_class_reg_req_gp;
        ++p;
    }

    ir_type *const fun_type = get_Call_type(node);
    record_returns_twice(irg, fun_type);

    calling_convention_t cconv;
    setup_calling_convention(&cconv, fun_type);

    size_t   n_mem_param = cconv.n_params > ARRAY_SIZE(reg_params) ? cconv.n_params - ARRAY_SIZE(reg_params) : 0;
    ir_node *mems[1 + n_mem_param];
    unsigned m = 0;

    ir_node *const mem = get_Call_mem(node);
    mems[m++]          = be_transform_node(mem);

    int const      frame_size = n_mem_param * 16;
    ir_node *const block      = be_transform_nodes_block(node);
    ir_node *const sp         = get_Start_sp(irg);
    ir_node *const call_frame = be_new_IncSP(block, sp, frame_size, 0);

    ins[n_loongarch64_call_stack]  = call_frame;
    reqs[n_loongarch64_call_stack] = &loongarch64_single_reg_req_gp_sp;

    dbg_info *const dbgi = get_irn_dbg_info(node);
    for (size_t i = 0; i != n_params; ++i) {
        ir_node *const arg = get_Call_param(node, i);
        ir_node *const val = extend_value(arg);

        reg_or_slot_t const *const param = &cconv.parameters[i];
        if (param->reg) {
            ins[p]  = val;
            reqs[p] = param->reg->single_req;
            ++p;
        } else {
            ir_node *const nomem = get_irg_no_mem(irg);
            mems[m++]            = new_bd_loongarch64_st_d(dbgi, block, nomem, call_frame, val, NULL, param->offset);
        }
    }

    free_calling_convention(&cconv);

    ins[n_loongarch64_call_mem]  = be_make_Sync(block, m, mems);
    reqs[n_loongarch64_call_mem] = arch_memory_req;

    unsigned const n_res = pn_loongarch64_call_first_result + ARRAY_SIZE(reg_caller_saves);

    ir_node *const call = callee ? new_bd_loongarch64_call(dbgi, block, p, ins, reqs, n_res, callee, 0)
                                 : new_bd_loongarch64_call_pointer(dbgi, block, p, ins, reqs, n_res);

    arch_set_irn_register_req_out(call, pn_loongarch64_call_M, arch_memory_req);
    arch_copy_irn_out_info(call, pn_loongarch64_call_stack, sp);
    for (size_t i = 0; i != ARRAY_SIZE(reg_caller_saves); ++i) {
        arch_set_irn_register_req_out(call, pn_loongarch64_call_first_result + i,
                                      loongarch64_registers[reg_caller_saves[i]].single_req);
    }

    ir_node *const call_stack = be_new_Proj(call, pn_loongarch64_call_stack);
    ir_node *const new_stack  = be_new_IncSP(block, call_stack, -frame_size, 0);
    be_stack_record_chain(&stack_env, call_frame, n_be_IncSP_pred, new_stack);

    return call;
}

TRANS_FUNC(Return) {
    unsigned       p     = n_loongarch64_return_first_result;
    unsigned const n_res = get_Return_n_ress(node);
    unsigned const n_ins = p + n_res + ARRAY_SIZE(reg_callee_saves);

    ir_graph *const                   irg  = get_irn_irg(node);
    arch_register_req_t const **const reqs = be_allocate_in_reqs(irg, n_ins);
    ir_node **const                   in   = ALLOCAN(ir_node *, n_ins);

    ir_node *const mem             = get_Return_mem(node);
    in[n_loongarch64_return_mem]   = be_transform_node(mem);
    reqs[n_loongarch64_return_mem] = arch_memory_req;

    in[n_loongarch64_return_stack]   = get_Start_sp(irg);
    reqs[n_loongarch64_return_stack] = &loongarch64_single_reg_req_gp_sp;

    in[n_loongarch64_return_addr]   = be_get_Start_proj(irg, &loongarch64_registers[REG_RA]);
    reqs[n_loongarch64_return_addr] = &loongarch64_class_reg_req_gp;

    reg_or_slot_t *const results = cconv.results;
    for (size_t i = 0; i != n_res; ++i) {
        ir_node *const res = get_Return_res(node, i);
        in[p]              = be_transform_node(res);
        reqs[p]            = results[i].reg->single_req;
        ++p;
    }

    for (size_t i = 0; i != ARRAY_SIZE(reg_callee_saves); ++i) {
        arch_register_t const *const reg = &loongarch64_registers[reg_callee_saves[i]];
        in[p]                            = be_get_Start_proj(irg, reg);
        reqs[p]                          = reg->single_req;
        ++p;
    }

    assert(p == n_ins);
    dbg_info *const dbgi  = get_irn_dbg_info(node);
    ir_node *const  block = be_transform_nodes_block(node);
    ir_node *const  ret   = new_bd_loongarch64_return(dbgi, block, n_ins, in, reqs);
    be_stack_record_chain(&stack_env, ret, n_loongarch64_return_stack, NULL);
    return ret;
}

TRANS_FUNC(Start) {
    be_start_out outs[N_LOONGARCH64_REGISTERS] = {
        [REG_ZERO] = BE_START_IGNORE, [REG_SP] = BE_START_IGNORE, [REG_TP] = BE_START_NO,
        [REG_R21] = BE_START_NO,      [REG_RA] = BE_START_REG,
    };
    /* function parameters in registers */
    ir_graph *const  irg  = get_irn_irg(node);
    ir_entity *const ent  = get_irg_entity(irg);
    ir_type *const   type = get_entity_type(ent);
    for (size_t i = 0, n = get_method_n_params(type); i != n; ++i) {
        arch_register_t const *const reg = cconv.parameters[i].reg;
        if (reg)
            outs[reg->global_index] = BE_START_REG;
    }
    /* callee_saves */
    for (size_t i = 0; i != ARRAY_SIZE(reg_callee_saves); ++i) {
        outs[reg_callee_saves[i]] = BE_START_REG;
    }

    return be_new_Start(irg, outs);
}

// ------------------- Misc -------------------

TRANS_FUNC(Phi) {
    ir_mode                   *mode = get_irn_mode(node);
    const arch_register_req_t *req;
    if (be_mode_needs_gp_reg(mode)) {
        req = &loongarch64_class_reg_req_gp;
    } else if (mode == mode_M) {
        req = arch_memory_req;
    } else {
        panic("invalid Phi mode");
    }

    return be_transform_phi(node, req);
}

TRANS_FUNC(Unknown) {
    ir_node *const block = be_transform_nodes_block(node);
    ir_mode *const mode  = get_irn_mode(node);
    if (be_mode_needs_gp_reg(mode)) {
        return be_new_Unknown(block, &loongarch64_class_reg_req_gp);
    }
    TODO(node);
}

// ------------------- Projections -------------------

TRANS_FUNC(Proj_Call) {
    ir_node *const pred = get_Proj_pred(node);
    ir_node *const call = be_transform_node(pred);
    unsigned const pn   = get_Proj_num(node);
    switch ((pn_Call)pn) {
    case pn_Call_M:
        return be_new_Proj(call, pn_loongarch64_call_M);
    default:
        TODO(node);
    }
}

TRANS_FUNC(Proj_Div) {
    ir_node *const pred = get_Proj_pred(node);
    unsigned const pn   = get_Proj_num(node);
    switch ((pn_Div)pn) {
    case pn_Div_M:
        return get_Div_mem(pred);
    case pn_Div_res:
        return be_transform_node(pred);
    default:
        TODO(node);
    }
}

TRANS_FUNC(Proj_Mod) {
    ir_node *const pred = get_Proj_pred(node);
    unsigned const pn   = get_Proj_num(node);
    switch ((pn_Mod)pn) {
    case pn_Div_M:
        return get_Mod_mem(pred);
    case pn_Div_res:
        return be_transform_node(pred);
    default:
        TODO(node);
    }
}

TRANS_FUNC(Proj_Proj_Call) {
    ir_node *const pred = get_Proj_pred(node);
    assert(get_Proj_num(pred) == pn_Call_T_result);

    ir_node *const ocall    = get_Proj_pred(pred);
    ir_type *const fun_type = get_Call_type(ocall);

    calling_convention_t cconv;
    setup_calling_convention(&cconv, fun_type);

    ir_node *const               call = be_transform_node(ocall);
    unsigned const               num  = get_Proj_num(node);
    arch_register_t const *const reg  = cconv.results[num].reg;
    unsigned const               pos  = be_get_out_for_reg(call, reg);

    free_calling_convention(&cconv);

    return be_new_Proj(call, pos);
}

TRANS_FUNC(Proj_Proj_Start) {
    assert(get_Proj_num(get_Proj_pred(node)) == pn_Start_T_args);

    ir_graph *const      irg   = get_irn_irg(node);
    unsigned const       num   = get_Proj_num(node);
    reg_or_slot_t *const param = &cconv.parameters[num];
    if (param->reg) {
        return be_get_Start_proj(irg, param->reg);
    } else {
        dbg_info *const dbgi  = get_irn_dbg_info(node);
        ir_node *const  block = be_transform_nodes_block(node);
        ir_node *const  mem   = be_get_Start_mem(irg);
        ir_node *const  base  = get_Start_sp(irg);
        ir_node *const  load  = new_bd_loongarch64_ld_d(dbgi, block, mem, base, param->entity, 0);
        return be_new_Proj(load, pn_loongarch64_ld_d_res);
    }
}

TRANS_FUNC(Proj_Proj) {
    ir_node *const pred      = get_Proj_pred(node);
    ir_node *const pred_pred = get_Proj_pred(pred);
    switch (get_irn_opcode(pred_pred)) {
    case iro_Call:
        return gen_Proj_Proj_Call(node);
    case iro_Start:
        return gen_Proj_Proj_Start(node);
    default:
        TODO(node);
    }
}

TRANS_FUNC(Proj_Load) {
    ir_node *load     = get_Proj_pred(node);
    ir_node *new_load = be_transform_node(load);
    switch ((pn_Load)get_Proj_num(node)) {
    case pn_Load_M:
        return be_new_Proj(new_load, pn_loongarch64_ld_b_M);
    case pn_Load_res:
        return be_new_Proj(new_load, pn_loongarch64_ld_b_res);
    default:
        TODO(node);
    }
}

TRANS_FUNC(Proj_Store) {
    ir_node *store     = get_Proj_pred(node);
    ir_node *new_store = be_transform_node(store);
    switch ((pn_Store)get_Proj_num(node)) {
    case pn_Store_M:
        return new_store;
    default:
        TODO(node);
    }
}

TRANS_FUNC(Proj_Start) {
    ir_graph *const irg = get_irn_irg(node);
    unsigned const  pn  = get_Proj_num(node);
    switch ((pn_Start)pn) {
    case pn_Start_M:
        return be_get_Start_mem(irg);
    case pn_Start_T_args:
        return new_r_Bad(irg, mode_T);
    case pn_Start_P_frame_base:
        return get_Start_sp(irg);
    }
    TODO(node);
}

static void loongarch64_register_transformers(void) {
    be_start_transform_setup();

    // Const
    be_set_transform_function(op_Const, gen_Const);
    // Arithmetic
    be_set_transform_function(op_Add, gen_Add);
    be_set_transform_function(op_Sub, gen_Sub);
    be_set_transform_function(op_Mul, gen_Mul);
    be_set_transform_function(op_Mulh, gen_Mulh);
    be_set_transform_function(op_Div, gen_Div);
    be_set_transform_function(op_Mod, gen_Mod);
    be_set_transform_function(op_Minus, gen_Minus);
    be_set_transform_function(op_Shl, gen_Shl);
    be_set_transform_function(op_Shr, gen_Shr);
    be_set_transform_function(op_Shrs, gen_Shrs);
    // Bit manipulation
    be_set_transform_function(op_And, gen_And);
    be_set_transform_function(op_Or, gen_Or);
    be_set_transform_function(op_Eor, gen_Eor);
    be_set_transform_function(op_Not, gen_Not);
    // Conversion
    be_set_transform_function(op_Conv, gen_Conv);
    // Load/Store or Memory related
    be_set_transform_function(op_Load, gen_Load);
    be_set_transform_function(op_Store, gen_Store);
    be_set_transform_function(op_Address, gen_Address);
    be_set_transform_function(op_Member, gen_Member);
    // Compare or Conditional
    be_set_transform_function(op_Cmp, gen_Cmp);
    be_set_transform_function(op_Cond, gen_Cond);
    be_set_transform_function(op_Mux, gen_Mux);
    // Control flow
    be_set_transform_function(op_IJmp, gen_IJmp);
    be_set_transform_function(op_Jmp, gen_Jmp);
    be_set_transform_function(op_Switch, gen_Switch);
    be_set_transform_function(op_Call, gen_Call);
    be_set_transform_function(op_Return, gen_Return);
    // Misc
    be_set_transform_function(op_Phi, gen_Phi);
    be_set_transform_function(op_Start, gen_Start);
    be_set_transform_function(op_Unknown, gen_Unknown);
    // Projection
    be_set_transform_proj_function(op_Call, gen_Proj_Call);
    be_set_transform_proj_function(op_Div, gen_Proj_Div);
    be_set_transform_proj_function(op_Load, gen_Proj_Load);
    be_set_transform_proj_function(op_Mod, gen_Proj_Mod);
    be_set_transform_proj_function(op_Proj, gen_Proj_Proj);
    be_set_transform_proj_function(op_Start, gen_Proj_Start);
    be_set_transform_proj_function(op_Store, gen_Proj_Store);
}

/**
 * Transform generic IR-nodes into loongarch64 machine instructions
 */
void loongarch64_transform_graph(ir_graph *irg) {
    assure_irg_properties(irg, IR_GRAPH_PROPERTY_NO_TUPLES | IR_GRAPH_PROPERTY_NO_BADS);

    loongarch64_register_transformers();

    set_allocatable_regs(irg);
    be_stack_init(&stack_env);
    ir_entity *const fun_ent  = get_irg_entity(irg);
    ir_type *const   fun_type = get_entity_type(fun_ent);
    setup_calling_convention(&cconv, fun_type);
    layout_parameter_entities(&cconv, irg);
    be_add_parameter_entity_stores(irg);

    be_transform_graph(irg, NULL);

    free_calling_convention(&cconv);
    be_stack_finish(&stack_env);
}

void loongarch64_init_transform(void) { FIRM_DBG_REGISTER(dbg, "firm.be.loongarch64.transform"); }
