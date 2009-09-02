/**************************************************************************
		GTA PROJECT   AT division
	Copyright, 1990, The Regents of the University of California.
		 Los Alamos National Laboratory

	Copyright, 2010, Helmholtz-Zentrum Berlin f. Materialien
		und Energie GmbH, Germany (HZB)
		(see file Copyright.HZB included in this distribution)
***************************************************************************
		Analysis of parse tree
***************************************************************************/
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "types.h"
#include "sym_table.h"
#include "snc_main.h"
#include "parse.h"
#include "analysis.h"

static const int impossible = 0;

static void analyse_definitions(Program *p);
static void analyse_option(Options *options, Expr *defn);
static void analyse_state_option(StateOptions *options, Expr *defn);
static void analyse_declaration(SymTable st, Expr *scope, Expr *defn);
static void analyse_assign(SymTable st, ChanList *chan_list, Expr *scope, Expr *defn);
static void analyse_monitor(SymTable st, Expr *scope, Expr *defn);
static void analyse_sync(SymTable st, Expr *scope, Expr *defn);
static void analyse_syncq(SymTable st, SyncQList *syncq_list, Expr *scope, Expr *defn);
static void assign_subscript(ChanList *chan_list, Expr *defn, Var *vp, Expr *subscr, Expr *pv_name);
static void assign_single(ChanList *chan_list, Expr *defn, Var *vp, Expr *pv_name);
static void assign_list(ChanList *chan_list, Expr *defn, Var *vp, Expr *pv_name_list);
static Chan *new_channel(ChanList *chan_list, Var *vp, uint count, uint index);
static SyncQ *new_sync_queue(SyncQList *syncq_list, Var *evp, uint size);
static void connect_variables(SymTable st, Expr *scope);
static void connect_state_change_stmts(SymTable st, Expr *scope);
static uint connect_states(SymTable st, Expr *ss_list);
static void add_var(SymTable st, Var *vp, Expr *scope);
static Var *find_var(SymTable st, char *name, Expr *scope);
static uint assign_ef_bits(Expr *scope);

Program *analyse_program(Expr *prog, Options options)
{
	assert(prog);
#ifdef DEBUG
	report("-------------------- Analysis --------------------\n");
#endif

	Program *p = new(Program);

	assert(p);

	p->options = options;
	p->prog = prog;

	p->name			= prog->value;
	if (prog->prog_param)
		p->param	= prog->prog_param->value;
	else
		p->param	= "";

	p->sym_table = sym_table_create();

	p->chan_list = new(ChanList);
	p->syncq_list = new(SyncQList);

#ifdef DEBUG
	report("created symbol table, channel list, and syncq list\n");
#endif

	analyse_definitions(p);
	p->num_ss = connect_states(p->sym_table, prog);
	connect_variables(p->sym_table, prog);
	connect_state_change_stmts(p->sym_table, prog);
	p->num_event_flags = assign_ef_bits(p->prog);
	return p;
}

int analyse_defn(Expr *scope, Expr *parent_scope, void *parg)
{
	Program	*p = (Program *)parg;

	assert(scope);

#ifdef DEBUG
	report("analyse_defn: scope=(%s:%s)\n",
		expr_type_name(scope), scope->value);
#endif

	assert(is_scope(scope));
	assert(!parent_scope || is_scope(parent_scope));

	Expr *defn_list = defn_list_from_scope(scope);
	VarList **pvar_list = pvar_list_from_scope(scope);

	/* NOTE: We always need to allocate a var_list, even if there are no
	   definitions on this level, so later on (see connect_variables below)
	   we can traverse in the other direction to find the nearest enclosing
	   scope. See connect_variables below. */
	if (!*pvar_list)
	{
		*pvar_list = new(VarList);
		(*pvar_list)->parent_scope = parent_scope;
	}

	Expr *defn;

	foreach (defn, defn_list)
	{
		switch (defn->type)
		{
		case D_OPTION:
			if (scope->type == D_PROG)
				analyse_option(&p->options, defn);
			else if (scope->type == D_STATE)
			{
				analyse_state_option(&scope->extra.e_state->options, defn);
			}
			break;
		case D_DECL:
			analyse_declaration(p->sym_table, scope, defn);
			break;
		case D_ASSIGN:
			analyse_assign(p->sym_table, p->chan_list, scope, defn);
			break;
		case D_MONITOR:
			analyse_monitor(p->sym_table, scope, defn);
			break;
		case D_SYNC:
			analyse_sync(p->sym_table, scope, defn);
			break;
		case D_SYNCQ:
			analyse_syncq(p->sym_table, p->syncq_list, scope, defn);
			break;
		case T_TEXT:
			break;
		default:
			assert(impossible);
		}
	}
	return TRUE; /* always descend into children */
}

