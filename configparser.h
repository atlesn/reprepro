#ifndef REPREPRO_CONFIGPARSER_H
#define REPREPRO_CONFIGPARSER_H

#ifndef REPREPRO_STRLIST_H
#include "strlist.h"
#endif
#ifndef REPREPRO_CHECKS_H
#include "checks.h"
#endif

struct configiterator;

typedef retvalue configsetfunction(void *,const char *,void *,struct configiterator *);
typedef retvalue configinitfunction(void *,void *,void **);
typedef retvalue configfinishfunction(void *, void *, void **, bool, struct configiterator *);

retvalue linkedlistfinish(void *, void *, void **, bool, struct configiterator *);

struct configfield {
	const char *name;
	size_t namelen;
	/* privdata, allocated struct, iterator */
	configsetfunction *setfunc;
	bool required;
};

struct constant {
	const char *name;
	int value;
};

#define CFr(name, sname, field) {name, sizeof(name)-1, configparser_ ## sname ## _set_ ## field, true}
#define CF(name, sname, field) {name, sizeof(name)-1, configparser_ ## sname ## _set_ ## field, false}

/*@observer@*/const char *config_filename(const struct configiterator *) __attribute__((pure));
unsigned int config_line(const struct configiterator *) __attribute__((pure));
unsigned int config_column(const struct configiterator *) __attribute__((pure));
unsigned int config_firstline(const struct configiterator *) __attribute__((pure));
unsigned int config_markerline(const struct configiterator *) __attribute__((pure));
unsigned int config_markercolumn(const struct configiterator *) __attribute__((pure));
retvalue config_getflags(struct configiterator *, const char *, const struct constant *, bool *, bool, const char *msg);
int config_nextnonspaceinline(struct configiterator *iter);
retvalue config_getlines(struct configiterator *,struct strlist *);
retvalue config_getwords(struct configiterator *,struct strlist *);
retvalue config_getall(struct configiterator *iter, /*@out@*/char **result_p);
retvalue config_getword(struct configiterator *, /*@out@*/char **);
retvalue config_getwordinline(struct configiterator *, /*@out@*/char **);
retvalue config_getonlyword(struct configiterator *, const char *, checkfunc, /*@out@*/char **);
retvalue config_getuniqwords(struct configiterator *, const char *, checkfunc, struct strlist *);
retvalue config_getsplitwords(struct configiterator *, const char *, struct strlist *, struct strlist *);
retvalue config_gettruth(struct configiterator *, const char *, bool *);
retvalue config_getnumber(struct configiterator *, const char *, long long *, long long minval, long long maxval);
retvalue config_getconstant(struct configiterator *, const struct constant *, int *);
#define config_getenum(iter,type,constants,result) ({int _val;retvalue _r = config_getconstant(iter, type ## _ ## constants, &_val);*(result) = (enum type)_val;_r;})
retvalue config_completeword(struct configiterator *, char, /*@out@*/char **);
void config_overline(struct configiterator *);
bool config_nextline(struct configiterator *);
retvalue configfile_parse(const char *filename, bool ignoreunknown, configinitfunction, configfinishfunction, const struct configfield *, size_t, void *privdata);

