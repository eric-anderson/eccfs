#!/usr/bin/perl -w
use strict;
use threads;
use threads::shared;
use File::Find;
use POSIX;
use Digest::SHA1;
use MIME::Base64;
use FileHandle;
use File::Copy;
use File::Compare;
use Filesys::Statvfs;
use Fcntl ':flock';
use Carp;
use Getopt::Long;
use File::Path;

$|=1;
$GLOBAL::debug = 0;

my $files_under;
my $base_dir;
my $nthreads = 3;

my $ret = GetOptions("path=s" => \$files_under,
		     "base=s" => \$base_dir,
		     "threads=i" => \$nthreads);
usage("missing arguments.")
    unless $ret && @ARGV == 1 && -d $ARGV[0];

my $eccfsdir = $ARGV[0];

my($lock, $rs_encode_file, $rs_decode_file, $workbase, $encodedir,
   $decodedir, $importdir, @eccdirs) = setup();

if (defined $files_under) {
    $base_dir ||= "";
    setupFilesUnder($files_under,$base_dir,$importdir);
}

# Notes: $fixup_decisions{subname} defines the rules for fixing up
# conflicts between files and directories.  This is so that the user
# only has to specify the fixup rule once.

my %reverify_directories;
my %reverify_files : shared;
my %fixup_decisions : shared; # this may not need to be shared

my @pending_imports : shared;
my $done : shared;
$done = 0;
my @threads;
for(my $i=0; $i < $nthreads; ++$i) {
    push(@threads, threads->create(sub { importerThread($i) }));
}
find(\&wanted, $importdir);
{
    lock(@pending_imports);
    $done = 1;
    cond_broadcast(@pending_imports);
}
print "Waiting for importers to finish...\n";
foreach my $thread (@threads) {
    my $ret = $thread->join();
    die "import thread failed??" unless $ret;
}
die "??" unless @pending_imports == 0;
rmdir($encodedir) or die "Can't rmdir $encodedir: $!";
rmdir($decodedir) or die "Can't rmdir $decodedir: $!";

print "Syncing filesystem...\n";
system("sync") == 0 
    or die "sync failed: $!";

print "Reverifying files...\n";
foreach my $subname (sort keys %reverify_files) {
    print "   verify $subname\n";
    my ($n, $m, @eccusedirs) = @{$reverify_files{$subname}};
    my %inuse;
    my @eccfiles = map { $inuse{$_} = 1; "$_/$subname" } @eccusedirs;
    foreach my $eccdir (@eccdirs) {
	next if $inuse{$eccdir};
	die "Incorrectly still existing file $eccdir/$subname"
	    if -f "$eccdir/$subname";
    }
    verifyEccSplitup('xx', $decodedir, "$importdir/$subname", \@eccfiles, $n, $m, 0);

    # Tell eccfs that we have just imported $subname
    my @ret = stat("$eccfsdir/.just-imported/$subname");
    die "just-imported stat failed: $!" 
	unless @ret == 0 && $! eq 'Numerical result out of range';

    die "Mismatch between $importdir/$subname and $eccfsdir/.force-ecc/$subname"
	unless compare("$importdir/$subname","$eccfsdir/.force-ecc/$subname") == 0;
    my $fh = new FileHandle "$importdir/$subname"
	or die "bad";
    my $sha1 = Digest::SHA1->new();
    $sha1->addfile($fh);
    my $digest = $sha1->hexdigest();
    $fh->close();
    
    unlink("$importdir/$subname") or die "Can't remove $importdir/$subname: $!";
    $fh = new FileHandle "$eccfsdir/$subname"
	or die "bad";
    $sha1 = new Digest::SHA1;
    $sha1->addfile($fh);
    my $eccdigest = $sha1->hexdigest();
    $fh->close();
    die "Mayday, checked ecc data but now it's changed $digest != $eccdigest for $subname"
	unless $digest eq $eccdigest;
}

print "Reverifying directories...\n";
foreach my $subname (reverse sort keys %reverify_directories) {
    foreach my $eccdir (@eccdirs) {
	die "??" unless -d "$eccdir/$subname";
    }
    next if $subname eq '';

    rmdir("$importdir/$subname")
	or die "Can't rmdir $importdir/$subname: $!";
}

exit(0);