VarList **pvar_list_from_scope(Expr *scope)
{
	assert(is_scope(scope));
	switch(scope->type)
	{
	case D_PROG:
		return &scope->extra.e_prog;
	case D_SS:
		assert(scope->extra.e_ss);
		return &scope->extra.e_ss->var_list;
	case D_STATE:
		assert(scope->extra.e_state);
		return &scope->extra.e_state->var_list;
	case D_WHEN:
		assert(scope->extra.e_when);
		return &scope->extra.e_when->var_list;
	case D_ENTRY:
		return &scope->extra.e_entry;
	case D_EXIT:
		return &scope->extra.e_exit;
	case S_CMPND:
		return &scope->extra.e_cmpnd;
	default:
		assert(impossible);
	}
}

Expr *defn_list_from_scope(Expr *scope)
{
	assert(is_scope(scope));
	switch(scope->type)
	{
	case D_PROG:
		return scope->prog_defns;
	case D_SS:
		return scope->ss_defns;
	case D_STATE:
		return scope->state_defns;
	case D_WHEN:
		return scope->when_defns;
	case D_ENTRY:
		return scope->entry_defns;
	case D_EXIT:
		return scope->exit_defns;
	case S_CMPND:
		return scope->cmpnd_defns;
	default:
		assert(impossible);
	}
}

static void analyse_definitions(Program *p)
{
#ifdef DEBUG
	report("**begin** analyse definitions\n");
#endif

	traverse_expr_tree(p->prog, scope_mask, ~has_sub_scope_mask, 0, analyse_defn, p);

#ifdef DEBUG
	report("**end** analyse definitions\n");
#endif
}

/* Options at the top-level. Note: latest given value for option wins. */
static void analyse_option(Options *options, Expr *defn)
{
	char	*optname = defn->value;
	int	optval = defn->extra.e_option;

	for (; *optname; optname++)
	{
		switch(*optname)
		{
		case 'a': options->async = optval; break;
		case 'c': options->conn = optval; break;
		case 'd': options->debug = optval; break;
		case 'e': options->newef = optval; break;
		case 'i': options->init_reg = optval; break;
		case 'l': options->line = optval; break;
		case 'm': options->main = optval; break;
		case 'r': options->reent = optval; break;
		case 'w': options->warn = optval; break;
		default: report_at_expr(defn,
		  "warning: unknown option '%s'\n", optname);
		}
	}
}

/* Options in state declarations. Note: latest given value for option wins. */
static void analyse_state_option(StateOptions *options, Expr *defn)
{
	char	*optname = defn->value;
	int	optval = defn->extra.e_option;

	for (; *optname; optname++)
	{
		switch(*optname)
		{
		case 't': options->do_reset_timers = optval; break;
		case 'e': options->no_entry_from_self = optval; break;
		case 'x': options->no_exit_to_self = optval; break;
		default: report_at_expr(defn,
		  "warning: unknown state option '%s'\n", optname);
		}
	}
}

static void analyse_declaration(SymTable st, Expr *scope, Expr *defn)
{
	Var *vp;

	assert(scope);
	assert(defn);

	vp = defn->extra.e_decl;

	assert(vp);
#ifdef DEBUG
	report("declaration: %s\n", vp->name);
#endif

	VarList *var_list = *pvar_list_from_scope(scope);

	if (!sym_table_insert(st, vp->name, var_list, vp))
	{
		Var *vp2 = sym_table_lookup(st, vp->name, var_list);
		if (vp2->decl)
			error_at_expr(defn,
			 "variable '%s' already declared at %s:%d\n",
			 vp->name, vp2->decl->src_file, vp2->decl->line_num);
		else
			error_at_expr(defn,
			 "variable '%s' already (implicitly) declared\n",
			 vp->name);
	}
	else
	{
		add_var(st, vp, scope);
	}
}

static void analyse_assign(SymTable st, ChanList *chan_list, Expr *scope, Expr *defn)
{
	char *name = defn->value;
	Var *vp = find_var(st, name, scope);

	if (!vp)
	{
		error_at_expr(defn, "variable '%s' not declared\n", name);
		return;
	}
	if (vp->type == V_NONE)
	{
		error_at_expr(defn, "this type of variable cannot be assigned to a pv\n", name);
		return;
	}
	if (defn->assign_subscr)
	{
		assign_subscript(chan_list, defn, vp, defn->assign_subscr, defn->assign_pvs);
	}
	else if (!defn->assign_pvs->next)
	{
		assign_single(chan_list, defn, vp, defn->assign_pvs);
	}
	else
	{
		assign_list(chan_list, defn, vp, defn->assign_pvs);
	}
}

