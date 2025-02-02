/*
   Copyright (c) 2021 iXsystems, Inc <https://www.ixsystems.com>
   This file is part of GlusterFS.

   This file is licensed to you under your choice of the GNU Lesser
   General Public License, version 3 or any later version (LGPLv3 or
   later), or the GNU General Public License, version 2 (GPLv2), in all
   cases as published by the Free Software Foundation.
*/
#ifdef GF_WITH_GENERIC_SNAPSHOT

#include <inttypes.h>
#include <sys/types.h>
#include <unistd.h>

#include "glusterd-messages.h"

#include "glusterd-utils.h"
#include "glusterd-snapshot-utils.h"

#include <glusterfs/dict.h>
#include <glusterfs/run.h>

#if defined(GF_LINUX_HOST_OS)
#include <mntent.h>
#else
#include "mntent_compat.h"
#endif

#if !defined(GF_GENERIC_SNAPSHOT_COMMAND)
#define GF_GENERIC_SNAPSHOT_COMMAND /usr/libexec/glusterfs/generic-snapshot.sh
#endif

extern char snap_mount_dir[VALID_GLUSTERD_PATHMAX];

typedef struct {
    char *mnt_type;
    char *brick_path;
    char *mnt_pt;
} glusterd_generic_metadata_t;

static gf_boolean_t _get_snap_id(char *snap_id, char *snap_volume_id, int32_t brick_num) {
    int len;

    len = snprintf(snap_id, NAME_MAX, "%s_%d", snap_volume_id, brick_num);
    if ((len < 0) || (len >= NAME_MAX)) {
        return _gf_false;
    } else {
        return _gf_true;
    }
}

static gf_boolean_t _get_metadata(glusterd_generic_metadata_t *meta, char *brick_path)
{
    xlator_t *this = THIS;
    char *mnt_pt = NULL;
    char buff[PATH_MAX] = "";
    struct mntent *entry = NULL;
    struct mntent save_entry = {
        0,
    };

    if (glusterd_get_brick_root(brick_path, &mnt_pt)) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_BRICKPATH_ROOT_GET_FAIL,
               "getting the root "
               "of the brick (%s) failed ",
               brick_path);
        return _gf_false;
    }

    char *dir = mnt_pt;
    do {
        entry = glusterd_get_mnt_entry_info(dir, buff, sizeof(buff),
                                            &save_entry);
        if (entry) {
            break;
        }
        // path may be a subdirectory of a mount -- ascend until we get the file system info
        int len = strlen(dir);
        dir = dirname(dir);
        if (len <= 1 || len >= strlen(dir)) {
            gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_MNTENTRY_GET_FAIL,
                   "getting the mount entry for "
                   "the brick (%s) failed",
                   brick_path);
            return _gf_false;
        }
    } while (true);

    meta->mnt_type = entry->mnt_type;
    meta->brick_path = gf_strdup(brick_path);
    meta->mnt_pt = gf_strdup(mnt_pt);
    GF_FREE(mnt_pt);

    return _gf_true;
}

static void _free_metadata(glusterd_generic_metadata_t *meta) {
    if (meta) {
        GF_FREE(meta->brick_path);
        GF_FREE(meta->mnt_pt);
    }
}

static gf_boolean_t glusterd_generic_probe(char *brick_path)
{
    int ret = -1;
    xlator_t *this = NULL;
    gf_boolean_t is_supported = _gf_false;
    glusterd_generic_metadata_t meta = {0};
    runner_t runner = { 0 };

    this = THIS;

    GF_VALIDATE_OR_GOTO("glusterd", this, out);
    GF_VALIDATE_OR_GOTO(this->name, brick_path, out);

    if (!glusterd_is_cmd_available(TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND))) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_COMMAND_NOT_FOUND,
               "generic snapshot command not found: %s", TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND));
        goto out;
    }

    if (!_get_metadata(&meta, brick_path)) {
        goto out;
    }

    runinit(&runner);
    runner_add_args(&runner, TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND), "probe",
                    meta.mnt_type, meta.brick_path, meta.mnt_pt, NULL);
    if (!runner_run(&runner)) {
        is_supported = _gf_true;
    }
out:
    _free_metadata(&meta);

    if (!is_supported) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_SNAP_CLONE_FAILED,
               "Brick (%s) not supported by generic snapshot command",
               meta.brick_path);
    }

    return is_supported;
}

