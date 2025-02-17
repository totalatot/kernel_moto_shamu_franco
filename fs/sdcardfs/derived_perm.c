/*
 * fs/sdcardfs/derived_perm.c
 *
 * Copyright (c) 2013 Samsung Electronics Co. Ltd
 *   Authors: Daeho Jeong, Woojoong Lee, Seunghwan Hyun,
 *               Sunghwan Yun, Sungjong Seo
 *
 * This program has been developed as a stackable file system based on
 * the WrapFS which written by
 *
 * Copyright (c) 1998-2011 Erez Zadok
 * Copyright (c) 2009     Shrikar Archak
 * Copyright (c) 2003-2011 Stony Brook University
 * Copyright (c) 2003-2011 The Research Foundation of SUNY
 *
 * This file is dual licensed.  It may be redistributed and/or modified
 * under the terms of the Apache 2.0 License OR version 2 of the GNU
 * General Public License.
 */

#include "sdcardfs.h"

/* copy derived state from parent inode */
static void inherit_derived_state(struct inode *parent, struct inode *child)
{
	struct sdcardfs_inode_info *pi = SDCARDFS_I(parent);
	struct sdcardfs_inode_info *ci = SDCARDFS_I(child);

	ci->perm = PERM_INHERIT;
	ci->userid = pi->userid;
	ci->d_uid = pi->d_uid;
	ci->under_android = pi->under_android;
}

/* helper function for derived state */
void setup_derived_state(struct inode *inode, perm_t perm,
                        userid_t userid, uid_t uid, bool under_android)
{
	struct sdcardfs_inode_info *info = SDCARDFS_I(inode);

	info->perm = perm;
	info->userid = userid;
	info->d_uid = uid;
	info->under_android = under_android;
}

/* While renaming, there is a point where we want the path from dentry, but the name from newdentry */
void get_derived_permission_new(struct dentry *parent, struct dentry *dentry, struct dentry *newdentry)
{
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);
	struct sdcardfs_inode_info *info = SDCARDFS_I(dentry->d_inode);
	struct sdcardfs_inode_info *parent_info= SDCARDFS_I(parent->d_inode);
	appid_t appid;

	/* By default, each inode inherits from its parent.
	 * the properties are maintained on its private fields
	 * because the inode attributes will be modified with that of
	 * its lower inode.
	 * The derived state will be updated on the last
	 * stage of each system call by fix_derived_permission(inode).
	 */

	inherit_derived_state(parent->d_inode, dentry->d_inode);

	/* Derive custom permissions based on parent and current node */
	switch (parent_info->perm) {
		case PERM_INHERIT:
			/* Already inherited above */
			break;
		case PERM_PRE_ROOT:
			/* Legacy internal layout places users at top level */
			info->perm = PERM_ROOT;
			info->userid = simple_strtoul(newdentry->d_name.name, NULL, 10);
			break;
		case PERM_ROOT:
			/* Assume masked off by default. */
			if (!strcasecmp(newdentry->d_name.name, "Android")) {
				/* App-specific directories inside; let anyone traverse */
				info->perm = PERM_ANDROID;
				info->under_android = true;
			}
			break;
		case PERM_ANDROID:
			if (!strcasecmp(newdentry->d_name.name, "data")) {
				/* App-specific directories inside; let anyone traverse */
				info->perm = PERM_ANDROID_DATA;
			} else if (!strcasecmp(newdentry->d_name.name, "obb")) {
				/* App-specific directories inside; let anyone traverse */
				info->perm = PERM_ANDROID_OBB;
				/* Single OBB directory is always shared */
			} else if (!strcasecmp(newdentry->d_name.name, "media")) {
				/* App-specific directories inside; let anyone traverse */
				info->perm = PERM_ANDROID_MEDIA;
			}
			break;
		case PERM_ANDROID_DATA:
		case PERM_ANDROID_OBB:
		case PERM_ANDROID_MEDIA:
			appid = get_appid(sbi->pkgl_id, newdentry->d_name.name);
			if (appid != 0) {
				info->d_uid = multiuser_get_uid(parent_info->userid, appid);
			}
			break;
	}
}

void get_derived_permission(struct dentry *parent, struct dentry *dentry)
{
	get_derived_permission_new(parent, dentry, &dentry->d_name);
}

static appid_t get_type(const char *name)
{
	const char *ext = strrchr(name, '.');
	appid_t id;

	if (ext && ext[0]) {
		ext = &ext[1];
		id = get_ext_gid(ext);
		return id?:AID_MEDIA_RW;
	}
	return AID_MEDIA_RW;
}

