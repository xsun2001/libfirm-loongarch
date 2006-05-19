
/**
 * More experiments on coalescing.
 * @author Sebastian Hack
 * @date   14.04.2006
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WITH_LIBCORE
#include <libcore/lc_opts.h>
#include <libcore/lc_opts_enum.h>
#endif /* WITH_LIBCORE */

#include <stdlib.h>
#include <limits.h>

#include "list.h"
#include "pdeq.h"
#include "bitset.h"

#include "debug.h"
#include "bitfiddle.h"

#include "irphase_t.h"
#include "irgraph_t.h"
#include "irnode_t.h"
#include "irprintf.h"
#include "irtools.h"

#include "beabi.h"
#include "benode_t.h"
#include "becopyopt.h"
#include "becopyopt_t.h"
#include "bechordal_t.h"

#define DUMP_BEFORE 1
#define DUMP_AFTER  2
#define DUMP_CLOUD  4
#define DUMP_ALL    2 * DUMP_CLOUD - 1

static int    dump_flags      = 0;
static double stop_percentage = 1.0;

/* Options using libcore */
#ifdef WITH_LIBCORE

static const lc_opt_enum_mask_items_t dump_items[] = {
	{ "before",  DUMP_BEFORE },
    { "after",   DUMP_AFTER  },
    { "cloud",   DUMP_CLOUD  },
	{ "all",     DUMP_ALL    },
	{ NULL,      0 }
};

static lc_opt_enum_mask_var_t dump_var = {
	&dump_flags, dump_items
};

static const lc_opt_table_entry_t options[] = {
	LC_OPT_ENT_ENUM_MASK("dump", "dump ifg before, after or after each cloud",                     &dump_var),
	LC_OPT_ENT_DBL      ("stop", "stop optimizing cloud at given percentage of total cloud costs", &stop_percentage),
	{ NULL }
};

void be_co2_register_options(lc_opt_entry_t *grp)
{
	lc_opt_entry_t *co2_grp = lc_opt_get_grp(grp, "co2");
	lc_opt_add_table(co2_grp, options);
}
#endif

/*
  ____  _             _
 / ___|| |_ __ _ _ __| |_
 \___ \| __/ _` | '__| __|
  ___) | || (_| | |  | |_
 |____/ \__\__,_|_|   \__|

*/

#define INFEASIBLE(cost) ((cost) == INT_MAX)

static be_ifg_dump_dot_cb_t ifg_dot_cb;

typedef unsigned col_t;

typedef struct _co2_irn_t       co2_irn_t;
typedef struct _co2_cloud_t     co2_cloud_t;
typedef struct _co2_cloud_irn_t co2_cloud_irn_t;

typedef struct {
	col_t col;
	int costs;
} col_cost_pair_t;

typedef struct {
	phase_t     ph;
	copy_opt_t *co;
	bitset_t   *ignore_regs;
	co2_irn_t  *touched;
	int         visited;
	int         n_regs;
	struct list_head cloud_head;
	DEBUG_ONLY(firm_dbg_module_t *dbg;)
} co2_t;

struct _co2_irn_t {
	ir_node         *irn;
	affinity_node_t *aff;
	co2_irn_t       *touched_next;
	col_t            tmp_col;
	col_t            orig_col;
	int				 last_color_change;
	bitset_t        *adm_cache;
	unsigned         fixed     : 1;
	unsigned         tmp_fixed : 1;
	struct list_head changed_list;
};

struct _co2_cloud_irn_t {
	struct _co2_irn_t  inh;
	co2_cloud_t       *cloud;
	int                visited;
	int                index;
	co2_cloud_irn_t   *mst_parent;
	int                mst_costs;
	int                mst_n_childs;
	co2_cloud_irn_t  **mst_childs;
	int               *col_costs;
	int                costs;
	int               *fronts;
	int               *color_badness;
	col_cost_pair_t   *tmp_coloring;
	struct list_head   cloud_list;
	struct list_head   mst_list;
};

struct _co2_cloud_t {
	co2_t            *env;
	struct obstack    obst;
	int               costs;
	int               mst_costs;
	int               inevit;
	int               best_costs;
	int               n_memb;
	int               max_degree;
	int			      ticks;
	co2_cloud_irn_t  *master;
	co2_cloud_irn_t  *mst_root;
	co2_cloud_irn_t **seq;
	struct list_head  members_head;
	struct list_head  list;
};

#define FRONT_BASE(ci,col)  ((ci)->fronts + col * (ci)->mst_n_childs)

#define get_co2_irn(co2, irn)         ((co2_irn_t *)       phase_get_or_set_irn_data(&co2->ph, irn))
#define get_co2_cloud_irn(co2, irn)   ((co2_cloud_irn_t *) phase_get_or_set_irn_data(&co2->ph, irn))

static void *co2_irn_init(phase_t *ph, ir_node *irn, void *data)
{
	co2_t *env         = (co2_t *) ph;
	affinity_node_t *a = get_affinity_info(env->co, irn);
	size_t size        = a ? sizeof(co2_cloud_irn_t) : sizeof(co2_irn_t);
	co2_irn_t *ci      = data ? data : phase_alloc(ph, size);

	memset(ci, 0, size);
	INIT_LIST_HEAD(&ci->changed_list);
	ci->touched_next = env->touched;
	ci->orig_col     = get_irn_col(env->co, irn);
	env->touched     = ci;
	ci->irn          = irn;
	ci->aff          = a;

	if(a) {
		co2_cloud_irn_t *cci = (co2_cloud_irn_t *) ci;
		INIT_LIST_HEAD(&cci->cloud_list);
		cci->mst_parent   = cci;
	}

	return ci;
}


static int cmp_clouds_gt(const void *a, const void *b)
{
	const co2_cloud_t **p = a;
	const co2_cloud_t **q = b;
	int c = (*p)->costs;
	int d = (*q)->costs;
	return QSORT_CMP(d, c);
}

/**
 * An order on color/costs pairs.
 * If the costs are equal, we use the color as a kind of normalization.
 */