// when clone=false, clonename and clone_volume_id are NULL
static int32_t _snapshot_create_or_clone(glusterd_brickinfo_t *snap_brickinfo,
                                    gf_boolean_t clone, char *snapname,
                                      char *snap_volume_id, char *clonename,
                                      char *clone_volume_id, int32_t brick_num)
{
    
    int ret = -1;
    xlator_t *this = THIS;
    char snap_id[NAME_MAX] = "";
    char clone_id[NAME_MAX] = "";
    glusterd_generic_metadata_t meta = {0};
    char *brick_path;
    runner_t runner = { 0 };

    GF_ASSERT(this);
    GF_ASSERT(snap_brickinfo);
    brick_path = snap_brickinfo->origin_path;

    if (!_get_metadata(&meta, brick_path)) {
        goto out;
    }
    if (!_get_snap_id(snap_id, snap_volume_id, brick_num)) {
        goto out;
    }

    /* Taking the actual snapshot */
    runinit(&runner);

    if (clone) {
        if (!_get_snap_id(clone_id, clone_volume_id, brick_num)) {
            goto out;
        }

        runner_add_args(&runner, TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND), "clone",
                        meta.mnt_type, meta.brick_path, meta.mnt_pt,
                        snap_id, clone_id, NULL);
    } else {
        runner_add_args(&runner, TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND), "snapshot",
                        meta.mnt_type, meta.brick_path, meta.mnt_pt,
                        snap_id, NULL);
    }

    ret = runner_run(&runner);
    if (ret) {
        if (clone)
            gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_SNAP_CLONE_FAILED,
                   "taking clone of the "
                   "brick (%s) failed",
                   brick_path);
        else
            gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_SNAP_CREATION_FAIL,
                   "taking snapshot of the "
                   "brick (%s) failed",
                   brick_path);
    }

out:
    _free_metadata(&meta);

    return ret;
}

/* This function actually calls the command for creating
   a snapshot of the backend brick filesystem.
*/
static int32_t glusterd_generic_snapshot_create(glusterd_brickinfo_t *snap_brickinfo,
                             char *snapname, char *snap_volume_id,
                             int32_t brick_num)
{
    return _snapshot_create_or_clone(
        snap_brickinfo, _gf_false, snapname, snap_volume_id, NULL, NULL, brick_num);
}

/* This function actually calls the command for cloning
   a snapshot of the backend brick filesystem.
*/
static int32_t glusterd_generic_snapshot_clone(glusterd_brickinfo_t *snap_brickinfo,
                            char *snapname, char *snap_volume_id,
                            char *clonename, char *clone_volume_id,
                            int32_t brick_num)
{
    return _snapshot_create_or_clone(snap_brickinfo, _gf_true, snapname,
                                                 snap_volume_id, clonename,
                                                 clone_volume_id, brick_num);
}

static int32_t glusterd_generic_brick_details(dict_t *rsp_dict,
                           glusterd_brickinfo_t *snap_brickinfo, char *snapname,
                           char *snap_volume_id, int32_t brick_num,
                           char *key_prefix)
{
    int32_t ret = -1;
    glusterd_conf_t *priv = NULL;
    xlator_t *this = THIS;
    char key[160] = ""; /* key_prefix is 128 bytes at most */
    char *brick_path = snap_brickinfo->origin_path;
    glusterd_generic_metadata_t meta = {0};

    GF_ASSERT(this);
    GF_ASSERT(rsp_dict);
    GF_ASSERT(snap_brickinfo);
    GF_ASSERT(key_prefix);
    priv = this->private;
    GF_ASSERT(priv);

    if (!_get_metadata(&meta, brick_path)) {
        goto out;
    }

    ret = snprintf(key, sizeof(key), "%s.vgname", key_prefix);
    if (ret < 0) {
        goto out;
    }

    ret = dict_set_str(rsp_dict, key, meta.mnt_pt);
    if (ret) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_DICT_SET_FAILED,
               "Could not save vgname ");
        goto out;
    }

    ret = snprintf(key, sizeof(key), "%s.data", key_prefix);
    if (ret < 0) {
        goto out;
    }

    ret = dict_set_str(rsp_dict, key, "-");
    if (ret) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_DICT_SET_FAILED,
               "Could not save data percent ");
        goto out;
    }

    ret = snprintf(key, sizeof(key), "%s.lvsize", key_prefix);
    if (ret < 0) {
        goto out;
    }

    ret = dict_set_str(rsp_dict, key, "-");
    if (ret) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_DICT_SET_FAILED,
               "Could not save meta data percent ");
        goto out;
    }

    ret = 0;

