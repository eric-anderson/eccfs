/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2006  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

// TODO: figure out why eccfs is currently getting the sizes wrong with
// du, even though the cmp works properly.

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

#include <Lintel/LintelAssert.H>
#include <Lintel/StringUtil.H>
#include <Lintel/HashUnique.H>
#include <Lintel/HashMap.H>

#include <openssl/sha.h>
#include <boost/format.hpp>
#include <boost/foreach.hpp>

static const int reverify_interval_seconds = 3600*24;
static const bool debug_read = true;

struct eccfs_args {
  char *eccdirs;
  char *importdir;
};

struct header {
    unsigned char version;
    unsigned char under_size;
    // 5 bits: n, 5 bits: m, 6 bits: chunknum details in gflib/header.h
    unsigned char n_m_chunknum_a; 
    unsigned char n_m_chunknum_b;
    // see gflib/header.h for the details on how the next three hashes are calculated
    unsigned char sha1_file_hash[20];
    unsigned char sha1_crosschunk_hash[20];
    unsigned char sha1_chunk_hash[20];

    inline unsigned getn() {
	return (n_m_chunknum_a >> 3) & 0x1F;
    }

    inline unsigned getm() {
	return ((n_m_chunknum_a & 0x07) << 2) |
	    (n_m_chunknum_b >> 6);
    }

    inline unsigned getchunknum() {
	return n_m_chunknum_b & 0x3F;
    }
};

using namespace std;

static const string path_root("/");

bool
prefixequal(const string &str, const string &prefix)
{
    return str.compare(0,prefix.size(),prefix) == 0;
}

static string force_ecc_directory("/.force-ecc");
static string force_ecc_prefix(force_ecc_directory + "/");

static string magic_info_file("/.magic-info");
static string just_imported_directory("/.just-imported");
static string just_imported_prefix(just_imported_directory + "/");

