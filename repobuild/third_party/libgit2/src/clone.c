/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include <assert.h>

#include "git2/clone.h"
#include "git2/remote.h"
#include "git2/revparse.h"
#include "git2/branch.h"
#include "git2/config.h"
#include "git2/checkout.h"
#include "git2/commit.h"
#include "git2/tree.h"

#include "common.h"
#include "remote.h"
#include "fileops.h"
#include "refs.h"
#include "path.h"
#include "repository.h"

static int create_branch(
	git_reference **branch,
	git_repository *repo,
	const git_oid *target,
	const char *name)
{
	git_commit *head_obj = NULL;
	git_reference *branch_ref = NULL;
	int error;

	/* Find the target commit */
	if ((error = git_commit_lookup(&head_obj, repo, target)) < 0)
		return error;

	/* Create the new branch */
	error = git_branch_create(&branch_ref, repo, name, head_obj, 0);

	git_commit_free(head_obj);

	if (!error)
		*branch = branch_ref;
	else
		git_reference_free(branch_ref);

	return error;
}

static int setup_tracking_config(
	git_repository *repo,
	const char *branch_name,
	const char *remote_name,
	const char *merge_target)
{
	git_config *cfg;
	git_buf remote_key = GIT_BUF_INIT, merge_key = GIT_BUF_INIT;
	int error = -1;

	if (git_repository_config__weakptr(&cfg, repo) < 0)
		return -1;

	if (git_buf_printf(&remote_key, "branch.%s.remote", branch_name) < 0)
		goto cleanup;

	if (git_buf_printf(&merge_key, "branch.%s.merge", branch_name) < 0)
		goto cleanup;

	if (git_config_set_string(cfg, git_buf_cstr(&remote_key), remote_name) < 0)
		goto cleanup;

	if (git_config_set_string(cfg, git_buf_cstr(&merge_key), merge_target) < 0)
		goto cleanup;

	error = 0;

cleanup:
	git_buf_free(&remote_key);
	git_buf_free(&merge_key);
	return error;
}

static int create_tracking_branch(
	git_reference **branch,
	git_repository *repo,
	const git_oid *target,
	const char *branch_name)
{
	int error;

	if ((error = create_branch(branch, repo, target, branch_name)) < 0)
		return error;

	return setup_tracking_config(
		repo,
		branch_name,
		GIT_REMOTE_ORIGIN,
		git_reference_name(*branch));
}

struct head_info {
	git_repository *repo;
	git_oid remote_head_oid;
	git_buf branchname;
	const git_refspec *refspec;
	bool found;
};

static int reference_matches_remote_head(
	const char *reference_name,
	void *payload)
{
	struct head_info *head_info = (struct head_info *)payload;
	git_oid oid;

	/* TODO: Should we guard against references
	 * which name doesn't start with refs/heads/ ?
	 */

	/* Stop looking if we've already found a match */
	if (head_info->found)
		return 0;

	if (git_reference_name_to_id(
		&oid,
		head_info->repo,
		reference_name) < 0) {
			/* If the reference doesn't exists, it obviously cannot match the expected oid. */
			giterr_clear();
			return 0;
	}

	if (git_oid__cmp(&head_info->remote_head_oid, &oid) == 0) {
		/* Determine the local reference name from the remote tracking one */
		if (git_refspec_transform_l(
			&head_info->branchname,
			head_info->refspec,
			reference_name) < 0)
				return -1;

		if (git_buf_len(&head_info->branchname) > 0) {
			if (git_buf_sets(
				&head_info->branchname,
				git_buf_cstr(&head_info->branchname) + strlen(GIT_REFS_HEADS_DIR)) < 0)
					return -1;

			head_info->found = 1;
		}
	}

	return 0;
}