sub setup {
    {
	open(MAGIC,"$eccfsdir/.magic-info")
	    or die "Unable to open $eccfsdir/.magic-info for read: $!";
	my $tmp = '';
	my $amt = read(MAGIC,$tmp,16384);
	
	chomp $tmp;
	my @lines = split("\n",$tmp);
	die "Invalid version info in $eccfsdir/.magic-info"
	    unless $lines[0] eq 'V1';
	shift @lines;
	die "Invalid line 2 in $eccfsdir/.magic-info"
	    unless $lines[0] =~ /^1 (\d+)$/o;
	my $neccdirs = $1;
	shift @lines;
	die "??" . scalar (@lines) . " != 1+$neccdirs" unless @lines == 1+$neccdirs;
	$importdir = shift @lines;
	@eccdirs = @lines;
	close(MAGIC);
    }
    
    usage("importdir '$importdir' not a dir") unless -d $importdir;
    map { usage("eccdir $_ not a dir") unless -d $_; } @eccdirs;
    
    my $rs_encode_file = "$ENV{HOME}/projects/eccfs/gflib/rs_encode_file";
    die "$rs_encode_file not executable" unless -x $rs_encode_file;
    
    my $rs_decode_file = "$ENV{HOME}/projects/eccfs/gflib/rs_decode_file";
    die "$rs_decode_file not executable" unless -x $rs_decode_file;
    
    my $workbase = "/tmp/workdir";
    unless (-d $workbase) {
	mkdir($workbase, 0770) or die "Can't mkdir $workbase: $!";
    }
    my $lock = getlock("$workbase/lock",60);
    die "Could not get lock file; another import is running??"
	unless defined $lock;
    my $encodedir = "$workbase/encode";
    if (-d $encodedir) {
	opendir(DIR, $encodedir) or die "noopendir $encodedir: $!";
	while(my $file = readdir(DIR)) {
	    next unless $file =~ /^ecc/o;
	    unlink("$encodedir/$file")
		or die "can't remove $encodedir/$file: $!";
	}
	closedir(DIR);
    } else {
	mkdir($encodedir, 0770) or die "Can't mkdir $encodedir: $!";
    }
    my $decodedir = "$workbase/decode";
    if (-d $decodedir) {
	opendir(DIR, $decodedir) or die "noopendir $decodedir: $!";
	while(my $file = readdir(DIR)) {
	    next unless $file =~ /^decode/o;
	unlink("$decodedir/$file")
	    or die "can't remove $decodedir/$file: $!";
	}
	closedir(DIR);
    } else {
	mkdir($decodedir, 0770) or die "Can't mkdir $decodedir: $!";
    }
    return ($lock, $rs_encode_file, $rs_decode_file, $workbase,
	    $encodedir, $decodedir, $importdir, @eccdirs);
}

sub wanted {
    my $subname = $File::Find::name;
    $subname =~ s!^$importdir!!o or die "?? $File::Find::name";
    $subname =~ s!^/+!!o;
    print "Wanted ($File::Find::name) -> $subname\n" if $GLOBAL::debug;
    if (-l $File::Find::name) {
	warn "importing symlink $File::Find::name as a file"
	    unless defined $files_under;
	handlefile($subname);
    } elsif (-d $File::Find::name) {
	handledir($subname);
    } elsif (-f $File::Find::name) {
	handlefile($subname);
    } else {
	die "$File::Find::name is not a file or a directory, this is unsupported.";
    }
}

sub handledir {
    my($subname) = @_;

    # Don't have to worry about parent directories as they would already have been processed by
    # handledir when handling importing of the parent

    foreach my $eccdir (@eccdirs) {
	next if -d "$eccdir/$subname"; # already ok
	if (-f "$eccdir/$subname") {
	    my $t = getFixup($subname, "$eccdir/$subname is a file, but $importdir/$subname is a directory.");

	    if ($t->[0] eq 'delete') {
		unlink("$eccdir/$subname") or die "Can't remove $eccdir/$subname: $!";
	    } elsif ($t->[0] eq 'rename') {
		rename("$eccdir/$subname","$eccdir/$t->[1]")
		    or die "Can't rename $eccdir/$subname to $eccdir/$t->[1]: $!";
	    }
	}
	mkdir("$eccdir/$subname",0777) or die "Can't mkdir $eccdir/$subname: $!";
    }
    $reverify_directories{$subname} = 1;
}

