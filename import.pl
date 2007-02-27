#!/usr/bin/perl -w
use strict;
use File::Find;
use POSIX;
use Digest::SHA1;
use MIME::Base64;
use FileHandle;
use File::Copy;
use File::Compare;

use lib "$ENV{HOME}/projects/eccfs";
use EccFS;

$|=1;
$GLOBAL::debug = 1;

# Notes: $Global::fixup{subname} defines the rules for fixing up
# conflicts between files and directories.  This is so that the user
# only has to specify the fixup rule once.

usage("missing arguments.")
    unless @ARGV == 1 && -d $ARGV[0];

my $eccfsdir = $ARGV[0];

my($importdir,@eccdirs);
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

usage("importdir not a dir") unless -d $importdir;
map { usage("eccdir $_ not a dir") unless -d $_; } @eccdirs;

my $rs_encode_file = "$ENV{HOME}/projects/eccfs/gflib/rs_encode_file";
die "$rs_encode_file not executable" unless -x $rs_encode_file;

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

system("sync") == 0 
    or die "sync failed: $!";

foreach my $subname (sort keys %Global::reverify_directories) {
    foreach my $eccdir (@eccdirs) {
	die "??" unless -d "$eccdir/$subname";
    }
}

foreach my $subname (sort keys %Global::reverify_files) {
    my ($eccusedirs, $n, $m) = @{$Global::reverify_files{$subname}};
    my %inuse;
    my @eccfiles = map { $inuse{$_} = 1; "$_/$subname" } @$eccusedirs;
    foreach my $eccdir (@eccdirs) {
	next if $inuse{$eccdir};
	die "Incorrectly still existing file $eccdir/$subname"
	    if -f "$eccdir/$subname";
    }
    verifyEccSplitup($decodedir, "$importdir/$subname", \@eccfiles, $n, $m, 0);

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

exit(0);

sub wanted {
    my $subname = $File::Find::name;
    $subname =~ s!^$importdir!!o or die "?? $File::Find::name";
    $subname =~ s!^/+!!o;
    print "Wanted ($File::Find::name) -> $subname\n" if $GLOBAL::debug;
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

    print "  handleFile($subname)\n" if $GLOBAL::debug;

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
    verifyEccSplitup($decodedir, "$importdir/$subname", \@eccfiles, $n, $m, 1);

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
	# this is just unlinking the temporary ecc working stuff ...
	unlink($from) or die "Unable to unlink $from: $!";
    }
    foreach my $eccdir (@eccdirs) {
	my $tmp = new FileHandle("+<$eccdir/$subname")
	    or die "bad $eccdir/$subname: $!";
	my $tmp2 = new IO::Handle;
	$tmp2->fdopen(fileno($tmp),"w");
	$tmp2->sync() or die "bad: $!";
	$tmp->close();
    }

    die "internal $i != $n + $m" unless $i == $n + $m;
    $Global::reverify_files{$subname} = [\@eccusedirs, $n, $m];
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
    die "$_[0]\nUsage: $0 <eccfs-mount-point>"
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

