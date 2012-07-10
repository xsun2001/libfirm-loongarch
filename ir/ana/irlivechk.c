/*
 * Copyright (C) 1995-2007 Inria Rhone-Alpes.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file    livechk.c
 * @date    21.04.2007
 * @author  Sebastian Hack
 * @brief
 *
 * Liveness checks as developed by Benoit Boissinot, Fabrice Rastello and myself.
 *
 * The speciality here is, that nothing has to be recomputed if new nodes are created
 * or old ones deleted.
 *
 * This algo has one core routine check_live_end_internal() which performs the liveness check.
 * It only relies on the precomputation done in the constructor, which in turn needs:
 * - out edges
 * - the dominance tree
 * - data obtained from a depth-first-search
 *
 * The precomputation remains valid as long as the CFG is not altered.
 */
#include <config.h>

#include <stdio.h>

#include "irgraph_t.h"
#include "irnode_t.h"
#include "irnodemap.h"
#include "iredges_t.h"

#include "irprintf.h"
#include "irdom.h"
#include "irdump.h"

#include "dfs_t.h"
#include "bitset.h"
#include "util.h"

#include "irlivechk.h"

#include "statev.h"

typedef struct bl_info_t {
	const ir_node *block;      /**< The block. */

	int be_tgt_calc : 1;
	int id : 31;               /**< a tight number for the block.
								 we're just reusing the pre num from
								 the DFS. */
	bitset_t *red_reachable;   /**< Holds all id's if blocks reachable
								 in the CFG modulo back edges. */

	bitset_t *be_tgt_reach;    /**< target blocks of back edges whose
								 sources are reachable from this block
								 in the reduced graph. */
} bl_info_t;

struct lv_chk_t {
	ir_nodemap     block_infos;
	struct obstack obst;
	dfs_t         *dfs;
	int            n_blocks;
	bitset_t      *back_edge_src;
	bitset_t      *back_edge_tgt;
	bl_info_t    **map;
	DEBUG_ONLY(firm_dbg_module_t *dbg;)
};

static bl_info_t *get_block_info(lv_chk_t *lv, const ir_node *block)
{
	bl_info_t *info = (bl_info_t*)ir_nodemap_get(&lv->block_infos, block);
	if (info == NULL) {
		info                = OALLOC(&lv->obst, bl_info_t);
		info->id            = get_Block_dom_tree_pre_num(block);
		info->block         = block;
		info->red_reachable = bitset_obstack_alloc(&lv->obst, lv->n_blocks);
		info->be_tgt_reach  = bitset_obstack_alloc(&lv->obst, lv->n_blocks);
		info->be_tgt_calc   = 0;
		ir_nodemap_insert(&lv->block_infos, block, info);
	}
	return info;
}

/**
 * Filter function to select all nodes for which liveness is computed.
 * @param irn A node.
 * @return    1 if the node shall be considered in liveness, 0 if not.
 */
static inline int is_liveness_node(const ir_node *irn)
{
	switch (get_irn_opcode(irn)) {
	case iro_Block:
	case iro_Bad:
	case iro_End:
	case iro_Anchor:
		return 0;
	default:
		break;
	}

	return 1;
}

/**
 * Compute the transitive closure on the reduced graph.
 * The reduced graph is the original graph without back edges.
 * Since that is a DAG, a reverse post order of the graph gives a toposort
 * which is ideally suited to compute the transitive closure.
 * Note also, that the DFS tree of the reduced graph is the same than the one
 * of the original graph. This saves us computing a new reverse post order.
 * We also can re-use the DFS tree of the original graph.
 */
static void red_trans_closure(lv_chk_t *lv)
{
	int i, n;

	for (i = 0, n = dfs_get_n_nodes(lv->dfs); i < n; ++i) {
		const ir_node *bl = (const ir_node*) dfs_get_post_num_node(lv->dfs, i);
		bl_info_t *bi = get_block_info(lv, bl);

		const ir_edge_t *edge;

		bitset_set(bi->red_reachable, bi->id);
		foreach_block_succ (bl, edge) {
			ir_node *succ = get_edge_src_irn(edge);
			bl_info_t *si = get_block_info(lv, succ);
			dfs_edge_kind_t kind = dfs_get_edge_kind(lv->dfs, bl, succ);

			/*
			 * if the successor is no back edge, include all reachable
			 * blocks from there into the reachable set of the current node
			 */
			if (kind != DFS_EDGE_BACK) {
				assert(dfs_get_post_num(lv->dfs, bl) > dfs_get_post_num(lv->dfs, succ));
				bitset_or(bi->red_reachable, si->red_reachable);
			}

			/* mark the block as a back edge src and succ as back edge tgt. */
			else {
				bitset_set(lv->back_edge_src, bi->id);
				bitset_set(lv->back_edge_tgt, si->id);
			}
		}

	}

}