/* Assign a (whole) variable to a channel.
 * Format: assign <variable> to <string>;
 */
static void assign_single(
	ChanList	*chan_list,
	Expr		*defn,
	Var		*vp,
	Expr		*pv_name
)
{
	assert(chan_list);
	assert(defn);
	assert(vp);
	assert(pv_name);

#ifdef DEBUG
	report("assign %s to %s;\n", vp->name, pv_name->value);
#endif

	if (vp->assign != M_NONE)
	{
		error_at_expr(defn, "variable '%s' already assigned\n", vp->name);
		return;
	}
	vp->assign = M_SINGLE;
	vp->chan.single = new_channel(
		chan_list, vp, vp->length1 * vp->length2, 0);
	vp->chan.single->name = pv_name->value;
}

static void assign_elem(
	ChanList	*chan_list,
	Expr		*defn,
	Var		*vp,
	int		n_subscr,
	Expr		*pv_name
)
{
	assert(chan_list);
	assert(defn);
	assert(vp);
	assert(pv_name);
	assert(n_subscr <= vp->length1);

	if (vp->assign == M_NONE)
	{
		int n;

		vp->assign = M_MULTI;
		vp->chan.multi = calloc(vp->length1, sizeof(Chan *));
		for (n = 0; n < vp->length1; n++)
		{
			vp->chan.multi[n] = new_channel(
				chan_list, vp, vp->length2, n);
		}
	}
	assert(vp->assign == M_MULTI);
	if (vp->chan.multi[n_subscr]->name)
	{
		error_at_expr(defn, "array element '%s[%d]' already assigned to '%s'\n",
			vp->name, n_subscr, vp->chan.multi[n_subscr]->name);
		return;
	}
	vp->chan.multi[n_subscr]->name = pv_name->value;
}

/* Assign an array element to a channel.
 * Format: assign <variable>[<subscr>] to <string>; */
static void assign_subscript(
	ChanList	*chan_list,
	Expr		*defn,
	Var		*vp,
	Expr		*subscr,
	Expr		*pv_name
)
{
	uint n_subscr;

	assert(chan_list);
	assert(defn);
	assert(vp);
	assert(subscr);
	assert(subscr->type == E_CONST);		/* syntax */
	assert(pv_name);

#ifdef DEBUG
	report("assign %s[%s] to '%s';\n", vp->name, subscr->value, pv_name);
#endif

	if (vp->class != VC_ARRAY1 && vp->class != VC_ARRAY2)
	{
		error_at_expr(defn, "variable '%s' is not an array\n", vp->name);
		return;
	}
	if (vp->assign == M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' already assigned\n", vp->name);
		return;
	}
	if (!strtoui(subscr->value, vp->length1, &n_subscr))
	{
		error_at_expr(subscr, "subscript in '%s[%s]' out of range\n",
			vp->name, subscr->value);
		return;
	}
	assign_elem(chan_list, defn, vp, n_subscr, pv_name);
}

/* Assign an array variable to multiple channels.
 * Format: assign <variable> to { <string>, <string>, ... };
 */
static void assign_list(
	ChanList	*chan_list,
	Expr		*defn,
	Var		*vp,
	Expr		*pv_name_list
)
{
	Expr	*pv_name;
	uint	n_subscr = 0;

	assert(chan_list);
	assert(defn);
	assert(vp);
	assert(pv_name_list);

#ifdef DEBUG
	report("assign %s to {", vp->name);
#endif

	if (vp->class != VC_ARRAY1 && vp->class != VC_ARRAY2)
	{
		error_at_expr(defn, "variable '%s' is not an array\n", vp->name);
		return;
	}
	if (vp->assign == M_SINGLE)
	{
		error_at_expr(defn, "assign: variable '%s' already assigned\n", vp->name);
		return;
	}
	foreach (pv_name, pv_name_list)
	{
#ifdef DEBUG
		report("'%s'%s", pv_name->value, pv_name->next ? ", " : "};\n");
#endif
		assign_elem(chan_list, defn, vp, n_subscr++, pv_name);
	}
}

static void monitor_var(Expr *defn, Var *vp)
{
	assert(defn);
	assert(vp);

#ifdef DEBUG
	report("monitor %s;", vp->name);
#endif

	if (vp->assign == M_NONE)
	{
		error_at_expr(defn, "variable '%s' not assigned\n", vp->name);
		return;
	}
	if (vp->monitor == M_SINGLE)
	{
		warning_at_expr(defn,
			"variable '%s' already monitored\n", vp->name);
		return;				/* nothing to do */
	}
	vp->monitor = M_SINGLE;			/* strengthen if M_MULTI */
	if (vp->assign == M_SINGLE)
	{
		vp->chan.single->monitor = TRUE;
	}
	else
	{
		uint n;
		assert(vp->assign == M_MULTI);
		for (n = 0; n < vp->length1; n++)
		{
			vp->chan.multi[n]->monitor = TRUE;
		}
	}
}