out:
    _free_metadata(&meta);

    return ret;
}

static int32_t glusterd_generic_snapshot_remove(glusterd_brickinfo_t *snap_brickinfo,
                             char *snapname, char *snap_volume_id,
                             int32_t brick_num)
{
    int32_t ret = -1;
    int len;
    xlator_t *this = THIS;
    glusterd_conf_t *priv = NULL;
    runner_t runner = { 0 };
    char snap_id[NAME_MAX] = "";
    char *brick_path = snap_brickinfo->origin_path;
    glusterd_generic_metadata_t meta = {0};

    GF_ASSERT(this);
    priv = this->private;
    GF_ASSERT(priv);
    GF_ASSERT(snap_brickinfo);

    if (!_get_metadata(&meta, brick_path)) {
        goto out;
    }

    if (!_get_snap_id(snap_id, snap_volume_id, brick_num)) {
        goto out;
    }

    runinit(&runner);
    runner_add_args(&runner, TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND), "remove",
                    meta.mnt_type, meta.brick_path, meta.mnt_pt,
                    snap_id, NULL);

    ret = runner_run(&runner);
    if (ret) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_SNAP_REMOVE_FAIL,
               "removing snapshot of the brick (%s) of snapshot %s failed",
               brick_path, snapname);
        goto out;
    }

out:
    _free_metadata(&meta);

    return ret;
}

static int32_t glusterd_generic_snapshot_activate_or_deactivate
(gf_boolean_t deactivate,
 glusterd_brickinfo_t *snap_brickinfo,
 char *snapname, char *snap_volume_id,
 int32_t brick_num)
{
    int32_t ret = -1;
    xlator_t *this = NULL;
    char snap_id[NAME_MAX] = "";
    char *brick_path = snap_brickinfo->origin_path;
    glusterd_generic_metadata_t meta = {0};
    runner_t runner = { 0 };

    this = THIS;
    GF_ASSERT(this);
    GF_ASSERT(snap_brickinfo);

    if (!_get_metadata(&meta, brick_path)) {
        goto out;
    }
    if (!_get_snap_id(snap_id, snap_volume_id, brick_num)) {
        goto out;
    }

    runinit(&runner);
    runner_add_args(&runner, TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND), deactivate ? "deactivate" : "activate",
                    meta.mnt_type, meta.brick_path, meta.mnt_pt,
                    snap_id, NULL);

    ret = runner_run(&runner);
    if (ret) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_SNAP_REMOVE_FAIL,
               deactivate ?
               "deactivating snapshot of the brick (%s) of snapshot %s failed"
               :
               "activating snapshot of the brick (%s) of snapshot %s failed"
               ,
               brick_path, snapname);
        goto out;
    }

out:
    _free_metadata(&meta);

    return ret;
}


static int32_t glusterd_generic_snapshot_activate(glusterd_brickinfo_t *snap_brickinfo,
                               char *snapname, char *snap_volume_id,
                               int32_t brick_num)
{
    return glusterd_generic_snapshot_activate_or_deactivate(false, snap_brickinfo, snapname,
                                                            snap_volume_id, brick_num);
}

static int32_t glusterd_generic_snapshot_deactivate(glusterd_brickinfo_t *snap_brickinfo,
                                 char *snapname, char *snap_volume_id,
                                 int32_t brick_num)
{
    return glusterd_generic_snapshot_activate_or_deactivate(true, snap_brickinfo, snapname,
                                                            snap_volume_id, brick_num);
}

