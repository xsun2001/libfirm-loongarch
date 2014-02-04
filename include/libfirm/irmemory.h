/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    Memory disambiguator
 * @author   Michael Beck
 * @date     27.12.2006
 */
#ifndef FIRM_ANA_IRMEMORY_H
#define FIRM_ANA_IRMEMORY_H

#include "firm_types.h"
#include "begin.h"

/** @ingroup irana
 * @defgroup ir_memory  Memory Disambiguator
 *
 * A memory disambiguator checks whether 2 given SSA values representing
 * addresses alias.
 *
 * @{
 */

/** The alias relation of two memory addresses. */
typedef enum ir_alias_relation {
	ir_no_alias,       /**< No alias. */
	ir_may_alias,      /**< Unknown state, may alias. */
	ir_sure_alias      /**< Sure alias. */
} ir_alias_relation;

/** The state of the entity usage flags. */
typedef enum ir_entity_usage_computed_state {
	ir_entity_usage_not_computed,
	ir_entity_usage_computed
} ir_entity_usage_computed_state;

/** Possible options for the memory disambiguator. */
typedef enum ir_disambuigator_options {
	aa_opt_no_opt               = 0,  /**< no options: always assume aliasing */
	aa_opt_type_based           = 1,  /**< use type based alias analysis: strict typed source language */
	aa_opt_byte_type_may_alias  = 2,  /**< if type based analysis is enabled: bytes types may alias other types */
	aa_opt_no_alias             = 16, /**< two addresses NEVER alias, use with CAUTION (gcc -fno-alias) */
	aa_opt_inherited            = 128 /**< only for implementation: options from a graph are inherited from global */
} ir_disambuigator_options;
ENUM_BITSET(ir_disambuigator_options)

/**
 * Returns a human readable name for an alias relation.
 */
FIRM_API const char *get_ir_alias_relation_name(ir_alias_relation rel);

/**
 * Determine the alias relation between two addresses.
 *
 * @param addr1   The first address.
 * @param type1   The type of the first memory access.
 * @param addr2   The second address.
 * @param type2   The type of the second memory access.
 *
 * The memory disambiguator tries to determine the alias state between
 * two memory addresses. The following rules are used:
 *
 * - different variable from the same segment never alias (R1 a)
 * - variables from different segments never alias when:
 *   - a global variable and a local one never alias (R1 b)
 *   - a global variable and a TLS one never alias (R1 c)
 *   - a local variable and a TLS one never alias (R1 d)
 *   - a local variable and a parameter never alias (R1 e)
 *   - a global variable and the result of a malloc routine never alias (R1 f)
 *   - a local variable and the result of a malloc routine never alias (R1 g)
 *   - a TLS variable and the result of a malloc routine never alias (R1 h)
 *   - a parameter and the result of a malloc routine (obtained in the
 *     same routine as the parameter) never alias (R1 i)
 * - two different variables never alias (R2)
 * - if one is a variable whose address has never been taken
 *   there is no alias (R3)
 * - if two memory addresses have the same base and their offsets
 *   do not describe overlapping regions there is no alias (R4)
 * - if opt_strong_typed is set and both addresses describe entities,
 *   different types never alias (R5)
 *
 * If none of these rules apply, the points-to framework must be
 * interrogated to detect the alias relation.
 */
FIRM_API ir_alias_relation get_alias_relation(
	const ir_node *addr1, const ir_type *type1,
	const ir_node *addr2, const ir_type *type2);

/**
 * Assure that the entity usage flags have been computed for the given graph.
 *
 * This analysis computes the entity usage state for all local variables.
 *
 * Even then the information is not cleaned from the variables, call
 * assure_irg_entity_usage_computed() again for recomputation.
 */
FIRM_API void assure_irg_entity_usage_computed(ir_graph *irg);

/**
 * Returns the current address taken state of the globals.
 */
FIRM_API ir_entity_usage_computed_state get_irp_globals_entity_usage_state(void);

/**
 * Sets the current address taken state of the globals.
 *
 * @param state  the new state
 */
FIRM_API void set_irp_globals_entity_usage_state(ir_entity_usage_computed_state state);

/**
 * Assure that the address taken flag is computed for the global and TLS entities (variables).
 *
 * This is an interprocedural analysis that computes the address_taken state
 * for all global and TLS variables.
 *
 * Note that this is a conservative estimation that by no Firm transformation
 * can be invalidated, so it's only recomputed if manually triggered by calling
 * set_irp_globals_entity_usage_state(ir_entity_usage_not_computed).
 * Even then the information is not cleaned from the variables, call
 * assure_irp_globals_entity_usage_computed() again for recomputation.
 */
FIRM_API void assure_irp_globals_entity_usage_computed(void);

/**
 * Returns the memory disambiguator options for a graph.
 *
 * @param irg  the graph
 */
FIRM_API unsigned get_irg_memory_disambiguator_options(const ir_graph *irg);

/**
 * Sets the memory disambiguator options for a graph.
 *
 * @param irg      the graph
 * @param options  a set of options
 */
FIRM_API void set_irg_memory_disambiguator_options(ir_graph *irg,
                                                   unsigned options);

/**
 * Sets the global disambiguator options for all graphs not having local
 * options.
 *
 * @param options  a set of options
 */
FIRM_API void set_irp_memory_disambiguator_options(unsigned options);

/**
 * Mark all private methods, i.e. those of which all call sites are known.
 * We use a very convervative estimation yet: If the address of a method is
 * never taken AND its visibility is visibility_local, then it's private.
 */
FIRM_API void mark_private_methods(void);

/** @} */

#include "end.h"

#endif