// TODO: put something in that clears the various caches if they
// exceed some fixed size; otherwise the two HashMaps will grow
// unbounded.

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
	magic_info_data = (boost::format("V1\n1 %d\n") % eccdirs.size()).str();
	magic_info_data.append(importdir);
	magic_info_data.append("\n");
	BOOST_FOREACH(string &tmp, eccdirs) {
	    magic_info_data.append(tmp);
	    magic_info_data.append("\n");
	}

    }

    int getattr_ecc(const string &path, struct stat *stbuf) {
	string tmp;

	for(unsigned i = 0; i < eccdirs.size(); ++i) {
	    tmp = eccdirs[i] + path;
	    cout << "lstat-try(" << tmp << ")\n";
	    int lstat_ret = lstat(tmp.c_str(), stbuf);
	    if (lstat_ret == -1 && errno == ENOENT) {
		continue;
	    }
	    if (lstat_ret != 0) {
		int save_errno = errno;
		cout << boost::format("error on lstat(%s): %s") % tmp % strerror(save_errno) << endl;
		return -save_errno;
	    }

	    if (S_ISDIR(stbuf->st_mode)) {
		return 0;
	    }
	    int fd = open(tmp.c_str(), O_RDONLY | O_LARGEFILE);
	    if (fd == -1) {
		cout << boost::format("unable to open %s: %s") % tmp % strerror(errno) << endl;
		return -EINVAL;
	    }

	    struct header hdr;
	    ssize_t ret = read(fd, &hdr, sizeof(struct header));
	    if (ret != sizeof(struct header)) {
		cout << boost::format("unable to read header from %s, only got %d bytes") % tmp % ret << endl;
		goto close_bad;
	    }
	    
	    if (hdr.version != 1) {
		goto close_bad;
	    }
	    
	    {
		unsigned n = hdr.getn();
		unsigned long long orig_size = 
		    (unsigned long long)(stbuf->st_size-sizeof(struct header)) * n - hdr.under_size;
		unsigned long long sz = orig_size;
		if (sz % (n*sizeof(unsigned char)) != 0) {
		    sz += (n*sizeof(unsigned char) - (sz % (n*sizeof(unsigned char))));
		}
		unsigned long long blocksize = sz/n;
		if (blocksize != (unsigned long long)(stbuf->st_size - sizeof(struct header))) {
		    fprintf(stderr, "huh confused blocksize on %s?\n", path.c_str());
		    goto close_bad;
		}
		    
		stbuf->st_size = orig_size;
		ret = close(fd);
		if (ret != 0) {
		    fprintf(stderr, "Warning, error on close: %s\n", strerror(errno));
		}
		
		goto ok;
	    }
	    
	close_bad:
	    ret = close(fd);
	    if (ret != 0) {
		fprintf(stderr, "Warning, error on close: %s\n", strerror(errno));
	    }
	    errno = EINVAL;
	    goto bad;
	}

	cout << "getattr(" << path << ") ERROR: not found anywhere\n";
	return -ENOENT;
	
    bad:
	return -errno;
    ok:
	stbuf->st_dev = 0;
	stbuf->st_ino = 0;
	return 0;

    }

    int fuse_getattr(const string &path, struct stat *stbuf) {
	if (path == force_ecc_directory || path == just_imported_directory) {
	    return fuse_getattr("/", stbuf); // works as well as anything else...
	}
	if (path == magic_info_file) {
	    int ret = fuse_getattr("/", stbuf);
	    if (ret == 0) {
		stbuf->st_mode = 0100664;
		stbuf->st_nlink = 1;
		stbuf->st_size = magic_info_data.size();
		stbuf->st_blocks = 8;
	    }
	    return ret;
	}
	if (prefixequal(path, force_ecc_prefix)) {
	    string subpath(path, force_ecc_prefix.size() - 1);
	    fprintf(stderr, "force ecc prefix %s -> %s\n", path.c_str(), subpath.c_str());
	    return getattr_ecc(subpath, stbuf);
	}
	if (prefixequal(path, just_imported_prefix)) {
	    string subpath(path, just_imported_prefix.size() - 1);
	    fprintf(stderr, "clearing crosschunk cache for %s\n", subpath.c_str());
	    crosschunk_hash_cache.remove(subpath, false);
	    string tmp;
	    for(unsigned i=0; i < eccdirs.size(); ++i) {
		tmp = eccdirs[i] + subpath;
		fprintf(stderr, "clearing verify cache for %s\n", tmp.c_str());
		last_chunk_checksum_verify.remove(tmp, false);
	    }
	    // return a strange error as positive acknowledgment
	    return -ERANGE; // ought not ever get an error about math result not reproducable from a FS
	}
	// cout << "getattr(" << path << ");\n";
	// TODO: get stats from multiple places and cross verify
	string tmp = importdir + path;
	int ret = lstat(tmp.c_str(), stbuf);
	if (ret == 0) {
	    stbuf->st_dev = 0;
	    stbuf->st_ino = 0;
	    return 0;
	}

	if (errno != ENOENT) {
	    cout << "weird error getattr(" << path << ") ERROR " << errno << ";" << strerror(errno) << "\n";
	
	    return -errno;
	}

	return getattr_ecc(path, stbuf);
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
	while (NULL != (ent = readdir(dir))) {
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

    int fuse_readdir(const string &path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
	// TODO: decide whether if we are doing a readdir on / if we should include
	// force_ecc_directory in the list of returned strings
	cout << "readdir(" << path << "," << offset << ");\n";
	AssertAlways(offset == 0, ("Unimplemented offset = %lld, but does not seem to be a problem, tested with 35855 files in a directory", (long long)offset));
	
	HashUnique<string> unique;
	int ret = readdir_partial(importdir + path, buf, filler, unique);
	if (ret != 0 && ret != -ENOENT) { // ok to not have directory in import dir.
	    return ret;
	}
	for(unsigned i = 0; i < eccdirs.size(); ++i) {
	    ret = readdir_partial(eccdirs[i] + path, buf, filler, unique);
	    if (ret != 0 && ret != -ENOENT) { // allow missing directories
		return ret;
	    }
	}
	return 0;
    }

    int open_ecc(const string &path, struct fuse_file_info *fi) {
	if ((fi->flags & (O_RDONLY|O_LARGEFILE)) == fi->flags) { 
	    // Only open backing bits for RDONLY | LARGEFILE.
	    int fd = -1;
	    for(unsigned i = 0; i < eccdirs.size(); ++i) {
		string tmp = eccdirs[i] + path;
		fd = open(tmp.c_str(), fi->flags);
		if (fd != -1) { // ugly duplication with fuse_open :(
		    int ret = close(fd);
		    if (ret == -1) {
			fprintf(stderr, "Warning, close(%d from %s) failed: %s\n", 
				fd, path.c_str(), strerror(errno));
			return -EINVAL;
		    }
		    return 0;
		}
	    }
	    return -errno;
	} else {
	    printf("Unable to open %s with flags 0x%x should be 0x%x\n", path.c_str(), 
		   fi->flags, O_RDONLY | O_LARGEFILE);
	    return -EINVAL;
	}
    }

    // May want to think about the problem of opening the file and
    // then while the file is open someone else writes the same
    // filename; in that case, when we do later read() bits, we will
    // pull from the written bit, not the backing file
    int fuse_open(const string &path, struct fuse_file_info *fi) {
	if (prefixequal(path, force_ecc_prefix)) {
	    string subpath(path, force_ecc_prefix.size() - 1);
	    fprintf(stderr, "force ecc prefix %s -> %s\n", path.c_str(), subpath.c_str());
	    return open_ecc(subpath, fi);
	}
	if (path == magic_info_file) {
	    return 0;
	}
	string tmp = importdir + path;
	int fd = open(tmp.c_str(), fi->flags);
	if (fd == -1) {
	    return open_ecc(path, fi);
	}
	int ret = close(fd);
	if (ret == -1) {
	    fprintf(stderr, "Warning, close(%d from %s) failed: %s\n", 
		    fd, path.c_str(), strerror(errno));
	    return -EINVAL;
	}
	return 0;
    }

    void read_ecc_close(int fd, const string &path) {
	int ret = close(fd);
	if (ret != 0) {
	    fprintf(stderr, "error closing %d from %s: %s\n",
		    fd, path.c_str(), strerror(errno));
	}
    }
    
    // Could have this function fill the read buffer on the way
    // through or do the read explicitly if the recent verify rule is
    // still true.
    bool read_ecc_verify_chunk_checksum(int fd, const string &path, 
					struct header &header,
					unsigned long long blocksize) {
	time_t now = time(NULL);

	// TODO: store the header sha1 checksum as well as the
	// timestamp and only consider a validation ok if the header
	// hasn't changed checksums in practice if it has the
	// crosschunk hash should fail to validate, but this is yet
	// another good paranoia check.

	if (last_chunk_checksum_verify[path] > now - reverify_interval_seconds) {
	    return true; // verified recently, assume still ok.
	}
	SHA_CTX ctx;

	SHA1_Init(&ctx);

	if (sizeof(header) != 4+3*20) {
	    fprintf(stderr, "Header size mismatch\n");
	    abort();
	}
	
	const unsigned bufsize = 1024*1024;
	char buf[bufsize];
	
	unsigned long long remain = blocksize;
	while(remain > 0) {
	    int read_amt = remain > bufsize ? bufsize : remain;
	    int amt = read(fd, buf, read_amt);
	    if (amt != read_amt) {
		fprintf(stderr, "Error or EOF while reading %s (%d != %d; %lld remain %lld blocksize): %s\n",
			path.c_str(), amt, read_amt, remain, blocksize, strerror(errno));
		return false;
	    }
	    SHA1_Update(&ctx, buf, read_amt);
	    remain -= amt;
	}
	int amt = read(fd, buf, 1);
	if (amt != 0) {
	    fprintf(stderr, "Failed to get EOF from %s after reading %d + %lld bytes\n", 
		    path.c_str(), (int)sizeof(struct header), blocksize);
	    return false;
	}

	unsigned char tmpdigest[20];
	SHA1_Final(tmpdigest, &ctx);

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, &header, 4+2*20);
	SHA1_Update(&ctx, tmpdigest, 20);

	unsigned char digest[20];
	SHA1_Final(digest, &ctx);

	if (memcmp(digest, header.sha1_chunk_hash, 20) != 0) {
	    fprintf(stderr, "Digest mismatch while reading %s\n",
		    path.c_str());
	    return false;
	}
	
	last_chunk_checksum_verify[path] = now;
	return true;
    }

    // Returns amount read or -1 on skipping of chunk
    ssize_t 
    read_ecc_if_correct_chunk(int fd, string &path, 
			      char *buf, off_t offset, 
			      size_t size, unsigned long long &orig_size,
			      const string &eccfs_path) {
	struct header tmp;
	ssize_t ret = read(fd, &tmp, sizeof(struct header));
	if (ret != sizeof(struct header)) {
	    if (debug_read) fprintf(stderr, "ERR-shortheader\n");
	    return -1;
	    
	}
	if (tmp.version != 1) {
	    if (debug_read) fprintf(stderr, "ERR-unknownversion\n");
	    return -1; 
	}
	 
	struct stat stat_buf;
	if (fstat(fd, &stat_buf) != 0) {
	    fprintf(stderr, "error on stat of %s: %s\n",
		    path.c_str(), strerror(errno));
	    return -1;
	}
	
	if (!crosschunk_hash_cache.exists(eccfs_path)) {
	    crosschunk_hash_cache[eccfs_path] = string((char *)tmp.sha1_crosschunk_hash,20);
	}

	const string &crosschunk_hash = crosschunk_hash_cache[eccfs_path];
	if (crosschunk_hash.size() != 20) {
	    fprintf(stderr, "internal error, cache bad");
	    return -1;
	}

	if (memcmp(crosschunk_hash.data(), tmp.sha1_crosschunk_hash, 20) != 0) {
	    fprintf(stderr, "crosschunk hash differs\n");
	    return -1;
	}
	unsigned n = tmp.getn();
	orig_size = 
	    (unsigned long long)(stat_buf.st_size-sizeof(struct header)) * n - tmp.under_size;
	unsigned long long sz = orig_size;
	if (sz % (n*sizeof(unsigned char)) != 0) {
	    sz += (n*sizeof(unsigned char) - (sz % (n*sizeof(unsigned char))));
	}
	unsigned long long blocksize = sz/n;
	if (blocksize != (unsigned long long)(stat_buf.st_size - sizeof(struct header))) {
	    fprintf(stderr, "huh confused blocksize on %s?\n", path.c_str());
	    return -1;
	}
	
	unsigned filenum = tmp.getchunknum();
	
	if (filenum >= n) {
	    if (debug_read) fprintf(stderr, "SKIP - ecc-chunk\n");
	    return -1; // ecc chunk, ignorable
	}
	unsigned chunknum = offset / blocksize;
	
	if (chunknum != filenum) {
	    if (debug_read) fprintf(stderr, "SKIP - wrong-chunk\n");
	    return -1; // wrong chunk
	}
	
	// right chunk; verify checksum...
	if (!read_ecc_verify_chunk_checksum(fd, path, tmp, blocksize)) {
	    if (debug_read) fprintf(stderr, "SKIP - no verify\n");
	    return -1;
	    }
	
	off_t chunk_offset = offset - chunknum * blocksize;
	
	size_t chunk_read_size = size;
	if ((unsigned long long)(chunk_offset + chunk_read_size) > blocksize) {
	    chunk_read_size = blocksize - chunk_offset;
	}
	if ((unsigned long long)(offset + chunk_read_size) > orig_size) {
	    chunk_read_size = orig_size - offset;
	}
	ret = pread(fd, buf, chunk_read_size, chunk_offset + sizeof(struct header));
	if (ret != (ssize_t)chunk_read_size) {
	    fprintf(stderr, "error on read from %s (%lld != %lld): %s",
		    path.c_str(), (long long)ret, (long long)chunk_read_size,
		    strerror(errno));
	    return -1;
	}
	if (debug_read) fprintf(stderr, "SUCCESS, read %lld bytes\n", 
				    (long long)chunk_read_size);
	return chunk_read_size;
    }

    int read_ecc(const string &path, char *buf, size_t size, 
		 off_t offset) {
	string tmp;

	size_t remain_size = size;

	while(remain_size > 0) {
	    size_t prev_remain_size = remain_size;
	    if (debug_read) {
		fprintf(stderr, "  Read loop %s off=%lld size=%lld remain_size=%lld\n",
			path.c_str(), (long long)offset, (long long)size, (long long)remain_size);
	    }
	    // find the right chunk...
	    for(unsigned i = 0; i < eccdirs.size(); ++i) {
		tmp = eccdirs[i] + path;
		if (debug_read) {
		    fprintf(stderr, "    Read ecc chunk %s: ", path.c_str());
		}
		int fd = open(tmp.c_str(), O_RDONLY | O_LARGEFILE);
		if (fd == -1) {
		    if (debug_read) fprintf(stderr, "ERR-unopenable\n");
		    continue;
		}
		unsigned long long orig_size;
		ssize_t amt_read = read_ecc_if_correct_chunk(fd, tmp, buf, offset, 
							     remain_size, orig_size,
							     path);
		if (amt_read >= 0) {
		    offset += amt_read;
		    remain_size -= amt_read;
		    buf += amt_read;
		    if (debug_read) {
			fprintf(stderr, "    Success read %lld offset=%lld, remain_size=%lld\n",
				(long long)amt_read, offset, (long long)remain_size);
		    }
		    if ((unsigned long long)offset == orig_size) {
			// Asked to read more than is present in the file...
			size -= remain_size;
			remain_size = 0;
		    }
		    int ret = close(fd);
		    if (ret != 0) {
			fprintf(stderr, "Error on close");
			return -EINVAL;
		    }
		    break;
		}
		read_ecc_close(fd, tmp);
	    }
	    if (prev_remain_size == remain_size) {
		fprintf(stderr, "Internal, size remaining didn't drop after read\n");
		return -EINVAL;
	    }
	}
	if (remain_size == 0) {
	    if (debug_read) {
		printf("successfully read %d bytes\n", (int)size);
	    }
	    return size;
	}
	return -EINVAL;
    }

    int read_magic_info(char *buf, size_t size, off_t offset) {
	if (offset != 0) 
	    return -EINVAL;
	if (size < magic_info_data.size()) {
	    return -EINVAL;
	}
	memcpy(buf,magic_info_data.data(),magic_info_data.size());
	return magic_info_data.size();
    }

    int fuse_read(const string &path, char *buf, size_t size, 
	     off_t offset) {
	if (debug_read) {
	    printf("\n");
	}
	if (prefixequal(path, force_ecc_prefix)) {
	    string subpath(path, force_ecc_prefix.size() - 1);
	    fprintf(stderr, "force ecc prefix %s -> %s\n", path.c_str(), subpath.c_str());
	    return read_ecc(subpath, buf, size, offset);
	}
	if (path == magic_info_file) {
	    return read_magic_info(buf, size, offset);
	}
	string tmp = importdir + path;
	int fd = open(tmp.c_str(), O_RDONLY);
	if (fd == -1) {
	    return read_ecc(path, buf, size, offset);
	}
	if (debug_read) {
	    cout << "read-import " << path << " bytes " << size << " offset " << offset << "\n";
	}
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
    HashMap<string, string> crosschunk_hash_cache;
    string magic_info_data;
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
    return fs.fuse_getattr(path, stbuf);
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
    return fs.fuse_readdir(path, buf, filler, offset, fi);
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
    return fs.fuse_open(path,fi);
}

extern "C"
int eccfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    return fs.fuse_read(path, buf, size, offset);
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
