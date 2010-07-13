/*****************************************************************************\
 *  acct_policy.c - Enforce accounting policy
 *****************************************************************************
 *  Copyright (C) 2008 Lawrence Livermore National Security.
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <slurm/slurm_errno.h>

#include "src/common/assoc_mgr.h"
#include "src/common/slurm_accounting_storage.h"

#include "src/slurmctld/slurmctld.h"

#define _DEBUG 0

enum {
	ACCT_POLICY_ADD_SUBMIT,
	ACCT_POLICY_REM_SUBMIT,
	ACCT_POLICY_JOB_BEGIN,
	ACCT_POLICY_JOB_FINI
};

static void _cancel_job(struct job_record *job_ptr)
{
	time_t now = time(NULL);

	last_job_update = now;
	job_ptr->job_state = JOB_FAILED;
	job_ptr->exit_code = 1;
	job_ptr->state_reason = FAIL_BANK_ACCOUNT;
	xfree(job_ptr->state_desc);
	job_ptr->start_time = job_ptr->end_time = now;
	job_completion_logger(job_ptr, false);
	delete_job_details(job_ptr);
}

static bool _valid_job_assoc(struct job_record *job_ptr)
{
	slurmdb_association_rec_t assoc_rec, *assoc_ptr;

	assoc_ptr = (slurmdb_association_rec_t *)job_ptr->assoc_ptr;
	if ((assoc_ptr == NULL) ||
	    (assoc_ptr->id  != job_ptr->assoc_id) ||
	    (assoc_ptr->uid != job_ptr->user_id)) {
		error("Invalid assoc_ptr for jobid=%u", job_ptr->job_id);
		memset(&assoc_rec, 0, sizeof(slurmdb_association_rec_t));
		if(job_ptr->assoc_id)
			assoc_rec.id = job_ptr->assoc_id;
		else {
			assoc_rec.uid       = job_ptr->user_id;
			assoc_rec.partition = job_ptr->partition;
			assoc_rec.acct      = job_ptr->account;
		}
		if (assoc_mgr_fill_in_assoc(acct_db_conn, &assoc_rec,
					    accounting_enforce,
					    (slurmdb_association_rec_t **)
					    &job_ptr->assoc_ptr)) {
			info("_validate_job_assoc: invalid account or "
			     "partition for uid=%u jobid=%u",
			     job_ptr->user_id, job_ptr->job_id);
			return false;
		}
		job_ptr->assoc_id = assoc_rec.id;
	}
	return true;
}

static void _adjust_limit_usage(int type, struct job_record *job_ptr)
{
	slurmdb_association_rec_t *assoc_ptr = NULL;
	assoc_mgr_lock_t locks = { WRITE_LOCK, NO_LOCK,
				   WRITE_LOCK, NO_LOCK, NO_LOCK };

	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS)
	    || !_valid_job_assoc(job_ptr))
		return;

	assoc_mgr_lock(&locks);
	if (job_ptr->qos_ptr && (accounting_enforce & ACCOUNTING_ENFORCE_QOS)) {
		ListIterator itr = NULL;
		slurmdb_qos_rec_t *qos_ptr = NULL;
		slurmdb_used_limits_t *used_limits = NULL;

		qos_ptr = (slurmdb_qos_rec_t *)job_ptr->qos_ptr;
		if(!qos_ptr->usage->user_limit_list)
			qos_ptr->usage->user_limit_list =
				list_create(slurmdb_destroy_used_limits);
		itr = list_iterator_create(qos_ptr->usage->user_limit_list);
		while((used_limits = list_next(itr))) {
			if(used_limits->uid == job_ptr->user_id)
				break;
		}
		list_iterator_destroy(itr);
		if(!used_limits) {
			used_limits = xmalloc(sizeof(slurmdb_used_limits_t));
			used_limits->uid = job_ptr->user_id;
			list_append(qos_ptr->usage->user_limit_list,
				    used_limits);
		}
		switch(type) {
		case ACCT_POLICY_ADD_SUBMIT:
			qos_ptr->usage->grp_used_submit_jobs++;
			used_limits->submit_jobs++;
			break;
		case ACCT_POLICY_REM_SUBMIT:
			if(qos_ptr->usage->grp_used_submit_jobs)
				qos_ptr->usage->grp_used_submit_jobs--;
			else
				debug2("acct_policy_remove_job_submit: "
				       "grp_submit_jobs underflow for qos %s",
				       qos_ptr->name);

			if(used_limits->submit_jobs)
				used_limits->submit_jobs--;
			else
				debug2("acct_policy_remove_job_submit: "
				       "used_submit_jobs underflow for "
				       "qos %s user %d",
				       qos_ptr->name, used_limits->uid);
			break;
		case ACCT_POLICY_JOB_BEGIN:
			qos_ptr->usage->grp_used_jobs++;
			qos_ptr->usage->grp_used_cpus += job_ptr->total_cpus;
			qos_ptr->usage->grp_used_nodes += job_ptr->node_cnt;
			used_limits->jobs++;
			break;
		case ACCT_POLICY_JOB_FINI:
			if(qos_ptr->usage->grp_used_jobs)
				qos_ptr->usage->grp_used_jobs--;
			else
				debug2("acct_policy_job_fini: used_jobs "
				       "underflow for qos %s", qos_ptr->name);

			qos_ptr->usage->grp_used_cpus -= job_ptr->total_cpus;
			if((int32_t)qos_ptr->usage->grp_used_cpus < 0) {
				qos_ptr->usage->grp_used_cpus = 0;
				debug2("acct_policy_job_fini: grp_used_cpus "
				       "underflow for qos %s", qos_ptr->name);
			}

			qos_ptr->usage->grp_used_nodes -= job_ptr->node_cnt;
			if((int32_t)qos_ptr->usage->grp_used_nodes < 0) {
				qos_ptr->usage->grp_used_nodes = 0;
				debug2("acct_policy_job_fini: grp_used_nodes "
				       "underflow for qos %s", qos_ptr->name);
			}

			if(used_limits->jobs)
				used_limits->jobs--;
			else
				debug2("acct_policy_job_fini: used_jobs "
				       "underflow for qos %s user %d",
				       qos_ptr->name, used_limits->uid);
			break;
		default:
			error("acct_policy: qos unknown type %d", type);
			break;
		}
	}

	assoc_ptr = (slurmdb_association_rec_t *)job_ptr->assoc_ptr;
	while(assoc_ptr) {
		switch(type) {
		case ACCT_POLICY_ADD_SUBMIT:
			assoc_ptr->usage->used_submit_jobs++;
			break;
		case ACCT_POLICY_REM_SUBMIT:
			if (assoc_ptr->usage->used_submit_jobs)
				assoc_ptr->usage->used_submit_jobs--;
			else
				debug2("acct_policy_remove_job_submit: "
				       "used_submit_jobs underflow for "
				       "account %s",
				       assoc_ptr->acct);
			break;
		case ACCT_POLICY_JOB_BEGIN:
			assoc_ptr->usage->used_jobs++;
			assoc_ptr->usage->grp_used_cpus += job_ptr->total_cpus;
			assoc_ptr->usage->grp_used_nodes += job_ptr->node_cnt;
			break;
		case ACCT_POLICY_JOB_FINI:
			if (assoc_ptr->usage->used_jobs)
				assoc_ptr->usage->used_jobs--;
			else
				debug2("acct_policy_job_fini: used_jobs "
				       "underflow for account %s",
				       assoc_ptr->acct);

			assoc_ptr->usage->grp_used_cpus -= job_ptr->total_cpus;
			if ((int32_t)assoc_ptr->usage->grp_used_cpus < 0) {
				assoc_ptr->usage->grp_used_cpus = 0;
				debug2("acct_policy_job_fini: grp_used_cpus "
				       "underflow for account %s",
				       assoc_ptr->acct);
			}

			assoc_ptr->usage->grp_used_nodes -= job_ptr->node_cnt;
			if ((int32_t)assoc_ptr->usage->grp_used_nodes < 0) {
				assoc_ptr->usage->grp_used_nodes = 0;
				debug2("acct_policy_job_fini: grp_used_nodes "
				       "underflow for account %s",
				       assoc_ptr->acct);
			}
			break;
		default:
			error("acct_policy: association unknown type %d", type);
			break;
		}
		/* now handle all the group limits of the parents */
		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
	}
	assoc_mgr_unlock(&locks);
}