static int col_cost_pair_lt(const void *a, const void *b)
{
	const col_cost_pair_t *p = a;
	const col_cost_pair_t *q = b;
	int c = p->costs;
	int d = q->costs;
	return QSORT_CMP(c, d);
}

static col_t get_col(co2_t *env, ir_node *irn)
{
	co2_irn_t *ci = get_co2_irn(env, irn);
	return ci->tmp_fixed ? ci->tmp_col : ci->orig_col;
}

static INLINE int color_is_fix(co2_t *env, ir_node *irn)
{
	co2_irn_t *ci = get_co2_irn(env, irn);
	return ci->fixed || ci->tmp_fixed;
}

static INLINE bitset_t *get_adm(co2_t *env, co2_irn_t *ci)
{
	if(!ci->adm_cache) {
		arch_register_req_t req;
		ci->adm_cache = bitset_obstack_alloc(phase_obst(&env->ph), env->n_regs);
		arch_get_register_req(env->co->aenv, &req, ci->irn, BE_OUT_POS(0));
		if(arch_register_req_is(&req, limited))
			req.limited(req.limited_env, ci->adm_cache);
		else {
			bitset_copy(ci->adm_cache, env->ignore_regs);
			bitset_flip_all(ci->adm_cache);
		}
	}

	return ci->adm_cache;
}

static bitset_t *admissible_colors(co2_t *env, co2_irn_t *ci, bitset_t *bs)
{
	bitset_copy(bs, get_adm(env, ci));
	return bs;
}

static int is_color_admissible(co2_t *env, co2_irn_t *ci, col_t col)
{
	bitset_t *bs = get_adm(env, ci);
	return bitset_is_set(bs, col);
}

static void incur_constraint_costs(co2_t *env, ir_node *irn, col_cost_pair_t *col_costs, int costs)
{
	bitset_t *aux = bitset_alloca(env->co->cls->n_regs);
	arch_register_req_t req;

	arch_get_register_req(env->co->aenv, &req, irn, BE_OUT_POS(0));

	if(arch_register_req_is(&req, limited)) {
		bitset_pos_t elm;
		int n_constr;

		req.limited(req.limited_env, aux);
		n_constr = bitset_popcnt(aux);
		bitset_foreach(aux, elm) {
			col_costs[elm].costs  = add_saturated(col_costs[elm].costs, costs / n_constr);
		}
	}
}

/**
 * Determine costs which shall indicate how cheap/expensive it is to try
 * to assign a node some color.
 * The costs are computed for all colors. INT_MAX means that it is impossible
 * to give the node that specific color.
 *
 * @param env       The co2 this pointer.
 * @param irn       The node.
 * @param col_costs An array of colors x costs where the costs are written to.
 */
static void determine_color_costs(co2_t *env, co2_irn_t *ci, col_cost_pair_t *col_costs)
{
	ir_node *irn       = ci->irn;
	be_ifg_t *ifg      = env->co->cenv->ifg;
	int n_regs         = env->co->cls->n_regs;
	bitset_t *forb     = bitset_alloca(n_regs);
	affinity_node_t *a = ci->aff;

	bitset_pos_t elm;
	ir_node *pos;
	void *it;
	int i;

	/* Put all forbidden colors into the aux bitset. */
	admissible_colors(env, ci, forb);
	bitset_flip_all(forb);

	for(i = 0; i < n_regs; ++i) {
		col_costs[i].col   = i;
		col_costs[i].costs = 0;
	}

	if(a) {
		neighb_t *n;

		co_gs_foreach_neighb(a, n) {
			if(color_is_fix(env, n->irn)) {
				col_t col = get_col(env, n->irn);
				col_costs[col].costs = add_saturated(col_costs[col].costs, -n->costs * 128);
			}

			incur_constraint_costs(env, n->irn, col_costs, -n->costs);
		}
	}

	it = be_ifg_neighbours_iter_alloca(ifg);
	be_ifg_foreach_neighbour(ifg, it, irn, pos) {
		col_t col = get_col(env, pos);
		if(color_is_fix(env, pos)) {
			col_costs[col].costs  = INT_MAX;
		}
		else {
			incur_constraint_costs(env, pos, col_costs, INT_MAX);
			col_costs[col].costs = add_saturated(col_costs[col].costs, 8 * be_ifg_degree(ifg, pos));
		}
	}

	/* Set the costs to infinity for each color which is not allowed at this node. */
	bitset_foreach(forb, elm) {
		col_costs[elm].costs  = INT_MAX;
	}

}

static void single_color_cost(co2_t *env, co2_irn_t *ci, col_t col, col_cost_pair_t *seq)
{
	int n_regs = env->co->cls->n_regs;
	int i;

	for(i = 0; i < n_regs; ++i) {
		seq[i].col   = i;
		seq[i].costs = INT_MAX;
	}

	assert(is_color_admissible(env, ci, col));
	seq[col].col = 0;
	seq[0].col   = col;
	seq[0].costs = 0;
}

static void reject_coloring(struct list_head *h)
{
	co2_irn_t *pos;

	list_for_each_entry(co2_irn_t, pos, h, changed_list)
		pos->tmp_fixed = 0;
}

static void materialize_coloring(struct list_head *h)
{
	co2_irn_t *pos;

	list_for_each_entry(co2_irn_t, pos, h, changed_list) {
		pos->orig_col  = pos->tmp_col;
		pos->tmp_fixed = 0;
	}
}

typedef struct {
	co2_irn_t *ci;
	col_t col;
} col_entry_t;

static col_entry_t *save_coloring(struct obstack *obst, struct list_head *changed)
{
	co2_irn_t *pos;
	col_entry_t ent;

	list_for_each_entry(co2_irn_t, pos, changed, changed_list) {
		ent.ci  = pos;
		ent.col = pos->tmp_col;
		pos->tmp_col = 0;
		obstack_grow(obst, &ent, sizeof(ent));
	}
	memset(&ent, 0, sizeof(ent));
	obstack_grow(obst, &ent, sizeof(ent));
	return obstack_finish(obst);
}

static int change_color_not(co2_t *env, ir_node *irn, col_t not_col, struct list_head *parent_changed, int depth);
static int change_color_single(co2_t *env, ir_node *irn, col_t tgt_col, struct list_head *parent_changed, int depth);