static void compute_back_edge_chain(lv_chk_t *lv, const ir_node *bl)
{
	bitset_t *tmp = bitset_alloca(lv->n_blocks);
	bl_info_t *bi = get_block_info(lv, bl);

	size_t elm;

	DBG((lv->dbg, LEVEL_2, "computing T_%d\n", bi->id));

	/* put all back edge sources reachable (reduced) from here in tmp */
	bitset_copy(tmp, bi->red_reachable);
	bitset_set(tmp, bi->id);
	bitset_and(tmp, lv->back_edge_src);
	bi->be_tgt_calc = 1;

	DBG((lv->dbg, LEVEL_2, "\treachable be src: %B\n", tmp));

	/* iterate over them ... */
	bitset_foreach(tmp, elm) {
		bl_info_t *si = lv->map[elm];
		const ir_edge_t *edge;

		/* and find back edge targets which are not reduced reachable from bl */
		foreach_block_succ (si->block, edge) {
			ir_node *tgt         = get_edge_src_irn(edge);
			bl_info_t *ti        = get_block_info(lv, tgt);
			dfs_edge_kind_t kind = dfs_get_edge_kind(lv->dfs, si->block, tgt);

			if (kind == DFS_EDGE_BACK && !bitset_is_set(bi->red_reachable, ti->id)) {
				if (!ti->be_tgt_calc)
					compute_back_edge_chain(lv, tgt);
				bitset_set(bi->be_tgt_reach, ti->id);
				bitset_or(bi->be_tgt_reach, ti->be_tgt_reach);
			}
		}
		bitset_clear(bi->be_tgt_reach, bi->id);
	}
}


static inline void compute_back_edge_chains(lv_chk_t *lv)
{
	size_t elm;
	int i, n;

	DBG((lv->dbg, LEVEL_2, "back edge sources: %B\n", lv->back_edge_src));
	bitset_foreach(lv->back_edge_src, elm) {
		compute_back_edge_chain(lv, lv->map[elm]->block);
	}

	for (i = 0, n = dfs_get_n_nodes(lv->dfs); i < n; ++i) {
		const ir_node *bl = (const ir_node*) dfs_get_post_num_node(lv->dfs, i);
		bl_info_t *bi     = get_block_info(lv, bl);

		const ir_edge_t *edge;

		if (!bitset_is_set(lv->back_edge_tgt, bi->id)) {
			foreach_block_succ (bl, edge) {
				ir_node *succ = get_edge_src_irn(edge);
				bl_info_t *si = get_block_info(lv, succ);
				dfs_edge_kind_t kind = dfs_get_edge_kind(lv->dfs, bl, succ);

				if (kind != DFS_EDGE_BACK) {
					assert(dfs_get_post_num(lv->dfs, bl) > dfs_get_post_num(lv->dfs, succ));
					bitset_or(bi->be_tgt_reach, si->be_tgt_reach);
				}
			}
		}
	}

	for (i = 0, n = dfs_get_n_nodes(lv->dfs); i < n; ++i) {
		const ir_node *bl = (const ir_node*) dfs_get_post_num_node(lv->dfs, i);
		bl_info_t *bi     = get_block_info(lv, bl);
		bitset_set(bi->be_tgt_reach, bi->id);
	}
}

