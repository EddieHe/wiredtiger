/*-
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/stat.h>
#ifndef _WIN32
#include <sys/time.h>
#endif
#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <time.h>

#ifdef _WIN32
#include "windows_shim.h"
#endif

#ifdef BDB
#include <db.h>
#endif
#include <wiredtiger.h>

#include <wiredtiger_ext.h>

extern WT_EXTENSION_API *wt_api;

#define	EXTPATH	"../../ext/"			/* Extensions path */

#define	BZIP_PATH							\
	EXTPATH "compressors/bzip2/.libs/libwiredtiger_bzip2.so"
#define	SNAPPY_PATH							\
	EXTPATH "compressors/snappy/.libs/libwiredtiger_snappy.so"
#define	ZLIB_PATH							\
	EXTPATH "compressors/zlib/.libs/libwiredtiger_zlib.so"

#define	REVERSE_PATH							\
	EXTPATH "collators/reverse/.libs/libwiredtiger_reverse_collator.so"

#define	KVS_BDB_PATH							\
	EXTPATH "test/kvs_bdb/.libs/libwiredtiger_kvs_bdb.so"
#define	HELIUM_PATH							\
	EXTPATH "datasources/helium/.libs/libwiredtiger_helium.so"

#define	LZO_PATH	".libs/lzo_compress.so"

#undef	M
#define	M(v)		((v) * 1000000)		/* Million */
#undef	MEGABYTE
#define	MEGABYTE(v)	((v) * 1048576)
#undef	GIGABYTE
#define	GIGABYTE(v)	((v) * 1073741824ULL)

/* Get a random value between a min/max pair. */
#define	MMRAND(min, max)	(rng() % (((max) + 1) - (min)) + (min))

#define	WT_NAME	"wt"				/* Object name */

#define	DATASOURCE(v)	(strcmp(v, g.c_data_source) == 0 ? 1 : 0)
#define	SINGLETHREADED	(g.c_threads == 1)

#ifndef _WIN32
#define	SIZET_FMT	"%zu"			/* size_t format string */
#else
#define	SIZET_FMT	"%Iu"			/* size_t format string */
#endif