static void monitor_elem(Expr *defn, Var *vp, Expr *subscr)
{
	uint	n_subscr;

	assert(defn);
	assert(vp);
	assert(subscr);
	assert(subscr->type == E_CONST);		/* syntax */

#ifdef DEBUG
	report("monitor %s[%s];\n", vp->name, subscr->value);
#endif

	if (vp->class != VC_ARRAY1 && vp->class != VC_ARRAY2)
	{
		error_at_expr(defn, "variable '%s' is not an array\n", vp->name);
		return;
	}
	if (!strtoui(subscr->value, vp->length1, &n_subscr))
	{
		error_at_expr(subscr, "subscript in '%s[%s]' out of range\n",
			vp->name, subscr->value);
		return;
	}
	if (vp->assign == M_NONE)
	{
		error_at_expr(defn, "array element '%s[%d]' not assigned\n",
			vp->name, n_subscr);
		return;
	}
	if (vp->monitor == M_SINGLE)
	{
		warning_at_expr(defn, "array element '%s[%d]' already monitored\n",
			vp->name, n_subscr);
		return;		/* nothing to do */
	}
	if (vp->assign == M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' is assigned to a single channel "
			"and can only be monitored wholesale\n", vp->name);
		return;
	}
	assert(vp->assign == M_MULTI);
	if (!vp->chan.multi[n_subscr]->name)
	{
		error_at_expr(defn, "array element '%s[%d]' not assigned\n",
			vp->name, n_subscr);
		return;
	}
	if (vp->chan.multi[n_subscr]->monitor)
	{
		warning_at_expr(defn, "'%s[%d]' already monitored\n",
			vp->name, n_subscr);
		return;					/* nothing to do */
	}
	vp->chan.multi[n_subscr]->monitor = TRUE;	/* do it */
}

static void analyse_monitor(SymTable st, Expr *scope, Expr *defn)
{
	Var	*vp;
	char	*var_name;

	assert(scope);
	assert(defn);
	assert(defn->type == D_MONITOR);

	var_name = defn->value;
	assert(var_name);

	vp = find_var(st, var_name, scope);
	if (!vp)
	{
		error_at_expr(defn,
			"variable '%s' not declared\n", var_name);
		return;
	}
	if (defn->monitor_subscr)
	{
		monitor_elem(defn, vp, defn->monitor_subscr);
	}
	else
	{
		monitor_var(defn, vp);
	}
}

static void sync_var(Expr *defn, Var *vp, Var *evp)
{
	assert(defn);
	assert(vp);
	assert(evp);
	assert(vp->sync != M_SINGLE);			/* call */

#ifdef DEBUG
	report("sync %s to %s;\n", vp->name, evp->name);
#endif

	if (vp->sync == M_MULTI)
	{
		error_at_expr(defn, "some array elements of '%s' already sync'd\n",
			vp->name);
		return;
	}
	if (vp->monitor != M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' not monitored\n", vp->name);
		return;
	}
	if (vp->assign == M_NONE)
	{
		error_at_expr(defn, "variable '%s' not assigned\n", vp->name);
		return;
	}
	vp->sync = M_SINGLE;
	if (vp->assign == M_SINGLE)
	{
		assert(vp->chan.single);
		assert(vp->monitor != M_MULTI);	/* by L1 */
		assert(vp->sync != M_MULTI);	/* by L1 */
		vp->chan.single->sync = evp;
		vp->sync = M_SINGLE;
	}
	else
	{
		uint n;
		assert(vp->assign == M_MULTI);	/* else */
		vp->sync = M_SINGLE;
		for (n = 0; n < vp->length1; n++)
		{
			assert(vp->chan.multi[n]->monitor);
			assert(!vp->chan.multi[n]->sync);
			vp->chan.multi[n]->sync = evp;
		}
	}
}