void fixup_lower_ownership(struct dentry *dentry, const char *name)
{
	struct path path;
	struct inode *inode;
	int error;
	struct sdcardfs_inode_info *info;
	struct sdcardfs_inode_data *info_d;
	struct sdcardfs_inode_data *info_top;
	perm_t perm;
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);
	uid_t uid = sbi->options.fs_low_uid;
	gid_t gid = sbi->options.fs_low_gid;
	struct iattr newattrs;

	if (!sbi->options.gid_derivation)
		return;

	info = SDCARDFS_I(dentry->d_inode);
	info_d = info->data;
	perm = info_d->perm;
	if (info_d->under_obb) {
		perm = PERM_ANDROID_OBB;
	} else if (info_d->under_cache) {
		perm = PERM_ANDROID_PACKAGE_CACHE;
	} else if (perm == PERM_INHERIT) {
		info_top = top_data_get(info);
		perm = info_top->perm;
		data_put(info_top);
	}

	switch (perm) {
	case PERM_ROOT:
	case PERM_ANDROID:
	case PERM_ANDROID_DATA:
	case PERM_ANDROID_MEDIA:
	case PERM_ANDROID_PACKAGE:
	case PERM_ANDROID_PACKAGE_CACHE:
		uid = multiuser_get_uid(info_d->userid, uid);
		break;
	case PERM_ANDROID_OBB:
		uid = AID_MEDIA_OBB;
		break;
	case PERM_PRE_ROOT:
	default:
		break;
	}
	switch (perm) {
	case PERM_ROOT:
	case PERM_ANDROID:
	case PERM_ANDROID_DATA:
	case PERM_ANDROID_MEDIA:
		if (S_ISDIR(dentry->d_inode->i_mode))
			gid = multiuser_get_uid(info_d->userid, AID_MEDIA_RW);
		else
			gid = multiuser_get_uid(info_d->userid, get_type(name));
		break;
	case PERM_ANDROID_OBB:
		gid = AID_MEDIA_OBB;
		break;
	case PERM_ANDROID_PACKAGE:
		if (uid_is_app(info_d->d_uid))
			gid = multiuser_get_ext_gid(info_d->d_uid);
		else
			gid = multiuser_get_uid(info_d->userid, AID_MEDIA_RW);
		break;
	case PERM_ANDROID_PACKAGE_CACHE:
		if (uid_is_app(info_d->d_uid))
			gid = multiuser_get_ext_cache_gid(info_d->d_uid);
		else
			gid = multiuser_get_uid(info_d->userid, AID_MEDIA_RW);
		break;
	case PERM_PRE_ROOT:
	default:
		break;
	}

	sdcardfs_get_lower_path(dentry, &path);
	inode = path.dentry->d_inode;
	if (path.dentry->d_inode->i_gid != gid || path.dentry->d_inode->i_uid != uid) {
		newattrs.ia_valid = ATTR_GID | ATTR_UID | ATTR_FORCE;
		newattrs.ia_uid = make_kuid(current_user_ns(), uid);
		newattrs.ia_gid = make_kgid(current_user_ns(), gid);
		if (!S_ISDIR(inode->i_mode))
			newattrs.ia_valid |=
				ATTR_KILL_SUID | ATTR_KILL_SGID | ATTR_KILL_PRIV;
		mutex_lock(&inode->i_mutex);
		error = security_path_chown(&path, newattrs.ia_uid, newattrs.ia_gid);
		if (!error)
			error = notify_change2(path.mnt, path.dentry, &newattrs);
		mutex_unlock(&inode->i_mutex);
		if (error)
			pr_debug("sdcardfs: Failed to touch up lower fs gid/uid for %s\n", name);
	}
	sdcardfs_put_lower_path(dentry, &path);
}

static int descendant_may_need_fixup(struct sdcardfs_inode_data *data,
		struct limit_search *limit)
{
	if (data->perm == PERM_ROOT)
		return (limit->flags & BY_USERID) ?
				data->userid == limit->userid : 1;
	if (data->perm == PERM_PRE_ROOT || data->perm == PERM_ANDROID)
		return 1;
	return 0;
}

static int needs_fixup(perm_t perm)
{
	if (perm == PERM_ANDROID_DATA || perm == PERM_ANDROID_OBB
			|| perm == PERM_ANDROID_MEDIA)
		return 1;
	return 0;
}

void get_derive_permissions_recursive(struct dentry *parent) {
	struct dentry *dentry;
	list_for_each_entry(dentry, &parent->d_subdirs, d_u.d_child) {
		if (dentry && dentry->d_inode) {
			mutex_lock(&dentry->d_inode->i_mutex);
			get_derived_permission(parent, dentry);
			fix_derived_permission(dentry->d_inode);
			get_derive_permissions_recursive(dentry);
			mutex_unlock(&dentry->d_inode->i_mutex);
		}
	}
}