static int recolor(co2_t *env, ir_node *irn, col_cost_pair_t *col_list, struct list_head *parent_changed, int depth)
{
	int n_regs         = env->co->cls->n_regs;
	be_ifg_t *ifg      = env->co->cenv->ifg;
	co2_irn_t *ci      = get_co2_irn(env, irn);
	int res            = 0;
	int n_aff          = 0;

	int i;

	for(i = 0; i < n_regs; ++i) {
		col_t tgt_col  = col_list[i].col;
		unsigned costs = col_list[i].costs;
		int neigh_ok   = 1;

		struct list_head changed;
		ir_node *n;
		void *it;

		DBG((env->dbg, LEVEL_3, "\t\t%2{firm:indent}trying color %d(%d) on %+F\n", depth, tgt_col, costs, irn));

		/* If the costs for that color (and all successive) are infinite, bail out we won't make it anyway. */
		if(INFEASIBLE(costs)) {
			DB((env->dbg, LEVEL_4, "\t\t%2{firm:indent}color %d infeasible\n", depth, tgt_col));
			ci->tmp_fixed = 0;
			return 0;
		}

		/* Set the new color of the node and mark the node as temporarily fixed. */
		ci->tmp_col     = tgt_col;
		ci->tmp_fixed   = 1;

		/*
		If that color has costs > 0, there's at least one neighbor having that color,
		so we will try to change the neighbors' colors, too.
		*/
		INIT_LIST_HEAD(&changed);
		list_add(&ci->changed_list, &changed);

		it = be_ifg_neighbours_iter_alloca(ifg);
		be_ifg_foreach_neighbour(ifg, it, irn, n) {

			/* try to re-color the neighbor if it has the target color. */
			if(get_col(env, n) == tgt_col) {
				struct list_head tmp;

				/*
				Try to change the color of the neighbor and record all nodes which
				get changed in the tmp list. Add this list to the "changed" list for
				that color. If we did not succeed to change the color of the neighbor,
				we bail out and try the next color.
				*/
				INIT_LIST_HEAD(&tmp);
				neigh_ok = change_color_not(env, n, tgt_col, &tmp, depth + 1);
				list_splice(&tmp, &changed);
				if(!neigh_ok)
					break;
			}
		}

		/*
		We managed to assign the target color to all neighbors, so from the perspective
		of the current node, every thing was ok and we can return safely.
		*/
		if(neigh_ok) {
			DBG((env->dbg, LEVEL_3, "\t\t%2{firm:indent}color %d(%d) was ok\n", depth, tgt_col, costs));
			list_splice(&changed, parent_changed);
			res = 1;
			break;
		}

		/*
		If not, that color did not succeed and we unfix all nodes we touched
		by traversing the changed list and setting tmp_fixed to 0 for these nodes.
		*/
		else
			reject_coloring(&changed);
	}

	return res;
}

static int change_color_not(co2_t *env, ir_node *irn, col_t not_col, struct list_head *parent_changed, int depth)
{
	co2_irn_t *ci = get_co2_irn(env, irn);
	int res       = 0;
	col_t col     = get_col(env, irn);

	DBG((env->dbg, LEVEL_3, "\t\t%2{firm:indent}clearing %+F(%d) of color %d\n", depth, irn, col, not_col));

	/* the node does not have to forbidden color. That's fine, mark it as visited and return. */
	if(col != not_col) {
		if(!ci->tmp_fixed) {
			ci->tmp_col     = col;
			ci->tmp_fixed   = 1;
		}

		list_add(&ci->changed_list, parent_changed);
		return 1;
	}

	/* The node has the color it should not have _and_ has not been visited yet. */
	if(!color_is_fix(env, irn)) {
		int n_regs            = env->co->cls->n_regs;
		col_cost_pair_t *csts = alloca(n_regs * sizeof(csts[0]));

		/* Get the costs for giving the node a specific color. */
		determine_color_costs(env, ci, csts);

		/* Since the node must not have the not_col, set the costs for that color to "infinity" */
		csts[not_col].costs = INT_MAX;

		/* sort the colors according costs, cheapest first. */
		qsort(csts, n_regs, sizeof(csts[0]), col_cost_pair_lt);

		/* Try recoloring the node using the color list. */
		res = recolor(env, irn, csts, parent_changed, depth);
	}

	/* If we came here, everything went ok. */
	return res;
}

static int change_color_single(co2_t *env, ir_node *irn, col_t tgt_col, struct list_head *parent_changed, int depth)
{
	co2_irn_t *ci = get_co2_irn(env, irn);
	col_t col     = get_col(env, irn);
	int res       = 0;

	DBG((env->dbg, LEVEL_3, "\t\t%2{firm:indent}trying to set %+F(%d) to color %d\n", depth, irn, col, tgt_col));

	/* the node has the wanted color. That's fine, mark it as visited and return. */
	if(col == tgt_col) {
		if(!ci->tmp_fixed) {
			ci->tmp_col     = col;
			ci->tmp_fixed   = 1;
			list_add(&ci->changed_list, parent_changed);
		}

		res = 1;
		goto end;
	}

	if(!color_is_fix(env, irn) && is_color_admissible(env, ci, tgt_col)) {
		int n_regs           = env->co->cls->n_regs;
		col_cost_pair_t *seq = alloca(n_regs * sizeof(seq[0]));

		/* Get the costs for giving the node a specific color. */
		single_color_cost(env, ci, tgt_col, seq);

		/* Try recoloring the node using the color list. */
		res = recolor(env, irn, seq, parent_changed, depth);

	}

end:
	DB((env->dbg, LEVEL_3, "\t\t%2{firm:indent}color %d %s for %+F\n", depth, tgt_col, res ? "was ok" : "failed", irn));
	return res;
}

static void front_inval_color(co2_cloud_irn_t *ci, col_t col)
{
	int *base = FRONT_BASE(ci, col);
	memset(base, -1, ci->mst_n_childs * sizeof(base[0]));
}

typedef struct {
	co2_cloud_irn_t *src, *tgt;
	int costs;
} edge_t;

