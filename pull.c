/*  This file is part of "reprepro"
 *  Copyright (C) 2006,2007 Bernhard R. Link
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02111-1301  USA
 */
#include <config.h>

#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"
#include "ignore.h"
#include "mprintf.h"
#include "strlist.h"
#include "names.h"
#include "pull.h"
#include "upgradelist.h"
#include "distribution.h"
#include "terms.h"
#include "filterlist.h"
#include "log.h"
#include "configparser.h"

extern int verbose;

/***************************************************************************
 * step one:                                                               *
 * parse CONFDIR/pull to get pull information saved in                     *
 * pull_rule structs                                                    *
 **************************************************************************/

/* the data for some upstream part to get pull from, some
 * some fields can be NULL or empty */
struct pull_rule {
	struct pull_rule *next;
	//e.g. "Name: woody"
	char *name;
	//e.g. "From: woody"
	char *from;
	//e.g. "Architectures: i386 sparc mips" (not set means all)
	struct strlist architectures_from;
	struct strlist architectures_into;
	bool architectures_set;
	//e.g. "Components: main contrib" (not set means all)
	struct strlist components;
	bool components_set;
	//e.g. "UDebComponents: main" // (not set means all)
	struct strlist udebcomponents;
	bool udebcomponents_set;
	// NULL means no condition
	/*@null@*/term *includecondition;
	struct filterlist filterlist;
	/*----only set after _addsourcedistribution----*/
	/*@NULL@*/ struct distribution *distribution;
	bool used;
};

static void pull_rule_free(/*@only@*/struct pull_rule *pull) {
	if( pull == NULL )
		return;
	free(pull->name);
	free(pull->from);
	strlist_done(&pull->architectures_from);
	strlist_done(&pull->architectures_into);
	strlist_done(&pull->components);
	strlist_done(&pull->udebcomponents);
	term_free(pull->includecondition);
	filterlist_release(&pull->filterlist);
	free(pull);
}

void pull_freerules(struct pull_rule *p) {
	while( p != NULL ) {
		struct pull_rule *rule;

		rule = p;
		p = rule->next;
		pull_rule_free(rule);
	}
}

CFlinkedlistinit(pull_rule)
CFvalueSETPROC(pull_rule, name)
CFvalueSETPROC(pull_rule, from)
CFuniqstrlistSETPROCset(pull_rule, components)
CFuniqstrlistSETPROCset(pull_rule, udebcomponents)
CFfilterlistSETPROC(pull_rule, filterlist)
CFtermSETPROC(pull_rule, includecondition)

CFUSETPROC(pull_rule, architectures) {
	CFSETPROCVAR(pull_rule, this);
	retvalue r;

	this->architectures_set = true;
	r = config_getsplitwords(iter, "Architectures",
			&this->architectures_from,
			&this->architectures_into);
	if( r == RET_NOTHING ) {
		fprintf(stderr,
"Warning parsing %s, line %u: an empty Architectures field\n"
"causes the whole rule to do nothing.\n",
				config_filename(iter),
				config_markerline(iter));
	}
	return r;
}

static const struct configfield pullconfigfields[] = {
	CFr("Name", pull_rule, name),
	CFr("From", pull_rule, from),
	CF("Architectures", pull_rule, architectures),
	CF("Components", pull_rule, components),
	CF("UDebComponents", pull_rule, udebcomponents),
	CF("FilterFormula", pull_rule, includecondition),
	CF("FilterList", pull_rule, filterlist)
};

retvalue pull_getrules(struct pull_rule **rules) {
	struct pull_rule *pull = NULL;
	retvalue r;

	r = configfile_parse("pulls", IGNORABLE(unknownfield),
			configparser_pull_rule_init, linkedlistfinish,
			pullconfigfields, ARRAYCOUNT(pullconfigfields), &pull);
	if( RET_IS_OK(r) )
		*rules = pull;
	else if( r == RET_NOTHING ) {
		assert( pull == NULL );
		*rules = NULL;
		r = RET_OK;
	} else {
		// TODO special handle unknownfield
		pull_freerules(pull);
	}
	return r;
}

