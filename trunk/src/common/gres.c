/*****************************************************************************\
 *  gres.c - driver for gres plugin
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#  if STDC_HEADERS
#    include <string.h>
#  endif
#  if HAVE_SYS_TYPES_H
#    include <sys/types.h>
#  endif /* HAVE_SYS_TYPES_H */
#  if HAVE_UNISTD_H
#    include <unistd.h>
#  endif
#  if HAVE_INTTYPES_H
#    include <inttypes.h>
#  else /* ! HAVE_INTTYPES_H */
#    if HAVE_STDINT_H
#      include <stdint.h>
#    endif
#  endif /* HAVE_INTTYPES_H */
#else /* ! HAVE_CONFIG_H */
#  include <sys/types.h>
#  include <unistd.h>
#  include <stdint.h>
#  include <string.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <slurm/slurm.h>
#include <slurm/slurm_errno.h>
#include <sys/stat.h>

#include "src/common/gres.h"
#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#define GRES_MAGIC 0x438a34d4

/* Gres symbols provided by the plugin */
typedef struct slurm_gres_ops {
	uint32_t	(*plugin_id);
	char		(*gres_name);
	char		(*help_msg);
	int		(*node_config_load)	( List gres_conf_list );
} slurm_gres_ops_t;

/* Gres plugin context, one for each gres type */
typedef struct slurm_gres_context {
	plugin_handle_t	cur_plugin;
	int		gres_errno;
	char *		gres_name_colon;
	int		gres_name_colon_len;
	char *		gres_type;
	slurm_gres_ops_t ops;
	plugrack_t	plugin_list;
	bool		unpacked_info;
} slurm_gres_context_t;

/* Generic gres data structure for adding to a list. Depending upon the
 * context, gres_data points to gres_node_state_t, gres_job_state_t or
 * gres_step_state_t */
typedef struct gres_state {
	uint32_t	plugin_id;
	void		*gres_data;
} gres_state_t;


static int gres_context_cnt = -1;
static uint32_t gres_cpu_cnt = 0;
static bool gres_debug = false;
static slurm_gres_context_t *gres_context = NULL;
static char *gres_plugin_list = NULL;
static pthread_mutex_t gres_context_lock = PTHREAD_MUTEX_INITIALIZER;
static List gres_conf_list = NULL;

static void	_destroy_gres_slurmd_conf(void *x);
static char *	_get_gres_conf(void);
static int	_load_gres_plugin(char *plugin_name,
				  slurm_gres_context_t *plugin_context);
static int	_log_gres_slurmd_conf(void *x, void *arg);
extern int	_node_config_init(char *node_name, char *orig_config,
				  slurm_gres_context_t *context_ptr,
				  gres_state_t *gres_ptr);
static int	_parse_gres_config(void **dest, slurm_parser_enum_t type,
				   const char *key, const char *value,
				   const char *line, char **leftover);
static int	_strcmp(const char *s1, const char *s2);
static int	_unload_gres_plugin(slurm_gres_context_t *plugin_context);

/* Variant of strcmp that will accept NULL string pointers */
static int  _strcmp(const char *s1, const char *s2)
{
	if ((s1 != NULL) && (s2 == NULL))
		return 1;
	if ((s1 == NULL) && (s2 == NULL))
		return 0;
	if ((s1 == NULL) && (s2 != NULL))
		return -1;
	return strcmp(s1, s2);
}