int cmp_edges(const void *a, const void *b)
{
	const edge_t *p = a;
	const edge_t *q = b;
	return QSORT_CMP(p->costs, q->costs);
}

static co2_cloud_irn_t *find_mst_root(co2_cloud_irn_t *ci)
{
	while(ci->mst_parent != ci->mst_parent)
		ci = ci->mst_parent;
	return ci;
}


static int cmp_parent(const void *a, const void *b)
{
	const co2_cloud_irn_t *p = a;
	const co2_cloud_irn_t *q = b;
	return QSORT_CMP(q->mst_costs, p->mst_costs);
}

static void fill_tmp_coloring(co2_cloud_irn_t *ci, col_t col)
{
	int n_regs = ci->cloud->env->n_regs;
	int i, j;

	for(i = 0; i < ci->mst_n_childs; ++i) {
		co2_cloud_irn_t *c = ci->mst_childs[i];
		for(j = 0; j < n_regs; ++j) {
			int costs = c->col_costs[j];
			if(INFEASIBLE(costs))
				c->tmp_coloring[j].costs = INT_MAX;
			else {
				int add = j != (int) col ? c->mst_costs : 0;
				c->tmp_coloring[j].costs = add + costs;
			}
			c->tmp_coloring[j].col = j;
		}
		qsort(c->tmp_coloring, n_regs, sizeof(c->tmp_coloring[0]), col_cost_pair_lt);
	}
}

static void determine_start_colors(co2_cloud_irn_t *ci, col_cost_pair_t *seq)
{
	int n_regs    = ci->cloud->env->n_regs;
	bitset_t *adm = bitset_alloca(n_regs);
	int i, j;

	// TODO: Prefer some colors depending on the neighbors, etc.

	admissible_colors(ci->cloud->env, &ci->inh, adm);
	for(i = 0; i < n_regs; ++i) {
		seq[i].col   = i;

		if (!bitset_is_set(adm, i))
			seq[i].costs = INT_MAX;
		else {
			seq[i].costs = 0;
			for(j = 0; j < ci->mst_n_childs; ++j) {
				co2_cloud_irn_t *child = ci->mst_childs[j];
				if (!INFEASIBLE(child->col_costs[i]))
					seq[i].costs -= ci->mst_childs[j]->col_costs[i];
			}
		}
	}

	qsort(seq, n_regs, sizeof(seq[0]), col_cost_pair_lt);
}

static int push_front(co2_cloud_irn_t *ci, int *front)
{
	co2_t *env   = ci->cloud->env;
	int n_regs   = env->n_regs;
	int min_diff = INT_MAX;
	int min_chld = -1;
	int i;

	for(i = 0; i < ci->mst_n_childs; ++i) {
		co2_cloud_irn_t *child = ci->mst_childs[i];
		int idx = front[i];


		if(idx + 1 < n_regs) {
			int diff = child->tmp_coloring[idx].costs - child->tmp_coloring[idx + 1].costs;
			if(diff < min_diff) {
				min_diff = diff;
				min_chld = i;
			}
		}
	}

	if(min_chld >= 0) {
		co2_cloud_irn_t *child = ci->mst_childs[min_chld];
		DBG((env->dbg, LEVEL_3, "\tsmallest diff with child %+F on index %d is %d\n", child->inh.irn, front[min_chld], min_diff));
		front[min_chld] += 1;
	}

	return min_chld;
}

static int color_subtree(co2_cloud_irn_t *ci, col_t col, struct list_head *changed, int depth)
{
	int n_childs = ci->mst_n_childs;
	/*
		select the front for the given color.
		The front will determine the colors of the children.
	*/
	int *front = FRONT_BASE(ci, col);
	int i, ok = 1;

	ok = change_color_single(ci->cloud->env, ci->inh.irn, col, changed, 0);
	for(i = 0; i < n_childs && ok; ++i) {
		co2_cloud_irn_t *child = ci->mst_childs[i];
		col_t col              = front[i];

		ok = color_subtree(child, col, changed, depth + 1);
	}

	return ok;
}

static int try_coloring(co2_cloud_irn_t *ci, col_t col, int *front, int *initial_ok, int depth)
{
	co2_t *env = ci->cloud->env;
	struct list_head changed;
	int i, ok = 1;

	INIT_LIST_HEAD(&changed);
	*initial_ok = ok = change_color_single(env, ci->inh.irn, col, &changed, depth + 1);

	for (i = 0; i < ci->mst_n_childs && ok; ++i) {
		co2_cloud_irn_t *child = ci->mst_childs[i];
		col_t tgt_col = child->tmp_coloring[front[i]].col;

		ok = color_subtree(child, tgt_col, &changed, depth + 1);
	}

	reject_coloring(&changed);

	return ok;
}

static int examine_subtree_coloring(co2_cloud_irn_t *ci, col_t col)
{
	int *front = FRONT_BASE(ci, col);
	int cost   = 0;
	int i;

	for(i = 0; i < ci->mst_n_childs; ++i) {
		co2_cloud_irn_t *chld = ci->mst_childs[i];
		col_t chld_col        = front[i];

		cost += examine_subtree_coloring(chld, chld_col);
		cost += col != chld_col ? chld->mst_costs : 0;
	}

	return cost;
}