/* main function for updating derived permission */
inline void update_derived_permission_lock(struct dentry *dentry)
{
	struct dentry *parent;

	if(!dentry || !dentry->d_inode) {
		printk(KERN_ERR "sdcardfs: %s: invalid dentry\n", __func__);
		return;
	}
	/* FIXME:
	 * 1. need to check whether the dentry is updated or not
	 * 2. remove the root dentry update
	 */
	mutex_lock(&dentry->d_inode->i_mutex);
	if(IS_ROOT(dentry)) {
		//setup_default_pre_root_state(dentry->d_inode);
	} else {
		parent = dget_parent(dentry);
		if(parent) {
			get_derived_permission(parent, dentry);
			dput(parent);
		}
	}
	fix_derived_permission(dentry->d_inode);
	mutex_unlock(&dentry->d_inode->i_mutex);
}

int need_graft_path(struct dentry *dentry)
{
	int ret = 0;
	struct dentry *parent = dget_parent(dentry);
	struct sdcardfs_inode_info *parent_info= SDCARDFS_I(parent->d_inode);
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);

	if(parent_info->perm == PERM_ANDROID &&
			!strcasecmp(dentry->d_name.name, "obb")) {

		/* /Android/obb is the base obbpath of DERIVED_UNIFIED */
		if(!(sbi->options.multiuser == false
				&& parent_info->userid == 0)) {
			ret = 1;
		}
	}
	dput(parent);
	return ret;
}

int is_obbpath_invalid(struct dentry *dent)
{
	int ret = 0;
	struct sdcardfs_dentry_info *di = SDCARDFS_D(dent);
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dent->d_sb);
	char *path_buf, *obbpath_s;

	/* check the base obbpath has been changed.
	 * this routine can check an uninitialized obb dentry as well.
	 * regarding the uninitialized obb, refer to the sdcardfs_mkdir() */
	spin_lock(&di->lock);
	if(di->orig_path.dentry) {
 		if(!di->lower_path.dentry) {
			ret = 1;
		} else {
			path_get(&di->lower_path);
			//lower_parent = lock_parent(lower_path->dentry);

			path_buf = kmalloc(PATH_MAX, GFP_ATOMIC);
			if(!path_buf) {
				ret = 1;
				printk(KERN_ERR "sdcardfs: fail to allocate path_buf in %s.\n", __func__);
			} else {
				obbpath_s = d_path(&di->lower_path, path_buf, PATH_MAX);
				if (d_unhashed(di->lower_path.dentry) ||
					strcasecmp(sbi->obbpath_s, obbpath_s)) {
					ret = 1;
				}
				kfree(path_buf);
			}

			//unlock_dir(lower_parent);
			path_put(&di->lower_path);
		}
	}
	spin_unlock(&di->lock);
	return ret;
}

int is_base_obbpath(struct dentry *dentry)
{
	int ret = 0;
	struct dentry *parent = dget_parent(dentry);
	struct sdcardfs_inode_info *parent_info= SDCARDFS_I(parent->d_inode);
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);

	spin_lock(&SDCARDFS_D(dentry)->lock);
	if (sbi->options.multiuser) {
		if(parent_info->perm == PERM_PRE_ROOT &&
				!strcasecmp(dentry->d_name.name, "obb")) {
			ret = 1;
		}
	} else  if (parent_info->perm == PERM_ANDROID &&
			!strcasecmp(dentry->d_name.name, "obb")) {
		ret = 1;
	}
	spin_unlock(&SDCARDFS_D(dentry)->lock);
	return ret;
}

/* The lower_path will be stored to the dentry's orig_path
 * and the base obbpath will be copyed to the lower_path variable.
 * if an error returned, there's no change in the lower_path
 * returns: -ERRNO if error (0: no error) */
int setup_obb_dentry(struct dentry *dentry, struct path *lower_path)
{
	int err = 0;
	struct sdcardfs_sb_info *sbi = SDCARDFS_SB(dentry->d_sb);
	struct path obbpath;

	/* A local obb dentry must have its own orig_path to support rmdir
	 * and mkdir of itself. Usually, we expect that the sbi->obbpath
	 * is avaiable on this stage. */
	sdcardfs_set_orig_path(dentry, lower_path);

	err = kern_path(sbi->obbpath_s,
			LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &obbpath);

	if(!err) {
		/* the obbpath base has been found */
		printk(KERN_INFO "sdcardfs: the sbi->obbpath is found\n");
		pathcpy(lower_path, &obbpath);
	} else {
		/* if the sbi->obbpath is not available, we can optionally
		 * setup the lower_path with its orig_path.
		 * but, the current implementation just returns an error
		 * because the sdcard daemon also regards this case as
		 * a lookup fail. */
		printk(KERN_INFO "sdcardfs: the sbi->obbpath is not available\n");
	}
	return err;
}