/***************************************************************************
 * step two:                                                               *
 * create pull_distribution structs to hold all additional information for *
 * a distribution                                                          *
 **************************************************************************/

struct pull_target;
static void pull_freetargets(struct pull_target *targets);

struct pull_distribution {
	struct pull_distribution *next;
	/*@dependant@*/struct distribution *distribution;
	struct pull_target *targets;
	/*@dependant@*/struct pull_rule *rules[];
};

void pull_freedistributions(struct pull_distribution *d) {
	while( d != NULL ) {
		struct pull_distribution *next;

		next = d->next;
		pull_freetargets(d->targets);
		free(d);
		d = next;
	}
}

static retvalue pull_initdistribution(struct pull_distribution **pp,
		struct distribution *distribution,
		struct pull_rule *rules) {
	struct pull_distribution *p;
	int i;

	assert(distribution != NULL);
	if( distribution->pulls.count == 0 )
		return RET_NOTHING;

	p = malloc(sizeof(struct pull_distribution)+
		sizeof(struct pull_rules *)*distribution->pulls.count);
	if( p == NULL )
		return RET_ERROR_OOM;
	p->next = NULL;
	p->distribution = distribution;
	p->targets = NULL;
	for( i = 0 ; i < distribution->pulls.count ; i++ ) {
		const char *name = distribution->pulls.values[i];
		if( strcmp(name,"-") == 0 ) {
			p->rules[i] = NULL;
		} else {
			struct pull_rule *rule = rules;
			while( rule && strcmp(rule->name,name) != 0 )
				rule = rule->next;
			if( rule == NULL ) {
				fprintf(stderr,
"Error: Unknown pull rule '%s' in distribution '%s'!\n",
						name, distribution->codename);
				return RET_ERROR_MISSING;
			}
			p->rules[i] = rule;
			rule->used = true;
		}
	}
	*pp = p;
	return RET_OK;
}

static retvalue pull_init(struct pull_distribution **pulls,
		struct pull_rule *rules,
		struct distribution *distributions) {
	struct pull_distribution *p = NULL, **pp = &p;
	struct distribution *d;
	retvalue r;

	for( d = distributions ; d != NULL ; d = d->next ) {
		if( !d->selected )
			continue;
		r = pull_initdistribution(pp, d, rules);
		if( RET_WAS_ERROR(r) ) {
			pull_freedistributions(p);
			return r;
		}
		if( RET_IS_OK(r) ) {
			assert( *pp != NULL );
			pp = &(*pp)->next;
		}
	}
	*pulls = p;
	return RET_OK;
}

/***************************************************************************
 * step three:                                                             *
 * load the config of the distributions mentioned in the rules             *
 **************************************************************************/

static retvalue pull_loadsourcedistributions(struct distribution *alldistributions, struct pull_rule *rules) {
	struct pull_rule *rule;
	struct distribution *d;

	for( rule = rules ; rule != NULL ; rule = rule->next ) {
		if( rule->used && rule->distribution == NULL ) {
			for( d = alldistributions ; d != NULL ; d = d->next ) {
				if( strcmp(d->codename, rule->from) == 0 ) {
					rule->distribution = d;
					break;
				}
			}
			if( d == NULL ) {
				fprintf(stderr, "Error: Unknown distribution '%s' referenced in pull rule '%s'\n",
						rule->from, rule->name);
				return RET_ERROR_MISSING;
			}
		}
	}
	return RET_OK;
}

/***************************************************************************
 * step four:                                                              *
 * create pull_targets and pull_sources                                    *
 **************************************************************************/

struct pull_source {
	struct pull_source *next;
	/* NULL, if this is a delete rule */
	struct target *source;
	struct pull_rule *rule;
};
struct pull_target {
	/*@null@*/struct pull_target *next;
	/*@null@*/struct pull_source *sources;
	/*@dependent@*/struct target *target;
	/*@null@*/struct upgradelist *upgradelist;
	/* Ignore delete marks (as some lists were missing) */
	bool ignoredelete;
};