lv_chk_t *lv_chk_new(ir_graph *irg)
{
	lv_chk_t *res = XMALLOC(lv_chk_t);
	int i;

	assure_doms(irg);

	stat_ev_tim_push();
	ir_nodemap_init(&res->block_infos, irg);
	obstack_init(&res->obst);

	FIRM_DBG_REGISTER(res->dbg, "ir.ana.lvchk");

	res->dfs           = dfs_new(&absgraph_irg_cfg_succ, irg);
	res->n_blocks      = dfs_get_n_nodes(res->dfs);
	res->back_edge_src = bitset_obstack_alloc(&res->obst, res->n_blocks);
	res->back_edge_tgt = bitset_obstack_alloc(&res->obst, res->n_blocks);
	res->map           = OALLOCNZ(&res->obst, bl_info_t*, res->n_blocks);

	/* fill the map which maps pre_num to block infos */
	for (i = res->n_blocks - 1; i >= 0; --i) {
		ir_node *irn  = (ir_node *) dfs_get_pre_num_node(res->dfs, i);
		bl_info_t *bi = get_block_info(res, irn);
		assert(bi->id < res->n_blocks);
		assert(res->map[bi->id] == NULL);
		res->map[bi->id] = bi;
	}

	/* first of all, compute the transitive closure of the CFG *without* back edges */
	red_trans_closure(res);

	/* compute back edge chains */
	compute_back_edge_chains(res);

#ifndef NDEBUG
	DBG((res->dbg, LEVEL_1, "liveness chk in %+F\n", irg));
	for (i = res->n_blocks - 1; i >= 0; --i) {
		const ir_node *irn = (const ir_node*) dfs_get_pre_num_node(res->dfs, i);
		bl_info_t *bi      = get_block_info(res, irn);
		DBG((res->dbg, LEVEL_1, "lv_chk for %d -> %+F\n", i, irn));
		DBG((res->dbg, LEVEL_1, "\tred reach: %B\n", bi->red_reachable));
		DBG((res->dbg, LEVEL_1, "\ttgt reach: %B\n", bi->be_tgt_reach));
	}
#endif

	DBG((res->dbg, LEVEL_1, "back edge src: %B\n", res->back_edge_src));
	DBG((res->dbg, LEVEL_1, "back edge tgt: %B\n", res->back_edge_tgt));

	stat_ev_tim_pop("lv_chk_cons_time");
	return res;
}

void lv_chk_free(lv_chk_t *lv)
{
	dfs_free(lv->dfs);
	obstack_free(&lv->obst, NULL);
	ir_nodemap_destroy(&lv->block_infos);
	xfree(lv);
}