static int _load_gres_plugin(char *plugin_name,
			     slurm_gres_context_t *plugin_context)
{
	/*
	 * Must be synchronized with slurm_gres_ops_t above.
	 */
	static const char *syms[] = {
		"plugin_id",
		"gres_name",
		"help_msg",
		"node_config_load",
	};
	int n_syms = sizeof(syms) / sizeof(char *);

	/* Find the correct plugin */
	plugin_context->gres_type	= xstrdup("gres/");
	xstrcat(plugin_context->gres_type, plugin_name);
	plugin_context->plugin_list	= NULL;
	plugin_context->cur_plugin	= PLUGIN_INVALID_HANDLE;
	plugin_context->gres_errno 	= SLURM_SUCCESS;

	plugin_context->cur_plugin = plugin_load_and_link(
					plugin_context->gres_type,
					n_syms, syms,
					(void **) &plugin_context->ops);
	if (plugin_context->cur_plugin != PLUGIN_INVALID_HANDLE)
		return SLURM_SUCCESS;

	error("gres: Couldn't find the specified plugin name for %s "
	      "looking at all files",
	      plugin_context->gres_type);

	/* Get plugin list */
	if (plugin_context->plugin_list == NULL) {
		char *plugin_dir;
		plugin_context->plugin_list = plugrack_create();
		if (plugin_context->plugin_list == NULL) {
			error("gres: cannot create plugin manager");
			return SLURM_ERROR;
		}
		plugrack_set_major_type(plugin_context->plugin_list,
					"gres");
		plugrack_set_paranoia(plugin_context->plugin_list,
				      PLUGRACK_PARANOIA_NONE, 0);
		plugin_dir = slurm_get_plugin_dir();
		plugrack_read_dir(plugin_context->plugin_list, plugin_dir);
		xfree(plugin_dir);
	}

	plugin_context->cur_plugin = plugrack_use_by_type(
					plugin_context->plugin_list,
					plugin_context->gres_type );
	if (plugin_context->cur_plugin == PLUGIN_INVALID_HANDLE) {
		error("gres: cannot find scheduler plugin for %s",
		       plugin_context->gres_type);
		return SLURM_ERROR;
	}

	/* Dereference the API. */
	if (plugin_get_syms(plugin_context->cur_plugin,
			    n_syms, syms,
			    (void **) &plugin_context->ops ) < n_syms ) {
		error("gres: incomplete %s plugin detected",
		      plugin_context->gres_type);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static int _unload_gres_plugin(slurm_gres_context_t *plugin_context)
{
	int rc;

	/*
	 * Must check return code here because plugins might still
	 * be loaded and active.
	 */
	if (plugin_context->plugin_list)
		rc = plugrack_destroy(plugin_context->plugin_list);
	else {
		rc = SLURM_SUCCESS;
		plugin_unload(plugin_context->cur_plugin);
	}
	xfree(plugin_context->gres_name_colon);
	xfree(plugin_context->gres_type);

	return rc;
}

/*
 * Initialize the gres plugin.
 *
 * Returns a SLURM errno.
 */
extern int gres_plugin_init(void)
{
	int i, j, rc = SLURM_SUCCESS;
	char *last = NULL, *names, *one_name, *full_name;

	slurm_mutex_lock(&gres_context_lock);
	if (slurm_get_debug_flags() & DEBUG_FLAG_GRES)
		gres_debug = true;
	else
		gres_debug = false;

	if (gres_context_cnt >= 0)
		goto fini;

	gres_plugin_list = slurm_get_gres_plugins();
	gres_context_cnt = 0;
	if ((gres_plugin_list == NULL) || (gres_plugin_list[0] == '\0'))
		goto fini;

	gres_context_cnt = 0;
	names = xstrdup(gres_plugin_list);
	one_name = strtok_r(names, ",", &last);
	while (one_name) {
		full_name = xstrdup("gres/");
		xstrcat(full_name, one_name);
		for (i=0; i<gres_context_cnt; i++) {
			if (!strcmp(full_name, gres_context[i].gres_type))
				break;
		}
		xfree(full_name);
		if (i<gres_context_cnt) {
			error("Duplicate plugin %s ignored",
			      gres_context[i].gres_type);
		} else {
			xrealloc(gres_context, (sizeof(slurm_gres_context_t) *
				 (gres_context_cnt + 1)));
			rc = _load_gres_plugin(one_name,
					       gres_context + gres_context_cnt);
			if (rc != SLURM_SUCCESS)
				break;
			gres_context_cnt++;
		}
		one_name = strtok_r(NULL, ",", &last);
	}
	xfree(names);

	/* Insure that plugin_id is valid and unique */
	for (i=0; i<gres_context_cnt; i++) {
		for (j=i+1; j<gres_context_cnt; j++) {
			if (*(gres_context[i].ops.plugin_id) !=
			    *(gres_context[j].ops.plugin_id))
				continue;
			fatal("GresPlugins: Duplicate plugin_id %u for %s and %s",
			      *(gres_context[i].ops.plugin_id),
			      gres_context[i].gres_type,
			      gres_context[j].gres_type);
		}
		if (*(gres_context[i].ops.plugin_id) < 100) {
			fatal("GresPlugins: Invalid plugin_id %u (<100) %s",
			      *(gres_context[i].ops.plugin_id),
			      gres_context[i].gres_type);
		}
		xassert(gres_context[i].ops.gres_name);
		gres_context[i].gres_name_colon =
			xstrdup_printf("%s:", gres_context[i].ops.gres_name);
		gres_context[i].gres_name_colon_len =
			strlen(gres_context[i].gres_name_colon);
	}

fini:	slurm_mutex_unlock(&gres_context_lock);
	return rc;
}

/*
 * Terminate the gres plugin. Free memory.
 *
 * Returns a SLURM errno.
 */
extern int gres_plugin_fini(void)
{
	int i, j, rc = SLURM_SUCCESS;

	slurm_mutex_lock(&gres_context_lock);
	if (gres_context_cnt < 0)
		goto fini;

	for (i=0; i<gres_context_cnt; i++) {
		j = _unload_gres_plugin(gres_context + i);
		if (j != SLURM_SUCCESS)
			rc = j;
	}
	xfree(gres_context);
	xfree(gres_plugin_list);
	FREE_NULL_LIST(gres_conf_list);
	gres_context_cnt = -1;

fini:	slurm_mutex_unlock(&gres_context_lock);
	return rc;
}

/*
 **************************************************************************
 *                          P L U G I N   C A L L S                       *
 **************************************************************************
 */

/*
 * Provide a plugin-specific help message for salloc, sbatch and srun
 * IN/OUT msg - buffer provided by caller and filled in by plugin
 * IN msg_size - size of msg buffer in bytes
 */
extern int gres_plugin_help_msg(char *msg, int msg_size)
{
	int i, rc;
	char *tmp_msg;
	char *header = "Valid gres options are:\n";

	if (msg_size < 1)
		return EINVAL;

	msg[0] = '\0';
	tmp_msg = xmalloc(msg_size);
	rc = gres_plugin_init();

	if ((strlen(header) + 2) <= msg_size)
		strcat(msg, header);
	slurm_mutex_lock(&gres_context_lock);
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		tmp_msg = (gres_context[i].ops.help_msg);
		if ((tmp_msg == NULL) || (tmp_msg[0] == '\0'))
			continue;
		if ((strlen(msg) + strlen(tmp_msg) + 2) > msg_size)
			break;
		strcat(msg, tmp_msg);
		strcat(msg, "\n");
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Perform reconfig, re-read any configuration files
 * OUT did_change - set if gres configuration changed
 */
extern int gres_plugin_reconfig(bool *did_change)
{
	int rc = SLURM_SUCCESS;
	char *plugin_names = slurm_get_gres_plugins();
	bool plugin_change;

	*did_change = false;
	slurm_mutex_lock(&gres_context_lock);
	if (slurm_get_debug_flags() & DEBUG_FLAG_GRES)
		gres_debug = true;
	else
		gres_debug = false;

	if (_strcmp(plugin_names, gres_plugin_list))
		plugin_change = true;
	else
		plugin_change = false;
	slurm_mutex_unlock(&gres_context_lock);

	if (plugin_change) {
		error("GresPlugins changed from %s to %s ignored",
		     gres_plugin_list, plugin_names);
		error("Restart the slurmctld daemon to change GresPlugins");
		*did_change = true;
#if 0
		/* This logic would load new plugins, but we need the old
		 * plugins to persist in order to process old state 
		 * information. */
		rc = gres_plugin_fini();
		if (rc == SLURM_SUCCESS)
			rc = gres_plugin_init();
#endif
	}
	xfree(plugin_names);

	return rc;
}

/*
 * Return the pathname of the gres.conf file
 */
static char *_get_gres_conf(void)
{
	char *val = getenv("SLURM_CONF");
	char *rc = NULL;
	int i;

	if (!val)
		return xstrdup(GRES_CONFIG_FILE);

	/* Replace file name on end of path */
	i = strlen(val) - strlen("slurm.conf") + strlen("gres.conf") + 1;
	rc = xmalloc(i);
	strcpy(rc, val);
	val = strrchr(rc, (int)'/');
	if (val)	/* absolute path */
		val++;
	else		/* not absolute path */
		val = rc;
	strcpy(val, "gres.conf");
	return rc;
}

/*
 * Destroy a gres_slurmd_conf_t record, free it's memory
 */
static void _destroy_gres_slurmd_conf(void *x)
{
	gres_slurmd_conf_t *p = (gres_slurmd_conf_t *) x;

	xassert(p);
	xfree(p->cpus);
	xfree(p->file);
	xfree(p->name);
	xfree(p);
}

/*
 * Log the contents of a gres_slurmd_conf_t record
 */
static int _log_gres_slurmd_conf(void *x, void *arg)
{
	gres_slurmd_conf_t *p;

	if (!gres_debug)
		return 0;

	p = (gres_slurmd_conf_t *) x;
	xassert(p);
	if (p->cpus) {
		info("Gres Name:%s Count:%u File:%s CPUs:%s CpuCnt:%u",
		     p->name, p->count, p->file, p->cpus, p->cpu_cnt);
	} else {
		info("Gres Name:%s Count:%u File:%s",
		     p->name, p->count, p->file);
	}
	return 0;
}

/*
 * Build gres_slurmd_conf_t record based upon a line from the gres.conf file
 */
static int _parse_gres_config(void **dest, slurm_parser_enum_t type,
			      const char *key, const char *value,
			      const char *line, char **leftover)
{
	static s_p_options_t _gres_options[] = {
		{"Count", S_P_UINT32},	/* Number of Gres available */
		{"CPUs", S_P_STRING},	/* CPUs to bind to Gres resource */
		{"File", S_P_STRING},	/* Path to Gres device */
		{NULL}
	};
	int i;
	s_p_hashtbl_t *tbl;
	gres_slurmd_conf_t *p;

	tbl = s_p_hashtbl_create(_gres_options);
	s_p_parse_line(tbl, *leftover, leftover);

	p = xmalloc(sizeof(gres_slurmd_conf_t));
	p->name = xstrdup(value);
	if (!s_p_get_uint32(&p->count, "Count", tbl))
		p->count = 1;
	if (s_p_get_string(&p->cpus, "CPUs", tbl)) {
		bitstr_t *cpu_bitmap;	/* Just use to validate config */
		p->cpu_cnt = gres_cpu_cnt;
		cpu_bitmap = bit_alloc(gres_cpu_cnt);
		if (cpu_bitmap == NULL)
			fatal("bit_alloc: malloc failure");
		i = bit_unfmt(cpu_bitmap, p->cpus);
		if (i != 0) {
			fatal("Invalid gres data for %s, CPUs=%s",
			      p->name, p->cpus);
		}
		FREE_NULL_BITMAP(cpu_bitmap);
	}
	if (s_p_get_string(&p->file, "File", tbl)) {
		struct stat config_stat;
		if (stat(p->file, &config_stat) < 0)
			fatal("can't stat gres.conf file %s: %m", p->file);
	}
	s_p_hashtbl_destroy(tbl);

	for (i=0; i<gres_context_cnt; i++) {
		if (strcasecmp(value, gres_context[i].ops.gres_name) == 0)
			break;
	}
	if (i >= gres_context_cnt) {
		error("Ignoring gres.conf Name=%s", value);
		_destroy_gres_slurmd_conf(p);
		return 0;
	}

	*dest = (void *)p;
	return 1;
}

/*
 * Load this node's configuration (how many resources it has, topology, etc.)
 * IN cpu_cnt - Number of CPUs on configured on this node
 */
extern int gres_plugin_node_config_load(uint32_t cpu_cnt)
{
	static s_p_options_t _gres_options[] = {
		{"Name", S_P_ARRAY, _parse_gres_config, NULL},
		{NULL}
	};

	int count, i, rc;
	struct stat config_stat;
	s_p_hashtbl_t *tbl;
	gres_slurmd_conf_t **gres_array;
	char *gres_conf_file = _get_gres_conf();

	rc = gres_plugin_init();
	if (gres_context_cnt == 0)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&gres_context_lock);
	gres_cpu_cnt = cpu_cnt;
	if (stat(gres_conf_file, &config_stat) < 0)
		fatal("can't stat gres.conf file %s: %m", gres_conf_file);
	tbl = s_p_hashtbl_create(_gres_options);
	if (s_p_parse_file(tbl, NULL, gres_conf_file) == SLURM_ERROR)
		fatal("error opening/reading %s", gres_conf_file);
	gres_conf_list = list_create(_destroy_gres_slurmd_conf);
	if (gres_conf_list == NULL)
		fatal("list_create: malloc failure");
	if (s_p_get_array((void ***) &gres_array, &count, "Name", tbl)) {
		for (i = 0; i < count; i++) {
			list_append(gres_conf_list, gres_array[i]);
			gres_array[i] = NULL;
		}
	}
	s_p_hashtbl_destroy(tbl);
	list_for_each(gres_conf_list, _log_gres_slurmd_conf, NULL);

	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		rc = (*(gres_context[i].ops.node_config_load))(gres_conf_list);
	}
	slurm_mutex_unlock(&gres_context_lock);

	xfree(gres_conf_file);
	return rc;
}

/*
 * Pack this node's gres configuration into a buffer
 * IN/OUT buffer - message buffer to pack
 */
extern int gres_plugin_node_config_pack(Buf buffer)
{
	int rc;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0, version= SLURM_PROTOCOL_VERSION;
	ListIterator iter;
	gres_slurmd_conf_t *gres_slurmd_conf;
 
	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	pack16(version, buffer);
	if (gres_conf_list)
		rec_cnt = list_count(gres_conf_list);
	pack16(rec_cnt, buffer);
	if (rec_cnt) {
		iter = list_iterator_create(gres_conf_list);
		if (iter == NULL)
			fatal("list_iterator_create: malloc failure");
		while ((gres_slurmd_conf = 
				(gres_slurmd_conf_t *) list_next(iter))) {
			pack32(magic, buffer);
			pack32(gres_slurmd_conf->plugin_id, buffer);
			pack32(gres_slurmd_conf->count, buffer);
			pack32(gres_slurmd_conf->cpu_cnt, buffer);
			packstr(gres_slurmd_conf->cpus, buffer);
		}
		list_iterator_destroy(iter);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

/*
 * Unpack this node's configuration from a buffer (build/packed by slurmd)
 * IN/OUT buffer - message buffer to unpack
 * IN node_name - name of node whose data is being unpacked
 */
extern int gres_plugin_node_config_unpack(Buf buffer, char* node_name)
{
	int i, j, rc;
	uint32_t count, cpu_cnt, magic, plugin_id, utmp32;
	uint16_t rec_cnt, version;
	char *tmp_cpus;
	gres_slurmd_conf_t *p;

	rc = gres_plugin_init();

	FREE_NULL_LIST(gres_conf_list);
	gres_conf_list = list_create(_destroy_gres_slurmd_conf);
	if (gres_conf_list == NULL)
		fatal("list_create: malloc failure");

	safe_unpack16(&version, buffer);
	if (version != SLURM_2_2_PROTOCOL_VERSION)
		return SLURM_ERROR;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	slurm_mutex_lock(&gres_context_lock);
 	for (j=0; j<gres_context_cnt; j++)
 		gres_context[j].unpacked_info = false;
	for (i=0; i<rec_cnt; i++) {
		safe_unpack32(&magic, buffer);
		if (magic != GRES_MAGIC)
			goto unpack_error;
		safe_unpack32(&plugin_id, buffer);
		safe_unpack32(&count, buffer);
		safe_unpack32(&cpu_cnt, buffer);
		safe_unpackstr_xmalloc(&tmp_cpus, &utmp32, buffer);
 		for (j=0; j<gres_context_cnt; j++) {
 			if (*(gres_context[j].ops.plugin_id) == plugin_id) {
				gres_context[j].unpacked_info = true;
 				break;
			}
 		}
		if (j >= gres_context_cnt) {
			/* A likely sign that GresPlugins is inconsistently
			 * configured. Not a fatal error, skip over the data. */
			error("gres_plugin_node_config_unpack: no plugin "
			      "configured to unpack data type %u from node %s",
			      plugin_id, node_name);
			xfree(tmp_cpus);
			continue;
		}
		p = xmalloc(sizeof(gres_slurmd_conf_t));
		p->count = count;
		p->cpu_cnt = cpu_cnt;
		p->cpus = tmp_cpus;
		p->plugin_id = plugin_id;
		tmp_cpus = NULL;	/* Nothing left to xfree */
		list_append(gres_conf_list, p);
	}
 	for (j=0; j<gres_context_cnt; j++) {
 		if (gres_context[j].unpacked_info)
			continue;

		/* A likely sign GresPlugins is inconsistently configured. */
		error("gres_plugin_node_config_unpack: no data type of type %s "
		      "from node %s", gres_context[j].gres_type, node_name);
	}
	slurm_mutex_unlock(&gres_context_lock);
	return rc;

unpack_error:
	error("gres_plugin_node_config_unpack: unpack error from node %s",
	      node_name);
	xfree(tmp_cpus);
	slurm_mutex_unlock(&gres_context_lock);
	return SLURM_ERROR;
}

/*
 * Delete an element placed on gres_list by _node_config_validate()
 * free associated memory
 */
static void _gres_node_list_delete(void *list_element)
{
	gres_state_t *gres_ptr;
	gres_node_state_t *gres_node_ptr;

	gres_ptr = (gres_state_t *) list_element;
	gres_node_ptr = (gres_node_state_t *) gres_ptr->gres_data;
	FREE_NULL_BITMAP(gres_node_ptr->gres_bit_alloc);
	xfree(gres_node_ptr);
	xfree(gres_ptr);
}

/*
 * Build a node's gres record based only upon the slurm.conf contents
 */
extern int _node_config_init(char *node_name, char *orig_config,
			     slurm_gres_context_t *context_ptr,
			     gres_state_t *gres_ptr)
{
	int rc = SLURM_SUCCESS;
	char *node_gres_config, *tok, *last_num = NULL, *last_tok = NULL;
	int32_t gres_config_cnt = 0;
	bool updated_config = false;
	gres_node_state_t *gres_data;

	if (gres_ptr->gres_data == NULL) {
		gres_data = xmalloc(sizeof(gres_node_state_t));
		gres_ptr->gres_data = gres_data;
		gres_data->gres_cnt_config = NO_VAL;
		gres_data->gres_cnt_found  = NO_VAL;
		updated_config = true;
	} else {
		gres_data = (gres_node_state_t *) gres_ptr->gres_data;
	}

	/* If the resource isn't configured for use with this node*/
	if ((orig_config == NULL) || (orig_config[0] == '\0') ||
	    (updated_config == false)) {
		gres_data->gres_cnt_config = 0;
		return rc;
	}

	node_gres_config = xstrdup(orig_config);
	tok = strtok_r(node_gres_config, ",", &last_tok);
	while (tok) {
		if (!strcmp(tok, context_ptr->ops.gres_name)) {
			gres_config_cnt = 1;
			break;
		}
		if (!strncmp(tok, context_ptr->gres_name_colon,
			     context_ptr->gres_name_colon_len)) {
			tok += context_ptr->gres_name_colon_len;
			gres_config_cnt = strtol(tok, &last_num, 10);
			if (last_num[0] == '\0')
				;
			else if ((last_num[0] == 'k') || (last_num[0] == 'K'))
				gres_config_cnt *= 1024;
			break;
		}
		tok = strtok_r(NULL, ",", &last_tok);
	}
	xfree(node_gres_config);

	gres_data->gres_cnt_config = gres_config_cnt;
	gres_data->gres_cnt_avail  = gres_config_cnt;
	if (gres_data->gres_bit_alloc == NULL) {
		gres_data->gres_bit_alloc =
			bit_alloc(gres_data->gres_cnt_avail);
	} else if (gres_data->gres_cnt_avail > 
		   bit_size(gres_data->gres_bit_alloc)) {
		gres_data->gres_bit_alloc =
			bit_realloc(gres_data->gres_bit_alloc,
				    gres_data->gres_cnt_avail);
	}
	if (gres_data->gres_bit_alloc == NULL)
		fatal("bit_alloc: malloc failure");

	return rc;
}

/*
 * Build a node's gres record based only upon the slurm.conf contents
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from slurm.conf
 * IN/OUT gres_list - List of Gres records for this node to track usage
 */
extern int gres_plugin_init_node_config(char *node_name, char *orig_config,
					List *gres_list)
{
	int i, rc;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		/* Find or create gres_state entry on the list */
		gres_iter = list_iterator_create(*gres_list);
		while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
			if (gres_ptr->plugin_id ==
			    *(gres_context[i].ops.plugin_id))
				break;
		}
		list_iterator_destroy(gres_iter);
		if (gres_ptr == NULL) {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			list_append(*gres_list, gres_ptr);
		}

		rc = _node_config_init(node_name, orig_config,
				       &gres_context[i], gres_ptr);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

static uint32_t _get_tot_gres_cnt(uint32_t plugin_id)
{
	ListIterator iter;
	gres_slurmd_conf_t *gres_slurmd_conf;
	uint32_t gres_cnt = 0;

	if (gres_conf_list == NULL)
		return gres_cnt;

	iter = list_iterator_create(gres_conf_list);
	if (iter == NULL)
		fatal("list_iterator_create: malloc failure");
	while ((gres_slurmd_conf = (gres_slurmd_conf_t *) list_next(iter))) {
		if (gres_slurmd_conf->plugin_id == plugin_id)
			gres_cnt += gres_slurmd_conf->count;
	}
	list_iterator_destroy(iter);
	return gres_cnt;
}

extern int _node_config_validate(char *node_name, uint32_t gres_cnt,
				 char *orig_config, char **new_config,
				 gres_state_t *gres_ptr,
				 uint16_t fast_schedule, char **reason_down,
				 slurm_gres_context_t *context_ptr)
{
	int rc = SLURM_SUCCESS;
	char *node_gres_config, *tok, *last_num = NULL, *last_tok = NULL;
	uint32_t gres_config_cnt = 0;
	bool updated_config = false;
	gres_node_state_t *gres_data;

	gres_data = (gres_node_state_t *) gres_ptr->gres_data;
	if (gres_data == NULL) {
		gres_ptr->gres_data = xmalloc(sizeof(gres_node_state_t));
		gres_data = (gres_node_state_t *) gres_ptr->gres_data;
		gres_data->gres_cnt_config = NO_VAL;
		gres_data->gres_cnt_found  = gres_cnt;
		updated_config = true;
	} else if (gres_data->gres_cnt_found != gres_cnt) {
		if (gres_data->gres_cnt_found != NO_VAL) {
			info("%s:count changed for node %s from %u to %u",
			     context_ptr->gres_type, node_name,
			     gres_data->gres_cnt_found, gres_cnt);
		}
		gres_data->gres_cnt_found = gres_cnt;
		updated_config = true;
	}
	if (updated_config == false)
		return SLURM_SUCCESS;

	if ((orig_config == NULL) || (orig_config[0] == '\0'))
		gres_data->gres_cnt_config = 0;
	else if (gres_data->gres_cnt_config == NO_VAL) {
		node_gres_config = xstrdup(orig_config);
		tok = strtok_r(node_gres_config, ",", &last_tok);
		while (tok) {
			if (!strcmp(tok, context_ptr->ops.gres_name)) {
				gres_config_cnt = 1;
				break;
			}
			if (!strncmp(tok, context_ptr->gres_name_colon,
				     context_ptr->gres_name_colon_len)) {
				tok += context_ptr->gres_name_colon_len;
				gres_config_cnt = strtol(tok, &last_num, 10);
				if (last_num[0] == '\0')
					;
				else if ((last_num[0] == 'k') ||
					 (last_num[0] == 'K'))
					gres_config_cnt *= 1024;
				break;
			}
			tok = strtok_r(NULL, ",", &last_tok);
		}
		xfree(node_gres_config);
		gres_data->gres_cnt_config = gres_config_cnt;
	}

	if ((gres_data->gres_cnt_config == 0) || (fast_schedule > 0))
		gres_data->gres_cnt_avail = gres_data->gres_cnt_config;
	else
		gres_data->gres_cnt_avail = gres_data->gres_cnt_found;

	if (gres_data->gres_bit_alloc == NULL) {
		gres_data->gres_bit_alloc =
			bit_alloc(gres_data->gres_cnt_avail);
	} else if (gres_data->gres_cnt_avail > 
		   bit_size(gres_data->gres_bit_alloc)) {
		gres_data->gres_bit_alloc =
			bit_realloc(gres_data->gres_bit_alloc,
				    gres_data->gres_cnt_avail);
		if (gres_data->gres_bit_alloc == NULL)
			fatal("bit_alloc: malloc failure");
	}

	if ((fast_schedule < 2) && 
	    (gres_data->gres_cnt_found < gres_data->gres_cnt_config)) {
		if (reason_down && (*reason_down == NULL)) {
			xstrfmtcat(*reason_down, "%s count too low",
				   context_ptr->gres_type);
		}
		rc = EINVAL;
	} else if ((fast_schedule == 0) && 
		   (gres_data->gres_cnt_found > gres_data->gres_cnt_config)) {
		/* need to rebuild new_config */
		char *new_configured_res = NULL;
		if (*new_config)
			node_gres_config = xstrdup(*new_config);
		else
			node_gres_config = xstrdup(orig_config);
		tok = strtok_r(node_gres_config, ",", &last_tok);
		while (tok) {
			if (new_configured_res)
				xstrcat(new_configured_res, ",");
			if (strcmp(tok, context_ptr->ops.gres_name) &&
			    strncmp(tok, context_ptr->gres_name_colon,
				    context_ptr->gres_name_colon_len)) {
				xstrcat(new_configured_res, tok);
			} else {
				xstrfmtcat(new_configured_res, "%s:%u",
					   context_ptr->ops.gres_name,
					   gres_data->gres_cnt_found);
			}
			tok = strtok_r(NULL, ",", &last_tok);
		}
		xfree(node_gres_config);
		xfree(*new_config);
		*new_config = new_configured_res;
	}

	return rc;
}

/*
 * Validate a node's configuration and put a gres record onto a list
 * Called immediately after gres_plugin_node_config_unpack().
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from slurm.conf
 * IN/OUT new_config - Updated gres info from slurm.conf if FastSchedule=0
 * IN/OUT gres_list - List of Gres records for this node to track usage
 * IN fast_schedule - 0: Validate and use actual hardware configuration
 *		      1: Validate hardware config, but use slurm.conf config
 *		      2: Don't validate hardware, use slurm.conf configuration
 * OUT reason_down - set to an explanation of failure, if any, don't set if NULL
 */
extern int gres_plugin_node_config_validate(char *node_name,
					    char *orig_config,
					    char **new_config,
					    List *gres_list,
					    uint16_t fast_schedule,
					    char **reason_down)
{
	int i, rc, rc2;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;
	uint32_t gres_cnt;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		/* Find or create gres_state entry on the list */
		gres_iter = list_iterator_create(*gres_list);
		while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
			if (gres_ptr->plugin_id ==
			    *(gres_context[i].ops.plugin_id))
				break;
		}
		list_iterator_destroy(gres_iter);
		if (gres_ptr == NULL) {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			list_append(*gres_list, gres_ptr);
		}
		gres_cnt = _get_tot_gres_cnt(*gres_context[i].ops.plugin_id);
		rc2 = _node_config_validate(node_name, gres_cnt, orig_config,
					    new_config, gres_ptr,
					    fast_schedule, reason_down,
					    &gres_context[i]);
		rc = MAX(rc, rc2);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

static int _node_reconfig(char *node_name, char *orig_config, char **new_config,
			  void **gres_data, uint16_t fast_schedule,
			  char *gres_name)
{
	char name_colon[128];
	int rc = SLURM_SUCCESS, name_colon_len;
	gres_node_state_t *gres_ptr;
	char *node_gres_config = NULL, *tok = NULL, *last = NULL;
	int32_t gres_config_cnt = 0;

	xassert(gres_data);
	name_colon_len = snprintf(name_colon, sizeof(name_colon), "%s:",
				  gres_name);
	gres_ptr = (gres_node_state_t *) *gres_data;
	if (gres_ptr == NULL) {
		gres_ptr = xmalloc(sizeof(gres_node_state_t));
		*gres_data = gres_ptr;
		gres_ptr->gres_cnt_config = NO_VAL;
		gres_ptr->gres_cnt_found  = NO_VAL;
	}

	if (orig_config) {
		node_gres_config = xstrdup(orig_config);
		tok = strtok_r(node_gres_config, ",", &last);
	}
	while (tok) {
		if (!strcmp(tok, gres_name)) {
			gres_config_cnt = 1;
			break;
		}
		if (!strncmp(tok, name_colon, name_colon_len)) {
			gres_config_cnt = strtol(tok+name_colon_len, &last, 10);
			if (last[0] == '\0')
				;
			else if ((last[0] == 'k') || (last[0] == 'K'))
				gres_config_cnt *= 1024;
			break;
		}
		tok = strtok_r(NULL, ",", &last);
	}
	gres_ptr->gres_cnt_config = gres_config_cnt;
	xfree(node_gres_config);

	if ((gres_ptr->gres_cnt_config == 0) || (fast_schedule > 0) ||
	    (gres_ptr->gres_cnt_found == NO_VAL))
		gres_ptr->gres_cnt_avail = gres_ptr->gres_cnt_config;
	else
		gres_ptr->gres_cnt_avail = gres_ptr->gres_cnt_found;

	if (gres_ptr->gres_bit_alloc == NULL) {
		gres_ptr->gres_bit_alloc = bit_alloc(gres_ptr->gres_cnt_avail);
	} else if (gres_ptr->gres_cnt_avail > 
		   bit_size(gres_ptr->gres_bit_alloc)) {
		gres_ptr->gres_bit_alloc = bit_realloc(gres_ptr->gres_bit_alloc,
						       gres_ptr->gres_cnt_avail);
	}
	if (gres_ptr->gres_bit_alloc == NULL)
		fatal("bit_alloc: malloc failure");

	if ((fast_schedule < 2) &&
	    (gres_ptr->gres_cnt_found != NO_VAL) &&
	    (gres_ptr->gres_cnt_found <  gres_ptr->gres_cnt_config)) {
		/* Do not set node DOWN, but give the node 
		 * a chance to register with more resources */
		gres_ptr->gres_cnt_found = NO_VAL;
	} else if ((fast_schedule == 0) &&
		   (gres_ptr->gres_cnt_found != NO_VAL) &&
		   (gres_ptr->gres_cnt_found >  gres_ptr->gres_cnt_config)) {
		/* need to rebuild new_config */
		char *new_configured_res = NULL;
		if (*new_config)
			node_gres_config = xstrdup(*new_config);
		else
			node_gres_config = xstrdup(orig_config);
		tok = strtok_r(node_gres_config, ",", &last);
		while (tok) {
			if (new_configured_res)
				xstrcat(new_configured_res, ",");
			if (strcmp(tok, gres_name) &&
			    strncmp(tok, name_colon, name_colon_len)) {
				xstrcat(new_configured_res, tok);
			} else {
				xstrfmtcat(new_configured_res, "%s:%u",
					   name_colon, gres_ptr->gres_cnt_found);
			}
			tok = strtok_r(NULL, ",", &last);
		}
		xfree(node_gres_config);
		xfree(*new_config);
		*new_config = new_configured_res;
	}

	return rc;
}
/*
 * Note that a node's configuration has been modified (e.g. "scontol update ..")
 * IN node_name - name of the node for which the gres information applies
 * IN orig_config - Gres information supplied from slurm.conf
 * IN/OUT new_config - Updated gres info from slurm.conf if FastSchedule=0
 * IN/OUT gres_list - List of Gres records for this node to track usage
 * IN fast_schedule - 0: Validate and use actual hardware configuration
 *		      1: Validate hardware config, but use slurm.conf config
 *		      2: Don't validate hardware, use slurm.conf configuration
 */
extern int gres_plugin_node_reconfig(char *node_name,
				     char *orig_config,
				     char **new_config,
				     List *gres_list,
				     uint16_t fast_schedule)
{
	int i, rc;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}
	for (i=0; ((i < gres_context_cnt) && (rc == SLURM_SUCCESS)); i++) {
		/* Find gres_state entry on the list */
		gres_iter = list_iterator_create(*gres_list);
		while ((gres_ptr = (gres_state_t *) list_next(gres_iter))){
			if (gres_ptr->plugin_id ==
			    *(gres_context[i].ops.plugin_id))
				break;
		}
		list_iterator_destroy(gres_iter);
		if (gres_ptr == NULL)
			continue;

		rc = _node_reconfig(node_name, orig_config, new_config,
				    &gres_ptr->gres_data, fast_schedule,
				    gres_context[i].ops.gres_name);
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

static int _node_state_pack(void *gres_data, Buf buffer)
{
	gres_node_state_t *gres_ptr = (gres_node_state_t *) gres_data;

	pack32(gres_ptr->gres_cnt_avail,  buffer);
	pack32(gres_ptr->gres_cnt_alloc,  buffer);
	pack_bit_str(gres_ptr->gres_bit_alloc, buffer);

	return SLURM_SUCCESS;
}

static int _node_state_unpack(void **gres_data, Buf buffer)
{
	gres_node_state_t *gres_ptr;

	gres_ptr = xmalloc(sizeof(gres_node_state_t));

	gres_ptr->gres_cnt_found = NO_VAL;
	if (buffer) {
		safe_unpack32(&gres_ptr->gres_cnt_avail,  buffer);
		safe_unpack32(&gres_ptr->gres_cnt_alloc,  buffer);
		unpack_bit_str(&gres_ptr->gres_bit_alloc, buffer);
		if (gres_ptr->gres_bit_alloc == NULL)
			goto unpack_error;
		if (gres_ptr->gres_cnt_avail != 
		    bit_size(gres_ptr->gres_bit_alloc)) {
			gres_ptr->gres_bit_alloc =
					bit_realloc(gres_ptr->gres_bit_alloc,
						    gres_ptr->gres_cnt_avail);
			if (gres_ptr->gres_bit_alloc == NULL)
				goto unpack_error;
		}
		if (gres_ptr->gres_cnt_alloc != 
		    bit_set_count(gres_ptr->gres_bit_alloc)) {
			error("gres _node_state_unpack bit count inconsistent");
			goto unpack_error;
		}
	}

	*gres_data = gres_ptr;
	return SLURM_SUCCESS;

unpack_error:
	FREE_NULL_BITMAP(gres_ptr->gres_bit_alloc);
	xfree(gres_ptr);
	*gres_data = NULL;
	return SLURM_ERROR;
}

/*
 * Pack a node's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_plugin_node_config_validate()
 * IN/OUT buffer - location to write state to
 * IN node_name - name of the node for which the gres information applies
 */
extern int gres_plugin_node_state_pack(List gres_list, Buf buffer,
				       char *node_name)
{
	int i, rc = SLURM_SUCCESS, rc2;
	uint32_t top_offset, gres_size = 0;
	uint32_t header_offset, size_offset, data_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (gres_list == NULL) {
		pack16(rec_cnt, buffer);
		return rc;
	}

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			header_offset = get_buf_offset(buffer);
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			size_offset = get_buf_offset(buffer);
			pack32(gres_size, buffer);	/* placeholder */
			data_offset = get_buf_offset(buffer);
			rc2 = _node_state_pack(gres_ptr->gres_data, buffer);
			if (rc2 != SLURM_SUCCESS) {
				rc = rc2;
				set_buf_offset(buffer, header_offset);
				break;
			}
			tail_offset = get_buf_offset(buffer);
			set_buf_offset(buffer, size_offset);
			gres_size = tail_offset - data_offset;
			pack32(gres_size, buffer);
			set_buf_offset(buffer, tail_offset);
			rec_cnt++;
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to pack record for "
			      "node %s",
			      gres_ptr->plugin_id, node_name);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

/*
 * Unpack a node's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_plugin_node_state_pack()
 * IN/OUT buffer - location to read state from
 * IN node_name - name of the node for which the gres information applies
 */
extern int gres_plugin_node_state_unpack(List *gres_list, Buf buffer,
					 char *node_name)
{
	int i, rc, rc2;
	uint32_t gres_size, magic, tail_offset, plugin_id;
	uint16_t rec_cnt;
	gres_state_t *gres_ptr;
	void *gres_data;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_node_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}

	for (i=0; i<gres_context_cnt; i++)
		gres_context[i].unpacked_info = false;

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;
		safe_unpack32(&magic, buffer);
		if (magic != GRES_MAGIC)
			goto unpack_error;
		safe_unpack32(&plugin_id, buffer);
		safe_unpack32(&gres_size, buffer);
		for (i=0; i<gres_context_cnt; i++) {
			if (*(gres_context[i].ops.plugin_id) == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("gres_plugin_node_state_unpack: no plugin "
			      "configured to unpack data type %u from node %s",
			      plugin_id, node_name);
			/* A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data. */
			tail_offset = get_buf_offset(buffer);
			tail_offset += gres_size;
			set_buf_offset(buffer, tail_offset);
			continue;
		}
		gres_context[i].unpacked_info = true;
		rc2 = _node_state_unpack(&gres_data, buffer);
		if (rc2 != SLURM_SUCCESS) {
			error("gres_plugin_node_state_unpack: error unpacking "
			      "data of type %s from node %s",
			      gres_context[i].ops.gres_name, node_name);
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}

fini:	/* Insure that every gres plugin is called for unpack, even if no data
	 * was packed by the node. A likely sign that GresPlugins is
	 * inconsistently configured. */
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_context[i].unpacked_info)
			continue;
		error("gres_plugin_node_state_unpack: no info packed for %s "
		      "by node %s",
		      gres_context[i].gres_type, node_name);
		rc2 = _node_state_unpack(&gres_data, NULL);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;

unpack_error:
	error("gres_plugin_node_state_unpack: unpack error from node %s",
	      node_name);
	rc = SLURM_ERROR;
	goto fini;
}

static void *_node_state_dup(void *gres_data)
{
	gres_node_state_t *gres_ptr = (gres_node_state_t *) gres_data;
	gres_node_state_t *new_gres;

	if (gres_ptr == NULL)
		return NULL;

	new_gres = xmalloc(sizeof(gres_node_state_t));
	new_gres->gres_cnt_found  = gres_ptr->gres_cnt_found;
	new_gres->gres_cnt_config = gres_ptr->gres_cnt_config;
	new_gres->gres_cnt_avail  = gres_ptr->gres_cnt_avail;
	new_gres->gres_cnt_alloc  = gres_ptr->gres_cnt_alloc;
	new_gres->gres_bit_alloc  = bit_copy(gres_ptr->gres_bit_alloc);

	return new_gres;
}

/*
 * Duplicate a node gres status (used for will-run logic)
 * IN gres_list - node gres state information
 * RET a copy of gres_list or NULL on failure
 */
extern List gres_plugin_node_state_dup(List gres_list)
{
	int i;
	List new_list = NULL;
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres;
	void *gres_data;

	if (gres_list == NULL)
		return new_list;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0)) {
		new_list = list_create(_gres_node_list_delete);
		if (new_list == NULL)
			fatal("list_create malloc failure");
	}
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			gres_data = _node_state_dup(gres_ptr->gres_data);
			if (gres_data) {
				new_gres = xmalloc(sizeof(gres_state_t));
				new_gres->plugin_id = gres_ptr->plugin_id;
				new_gres->gres_data = gres_data;
				list_append(new_list, new_gres);
			}
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to dup node record",
			      gres_ptr->plugin_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_list;
}

static void _node_state_dealloc(void *gres_data)
{
	gres_node_state_t *gres_ptr = (gres_node_state_t *) gres_data;

	gres_ptr->gres_cnt_alloc = 0;
	if (gres_ptr->gres_bit_alloc) {
		int i = bit_size(gres_ptr->gres_bit_alloc) - 1;
		if (i > 0)
			bit_nclear(gres_ptr->gres_bit_alloc, 0, i);
	}
}

/*
 * Deallocate all resources on this node previous allocated to any jobs.
 *	This function isused to synchronize state after slurmctld restarts or
 *	is reconfigured.
 * IN gres_list - node gres state information
 */
extern void gres_plugin_node_state_dealloc(List gres_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (gres_list == NULL)
		return;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			_node_state_dealloc(gres_ptr->gres_data);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

static int _node_state_realloc(void *job_gres_data, int node_offset,
			       void *node_gres_data, char *gres_name)
{
	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_node_state_t *node_gres_ptr = (gres_node_state_t *) node_gres_data;
	int i, job_bit_size, node_bit_size;

	xassert(job_gres_ptr);
	xassert(node_gres_ptr);

	if (node_offset >= job_gres_ptr->node_cnt) {
		error("gres: %s job node offset is bad (%d >= %u)",
		      gres_name, node_offset, job_gres_ptr->node_cnt);
		return EINVAL;
	}

	if ((job_gres_ptr->gres_bit_alloc == NULL) ||
	    (job_gres_ptr->gres_bit_alloc[node_offset] == NULL)) {
		error("gres/%s:job bit_alloc is NULL", gres_name);
		return EINVAL;
	}

	if (node_gres_ptr->gres_bit_alloc == NULL) {
		error("gres/%s: node bit_alloc is NULL", gres_name);
		return EINVAL;
	}

	job_bit_size  = bit_size(job_gres_ptr->gres_bit_alloc[node_offset]);
	node_bit_size = bit_size(node_gres_ptr->gres_bit_alloc);
	if (job_bit_size > node_bit_size) {
		error("gres/%s: job/node bit size mismatch (%d != %d)",
		      gres_name, job_bit_size, node_bit_size);
		/* Node needs to register with more resources, expand
		 * node's bitmap now so we can merge the data */
		node_gres_ptr->gres_bit_alloc =
				bit_realloc(node_gres_ptr->gres_bit_alloc,
					    job_bit_size);
		if (node_gres_ptr->gres_bit_alloc == NULL)
			fatal("bit_realloc: malloc failure");
		node_bit_size = job_bit_size;
	}
	if (job_bit_size < node_bit_size) {
		error("gres/%s: job/node bit size mismatch (%d != %d)",
		      gres_name, job_bit_size, node_bit_size);
		/* Update what we can */
		node_bit_size = MIN(job_bit_size, node_bit_size);
		for (i=0; i<node_bit_size; i++) {
			if (!bit_test(job_gres_ptr->gres_bit_alloc[node_offset],
				      i))
				continue;
			node_gres_ptr->gres_cnt_alloc++;
			bit_set(node_gres_ptr->gres_bit_alloc, i);
		}
	} else {
		node_gres_ptr->gres_cnt_alloc += bit_set_count(job_gres_ptr->
							       gres_bit_alloc
							       [node_offset]);
		bit_or(node_gres_ptr->gres_bit_alloc,
		       job_gres_ptr->gres_bit_alloc[node_offset]);
	}

	return SLURM_SUCCESS;
}

/*
 * Allocate in this nodes record the resources previously allocated to this
 *	job. This function isused to synchronize state after slurmctld restarts
 *	or is reconfigured.
 * IN job_gres_list - job gres state information
 * IN node_offset - zero-origin index of this node in the job's allocation
 * IN node_gres_list - node gres state information
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_node_state_realloc(List job_gres_list, int node_offset,
					  List node_gres_list)
{
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;
	int i;

	if (job_gres_list == NULL)
		return SLURM_SUCCESS;
	if (node_gres_list == NULL)
		return SLURM_ERROR;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *)
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL) {
			error("Could not find plugin id %u to realloc job",
			      job_gres_ptr->plugin_id);
			continue;
		}

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			_node_state_realloc(job_gres_ptr->gres_data, 
					    node_offset,
					    node_gres_ptr->gres_data,
					    gres_context[i].ops.gres_name);
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return SLURM_SUCCESS;
}

static void _node_state_log(void *gres_data, char *node_name, char *gres_name)
{
	gres_node_state_t *gres_ptr;

	xassert(gres_data);
	gres_ptr = (gres_node_state_t *) gres_data;
	info("gres/%s: state for %s", gres_name, node_name);
	info("  gres_cnt found:%u configured:%u avail:%u alloc:%u",
	     gres_ptr->gres_cnt_found, gres_ptr->gres_cnt_config,
	     gres_ptr->gres_cnt_avail, gres_ptr->gres_cnt_alloc);
	if (gres_ptr->gres_bit_alloc) {
		char tmp_str[128];
		bit_fmt(tmp_str, sizeof(tmp_str), gres_ptr->gres_bit_alloc);
		info("  gres_bit_alloc:%s", tmp_str);
	} else {
		info("  gres_bit_alloc:NULL");
	}
}

/*
 * Log a node's current gres state
 * IN gres_list - generated by gres_plugin_node_config_validate()
 * IN node_name - name of the node for which the gres information applies
 */
extern void gres_plugin_node_state_log(List gres_list, char *node_name)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (!gres_debug || (gres_list == NULL))
		return;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			_node_state_log(gres_ptr->gres_data, node_name,
					gres_context[i].ops.gres_name);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

static void _job_state_delete(void *gres_data)
{
	int i;
	gres_job_state_t *gres_ptr = (gres_job_state_t *) gres_data;

	if (gres_ptr == NULL)
		return;

	if (gres_ptr->gres_bit_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_alloc[i]);
		xfree(gres_ptr->gres_bit_alloc);
	}
	if (gres_ptr->gres_bit_step_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_step_alloc[i]);
		xfree(gres_ptr->gres_bit_step_alloc);
	}
	xfree(gres_ptr);
}

