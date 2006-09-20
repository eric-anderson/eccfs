#!/usr/bin/perl -w
use strict;
use File::Find;
use POSIX;
use Digest::SHA1;
use MIME::Base64;
use FileHandle;
use File::Copy;

$|=1;
my $debug = 1;

# Notes: $Global::fixup{subname} defines the rules for fixing up
# conflicts between files and directories.  This is so that the user
# only has to specify the fixup rule once.

usage("missing arguments.")
    unless @ARGV >= 3;

my($importdir,@eccdirs) = @ARGV;

usage("importdir not a dir") unless -d $importdir;
map { usage("eccdir $_ not a dir") unless -d $_; } @eccdirs;

my $rs_encode_file = "$ENV{HOME}/rs_encode_file";
die "$rs_encode_file not executable" unless -x $rs_encode_file;
my $rs_decode_file = "$ENV{HOME}/rs_decode_file";
die "$rs_decode_file not executable" unless -x $rs_decode_file;

my $workbase = "/tmp/workdir";
unless (-d $workbase) {
    mkdir($workbase, 0770) or die "Can't mkdir $workbase: $!";
}
my $encodedir = "$workbase/encode";
mkdir($encodedir, 0770) or die "Can't mkdir $encodedir: $!";
my $decodedir = "$workbase/decode";
mkdir($decodedir, 0770) or die "Can't mkdir $decodedir: $!";
find(\&wanted, $importdir);
rmdir($encodedir) or die "Can't rmdir $encodedir: $!";
rmdir($decodedir) or die "Can't rmdir $decodedir: $!";

print "TODO: reverify files after copy to ecc dirs, verify proper decode through eccfs, and clear out import...\n";
%Global::reverify_directories if 0;
%Global::reverify_files if 0;
exit(0);