sub handlefile {
    my($subname) = @_;

    # TODO: consider doing a bit of waiting if @pending_imports is really long
    lock(@pending_imports);
    push(@pending_imports, $subname);
    cond_signal(@pending_imports);
}

sub getPending {
    lock(@pending_imports);
    while(1) {
	return shift @pending_imports
	    if @pending_imports > 0;
	return undef if $done;
	cond_wait(@pending_imports);
    }
}

sub importerThread {
    my ($threadid) = @_;

    print "Start importer thread #$threadid...\n";
    while(1) {
	my $file = getPending();
	last unless defined $file;
	importfile($file, $threadid);
    }
    lock(@pending_imports);
    die "??" unless $done;
    print "Finish importer thread #$threadid...\n";
    return 1;
}

sub importfile {
    my($subname, $threadid) = @_;

    print "  handleFile($subname)\n" if $GLOBAL::debug;

    my ($n,$m) = determineNM($subname);
    print "import $subname as ($n,$m)\n";
    my $max = @eccdirs;
    die "Unable to import $subname, should be broken into $n data and $m parity pieces, but only $max places available"
	unless $n + $m <= $max;

    my $import_size = -s "$importdir/$subname";
    my @eccusedirs = selectEccDirs($n, $m);
    die "huh" . scalar @eccusedirs unless @eccusedirs == $n + $m;
    my $q_subname = quotemeta($subname);
    my $ret = system("$rs_encode_file $importdir/$q_subname $n $m $encodedir/ecc-t$threadid >/dev/null 2>&1");
    die "Encoding of $subname failed?"
	unless $ret == 0;
    
    my $eccsize = 4+20+20 + POSIX::ceil($import_size / $n); # header + datasize
    my @eccfiles = map { sprintf("%s/ecc-t$threadid-%04d.rs", $encodedir, $_) } (0 .. $n+$m - 1);
    verifyEccSplitup($threadid, $decodedir, "$importdir/$subname", \@eccfiles, $n, $m, 1);

    # Don't have to worry about parent directories as they would already have been processed by
    # handledir when handling importing of the parent

    my $warned = 0;
    foreach my $eccdir (@eccdirs) {
	if (-f "$eccdir/$subname") {
	    warn "WARNING: overwriting $subname, might not successfully create new version"
		unless $warned;
	    $warned = 1;
	    unlink("$eccdir/$subname");
	} elsif (-d "$eccdir/$subname") {
	    my $t = getFixup($subname, "$eccdir/$subname is a directory, but $importdir/$subname is a file.");
	    if ($t->[0] eq 'delete') {
		die "delete unimplemented.";
	    } elsif ($t->[0] eq 'rename') {
		rename("$eccdir/$subname","$eccdir/$t->[1]")
		    or die "Can't rename $eccdir/$subname to $eccdir/$t->[1]: $!";
	    } else {
		die "internal";
	    }
	}
    }

    die "??" unless @eccusedirs == $n + $m;
    my $i = 0;
    foreach my $eccdir (@eccusedirs) {
	my $from = sprintf("%s/ecc-t$threadid-%04d.rs", $encodedir, $i);
	copy($from, "$eccdir/$subname")
	    or die "Unable to copy $from to $eccdir/$subname: $!";
	++$i;
	# this is just unlinking the temporary ecc working stuff ...
	unlink($from) or die "Unable to unlink $from: $!";
    }
    
    # Interestingly, the evidence is that this makes the import run
    # faster; presumably it's managing to overlap more of the I/Os.

    foreach my $eccdir (@eccusedirs) {
  	my $tmp = new FileHandle("+<$eccdir/$subname")
  	    or die "bad $eccdir/$subname: $!";
  	my $tmp2 = new IO::Handle;
  	$tmp2->fdopen(fileno($tmp),"w");
	$tmp2->sync() or die "bad: $!";
  	$tmp->close();
    }

    die "internal $i != $n + $m" unless $i == $n + $m;
    lock(%reverify_files);
    # This works, oddly you can't &share(['f','g','h'])
    my $tmp = &share([]);
    @$tmp = ($n, $m, @eccusedirs);
    $reverify_files{$subname} = $tmp;
}

