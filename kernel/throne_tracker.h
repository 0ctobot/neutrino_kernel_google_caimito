#ifndef __KSU_H_UID_OBSERVER
#define __KSU_H_UID_OBSERVER
#include <linux/namei.h>

void ksu_throne_tracker_init();

void ksu_throne_tracker_exit();

void track_throne(bool prune_only);

/**
 * is_lock_held - Check if a file's dentry lock is held
 * @path: Path to check
 *
 * Returns true if the file is being modified (locked), false if stable.
 * Used to avoid accessing files during rename/delete operations.
 */
static bool is_lock_held(const char *path)
{
    struct path kpath;

    if (kern_path(path, 0, &kpath))
        return true;

    if (!kpath.dentry) {
        path_put(&kpath);
        return true;
    }

    if (!spin_trylock(&kpath.dentry->d_lock)) {
        pr_info("%s: lock held for %s, bail out!\n", __func__, path);
        path_put(&kpath);
        return true;
    }

    // We acquired the lock, release it
    spin_unlock(&kpath.dentry->d_lock);
    path_put(&kpath);
    return false;
}

#endif