static int update_head_to_new_branch(
	git_repository *repo,
	const git_oid *target,
	const char *name)
{
	git_reference *tracking_branch = NULL;
	int error;

	if ((error = create_tracking_branch(
		&tracking_branch,
		repo,
		target,
		name)) < 0)
			return error;

	error = git_repository_set_head(repo, git_reference_name(tracking_branch));

	git_reference_free(tracking_branch);

	return error;
}

static int get_head_callback(git_remote_head *head, void *payload)
{
	git_remote_head **destination = (git_remote_head **)payload;

	/* Save the first entry, and terminate the enumeration */
	*destination = head;
	return 1;
}

static int update_head_to_remote(git_repository *repo, git_remote *remote)
{
	int retcode = -1;
	git_refspec dummy_spec;
	git_remote_head *remote_head;
	struct head_info head_info;
	git_buf remote_master_name = GIT_BUF_INIT;

	/* Did we just clone an empty repository? */
	if (remote->refs.length == 0) {
		return setup_tracking_config(
			repo,
			"master",
			GIT_REMOTE_ORIGIN,
			GIT_REFS_HEADS_MASTER_FILE);
	}

	/* Get the remote's HEAD. This is always the first ref in remote->refs. */
	remote_head = NULL;

	if (!remote->transport->ls(remote->transport, get_head_callback, &remote_head))
		return -1;

	assert(remote_head);

	git_oid_cpy(&head_info.remote_head_oid, &remote_head->oid);
	git_buf_init(&head_info.branchname, 16);
	head_info.repo = repo;
	head_info.refspec = git_remote__matching_refspec(remote, GIT_REFS_HEADS_MASTER_FILE);
	head_info.found = 0;

	if (head_info.refspec == NULL) {
		memset(&dummy_spec, 0, sizeof(git_refspec));
		head_info.refspec = &dummy_spec;
	}

	/* Determine the remote tracking reference name from the local master */
	if (git_refspec_transform_r(
		&remote_master_name,
		head_info.refspec,
		GIT_REFS_HEADS_MASTER_FILE) < 0)
			return -1;

	/* Check to see if the remote HEAD points to the remote master */
	if (reference_matches_remote_head(git_buf_cstr(&remote_master_name), &head_info) < 0)
		goto cleanup;

	if (head_info.found) {
		retcode = update_head_to_new_branch(
			repo,
			&head_info.remote_head_oid,
			git_buf_cstr(&head_info.branchname));

		goto cleanup;
	}

	/* Not master. Check all the other refs. */
	if (git_reference_foreach_name(
		repo,
		reference_matches_remote_head,
		&head_info) < 0)
			goto cleanup;

	if (head_info.found) {
		retcode = update_head_to_new_branch(
			repo,
			&head_info.remote_head_oid,
			git_buf_cstr(&head_info.branchname));

		goto cleanup;
	} else {
		retcode = git_repository_set_head_detached(
			repo,
			&head_info.remote_head_oid);
		goto cleanup;
	}

cleanup:
	git_buf_free(&remote_master_name);
	git_buf_free(&head_info.branchname);
	return retcode;
}

static int update_head_to_branch(
		git_repository *repo,
		const char *remote_name,
		const char *branch)
{
	int retcode;
	git_buf remote_branch_name = GIT_BUF_INIT;
	git_reference* remote_ref = NULL;

	assert(remote_name && branch);

	if ((retcode = git_buf_printf(&remote_branch_name, GIT_REFS_REMOTES_DIR "%s/%s",
		remote_name, branch)) < 0 )
		goto cleanup;

	if ((retcode = git_reference_lookup(&remote_ref, repo, git_buf_cstr(&remote_branch_name))) < 0)
		goto cleanup;

	retcode = update_head_to_new_branch(repo, git_reference_target(remote_ref), branch);

cleanup:
	git_reference_free(remote_ref);
	git_buf_free(&remote_branch_name);
	return retcode;
}

/*
 * submodules?
 */