#define CFlinkedlistinit(sname) \
static retvalue configparser_ ## sname ## _init(void *rootptr, void *lastitem, void **newptr) { \
	struct sname *n, **root_p = rootptr, *last = lastitem; \
	n = calloc(1, sizeof(struct sname)); \
	if( n == NULL ) \
		return RET_ERROR_OOM; \
	if( last == NULL ) \
		*root_p = n; \
	else \
		last->next = n; \
	*newptr = n; \
	return RET_OK; \
}
#define CFcheckvalueSETPROC(sname, field, checker) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_getonlyword(iter, name, checker, &item->field); \
}
#define CFvalueSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_getonlyword(iter, name, NULL, &item->field); \
}
#define CFscriptSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	char *value, *fullvalue; retvalue r; \
	r = config_getonlyword(iter, name, NULL, &value); \
	if( RET_IS_OK(r) ) { \
		assert( value != NULL && value[0] != '\0' ); \
		if( value[0] != '/' ) { \
			fullvalue = calc_dirconcat(global.confdir, value); \
			free(value); \
		} else \
			fullvalue = value; \
		if( fullvalue == NULL ) \
			return RET_ERROR_OOM; \
		item->field = fullvalue; \
	} \
	return r; \
}
#define CFlinelistSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), UNUSED(const char *name), void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_getlines(iter, &item->field); \
}
#define CFstrlistSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), UNUSED(const char *name), void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_getwords(iter, &item->field); \
}
#define CFcheckuniqstrlistSETPROC(sname, field, checker) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	retvalue r; \
	r = config_getuniqwords(iter, name, checker, &item->field); \
	if( r == RET_NOTHING ) { \
		fprintf(stderr, \
"Error parsing %s, line %d, column %d:\n" \
" An empty %s-field is not allowed.\n", config_filename(iter), \
					config_line(iter), \
					config_column(iter), \
					name); \
		r = RET_ERROR; \
	} \
	return r; \
}
#define CFuniqstrlistSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_getuniqwords(iter, name, NULL, &item->field); \
}
#define CFuniqstrlistSETPROCset(sname, name) \
static retvalue configparser_ ## sname ## _set_ ## name (UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	item->name ## _set = true; \
	return config_getuniqwords(iter, name, NULL, &item->name); \
}
#define CFsplitstrlistSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	item->field ## _set = true; \
	return config_getsplitwords(iter, name, &item->field ## _from, &item->field ## _into); \
}
#define CFtruthSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_gettruth(iter, name, &item->field); \
}
#define CFtruthSETPROC2(sname, name, field) \
static retvalue configparser_ ## sname ## _set_ ## name(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_gettruth(iter, name, &item->field); \
}
#define CFallSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), UNUSED(const char *name), void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return config_getall(iter, &item->field); \
}
#define CFfilterlistSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), UNUSED(const char *name), void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return filterlist_load(&item->field, iter); \
}
#define CFexportmodeSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), UNUSED(const char *name), void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	return exportmode_set(&item->field, iter); \
}
#define CFUSETPROC(sname,field) static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), UNUSED(const char *name), void *thisdata_ ## sname, struct configiterator *iter)
#define CFuSETPROC(sname,field) static retvalue configparser_ ## sname ## _set_ ## field(void *privdata_ ## sname, UNUSED(const char *name), void *thisdata_ ## sname, struct configiterator *iter)
#define CFSETPROC(sname,field) static retvalue configparser_ ## sname ## _set_ ## field(void *privdata_ ## sname, const char *headername, void *thisdata_ ## sname, struct configiterator *iter)
#define CFSETPROCVARS(sname,item,mydata) struct sname *item = thisdata_ ## sname; struct read_ ## sname ## _data *mydata = privdata_ ## sname
#define CFSETPROCVAR(sname,item) struct sname *item = thisdata_ ## sname

#define CFstartparse(sname) static retvalue startparse ## sname(UNUSED(void *dummyprivdata), UNUSED(void *lastdata), void **result_p_ ##sname)
#define CFstartparseVAR(sname,r) struct sname **r = (void*)result_p_ ## sname
#define CFfinishparse(sname) static retvalue finishparse ## sname(void *privdata_ ## sname, void *thisdata_ ## sname, void **lastdata_p_ ##sname, bool complete, struct configiterator *iter)
#define CFfinishparseVARS(sname,this,last,mydata) struct sname *this = thisdata_ ## sname, **last = (void*)lastdata_p_ ## sname; struct read_ ## sname ## _data *mydata = privdata_ ## sname
#define CFUfinishparseVARS(sname,this,last,mydata) struct sname *this = thisdata_ ## sname
#define CFhashesSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), const char *name, void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	retvalue r; \
	r = config_getflags(iter, name, hashnames, item->field, false, "(allowed values: sha1 and sha256)"); \
	if( !RET_IS_OK(r) ) \
		return r; \
	if( item->field[cs_md5sum] ) { \
		fprintf(stderr, "%s:%u: Due to internal limitation, md5 hashes cannot yet be ignored. Sorry.\n", \
				config_filename(iter), config_firstline(iter)); \
		return RET_ERROR; \
	}\
	return RET_OK; \
}

// TODO: better error reporting:
#define CFtermSETPROC(sname, field) \
static retvalue configparser_ ## sname ## _set_ ## field(UNUSED(void *dummy), UNUSED(const char *name), void *data, struct configiterator *iter) { \
	struct sname *item = data; \
	char *formula; \
	retvalue r; \
	r = config_getall(iter, &formula); \
	if( ! RET_IS_OK(r) ) \
		return r; \
	r = term_compile(&item->field, formula, T_OR|T_BRACKETS|T_NEGATION|T_VERSION|T_NOTEQUAL); \
	free(formula); \
	return r; \
}

// TODO: decide which should get better checking, which might allow escapes spaces:
#define CFurlSETPROC CFvalueSETPROC
#define CFdirSETPROC CFvalueSETPROC
#define CFfileSETPROC CFvalueSETPROC
#define config_getfileinline config_getwordinline
#define CFkeySETPROC CFvalueSETPROC

#endif /* REPREPRO_CONFIGPARSER_H */