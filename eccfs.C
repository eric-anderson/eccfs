/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2006  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

// #include <config.h>

#ifdef linux
/* For pread()/pwrite() */
#define _XOPEN_SOURCE 500
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <LintelAssert.H>
#include <StringUtil.H>
#include <HashUnique.H>

#include <openssl/sha.h>

static const int reverify_interval_seconds = 3600*24;

struct eccfs_args {
  char *eccdirs;
  char *importdir;
};

struct header {
    unsigned char version;
    unsigned char under_size;
    unsigned char n_m_filenum_a; // 5 bits: n, 5 bits: m, 6 bits: filenum
    unsigned char n_m_filenum_b;
    unsigned char sha1_file_hash[20];
    unsigned char sha1_chunk_hash[20];

    inline unsigned getn() {
	return n_m_filenum_a & 0x1F;
    }

    inline unsigned getm() {
	return (n_m_filenum_a >> 5) & 0x07 + 
	    ((n_m_filenum_b & 0x03) << 3);
    }

    inline unsigned getfilenum() {
	return (n_m_filenum_b >> 2) & 0x3F;
    }
};

using namespace std;

static const string path_root("/");
class EccFS {
public:
    void init(eccfs_args *args) {
	AssertAlways(args->eccdirs != NULL, 
		     ("eccdirs option is required"));
	AssertAlways(args->importdir != NULL,
		     ("importdir option is required"));
		    
	split(args->eccdirs,",",eccdirs);
	importdir = args->importdir;
	AssertAlways(importdir[importdir.size()-1] != '/',("bad"));
	for(unsigned i = 0; i < eccdirs.size(); ++i) {
	    string &tmp = eccdirs[i];
	    AssertAlways(tmp[tmp.size()-1] != '/',("bad"));
	}
    }

    int getattr(const string &path, struct stat *stbuf) {
	// cout << "getattr(" << path << ");\n";
	// TODO: get stats from multiple places and cross verify
	string tmp = importdir + path;
	int ret = lstat(tmp.c_str(), stbuf);
	if (ret == 0) {
	    goto ok;
	}
	if (errno != ENOENT) {
	    goto bad;
	}
	for(unsigned i = 0; i < eccdirs.size(); ++i) {
	    tmp = eccdirs[i] + path;
	    cout << "lstat-try(" << tmp << ")\n";
	    ret = lstat(tmp.c_str(), stbuf);
	    if (ret == 0) {
		goto ok;
	    }
	    if (ret != -1 || errno != ENOENT) {
		goto bad;
	    }
	}
	cout << "getattr(" << path << ") ERROR: not found anywhere\n";
	return -ENOENT;
    bad:
	cout << "weird error getattr(" << path << ") ERROR " << errno << ";" << strerror(errno) << "\n";
	
	return -errno;
	
    ok:
	stbuf->st_dev = 0;
	stbuf->st_ino = 0;
	return 0;
    }

    static const int debug_readdir_partial = 1;

    int readdir_partial(const string &path, void *buf, fuse_fill_dir_t filler,
			HashUnique<string> &unique) {
	if (debug_readdir_partial) cout << "readdir_partial(" << path << ")\n";
	DIR *dir = opendir(path.c_str());
	if (dir == NULL) {
	    return -errno;
	}
	
	struct stat tmp;
	memset(&tmp, 0, sizeof(tmp));
	struct dirent *ent;
	while (NULL != (ent = ::readdir(dir))) {
	    if (debug_readdir_partial > 1) cout << "readdir_partial(" << path << "): file " << ent->d_name << ": ";
	    string d_name(ent->d_name);
	    if (unique.exists(d_name)) {
		if (debug_readdir_partial > 1) cout << "duplicate\n";
		continue;
	    } else {
		if (debug_readdir_partial > 1) cout << "new\n";
		unique.add(d_name);
	    }
	    tmp.st_ino = ent->d_ino;
	    tmp.st_mode = ent->d_type << 12;
		
	    if (filler(buf, ent->d_name, &tmp, 0))
		break;
	}
	int ret = closedir(dir);
	if (ret != 0) {
	    cout << "closedir(" << path << ") failed??\n";
	    return -errno;
	}
	return 0;
    }

