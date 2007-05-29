#!/usr/bin/python
import sys,os,re,string,sha,stat

# call to main() is at the bottom of the script

def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        error("Usage: " + sys.argv[0] + " <eccfs-mount-point>")

    eccfs_dir = sys.argv[1]

    f = open(eccfs_dir + '/.magic-info','r')
    if f.readline() != "V1\n":
        error("Only able to eccfs magic-info V1")
    
    tmp = f.readline()
    
    match = re.search(r'1 (\d+)',tmp)
    
    if not match:
        error("not V1?")
        
    neccdirs = int(match.group(1))
    
    importdir = chomp_readline(f)
    
    eccdirs = []
    cur_scan_positions = []
    for i in range(neccdirs):
        dir = chomp_readline(f)
        eccdirs.append(dir)
        cur_scan_positions.append(get_cur_scan_position(dir + "/.magic-info"))
        
    cur_scan_positions = [ [ 'fuse-2.5.2','lib','.libs' ] ]
    cur_scan_positions.sort()

    doscan([],eccdirs,cur_scan_positions[0],0)

def get_cur_scan_position(filename):
    try:
        f = open(filename,'r')
    except IOError:
        return []
    error("unimplemented")

def error(s):
    print s
    sys.exit(1)
    
def chomp_readline(f):
    tmp = f.readline();
    if tmp[-1:] != "\n":
        error("internal")
    return tmp[:-1]

def doscan(curpos, eccdirs, skiptos, scancount):
    curbase = "/" + string.join(curpos, "/")
    filelist = unionreaddir(eccdirs, curbase)

    if len(skiptos) > 0:
        skipto = skiptos.pop(0)
        while len(filelist) > 0 and filelist[0] < skipto:
            filelist.pop(0)
	if filelist[0] != skipto:
	    # not an exact match, prune skiptos so we don't incorrectly prune
	    # on sub-directories
	    skiptos = []        

    print curbase, " --> ", filelist
    if curbase != "/":
        curbase += "/"
        
    for file in filelist:
        curpath = curbase + file
        if os.path.isdir(eccdirs[0] + curpath):
            check_onlydir(eccdirs,curpath)
            scancount = doscan(curpos + [file], eccdirs, skiptos, scancount)
        else:
            check_file(eccdirs,curpath)
            scancount += 1

    return scancount

def check_onlydir(eccdirs, path):
    for eccdir in eccdirs:
        tmp = eccdir + path
        if not os.path.isdir(tmp):
            error("Directory hierarchy not parallel over '" + path + "', missing " + tmp)

def check_file(eccdirs, path):
    print "Checking file " + path
    files = []
    for eccdir in eccdirs:
        if os.path.isfile(eccdir + path):
            files += [ECCFile(eccdir + path)]
    n = files[0].n
    m = files[0].m
    under_size = files[0].under_size
    sha1_file_hash = files[0].sha1_file_hash
    sha1_crosschunk_hash = files[0].sha1_crosschunk_hash
    for eccfile in files:
        if eccfile.n != n:
            error("mismatch on n")
        if eccfile.m != m:
            error("mismatch on m")
        if eccfile.under_size != under_size:
            error("mismatch on under_size")
        if eccfile.sha1_file_hash != sha1_file_hash:
            error("mismatch on sha1_file_hash")
        if eccfile.sha1_crosschunk_hash != sha1_crosschunk_hash:
            error("mismatch on crosschunk hash")
    files.sort(lambda x,y: cmp(x.chunknum, y.chunknum))

    sha1 = sha.new()
    sha1_filehash = sha.new()
    for i in range(len(files)):
        f = files[i]
        if f.chunknum != i:
            error("missing chunk")
        sha1.update(f.header + f.sha1_file_hash + f.sha1_data_digest)
        if i < n:
            bytes = f.chunk_size
            if i == n-1:
                bytes = f.chunk_size - f.under_size
            f.sha_filedata(sha1_filehash, bytes)

    if sha1.digest() != sha1_crosschunk_hash:
        error("bad crosschunk hash")
    if sha1_filehash.digest() != sha1_file_hash:
        error("bad file hash")
        
def unionreaddir(eccdirs, basedir):
    found = {}
    for eccdir in eccdirs:
        for file in os.listdir(eccdir + basedir):
            found[file] = 1
    ret = found.keys()
    ret.sort()
    print "unionreaddr(" + basedir + "): " + str(ret)
    return ret

class ECCFile:
    "Class for verifying ecc files"

    def __init__(self, filename):
        self.filename = filename
        self.file = open(filename, 'r')

        self.header = self.xread(4)
        if ord(self.header[0]) != 1:
            error("bad version in file " + filename)
        self.under_size = ord(self.header[1])
        a = ord(self.header[2])
        b = ord(self.header[3])
        self.n = (a >> 3) & 0x1F
        self.m = ((a & 0x3) << 2) | (b >> 6)
        self.chunknum = b & 0x3F
                                     
        self.sha1_file_hash = self.xread(20)
        self.sha1_crosschunk_hash = self.xread(20)
        self.sha1_chunk_hash = self.xread(20)

        statbits = os.fstat(self.file.fileno())
        self.chunk_size = statbits[stat.ST_SIZE] - (4+3*20)
        self.file_size = self.chunk_size * self.n - self.under_size

        sha1 = sha.new()
        self.sha_remaining(sha1, self.chunk_size)
        tmp = self.file.read(1)
        if len(tmp) != 0:
            error("Found extra data at end of file")
        self.sha1_data_digest = sha1.digest()

        sha1 = sha.new()
        sha1.update(self.header + self.sha1_file_hash
                    + self.sha1_crosschunk_hash)
        sha1.update(self.sha1_data_digest)

        check_chunk_hash = sha1.digest()
        if check_chunk_hash != self.sha1_chunk_hash:
            error("Mismatch on chunk hash")

        # print filename + ": n=" + str(self.n) + ", m=" + str(self.m) + ", chunknum=" + str(self.chunknum)

    def sha_filedata(self, sha1, bytes):
        self.file.seek(4+3*20)
        self.sha_remaining(sha1, bytes)

    def sha_remaining(self, sha1, bytes):
        while bytes > 0:
            amt = bytes
            if amt > 256*1024:
                amt = 256*1024
            data = self.file.read(amt)
            if len(data) != amt:
                error("did not read expected amount")
            sha1.update(data)
            bytes -= amt

    def xread(self,n):
        ret = self.file.read(n)
        if len(ret) != n:
            error("short read from file " + filename)
        return ret
    
# make sure this stays last
main()

