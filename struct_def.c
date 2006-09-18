#include <fuse.h>

int eccfs_getattr(const char *path, struct stat *stbuf);
int eccfs_access(const char *path, int mask);
int eccfs_readlink(const char *path, char *buf, size_t size);
int eccfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi);
int eccfs_mknod(const char *path, mode_t mode, dev_t rdev);
int eccfs_mkdir(const char *path, mode_t mode);
int eccfs_unlink(const char *path);
int eccfs_rmdir(const char *path);
int eccfs_symlink(const char *from, const char *to);
int eccfs_rename(const char *from, const char *to);
int eccfs_link(const char *from, const char *to);
int eccfs_chmod(const char *path, mode_t mode);
int eccfs_chown(const char *path, uid_t uid, gid_t gid);
int eccfs_truncate(const char *path, off_t size);
int eccfs_utime(const char *path, struct utimbuf *buf);
int eccfs_open(const char *path, struct fuse_file_info *fi);
int eccfs_read(const char *path, char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi);
int eccfs_write(const char *path, const char *buf, size_t size,
	      off_t offset, struct fuse_file_info *fi);
int eccfs_statfs(const char *path, struct statvfs *stbuf);

struct fuse_operations eccfs_oper = {
    .getattr	= eccfs_getattr,
    .access	= eccfs_access,
    .readlink	= eccfs_readlink,
    .readdir	= eccfs_readdir,
    .mknod	= eccfs_mknod,
    .mkdir	= eccfs_mkdir,
    .symlink	= eccfs_symlink,
    .unlink	= eccfs_unlink,
    .rmdir	= eccfs_rmdir,
    .rename	= eccfs_rename,
    .link	= eccfs_link,
    .chmod	= eccfs_chmod,
    .chown	= eccfs_chown,
    .truncate	= eccfs_truncate,
    .utime	= eccfs_utime,
    .open	= eccfs_open,
    .read	= eccfs_read,
    .write	= eccfs_write,
    .statfs	= eccfs_statfs,
};