    int readdir(const string &path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
	cout << "readdir(" << path << "," << offset << ");\n";
	AssertAlways(offset == 0, ("Unimplemented offset = %lld, but does not seem to be a problem, tested with 35855 files in a directory", (long long)offset));
	
	HashUnique<string> unique;
	int ret = readdir_partial(importdir + path, buf, filler, unique);
	if (ret != 0) {
	    return ret;
	}
	for(unsigned i = 0; i < eccdirs.size(); ++i) {
	    ret = readdir_partial(eccdirs[i] + path, buf, filler, unique);
	    if (ret != 0) {
		return ret;
	    }
	}
	return 0;
    }

    // May want to think about the problem of opening the file and
    // then while the file is open someone else writes the same
    // filename; in that case, when we do later read() bits, we will
    // pull from the written bit, not the backing file
    int open(const string &path, struct fuse_file_info *fi) {
	string tmp = importdir + path;
	int fd = ::open(tmp.c_str(), fi->flags);
	if (fd == -1 && fi->flags == O_RDONLY) { // Can't open backing bits for anything other than RDONLY.
	    for(unsigned i = 0; i < eccdirs.size(); ++i) {
		tmp = eccdirs[i] + path;
		fd = ::open(tmp.c_str(), fi->flags);
		if (fd != -1)
		    break;
	    }
	    return -errno;
	}
	int ret = close(fd);
	if (ret == -1) {
	    fprintf(stderr, "Warning, close(%s) failed: %s\n", 
		    path.c_str(), strerror(errno));
	    return -EINVAL;
	}
	return 0;
    }

    void read_ecc_close(int fd, const string &path) {
	int ret = close(fd);
	if (ret != 0) {
	    fprintf(stderr, "error closing %s: %s\n",
		    path.c_str(), strerror(errno));
	}
    }
    
    bool read_ecc_verify_chunk_checksum(int fd, const string &path, 
					struct header &header,
					unsigned long long blocksize) {
	time_t now = time(NULL);
	if (last_chunk_checksum_verify[path] > now - reverify_interval_seconds) {
	    return true; // verified recently, assume still ok.
	}
	SHA_CTX ctx;

	SHA1_Init(&ctx);

	if (sizeof(header) != 4+20+20) {
	    fprintf(stderr, "Header size mismatch\n");
	    abort();
	}
	SHA1_Update(&ctx, &header, 4+20);
	
	const unsigned bufsize = 1024*1024;
	char buf[bufsize];
	
	while(blocksize > 0) {
	    int remain = blocksize > bufsize ? bufsize : blocksize;
	    int amt = ::read(fd, buf, remain);
	    if (amt != remain) {
		fprintf(stderr, "Error or EOF while reading %s: %s\n",
			path.c_str(), strerror(errno));
		return false;
	    }
	    SHA1_Update(&ctx, buf, remain);
	    blocksize -= amt;
	}

	unsigned char digest[20];
	SHA1_Final(digest, &ctx);
	if (memcmp(digest, header.sha1_chunk_hash, 20) != 0) {
	    fprintf(stderr, "Digest mismatch while reading %s\n",
		    path.c_str());
	    return false;
	}
	
	return true;
    }

    bool read_ecc_is_correct_chunk(int fd, string &path, unsigned &chunk_size,
				   off_t offset) {
	struct header tmp;
	int ret = pread(fd, &tmp, sizeof(struct header), 0);
	if (ret != sizeof(struct header)) {
	    return false;
	    
	}
	if (tmp.version == 1) {
	    struct stat buf;
	    if (fstat(fd, &buf) != 0) {
		fprintf(stderr, "error on stat of %s: %s\n",
			path.c_str(), strerror(errno));
		return false;
	    }
		
	    unsigned n = tmp.getn();
	    unsigned long long orig_size = 
		(unsigned long long)(buf.st_size-sizeof(struct header)) * n - tmp.under_size;
	    unsigned long long sz = orig_size;
	    if (sz % (n*sizeof(unsigned char)) != 0) {
		sz += (n*sizeof(unsigned char) - (sz % (n*sizeof(unsigned char))));
	    }
	    unsigned long long blocksize = sz/n;
	    if (blocksize != (unsigned long long)(buf.st_size - sizeof(struct header))) {
		fprintf(stderr, "huh confused blocksize on %s?\n", path.c_str());
		return false;
	    }
	    
	    unsigned filenum = tmp.getfilenum();

	    if (filenum >= n) {
		return false; // ecc chunk, ignorable
	    }
	    unsigned chunknum = offset / blocksize;
	    
	    if (chunknum != filenum) {
		return false; // wrong chunk
	    }
	    
	    // right chunk; verify checksum...
	    return read_ecc_verify_chunk_checksum(fd, path, tmp, blocksize);
	}		
	return false; 
    }