unsigned lv_chk_bl_xxx(lv_chk_t *lv, const ir_node *bl, const ir_node *var)
{
	int res  = 0;
	ir_node *def_bl;
	stat_ev_cnt_decl(uses);
	stat_ev_cnt_decl(iter);

	assert(is_Block(bl) && "can only check for liveness in a block");

	/* If the variable ist no liveness related var, bail out. */
	if (!is_liveness_node(var))
		return 0;

	stat_ev_ctx_push_fmt("lv_chk", "%u", get_irn_idx(var));
	stat_ev_tim_push();

	/* If there is no dominance relation, go out, too */
	def_bl = get_nodes_block(var);
	if (!block_dominates(def_bl, bl)) {
		stat_ev("lv_chk_no_dom");
		goto end;
	}

	/*
	 * If the block in question is the same as the definition block,
	 * the algorithm is simple. Just check for uses not inside this block.
	 */
	if (def_bl == bl) {
		const ir_edge_t *edge;

		stat_ev("lv_chk_def_block");
		DBG((lv->dbg, LEVEL_2, "lv check same block %+F in %+F\n", var, bl));
		foreach_out_edge (var, edge) {
			ir_node *use    = get_edge_src_irn(edge);
			ir_node *use_bl;

			if (!is_liveness_node(use))
				continue;

			stat_ev_cnt_inc(uses);
			use_bl = get_nodes_block(use);
			if (is_Phi(use)) {
				int pos = get_edge_src_pos(edge);
				use_bl  = get_Block_cfgpred_block(use_bl, pos);

				if (use_bl == bl) {
					DBG((lv->dbg, LEVEL_2, "\tphi %+F in succ %+F,%d -> live end\n", use, use_bl, pos));
					res |= lv_chk_state_end;
				}
			}

			if (use_bl != def_bl) {
				res = lv_chk_state_end | lv_chk_state_out;
				goto end;
			}
		}

		goto end;
	}

	/*
	 * this is the more complicated case.
	 * We try to gather as much information as possible during looking
	 * at the uses.
	 *
	 * Note that we know for sure that bl != def_bl. That is sometimes
	 * silently exploited below.
	 */
	else {
		bl_info_t *def = get_block_info(lv, def_bl);
		bl_info_t *bli = get_block_info(lv, bl);
		bitset_t *uses = bitset_alloca(lv->n_blocks);
		bitset_t *Tq;

		size_t i;
		unsigned min_dom, max_dom;
		const ir_edge_t *edge;

		/* if the block has no DFS info, it cannot be reached.
		 * This can happen in functions with endless loops.
		 * we then go out, since nothing is live there.
		 *
		 * TODO: Is that right?
		 */
		if (!bli)
			goto end;

		(void) def;
		DBG((lv->dbg, LEVEL_2, "lv check %+F (def in %+F #%d) in different block %+F #%d\n",
					var, def_bl, def->id, bl, bli->id));

		foreach_out_edge (var, edge) {
			ir_node *user = get_edge_src_irn(edge);
			int mask      = lv_chk_state_in;

			ir_node *use_bl;
			bl_info_t *bi;

			/* if the user is no liveness node, the use does not count */
			if (!is_liveness_node(user))
				continue;

			stat_ev_cnt_inc(uses);

			/* if the user is a phi, the use is in the predecessor
			 * furthermore, prepare a mask so that in the case where
			 * bl (the block in question) coincides with a use, it
			 * can be marked live_end there. */
			use_bl = get_nodes_block(user);
			if (is_Phi(user)) {
				int pos = get_edge_src_pos(edge);
				use_bl  = get_Block_cfgpred_block(use_bl, pos);
				mask   |= lv_chk_state_end;
			}


			/* if the use block coincides with the query block, we
			 * already gather a little liveness information.
			 * The variable is surely live there, since bl != def_bl
			 * (that case is treated above). */
			if (use_bl == bl)
				res |= mask;

			bi = get_block_info(lv, use_bl);

			if (bi)
				bitset_set(uses, bi->id);
		}

		/* get the dominance range which really matters. all uses outside
		 * the definition's dominance range are not to consider. note,
		 * that the definition itself is also not considered. The case
		 * where bl == def_bl is considered above. */
		min_dom = get_Block_dom_tree_pre_num(def_bl) + 1;
		max_dom = get_Block_dom_max_subtree_pre_num(def_bl);

		DBG((lv->dbg, LEVEL_2, "\tuses: %B\n", uses));

		/* prepare a set with all reachable back edge targets.
		 * this will determine our "looking points" from where
		 * we will search/find the calculated uses. */
		Tq = bli->be_tgt_reach;

		/* now, visit all viewing points in the temporary bitset lying
		 * in the dominance range of the variable. Note that for reducible
		 * flow-graphs the first iteration is sufficient and the loop
		 * will be left. */
		DBG((lv->dbg, LEVEL_2, "\tbe tgt reach: %B, dom span: [%d, %d]\n", Tq, min_dom, max_dom));
		i = bitset_next_set(Tq, min_dom);
		while (i <= max_dom) {
			bl_info_t *ti = lv->map[i];
			int use_in_current_block = bitset_is_set(uses, ti->id);

			stat_ev_cnt_inc(iter);

			/*
			 * This is somewhat tricky. Since this routine handles both, live in
			 * and end/out we have to handle all the border cases correctly.
			 * Each node is in its own red_reachable set (see calculation
			 * function above). That means, that in the case where bl == t, the
			 * intersection check of uses and reachability below will always
			 * find an intersection, namely t.
			 *
			 * However, if a block contains a use and the variable is dead
			 * afterwards, it is not live end/out at that block. Besides
			 * back-edge target. If a var is live-in at a back-edge target it
			 * is also live out/end there since the variable is live in the
			 * underlying loop. So in the case where t == bl and that is not
			 * a back-edge target, we have to remove that use from consideration
			 * to determine if the var is live out/end there.
			 *
			 * Note that the live in information has been calculated by the
			 * uses iteration above.
			 */
			if (ti == bli && !bitset_is_set(lv->back_edge_tgt, ti->id)) {
				DBG((lv->dbg, LEVEL_2, "\tlooking not from a back edge target and q == t. removing use: %d\n", ti->id));
				bitset_clear(uses, ti->id);
			}

			/* If we can reach a use, the variable is live there and we say goodbye */
			DBG((lv->dbg, LEVEL_2, "\tlooking from %d: seeing %B\n", ti->id, ti->red_reachable));
			if (bitset_intersect(ti->red_reachable, uses)) {
				res |= lv_chk_state_in | lv_chk_state_out | lv_chk_state_end;
				goto end;
			}

			/*
			 * if we deleted a use do to the commentary above, we have to
			 * re-add it since it might be visible from further view points
			 * (we only need that in the non-reducible case).
			 */
			if (use_in_current_block)
				bitset_set(uses, ti->id);

			i = bitset_next_set(Tq, get_Block_dom_max_subtree_pre_num(ti->block) + 1);
		}

	}

end:
	stat_ev_tim_pop("lv_chk_query_time");
	stat_ev_cnt_done(uses, "lv_chk_uses");
	stat_ev_cnt_done(iter, "lv_chk_iter");
	stat_ev_ctx_pop("lv_chk");

	return res;
}