static int cloud_mst_build_colorings(co2_cloud_irn_t *ci, int depth)
{
	co2_t *env           = ci->cloud->env;
	int n_regs           = env->n_regs;
	col_cost_pair_t *seq = alloca(n_regs * sizeof(seq[0]));
	int *front           = alloca(ci->mst_n_childs * sizeof(front[0]));
	int best_col         = -1;
	int best_cost        = INT_MAX;


	int i;

	DBG((env->dbg, LEVEL_2, "\t%2{firm:indent}build colorings: %+F\n", depth, ci->inh.irn));

	for (i = 0; i < ci->mst_n_childs; ++i)
		cloud_mst_build_colorings(ci->mst_childs[i], depth + 1);

	for (i = 0; i < n_regs; ++i)
		ci->col_costs[i] = INT_MAX;

	/* Sort the children according to the cost of the affinity edge they have to the current node. */
	// qsort(child, ci->mst_n_childs, sizeof(childs[0]), cmp_parent);

	determine_start_colors(ci, seq);
	// qsort(seq, n_regs, sizeof(seq[0]), col_cost_pair_lt);

	for(i = 0; i < n_regs; ++i) {
		col_t col = seq[i].col;
		int costs = seq[i].costs;
		int done  = 0;

		if(INFEASIBLE(costs))
			break;

		/*
			Judge, if it is worthwhile trying this color.
			If another color was so good that we cannot get any better, bail out here.
			Perhaps???
		*/

		DBG((env->dbg, LEVEL_2, "\t%2{firm:indent}%+F trying color %d\n", depth, ci->inh.irn, col));

		/* This sorts the tmp_coloring array in the children according to the costs of the current color. */
		fill_tmp_coloring(ci, col);

		/* Initialize the front. It gives the indexes into the color tmp_coloring array. */
		memset(front, 0, ci->mst_n_childs * sizeof(front));

		/*
			As long as we have color configurations to try.
			We try the best ones first and get worse over and over.
		*/
		while (!done) {
			int j, try_push;

			if (try_coloring(ci, col, front, &try_push, depth + 1)) {
				int *res_front = FRONT_BASE(ci, col);
				int costs;

				for(j = 0; j < ci->mst_n_childs; ++j) {
					co2_cloud_irn_t *child = ci->mst_childs[j];
					col_t col              = child->tmp_coloring[front[j]].col;
					res_front[j] = col;
				}

				costs = examine_subtree_coloring(ci, col);
				ci->col_costs[col] = costs;
				done = 1;

				/* Set the current best color. */
				if(costs < best_cost) {
					best_cost = costs;
					best_col  = col;
				}
			}

			DBG((env->dbg, LEVEL_2, "\t%2{firm:indent}-> %s\n", depth, done ? "ok" : "failed"));

			/* Worsen the configuration, if that one didn't succeed. */
			if (!done)
				done = try_push ? push_front(ci, front) < 0 : 1;
		}
	}

	DBG((env->dbg, LEVEL_2, "%2{firm:indent} %+F\n", depth, ci->inh.irn));
	for(i = 0; i < n_regs; ++i)
		DBG((env->dbg, LEVEL_2, "%2{firm:indent}  color %d costs %d\n", depth, i, ci->col_costs[i]));

	return best_col;
}


static void determine_color_badness(co2_cloud_irn_t *ci, int depth)
{
	co2_t *env     = ci->cloud->env;
	int n_regs     = env->n_regs;
	be_ifg_t *ifg  = env->co->cenv->ifg;
	co2_irn_t *ir  = &ci->inh;
	bitset_t *bs   = bitset_alloca(n_regs);

	bitset_pos_t elm;
	ir_node *irn;
	int i, j;
	void *it;

	admissible_colors(env, &ci->inh, bs);
	bitset_flip_all(bs);
	bitset_foreach(bs, elm)
		ci->color_badness[elm] = n_regs * ci->costs;

	/* Use constrained/fixed interfering neighbors to influence the color badness */
	it = be_ifg_neighbours_iter_alloca(ifg);
	be_ifg_foreach_neighbour(ifg, it, ir->irn, irn) {
		co2_irn_t *ni = get_co2_irn(env, irn);
		int n_adm;

		admissible_colors(env, ni, bs);
		n_adm = bitset_popcnt(bs);
		bitset_foreach(bs, elm)
			ci->color_badness[elm] += n_regs - n_adm;

		if(ni->fixed) {
			col_t c = get_col(env, ni->irn);
			ci->color_badness[c] += n_regs * ci->costs;
		}
	}

	/* Collect the color badness for the whole subtree */
	for(i = 0; i < ci->mst_n_childs; ++i) {
		co2_cloud_irn_t *child = ci->mst_childs[i];
		determine_color_badness(child, depth + 1);

		for(j = 0; j < n_regs; ++j)
			ci->color_badness[j] += child->color_badness[j];
	}

	for(j = 0; j < n_regs; ++j)
		DBG((env->dbg, LEVEL_2, "%2{firm:indent}%+F col %d badness %d\n", depth, ci->inh.irn, j, ci->color_badness[j]));
}

static void apply_coloring(co2_cloud_irn_t *ci, col_t col, struct list_head *changed, int depth);


static int coalesce_top_down(co2_cloud_irn_t *ci, int child_nr, struct list_head *parent_changed, int depth)
{
	co2_t *env           = ci->cloud->env;
	col_cost_pair_t *seq = alloca(env->n_regs * sizeof(seq[0]));
	int is_root          = ci->mst_parent == ci;
	col_t parent_col     = is_root ? -1 : get_col(env, ci->mst_parent->inh.irn);
	int min_badness      = INT_MAX;
	int best_col_costs   = INT_MAX;
	int best_col         = -1;

	struct list_head changed;
	int ok, i, j;

	for(i = 0; i < env->n_regs; ++i) {
		int badness = ci->color_badness[i];

		seq[i].col   = i;
		seq[i].costs = is_color_admissible(env, &ci->inh, i) ? ci->color_badness[i] : INT_MAX;

		min_badness = MIN(min_badness, badness);
	}

	/* If we are not the root and the parent's color is allowed for this node give it top prio. */
	if(!is_root && is_color_admissible(env, &ci->inh, parent_col))
		seq[parent_col].costs = min_badness - 1;

	qsort(seq, env->n_regs, sizeof(seq[0]), col_cost_pair_lt);

	INIT_LIST_HEAD(&changed);
	for(i = 0; i < env->n_regs; ++i) {
		col_t col    = seq[i].col;
		int costs    = seq[i].costs;
		int add_cost = !is_root && col != parent_col ? ci->mst_costs : 0;

		int subtree_costs, sum_costs;

		DBG((env->dbg, LEVEL_2, "\t%2{firm:indent}%+F trying color %d\n", depth, ci->inh.irn, col));
		INIT_LIST_HEAD(&changed);
		ok = change_color_single(env, ci->inh.irn, col, &changed, depth);

		for(j = 0; ok && j < ci->mst_n_childs; ++j) {
			ok = coalesce_top_down(ci->mst_childs[j], j, &changed, depth + 1) >= 0;
		}

		/* If the subtree could not be colored, we have to try another color. */
		if (!ok) {
			reject_coloring(&changed);
			continue;
		}

		subtree_costs      = examine_subtree_coloring(ci, col);
		sum_costs          = subtree_costs + add_cost;
		DBG((env->dbg, LEVEL_2, "\t%2{firm:indent}-> %+F costing %d + %d is ok.\n", depth, ci->inh.irn, subtree_costs, add_cost));

		if(sum_costs < best_col_costs) {
			best_col           = col;
			best_col_costs     = sum_costs;
			ci->col_costs[col] = subtree_costs;
		}

		reject_coloring(&changed);

		if(sum_costs == 0)
			break;

		/* If we are at the root and we achieved an acceptable amount of optimization, we finish. */
#if 0
		if(is_root && (ci->cloud->mst_costs * stop_percentage < ci->cloud->mst_costs - sum_costs)) {
			assert(best_col != -1);
			break;
		}
#endif
	}

	if(!is_root) {
		int *front = FRONT_BASE(ci->mst_parent, parent_col);
		front[child_nr] = best_col;
	}

	if(best_col >= 0) {
		DBG((env->dbg, LEVEL_2, "\t%2{firm:indent}applying best color %d for %+F\n", depth, best_col, ci->inh.irn));
		apply_coloring(ci, best_col, parent_changed, depth);
	}

	return best_col;
}

