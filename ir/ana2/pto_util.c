/* -*- c -*- */

/*
   Project:     libFIRM
   File name:   ir/ana/pto_util.c
   Purpose:     Utilitites for PTO
   Author:      Florian
   Modified by:
   Created:     Sat Nov 13 19:35:27 CET 2004
   CVS-ID:      $Id$
   Copyright:   (c) 1999-2004 Universitšt Karlsruhe
   Licence:     This file is protected by the GPL -  GNU GENERAL PUBLIC LICENSE.
*/

# ifdef HAVE_CONFIG_H
#  include <config.h>
# endif

/*
 pto_util: Utilitites for PTO
*/

# include "pto_util.h"

# include "irnode.h"
# include "irgwalk.h"
# include "xmalloc.h"

# include "pto_debug.h"

/* Local Defines: */

/* Local Data Types: */
/* Environment for find_irg_args */
typedef struct find_irg_args_env {
  ir_node **args;
  ir_node *arg;
} find_irg_args_env_t;


/* Local Variables: */

/* Local Prototypes: */

/* ===================================================
   Local Implementation:
   =================================================== */
/* Helper for find_irg_args */
static void find_irg_arg (ir_node *node, void *env)
{
  find_irg_args_env_t *arg_env = (find_irg_args_env_t*) env;

  if (iro_Proj == get_irn_opcode (node)) {
    if (arg_env->arg == get_Proj_pred (node)) {
      long n = get_Proj_proj (node);

      assert (! arg_env->args [n]);

      arg_env->args [n] = node;
    }
  }
}

/* ===================================================
   Exported Implementation:
   =================================================== */
/* Find the arguments of a graph. For a method that has n args, the
  result array has 'n+1' entries, the last of which is written NULL. */
ir_node **find_irg_args (ir_graph *graph)
{
  type *tp = get_entity_type (get_irg_entity (graph));
  const int n_args = get_method_n_params (tp);
  ir_node **args = (ir_node**) xmalloc (sizeof (ir_node*) * (n_args+1));
  ir_node *arg = get_irg_args (graph);
  find_irg_args_env_t *arg_env =
    (find_irg_args_env_t*) xmalloc (sizeof (find_irg_args_env_t));

  arg_env->args = args;
  arg_env->arg  = arg;

  /* or use get_irg_end ?!? */
  {
    ir_graph *save = get_current_ir_graph ();
    set_current_ir_graph (graph);
    irg_walk (get_irg_end (graph), find_irg_arg, NULL, arg_env);
    set_current_ir_graph (save);
  }

  free (arg_env);

  args [n_args] = NULL;

  return (args);
}
/* Get the entity of a ptr */
entity *get_ptr_ent (ir_node *ptr)
{
  entity *ent = NULL;
  const opcode ptr_op = get_irn_opcode (ptr);
  switch (ptr_op) {
  case (iro_Sel): {
    ent = get_Sel_entity (ptr);
  } break;

  case (iro_SymConst): {
    ent = get_SymConst_entity (ptr);
  } break;

  default: {
    fprintf (stderr, "%s: no ent for ptr=%s[%ld]\n",
             __FUNCTION__,
             get_op_name (get_irn_op (ptr)),
             get_irn_node_nr (ptr));
    assert (0);
  }
  }

  return (ent);
}




/*
  $Log$
  Revision 1.7  2004/11/24 14:53:56  liekweg
  Bugfixes

  Revision 1.6  2004/11/18 16:37:07  liekweg
  rewrite


*/