sub getFixup {
    my($subname, $msg) = @_;

    lock(%fixup_decisions);
    lock(@pending_imports); # lock this to make other threads not print stuff out
    while (! defined $fixup_decisions{$subname}) {
	print "$msg\n";
	print "what do you want to do with existing $subname: abort, delete, or rename [abort]?";
	my $choice = <STDIN>;
	chomp;
	$choice = 'abort' if $choice eq '';
	if ($choice =~ /^a(bort)?$/io) {
	    exit(1);
	} elsif ($choice =~ /^d(elete)?$/io) {
	    $fixup_decisions{$subname} = ['delete'];
	} elsif ($choice =~ /^r(ename)?$/io) {
	    my $n = 0;
	    my $default = "$subname.$n";
	    while(existsAnyEcc($default)) {
		++$n;
		$default = "$subname.$n";
	    }
	    print "what should $subname be renamed to [$default]: ";
	    my $rename_to = <STDIN>;
	    chomp;
	    $rename_to = $default if $rename_to eq '';
	    if ($rename_to =~ m!^/!o) {
		warn "$rename_to invalid";
	    } elsif (existsAnyEcc($rename_to)) {
		warn "$rename_to already exists in some ecc dir";
	    } else {
		$fixup_decisions{$subname} = ['rename',$rename_to];
	    }
	} else {
	    die "Unrecognized choice '$choice'";
	}
    } 

    print "Chose to $fixup_decisions{$subname}->[0] $subname\n";

    return $fixup_decisions{$subname};
}

sub existsAnyEcc {
    my($subname) = @_;

    foreach my $eccdir (@eccdirs) {
	return 1 if -e "$eccdir/$subname";
    }
    return 0;
}


sub usage {
    die "$_[0]\nUsage: $0 [--threads=#] <eccfs-mount-point>"
}

sub verifyEccSplitup {
    my($threadid, $decodedir, $dataname, $files, $n, $m, $verify_level) = @_;

    print "verifyEccSplitup($dataname, [ " . join(", ", @$files) . "], $n, $m)\n"
	if $GLOBAL::debug;
    my $size = -s $dataname;

    my $fh = new FileHandle($dataname) 
	or die "Unable to open $dataname for read: $!";
    my $sha1 = Digest::SHA1->new;
    my $bytes_read = 0;
    while (1) {
	my $buffer;
	my $amt = sysread($fh, $buffer, 262144);
	die "Read failed: $!" unless defined $amt && $amt >= 0;
	last if $amt == 0;
	$sha1->add($buffer);
	$bytes_read += $amt;
    }
    die "Size mismatch?? $size != $bytes_read" 
	unless $size == $bytes_read;
    $fh->close();

    my $file_digest = $sha1->digest();
    
    my $rounded_size = $n * POSIX::ceil($size/$n);
    my $under_size = $rounded_size - $size;
    die "??" unless $under_size >= 0 && $under_size < 256;

    my $sha1_crosschunk = new Digest::SHA1;
    my $sha1_filehash = new Digest::SHA1;
    my $crosschunk_hash;
    for(my $i=0; $i < @$files; ++$i) {
	my $tmp = verifyFile($i, $under_size, $rounded_size / $n, $n, $m, 
			     $file_digest, $files->[$i], $sha1_crosschunk, $sha1_filehash);
	$crosschunk_hash = $tmp unless defined $crosschunk_hash;
	die "Crosschunk hash mismatch" unless $crosschunk_hash eq $tmp;
    }

    my $file_digest_over_chunks = $sha1_filehash->digest();
    die "Mismatch between digest calculated over file ecc chunks and file digest: " .
	unpack("H*",$file_digest_over_chunks) . " != " . unpack("H*",$file_digest)
	unless $file_digest eq $file_digest_over_chunks;
    my $crosschunk_digest_over_chunks = $sha1_crosschunk->digest();
    die "Mismatch between crosschunk_digest calculated over chunks and stored digest: " .
	unpack("H*",$crosschunk_digest_over_chunks) . " != " . unpack("H*",$crosschunk_hash)
	unless $crosschunk_digest_over_chunks eq $crosschunk_hash;
    
    return if $verify_level == 0;

    for(my $remove_start = 0; $remove_start < $n; ++$remove_start) {
	my @recover_from = @$files;
	for (my $j = $remove_start; $j < $remove_start + $m; ++$j) {
	    $recover_from[$j] = undef;
	}
	print "verifyRecover(skip $remove_start for $m)\n"
	    if $GLOBAL::debug;
	verifyRecover($threadid, $decodedir, \@recover_from, $size, $file_digest);
    }
}