static void _gres_job_list_delete(void *list_element)
{
	int i;
	gres_state_t *gres_ptr;

	if (gres_plugin_init() != SLURM_SUCCESS)
		return;

	gres_ptr = (gres_state_t *) list_element;
	slurm_mutex_lock(&gres_context_lock);
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_ptr->plugin_id != *(gres_context[i].ops.plugin_id))
			continue;
		_job_state_delete(gres_ptr->gres_data);
		xfree(gres_ptr);
		break;
	}
	slurm_mutex_unlock(&gres_context_lock);
}

static int _job_state_validate(char *config, void **gres_data, char *gres_name)
{
	char *last = NULL;
	char name_colon[128];
	int name_colon_len;
	gres_job_state_t *gres_ptr;
	uint32_t cnt;
	uint8_t mult = 0;

	name_colon_len = snprintf(name_colon, sizeof(name_colon), "%s:",
				  gres_name);
	if (!strcmp(config, gres_name)) {
		cnt = 1;
	} else if (!strncmp(config, name_colon, name_colon_len)) {
		cnt = strtol(config+name_colon_len, &last, 10);
		if (last[0] == '\0')
			;
		else if ((last[0] == 'k') || (last[0] == 'K'))
			cnt *= 1024;
		else if (!strcasecmp(last, "*cpu"))
			mult = 1;
		else
			return SLURM_ERROR;
		if (cnt == 0)
			return SLURM_ERROR;
	} else
		return SLURM_ERROR;

	gres_ptr = xmalloc(sizeof(gres_job_state_t));
	gres_ptr->gres_cnt_alloc = cnt;
	gres_ptr->gres_cnt_mult  = mult;
	*gres_data = gres_ptr;
	return SLURM_SUCCESS;
}