typedef struct {
	char *progname;				/* Program name */

	char *home;				/* Home directory */
	char *home_backup;			/* Hot-backup directory */
	char *home_backup_init;			/* Initialize backup command */
	char *home_bdb;				/* BDB directory */
	char *home_config;			/* Run CONFIG file path */
	char *home_init;			/* Initialize home command */
	char *home_kvs;				/* KVS directory */
	char *home_log;				/* Operation log file path */
	char *home_rand;			/* RNG log file path */
	char *home_salvage_copy;		/* Salvage copy command */
	char *home_stats;			/* Statistics file path */

	char *helium_mount;			/* Helium volume */

	void *bdb;				/* BDB comparison handle */
	void *dbc;				/* BDB cursor handle */

	WT_CONNECTION	 *wts_conn;
	WT_EXTENSION_API *wt_api;

	int   rand_log_stop;			/* Logging turned off */
	FILE *rand_log;				/* Random number log */

	uint32_t run_cnt;			/* Run counter */

	enum {
	    LOG_FILE=1,				/* Use a log file */
	    LOG_OPS=2				/* Log all operations */
	} logging;
	FILE *logfp;				/* Log file */

	int replay;				/* Replaying a run. */
	int track;				/* Track progress */
	int threads_finished;			/* Operations completed */

	pthread_rwlock_t backup_lock;		/* Hot backup running */

	/*
	 * We have a list of records that are appended, but not yet "resolved",
	 * that is, we haven't yet incremented the g.rows value to reflect the
	 * new records.
	 */
	uint64_t *append;			/* Appended records */
	size_t    append_max;			/* Maximum unresolved records */
	size_t	  append_cnt;			/* Current unresolved records */
	pthread_rwlock_t append_lock;		/* Single-thread resolution */

	char *uri;				/* Object name */

	char *config_open;			/* Command-line configuration */

	uint32_t c_auto_throttle;		/* Config values */
	uint32_t c_backups;
	uint32_t c_bitcnt;
	uint32_t c_bloom;
	uint32_t c_bloom_bit_count;
	uint32_t c_bloom_hash_count;
	uint32_t c_bloom_oldest;
	uint32_t c_cache;
	uint32_t c_compact;
	uint32_t c_checkpoints;
	char	*c_checksum;
	uint32_t c_chunk_size;
	char	*c_compression;
	char	*c_config_open;
	uint32_t c_data_extend;
	char	*c_data_source;
	uint32_t c_delete_pct;
	uint32_t c_dictionary;
	uint32_t c_evict_max;
	uint32_t c_firstfit;
	char	*c_file_type;
	uint32_t c_huffman_key;
	uint32_t c_huffman_value;
	uint32_t c_insert_pct;
	uint32_t c_internal_key_truncation;
	uint32_t c_intl_page_max;
	char	*c_isolation;
	uint32_t c_key_gap;
	uint32_t c_key_max;
	uint32_t c_key_min;
	uint32_t c_leaf_page_max;
	uint32_t c_leak_memory;
	uint32_t c_logging;
	uint32_t c_lsm_worker_threads;
	uint32_t c_merge_max;
	uint32_t c_mmap;
	uint32_t c_ops;
	uint32_t c_prefix_compression;
	uint32_t c_prefix_compression_min;
	uint32_t c_repeat_data_pct;
	uint32_t c_reverse;
	uint32_t c_rows;
	uint32_t c_runs;
	uint32_t c_split_pct;
	uint32_t c_statistics;
	uint32_t c_threads;
	uint32_t c_value_max;
	uint32_t c_value_min;
	uint32_t c_write_pct;

#define	FIX				1	
#define	ROW				2
#define	VAR				3
	u_int type;				/* File type's flag value */

#define	CHECKSUM_OFF			1
#define	CHECKSUM_ON			2
#define	CHECKSUM_UNCOMPRESSED		3
	u_int c_checksum_flag;			/* Checksum flag value */

#define	COMPRESS_NONE			1
#define	COMPRESS_BZIP			2
#define	COMPRESS_BZIP_RAW		3
#define	COMPRESS_LZO			4
#define	COMPRESS_SNAPPY			5
#define	COMPRESS_ZLIB			6
#define	COMPRESS_ZLIB_NO_RAW		7
	u_int c_compression_flag;		/* Compression flag value */

#define	ISOLATION_RANDOM		1
#define	ISOLATION_READ_UNCOMMITTED	2
#define	ISOLATION_READ_COMMITTED	3
#define	ISOLATION_SNAPSHOT		4
	u_int c_isolation_flag;			/* Isolation flag value */

	uint64_t key_cnt;			/* Keys loaded so far */
	uint64_t rows;				/* Total rows */

	uint32_t key_rand_len[1031];		/* Key lengths */
} GLOBAL;
extern GLOBAL g;

typedef struct {
	uint64_t search;			/* operations */
	uint64_t insert;
	uint64_t update;
	uint64_t remove;

	uint64_t commit;			/* transaction resolution */
	uint64_t rollback;
	uint64_t deadlock;

	int       id;				/* simple thread ID */
	pthread_t tid;				/* thread ID */

#define	TINFO_RUNNING	1			/* Running */
#define	TINFO_COMPLETE	2			/* Finished */
#define	TINFO_JOINED	3			/* Resolved */
	volatile int state;			/* state */
} TINFO;

void	 bdb_close(void);
void	 bdb_insert(const void *, size_t, const void *, size_t);
void	 bdb_np(int, void *, size_t *, void *, size_t *, int *);
void	 bdb_open(void);
void	 bdb_read(uint64_t, void *, size_t *, int *);
void	 bdb_remove(uint64_t, int *);
void	 bdb_update(const void *, size_t, const void *, size_t, int *);

void	*backup(void *);
void	*compact(void *);
void	 config_clear(void);
void	 config_error(void);
void	 config_file(const char *);
void	 config_print(int);
void	 config_setup(void);
void	 config_single(const char *, int);
void	 die(int, const char *, ...);
void	 key_len_setup(void);
void	 key_gen_setup(uint8_t **);
void	 key_gen(uint8_t *, size_t *, uint64_t, int);
void	 path_setup(const char *);
uint32_t rng(void);
void	 rng_init(void);
void	 track(const char *, uint64_t, TINFO *);
void	 val_gen_setup(uint8_t **);
void	 value_gen(uint8_t *, size_t *, uint64_t);
void	 wts_close(void);
void	 wts_create(void);
void	 wts_dump(const char *, int);
void	 wts_load(void);
void	 wts_open(const char *, int, WT_CONNECTION **);
void	 wts_ops(void);
void	 wts_read_scan(void);
void	 wts_salvage(void);
void	 wts_stats(void);
void	 wts_verify(const char *);