static void sync_elem(Expr *defn, Var *vp, Expr *subscr, Var *evp)
{
	uint	n_subscr;

	assert(defn);					/* syntax */
	assert(vp);					/* call */
	assert(subscr);					/* call */
	assert(subscr->type == E_CONST);		/* syntax */
	assert(evp);					/* syntax */

	assert(vp->sync != M_SINGLE);			/* call */

#ifdef DEBUG
	report("sync %s[%d] to %s;\n", vp->name, subscr->value, evp->name);
#endif

	if (vp->class != VC_ARRAY1 && vp->class != VC_ARRAY2)
	{
		error_at_expr(defn, "variable '%s' is not an array\n", vp->name);
		return;
	}
	if (!strtoui(subscr->value, vp->length1, &n_subscr))
	{
		error_at_expr(subscr, "subscript in '%s[%s]' out of range\n",
			vp->name, subscr->value);
		return;
	}
	if (vp->assign == M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' is assigned to a single channel "
			"and can only be sync'd wholesale\n", vp->name);
		return;
	}
	if (vp->assign == M_NONE || !vp->chan.multi[n_subscr]->name)
	{
		error_at_expr(defn, "array element '%s[%d]' not assigned\n",
			vp->name, n_subscr);
		return;
	}
	assert(vp->assign == M_MULTI);
	if (vp->monitor == M_NONE || !vp->chan.multi[n_subscr]->monitor)
	{
		error_at_expr(defn, "array element '%s[%d]' not monitored\n",
			vp->name, n_subscr);
		return;
	}
	if (vp->chan.multi[n_subscr]->sync)
	{
		warning_at_expr(defn, "'%s[%d]' already sync'd\n",
			vp->name, n_subscr);
		return;					/* nothing to do */
	}
	vp->sync = M_MULTI;
	vp->chan.multi[n_subscr]->sync = evp;		/* do it */
}

static void analyse_sync(SymTable st, Expr *scope, Expr *defn)
{
	char	*var_name, *ef_name;
	Var	*vp, *evp;

	assert(scope);
	assert(defn);
	assert(defn->type == D_SYNC);

	var_name = defn->value;
	assert(var_name);

	assert(defn->sync_evflag);
	ef_name = defn->sync_evflag->value;
	assert(ef_name);

	vp = find_var(st, var_name, scope);
	if (!vp)
	{
		error_at_expr(defn, "variable '%s' not declared\n", var_name);
		return;
	}
	if (vp->sync == M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' already sync'd\n", vp->name);
		return;
	}
	evp = find_var(st, ef_name, scope);
	if (!evp)
	{
		error_at_expr(defn, "event flag '%s' not declared\n", ef_name);
		return;
	}
	if (evp->class != VC_EVFLAG)
	{
		error_at_expr(defn, "variable '%s' is not a event flag\n", ef_name);
		return;
	}
	if (defn->sync_subscr)
	{
		sync_elem(defn, vp, defn->sync_subscr, evp);
	}
	else
	{
		sync_var(defn, vp, evp);
	}
}

static void syncq_var(Expr *defn, Var *vp, SyncQ *qp)
{
	assert(defn);
	assert(vp);
	assert(qp);					/* call */
	assert(vp->syncq != M_SINGLE);			/* call */

#ifdef DEBUG
	report("syncq %s to %s;\n", vp->name, qp->ef_var->name);
#endif

	if (vp->syncq == M_MULTI)
	{
		error_at_expr(defn, "some array elements of '%s' already syncq'd\n",
			vp->name);
		return;
	}
	if (vp->monitor != M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' not monitored\n", vp->name);
		return;
	}
	if (vp->assign == M_NONE)
	{
		error_at_expr(defn, "variable '%s' not assigned\n", vp->name);
		return;
	}
	vp->syncq = M_SINGLE;
	if (vp->assign == M_SINGLE)
	{
		assert(vp->chan.single);
		assert(vp->monitor != M_MULTI);	/* by L1 */
		assert(vp->syncq != M_MULTI);	/* by L1 */
		vp->chan.single->syncq = qp;
		vp->syncq = M_SINGLE;
	}
	else
	{
		uint n;
		assert(vp->assign == M_MULTI);	/* else */
		vp->syncq = M_SINGLE;
		for (n = 0; n < vp->length1; n++)
		{
			assert(vp->chan.multi[n]->monitor);
			assert(!vp->chan.multi[n]->syncq);
			vp->chan.multi[n]->syncq = qp;
		}
	}
}