/*
 * acct_policy_add_job_submit - Note that a job has been submitted for
 *	accounting policy purposes.
 */
extern void acct_policy_add_job_submit(struct job_record *job_ptr)
{
	_adjust_limit_usage(ACCT_POLICY_ADD_SUBMIT, job_ptr);
}

/*
 * acct_policy_remove_job_submit - Note that a job has finished (might
 *      not had started or been allocated resources) for accounting
 *      policy purposes.
 */
extern void acct_policy_remove_job_submit(struct job_record *job_ptr)
{
	_adjust_limit_usage(ACCT_POLICY_REM_SUBMIT, job_ptr);
}

/*
 * acct_policy_job_begin - Note that a job is starting for accounting
 *	policy purposes.
 */
extern void acct_policy_job_begin(struct job_record *job_ptr)
{
	_adjust_limit_usage(ACCT_POLICY_JOB_BEGIN, job_ptr);
}

/*
 * acct_policy_job_fini - Note that a job is completing for accounting
 *	policy purposes.
 */
extern void acct_policy_job_fini(struct job_record *job_ptr)
{
	_adjust_limit_usage(ACCT_POLICY_JOB_FINI, job_ptr);
}

/*
 * acct_policy_job_runnable - Determine of the specified job can execute
 *	right now or not depending upon accounting policy (e.g. running
 *	job limit for this association). If the association limits prevent
 *	the job from ever running (lowered limits since job submission),
 *	then cancel the job.
 */
