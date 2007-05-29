LINTEL_DIR := /home/anderse/build/optimize
CFLAGS := -D_FILE_OFFSET_BITS=64 -D_REENTRANT -DFUSE_USE_VERSION=25 -Wall -g -I/opt/fuse/include  -I$(LINTEL_DIR)/include -I/home/anderse/projects/ticoli/simulator/boost_foreach
CXXFLAGS := $(CFLAGS)

eccfs: eccfs.o struct_def.o
	g++ -o eccfs -L/opt/fuse/lib -L$(LINTEL_DIR)/lib eccfs.o struct_def.o -lfuse -lLintel -lcrypto -lpthread -Wl,--rpath -Wl,$(LINTEL_DIR)/lib -Wl,--rpath -Wl,/opt/fuse/lib 

run: eccfs
	[ -d /tmp/import ] || mkdir /tmp/import
	[ -d /tmp/ecc1 ] || mkdir /tmp/ecc1
	[ -d /tmp/ecc2 ] || mkdir /tmp/ecc2
	[ -d /tmp/ecc3 ] || mkdir /tmp/ecc3
	[ -d /tmp/ecc4 ] || mkdir /tmp/ecc4
#	[ -f /tmp/ecc1/libfuse.so.2.5.2 ] || cp tmp/a-0003.rs /tmp/ecc1/libfuse.so.2.5.2
#	[ -f /tmp/ecc1/libfuse.so.2.5.2 ] || cp tmp/a-0002.rs /tmp/ecc2/libfuse.so.2.5.2
#	[ -f /tmp/ecc1/libfuse.so.2.5.2 ] || cp tmp/a-0001.rs /tmp/ecc3/libfuse.so.2.5.2
#	[ -f /tmp/ecc1/libfuse.so.2.5.2 ] || cp tmp/a-0000.rs /tmp/ecc4/libfuse.so.2.5.2
	./eccfs -d --eccdirs=/tmp/ecc1,/tmp/ecc2,/tmp/ecc3,/tmp/ecc4 --importdir=/tmp/import /mnt/tmp

eric-home: eccfs
	./eccfs -d --eccdirs=/mnt/backup-1/eccfs,/mnt/backup-2/eccfs,/mnt/backup-3/eccfs,/mnt/backup-4/eccfs,/mnt/parity-only-1/eccfs,/mnt/parity-only-2/eccfs --importdir=/tmp/import /mnt/eccfs