/*
 * Given a job's requested gres configuration, validate it and build a gres list
 * IN req_config - job request's gres input string
 * OUT gres_list - List of Gres records for this job to track usage
 * RET SLURM_SUCCESS or ESLURM_INVALID_GRES
 */
extern int gres_plugin_job_state_validate(char *req_config, List *gres_list)
{
	char *tmp_str, *tok, *last = NULL;
	int i, rc, rc2;
	gres_state_t *gres_ptr;
	void *gres_data;

	if ((req_config == NULL) || (req_config[0] == '\0')) {
		*gres_list = NULL;
		return SLURM_SUCCESS;
	}

	if ((rc = gres_plugin_init()) != SLURM_SUCCESS)
		return rc;

	slurm_mutex_lock(&gres_context_lock);
	tmp_str = xstrdup(req_config);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_job_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}

	tok = strtok_r(tmp_str, ",", &last);
	while (tok && (rc == SLURM_SUCCESS)) {
		rc2 = SLURM_ERROR;
		for (i=0; i<gres_context_cnt; i++) {
			rc2 = _job_state_validate(tok, &gres_data,
						  gres_context[i].ops.gres_name);
			if (rc2 != SLURM_SUCCESS)
				continue;
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
			break;		/* processed it */
		}
		if (rc2 != SLURM_SUCCESS) {
			info("Invalid gres job specification %s", tok);
			rc = ESLURM_INVALID_GRES;
			break;
		}
		tok = strtok_r(NULL, ",", &last);
	}
	slurm_mutex_unlock(&gres_context_lock);

	xfree(tmp_str);
	return rc;
}

