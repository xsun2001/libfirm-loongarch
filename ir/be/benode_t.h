/**
 * @file   benode_t.h
 * @date   17.05.2005
 * @author Sebastian Hack
 *
 * Backend node support.
 *
 * Copyright (C) 2005 Universitaet Karlsruhe
 * Released under the GPL
 */

#ifndef _BENODE_T_H
#define _BENODE_T_H

#include "firm_config.h"
#include "pmap.h"

#include "irmode.h"
#include "irnode.h"
#include "entity_t.h"

#include "be_t.h"
#include "bearch.h"

#define BE_SPILL_NO_OFFSET ((unsigned) -1)

typedef enum {
	beo_NoBeOp = 0,
	beo_Spill,
	beo_Reload,
	beo_Perm,
	beo_Copy,
	beo_Keep,
	beo_Last
} be_opcode_t;

void be_node_init(void);

const arch_irn_handler_t be_node_irn_handler;

ir_node *be_new_Spill(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, ir_node *node_to_spill, ir_node *ctx);
ir_node *be_new_Reload(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, ir_mode *mode, ir_node *spill_node);
ir_node *be_new_Copy(const arch_register_class_t *cls, ir_graph *irg, ir_node *block, ir_node *in);
ir_node *be_new_Perm(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, int arity, ir_node *in[]);
ir_node *be_new_Keep(const arch_register_class_t *cls, ir_graph *irg, ir_node *bl, int arity, ir_node *in[]);

ir_node *be_spill(const arch_env_t *arch_env, ir_node *irn,ir_node *spill_ctx);
ir_node *be_reload(const arch_env_t *arch_env, const arch_register_class_t *cls, ir_node *irn, int pos, ir_mode *mode, ir_node *spill);

int be_is_Spill(const ir_node *irn);
int be_is_Reload(const ir_node *irn);
int be_is_Copy(const ir_node *irn);
int be_is_Perm(const ir_node *irn);
int be_is_Keep(const ir_node *irn);

void   be_set_Spill_entity(ir_node *irn, entity *ent);
entity *be_get_spill_entity(ir_node *irn);

ir_node *be_get_Spill_context(const ir_node *irn);


/**
 * Modify the output register requirements of a Perm.
 * This function incur register constraints to an output value of a Perm.
 * This is used when handling register constraints in general,
 * see beconstrperm.c
 */
void be_set_Perm_out_req(ir_node *irn, int pos, const arch_register_req_t *req);

/**
 * Insert a Perm node after a specific node in the schedule.
 * The Perm permutes over all values live at the given node.
 * This means that all liveness intervals are cut apart at this
 * location in the program.
 */
ir_node *insert_Perm_after(const arch_env_t *env,
						   const arch_register_class_t *cls,
						   dom_front_info_t *dom_front,
						   ir_node *pos);

#endif /* _BENODE_T_H */