static void syncq_elem(Expr *defn, Var *vp, Expr *subscr, SyncQ *qp)
{
	uint	n_subscr;

	assert(defn);					/* syntax */
	assert(vp);					/* call */
	assert(subscr);					/* call */
	assert(subscr->type == E_CONST);		/* syntax */
	assert(qp);					/* call */

	assert(vp->syncq != M_SINGLE);			/* call */

#ifdef DEBUG
	report("syncq %s[%d] to %s;\n", vp->name, subscr->value, qp->ef_var->name);
#endif

	if (vp->class != VC_ARRAY1 && vp->class != VC_ARRAY2)
	{
		error_at_expr(defn, "variable '%s' is not an array\n", vp->name);
		return;
	}
	if (!strtoui(subscr->value, vp->length1, &n_subscr))
	{
		error_at_expr(subscr, "subscript in '%s[%s]' out of range\n",
			vp->name, subscr->value);
		return;
	}
	if (vp->assign == M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' is assigned to a single channel "
			"and can only be syncq'd wholesale\n", vp->name);
		return;
	}
	if (vp->assign == M_NONE || !vp->chan.multi[n_subscr]->name)
	{
		error_at_expr(defn, "array element '%s[%d]' not assigned\n",
			vp->name, n_subscr);
		return;
	}
	assert(vp->assign == M_MULTI);
	if (vp->monitor == M_NONE || !vp->chan.multi[n_subscr]->monitor)
	{
		error_at_expr(defn, "array element '%s[%d]' not monitored\n",
			vp->name, n_subscr);
		return;
	}
	if (vp->chan.multi[n_subscr]->syncq)
	{
		warning_at_expr(defn, "'%s[%d]' already syncq'd\n",
			vp->name, n_subscr);
		return;					/* nothing to do */
	}
	vp->syncq = M_MULTI;
	vp->chan.multi[n_subscr]->syncq = qp;		/* do it */
}

static void analyse_syncq(SymTable st, SyncQList *syncq_list, Expr *scope, Expr *defn)
{
	char	*var_name, *ef_name;
	Var	*vp, *evp;
	SyncQ	*qp;
	uint	n_size = 0;

	assert(scope);
	assert(defn);
	assert(defn->type == D_SYNC);

	var_name = defn->value;
	assert(var_name);

	assert(defn->syncq_evflag);
	ef_name = defn->syncq_evflag->value;
	assert(ef_name);

	vp = find_var(st, var_name, scope);
	if (!vp)
	{
		error_at_expr(defn, "variable '%s' not declared\n", var_name);
		return;
	}
	if (vp->syncq == M_SINGLE)
	{
		error_at_expr(defn, "variable '%s' already syncq'd\n", vp->name);
		return;
	}
	evp = find_var(st, ef_name, scope);
	if (!evp)
	{
		error_at_expr(defn, "event flag '%s' not declared\n", ef_name);
		return;
	}
	if (evp->class != VC_EVFLAG)
	{
		error_at_expr(defn, "variable '%s' is not a event flag\n", ef_name);
		return;
	}
	if (evp->chan.evflag->queued)
	{
		error_at_expr(defn, "event flag '%s' is already used for another syncq\n",
			ef_name);
		return;
	}
	if (defn->syncq_size && !strtoui(defn->syncq_size->value, UINT_MAX, &n_size))
	{
		error_at_expr(defn->syncq_size, "queue size '%s' out of range\n",
			defn->syncq_size->value);
		return;
	}
	evp->chan.evflag->queued = TRUE;
	qp = new_sync_queue(syncq_list, evp, n_size);
	if (defn->syncq_subscr)
	{
		syncq_elem(defn, vp, defn->syncq_subscr, qp);
	}
	else
	{
		syncq_var(defn, vp, qp);
	}
}

/* Allocate a channel structure for this variable, add it to the channel list,
   and initialize members index, var, and count. Also increase channel
   count in the list. */
static Chan *new_channel(ChanList *chan_list, Var *vp, uint count, uint index)
{
	Chan *cp = new(Chan);

	cp->var = vp;
	cp->count = count;
	cp->index = index;
	if (index == 0)
		vp->index = chan_list->num_elems;
	chan_list->num_elems++;
	/* add new channel to chan_list */
	if (!chan_list->first)
		chan_list->first = cp;
	else
		chan_list->last->next = cp;
	chan_list->last = cp;
	cp->next = 0;
	return cp;
}

/* Allocate a sync queue structure for event flag evp, add it to the sync queue
   list, and initialize members index, var, and size. Also increase sync queue
   count in the list. */
static SyncQ *new_sync_queue(SyncQList *syncq_list, Var *evp, uint size)
{
	SyncQ *qp = new(SyncQ);

	qp->index = syncq_list->num_elems++;
	qp->ef_var = evp;
	qp->size = size;

	/* add new syncqnel to syncq_list */
	if (!syncq_list->first)
		syncq_list->first = qp;
	else
		syncq_list->last->next = qp;
	syncq_list->last = qp;
	qp->next = 0;

	return qp;
}

/* Add a variable to a scope (append to the end of the var_list) */
void add_var(SymTable st, Var *vp, Expr *scope)
{
	VarList	*var_list = *pvar_list_from_scope(scope);

	if (!var_list->first)
		var_list->first = vp;
	else
		var_list->last->next = vp;
	var_list->last = vp;
	vp->next = 0;

	vp->scope = scope;
}

/* Find a variable by name, given a scope; first searches the given
   scope, then the parent scope, and so on. Returns a pointer to the
   var struct or 0 if the variable is not found. */