static void *_job_state_dup(void *gres_data)
{

	int i;
	gres_job_state_t *gres_ptr = (gres_job_state_t *) gres_data;
	gres_job_state_t *new_gres_ptr;

	if (gres_ptr == NULL)
		return NULL;

	new_gres_ptr = xmalloc(sizeof(gres_job_state_t));
	new_gres_ptr->gres_cnt_alloc	= gres_ptr->gres_cnt_alloc;
	new_gres_ptr->gres_cnt_mult	= gres_ptr->gres_cnt_mult;
	new_gres_ptr->node_cnt		= gres_ptr->node_cnt;
	new_gres_ptr->gres_bit_alloc	= xmalloc(sizeof(bitstr_t *) *
						  gres_ptr->node_cnt);
	for (i=0; i<gres_ptr->node_cnt; i++) {
		if (gres_ptr->gres_bit_alloc[i] == NULL)
			continue;
		new_gres_ptr->gres_bit_alloc[i] = bit_copy(gres_ptr->
							   gres_bit_alloc[i]);
	}
	return new_gres_ptr;
}

/*
 * Create a copy of a job's gres state
 * IN gres_list - List of Gres records for this job to track usage
 * RET The copy or NULL on failure
 */
List gres_plugin_job_state_dup(List gres_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres_state;
	List new_gres_list = NULL;
	void *new_gres_data;

	if (gres_list == NULL)
		return new_gres_list;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			new_gres_data = _job_state_dup(gres_ptr->gres_data);
			if (new_gres_data == NULL)
				break;
			if (new_gres_list == NULL) {
				new_gres_list = list_create(_gres_job_list_delete);
				if (new_gres_list == NULL)
					fatal("list_create: malloc failure");
			}
			new_gres_state = xmalloc(sizeof(gres_state_t));
			new_gres_state->plugin_id = gres_ptr->plugin_id;
			new_gres_state->gres_data = new_gres_data;
			list_append(new_gres_list, new_gres_state);
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to dup job record",
			      gres_ptr->plugin_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_gres_list;
}

static int _job_state_pack(void *gres_data, Buf buffer)
{
	int i;
	gres_job_state_t *gres_ptr = (gres_job_state_t *) gres_data;

	pack32(gres_ptr->gres_cnt_alloc, buffer);
	pack8 (gres_ptr->gres_cnt_mult,  buffer);

	pack32(gres_ptr->node_cnt,      buffer);
	for (i=0; i<gres_ptr->node_cnt; i++)
		pack_bit_str(gres_ptr->gres_bit_alloc[i], buffer);

	return SLURM_SUCCESS;
}

/*
 * Pack a job's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_plugin_job_config_validate()
 * IN/OUT buffer - location to write state to
 * IN job_id - job's ID
 */
extern int gres_plugin_job_state_pack(List gres_list, Buf buffer,
				      uint32_t job_id)
{
	int i, rc = SLURM_SUCCESS, rc2;
	uint32_t top_offset, gres_size = 0;
	uint32_t header_offset, size_offset, data_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			header_offset = get_buf_offset(buffer);
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			size_offset = get_buf_offset(buffer);
			pack32(gres_size, buffer);	/* placeholder */
			data_offset = get_buf_offset(buffer);
			rc2 = _job_state_pack(gres_ptr->gres_data, buffer);
			if (rc2 != SLURM_SUCCESS) {
				rc = rc2;
				set_buf_offset(buffer, header_offset);
				continue;
			}
			tail_offset = get_buf_offset(buffer);
			set_buf_offset(buffer, size_offset);
			gres_size = tail_offset - data_offset;
			pack32(gres_size, buffer);
			set_buf_offset(buffer, tail_offset);
			rec_cnt++;
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to pack record for "
			      "job %u",
			      gres_ptr->plugin_id, job_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

static int _job_state_unpack(void **gres_data, Buf buffer, char *gres_name)
{
	int i;
	gres_job_state_t *gres_ptr;

	gres_ptr = xmalloc(sizeof(gres_job_state_t));

	if (buffer) {
		safe_unpack32(&gres_ptr->gres_cnt_alloc,  buffer);
		safe_unpack8 (&gres_ptr->gres_cnt_mult,   buffer);

		safe_unpack32(&gres_ptr->node_cnt,       buffer);
		gres_ptr->gres_bit_alloc = xmalloc(sizeof(bitstr_t *) *
						   gres_ptr->node_cnt);
		for (i=0; i<gres_ptr->node_cnt; i++)
			unpack_bit_str(&gres_ptr->gres_bit_alloc[i], buffer);
	}

	*gres_data = gres_ptr;
	return SLURM_SUCCESS;

unpack_error:
	error("Unpacking gres/%s job state info", gres_name);
	if (gres_ptr->gres_bit_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_alloc[i]);
		xfree(gres_ptr->gres_bit_alloc);
	}
	xfree(gres_ptr);
	*gres_data = NULL;
	return SLURM_ERROR;
}

/*
 * Unpack a job's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_plugin_job_state_pack()
 * IN/OUT buffer - location to read state from
 * IN job_id - job's ID
 */
extern int gres_plugin_job_state_unpack(List *gres_list, Buf buffer,
					uint32_t job_id)
{
	int i, rc, rc2;
	uint32_t gres_size, magic, tail_offset, plugin_id;
	uint16_t rec_cnt;
	gres_state_t *gres_ptr;
	void *gres_data;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_job_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}

	for (i=0; i<gres_context_cnt; i++)
		gres_context[i].unpacked_info = false;

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;
		safe_unpack32(&magic, buffer);
		if (magic != GRES_MAGIC)
			goto unpack_error;
		safe_unpack32(&plugin_id, buffer);
		safe_unpack32(&gres_size, buffer);
		for (i=0; i<gres_context_cnt; i++) {
			if (*(gres_context[i].ops.plugin_id) == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("gres_plugin_job_state_unpack: no plugin "
			      "configured to unpack data type %u from job %u",
			      plugin_id, job_id);
			/* A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data. */
			tail_offset = get_buf_offset(buffer);
			tail_offset += gres_size;
			set_buf_offset(buffer, tail_offset);
			continue;
		}
		gres_context[i].unpacked_info = true;
		rc2 = _job_state_unpack(&gres_data, buffer,
					gres_context[i].ops.gres_name);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}

fini:	/* Insure that every gres plugin is called for unpack, even if no data
	 * was packed by the job. A likely sign that GresPlugins is
	 * inconsistently configured. */
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_context[i].unpacked_info)
			continue;
		debug("gres_plugin_job_state_unpack: no info packed for %s "
		      "by job %u",
		      gres_context[i].gres_type, job_id);
		rc2 = _job_state_unpack(&gres_data, NULL,
					gres_context[i].ops.gres_name);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;

unpack_error:
	error("gres_plugin_job_state_unpack: unpack error from job %u",
	      job_id);
	rc = SLURM_ERROR;
	goto fini;
}

static uint32_t _job_test(void *job_gres_data, void *node_gres_data,
			  bool use_total_gres)
{
	uint32_t gres_avail;
	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_node_state_t *node_gres_ptr = (gres_node_state_t *) node_gres_data;

	gres_avail = node_gres_ptr->gres_cnt_avail;
	if (!use_total_gres)
		gres_avail -= node_gres_ptr->gres_cnt_alloc;

	if (job_gres_ptr->gres_cnt_mult == 0) {
		/* per node gres limit */
		if (job_gres_ptr->gres_cnt_alloc > gres_avail)
			return (uint32_t) 0;
		return NO_VAL;
	} else {
		/* per CPU gres limit */
		return (uint32_t) (gres_avail / job_gres_ptr->gres_cnt_alloc);
	}
}

/*
 * Determine how many CPUs on the node can be used by this job
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * IN node_gres_list - node's gres_list built by
 *                     gres_plugin_node_config_validate()
 * IN use_total_gres - if set then consider all gres resources as available,
 *		       and none are commited to running jobs
 * RET: NO_VAL    - All CPUs on node are available
 *      otherwise - Specific CPU count
 */
extern uint32_t gres_plugin_job_test(List job_gres_list, List node_gres_list,
				     bool use_total_gres)
{
	int i;
	uint32_t cpu_cnt, tmp_cnt;
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;

	if (job_gres_list == NULL)
		return NO_VAL;
	if (node_gres_list == NULL)
		return NO_VAL;

	cpu_cnt = NO_VAL;
	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *)
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL) {
			/* node lack resources required by the job */
			cpu_cnt = 0;
			break;
		}

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			tmp_cnt = _job_test(job_gres_ptr->gres_data,
					    node_gres_ptr->gres_data,
					    use_total_gres);
			cpu_cnt = MIN(tmp_cnt, cpu_cnt);
			break;
		}
		if (cpu_cnt == 0)
			break;
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return cpu_cnt;
}