static void populate_cloud(co2_t *env, co2_cloud_t *cloud, affinity_node_t *a, int curr_costs)
{
	be_ifg_t *ifg       = env->co->cenv->ifg;
	co2_cloud_irn_t *ci = get_co2_cloud_irn(env, a->irn);
	int costs           = 0;
	neighb_t *n;

	if(ci->visited >= env->visited)
		return;

	/* mark the node as visited and add it to the cloud. */
	ci->visited = env->visited;
	ci->cloud   = cloud;
	list_add(&ci->cloud_list, &cloud->members_head);

	DB((env->dbg, LEVEL_3, "\t%+F\n", ci->inh.irn));

	/* determine the nodes costs */
	co_gs_foreach_neighb(a, n) {
		costs += n->costs;
		DB((env->dbg, LEVEL_3, "\t\tneigh %+F cost %d\n", n->irn, n->costs));
		if(be_ifg_connected(ifg, a->irn, n->irn))
			cloud->inevit += n->costs;
	}

	/* add the node's cost to the total costs of the cloud. */
	ci->costs          = costs;
	cloud->costs      += costs;
	cloud->max_degree  = MAX(cloud->max_degree, ci->inh.aff->degree);
	cloud->n_memb++;

	/* If this is the heaviest node in the cloud, set it as the cloud's master. */
	if(costs >= curr_costs) {
		curr_costs    = costs;
		cloud->master = ci;
	}

	/* add all the neighbors of the node to the cloud. */
	co_gs_foreach_neighb(a, n) {
		affinity_node_t *an = get_affinity_info(env->co, n->irn);
		assert(an);
		populate_cloud(env, cloud, an, curr_costs);
	}
}

static co2_cloud_t *new_cloud(co2_t *env, affinity_node_t *a)
{
	co2_cloud_t *cloud = phase_alloc(&env->ph, sizeof(cloud[0]));
	co2_cloud_irn_t *ci;
	int i;

	DBG((env->dbg, LEVEL_2, "new cloud with %+F\n", a->irn));
	memset(cloud, 0, sizeof(cloud[0]));
	INIT_LIST_HEAD(&cloud->members_head);
	INIT_LIST_HEAD(&cloud->list);
	list_add(&cloud->list, &env->cloud_head);
	cloud->best_costs = INT_MAX;
	cloud->env = env;
	env->visited++;
	populate_cloud(env, cloud, a, 0);

	/* Allocate space for the best colors array, where the best coloring is saved. */
	// cloud->best_cols = phase_alloc(&env->ph, cloud->n_memb * sizeof(cloud->best_cols[0]));

	/* Also allocate space for the node sequence and compute that sequence. */
	cloud->seq    = phase_alloc(&env->ph, cloud->n_memb * sizeof(cloud->seq[0]));

	i = 0;
	list_for_each_entry(co2_cloud_irn_t, ci, &cloud->members_head, cloud_list) {
		ci->index       = i;
		cloud->seq[i++] = ci;
	}
	DBG((env->dbg, LEVEL_2, "cloud cost %d\n", cloud->costs));

	return cloud;
}

static void apply_coloring(co2_cloud_irn_t *ci, col_t col, struct list_head *changed, int depth)
{
	ir_node *irn = ci->inh.irn;
	int *front   = FRONT_BASE(ci, col);
	int i, ok;

	DBG((ci->cloud->env->dbg, LEVEL_2, "%2{firm:indent}setting %+F to %d\n", depth, irn, col));
	ok = change_color_single(ci->cloud->env, irn, col, changed, depth);
	assert(ok && "Color changing may not fail while committing the coloring");

 	for(i = 0; i < ci->mst_n_childs; ++i) {
		apply_coloring(ci->mst_childs[i], front[i], changed, depth + 1);
	}
}