static void pull_freetargets(struct pull_target *targets) {
	while( targets != NULL ) {
		struct pull_target *target = targets;
		targets = target->next;
		while( target->sources != NULL ) {
			struct pull_source *source = target->sources;
			target->sources = source->next;
			free(source);
		}
		free(target);
	}
}

static retvalue pull_createsource(struct pull_rule *rule,
		struct target *target,
		struct pull_source ***s) {
	const struct strlist *c;
	const struct strlist *a_from,*a_into;
	int ai;

	assert( rule != NULL );
	assert( rule->distribution != NULL );

	if( rule->architectures_set ) {
		a_from = &rule->architectures_from;
		a_into = &rule->architectures_into;
	} else {
		a_from = &rule->distribution->architectures;
		a_into = &rule->distribution->architectures;
	}
	if( strcmp(target->packagetype,"udeb") == 0 )  {
		if( rule->udebcomponents_set )
			c = &rule->udebcomponents;
		else
			c = &rule->distribution->udebcomponents;
	} else {
		if( rule->components_set )
			c = &rule->components;
		else
			c = &rule->distribution->components;
	}

	if( !strlist_in(c, target->component) )
		return RET_NOTHING;

	for( ai = 0 ; ai < a_into->count ; ai++ ) {
		struct pull_source *source;
		if( strcmp(a_into->values[ai],target->architecture) != 0 )
			continue;

		source = malloc(sizeof(struct pull_source));
		if( source == NULL )
			return RET_ERROR_OOM;

		source->next = NULL;
		source->rule = rule;
		source->source = distribution_getpart(rule->distribution,
				target->component, a_from->values[ai],
				target->packagetype);
		**s = source;
		*s = &source->next;
	}
	return RET_OK;
}

static retvalue pull_createdelete(struct pull_source ***s) {
	struct pull_source *source;

	source = malloc(sizeof(struct pull_source));
	if( source == NULL )
		return RET_ERROR_OOM;

	source->next =  NULL;
	source->rule = NULL;
	source->source = NULL;
	**s = source;
	*s = &source->next;
	return RET_OK;
}

static retvalue generatepulltarget(struct pull_distribution *pd, struct target *target) {
	struct pull_source **s;
	struct pull_target *pt;
	retvalue r;
	int i;

	pt = malloc(sizeof(struct pull_target));
	if( pt == NULL ) {
		return RET_ERROR_OOM;
	}
	pt->target = target;
	pt->next = pd->targets;
	pt->upgradelist = NULL;
	pt->ignoredelete = false;
	pt->sources = NULL;
	s = &pt->sources;
	pd->targets = pt;

	for( i = 0 ; i < pd->distribution->pulls.count ; i++ ) {
		struct pull_rule *rule = pd->rules[i];

		if( rule == NULL)
			r = pull_createdelete(&s);
		else
			r = pull_createsource(rule, target, &s);
		if( RET_WAS_ERROR(r) )
			return r;
	}

	return RET_OK;
}

static retvalue pull_generatetargets(struct pull_distribution *pull_distributions) {
	struct pull_distribution *pd;
	struct target *target;
	struct pull_distribution *u_ds;
	retvalue r;

	u_ds = NULL;

	for( pd = pull_distributions ; pd != NULL ; pd = pd->next ) {
		for( target = pd->distribution->targets ; target != NULL ;
				target = target->next) {

			r = generatepulltarget(pd,target);
			if( RET_WAS_ERROR(r) )
				return r;
		}
	}
	return RET_OK;
}

/***************************************************************************
 * Some checking to be able to warn against typos                          *
 **************************************************************************/

static inline void markasused(const struct strlist *pulls, const char *rulename, const struct strlist *needed, const struct strlist *have, bool *found) {
	int i, j, o;

	for( i = 0 ; i < pulls->count ; i++ ) {
		if( strcmp(pulls->values[i], rulename) != 0 )
			continue;
		for( j = 0 ; j < have->count ; j++ ) {
			o = strlist_ofs(needed, have->values[j]);
			if( o >= 0 )
				found[o] = true;
		}
	}
}