extern int _job_alloc(void *job_gres_data, void *node_gres_data,
		      int node_cnt, int node_offset, uint32_t cpu_cnt,
		      char *gres_name)
{
	int i;
	uint32_t gres_cnt;
	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_node_state_t *node_gres_ptr = (gres_node_state_t *) node_gres_data;

	/*
	 * Validate data structures. Either job_gres_data->node_cnt and
	 * job_gres_data->gres_bit_alloc are both set or both zero/NULL.
	 */
	xassert(node_cnt);
	xassert(node_offset >= 0);
	xassert(job_gres_ptr);
	xassert(node_gres_ptr);
	xassert(node_gres_ptr->gres_bit_alloc);
	if (job_gres_ptr->node_cnt == 0) {
		job_gres_ptr->node_cnt = node_cnt;
		if (job_gres_ptr->gres_bit_alloc) {
			error("gres/%s: node_cnt==0 and bit_alloc is set",
			      gres_name);
			xfree(job_gres_ptr->gres_bit_alloc);
		}
		job_gres_ptr->gres_bit_alloc = xmalloc(sizeof(bitstr_t *) *
						      node_cnt);
	} else if (job_gres_ptr->node_cnt < node_cnt) {
		error("gres/%s: node_cnt increase from %u to %d",
		      gres_name, job_gres_ptr->node_cnt, node_cnt);
		if (node_offset >= job_gres_ptr->node_cnt)
			return SLURM_ERROR;
	} else if (job_gres_ptr->node_cnt > node_cnt) {
		error("gres/%s: node_cnt decrease from %u to %d",
		      gres_name, job_gres_ptr->node_cnt, node_cnt);
	}

	/*
	 * Check that sufficient resources exist on this node
	 */
	if (job_gres_ptr->gres_cnt_mult == 0)
		gres_cnt = job_gres_ptr->gres_cnt_alloc;
	else
		gres_cnt = (job_gres_ptr->gres_cnt_alloc * cpu_cnt);
	i =  node_gres_ptr->gres_cnt_alloc + gres_cnt;
	i -= node_gres_ptr->gres_cnt_avail;
	if (i > 0) {
		error("gres/%s: overallocated resources by %d", gres_name, i);
		/* proceed with request, give job what's available */
	}

	/*
	 * Select the specific resources to use for this job.
	 * We'll need to add topology information in the future
	 */
	if (job_gres_ptr->gres_bit_alloc[node_offset]) {
		/* Resuming a suspended job, resources already allocated */
		debug("gres/%s: job's bit_alloc is already set for node %d",
		      gres_name, node_offset);
		gres_cnt = MIN(bit_size(node_gres_ptr->gres_bit_alloc),
			       bit_size(job_gres_ptr->
					gres_bit_alloc[node_offset]));
		for (i=0; i<gres_cnt; i++) {
			if (bit_test(job_gres_ptr->gres_bit_alloc[node_offset],
				     i)) {
				bit_set(node_gres_ptr->gres_bit_alloc, i);
				node_gres_ptr->gres_cnt_alloc++;
			}
		}
	} else {
		job_gres_ptr->gres_bit_alloc[node_offset] = 
				bit_alloc(node_gres_ptr->gres_cnt_avail);
		if (job_gres_ptr->gres_bit_alloc[node_offset] == NULL)
			fatal("bit_copy: malloc failure");
		for (i=0; i<node_gres_ptr->gres_cnt_avail && gres_cnt>0; i++) {
			if (bit_test(node_gres_ptr->gres_bit_alloc, i))
				continue;
			bit_set(node_gres_ptr->gres_bit_alloc, i);
			bit_set(job_gres_ptr->gres_bit_alloc[node_offset], i);
			node_gres_ptr->gres_cnt_alloc++;
			gres_cnt--;
		}
	}

	return SLURM_SUCCESS;
}

/*
 * Allocate resource to a job and update node and job gres information
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * IN node_gres_list - node's gres_list built by
 *		gres_plugin_node_config_validate()
 * IN node_cnt - total number of nodes originally allocated to the job
 * IN node_offset - zero-origin index to the node of interest
 * IN cpu_cnt - number of CPUs allocated to this job on this node
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_job_alloc(List job_gres_list, List node_gres_list, 
				 int node_cnt, int node_offset,
				 uint32_t cpu_cnt)
{
	int i, rc, rc2;
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;

	if (job_gres_list == NULL)
		return SLURM_SUCCESS;
	if (node_gres_list == NULL)
		return SLURM_ERROR;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *) 
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL)
			continue;

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id != 
			    *(gres_context[i].ops.plugin_id))
				continue;
			rc2 = _job_alloc(job_gres_ptr->gres_data, 
					 node_gres_ptr->gres_data, node_cnt,
					 node_offset, cpu_cnt,
					 gres_context[i].ops.gres_name);
			if (rc2 != SLURM_SUCCESS)
				rc = rc2;
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

static int _job_dealloc(void *job_gres_data, void *node_gres_data,
		        int node_offset, char *gres_name)
{
	int i, len;
	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_node_state_t *node_gres_ptr = (gres_node_state_t *) node_gres_data;

	/*
	 * Validate data structures. Either job_gres_data->node_cnt and
	 * job_gres_data->gres_bit_alloc are both set or both zero/NULL.
	 */
	xassert(node_offset >= 0);
	xassert(job_gres_ptr);
	xassert(node_gres_ptr);
	xassert(node_gres_ptr->gres_bit_alloc);
	if (job_gres_ptr->node_cnt <= node_offset) {
		error("gres/%s bad node_offset %d count is %u",
		      gres_name, node_offset, job_gres_ptr->node_cnt);
		return SLURM_ERROR;
	}
	if (job_gres_ptr->gres_bit_alloc == NULL) {
		error("gres/%s job's bitmap is NULL", gres_name);
		return SLURM_ERROR;
	}
	if (job_gres_ptr->gres_bit_alloc[node_offset] == NULL) {
		error("gres/%s: job's bitmap is empty", gres_name);
		return SLURM_ERROR;
	}

	len = bit_size(job_gres_ptr->gres_bit_alloc[node_offset]);
	i   = bit_size(node_gres_ptr->gres_bit_alloc);
	if (i != len) {
		error("gres/%s: job and node bitmap sizes differ (%d != %d)",
		      gres_name, len, i);
		len = MIN(len, i);
		/* proceed with request, make best effort */
	}
	for (i=0; i<len; i++) {
		if (!bit_test(job_gres_ptr->gres_bit_alloc[node_offset], i))
			continue;
		bit_clear(node_gres_ptr->gres_bit_alloc, i);
		/* NOTE: Do not clear bit from
		 * job_gres_ptr->gres_bit_alloc[node_offset]
		 * since this may only be an emulated deallocate */
		node_gres_ptr->gres_cnt_alloc--;
	}

	return SLURM_SUCCESS;
}

/*
 * Deallocate resource from a job and update node and job gres information
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * IN node_gres_list - node's gres_list built by
 *		gres_plugin_node_config_validate()
 * IN node_offset - zero-origin index to the node of interest
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_job_dealloc(List job_gres_list, List node_gres_list, 
				   int node_offset)
{
	int i, rc, rc2;
	ListIterator job_gres_iter,  node_gres_iter;
	gres_state_t *job_gres_ptr, *node_gres_ptr;

	if (job_gres_list == NULL)
		return SLURM_SUCCESS;
	if (node_gres_list == NULL)
		return SLURM_ERROR;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	job_gres_iter = list_iterator_create(job_gres_list);
	while ((job_gres_ptr = (gres_state_t *) list_next(job_gres_iter))) {
		node_gres_iter = list_iterator_create(node_gres_list);
		while ((node_gres_ptr = (gres_state_t *) 
				list_next(node_gres_iter))) {
			if (job_gres_ptr->plugin_id == node_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(node_gres_iter);
		if (node_gres_ptr == NULL)
			continue;

		for (i=0; i<gres_context_cnt; i++) {
			if (job_gres_ptr->plugin_id != 
			    *(gres_context[i].ops.plugin_id))
				continue;
			rc2 = _job_dealloc(job_gres_ptr->gres_data, 
					   node_gres_ptr->gres_data,
					   node_offset,
					   gres_context[i].ops.gres_name);
			if (rc2 != SLURM_SUCCESS)
				rc = rc2;
			break;
		}
	}
	list_iterator_destroy(job_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}

static void _job_state_log(void *gres_data, uint32_t job_id, char *gres_name)
{
	gres_job_state_t *gres_ptr;
	char *mult, tmp_str[128];
	int i;

	xassert(gres_data);
	gres_ptr = (gres_job_state_t *) gres_data;
	info("gres: %s state for job %u", gres_name, job_id);
	if (gres_ptr->gres_cnt_mult)
		mult = "cpu";
	else
		mult = "node";
	info("  gres_cnt:%u per %s node_cnt:%u", gres_ptr->gres_cnt_alloc, mult,
	     gres_ptr->node_cnt);

	if (gres_ptr->node_cnt && gres_ptr->gres_bit_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_ptr->gres_bit_alloc[i]);
			info("  gres_bit_alloc[%d]:%s", i, tmp_str);
		}
	} else {
		info("  gres_bit_alloc:NULL");
	}

	if (gres_ptr->node_cnt && gres_ptr->gres_bit_step_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_ptr->gres_bit_step_alloc[i]);
			info("  gres_bit_step_alloc[%d]:%s", i, tmp_str);
		}
	} else {
		info("  gres_bit_step_alloc:NULL");
	}
}

/*
 * Log a job's current gres state
 * IN gres_list - generated by gres_plugin_job_state_validate()
 * IN job_id - job's ID
 */
extern void gres_plugin_job_state_log(List gres_list, uint32_t job_id)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (!gres_debug || (gres_list == NULL))
		return;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			_job_state_log(gres_ptr->gres_data, job_id,
				       gres_context[i].ops.gres_name);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

static void _step_state_delete(void *gres_data)
{
	int i;
	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;

	if (gres_ptr == NULL)
		return;

	if (gres_ptr->gres_bit_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_alloc[i]);
		xfree(gres_ptr->gres_bit_alloc);
	}
	xfree(gres_ptr);
}

static void _gres_step_list_delete(void *list_element)
{
	int i;
	gres_state_t *gres_ptr;

	if (gres_plugin_init() != SLURM_SUCCESS)
		return;

	gres_ptr = (gres_state_t *) list_element;
	slurm_mutex_lock(&gres_context_lock);
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_ptr->plugin_id != *(gres_context[i].ops.plugin_id))
			continue;
		_step_state_delete(gres_ptr->gres_data);
		xfree(gres_ptr);
		break;
	}
	slurm_mutex_unlock(&gres_context_lock);
}

static int _step_state_validate(char *config, void **gres_data, char *gres_name)
{
	int name_colon_len;
	char *last = NULL, name_colon[128];
	gres_job_state_t *gres_ptr;
	uint32_t cnt;
	uint8_t mult = 0;

	name_colon_len = snprintf(name_colon, sizeof(name_colon), "%s:",
				  gres_name);
	if (!strcmp(config, gres_name)) {
		cnt = 1;
	} else if (!strncmp(config, name_colon, name_colon_len)) {
		cnt = strtol(config+name_colon_len, &last, 10);
		if (last[0] == '\0')
			;
		else if ((last[0] == 'k') || (last[0] == 'K'))
			cnt *= 1024;
		else if (!strcasecmp(last, "*cpu"))
			mult = 1;
		else
			return SLURM_ERROR;
		if (cnt == 0)
			return SLURM_ERROR;
	} else
		return SLURM_ERROR;

	gres_ptr = xmalloc(sizeof(gres_step_state_t));
	gres_ptr->gres_cnt_alloc = cnt;
	gres_ptr->gres_cnt_mult  = mult;
	*gres_data = gres_ptr;
	return SLURM_SUCCESS;
}

static uint32_t _step_test(void *step_gres_data, void *job_gres_data,
			   int node_offset, bool ignore_alloc, char *gres_name)
{
	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_step_state_t *step_gres_ptr = (gres_step_state_t *) step_gres_data;
	uint32_t gres_cnt;

	xassert(job_gres_ptr);
	xassert(step_gres_ptr);

	if (node_offset == NO_VAL) {
		if (step_gres_ptr->gres_cnt_alloc > job_gres_ptr->gres_cnt_alloc)
			return 0;
		return NO_VAL;
	}

	if (node_offset >= job_gres_ptr->node_cnt) {
		error("gres/%s: step_test node offset invalid (%d >= %u)",
		      gres_name, node_offset, job_gres_ptr->node_cnt);
		return 0;
	}
	if ((job_gres_ptr->gres_bit_alloc == NULL) ||
	    (job_gres_ptr->gres_bit_alloc[node_offset] == NULL)) {
		error("gres/%s: step_test gres_bit_alloc is NULL", gres_name);
		return 0;
	}

	gres_cnt = bit_set_count(job_gres_ptr->gres_bit_alloc[node_offset]);
	if (!ignore_alloc &&
	    job_gres_ptr->gres_bit_step_alloc &&
	    job_gres_ptr->gres_bit_step_alloc[node_offset]) {
		gres_cnt -= bit_set_count(job_gres_ptr->
					  gres_bit_step_alloc[node_offset]);
	}
	if (step_gres_ptr->gres_cnt_mult)	/* Gres count per CPU */
		gres_cnt /= step_gres_ptr->gres_cnt_alloc;
	else if (step_gres_ptr->gres_cnt_alloc > gres_cnt)
		gres_cnt = 0;
	else
		gres_cnt = NO_VAL;

	return gres_cnt;
}

