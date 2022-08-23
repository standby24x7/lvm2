/*
 * Copyright (C) 2022 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/device/device.h"
#include "lib/device/dev-type.h"
#include "lib/misc/lvm-exec.h"
#include "lib/activate/dev_manager.h"

#include <dirent.h>
#include <mntent.h>
#include <sys/ioctl.h>

/*
 * Set the path of the dm-crypt device, i.e. /dev/dm-N, that is using the LV.
 */
static int _get_crypt_path(dev_t lv_devt, char *lv_path, char *crypt_path)
{
	char holders_path[PATH_MAX];
	char *holder_name;
	DIR *dr;
	struct stat st;
	struct dirent *de;
	int ret = 0;

	if (dm_snprintf(holders_path, sizeof(holders_path), "%sdev/block/%d:%d/holders",
			dm_sysfs_dir(), (int)MAJOR(lv_devt), (int)MINOR(lv_devt)) < 0)
		return_0;

	/* If the crypt dev is not active, there will be no LV holder. */
	if (stat(holders_path, &st)) {
		log_error("Missing %s for %s", crypt_path, lv_path);
		return 0;
	}

	if (!(dr = opendir(holders_path))) {
		log_error("Cannot open %s", holders_path);
		return 0;
	}

	while ((de = readdir(dr))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		holder_name = de->d_name;

		if (strncmp(holder_name, "dm", 2)) {
			log_error("Unrecognized holder %s of %s", holder_name, lv_path);
			ret = 0;
			break;
		}

		/* We could read the holder's dm uuid to verify it's a crypt dev. */

		if (dm_snprintf(crypt_path, PATH_MAX, "/dev/%s", holder_name) < 0) {
			ret = 0;
			stack;
			break;
		}
		ret = 1;
		break;
	}
	closedir(dr);
	if (ret)
		log_debug("Found holder %s of %s.", crypt_path, lv_path);
	else
		log_debug("No holder in %s", holders_path);
	return ret;
}

int fs_get_info(struct cmd_context *cmd, struct logical_volume *lv,
		struct fs_info *fsi, int include_mount)
{
	char lv_path[PATH_MAX];
	char crypt_path[PATH_MAX];
	struct stat st_lv;
	struct stat st_crypt;
	struct stat st_top;
	struct stat stme;
	struct fs_info info;
	FILE *fme = NULL;
	struct mntent *me;
	int ret;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir,
			lv->vg->name, lv->name) < 0) {
		log_error("Couldn't create LV path for %s.", display_lvname(lv));
		return 0;
	}

	if (stat(lv_path, &st_lv) < 0) {
		log_error("Failed to get LV path %s", lv_path);
		return 0;
	}

	memset(&info, 0, sizeof(info));

	if (!fs_get_blkid(lv_path, &info)) {
		log_error("No file system info from blkid for %s", display_lvname(lv));
		return 0;
	}

	if (fsi->nofs)
		return 1;

	/*
	 * If there's a LUKS dm-crypt layer over the LV, then
	 * return fs info from that layer, setting needs_crypt
	 * to indicate a crypt layer between the fs and LV.
	 */
	if (!strcmp(info.fstype, "crypto_LUKS")) {
		if (!_get_crypt_path(st_lv.st_rdev, lv_path, crypt_path)) {
			log_error("Cannot find active LUKS dm-crypt device using %s.",
				  display_lvname(lv));
			return 0;
		}

		if (stat(crypt_path, &st_crypt) < 0) {
			log_error("Failed to get crypt path %s", crypt_path);
			return 0;
		}

		memset(&info, 0, sizeof(info));

		log_print("File system found on crypt device %s on LV %s.",
			  crypt_path, display_lvname(lv));

		if (!fs_get_blkid(crypt_path, &info)) {
			log_error("No file system info from blkid for dm-crypt device %s on LV %s.",
				  crypt_path, display_lvname(lv));
			return 0;
		}
		*fsi = info;
		fsi->needs_crypt = 1;
		fsi->crypt_devt = st_crypt.st_rdev;
		memcpy(fsi->fs_dev_path, crypt_path, PATH_MAX);
		st_top = st_crypt;

		if (!get_crypt_table_offset(st_crypt.st_rdev, &fsi->crypt_offset_bytes)) {
			log_error("Failed to get crypt data offset.");
			return 0;
		}
	} else {
		*fsi = info;
		memcpy(fsi->fs_dev_path, lv_path, PATH_MAX);
		st_top = st_lv;
	}

	if (!include_mount)
		return 1;

	if (!(fme = setmntent("/etc/mtab", "r")))
		return_0;

	ret = 1;

	while ((me = getmntent(fme))) {
		if (strcmp(me->mnt_type, fsi->fstype))
			continue;
		if (me->mnt_dir[0] != '/')
			continue;
		if (me->mnt_fsname[0] != '/')
			continue;
		if (stat(me->mnt_dir, &stme) < 0)
			continue;
		if (stme.st_dev != st_top.st_rdev)
			continue;

		log_debug("fs_get_info %s is mounted \"%s\"", fsi->fs_dev_path, me->mnt_dir);
		fsi->mounted = 1;
		strncpy(fsi->mount_dir, me->mnt_dir, PATH_MAX-1);
	}
	endmntent(fme);

	fsi->unmounted = !fsi->mounted;
	return ret;
}

