#include <stdlib.h>
#include <ctype.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"

#include "vcc_if.h"

enum VAR_TYPE {
	STRING,
	INT,
	REAL,
	DURATION
};

struct var {
	unsigned magic;
#define VAR_MAGIC 0x8A21A651
	char *name;
	enum VAR_TYPE type;
	union {
		char *STRING;
		int INT;
		double REAL;
		double DURATION;
	} value;
	VTAILQ_ENTRY(var) list;
};

struct var_head {
	unsigned magic;
#define VAR_HEAD_MAGIC 0x64F33E2F
	unsigned xid;
	VTAILQ_HEAD(, var) vars;
};

static struct var_head **var_list = NULL;
static int var_list_sz = 0;
static VTAILQ_HEAD(, var) global_vars = VTAILQ_HEAD_INITIALIZER(global_vars);
static pthread_mutex_t var_list_mtx = PTHREAD_MUTEX_INITIALIZER;


static void vh_init(struct var_head *vh)
{

	AN(vh);
	memset(vh, 0, sizeof *vh);
	vh->magic = VAR_HEAD_MAGIC;
	VTAILQ_INIT(&vh->vars);
}

static struct var * vh_get_var(struct var_head *vh, const char *name) {
	struct var *v;

	AN(vh);
	AN(name);
	VTAILQ_FOREACH(v, &vh->vars, list) {
		CHECK_OBJ_NOTNULL(v, VAR_MAGIC);
		AN(v->name);
		if (strcmp(v->name, name) == 0)
			return v;
	}
	return NULL;
}

static struct var * vh_get_var_alloc(struct var_head *vh, const char *name,
    struct sess *sp)
{
	struct var *v;

	v = vh_get_var(vh, name);

	if (!v) {
		/* Allocate and add */
		v = (struct var*)WS_Alloc(sp->ws, sizeof(struct var));
		AN(v);
		v->magic = VAR_MAGIC;
		v->name = WS_Dup(sp->ws, name);
		AN(v->name);
		VTAILQ_INSERT_HEAD(&vh->vars, v, list);
	}
	return v;
}

int
init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{

	AZ(pthread_mutex_lock(&var_list_mtx));
	if (var_list == NULL) {
		AZ(var_list_sz);
		var_list_sz = 256;
		var_list = malloc(sizeof(struct var_head *) * 256);
		AN(var_list);
		for (int i = 0 ; i < var_list_sz; i++) {
			var_list[i] = malloc(sizeof(struct var_head));
			vh_init(var_list[i]);
		}
	}
	AZ(pthread_mutex_unlock(&var_list_mtx));
	return 0;
}

static struct var_head * get_vh(struct sess *sp)
{
	struct var_head *vh;

	AZ(pthread_mutex_lock(&var_list_mtx));
	while (var_list_sz <= sp->id) {
		int ns = var_list_sz*2;
		/* resize array */
		var_list = realloc(var_list, ns * sizeof(struct var_head *));
		for (; var_list_sz < ns; var_list_sz++) {
			var_list[var_list_sz] = malloc(sizeof(struct var_head));
			vh_init(var_list[var_list_sz]);
		}
		assert(var_list_sz == ns);
		AN(var_list);
	}
	vh = var_list[sp->id];

	if (vh->xid != sp->xid) {
		vh_init(vh);
		vh->xid = sp->xid;
	}
	AZ(pthread_mutex_unlock(&var_list_mtx));
	return vh;
}

void
vmod_set(struct sess *sp, const char *name, const char *value)
{
	vmod_set_string(sp, name, value);
}

const char *
vmod_get(struct sess *sp, const char *name)
{
	return vmod_get_string(sp, name);
}

void
vmod_set_string(struct sess *sp, const char *name, const char *value)
{
	struct var *v;

	if (name == NULL)
		return;
	v = vh_get_var_alloc(get_vh(sp), name, sp);
	AN(v);
	v->type = STRING;
	if (value == NULL)
		value = "";
	v->value.STRING = WS_Dup(sp->ws, value);
}