static void process_cloud(co2_cloud_t *cloud)
{
	co2_t *env  = cloud->env;
	int n_edges = 0;

	struct list_head changed;
	edge_t *edges;
	int i;
	int best_col;

	/* Collect all edges in the cloud on an obstack and sort the increasingly */
	obstack_init(&cloud->obst);
	for(i = 0; i < cloud->n_memb; ++i) {
		co2_cloud_irn_t *ci = cloud->seq[i];
		neighb_t *n;

		co_gs_foreach_neighb(ci->inh.aff, n) {
			co2_cloud_irn_t *ni = get_co2_cloud_irn(cloud->env, n->irn);
			if(ci->index < ni->index) {
				edge_t e;
				e.src   = ci;
				e.tgt   = ni;
				e.costs = n->costs;
				obstack_grow(&cloud->obst, &e, sizeof(e));
				n_edges++;
			}
		}
	}
	edges = obstack_finish(&cloud->obst);
	qsort(edges, n_edges, sizeof(edges[0]), cmp_edges);

	/* Compute the maximum spanning tree using Kruskal/Union-Find */
	DBG((env->dbg, LEVEL_2, "computing spanning tree of cloud with master %+F\n", cloud->master->inh.irn));
	for(i = 0; i < n_edges; ++i) {
		edge_t *e        = &edges[i];
		co2_cloud_irn_t *p_src = find_mst_root(e->src);
		co2_cloud_irn_t *p_tgt = find_mst_root(e->tgt);

		if(p_src != p_tgt) {

			/*
				Bring the more costly nodes near to the root of the MST.
				Thus, tgt shall always be the more expensive node.
			*/
			if(p_src->costs > p_tgt->costs) {
				void *tmp = p_src;
				p_src = p_tgt;
				p_tgt = tmp;
			}

			p_tgt->mst_n_childs++;
			p_src->mst_parent = p_tgt;
			p_src->mst_costs  = e->costs;

			DBG((env->dbg, LEVEL_2, "\tadding edge %+F -- %+F cost %d\n", p_src->inh.irn, p_tgt->inh.irn, e->costs));
		}
	}
	obstack_free(&cloud->obst, edges);

	for(i = 0; i < cloud->n_memb; ++i) {
		co2_cloud_irn_t *ci = cloud->seq[i];
		int j;

		ci->mst_childs      = obstack_alloc(&cloud->obst, ci->mst_n_childs * sizeof(ci->mst_childs));
		ci->col_costs       = obstack_alloc(&cloud->obst, env->n_regs * sizeof(ci->col_costs[0]));
		ci->tmp_coloring    = obstack_alloc(&cloud->obst, env->n_regs * sizeof(ci->tmp_coloring[0]));
		ci->fronts          = obstack_alloc(&cloud->obst, env->n_regs * ci->mst_n_childs * sizeof(ci->fronts[0]));
		ci->color_badness   = obstack_alloc(&cloud->obst, env->n_regs * sizeof(ci->fronts[0]));
		memset(ci->color_badness, 0, env->n_regs * sizeof(ci->color_badness[0]));
		memset(ci->col_costs, 0, env->n_regs * sizeof(ci->col_costs[0]));
		memset(ci->tmp_coloring, 0, env->n_regs * sizeof(ci->tmp_coloring[0]));
		memset(ci->fronts, 0, env->n_regs * ci->mst_n_childs * sizeof(ci->fronts[0]));

		for(j = 0; j < env->n_regs; j++)
			ci->col_costs[j] = INT_MAX;

		ci->mst_n_childs    = 0;
	}

	/* build the child arrays in the nodes */
	for(i = 0; i < cloud->n_memb; ++i) {
		co2_cloud_irn_t *ci = cloud->seq[i];
		if(ci->mst_parent != ci)
			ci->mst_parent->mst_childs[ci->mst_parent->mst_n_childs++] = ci;
		else {
			cloud->mst_root  = ci;
			cloud->mst_costs = 0;
		}
	}

	/* Compute the "best" colorings. */
	// best_col = cloud_mst_build_colorings(cloud->mst_root, 0);

	determine_color_badness(cloud->mst_root, 0);
	INIT_LIST_HEAD(&changed);
	best_col = coalesce_top_down(cloud->mst_root, -1, &changed, 0);

	/* The coloring should represent the one with the best costs. */
	materialize_coloring(&changed);
	DBG((env->dbg, LEVEL_2, "\tbest coloring for root %+F was %d costing %d\n",
		cloud->mst_root->inh.irn, best_col, examine_subtree_coloring(cloud->mst_root, best_col)));

	/* Fix all nodes in the cloud. */
	for(i = 0; i < cloud->n_memb; ++i)
		cloud->seq[i]->inh.fixed = 1;

	/* Free all space used while optimizing this cloud. */
	obstack_free(&cloud->obst, NULL);
}

static int cloud_costs(co2_cloud_t *cloud)
{
	int i, costs = 0;
	neighb_t *n;

	for(i = 0; i < cloud->n_memb; ++i) {
		co2_irn_t *ci = (co2_irn_t *) cloud->seq[i];
		col_t col = get_col(cloud->env, ci->irn);
		co_gs_foreach_neighb(ci->aff, n) {
			col_t n_col = get_col(cloud->env, n->irn);
			costs += col != n_col ? n->costs : 0;
		}
	}

	return costs / 2;
}

static void process(co2_t *env)
{
	affinity_node_t *a;
	co2_cloud_t *pos;
	co2_cloud_t **clouds;
	int n_clouds;
	int i;
	int init_costs  = 0;
	int all_costs   = 0;
	int final_costs = 0;

	n_clouds = 0;
	co_gs_foreach_aff_node(env->co, a) {
		co2_cloud_irn_t *ci = get_co2_cloud_irn(env, a->irn);

		if(!ci->cloud) {
			co2_cloud_t *cloud = new_cloud(env, a);
			n_clouds++;
		}
	}

	i = 0;
	clouds = xmalloc(n_clouds * sizeof(clouds[0]));
	list_for_each_entry(co2_cloud_t, pos, &env->cloud_head, list)
		clouds[i++] = pos;
	qsort(clouds, n_clouds, sizeof(clouds[0]), cmp_clouds_gt);

	for(i = 0; i < n_clouds; ++i) {
		init_costs  += cloud_costs(clouds[i]);
		process_cloud(clouds[i]);
		all_costs   += clouds[i]->costs;
		final_costs += cloud_costs(clouds[i]);

		/* Dump the IFG if the user demanded it. */
		if (dump_flags & DUMP_CLOUD) {
			char buf[256];
			FILE *f;

			ir_snprintf(buf, sizeof(buf), "ifg_%F_%s_cloud_%d.dot", env->co->irg, env->co->cls->name, i);
			if(f = fopen(buf, "wt")) {
				be_ifg_dump_dot(env->co->cenv->ifg, env->co->irg, f, &ifg_dot_cb, env);
				fclose(f);
			}
		}
	}

	DB((env->dbg, LEVEL_1, "all costs: %d, init costs: %d, final costs: %d\n", all_costs, init_costs, final_costs));

	xfree(clouds);
}