    int read_ecc(const string &path, char *buf, size_t size, 
		 off_t offset) {
	string tmp;
	// find the right chunk...
	for(unsigned i = 0; i < eccdirs.size(); ++i) {
	    tmp = eccdirs[i] + path;
	    int fd = open(tmp.c_str(), O_RDONLY);
	    if (fd == -1) {
		continue;
	    }
	    unsigned chunk_size = 0;
	    if (!read_ecc_is_correct_chunk(fd, tmp, chunk_size, offset)) {
		read_ecc_close(fd, tmp);
	    }
	    read_ecc_close(fd, tmp);
	}
	return -EINVAL;
    }

    int read(const string &path, char *buf, size_t size, 
	     off_t offset) {
	string tmp = importdir + path;
	int fd = ::open(tmp.c_str(), O_RDONLY);
	if (fd == -1) {
	    return read_ecc(path, buf, size, offset);
	}
	cout << "read-import " << path << " bytes " << size << " offset " << offset << "\n";
	int ret = pread(fd, buf, size, offset);
	if (ret == -1) {
	    return -errno;
	}
	close(fd);
	return ret;
    }
private:
    vector<string> eccdirs;
    string importdir;
    HashMap<string, time_t> last_chunk_checksum_verify;
};

eccfs_args eccfs_args;
EccFS fs;

static struct fuse_opt eccfs_opts[] = {
  { "--eccdirs=%s",  offsetof(struct eccfs_args, eccdirs), 0 },
  { "--importdir=%s", offsetof(struct eccfs_args, importdir), 0 },
};

extern "C"
int eccfs_getattr(const char *path, struct stat *stbuf)
{
    return fs.getattr(path, stbuf);
}

extern "C"
int eccfs_access(const char *path, int mask)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = access(path, mask);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_readlink(const char *path, char *buf, size_t size)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = readlink(path, buf, size - 1);
    if (res == -1)
        return -errno;

    buf[res] = '\0';
    return 0;
}


extern "C"
int eccfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		  off_t offset, struct fuse_file_info *fi)
{
    return fs.readdir(path, buf, filler, offset, fi);
}

extern "C"
int eccfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    /* On Linux this could just be 'mknod(path, mode, rdev)' but this
       is more portable */
    if (S_ISREG(mode)) {
        res = open(path, O_CREAT | O_EXCL | O_WRONLY, mode);
        if (res >= 0)
            res = close(res);
    } else if (S_ISFIFO(mode))
        res = mkfifo(path, mode);
    else
        res = mknod(path, mode, rdev);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_mkdir(const char *path, mode_t mode)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = mkdir(path, mode);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_unlink(const char *path)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = unlink(path);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_rmdir(const char *path)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = rmdir(path);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_symlink(const char *from, const char *to)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = symlink(from, to);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_rename(const char *from, const char *to)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = rename(from, to);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_link(const char *from, const char *to)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = link(from, to);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_chmod(const char *path, mode_t mode)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = chmod(path, mode);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_chown(const char *path, uid_t uid, gid_t gid)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = lchown(path, uid, gid);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_truncate(const char *path, off_t size)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = truncate(path, size);
    if (res == -1)
        return -errno;

    return 0;
}

extern "C"
int eccfs_utime(const char *path, struct utimbuf *buf)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = utime(path, buf);
    if (res == -1)
        return -errno;

    return 0;
}


extern "C"
int eccfs_open(const char *path, struct fuse_file_info *fi)
{
    return fs.open(path,fi);
}

extern "C"
int eccfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    return fs.read(path, buf, size, offset);
}

extern "C"
int eccfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    int fd;
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    (void) fi;
    fd = open(path, O_WRONLY);
    if (fd == -1)
        return -errno;

    res = pwrite(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return res;
}

extern "C" 
int eccfs_statfs(const char *path, struct statvfs *stbuf)
{
    int res;

    printf("Unimplemented %s\n", __PRETTY_FUNCTION__);
    return -EINVAL;
    res = statvfs(path, stbuf);
    if (res == -1)
        return -errno;

    return 0;
}

extern struct fuse_operations eccfs_oper;

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    umask(0);

    memset(&eccfs_args,sizeof(struct eccfs_args), 0);
    if (-1 == fuse_opt_parse(&args, &eccfs_args, eccfs_opts, NULL)) {
        exit(1);
    }

    fs.init(&eccfs_args);

    return fuse_main(args.argc, args.argv, &eccfs_oper);
}