static int32_t glusterd_generic_snapshot_restore(glusterd_brickinfo_t *snap_brickinfo,
                              char *snapname, char *snap_volume_id,
                              int32_t brick_num,
                              gf_boolean_t *retain_origin_path)
{
    int ret = -1;
    xlator_t *this = THIS;
    glusterd_conf_t *priv = NULL;
    runner_t runner = { 0 };
    int len;
    char snap_id[NAME_MAX] = "";
    char mnt_pt[PATH_MAX] = "";
    char *brick_path = snap_brickinfo->origin_path;
    glusterd_generic_metadata_t meta = {0};

    GF_ASSERT(this);
    priv = this->private;
    GF_ASSERT(priv);
    GF_ASSERT(snap_brickinfo);

    if (!_get_metadata(&meta, brick_path)) {
        goto out;
    }
    if (!_get_snap_id(snap_id, snap_volume_id, brick_num)) {
        goto out;
    }
    len = snprintf(mnt_pt, sizeof(mnt_pt), "%s/%s/brick%d",
                   snap_mount_dir, snap_volume_id, brick_num);
    if ((len < 0) || (len >= sizeof(mnt_pt))) {
        goto out;
    }

    runinit(&runner);
    runner_add_args(&runner, TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND), "restore",
                    meta.mnt_type, meta.brick_path, meta.mnt_pt,
                    snap_id, mnt_pt, NULL);

    ret = runner_run(&runner);
    if (ret) {
        gf_msg(this->name, GF_LOG_ERROR, 0, GD_MSG_SNAP_RESTORE_FAIL,
               "restoring snapshot of the "
               "brick (%s) of snapshot %s failed",
               brick_path, snapname);
        goto out;
    }
out:
    _free_metadata(&meta);

    return ret;
}

static int32_t glusterd_generic_snap_clone_brick_path(char *snap_mount_dir,
                                   char *origin_brick_path, int clone,
                                   char *snap_clone_name,
                                   char *snap_clone_volume_id,
                                   char *snap_brick_dir, int brick_num,
                                   glusterd_brickinfo_t *brickinfo, int restore)
{
    xlator_t *this = THIS;

    int ret = 0;
    int len;
    char *origin_brick_mount = NULL;
    char *origin_brick = NULL;
    char *brick_path = brickinfo->origin_path;
    char snap_id[NAME_MAX] = "";
    glusterd_generic_metadata_t meta = {0};
    runner_t runner = { 0 };
    char *ptr = NULL;

    this = THIS;
    GF_ASSERT(this);
    GF_ASSERT(brickinfo);

    origin_brick = gf_strdup(origin_brick_path);
    origin_brick_mount = dirname(origin_brick);

    if (restore && !clone) {
        len = snprintf(brickinfo->path, sizeof(brickinfo->path),
                       "%s/%s/brick%d%s", snap_mount_dir,
                       snap_clone_volume_id, brick_num, snap_brick_dir);
    } else {
        if (!_get_metadata(&meta, brick_path)) {
            goto out;
        }
        if (!_get_snap_id(snap_id, snap_clone_volume_id, brick_num)) {
            goto out;
        }

        runinit(&runner);
        runner_add_args(&runner, TOSTRING(GF_GENERIC_SNAPSHOT_COMMAND), clone ? "clone-path" : "snapshot-path",
                        meta.mnt_type, meta.brick_path, meta.mnt_pt,
                        origin_brick_mount, snap_id, snap_brick_dir,
                        NULL);
        runner_redir(&runner, STDOUT_FILENO, RUN_PIPE);

        ret = runner_start(&runner);
        if (ret) {
            runner_end(&runner);
            goto out;
        }

        ptr = fgets(brickinfo->path, sizeof(brickinfo->path), runner_chio(&runner, STDOUT_FILENO));
        if (!ptr || !strlen(brickinfo->path)) {
            runner_end(&runner);
            ret = -1;
            goto out;
        }
        strtok(brickinfo->path, "\n");

        ret = runner_end(&runner);

        if (ret) {
            goto out;
        }
    }

out:
    _free_metadata(&meta);

    if (origin_brick)
        GF_FREE(origin_brick);

    return ret;
}

struct glusterd_snap_ops generic_snap_ops = {
    .name = "GENERIC",
    .probe = glusterd_generic_probe,
    .details = glusterd_generic_brick_details,
    .create = glusterd_generic_snapshot_create,
    .clone = glusterd_generic_snapshot_clone,
    .remove = glusterd_generic_snapshot_remove,
    .activate = glusterd_generic_snapshot_activate,
    .deactivate = glusterd_generic_snapshot_deactivate,
    .restore = glusterd_generic_snapshot_restore,
    .brick_path = glusterd_generic_snap_clone_brick_path
};

#endif // GF_WITH_GENERIC_SNAPSHOT