static void writeback_colors(co2_t *env)
{
	const arch_env_t *aenv = env->co->aenv;
	co2_irn_t *irn;

	for(irn = env->touched; irn; irn = irn->touched_next) {
		const arch_register_t *reg = arch_register_for_index(env->co->cls, irn->orig_col);
		arch_set_irn_register(aenv, irn->irn, reg);
	}
}


/*
  ___ _____ ____   ____   ___ _____   ____                        _
 |_ _|  ___/ ___| |  _ \ / _ \_   _| |  _ \ _   _ _ __ ___  _ __ (_)_ __   __ _
  | || |_ | |  _  | | | | | | || |   | | | | | | | '_ ` _ \| '_ \| | '_ \ / _` |
  | ||  _|| |_| | | |_| | |_| || |   | |_| | |_| | | | | | | |_) | | | | | (_| |
 |___|_|   \____| |____/ \___/ |_|   |____/ \__,_|_| |_| |_| .__/|_|_| |_|\__, |
                                                           |_|            |___/
*/

static const char *get_dot_color_name(int col)
{
	static const char *names[] = {
		"blue",
		"red",
		"green",
		"yellow",
		"cyan",
		"magenta",
		"orange",
		"chocolate",
		"beige",
		"navy",
		"darkgreen",
		"darkred",
		"lightPink",
		"chartreuse",
		"lightskyblue",
		"linen",
		"pink",
		"lightslateblue",
		"mintcream",
		"red",
		"darkolivegreen",
		"mediumblue",
		"mistyrose",
		"salmon",
		"darkseagreen",
		"mediumslateblue"
		"moccasin",
		"tomato",
		"forestgreen",
		"darkturquoise",
		"palevioletred"
	};

	return col < sizeof(names)/sizeof(names[0]) ? names[col] : "white";
}

static const char *get_dot_shape_name(co2_t *env, co2_irn_t *ci)
{
	arch_register_req_t req;

	arch_get_register_req(env->co->aenv, &req, ci->irn, BE_OUT_POS(0));
	if(arch_register_req_is(&req, limited))
		return "diamond";

	if(ci->fixed)
		return "rectangle";

	if(ci->tmp_fixed)
		return "hexagon";

	return "ellipse";
}

static void ifg_dump_graph_attr(FILE *f, void *self)
{
	fprintf(f, "overlay=false");
}

static int ifg_is_dump_node(void *self, ir_node *irn)
{
	co2_t *env = self;
	return !arch_irn_is(env->co->aenv, irn, ignore);
}

static void ifg_dump_node_attr(FILE *f, void *self, ir_node *irn)
{
	co2_t *env    = self;
	co2_irn_t *ci = get_co2_irn(env, irn);
	int peri      = 1;

	if(ci->aff) {
		co2_cloud_irn_t *cci = (void *) ci;
		if (cci->cloud && cci->cloud->mst_root == cci)
			peri = 2;
	}

	ir_fprintf(f, "label=\"%+F\" style=filled peripheries=%d color=%s shape=%s", irn, peri,
		get_dot_color_name(get_col(env, irn)), get_dot_shape_name(env, ci));
}

static void ifg_dump_at_end(FILE *file, void *self)
{
	co2_t *env = self;
	affinity_node_t *a;

	co_gs_foreach_aff_node(env->co, a) {
		co2_cloud_irn_t *ai = get_co2_cloud_irn(env, a->irn);
		int idx = get_irn_idx(a->irn);
		neighb_t *n;

		co_gs_foreach_neighb(a, n) {
			int nidx = get_irn_idx(n->irn);
			co2_cloud_irn_t *ci = get_co2_cloud_irn(env, n->irn);

			if(idx < nidx) {
				const char *color = get_col(env, a->irn) == get_col(env, n->irn) ? "black" : "red";
				const char *arr = "arrowhead=dot arrowtail=dot";

				if(ci->mst_parent == ai)
					arr = "arrowtail=normal";
				else if(ai->mst_parent == ci)
					arr = "arrowhead=normal";

				fprintf(file, "\tn%d -- n%d [label=\"%d\" %s style=dashed color=%s weight=0.01];\n", idx, nidx, n->costs, arr, color);
			}
		}
	}
}


static be_ifg_dump_dot_cb_t ifg_dot_cb = {
	ifg_is_dump_node,
	ifg_dump_graph_attr,
	ifg_dump_node_attr,
	NULL,
	NULL,
	ifg_dump_at_end
};


void co_solve_heuristic_new(copy_opt_t *co)
{
	co2_t env;
	FILE *f;

	phase_init(&env.ph, "co2", co->cenv->birg->irg, PHASE_DEFAULT_GROWTH, co2_irn_init);
	env.touched     = NULL;
	env.visited     = 0;
	env.co          = co;
	env.n_regs      = co->cls->n_regs;
	env.ignore_regs = bitset_alloca(co->cls->n_regs);
	arch_put_non_ignore_regs(co->aenv, co->cls, env.ignore_regs);
	bitset_flip_all(env.ignore_regs);
	be_abi_put_ignore_regs(co->cenv->birg->abi, co->cls, env.ignore_regs);
	FIRM_DBG_REGISTER(env.dbg, "firm.be.co2");
	INIT_LIST_HEAD(&env.cloud_head);

	if(dump_flags & DUMP_BEFORE) {
		if(f = be_chordal_open(co->cenv, "ifg_before_", "dot")) {
			be_ifg_dump_dot(co->cenv->ifg, co->irg, f, &ifg_dot_cb, &env);
			fclose(f);
		}
	}

	process(&env);

	if(dump_flags & DUMP_AFTER) {
		if(f = be_chordal_open(co->cenv, "ifg_after_", "dot")) {
			be_ifg_dump_dot(co->cenv->ifg, co->irg, f, &ifg_dot_cb, &env);
			fclose(f);
		}
	}

	writeback_colors(&env);
	phase_free(&env.ph);
}