/*
 * Given a step's requested gres configuration, validate it and build gres list
 * IN req_config - step request's gres input string
 * OUT step_gres_list - List of Gres records for this step to track usage
 * IN job_gres_list - List of Gres records for this job
 * RET SLURM_SUCCESS or ESLURM_INVALID_GRES
 */
extern int gres_plugin_step_state_validate(char *req_config,
					   List *step_gres_list,
					   List job_gres_list)
{
	char *tmp_str, *tok, *last = NULL;
	int i, rc, rc2, rc3;
	gres_state_t *step_gres_ptr, *job_gres_ptr;
	void *step_gres_data, *job_gres_data;
	ListIterator job_gres_iter;

	*step_gres_list = NULL;
	if ((req_config == NULL) || (req_config[0] == '\0'))
		return SLURM_SUCCESS;
	if (job_gres_list == NULL) {
		info("step has gres spec, while job has none");
		return SLURM_ERROR;
	}

	if ((rc = gres_plugin_init()) != SLURM_SUCCESS)
		return rc;

	slurm_mutex_lock(&gres_context_lock);
	tmp_str = xstrdup(req_config);
	if ((gres_context_cnt > 0) && (*step_gres_list == NULL)) {
		*step_gres_list = list_create(_gres_step_list_delete);
		if (*step_gres_list == NULL)
			fatal("list_create malloc failure");
	}

	tok = strtok_r(tmp_str, ",", &last);
	while (tok && (rc == SLURM_SUCCESS)) {
		rc2 = SLURM_ERROR;
		for (i=0; i<gres_context_cnt; i++) {
			rc2 = _step_state_validate(tok, &step_gres_data,
						   gres_context[i].ops.gres_name);
			if (rc2 != SLURM_SUCCESS)
				continue;
			/* Now make sure the step's request isn't too big for
			 * the job's gres allocation */
			job_gres_iter = list_iterator_create(job_gres_list);
			while ((job_gres_ptr = (gres_state_t *)
					list_next(job_gres_iter))) {
				if (job_gres_ptr->plugin_id ==
				    *(gres_context[i].ops.plugin_id))
					break;
			}
			list_iterator_destroy(job_gres_iter);
			if (job_gres_ptr == NULL) {
				info("Step gres request not in job alloc %s",
				     tok);
				rc = ESLURM_INVALID_GRES;
				break;
			}
			job_gres_data = job_gres_ptr->gres_data;
			rc3 = _step_test(step_gres_data, job_gres_data, NO_VAL,
					 true, gres_context[i].ops.gres_name);
			if (rc3 == 0) {
				info("Step gres more than in job allocation %s",
				     tok);
				rc = ESLURM_INVALID_GRES;
				break;
			}

			step_gres_ptr = xmalloc(sizeof(gres_state_t));
			step_gres_ptr->plugin_id = *(gres_context[i].ops.
						     plugin_id);
			step_gres_ptr->gres_data = step_gres_data;
			list_append(*step_gres_list, step_gres_ptr);
			break;		/* processed it */
		}
		if (rc2 != SLURM_SUCCESS) {
			info("Invalid gres step specification %s", tok);
			rc = ESLURM_INVALID_GRES;
			break;
		}
		tok = strtok_r(NULL, ",", &last);
	}
	slurm_mutex_unlock(&gres_context_lock);

	xfree(tmp_str);
	return rc;
}

static void *_step_state_dup(void *gres_data)
{

	int i;
	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;
	gres_step_state_t *new_gres_ptr;

	if (gres_ptr == NULL)
		return NULL;

	new_gres_ptr = xmalloc(sizeof(gres_step_state_t));
	new_gres_ptr->gres_cnt_alloc	= gres_ptr->gres_cnt_alloc;
	new_gres_ptr->gres_cnt_mult	= gres_ptr->gres_cnt_mult;
	new_gres_ptr->node_cnt		= gres_ptr->node_cnt;
	new_gres_ptr->gres_bit_alloc	= xmalloc(sizeof(bitstr_t *) *
						  gres_ptr->node_cnt);
	for (i=0; i<gres_ptr->node_cnt; i++) {
		if (gres_ptr->gres_bit_alloc[i] == NULL)
			continue;
		new_gres_ptr->gres_bit_alloc[i] = bit_copy(gres_ptr->
							  gres_bit_alloc[i]);
	}
	return new_gres_ptr;
}

/*
 * Create a copy of a step's gres state
 * IN gres_list - List of Gres records for this step to track usage
 * RET The copy or NULL on failure
 */
List gres_plugin_step_state_dup(List gres_list)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr, *new_gres_state;
	List new_gres_list = NULL;
	void *new_gres_data;

	if (gres_list == NULL)
		return new_gres_list;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			new_gres_data = _step_state_dup(gres_ptr->gres_data);
			if (new_gres_data == NULL)
				break;
			if (new_gres_list == NULL) {
				new_gres_list = list_create(_gres_step_list_delete);
				if (new_gres_list == NULL)
					fatal("list_create: malloc failure");
			}
			new_gres_state = xmalloc(sizeof(gres_state_t));
			new_gres_state->plugin_id = gres_ptr->plugin_id;
			new_gres_state->gres_data = new_gres_data;
			list_append(new_gres_list, new_gres_state);
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to dup step record",
			      gres_ptr->plugin_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return new_gres_list;
}

static int _step_state_pack(void *gres_data, Buf buffer)
{
	int i;
	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;

	pack32(gres_ptr->gres_cnt_alloc, buffer);
	pack8 (gres_ptr->gres_cnt_mult,  buffer);

	pack32(gres_ptr->node_cnt,      buffer);
	for (i=0; i<gres_ptr->node_cnt; i++)
		pack_bit_str(gres_ptr->gres_bit_alloc[i], buffer);

	return SLURM_SUCCESS;
}

/*
 * Pack a step's current gres status, called from slurmctld for save/restore
 * IN gres_list - generated by gres_plugin_step_allocate()
 * IN/OUT buffer - location to write state to
 * IN job_id, step_id - job and step ID for logging
 */
extern int gres_plugin_step_state_pack(List gres_list, Buf buffer,
				       uint32_t job_id, uint32_t step_id)
{
	int i, rc = SLURM_SUCCESS, rc2;
	uint32_t top_offset, gres_size = 0;
	uint32_t header_offset, size_offset, data_offset, tail_offset;
	uint32_t magic = GRES_MAGIC;
	uint16_t rec_cnt = 0;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	top_offset = get_buf_offset(buffer);
	pack16(rec_cnt, buffer);	/* placeholder if data */

	if (gres_list == NULL)
		return rc;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			header_offset = get_buf_offset(buffer);
			pack32(magic, buffer);
			pack32(gres_ptr->plugin_id, buffer);
			size_offset = get_buf_offset(buffer);
			pack32(gres_size, buffer);	/* placeholder */
			data_offset = get_buf_offset(buffer);
			rc2 = _step_state_pack(gres_ptr->gres_data, buffer);
			if (rc2 != SLURM_SUCCESS) {
				rc = rc2;
				set_buf_offset(buffer, header_offset);
				continue;
			}
			tail_offset = get_buf_offset(buffer);
			set_buf_offset(buffer, size_offset);
			gres_size = tail_offset - data_offset;
			pack32(gres_size, buffer);
			set_buf_offset(buffer, tail_offset);
			rec_cnt++;
			break;
		}
		if (i >= gres_context_cnt) {
			error("Could not find plugin id %u to pack record for "
			      "step %u.%u",
			      gres_ptr->plugin_id, job_id, step_id);
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	tail_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, top_offset);
	pack16(rec_cnt, buffer);
	set_buf_offset(buffer, tail_offset);

	return rc;
}

static int _step_state_unpack(void **gres_data, Buf buffer, char *gres_name)
{
	int i;
	gres_step_state_t *gres_ptr;

	gres_ptr = xmalloc(sizeof(gres_step_state_t));

	if (buffer) {
		safe_unpack32(&gres_ptr->gres_cnt_alloc,  buffer);
		safe_unpack8 (&gres_ptr->gres_cnt_mult,   buffer);

		safe_unpack32(&gres_ptr->node_cnt,       buffer);
		gres_ptr->gres_bit_alloc = xmalloc(sizeof(bitstr_t *) *
						  gres_ptr->node_cnt);
		for (i=0; i<gres_ptr->node_cnt; i++)
			unpack_bit_str(&gres_ptr->gres_bit_alloc[i], buffer);
	}

	*gres_data = gres_ptr;
	return SLURM_SUCCESS;

unpack_error:
	error("Unpacking gres/%s step state info", gres_name);
	if (gres_ptr->gres_bit_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++)
			FREE_NULL_BITMAP(gres_ptr->gres_bit_alloc[i]);
		xfree(gres_ptr->gres_bit_alloc);
	}
	xfree(gres_ptr);
	*gres_data = NULL;
	return SLURM_ERROR;
}

/*
 * Unpack a step's current gres status, called from slurmctld for save/restore
 * OUT gres_list - restored state stored by gres_plugin_step_state_pack()
 * IN/OUT buffer - location to read state from
 * IN job_id, step_id - job and step ID for logging
 */
extern int gres_plugin_step_state_unpack(List *gres_list, Buf buffer,
					 uint32_t job_id, uint32_t step_id)
{
	int i, rc, rc2;
	uint32_t gres_size, magic, tail_offset, plugin_id;
	uint16_t rec_cnt;
	gres_state_t *gres_ptr;
	void *gres_data;

	safe_unpack16(&rec_cnt, buffer);
	if (rec_cnt == 0)
		return SLURM_SUCCESS;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	if ((gres_context_cnt > 0) && (*gres_list == NULL)) {
		*gres_list = list_create(_gres_step_list_delete);
		if (*gres_list == NULL)
			fatal("list_create malloc failure");
	}

	for (i=0; i<gres_context_cnt; i++)
		gres_context[i].unpacked_info = false;

	while ((rc == SLURM_SUCCESS) && (rec_cnt)) {
		if ((buffer == NULL) || (remaining_buf(buffer) == 0))
			break;
		rec_cnt--;
		safe_unpack32(&magic, buffer);
		if (magic != GRES_MAGIC)
			goto unpack_error;
		safe_unpack32(&plugin_id, buffer);
		safe_unpack32(&gres_size, buffer);
		for (i=0; i<gres_context_cnt; i++) {
			if (*(gres_context[i].ops.plugin_id) == plugin_id)
				break;
		}
		if (i >= gres_context_cnt) {
			error("gres_plugin_step_state_unpack: no plugin "
			      "configured to unpack data type %u from "
			      "step %u.%u",
			      plugin_id, job_id, step_id);
			/* A likely sign that GresPlugins has changed.
			 * Not a fatal error, skip over the data. */
			tail_offset = get_buf_offset(buffer);
			tail_offset += gres_size;
			set_buf_offset(buffer, tail_offset);
			continue;
		}
		gres_context[i].unpacked_info = true;
		rc2 = _step_state_unpack(&gres_data, buffer,
					 gres_context[i].ops.gres_name);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}

fini:	/* Insure that every gres plugin is called for unpack, even if no data
	 * was packed by the job. A likely sign that GresPlugins is
	 * inconsistently configured. */
	for (i=0; i<gres_context_cnt; i++) {
		if (gres_context[i].unpacked_info)
			continue;
		debug("gres_plugin_job_state_unpack: no info packed for %s "
		      "by step %u.%u",
		      gres_context[i].gres_type, job_id, step_id);
		rc2 = _step_state_unpack(&gres_data, NULL,
					 gres_context[i].ops.gres_name);
		if (rc2 != SLURM_SUCCESS) {
			rc = rc2;
		} else {
			gres_ptr = xmalloc(sizeof(gres_state_t));
			gres_ptr->plugin_id = *(gres_context[i].ops.plugin_id);
			gres_ptr->gres_data = gres_data;
			list_append(*gres_list, gres_ptr);
		}
	}
	slurm_mutex_unlock(&gres_context_lock);

	return rc;

unpack_error:
	error("gres_plugin_job_state_unpack: unpack error from step %u.%u",
	      job_id, step_id);
	rc = SLURM_ERROR;
	goto fini;
}