const char *
vmod_get_string(struct sess *sp, const char *name)
{
	struct var *v;
	if (name == NULL)
		return (NULL);
	v = vh_get_var(get_vh(sp), name);
	if (!v || v->type != STRING)
		return NULL;
	return (v->value.STRING);
}

void
vmod_set_int(struct sess *sp, const char *name, int value)
{
	struct var *v;
	if (name == NULL)
		return;
	v = vh_get_var_alloc(get_vh(sp), name, sp);
	AN(v);
	v->type = INT;
	v->value.INT = value;
}

int
vmod_get_int(struct sess *sp, const char *name)
{
	struct var *v;

	if (name == NULL)
		return 0;
	v = vh_get_var(get_vh(sp), name);

	if (!v || v->type != INT)
		return 0;
	return (v->value.INT);
}

void
vmod_set_real(struct sess *sp, const char *name, double value)
{
	struct var *v;
	if (name == NULL)
		return;
	v = vh_get_var_alloc(get_vh(sp), name, sp);
	AN(v);
	v->type = REAL;
	v->value.REAL = value;
}

double
vmod_get_real(struct sess *sp, const char *name)
{
	struct var *v;

	if (name == NULL)
		return (0.);
	v = vh_get_var(get_vh(sp), name);

	if (!v || v->type != REAL)
		return 0.;
	return (v->value.REAL);
}

void
vmod_set_duration(struct sess *sp, const char *name, double value)
{
	struct var *v;
	if (name == NULL)
		return;
	v = vh_get_var_alloc(get_vh(sp), name, sp);
	AN(v);
	v->type = DURATION;
	v->value.DURATION = value;
}

double
vmod_get_duration(struct sess *sp, const char *name)
{
	struct var *v;

	if (name == NULL)
		return (0.);
	v = vh_get_var(get_vh(sp), name);

	if (!v || v->type != DURATION)
		return 0;
	return (v->value.DURATION);
}

void vmod_clear(struct sess *sp)
{
	struct var_head *vh;
	vh = get_vh(sp);
	vh_init(vh);
}

void
vmod_global_set(struct sess *sp, const char *name, const char *value)
{
	struct var *v;

	if (name == NULL)
		return;

	AZ(pthread_mutex_lock(&var_list_mtx));
	VTAILQ_FOREACH(v, &global_vars, list) {
		CHECK_OBJ_NOTNULL(v, VAR_MAGIC);
		AN(v->name);
		if (strcmp(v->name, name) == 0)
			break;
	}
	if (v) {
		VTAILQ_REMOVE(&global_vars, v, list);
		free(v->name);
		v->name = NULL;
	} else
		ALLOC_OBJ(v, VAR_MAGIC);
	AN(v);
	v->name = strdup(name);
	AN(v->name);
	VTAILQ_INSERT_HEAD(&global_vars, v, list);
	if (v->type == STRING)
		free(v->value.STRING);
	v->value.STRING = NULL;
	v->type = STRING;
	if (value != NULL)
		v->value.STRING = strdup(value);

	AZ(pthread_mutex_unlock(&var_list_mtx));
}

const char *
vmod_global_get(struct sess *sp, const char *name)
{
	struct var *v;
	const char *r = NULL;

	AZ(pthread_mutex_lock(&var_list_mtx));
	VTAILQ_FOREACH(v, &global_vars, list) {
		CHECK_OBJ_NOTNULL(v, VAR_MAGIC);
		AN(v->name);
		if (strcmp(v->name, name) == 0)
			break;
	}
	if (v && v->value.STRING != NULL) {
		r = WS_Dup(sp->ws, v->value.STRING);
		AN(r);
	}
	AZ(pthread_mutex_unlock(&var_list_mtx));
	return(r);
}