extern bool acct_policy_job_runnable(struct job_record *job_ptr)
{
	slurmdb_qos_rec_t *qos_ptr;
	slurmdb_association_rec_t *assoc_ptr;
	uint32_t time_limit;
	uint64_t cpu_time_limit;
	uint64_t job_cpu_time_limit;
	bool rc = true;
	uint64_t usage_mins;
	uint32_t wall_mins;
	bool cancel_job = 0;
	int parent = 0; /*flag to tell us if we are looking at the
			 * parent or not
			 */
	assoc_mgr_lock_t locks = { READ_LOCK, NO_LOCK,
				   READ_LOCK, NO_LOCK, NO_LOCK };

	/* check to see if we are enforcing associations */
	if (!accounting_enforce)
		return true;

	if (!_valid_job_assoc(job_ptr)) {
		_cancel_job(job_ptr);
		return false;
	}

	/* now see if we are enforcing limits */
	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS))
		return true;

	/* clear old state reason */
        if ((job_ptr->state_reason == WAIT_ASSOC_JOB_LIMIT) ||
	    (job_ptr->state_reason == WAIT_ASSOC_RESOURCE_LIMIT) ||
	    (job_ptr->state_reason == WAIT_ASSOC_TIME_LIMIT))
                job_ptr->state_reason = WAIT_NO_REASON;


	job_cpu_time_limit = (uint64_t)job_ptr->time_limit
		* (uint64_t)job_ptr->details->min_cpus;

	assoc_mgr_lock(&locks);
	qos_ptr = job_ptr->qos_ptr;
	if(qos_ptr) {
		usage_mins = (uint64_t)(qos_ptr->usage->usage_raw / 60.0);
		wall_mins = qos_ptr->usage->grp_used_wall / 60;

		if ((qos_ptr->grp_cpu_mins != (uint64_t)INFINITE)
		    && (usage_mins >= qos_ptr->grp_cpu_mins)) {
			job_ptr->state_reason = WAIT_ASSOC_JOB_LIMIT;
			xfree(job_ptr->state_desc);
			debug2("Job %u being held, "
			       "the job is at or exceeds QOS %s's "
			       "group max cpu minutes of %llu with %llu",
			       job_ptr->job_id,
			       qos_ptr->name, qos_ptr->grp_cpu_mins,
			       usage_mins);
			rc = false;
			goto end_it;
		}

		if (qos_ptr->grp_cpus != INFINITE) {
			if (job_ptr->details->min_cpus > qos_ptr->grp_cpus) {
				info("job %u is being cancelled, "
				     "min cpu request %u exceeds "
				     "group max cpu limit %u for qos '%s'",
				     job_ptr->job_id,
				     job_ptr->details->min_cpus,
				     qos_ptr->grp_cpus,
				     qos_ptr->name);
				cancel_job = 1;
				rc = false;
				goto end_it;
			} else if ((qos_ptr->usage->grp_used_cpus +
				    job_ptr->details->min_cpus) >
				   qos_ptr->grp_cpus) {
				job_ptr->state_reason =
					WAIT_ASSOC_RESOURCE_LIMIT;
				xfree(job_ptr->state_desc);
				debug2("job %u being held, "
				       "the job is at or exceeds "
				       "group max cpu limit %u "
				       "with already used %u + requested %u "
				       "for qos %s",
				       job_ptr->job_id,
				       qos_ptr->grp_cpus,
				       qos_ptr->usage->grp_used_cpus,
				       job_ptr->details->min_cpus,
				       qos_ptr->name);
				rc = false;
				goto end_it;
			}
		}

		if ((qos_ptr->grp_jobs != INFINITE) &&
		    (qos_ptr->usage->grp_used_jobs >= qos_ptr->grp_jobs)) {
			job_ptr->state_reason = WAIT_ASSOC_JOB_LIMIT;
			xfree(job_ptr->state_desc);
			debug2("job %u being held, "
			       "the job is at or exceeds QOS %s's "
			       "group max jobs limit %u with %u for qos %s",
			       job_ptr->job_id,
			       qos_ptr->grp_jobs,
			       qos_ptr->usage->grp_used_jobs, qos_ptr->name);

			rc = false;
			goto end_it;
		}

		if (qos_ptr->grp_nodes != INFINITE) {
			if (job_ptr->details->min_nodes > qos_ptr->grp_nodes) {
				info("job %u is being cancelled, "
				     "min node request %u exceeds "
				     "group max node limit %u for qos '%s'",
				     job_ptr->job_id,
				     job_ptr->details->min_nodes,
				     qos_ptr->grp_nodes,
				     qos_ptr->name);
				cancel_job = 1;
				rc = false;
				goto end_it;
			} else if ((qos_ptr->usage->grp_used_nodes +
				    job_ptr->details->min_nodes) >
				   qos_ptr->grp_nodes) {
				job_ptr->state_reason =
					WAIT_ASSOC_RESOURCE_LIMIT;
				xfree(job_ptr->state_desc);
				debug2("job %u being held, "
				       "the job is at or exceeds "
				       "group max node limit %u "
				       "with already used %u + requested %u "
				       "for qos %s",
				       job_ptr->job_id,
				       qos_ptr->grp_nodes,
				       qos_ptr->usage->grp_used_nodes,
				       job_ptr->details->min_nodes,
				       qos_ptr->name);
				rc = false;
				goto end_it;
			}
		}

		/* we don't need to check submit_jobs here */

		if ((qos_ptr->grp_wall != INFINITE)
		    && (wall_mins >= qos_ptr->grp_wall)) {
			job_ptr->state_reason = WAIT_ASSOC_JOB_LIMIT;
			xfree(job_ptr->state_desc);
			debug2("job %u being held, "
			       "the job is at or exceeds "
			       "group wall limit %u "
			       "with %u for qos %s",
			       job_ptr->job_id,
			       qos_ptr->grp_wall,
			       wall_mins, qos_ptr->name);

			rc = false;
			goto end_it;
		}

		if (qos_ptr->max_cpu_mins_pj != INFINITE) {
			cpu_time_limit = qos_ptr->max_cpu_mins_pj;
			if ((job_ptr->time_limit != NO_VAL) &&
			    (job_cpu_time_limit > cpu_time_limit)) {
				info("job %u being cancelled, "
				     "cpu time limit %u exceeds "
				     "qos max per job %u",
				     job_ptr->job_id, job_cpu_time_limit,
				     cpu_time_limit);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}

		if (qos_ptr->max_cpus_pj != INFINITE) {
			if (job_ptr->details->min_cpus >
			    qos_ptr->max_cpus_pj) {
				info("job %u being cancelled, "
				     "min cpu limit %u exceeds "
				     "qos max %u",
				     job_ptr->job_id,
				     job_ptr->details->min_cpus,
				     qos_ptr->max_cpus_pj);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}

		if (qos_ptr->max_jobs_pu != INFINITE) {
			slurmdb_used_limits_t *used_limits = NULL;
			if(qos_ptr->usage->user_limit_list) {
				ListIterator itr = list_iterator_create(
					qos_ptr->usage->user_limit_list);
				while((used_limits = list_next(itr))) {
					if(used_limits->uid == job_ptr->user_id)
						break;
				}
				list_iterator_destroy(itr);
			}
			if(used_limits && (used_limits->jobs
					   >= qos_ptr->max_jobs_pu)) {
				debug2("job %u being held, "
				       "the job is at or exceeds "
				       "max jobs limit %u with %u for QOS %s",
				       job_ptr->job_id,
				       qos_ptr->max_jobs_pu,
				       used_limits->jobs, qos_ptr->name);
				rc = false;
				goto end_it;
			}
		}

		if (qos_ptr->max_nodes_pj != INFINITE) {
			if (job_ptr->details->min_nodes >
			    qos_ptr->max_nodes_pj) {
				info("job %u being cancelled, "
				     "min node limit %u exceeds "
				     "qos max %u",
				     job_ptr->job_id,
				     job_ptr->details->min_nodes,
				     qos_ptr->max_nodes_pj);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}

		/* we don't need to check submit_jobs_pu here */

		/* if the qos limits have changed since job
		 * submission and job can not run, then kill it */
		if (qos_ptr->max_wall_pj != INFINITE) {
			time_limit = qos_ptr->max_wall_pj;
			if ((job_ptr->time_limit != NO_VAL) &&
			    (job_ptr->time_limit > time_limit)) {
				info("job %u being cancelled, "
				     "time limit %u exceeds qos max wall pj %u",
				     job_ptr->job_id, job_ptr->time_limit,
				     time_limit);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}
	}

	assoc_ptr = job_ptr->assoc_ptr;
	while(assoc_ptr) {
		usage_mins = (uint64_t)(assoc_ptr->usage->usage_raw / 60.0);
		wall_mins = assoc_ptr->usage->grp_used_wall / 60;
#if _DEBUG
		info("acct_job_limits: %u of %u",
		     assoc_ptr->usage->used_jobs, assoc_ptr->max_jobs);
#endif
		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->grp_cpu_mins == (uint64_t)INFINITE))
		    && (assoc_ptr->grp_cpu_mins != (uint64_t)INFINITE)
		    && (usage_mins >= assoc_ptr->grp_cpu_mins)) {
			job_ptr->state_reason = WAIT_ASSOC_JOB_LIMIT;
			xfree(job_ptr->state_desc);
			debug2("job %u being held, "
			       "assoc %u is at or exceeds "
			       "group max cpu minutes limit %llu "
			       "with %Lf for account %s",
			       job_ptr->job_id, assoc_ptr->id,
			       assoc_ptr->grp_cpu_mins,
			       assoc_ptr->usage->usage_raw, assoc_ptr->acct);

			rc = false;
			goto end_it;
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->grp_cpus == INFINITE))
		    && (assoc_ptr->grp_cpus != INFINITE)) {
			if (job_ptr->details->min_cpus >
			    assoc_ptr->grp_cpus) {
				info("job %u being cancelled, "
				     "min cpu request %u exceeds "
				     "group max cpu limit %u for account %s",
				     job_ptr->job_id,
				     job_ptr->details->min_cpus,
				     assoc_ptr->grp_cpus, assoc_ptr->acct);
				cancel_job = 1;
				rc = false;
				goto end_it;
			} else if ((assoc_ptr->usage->grp_used_cpus +
				    job_ptr->details->min_cpus) >
				   assoc_ptr->grp_cpus) {
				job_ptr->state_reason =
					WAIT_ASSOC_RESOURCE_LIMIT;
				xfree(job_ptr->state_desc);
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group max cpu limit %u "
				       "with already used %u + requested %u "
				       "for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       assoc_ptr->grp_cpus,
				       assoc_ptr->usage->grp_used_cpus,
				       job_ptr->details->min_cpus,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->grp_jobs == INFINITE)) &&
		    (assoc_ptr->grp_jobs != INFINITE) &&
		    (assoc_ptr->usage->used_jobs >= assoc_ptr->grp_jobs)) {
			job_ptr->state_reason = WAIT_ASSOC_JOB_LIMIT;
			xfree(job_ptr->state_desc);
			debug2("job %u being held, "
			       "assoc %u is at or exceeds "
			       "group max jobs limit %u with %u for account %s",
			       job_ptr->job_id, assoc_ptr->id,
			       assoc_ptr->grp_jobs,
			       assoc_ptr->usage->used_jobs, assoc_ptr->acct);

			rc = false;
			goto end_it;
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->grp_nodes == INFINITE))
		    && (assoc_ptr->grp_nodes != INFINITE)) {
			if (job_ptr->details->min_nodes >
			    assoc_ptr->grp_nodes) {
				info("job %u being cancelled, "
				     "min node request %u exceeds "
				     "group max node limit %u for account %s",
				     job_ptr->job_id,
				     job_ptr->details->min_nodes,
				     assoc_ptr->grp_nodes, assoc_ptr->acct);
				cancel_job = 1;
				rc = false;
				goto end_it;
			} else if ((assoc_ptr->usage->grp_used_nodes +
				    job_ptr->details->min_nodes) >
				   assoc_ptr->grp_nodes) {
				job_ptr->state_reason =
					WAIT_ASSOC_RESOURCE_LIMIT;
				xfree(job_ptr->state_desc);
				debug2("job %u being held, "
				       "assoc %u is at or exceeds "
				       "group max node limit %u "
				       "with already used %u + requested %u "
				       "for account %s",
				       job_ptr->job_id, assoc_ptr->id,
				       assoc_ptr->grp_nodes,
				       assoc_ptr->usage->grp_used_nodes,
				       job_ptr->details->min_nodes,
				       assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		/* we don't need to check submit_jobs here */

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->grp_wall == INFINITE))
		    && (assoc_ptr->grp_wall != INFINITE)
		    && (wall_mins >= assoc_ptr->grp_wall)) {
			job_ptr->state_reason = WAIT_ASSOC_JOB_LIMIT;
			xfree(job_ptr->state_desc);
			debug2("job %u being held, "
			       "assoc %u is at or exceeds "
			       "group wall limit %u "
			       "with %u for account %s",
			       job_ptr->job_id, assoc_ptr->id,
			       assoc_ptr->grp_wall,
			       wall_mins, assoc_ptr->acct);

			rc = false;
			goto end_it;
		}


		/* We don't need to look at the regular limits for
		 * parents since we have pre-propogated them, so just
		 * continue with the next parent
		 */
		if(parent) {
			assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
			continue;
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->max_cpu_mins_pj == INFINITE)) &&
		    (assoc_ptr->max_cpu_mins_pj != INFINITE)) {
			cpu_time_limit = assoc_ptr->max_cpu_mins_pj;
			if ((job_ptr->time_limit != NO_VAL) &&
			    (job_cpu_time_limit > cpu_time_limit)) {
				info("job %u being cancelled, "
				     "cpu time limit %u exceeds "
				     "assoc max per job %u",
				     job_ptr->job_id, job_cpu_time_limit,
				     cpu_time_limit);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->max_cpus_pj == INFINITE)) &&
		    (assoc_ptr->max_cpus_pj != INFINITE)) {
			if (job_ptr->details->min_cpus >
			    assoc_ptr->max_cpus_pj) {
				info("job %u being cancelled, "
				     "min cpu limit %u exceeds "
				     "account max %u",
				     job_ptr->job_id,
				     job_ptr->details->min_cpus,
				     assoc_ptr->max_cpus_pj);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->max_jobs_pu == INFINITE)) &&
		    (assoc_ptr->max_jobs != INFINITE) &&
		    (assoc_ptr->usage->used_jobs >= assoc_ptr->max_jobs)) {
			job_ptr->state_reason = WAIT_ASSOC_JOB_LIMIT;
			xfree(job_ptr->state_desc);
			debug2("job %u being held, "
			       "assoc %u is at or exceeds "
			       "max jobs limit %u with %u for account %s",
			       job_ptr->job_id, assoc_ptr->id,
			       assoc_ptr->max_jobs,
			       assoc_ptr->usage->used_jobs, assoc_ptr->acct);
			rc = false;
			goto end_it;
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->max_nodes_pj == INFINITE))
		    && (assoc_ptr->max_nodes_pj != INFINITE)) {
			if (job_ptr->details->min_nodes >
			    assoc_ptr->max_nodes_pj) {
				info("job %u being cancelled, "
				     "min node limit %u exceeds "
				     "account max %u",
				     job_ptr->job_id,
				     job_ptr->details->min_nodes,
				     assoc_ptr->max_nodes_pj);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}

		/* we don't need to check submit_jobs here */

		/* if the association limits have changed since job
		 * submission and job can not run, then kill it */
		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->max_wall_pj == INFINITE))
		    && (assoc_ptr->max_wall_pj != INFINITE)) {
			time_limit = assoc_ptr->max_wall_pj;
			if ((job_ptr->time_limit != NO_VAL) &&
			    (job_ptr->time_limit > time_limit)) {
				info("job %u being cancelled, "
				     "time limit %u exceeds account max %u",
				     job_ptr->job_id, job_ptr->time_limit,
				     time_limit);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}

		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
		parent = 1;
	}