static void _step_state_log(void *gres_data, uint32_t job_id, uint32_t step_id,
			    char *gres_name)
{
	gres_step_state_t *gres_ptr = (gres_step_state_t *) gres_data;
	char *mult, tmp_str[128];
	int i;

	xassert(gres_ptr);
	info("gres/%s state for step %u.%u", gres_name, job_id, step_id);
	if (gres_ptr->gres_cnt_mult)
		mult = "cpu";
	else
		mult = "node";
	info("  gres_cnt:%u per %s node_cnt:%u", gres_ptr->gres_cnt_alloc, mult,
	     gres_ptr->node_cnt);

	if (gres_ptr->node_cnt && gres_ptr->gres_bit_alloc) {
		for (i=0; i<gres_ptr->node_cnt; i++) {
			bit_fmt(tmp_str, sizeof(tmp_str),
				gres_ptr->gres_bit_alloc[i]);
			info("  gres_bit_alloc[%d]:%s", i, tmp_str);
		}
	} else {
		info("  gres_bit_alloc:NULL");
	}
}

/*
 * Log a step's current gres state
 * IN gres_list - generated by gres_plugin_step_allocate()
 * IN job_id - job's ID
 */
extern void gres_plugin_step_state_log(List gres_list, uint32_t job_id,
				       uint32_t step_id)
{
	int i;
	ListIterator gres_iter;
	gres_state_t *gres_ptr;

	if (!gres_debug || (gres_list == NULL))
		return;

	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	gres_iter = list_iterator_create(gres_list);
	while ((gres_ptr = (gres_state_t *) list_next(gres_iter))) {
		for (i=0; i<gres_context_cnt; i++) {
			if (gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			_step_state_log(gres_ptr->gres_data, job_id, step_id,
					gres_context[i].ops.gres_name);
			break;
		}
	}
	list_iterator_destroy(gres_iter);
	slurm_mutex_unlock(&gres_context_lock);
}

/*
 * Determine how many CPUs of a job's allocation can be allocated to a job
 *	on a specific node
 * IN job_gres_list - a running job's gres info
 * IN/OUT step_gres_list - a pending job step's gres requirements
 * IN node_offset - index into the job's node allocation
 * IN ignore_alloc - if set ignore resources already allocated to running steps
 * RET Count of available CPUs on this node, NO_VAL if no limit
 */
extern uint32_t gres_plugin_step_test(List step_gres_list, List job_gres_list,
				      int node_offset, bool ignore_alloc)
{
	int i;
	uint32_t cpu_cnt, tmp_cnt;
	ListIterator  job_gres_iter, step_gres_iter;
	gres_state_t *job_gres_ptr, *step_gres_ptr;

	if (step_gres_list == NULL)
		return NO_VAL;
	if (job_gres_list == NULL)
		return 0;

	cpu_cnt = NO_VAL;
	(void) gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	step_gres_iter = list_iterator_create(step_gres_list);
	while ((step_gres_ptr = (gres_state_t *) list_next(step_gres_iter))) {
		job_gres_iter = list_iterator_create(job_gres_list);
		while ((job_gres_ptr = (gres_state_t *)
				list_next(job_gres_iter))) {
			if (step_gres_ptr->plugin_id == job_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(job_gres_iter);
		if (job_gres_ptr == NULL) {
			/* job lack resources required by the step */
			cpu_cnt = 0;
			break;
		}

		for (i=0; i<gres_context_cnt; i++) {
			if (step_gres_ptr->plugin_id !=
			    *(gres_context[i].ops.plugin_id))
				continue;
			tmp_cnt = _step_test(step_gres_ptr->gres_data,
					     job_gres_ptr->gres_data,
					     node_offset, ignore_alloc,
					     gres_context[i].ops.gres_name);
			cpu_cnt = MIN(tmp_cnt, cpu_cnt);
			break;
		}
		if (cpu_cnt == 0)
			break;
	}
	list_iterator_destroy(step_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return cpu_cnt;
}

static int _step_alloc(void *step_gres_data, void *job_gres_data,
		       int node_offset, int cpu_cnt, char *gres_name)
{
	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_step_state_t *step_gres_ptr = (gres_step_state_t *) step_gres_data;
	uint32_t gres_avail, gres_needed;
	bitstr_t *gres_bit_alloc;

	xassert(job_gres_ptr);
	xassert(step_gres_ptr);
	if (node_offset >= job_gres_ptr->node_cnt) {
		error("gres/%s: step_alloc node offset invalid (%d >= %u)",
		      gres_name, node_offset, job_gres_ptr->node_cnt);
		return SLURM_ERROR;
	}
	if ((job_gres_ptr->gres_bit_alloc == NULL) ||
	    (job_gres_ptr->gres_bit_alloc[node_offset] == NULL)) {
		error("gres/%s: step_alloc gres_bit_alloc is NULL", gres_name);
		return SLURM_ERROR;
	}

	gres_bit_alloc = bit_copy(job_gres_ptr->gres_bit_alloc[node_offset]);
	if (gres_bit_alloc == NULL)
		fatal("bit_copy malloc failure");
	if (job_gres_ptr->gres_bit_step_alloc &&
	    job_gres_ptr->gres_bit_step_alloc[node_offset]) {
		bit_not(job_gres_ptr->gres_bit_step_alloc[node_offset]);
		bit_and(gres_bit_alloc,
			job_gres_ptr->gres_bit_step_alloc[node_offset]);
		bit_not(job_gres_ptr->gres_bit_step_alloc[node_offset]);
	}
	gres_avail  = bit_set_count(gres_bit_alloc);
	gres_needed = step_gres_ptr->gres_cnt_alloc;
	if (step_gres_ptr->gres_cnt_mult)
		gres_needed *= cpu_cnt;
	if (gres_needed > gres_avail) {
		error("gres/%s: step oversubscribing resources on node %d",
		      gres_name, node_offset);
	} else {
		int gres_rem = gres_needed;
		int i, len = bit_size(gres_bit_alloc);
		for (i=0; i<len; i++) {
			if (gres_rem > 0) {
				if (bit_test(gres_bit_alloc, i))
					gres_rem--;
			} else {
				bit_clear(gres_bit_alloc, i);
			}
		}
	}

	if (job_gres_ptr->gres_bit_step_alloc == NULL) {
		job_gres_ptr->gres_bit_step_alloc =
			xmalloc(sizeof(bitstr_t *) * job_gres_ptr->node_cnt);
	}
	if (job_gres_ptr->gres_bit_step_alloc[node_offset]) {
		bit_or(job_gres_ptr->gres_bit_step_alloc[node_offset],
		       gres_bit_alloc);
	} else {
		job_gres_ptr->gres_bit_step_alloc[node_offset] =
			bit_copy(gres_bit_alloc);
	}
	if (step_gres_ptr->gres_bit_alloc == NULL) {
		step_gres_ptr->gres_bit_alloc = xmalloc(sizeof(bitstr_t *) *
						       job_gres_ptr->node_cnt);
		step_gres_ptr->node_cnt = job_gres_ptr->node_cnt;
	}
	if (step_gres_ptr->gres_bit_alloc[node_offset]) {
		error("gres/%s: step bit_alloc already exists", gres_name);
		bit_or(step_gres_ptr->gres_bit_alloc[node_offset],gres_bit_alloc);
		FREE_NULL_BITMAP(gres_bit_alloc);
	} else {
		step_gres_ptr->gres_bit_alloc[node_offset] = gres_bit_alloc;
	}

	return SLURM_SUCCESS;
}

/*
 * Allocate resource to a step and update job and step gres information
 * IN step_gres_list - step's gres_list built by
 *		gres_plugin_step_state_validate()
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * IN node_offset - zero-origin index to the node of interest
 * IN cpu_cnt - number of CPUs allocated to this job on this node
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_step_alloc(List step_gres_list, List job_gres_list,
				  int node_offset, int cpu_cnt)
{
	int i, rc, rc2;
	ListIterator step_gres_iter,  job_gres_iter;
	gres_state_t *step_gres_ptr, *job_gres_ptr;

	if (step_gres_list == NULL)
		return SLURM_SUCCESS;
	if (job_gres_list == NULL)
		return SLURM_ERROR;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	step_gres_iter = list_iterator_create(step_gres_list);
	while ((step_gres_ptr = (gres_state_t *) list_next(step_gres_iter))) {
		job_gres_iter = list_iterator_create(job_gres_list);
		while ((job_gres_ptr = (gres_state_t *) 
				list_next(job_gres_iter))) {
			if (step_gres_ptr->plugin_id == job_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(job_gres_iter);
		if (job_gres_ptr == NULL)
			continue;

		for (i=0; i<gres_context_cnt; i++) {
			if (step_gres_ptr->plugin_id != 
			    *(gres_context[i].ops.plugin_id))
				continue;
			rc2 = _step_alloc(step_gres_ptr->gres_data, 
					  job_gres_ptr->gres_data,
					  node_offset, cpu_cnt,
					  gres_context[i].ops.gres_name);
			if (rc2 != SLURM_SUCCESS)
				rc = rc2;
			break;
		}
	}
	list_iterator_destroy(step_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}


static int _step_dealloc(void *step_gres_data, void *job_gres_data,
			 char *gres_name)
{

	gres_job_state_t  *job_gres_ptr  = (gres_job_state_t *)  job_gres_data;
	gres_step_state_t *step_gres_ptr = (gres_step_state_t *) step_gres_data;
	uint32_t i, j, node_cnt;
	int len_j, len_s;

	xassert(job_gres_ptr);
	xassert(step_gres_ptr);
	node_cnt = MIN(job_gres_ptr->node_cnt, step_gres_ptr->node_cnt);
	if (step_gres_ptr->gres_bit_alloc == NULL) {
		error("gres/%s: step dealloc bit_alloc is NULL", gres_name);
		return SLURM_ERROR;
	}
	if (job_gres_ptr->gres_bit_alloc == NULL) {
		error("gres/%s: step dealloc, job's bit_alloc is NULL",
		      gres_name);
		return SLURM_ERROR;
	}
	for (i=0; i<node_cnt; i++) {
		if (step_gres_ptr->gres_bit_alloc[i] == NULL)
			continue;
		if (job_gres_ptr->gres_bit_alloc[i] == NULL) {
			error("gres/%s: step dealloc, job's bit_alloc[%d] is "
			      "NULL", gres_name, i);
			continue;
		}
		len_j = bit_size(job_gres_ptr->gres_bit_alloc[i]);
		len_s = bit_size(step_gres_ptr->gres_bit_alloc[i]);
		if (len_j != len_s) {
			error("gres/%s: step dealloc, bit_alloc[%d] size "
			      "mis-match (%d != %d)",
			      gres_name, i, len_j, len_s);
			len_j = MIN(len_j, len_s);
		}
		for (j=0; j<len_j; j++) {
			if (!bit_test(step_gres_ptr->gres_bit_alloc[i], j))
				continue;
			if (job_gres_ptr->gres_bit_step_alloc &&
			    job_gres_ptr->gres_bit_step_alloc[i]) {
				bit_clear(job_gres_ptr->gres_bit_step_alloc[i],
					  j);
			}
		}
		FREE_NULL_BITMAP(step_gres_ptr->gres_bit_alloc[i]);
	}

	return SLURM_SUCCESS;
}

/*
 * Deallocate resource to a step and update job and step gres information
 * IN step_gres_list - step's gres_list built by
 *		gres_plugin_step_state_validate()
 * IN job_gres_list - job's gres_list built by gres_plugin_job_state_validate()
 * RET SLURM_SUCCESS or error code
 */
extern int gres_plugin_step_dealloc(List step_gres_list, List job_gres_list)
{
	int i, rc, rc2;
	ListIterator step_gres_iter,  job_gres_iter;
	gres_state_t *step_gres_ptr, *job_gres_ptr;

	if (step_gres_list == NULL)
		return SLURM_SUCCESS;
	if (job_gres_list == NULL)
		return SLURM_ERROR;

	rc = gres_plugin_init();

	slurm_mutex_lock(&gres_context_lock);
	step_gres_iter = list_iterator_create(step_gres_list);
	while ((step_gres_ptr = (gres_state_t *) list_next(step_gres_iter))) {
		job_gres_iter = list_iterator_create(job_gres_list);
		while ((job_gres_ptr = (gres_state_t *) 
				list_next(job_gres_iter))) {
			if (step_gres_ptr->plugin_id == job_gres_ptr->plugin_id)
				break;
		}
		list_iterator_destroy(job_gres_iter);
		if (job_gres_ptr == NULL)
			continue;

		for (i=0; i<gres_context_cnt; i++) {
			if (step_gres_ptr->plugin_id != 
			    *(gres_context[i].ops.plugin_id))
				continue;
			rc2 = _step_dealloc(step_gres_ptr->gres_data, 
					   job_gres_ptr->gres_data,
					   gres_context[i].ops.gres_name);
			if (rc2 != SLURM_SUCCESS)
				rc = rc2;
			break;
		}
	}
	list_iterator_destroy(step_gres_iter);
	slurm_mutex_unlock(&gres_context_lock);

	return rc;
}