static void checkifarchitectureisused(const struct strlist *architectures, const struct distribution *alldistributions, const struct pull_rule *rule, const char *action) {
	bool *found;
	const struct distribution *d;
	int i;

	assert( rule != NULL );
	if( architectures->count == 0 )
		return;
	found = strlist_preparefoundlist(architectures, true);
	if( found == NULL )
		return;
	for( d = alldistributions ; d != NULL ; d = d->next ) {
		markasused(&d->pulls, rule->name,
				architectures, &d->architectures,
				found);
	}
	for( i = 0 ; i < architectures->count ; i++ ) {
		if( found[i] )
			continue;
		fprintf(stderr,
"Warning: pull rule '%s' wants to %s architecture '%s',\n"
"but no distribution using this has such an architecture.\n"
"(This will simply be ignored and is not even checked when using --fast).\n",
				rule->name, action,
				architectures->values[i]);
	}
	free(found);
	return;
}

static void checkifcomponentisused(const struct strlist *components, const struct distribution *alldistributions, const struct pull_rule *rule, const char *action) {
	bool *found;
	const struct distribution *d;
	int i;

	assert( rule != NULL );
	if( components->count == 0 )
		return;
	found = strlist_preparefoundlist(components, true);
	if( found == NULL )
		return;
	for( d = alldistributions ; d != NULL ; d = d->next ) {
		markasused(&d->pulls, rule->name,
				components, &d->components,
				found);
	}
	for( i = 0 ; i < components->count ; i++ ) {
		if( found[i] )
			continue;
		fprintf(stderr,
"Warning: pull rule '%s' wants to %s component '%s',\n"
"but no distribution using this has such an component.\n"
"(This will simply be ignored and is not even checked when using --fast).\n",
				rule->name, action,
				components->values[i]);
	}
	free(found);
	return;
}

static void checkifudebcomponentisused(const struct strlist *udebcomponents, const struct distribution *alldistributions, const struct pull_rule *rule, const char *action) {
	bool *found;
	const struct distribution *d;
	int i;

	assert( rule != NULL );
	if( udebcomponents->count == 0 )
		return;
	found = strlist_preparefoundlist(udebcomponents, true);
	if( found == NULL )
		return;
	for( d = alldistributions ; d != NULL ; d = d->next ) {
		markasused(&d->pulls, rule->name,
				udebcomponents, &d->udebcomponents,
				found);
	}
	for( i = 0 ; i < udebcomponents->count ; i++ ) {
		if( found[i] )
			continue;
		fprintf(stderr,
"Warning: pull rule '%s' wants to %s udeb component '%s',\n"
"but no distribution using this has such an udeb component.\n"
"(This will simply be ignored and is not even checked when using --fast).\n",
				rule->name, action,
				udebcomponents->values[i]);
	}
	free(found);
	return;
}

static void checksubset(const struct strlist *needed, const struct strlist *have, const char *rulename, const char *from, const char *what) {
	int i, j;

	for( i = 0 ; i < needed->count ; i++ ) {
		const char *value = needed->values[i];

		if( strcmp(value, "none") == 0 )
			continue;

		for( j = 0 ; j < i ; j++ ) {
			if( strcmp(value, needed->values[j]) == 0 )
				break;
		}
		if( j < i )
			continue;

		if( !strlist_in(have, value) ) {
			fprintf(stderr,
"Warning: pull rule '%s' wants to get something from %s '%s',\n"
"but there is no such %s in distribution '%s'.\n"
"(This will simply be ignored and is not even checked when using --fast).\n",
					rulename, what, value, what, from);
		}
	}
}