Var *find_var(SymTable st, char *name, Expr *scope)
{
	VarList *var_list = *pvar_list_from_scope(scope);
	Var	*vp;

#ifdef DEBUG
	report("searching %s in %s:%s, ", name, scope->value,
		expr_type_name(scope));
#endif
	vp = sym_table_lookup(st, name, var_list);
	if (vp)
	{
#ifdef DEBUG
		report("found\n");
#endif
		return vp;
	}
	else if (!var_list->parent_scope)
	{
#ifdef DEBUG
		report("not found\n");
#endif
		return 0;
	}
	else
		return find_var(st, name, var_list->parent_scope);
}

/* Connect a variable in an expression (E_VAR) to the Var structure.
   If there is no such structure, e.g. because the variable has not been
   declared, then allocate one, assign type V_NONE, and assign the most
   local scope for the variable. */
static int connect_variable(Expr *ep, Expr *scope, void *parg)
{
	SymTable	st = *(SymTable *)parg;
	Var		*vp;

	assert(ep);
	assert(ep->type == E_VAR);
	assert(scope);

#ifdef DEBUG
	report("connect_variable: %s, line %d\n", ep->value, ep->line_num);
#endif

	vp = find_var(st, ep->value, scope);

#ifdef DEBUG
	if (vp)
	{
		report_at_expr(ep, "var %s found in scope (%s:%s)\n", ep->value,
			expr_type_name(scope),
			scope->value);
	}
	else
		report_at_expr(ep, "var %s not found\n", ep->value);
#endif
	if (!vp)
	{
		VarList *var_list = *pvar_list_from_scope(scope);

		warning_at_expr(ep, "variable '%s' used but not declared\n",
			ep->value);
		/* create a pseudo declaration so we can finish the analysis phase */
		vp = new(Var);
		vp->name = ep->value;
		vp->type = V_NONE;	/* undeclared type */
		vp->class = VC_FOREIGN;
		vp->length1 = 1;
		vp->length2 = 1;
		vp->value = 0;
		/* add this variable to the top-level scope, NOT the current scope */
		while (var_list->parent_scope) {
			var_list = *pvar_list_from_scope(var_list->parent_scope);
		}
		sym_table_insert(st, vp->name, var_list, vp);
		add_var(st, vp, scope);
	}
	ep->extra.e_var = vp;		/* make connection */
	return FALSE;			/* there are no children anyway */
}

static void connect_variables(SymTable st, Expr *scope)
{
#ifdef DEBUG
	report("**begin** connect_variables\n");
#endif
	traverse_expr_tree(scope, 1<<E_VAR, ~has_sub_expr_mask,
		0, connect_variable, &st);
#ifdef DEBUG
	report("**end** connect_variables\n");
#endif
}

void traverse_expr_tree(
	Expr		*ep,		/* start expression */
	int		call_mask,	/* when to call iteratee */
	int		stop_mask,	/* when to stop descending */
	Expr		*scope,		/* current scope, 0 at top-level */
	expr_iter	*iteratee,	/* function to call */
	void		*parg		/* argument to pass to function */
)
{
	Expr	*cep;
	int	i;
	int	descend = TRUE;

	if (!ep)
		return;

#ifdef DEBUG
	report("traverse_expr_tree(type=%s,value=%s)\n",
		expr_type_name(ep), ep->value);
#endif

	/* Call the function? */
	if (call_mask & (1<<ep->type))
	{
		descend = iteratee(ep, scope, parg);
	}

	if (!descend)
		return;

	/* Are we just entering a new scope? */
	if (is_scope(ep))
	{
#ifdef DEBUG
	report("traverse_expr_tree: new scope=(%s,%s)\n",
		expr_type_name(ep), ep->value);
#endif
		scope = ep;
	}

	/* Descend into children */
	for (i = 0; i < expr_type_info[ep->type].num_children; i++)
	{
		foreach (cep, ep->children[i])
		{
			if (cep && !((1<<cep->type) & stop_mask))
			{
				traverse_expr_tree(cep, call_mask, stop_mask,
					scope, iteratee, parg);
			}
		}
	}
}

static int assign_next_delay_id(Expr *ep, Expr *scope, void *parg)
{
	int *delay_id = (int *)parg;

	assert(ep->type == E_DELAY);
	ep->extra.e_delay = *delay_id;
	*delay_id += 1;
	return FALSE;	/* delays cannot be nested as they do not return a value */
}