sub verifyFile {
    my($chunknum, $under_size, $chunk_size, $n, $m, $file_digest, $chunkname,
       $sha1_crosschunk, $sha1_filehash) = @_;

    print "    verifyFile($chunknum, $chunkname)..." if $GLOBAL::debug;
    my $fh = new FileHandle($chunkname) 
	or die "Unable to open $chunkname for read: $!";
    
    my $header;
    my $amt = sysread($fh, $header, 4+3*20);

    die "read bad" unless 4+3*20 == $amt;
    my($version, $f_under_size, $f_info) = unpack("CCn", $header);
    
    die "Bad version $version != 1" 
	unless 1 == $version;
    die "Bad under size $f_under_size != $under_size" 
	unless $f_under_size == $under_size;

    my $f_n = $f_info >> 11;
    my $f_m = ($f_info >> 6) & 0x1F;
    my $f_chunknum = $f_info & 0x3F;
    
    die "Bad file_n $f_n != $n" 
	unless $f_n == $n;
    die "Bad file_m $f_m != $m"
	unless $f_m == $m;
    confess "Bad file_chunknum $f_chunknum != $chunknum from $chunkname"
	unless $f_chunknum == $chunknum;

    my $f_file_digest = substr($header, 4, 20);
    die "Bad file hash " . unpack("H*",$f_file_digest) . " != " . unpack("H*", $file_digest)
	unless $f_file_digest eq $file_digest;
    my $f_crosschunk_hash = substr($header, 4+20, 20);
    my $f_chunk_digest = substr($header, 4+2*20, 20);

    my $sha1 = new Digest::SHA1;

    my $bytes_read = 0;
    my $filesize = $n * $chunk_size - $under_size;
    my $filedata_remain = $filesize - $chunknum * $chunk_size;
    while (1) {
	my $buffer;
	$amt = sysread($fh, $buffer, 262144);
	die "Read failed: $!" unless defined $amt && $amt >= 0;
	last if $amt == 0;
	$sha1->add($buffer);
	if ($filedata_remain > length($buffer)) {
	    $sha1_filehash->add($buffer);
	} elsif ($filedata_remain > 0) {
	    $sha1_filehash->add(substr($buffer, 0, $filedata_remain));
	} else {
	    # ignore, not part of file data...
	}
	$bytes_read += $amt;
	$filedata_remain -= $amt;
    }
    die "Didn't get proper number of bytes from reading chunk; $bytes_read != $chunk_size"
	unless $bytes_read == $chunk_size;

    my $filechunk_digest = $sha1->digest();
    
    $sha1->reset();
    $sha1->add(substr($header, 0, 4+2*20), $filechunk_digest);
    my $chunk_digest = $sha1->digest();
    die "Bad chunk hash " . unpack("H*",$f_chunk_digest) . " != " . unpack("H*",$chunk_digest)
	unless $f_chunk_digest eq $chunk_digest;
    print "ok\n" if $GLOBAL::debug;

    $sha1_crosschunk->add(substr($header, 0, 4+20), $filechunk_digest);

    return $f_crosschunk_hash;
}

sub verifyRecover {
    my($threadid, $decodedir, $recover_from, $expected_size, $file_digest) = @_;

    for(my $i=0;$i<@$recover_from; ++$i) {
	my $file = $recover_from->[$i];
	next unless defined $file;
	die "$file doesn't exist??" unless -f $file;
	my $target = sprintf("%s/decode-t$threadid-%04d.rs",$decodedir, $i);
	die "$target already exists" if -e $target || -l $target;
	symlink($file, $target) 
	    || die "Unable to symlink $file to $target";
    }
    my $fh = new FileHandle "$rs_decode_file $decodedir/decode-t$threadid 2>/dev/null |"
	or die "Can't run $rs_decode_file $decodedir/decode: $!";
    my $sha1 = Digest::SHA1->new;
    my $bytes_read = 0;
    while (1) {
	my $buffer;
	my $amt = sysread($fh, $buffer, 262144);
	die "Read failed: $!" unless defined $amt && $amt >= 0;
	last if $amt == 0;
	$sha1->add($buffer);
	$bytes_read += $amt;
    }
    close($fh)
	or die "close failed: $!";
    die "exit code of '$rs_decode_file $decodedir/decode' not 0" 
	unless $? == 0;
    die "Unexpected size of decode: $bytes_read != $expected_size"
	unless $bytes_read == $expected_size;
    my $digest = $sha1->digest();
    die "Invalid digest " . unpack("H*", $file_digest) . " != " . unpack("H*", $digest)
	unless $file_digest eq $digest;

    for(my $i=0;$i<@$recover_from; ++$i) {
	my $file = $recover_from->[$i];
	next unless defined $file;
	my $target = sprintf("%s/decode-t$threadid-%04d.rs",$decodedir, $i);
	unlink($target)
	    or die "Unable to unlink $target: $!";
    }
}