static void searchunused(const struct distribution *alldistributions, const struct pull_rule *rule) {
	if( rule->distribution != NULL ) {
		// TODO: move this part of the checks into parsing?
		checksubset(&rule->architectures_from,
				&rule->distribution->architectures,
				rule->name, rule->from, "architecture");
		checksubset(&rule->components,
				&rule->distribution->components,
				rule->name, rule->from, "component");
		checksubset(&rule->udebcomponents,
				&rule->distribution->udebcomponents,
				rule->name, rule->from, "udeb component");
	}

	if( rule->distribution == NULL ) {
		assert( strcmp(rule->from, "*") == 0 );
		checkifarchitectureisused(&rule->architectures_from,
				alldistributions, rule, "get something from");
		/* no need to check component and udebcomponent, as those
		 * are the same with the others */
	}
	checkifarchitectureisused(&rule->architectures_into,
			alldistributions, rule, "put something into");
	checkifcomponentisused(&rule->components,
			alldistributions, rule, "put something into");
	checkifudebcomponentisused(&rule->udebcomponents,
			alldistributions, rule, "put something into");
}

static void pull_searchunused(const struct distribution *alldistributions, struct pull_rule *pull_rules) {
	struct pull_rule *rule;

	for( rule = pull_rules ; rule != NULL ; rule = rule->next ) {
		if( !rule->used )
			continue;

		searchunused(alldistributions, rule);
	}
}

/***************************************************************************
 * combination of the steps two, three and four                            *
 **************************************************************************/

retvalue pull_prepare(struct distribution *alldistributions, struct pull_rule *rules, bool fast, struct pull_distribution **pd) {
	struct pull_distribution *pulls;
	retvalue r;

	r = pull_init(&pulls, rules, alldistributions);
	if( RET_WAS_ERROR(r) )
		return r;

	r = pull_loadsourcedistributions(alldistributions, rules);
	if( RET_WAS_ERROR(r) ) {
		pull_freedistributions(pulls);
		return r;
	}
	if( !fast )
		pull_searchunused(alldistributions, rules);

	r = pull_generatetargets(pulls);
	if( RET_WAS_ERROR(r) ) {
		pull_freedistributions(pulls);
		return r;
	}
	*pd = pulls;
	return RET_OK;
}

/***************************************************************************
 * step five:                                                              *
 * decide what gets pulled                                                 *
 **************************************************************************/

static upgrade_decision ud_decide_by_rule(void *privdata, const char *package,UNUSED(const char *old_version),UNUSED(const char *new_version),const char *newcontrolchunk) {
	struct pull_rule *rule = privdata;
	retvalue r;

	switch( filterlist_find(package,&rule->filterlist) ) {
		case flt_deinstall:
		case flt_purge:
			return UD_NO;
		case flt_hold:
			return UD_HOLD;
		case flt_error:
			/* cannot yet be handled! */
			fprintf(stderr,
"Package name marked to be unexpected('error'): '%s'!\n", package);
			return UD_ERROR;
		case flt_install:
			break;
	}

	if( rule->includecondition != NULL ) {
		r = term_decidechunk(rule->includecondition,newcontrolchunk);
		if( RET_WAS_ERROR(r) )
			return UD_ERROR;
		if( r == RET_NOTHING ) {
			return UD_NO;
		}
	}

	return UD_UPGRADE;
}

static inline retvalue pull_searchformissing(FILE *out,struct database *database,struct pull_target *p) {
	struct pull_source *source;
	retvalue result,r;

	if( verbose > 2 )
		fprintf(out,"  pulling into '%s'\n",p->target->identifier);
	assert(p->upgradelist == NULL);
	r = upgradelist_initialize(&p->upgradelist, p->target, database);
	if( RET_WAS_ERROR(r) )
		return r;

	result = RET_NOTHING;

	for( source=p->sources ; source != NULL ; source=source->next ) {

		if( source->rule == NULL ) {
			if( verbose > 4 )
				fprintf(out,"  marking everything to be deleted\n");
			r = upgradelist_deleteall(p->upgradelist);
			RET_UPDATE(result,r);
			if( RET_WAS_ERROR(r) )
				return result;
			p->ignoredelete = false;
			continue;
		}

		if( verbose > 4 )
			fprintf(out,"  looking what to get from '%s'\n",
					source->source->identifier);
		r = upgradelist_pull(p->upgradelist,
				source->source,
				ud_decide_by_rule, source->rule,
				database);
		RET_UPDATE(result,r);
		if( RET_WAS_ERROR(r) )
			return result;
	}

	return result;
}