#define FS_CMD_MAX_ARGS 16

int crypt_resize_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
			uint64_t newsize_bytes_fs)
{
	char crypt_path[PATH_MAX];
	char newsize_str[16] = { 0 };
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (dm_snprintf(newsize_str, sizeof(newsize_str), "%llu", (unsigned long long)newsize_bytes_fs) < 0)
		return_0;

	if (dm_snprintf(crypt_path, sizeof(crypt_path), "/dev/dm-%d", (int)MINOR(fsi->crypt_devt)) < 0)
		return_0;

	argv[0] = LVRESIZE_FS_HELPER_PATH; /* from configure, usually in /usr/libexec */
	argv[++args] = "--cryptresize";
	argv[++args] = "--cryptpath";
	argv[++args] = crypt_path;
	argv[++args] = "--newsizebytes";
	argv[++args] = newsize_str;
	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to resize crypt dev with lvresize_fs_helper.");
		return 0;
	}

	return 1;
}

/*
 * The helper script does the following steps for reduce:
 * devpath = $cryptpath ? $cryptpath : $lvpath
 * if needs_unmount
 * 	umount $mountdir 
 * if needs_fsck
 * 	e2fsck -f -p $devpath
 * if needs_mount
 * 	mount $devpath $tmpdir
 * if $fstype == "ext"
 * 	resize2fs $devpath $newsize_kb
 * if needs_crypt
 * 	cryptsetup resize --size $newsize_sectors $cryptpath
 *
 * Note: when a crypt layer is included, newsize_bytes_fs is smaller
 * than newsize_bytes_lv because of the crypt header.
 */