/* Check for duplicate state set and state names and resolve transitions between states */
static uint connect_states(SymTable st, Expr *prog)
{
	Expr	*ssp;
	int	num_ss = 0;

	foreach (ssp, prog->prog_statesets)
	{
		Expr *sp;
		int num_states = 0;

#ifdef DEBUG
		report("connect_states: ss = %s\n", ssp->value);
#endif
		if (!sym_table_insert(st, ssp->value, prog, ssp))
		{
			Expr *ssp2 = sym_table_lookup(st, ssp->value, prog);
			error_at_expr(ssp,
				"a state set with name '%s' was already "
				"declared at line %d\n", ssp->value, ssp2->line_num);
		}
		foreach (sp, ssp->ss_states)
		{
			if (!sym_table_insert(st, sp->value, ssp, sp))
			{
				Expr *sp2 = sym_table_lookup(st, sp->value, ssp);
				error_at_expr(sp,
					"a state with name '%s' in state set '%s' "
					"was already declared at line %d\n",
					sp->value, ssp->value, sp2->line_num);
			}
			assert(sp->extra.e_state);
#ifdef DEBUG
			report("connect_states: ss = %s, state = %s, index = %d\n",
				ssp->value, sp->value, num_states);
#endif
			sp->extra.e_state->index = num_states++;
		}
		ssp->extra.e_ss->num_states = num_states;
#ifdef DEBUG
		report("connect_states: ss = %s, num_states = %d\n", ssp->value, num_states);
#endif
		foreach (sp, ssp->ss_states)
		{
			Expr *tp;
			/* Each state has its own delay ids */
			int delay_id = 0;

			foreach (tp, sp->state_whens)
			{
				Expr *next_sp = sym_table_lookup(st, tp->value, ssp);

				if (!next_sp)
				{
					error_at_expr(tp,
						"a state with name '%s' does not "
						"exist in state set '%s'\n",
					 	tp->value, ssp->value);
				}
				tp->extra.e_when->next_state = next_sp;
				assert(!next_sp || strcmp(tp->value,next_sp->value) == 0);
#ifdef DEBUG
				report("connect_states: ss = %s, state = %s, when(...){...} state (%s,%d)\n",
					ssp->value, sp->value, tp->value, next_sp->extra.e_state->index);
#endif
				/* assign delay ids */
				traverse_expr_tree(tp->when_cond, 1<<E_DELAY, 0, 0,
					assign_next_delay_id, &delay_id);
			}
		}
		num_ss++;
	}
	return num_ss;
}

typedef struct {
	SymTable	st;
	Expr		*ssp;
	int		in_when;
} connect_state_change_arg;

static int iter_connect_state_change_stmts(Expr *ep, Expr *scope, void *parg)
{
	connect_state_change_arg *pcsc_arg = (connect_state_change_arg *)parg;

	assert(pcsc_arg);
	assert(ep);
	if (ep->type == D_SS)
	{
		pcsc_arg->ssp = ep;
		return TRUE;
	}
	else if (ep->type == D_ENTRY || ep->type == D_EXIT)
	{
		/* to flag erroneous state change statements, see below */
		pcsc_arg->in_when = FALSE;
		return TRUE;
	}
	else if (ep->type == D_WHEN)
	{
		pcsc_arg->in_when = TRUE;
		return TRUE;
	}
	else
	{
		assert(ep->type == S_CHANGE);
		if (!pcsc_arg->ssp || !pcsc_arg->in_when)
		{
			error_at_expr(ep, "state change statement not allowed here\n");
		}
		else
		{
			Expr *sp = sym_table_lookup(pcsc_arg->st, ep->value, pcsc_arg->ssp);
			if (!sp)
			{
				error_at_expr(ep,
					"a state with name '%s' does not "
					"exist in state set '%s'\n",
				 	ep->value, pcsc_arg->ssp->value);
				return FALSE;
			}
			ep->extra.e_change = sp;
		}
		return FALSE;
	}
}

static void connect_state_change_stmts(SymTable st, Expr *scope)
{
	connect_state_change_arg csc_arg = {st, 0, FALSE};
	traverse_expr_tree(scope,
		(1<<S_CHANGE)|(1<<D_SS)|(1<<D_ENTRY)|(1<<D_EXIT)|(1<<D_WHEN),
		expr_mask, 0, iter_connect_state_change_stmts, &csc_arg);
}

/* Assign event bits to event flags and associate pv channels with
 * event flags. Return number of event flags found.
 */
static uint assign_ef_bits(Expr *scope)
{
	Var	*vp;
	int	num_event_flags = 0;
	VarList	*var_list;

	var_list = *pvar_list_from_scope(scope);

	/* Assign event flag numbers (starting at 1) */
	foreach (vp, var_list->first)
	{
		if (vp->class == VC_EVFLAG)
		{
			vp->chan.evflag->index = ++num_event_flags;
		}
	}
	return num_event_flags;
}