static retvalue pull_search(FILE *out,struct database *database,struct pull_distribution *d) {
	retvalue result,r;
	struct pull_target *u;

	if( d->distribution->deb_override != NULL ||
			d->distribution->dsc_override != NULL ||
			d->distribution->udeb_override != NULL ) {
		if( verbose >= 0 )
			fprintf(stderr,
"Warning: Override files of '%s' ignored as not yet supported while updating!\n",
					d->distribution->codename);
	}
	if( d->distribution->tracking != dt_NONE ) {
		fprintf(stderr,
"WARNING: Pull does not yet update tracking data. Tracking data of %s will be outdated!\n",
					d->distribution->codename);
	}

	result = RET_NOTHING;
	for( u=d->targets ; u != NULL ; u=u->next ) {
		r = pull_searchformissing(out, database, u);
		RET_UPDATE(result,r);
		if( RET_WAS_ERROR(r) )
			break;
	}
	return result;
}

static retvalue pull_install(struct database *database,struct pull_distribution *distribution,struct strlist *dereferencedfilekeys) {
	retvalue result,r;
	struct pull_target *u;

	assert( logger_isprepared(distribution->distribution->logger) );

	result = RET_NOTHING;
	for( u=distribution->targets ; u != NULL ; u=u->next ) {
		r = upgradelist_install(u->upgradelist,
				distribution->distribution->logger,
				database,
				u->ignoredelete, dereferencedfilekeys);
		RET_UPDATE(distribution->distribution->status, r);
		RET_UPDATE(result,r);
		upgradelist_free(u->upgradelist);
		u->upgradelist = NULL;
		if( RET_WAS_ERROR(r) )
			break;
	}
	return result;
}

static void pull_dump(struct pull_distribution *distribution) {
	struct pull_target *u;

	for( u=distribution->targets ; u != NULL ; u=u->next ) {
		if( u->upgradelist == NULL )
			continue;
		printf("Updates needed for '%s':\n",u->target->identifier);
		upgradelist_dump(u->upgradelist);
		upgradelist_free(u->upgradelist);
		u->upgradelist = NULL;
	}
}

retvalue pull_update(struct database *database,struct pull_distribution *distributions,struct strlist *dereferencedfilekeys) {
	retvalue result,r;
	struct pull_distribution *d;

	for( d=distributions ; d != NULL ; d=d->next) {
		r = distribution_prepareforwriting(d->distribution);
		if( RET_WAS_ERROR(r) )
			return r;
	}

	if( verbose >= 0 )
		printf("Calculating packages to pull...\n");

	result = RET_NOTHING;

	for( d=distributions ; d != NULL ; d=d->next) {
		r = pull_search(stdout, database, d);
		RET_UPDATE(result,r);
		if( RET_WAS_ERROR(r) )
			break;
		// TODO: make already here sure the files are ready?
	}
	if( RET_WAS_ERROR(result) ) {
		for( d=distributions ; d != NULL ; d=d->next) {
			struct pull_target *u;
			for( u=d->targets ; u != NULL ; u=u->next ) {
				upgradelist_free(u->upgradelist);
				u->upgradelist = NULL;
			}
		}
		return result;
	}
	if( verbose >= 0 )
		printf("Installing (and possibly deleting) packages...\n");

	for( d=distributions ; d != NULL ; d=d->next) {
		r = pull_install(database, d, dereferencedfilekeys);
		RET_UPDATE(result,r);
		if( RET_WAS_ERROR(r) )
			break;
	}
	logger_wait();

	return result;
}

retvalue pull_checkupdate(struct database *database, struct pull_distribution *distributions) {
	struct pull_distribution *d;
	retvalue result,r;

	if( verbose >= 0 )
		fprintf(stderr,"Calculating packages to get...\n");

	result = RET_NOTHING;

	for( d=distributions ; d != NULL ; d=d->next) {
		r = pull_search(stderr, database, d);
		RET_UPDATE(result,r);
		if( RET_WAS_ERROR(r) )
			break;
		pull_dump(d);
	}

	return result;
}