sub pickMostFree {
    my ($from_dirs, $count) = @_;

    return () if $count == 0;
    my %freespace;
    map { 
	my ($bsize, $frsize, $blocks, $bfree, $bavail,
	    $files, $ffree, $favail, $flag, $namemax) = statvfs($_);
	$freespace{$_} = $bavail * $bsize;
    } @$from_dirs;
    my @sorted = sort { $freespace{$b} <=> $freespace{$a} } @$from_dirs;
    return @sorted[0 .. $count - 1];
}

sub selectEccDirs {
    my ($n, $m) = @_;

    die "??" unless $n + $m <= @eccdirs;

    my @parity_only = grep(/parity-only/o, @eccdirs);
    my @anydata = grep(!/parity-only/o, @eccdirs);

    die "need more data dirs, too many parity-only for ($n,$m)" 
	unless $n <= @eccdirs - @parity_only;

    my $parity_only = $m > @parity_only ? @parity_only : $m;

    my @parity_dirs = pickMostFree(\@parity_only, $parity_only);

    my @remain_dirs = pickMostFree(\@anydata, $n + $m - $parity_only);

    return (@remain_dirs, @parity_dirs);
}

sub determineNM {
    my ($filename) = @_;

    return (3,2) if $filename =~ m!/1ds2-dcim/!o;
    return (1,4) if $filename =~ m!/eric-good/psd/!o;
    return (2,3) if $filename =~ /\.psd$/o;
    return (3,1);
}

# undef on failure, lock fh on success
sub getlock {
    my($filename, $waittime) = @_;

    my $fh = new FileHandle "+>>$filename"
	or die "Unable to open $filename for append: $!";
    
    if (defined $waittime) {
	my $start = time;
	while(1) {
	    my $ret = flock($fh,LOCK_EX|LOCK_NB);
	    unless(defined $ret) {
		die "flock failed: $!";
	    }
	    last if $ret;

	    my $remain = $waittime - (time - $start);
	    print STDERR "delayed waiting for lock, $remain seconds remain...\n";
	    return undef
		if (time - $start) >= $waittime;
	    sleep(1);
	}
    } else {
	unless(flock($fh,LOCK_EX)) {
	    die "flock failed: $!";
	}
    }
    my $now = localtime(time);
    print $fh "Locked by $$ at $now\n";
    $fh->flush();
    return $fh;
}

sub setupFilesUnder {
    my($files_under, $base_dir, $importdir) = @_;

    $base_dir = "/$base_dir" unless $base_dir =~ m!^/!o;
    die "--path argument needs to be absolute"
	unless $files_under =~ m!^/!o;
    opendir(DIR,"$importdir") or die "bad";
    while(my $file = readdir(DIR)) {
	next if $file eq '.' || $file eq '..';
	die "--path is not allowed if there are already files in $importdir";
    }
    closedir(DIR);
    
    mkpath("$importdir/$base_dir")
	or die "Can't mkdirpath $importdir/$base_dir";
    
    find(sub {
	die "no symlinks $File::Find::name" 
	    if -l $_;
	die "?? $File::Find::name $files_under"
	    unless substr($File::Find::name, 0, length $files_under) eq $files_under;
	my $relpath = substr($File::Find::name, length $files_under);
	return if $relpath eq '';
	my $dest = "$importdir$base_dir$relpath";
	if (-d $_) {
	    mkdir($dest, 0777) 
		or die "Unable to mkdir $dest: $!";
	} elsif (-f $_) {
	    symlink($File::Find::name, $dest)
		or die "Can't symlink $File::Find::name to $dest: $!";
	} else {
	    die "?? $File::Find::name";
	}
    }, $files_under);
}