static int create_and_configure_origin(
		git_remote **out,
		git_repository *repo,
		const char *url,
		const git_clone_options *options)
{
	int error;
	git_remote *origin = NULL;
	const char *name;

	name = options->remote_name ? options->remote_name : "origin";
	if ((error = git_remote_create(&origin, repo, name, url)) < 0)
		goto on_error;

	if (options->ignore_cert_errors)
		git_remote_check_cert(origin, 0);

	if ((error = git_remote_set_callbacks(origin, &options->remote_callbacks)) < 0)
		goto on_error;

	if ((error = git_remote_save(origin)) < 0)
		goto on_error;

	*out = origin;
	return 0;

on_error:
	git_remote_free(origin);
	return error;
}

static bool should_checkout(
	git_repository *repo,
	bool is_bare,
	const git_checkout_opts *opts)
{
	if (is_bare)
		return false;

	if (!opts)
		return false;

	if (opts->checkout_strategy == GIT_CHECKOUT_NONE)
		return false;

	return !git_repository_head_unborn(repo);
}

int git_clone_into(git_repository *repo, git_remote *remote, const git_checkout_opts *co_opts, const char *branch)
{
	int error = 0, old_fetchhead;
	size_t nspecs;

	assert(repo && remote);

	if (!git_repository_is_empty(repo)) {
		giterr_set(GITERR_INVALID, "the repository is not empty");
		return -1;
	}

	if ((error = git_remote_add_fetch(remote, "refs/tags/*:refs/tags/*")) < 0)
		return error;

	old_fetchhead = git_remote_update_fetchhead(remote);
	git_remote_set_update_fetchhead(remote, 0);

	if ((error = git_remote_fetch(remote)) < 0)
		goto cleanup;

	if (branch)
		error = update_head_to_branch(repo, git_remote_name(remote), branch);
	/* Point HEAD to the same ref as the remote's head */
	else
		error = update_head_to_remote(repo, remote);

	if (!error && should_checkout(repo, git_repository_is_bare(repo), co_opts))
		error = git_checkout_head(repo, co_opts);

cleanup:
	git_remote_set_update_fetchhead(remote, old_fetchhead);
	/* Remove the tags refspec */
	nspecs = git_remote_refspec_count(remote);
	git_remote_remove_refspec(remote, nspecs);

	return error;
}

int git_clone(
	git_repository **out,
	const char *url,
	const char *local_path,
	const git_clone_options *_options)
{
	int retcode = GIT_ERROR;
	git_repository *repo = NULL;
	git_remote *origin;
	git_clone_options options = GIT_CLONE_OPTIONS_INIT;
	int remove_directory_on_failure = 0;

	assert(out && url && local_path);

	if (_options)
		memcpy(&options, _options, sizeof(git_clone_options));

	GITERR_CHECK_VERSION(&options, GIT_CLONE_OPTIONS_VERSION, "git_clone_options");

	/* Only clone to a new directory or an empty directory */
	if (git_path_exists(local_path) && !git_path_is_empty_dir(local_path)) {
		giterr_set(GITERR_INVALID,
			"'%s' exists and is not an empty directory", local_path);
		return GIT_ERROR;
	}

	/* Only remove the directory on failure if we create it */
	remove_directory_on_failure = !git_path_exists(local_path);

	if ((retcode = git_repository_init(&repo, local_path, options.bare)) < 0)
		return retcode;

	if ((retcode = create_and_configure_origin(&origin, repo, url, &options)) < 0)
		goto cleanup;

	retcode = git_clone_into(repo, origin, &options.checkout_opts, options.checkout_branch);
	git_remote_free(origin);

	if (retcode < 0)
		goto cleanup;

	*out = repo;
	return 0;

cleanup:
	git_repository_free(repo);
	if (remove_directory_on_failure)
		git_futils_rmdir_r(local_path, NULL, GIT_RMDIR_REMOVE_FILES);
	else
		git_futils_cleanupdir_r(local_path);

	return retcode;
}