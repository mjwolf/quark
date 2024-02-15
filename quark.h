#ifndef _QUARK_H_
#define _QUARK_H_

/* Linux specific */
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

/* Sys */
#include <sys/param.h>		/* MAXPATHLEN */

/* Compat, tree.h, queue.h */
#include "compat.h"

/* btf.c */
int	quark_btf_init(void);
ssize_t	quark_btf_offset(const char *);

/* qutil.c */
struct qstr {
	char	*p;
	char	 small[64];
};

struct perf_record_sample;
struct perf_sample_data_loc;

ssize_t	qread(int, void *, size_t);
int	qwrite(int, const void *, size_t);
void	qstr_init(struct qstr *);
int	qstr_ensure(struct qstr *, size_t);
int	qstr_copy_data_loc(struct qstr *, struct perf_record_sample *,
    struct perf_sample_data_loc *);
void	qstr_free(struct qstr *);
/*
 * Time helpers
 */
#ifndef NS_PER_S
#define NS_PER_S	1000000000L
#endif /* NS_PER_S */

#ifndef NS_PER_MS
#define NS_PER_MS	1000000L
#endif /* NS_PER_MS */

#ifndef MS_TO_NS
#define MS_TO_NS(_x)	((_x) * NS_PER_MS)
#endif /* MS_TO_NS */

/*
 * Perf related declarations
 */
struct perf_sample_id {
	u32	pid;
	u32	tid;
	u64	time;		/* See raw_evenr_insert() */
	u64	stream_id;	/* We can likely get rid of this */
	u32	cpu;
	u32	cpu_unused;
};

struct perf_record_fork {
	struct perf_event_header	header;
	u32				pid;
	u32				ppid;
	u32				tid;
	u32				ptid;
	u64				time;
	struct perf_sample_id		sample_id;
};

struct perf_record_exit {
	struct perf_event_header	header;
	u32				pid;
	u32				ppid;
	u32				tid;
	u32				ptid;
	u64				time;
	struct perf_sample_id		sample_id;
};

/*
 * Kernels might actually have a different common area, so far we only
 * need common_type, so hold onto that
 */
struct perf_sample_data_hdr {
	/* this is the actual id from tracefs eg: sched_process_exec/id */
	u16	 common_type;
	/* ... */
};

struct perf_sample_data_loc {
	u16	offset;
	u16	size;
};

struct perf_record_sample {
	struct perf_event_header	header;
	struct perf_sample_id		sample_id;
	u32				data_size;
	char				data[];
};

struct perf_event {
	union {
		struct perf_event_header	header;
		struct perf_record_fork		fork;
		struct perf_record_exit		exit;
		struct perf_record_sample	sample;
	};
};

struct perf_mmap {
	struct perf_event_mmap_page	*metadata;
	size_t				 mapped_size;
	size_t				 data_size;
	size_t				 data_mask;
	u8				*data_start;
	u64				 data_tmp_tail;
	u8				 wrapped_event_buf[4096] __aligned(8);
};

struct perf_group_leader {
	TAILQ_ENTRY(perf_group_leader)	 entry;
	int				 fd;
	int				 cpu;
	struct perf_event_attr		 attr;
	struct perf_mmap		 mmap;
};

/*
 * Quark sample formats
 */
enum sample_kinds {
	EXEC_SAMPLE = 1,
	WAKE_UP_NEW_TASK_SAMPLE,
	EXIT_THREAD_SAMPLE,
};

struct exec_sample {
	struct perf_sample_data_loc	filename;
	s32				pid;
	s32				old_pid;
};

/* 8 byte aligned, we have to be careful with padding still */
struct task_sample {
	u64	probe_ip;
	u32	uid;
	u32	gid;
	u32	suid;
	u32	sgid;
	u32	euid;
	u32	egid;
	u64	cap_inheritable;
	u64	cap_permitted;
	u64	cap_effective;
	u64	cap_bset;
	u64	cap_ambient;
	u32	pid;
	u32	tid;
	u64	start_time;
	u64	start_boottime;
	s32	exit_code;
};

/*
 * Kprobe related declarations
 */
struct kprobe_arg {
	const char	*name;
	const char	*reg;
	const char	*typ;
	const char	*arg_dsl;
};

struct kprobe {
	const char		*name;
	const char		*target;
	int			 sample_kind;
	int			 is_kret;
	struct kprobe_arg	 args[];
};

struct kprobe_state {
	TAILQ_ENTRY(kprobe_state)	 entry;
	struct kprobe			*k;
	struct perf_event_attr		 attr;
	int				 fd;
	int				 cpu;
	int				 group_fd;
};

/*
 * Raw events
 */
enum {
	RAW_FORK = 1,
	RAW_EXEC,
	RAW_EXIT,
	RAW_WAKE_UP_NEW_TASK,
	RAW_EXIT_THREAD
};

struct raw_exec {
	struct qstr		filename;
};

struct raw_fork {
	u32			parent_pid;
	u32			child_pid;
};

struct raw_event {
	RB_ENTRY(raw_event)			entry_by_time;
	RB_ENTRY(raw_event)			entry_by_pidtime;
	TAILQ_HEAD(agg_queue, raw_event)	agg_queue;
	TAILQ_ENTRY(raw_event)			agg_entry;
	u32					opid;
	u32					pid;
	u32					tid;
	u32					cpu;
	u64					time;
	int					type;
	union {
		struct raw_exec			exec;
		struct raw_fork			fork;
		struct task_sample		task_sample;
	};
};

/*
 * Raw Event Tree by time, where RB_MIN() is the oldest element in the tree, no
 * clustering of pids so we can easily get the oldest event.
 */
RB_HEAD(raw_event_by_time, raw_event);

/*
 * Raw Event Tree by pid and time, this creates clusters of the same pid which
 * are then organized by time, this is used in assembly and aggregation, if we
 * used the 'by_time' tree, we would have to traverse the full tree in case of a
 * miss.
 */
/* XXX this should be by tid, but we're not there yet XXX */
RB_HEAD(raw_event_by_pidtime, raw_event);

/*
 * List of all ring buffer leaders, we have on per cpu.
 */
TAILQ_HEAD(perf_group_leaders, perf_group_leader);

/*
 * List of online kprobes.
 */
TAILQ_HEAD(kprobe_states, kprobe_state);

struct quark_queue_stats {
	u64	insertions;
	u64	removals;
	u64	aggregations;
	u64	non_aggregations;
	/* TODO u64	peak_nodes; */
};
/*
 * Quark Queue (qq) is the main structure the user interacts with, it acts as
 * our main storage datastructure.
 */
struct quark_queue {
	struct perf_group_leaders	perf_group_leaders;
	struct kprobe_states		kprobe_states;
	struct raw_event_by_time	raw_event_by_time;
	struct raw_event_by_pidtime	raw_event_by_pidtime;
	struct quark_queue_stats	stats;
#define QQ_THREAD_EVENTS		(1 << 0)
#define QQ_PERF_TASK_EVENTS		(1 << 1)
	int				flags;
};

#endif /* _QUARK_H_ */