sub wanted {
    my $subname = $File::Find::name;
    $subname =~ s!^$importdir!!o or die "?? $File::Find::name";
    $subname =~ s!^/+!!o;
    print "Wanted ($File::Find::name) -> $subname\n" if $debug;
    if (-d $File::Find::name) {
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
    $Global::reverify_directories{$subname} = 1;
}

sub handlefile {
    my($subname) = @_;

    print "  handleFile($subname)\n" if $debug;

    my ($n,$m) = determineNM($subname);
    my $max = @eccdirs;
    die "Unable to import $subname, should be broken into $n data and $m parity pieces, but only $max places available"
	unless $n + $m <= $max;

    my $import_size = -s "$importdir/$subname";
    my @eccusedirs = selectEccDirs($n + $m);
    die "huh" . scalar @eccusedirs unless @eccusedirs == $n + $m;
    my $q_subname = quotemeta($subname);
    my $ret = system("$rs_encode_file $importdir/$q_subname $n $m $encodedir/ecc");
    die "Encoding of $subname failed?"
	unless $ret == 0;
    
    my $eccsize = 4+20+20 + POSIX::ceil($import_size / $n); # header + datasize
    my @eccfiles = map { sprintf("%s/ecc-%04d.rs", $encodedir, $_) } (0 .. $n+$m - 1);
    verifyEccSplitup("$importdir/$subname", \@eccfiles, $n, $m);

    # Don't have to worry about parent directories as they would already have been processed by
    # handledir when handling importing of the parent

    foreach my $eccdir (@eccdirs) {
	if (-f "$eccdir/$subname") {
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

    my %selected;
    map { $selected{$_} = 1 } @eccusedirs;
    my $i = 0;
    foreach my $eccdir (@eccdirs) {
	next unless $selected{$eccdir};
	my $from = sprintf("%s/ecc-%04d.rs", $encodedir, $i);
	copy($from, "$eccdir/$subname")
	    or die "Unable to copy $from to $eccdir/$subname: $!";
	++$i;
	unlink($from) or die "Unable to unlink $from: $!";
    }
    die "internal $i != $n + $m" unless $i == $n + $m;
    $Global::reverify_files{$subname} = \@eccusedirs;
}

sub getFixup {
    my($subname, $msg) = @_;

    while (! defined $Global::fixup{$subname}) {
	print "$msg\n";
	print "what do you want to do with existing $subname: abort, delete, or rename [abort]?";
	my $choice = <STDIN>;
	chomp;
	$choice = 'abort' if $choice eq '';
	if ($choice =~ /^a(bort)?$/io) {
	    exit(1);
	} elsif ($choice =~ /^d(elete)?$/io) {
	    $Global::fixup{$subname} = ['delete'];
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
		$Global::fixup{$subname} = ['rename',$rename_to];
	    }
	} else {
	    die "Unrecognized choice '$choice'";
	}
    } 

    print "Chose to $Global::fixup{$subname}->[0] $subname\n";

    return $Global::fixup{$subname};
}

sub existsAnyEcc {
    my($subname) = @_;

    foreach my $eccdir (@eccdirs) {
	return 1 if -e "$eccdir/$subname";
    }
    return 0;
}


sub usage {
    die "$_[0]\nUsage: $0 <importdir> <eccdirs...>"
}

sub determineNM {
    my ($filename) = @_;

    return (2,2);
}

sub selectEccDirs {
    my ($ndirs) = @_;

    die "??" unless $ndirs <= @eccdirs;
    return @eccdirs[0 .. $ndirs-1];
}

sub verifyEccSplitup {
    my($dataname, $files, $n, $m) = @_;

    print "verifyEccSplitup($dataname, [ " . join(", ", @$files) . "], $n, $m)\n"
	if $debug;
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

    print "TODO: VERIFY that sha1 of chunks read together add up to sha1 of orig file\n";
    for(my $i=0; $i < @$files; ++$i) {
	verifyFile($i, $under_size, $rounded_size / $n, $n, $m, $file_digest, $files->[$i]);
    }

    for(my $remove_start = 0; $remove_start < $n; ++$remove_start) {
	my @recover_from = @$files;
	for (my $j = $remove_start; $j < $remove_start + $m; ++$j) {
	    $recover_from[$j] = undef;
	}
	print "verifyRecover(skip $remove_start for $m)\n"
	    if $debug;
	verifyRecover(\@recover_from, $size, $file_digest);
    }
}

sub verifyFile {
    my($chunknum, $under_size, $chunk_size, $n, $m, $file_digest, $chunkname) = @_;

    print "    verifyFile($chunknum, $chunkname)..." if $debug;
    my $fh = new FileHandle($chunkname) 
	or die "Unable to open $chunkname for read: $!";
    
    my $header;
    my $amt = sysread($fh, $header, 4+20+20);

    die "read bad" unless 4+20+20 == $amt;
    my($version, $f_under_size, $f_infoa, $f_infob) = unpack("CCCC", $header);
    
    die "Bad version $version != 1" 
	unless 1 == $version;
    die "Bad under size $f_under_size != $under_size" 
	unless $f_under_size == $under_size;

    my $f_n = $f_infoa & 0x1F;
    my $f_m = (($f_infoa >> 5) & 0x7) + (($f_infob & 0x3) << 3);
    my $f_chunknum = (($f_infob >> 2) & 0x3F);
    
    die "Bad file_n $f_n != $n" 
	unless $f_n == $n;
    die "Bad file_m $f_m != $m"
	unless $f_m == $m;
    die "Bad file_chunknum $f_chunknum != $chunknum"
	unless $f_chunknum == $chunknum;

    my $f_file_digest = substr($header,4,20);
    die "Bad file hash " . unpack("H*",$f_file_digest) . " != " . unpack("H*", $file_digest)
	unless $f_file_digest eq $file_digest;

    my $sha1 = new Digest::SHA1;

    $sha1->add(substr($header,0,4+20));
    my $bytes_read = 0;
    while (1) {
	my $buffer;
	$amt = sysread($fh, $buffer, 262144);
	die "Read failed: $!" unless defined $amt && $amt >= 0;
	last if $amt == 0;
	$sha1->add($buffer);
	$bytes_read += $amt;
    }
    die "Didn't get proper number of bytes from reading chunk; $bytes_read != $chunk_size"
	unless $bytes_read == $chunk_size;
    my $f_chunk_digest = substr($header, 4+20, 20);
    my $chunk_digest = $sha1->digest();
    die "Bad chunk hash " . unpack("H*",$f_chunk_digest) . " != " . unpack("H*",$chunk_digest)
	unless $f_chunk_digest eq $chunk_digest;
    print "ok\n" if $debug;
}

sub verifyRecover {
    my($recover_from, $expected_size, $file_digest) = @_;

    for(my $i=0;$i<@$recover_from; ++$i) {
	my $file = $recover_from->[$i];
	next unless defined $file;
	die "$file doesn't exist??" unless -f $file;
	my $target = sprintf("%s/decode-%04d.rs",$decodedir, $i);
	die "$target already exists" if -e $target || -l $target;
	symlink($file, $target) 
	    || die "Unable to symlink $file to $target";
    }
    my $fh = new FileHandle "$rs_decode_file $decodedir/decode 2>/dev/null |"
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
	my $target = sprintf("%s/decode-%04d.rs",$decodedir, $i);
	unlink($target)
	    or die "Unable to unlink $target: $!";
    }
}