end_it:
	assoc_mgr_unlock(&locks);

	if(cancel_job)
		_cancel_job(job_ptr);

	return rc;
}

extern bool acct_policy_node_usable(struct job_record *job_ptr,
				    uint32_t used_cpus,
				    char *node_name, uint32_t node_cpus)
{
	slurmdb_qos_rec_t *qos_ptr;
	slurmdb_association_rec_t *assoc_ptr;
	bool rc = true;
	uint32_t total_cpus = used_cpus + node_cpus;
	bool cancel_job = 0;
	int parent = 0; /* flag to tell us if we are looking at the
			 * parent or not
			 */
	assoc_mgr_lock_t locks = { READ_LOCK, NO_LOCK,
				   READ_LOCK, NO_LOCK, NO_LOCK };

	/* check to see if we are enforcing associations */
	if (!accounting_enforce)
		return true;

	if (!_valid_job_assoc(job_ptr)) {
		_cancel_job(job_ptr);
		return false;
	}

	/* now see if we are enforcing limits */
	if (!(accounting_enforce & ACCOUNTING_ENFORCE_LIMITS))
		return true;

	/* clear old state reason */
        if ((job_ptr->state_reason == WAIT_ASSOC_JOB_LIMIT) ||
	    (job_ptr->state_reason == WAIT_ASSOC_RESOURCE_LIMIT) ||
	    (job_ptr->state_reason == WAIT_ASSOC_TIME_LIMIT))
                job_ptr->state_reason = WAIT_NO_REASON;


	assoc_mgr_lock(&locks);
	qos_ptr = job_ptr->qos_ptr;
	if(qos_ptr) {
		if (qos_ptr->grp_cpus != INFINITE) {
			if ((total_cpus+qos_ptr->usage->grp_used_cpus)
			    > qos_ptr->grp_cpus) {
				debug("Can't use %s, adding it's %u cpus "
				      "exceeds "
				      "group max cpu limit %u for qos '%s'",
				     node_name,
				     node_cpus,
				     qos_ptr->grp_cpus,
				     qos_ptr->name);
				rc = false;
				goto end_it;
			}
		}

		if (qos_ptr->max_cpus_pj != INFINITE) {
			if (total_cpus > qos_ptr->max_cpus_pj) {
				debug("Can't use %s, adding it's %u cpus "
				      "exceeds "
				      "max cpu limit %u for qos '%s'",
				      node_name,
				      node_cpus,
				      qos_ptr->max_cpus_pj,
				      qos_ptr->name);
				cancel_job = 1;
				rc = false;
				goto end_it;
			}
		}
	}

	assoc_ptr = job_ptr->assoc_ptr;
	while(assoc_ptr) {
		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->grp_cpus == INFINITE))
		    && (assoc_ptr->grp_cpus != INFINITE)) {
			if ((total_cpus+assoc_ptr->usage->grp_used_cpus)
			    > assoc_ptr->grp_cpus) {
				debug("Can't use %s, adding it's %u cpus "
				      "exceeds "
				      "group max cpu limit %u for account '%s'",
				      node_name,
				      node_cpus,
				      assoc_ptr->grp_cpus,
				      assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}

		/* We don't need to look at the regular limits for
		 * parents since we have pre-propogated them, so just
		 * continue with the next parent
		 */
		if(parent) {
			assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
			continue;
		}

		if ((!qos_ptr ||
		     (qos_ptr && qos_ptr->max_cpus_pj == INFINITE)) &&
		    (assoc_ptr->max_cpus_pj != INFINITE)) {
			if (job_ptr->details->min_cpus >
			    assoc_ptr->max_cpus_pj) {
				debug("Can't use %s, adding it's %u cpus "
				      "exceeds "
				      "max cpu limit %u for account '%s'",
				      node_name,
				      node_cpus,
				      assoc_ptr->max_cpus_pj,
				      assoc_ptr->acct);
				rc = false;
				goto end_it;
			}
		}
		assoc_ptr = assoc_ptr->usage->parent_assoc_ptr;
		parent = 1;
	}
end_it:
	assoc_mgr_unlock(&locks);

	if(cancel_job)
		_cancel_job(job_ptr);

	return rc;
}