int fs_reduce_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		     uint64_t newsize_bytes_fs, char *fsmode)
{
	char lv_path[PATH_MAX];
	char crypt_path[PATH_MAX];
	char newsize_str[16] = { 0 };
	const char *argv[FS_CMD_MAX_ARGS + 4];
	char *devpath;
	int args = 0;
	int status;

	if (dm_snprintf(newsize_str, sizeof(newsize_str), "%llu", (unsigned long long)newsize_bytes_fs) < 0)
		return_0;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir, lv->vg->name, lv->name) < 0)
		return_0;

	argv[0] = LVRESIZE_FS_HELPER_PATH; /* from configure, usually in /usr/libexec */
	argv[++args] = "--fsreduce";
	argv[++args] = "--fstype";
	argv[++args] = fsi->fstype;
	argv[++args] = "--lvpath";
	argv[++args] = lv_path;

	if (newsize_bytes_fs) {
		argv[++args] = "--newsizebytes";
		argv[++args] = newsize_str;
	}
	if (fsi->mounted) {
		argv[++args] = "--mountdir";
		argv[++args] = fsi->mount_dir;
	}

	if (fsi->needs_unmount)
		argv[++args] = "--unmount";
	if (fsi->needs_mount)
		argv[++args] = "--mount";
	if (fsi->needs_fsck)
		argv[++args] = "--fsck";

	if (fsi->needs_crypt) {
		if (dm_snprintf(crypt_path, sizeof(crypt_path), "/dev/dm-%d", (int)MINOR(fsi->crypt_devt)) < 0)
			return_0;
		argv[++args] = "--cryptresize";
		argv[++args] = "--cryptpath";
		argv[++args] = crypt_path;
	}

	/*
	 * fsmode manage means the fs should be remounted after
	 * resizing if it was unmounted.
	 */
	if (fsi->needs_unmount && !strcmp(fsmode, "manage"))
		argv[++args] = "--remount";

	argv[++args] = NULL;

	devpath = fsi->needs_crypt ? crypt_path : (char *)display_lvname(lv);

	log_print("Reducing file system %s to %s (%llu bytes) on %s...",
		  fsi->fstype, display_size(cmd, newsize_bytes_fs/512),
		  (unsigned long long)newsize_bytes_fs, devpath);

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to reduce file system with lvresize_fs_helper.");
		return 0;
	}

	log_print("Reduced file system %s on %s.", fsi->fstype, devpath);

	return 1;
}

/*
 * The helper script does the following steps for extend:
 * devpath = $cryptpath ? $cryptpath : $lvpath
 * if needs_unmount
 * 	umount $mountdir 
 * if needs_fsck
 * 	e2fsck -f -p $devpath
 * if needs_crypt
 * 	cryptsetup resize $cryptpath
 * if needs_mount
 * 	mount $devpath $tmpdir
 * if $fstype == "ext"
 * 	resize2fs $devpath
 * if $fstype == "xfs"
 * 	xfs_growfs $devpath
 *
 * Note: when a crypt layer is included, newsize_bytes_fs is smaller
 * than newsize_bytes_lv because of the crypt header.
 */

int fs_extend_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		     uint64_t newsize_bytes_fs, char *fsmode)
{
	char lv_path[PATH_MAX];
	char crypt_path[PATH_MAX];
	const char *argv[FS_CMD_MAX_ARGS + 4];
	char *devpath;
	int args = 0;
	int status;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir, lv->vg->name, lv->name) < 0)
		return_0;

	argv[0] = LVRESIZE_FS_HELPER_PATH; /* from configure, usually in /usr/libexec */
	argv[++args] = "--fsextend";
	argv[++args] = "--fstype";
	argv[++args] = fsi->fstype;
	argv[++args] = "--lvpath";
	argv[++args] = lv_path;

	if (fsi->mounted) {
		argv[++args] = "--mountdir";
		argv[++args] = fsi->mount_dir;
	}

	if (fsi->needs_unmount)
		argv[++args] = "--unmount";
	if (fsi->needs_mount)
		argv[++args] = "--mount";
	if (fsi->needs_fsck)
		argv[++args] = "--fsck";

	if (fsi->needs_crypt) {
		if (dm_snprintf(crypt_path, sizeof(crypt_path), "/dev/dm-%d", (int)MINOR(fsi->crypt_devt)) < 0)
			return_0;
		argv[++args] = "--cryptresize";
		argv[++args] = "--cryptpath";
		argv[++args] = crypt_path;
	}

	/*
	 * fsmode manage means the fs should be remounted after
	 * resizing if it was unmounted.
	 */
	if (fsi->needs_unmount && !strcmp(fsmode, "manage"))
		argv[++args] = "--remount";

	argv[++args] = NULL;

	devpath = fsi->needs_crypt ? crypt_path : (char *)display_lvname(lv);

	log_print("Extending file system %s to %s (%llu bytes) on %s...",
		  fsi->fstype, display_size(cmd, newsize_bytes_fs/512),
		  (unsigned long long)newsize_bytes_fs, devpath);

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to extend file system with lvresize_fs_helper.");
		return 0;
	}

	log_print("Extended file system %s on %s.", fsi->fstype, devpath);

	return 1;
}
