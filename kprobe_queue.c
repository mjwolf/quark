#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "quark.h"

#define PERF_MMAP_PAGES		16		/* Must be power of 2 */
#define MAX_SAMPLE_IDS		4096		/* id_to_sample_kind map */

/* matches each sample event to a kind like EXEC_SAMPLE, FOO_SAMPLE */
u8	id_to_sample_kind[MAX_SAMPLE_IDS];
/*
 * This is the offset from the common area of a probe to the body. It is
 * almost always 8, but some older redhat kernels are different.
 */
ssize_t	probe_data_body_offset = -1;
int	kprobe_queue_refs;

static int	kprobe_queue_populate(struct quark_queue *);
static void	kprobe_queue_close(struct quark_queue *);

struct quark_queue_ops queue_ops_kprobe = {
	.open	  = kprobe_queue_open,
	.populate = kprobe_queue_populate,
	.close	  = kprobe_queue_close,
};

static char *
str_of_dataloc(struct perf_record_sample *sample,
    struct perf_sample_data_loc *data_loc)
{
	return (sample->data + data_loc->offset);
}

#if 0
/*
 * Copies out the string pointed to by data size, if retval is >= than dst_size,
 * it means we truncated. May return -1 on bad values.
 */
static ssize_t
strlcpy_data_loc(void *dst, ssize_t dst_size,
    struct perf_record_sample *sample, struct perf_sample_data_loc *data_loc)
{
	ssize_t				 n;
	char				*p = dst, *data;

	p = dst;
	n = min(dst_size, data_loc->size);
	if (n <= 0)
		return (-1);
	data = sample->data;
	memcpy(p, data + data_loc->offset, n);
	/* never trust the kernel */
	p[n - 1] = 0;

	return (n - 1);
}
#endif
static inline int
sample_kind_of_id(int id)
{
	if (unlikely(id <= 0 || id >= MAX_SAMPLE_IDS)) {
		warnx("%s: invalid id %d", __func__, id);
		return (errno = ERANGE, -1);
	}

	return (id_to_sample_kind[id]);
}

static inline void *
sample_data_body(struct perf_record_sample *sample)
{
	return (sample->data + probe_data_body_offset);
}

static inline int
sample_data_id(struct perf_record_sample *sample)
{
	struct perf_sample_data_hdr *h = (struct perf_sample_data_hdr *)sample->data;
	return (h->common_type);
}
#if 0
static inline int
sample_kind(struct perf_record_sample *sample)
{
	return (sample_kind_of_id(sample_data_id(sample)));
}
#endif

static int
build_path(struct path_ctx *ctx, struct qstr *dst)
{
	int	 i, done;
	char	*p, *pwd, *ppwd, path[MAXPATHLEN];
	u64	 pwd_k;

	p = &path[sizeof(path) - 1];
	*p = 0;
	done = 0;
	for (i = 0; i < (int)nitems(ctx->pwd) && !done; i++) {
		pwd_k = ctx->pwd[i].pwd_k;
		pwd = ctx->pwd[i].pwd;
		if (pwd_k == ctx->root_k)
			break;
		if (pwd_k == ctx->mnt_root_k) {
			pwd = ctx->mnt_mountpoint;
			done = 1;
		}
		/* XXX this strlen sucks as we had the length on the wire */
		ppwd = pwd + strlen(pwd);
		/* +1 is the / */
		/* XXX this is way too dangerous XXX */
		if (((ppwd - pwd) + 1) > (p - path))
			return (errno = ENAMETOOLONG, -1);
		while (ppwd != pwd)
			*--p = *--ppwd;
		*--p = '/';
	}
	if (*p == 0)
		*--p = '/';

	/* XXX double copy XXX */
	return (qstr_strcpy(dst, p));
}

static struct raw_event *
perf_sample_to_raw(struct quark_queue *qq, struct perf_record_sample *sample)
{
	int			 id, kind;
	ssize_t			 n;
	struct raw_event	*raw = NULL;

	id = sample_data_id(sample);
	kind = sample_kind_of_id(id);

	switch (kind) {
	case EXEC_SAMPLE: {
		struct exec_sample *exec = sample_data_body(sample);
		if ((raw = raw_event_alloc()) == NULL)
			return (NULL);
		raw->type = RAW_EXEC;
		qstr_init(&raw->exec.filename);
		n = qstr_copy_data_loc(&raw->exec.filename, sample, &exec->filename);
		if (n == -1)
			warnx("can't copy exec filename");
		break;
	}
	case WAKE_UP_NEW_TASK_SAMPLE: /* FALLTHROUGH */
	case EXIT_THREAD_SAMPLE: {
		struct task_sample	*w = sample_data_body(sample);
		struct path_ctx		 pctx;
		int			 i;
		/*
		 * ev->sample.sample_id.pid is the parent, if the new task has
		 * the same pid as it, then this is a thread event
		 */
		if ((qq->flags & QQ_THREAD_EVENTS) == 0
		    && w->pid != w->tid)
			return (NULL);
		if ((raw = raw_event_alloc()) == NULL)
			return (NULL);
		if (kind == WAKE_UP_NEW_TASK_SAMPLE) {
			raw->type = RAW_WAKE_UP_NEW_TASK;
			/*
			 * Cheat, make this look like a child event.
			 */
			raw->pid = w->pid;
			raw->tid = w->tid;
			raw->task.ppid = sample->sample_id.pid;
			pctx.root = str_of_dataloc(sample, &w->root_s);
			pctx.root_k = w->root_k;
			pctx.mnt_root = str_of_dataloc(sample, &w->mnt_root_s);
			pctx.mnt_root_k = w->mnt_root_k;
			pctx.mnt_mountpoint = str_of_dataloc(sample,
			    &w->mnt_mountpoint_s);
			pctx.mnt_mountpoint_k = w->mnt_mountpoint_k;
			for (i = 0; i < (int)nitems(pctx.pwd); i++) {
				pctx.pwd[i].pwd = str_of_dataloc(sample,
				    &w->pwd_s[i]);
				pctx.pwd[i].pwd_k = w->pwd_k[i];
			}
			qstr_init(&raw->task.cwd);
			if (build_path(&pctx, &raw->task.cwd) == -1)
				warn("can't build path");
			raw->task.exit_code = -1;
			raw->task.exit_time_event = 0;
		} else {
			raw->type = RAW_EXIT_THREAD;
			/*
			 * We derive ppid from the incoming sample header as
			 * it's originally an event of the parent, since exit is
			 * originally an event of the child, we don't have
			 * access to ppid.
			 */
			raw->task.ppid = -1;
			raw->task.exit_code = (w->exit_code >> 8) & 0xff;
			raw->task.exit_time_event = sample->sample_id.time;
		}
		raw->task.cap_inheritable = w->cap_inheritable;
		raw->task.cap_permitted = w->cap_permitted;
		raw->task.cap_effective = w->cap_effective;
		raw->task.cap_bset = w->cap_bset;
		raw->task.cap_ambient = w->cap_ambient;
		raw->task.start_boottime = w->start_boottime;
		raw->task.uid = w->uid;
		raw->task.gid = w->gid;
		raw->task.suid = w->suid;
		raw->task.sgid = w->sgid;
		raw->task.euid = w->euid;
		raw->task.egid = w->egid;

		break;
	}
	case EXEC_CONNECTOR_SAMPLE: {
		char				*start, *p, *end;
		int				 i;
		struct exec_connector_sample	*exec_sample = sample_data_body(sample);
		struct raw_exec_connector	*exec;

		if ((raw = raw_event_alloc()) == NULL)
			return (NULL);
		raw->type = RAW_EXEC_CONNECTOR;
		exec = &raw->exec_connector;
		qstr_init(&exec->args);

		start = p = (char *)&exec_sample->stack[0];
		end = start + sizeof(exec_sample->stack);

		for (i = 0; i < (int)exec_sample->argc && p < end; i++)
			p += strnlen(p, end - p) + 1;
		if (p >= end)
			p = end;
		exec->args_len = p - start;
		if (exec->args_len == 0)
			exec->args.p[0] = 0;
		else {
			if (qstr_memcpy(&exec->args, start, exec->args_len) == -1)
				warnx("can't copy args");
			exec->args.p[exec->args_len - 1] = 0;
		}
		strlcpy(exec->comm, str_of_dataloc(sample, &exec_sample->comm),
		    sizeof(exec->comm));
		break;
	}
	default:
		warnx("%s: unknown or invalid sample id=%d", __func__, id);
		return (NULL);
	}

	return (raw);
}

static struct raw_event *
perf_event_to_raw(struct quark_queue *qq, struct perf_event *ev)
{
	struct raw_event		*raw = NULL;
	struct perf_sample_id		*sid = NULL;
	ssize_t				 n;

	switch (ev->header.type) {
	case PERF_RECORD_SAMPLE:
		raw = perf_sample_to_raw(qq, &ev->sample);
		if (raw != NULL)
			sid = &ev->sample.sample_id;
		break;
	case PERF_RECORD_COMM:
		/*
		 * Supress comm events due to exec as we can fetch comm
		 * directly from the task struct
		 */
		if (ev->header.misc & PERF_RECORD_MISC_COMM_EXEC)
			return (NULL);
		if ((qq->flags & QQ_THREAD_EVENTS) == 0 &&
		    ev->comm.pid != ev->comm.tid)
			return (NULL);
		if ((raw = raw_event_alloc()) == NULL)
			return (NULL);
		raw->type = RAW_COMM;
		n = strlcpy(raw->comm.comm, ev->comm.comm,
		    sizeof(raw->comm.comm));
		/*
		 * Yes, comm is variable length, maximum 16. The kernel
		 * guarantees alignment on an 8byte boundary for the sample_id,
		 * that means we have to calculate the next boundary.
		 */
		sid = (struct perf_sample_id *)
		    ALIGN_UP(ev->comm.comm + n + 1, 8);
		break;
	case PERF_RECORD_FORK:
	case PERF_RECORD_EXIT:
		/*
		 * As long as we are still using PERF_RECORD_COMM events, the
		 * kernel implies we want FORK and EXIT as well, see
		 * core.c:perf_event_task_match(), this is likely unintended
		 * behaviour.
		 */
		break;
	case PERF_RECORD_LOST:
		qq->stats.lost += ev->lost.lost;
		break;
	default:
		warnx("%s unhandled type %d\n", __func__, ev->header.type);
		return (NULL);
		break;
	}

	if (sid != NULL) {
		/* FORK/WAKE_UP_NEW_TASK overloads pid and tid */
		if (raw->pid == 0)
			raw->pid = sid->pid;
		if (raw->tid == 0)
			raw->tid = sid->tid;
		raw->opid = sid->pid;
		raw->tid = sid->tid;
		raw->time = sid->time;
		raw->cpu = sid->cpu;
	}

	return (raw);
}

static int
perf_mmap_init(struct perf_mmap *mm, int fd)
{
	mm->mapped_size = (1 + PERF_MMAP_PAGES) * getpagesize();
	mm->metadata = mmap(NULL, mm->mapped_size, PROT_READ|PROT_WRITE,
	    MAP_SHARED, fd, 0);
	if (mm->metadata == MAP_FAILED)
		return (-1);
	mm->data_size = PERF_MMAP_PAGES * getpagesize();
	mm->data_mask = mm->data_size - 1;
	mm->data_start = (uint8_t *)mm->metadata + getpagesize();
	mm->data_tmp_tail = mm->metadata->data_tail;

	return (0);
}

static inline uint64_t
perf_mmap_load_head(struct perf_event_mmap_page *metadata)
{
	return (__atomic_load_n(&metadata->data_head, __ATOMIC_ACQUIRE));
}

static inline void
perf_mmap_update_tail(struct perf_event_mmap_page *metadata, uint64_t tail)
{
	return (__atomic_store_n(&metadata->data_tail, tail, __ATOMIC_RELEASE));
}

static struct perf_event *
perf_mmap_read(struct perf_mmap *mm)
{
	struct perf_event_header	*evh;
	uint64_t			 data_head;
	int				 diff;
	ssize_t				 leftcont;	/* contiguous size left */

	data_head = perf_mmap_load_head(mm->metadata);
	diff = data_head - mm->data_tmp_tail;
	evh = (struct perf_event_header *)
	    (mm->data_start + (mm->data_tmp_tail & mm->data_mask));

	/* Do we have at least one complete event */
	if (diff < (int)sizeof(*evh) || diff < evh->size)
		return (NULL);
	/* Guard that we will always be able to fit a wrapped event */
	if (unlikely(evh->size > sizeof(mm->wrapped_event_buf)))
		errx(1, "getting an event larger than wrapped buf");
	/* How much contiguous space there is left */
	leftcont = mm->data_size - (mm->data_tmp_tail & mm->data_mask);
	/* Everything fits without wrapping */
	if (likely(evh->size <= leftcont)) {
		mm->data_tmp_tail += evh->size;
		return ((struct perf_event *)evh);
	}
	/*
	 * Slow path, we have to copy the event out in a linear buffer. Start
	 * from the remaining end
	 */
	memcpy(mm->wrapped_event_buf, evh, leftcont);
	/* Copy the wrapped portion from the beginning */
	memcpy(mm->wrapped_event_buf + leftcont, mm->data_start, evh->size - leftcont);
	/* Record where our future tail will be on consume */
	mm->data_tmp_tail += evh->size;

	return ((struct perf_event *)mm->wrapped_event_buf);
}

static inline void
perf_mmap_consume(struct perf_mmap *mmap)
{
	perf_mmap_update_tail(mmap->metadata, mmap->data_tmp_tail);
}

static int
perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
    int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

static int
open_tracing(int flags, const char *fmt, ...)
{
	va_list  ap;
	int	 dfd, fd, i, r, saved_errno;
	char	 tail[MAXPATHLEN];
	char	*paths[] = {
		"/sys/kernel/tracing",
		"/sys/kernel/debug/tracing",
	};

	va_start(ap, fmt);
	r = vsnprintf(tail, sizeof(tail), fmt, ap);
	va_end(ap);
	if (r == -1 || r >= (int)sizeof(tail))
		return (-1);
	if (tail[0] == '/')
		return (errno = EINVAL, -1);

	saved_errno = 0;
	for (i = 0; i < (int)nitems(paths); i++) {
		if ((dfd = open(paths[i], O_PATH)) == -1) {
			if (!saved_errno && errno != ENOENT)
				saved_errno = errno;
			warn("open: %s", paths[i]);
			continue;
		}
		fd = openat(dfd, tail, flags);
		close(dfd);
		if (fd == -1) {
			if (!saved_errno && errno != ENOENT)
				saved_errno = errno;
			warn("open: %s", tail);
			continue;
		}

		return (fd);
	}

	if (saved_errno)
		errno = saved_errno;

	return (-1);
}

static int
fetch_tracing_id(const char *tail)
{
	int		 id, fd;
	char		 idbuf[16];
	const char	*errstr;
	ssize_t		 n;

	fd = open_tracing(O_RDONLY, "%s", tail);
	if (fd == -1)
		return (-1);

	n = qread(fd, idbuf, sizeof(idbuf));
	close(fd);
	if (n <= 0)
		return (-1);
	idbuf[n - 1] = 0;
	id = strtonum(idbuf, 1, MAX_SAMPLE_IDS - 1, &errstr);
	if (errstr != NULL) {
		warnx("strtonum: %s", errstr);
		return (errno = ERANGE, -1);
	}

	return (id);
}

static int
parse_probe_data_body_offset(void)
{
	int		 fd;
	FILE		*f;
	char		*line, *s, *e;
	const char	*errstr;
	ssize_t		 n, data_offset;
	size_t		 line_len;
	int		 past_common;

	if (probe_data_body_offset != -1)
		return (0);

	fd = open_tracing(O_RDONLY, "events/sched/sched_process_exec/format");
	if (fd == -1)
		return (-1);
	f = fdopen(fd, "r");
	if (f == NULL) {
		close(fd);
		return (-1);
	}

	past_common = 0;
	line = NULL;
	line_len = 0;
	data_offset = -1;
	while ((n = getline(&line, &line_len, f)) != -1) {
		if (!past_common) {
			past_common = !strcmp(line, "\n");
			continue;
		}
		s = strstr(line, "offset:");
		if (s == NULL)
			break;
		s += strlen("offset:");
		e = strchr(s, ';');
		if (e == NULL)
			break;
		*e = 0;
		data_offset = strtonum(s, 0, SSIZE_MAX, &errstr);
		if (errstr)
			data_offset = -1;
		break;
	}
	free(line);
	fclose(f);
	probe_data_body_offset = data_offset;

	return (0);
}

static int
kprobe_exp(char *exp, ssize_t *off1)
{
	ssize_t		 off;

	switch (*exp) {
	case '(': {
		char	*p, *o, *pa, *pb, c;
		ssize_t	 ia, ib;

		if ((p = strdup(exp)) == NULL)
			return (-1);
		o = p;
		*p++ = 0;
		pa = p;
		if (((p = strchr(pa, '+')) == NULL) &&
		    ((p = strchr(pa, '-')) == NULL)) {
			free(o);
			return (-1);
		}
		c = *p;
		*p++ = 0;
		pb = p;
		if ((p = strchr(p, ')')) == NULL) {
			warnx("%s: %s unbalanced parenthesis\n", __func__, exp);
			free(o);
			return (-1);
		}
		*p = 0;
		if (kprobe_exp(pa, &ia) == -1) {
			warnx("%s: %s is unresolved\n", __func__, pa);
			free(o);
			return (-1);
		}
		if (kprobe_exp(pb, &ib) == -1) {
			warnx("%s: %s is unresolved\n", __func__, pb);
			free(o);
			return (-1);
		}
		free(o);
		off = c == '+' ? ia + ib : ia - ib;
		break;
	}
	default: {
		const char	*errstr;

		off = strtonum(exp, INT32_MIN, INT32_MAX, &errstr);
		if (errstr == NULL)
			break;
		if ((off = quark_btf_offset(exp)) == -1) {
			warnx("%s: %s is unresolved\n", __func__, exp);
			return (-1);
		}
		break;
	}}

	*off1 = off;

	return (0);
}

static char *
kprobe_make_arg(struct kprobe_arg *karg)
{
	int	 i;
	ssize_t	 off;
	char	*p, **pp, *last, *kstr, *tokens[128], *arg_dsl;

	kstr = NULL;
	if ((arg_dsl = strdup(karg->arg_dsl)) == NULL)
		return (NULL);
	i = 0;
	for (p = strtok_r(arg_dsl, " ", &last);
	     p != NULL;
	     p = strtok_r(NULL, " ", &last)) {
		/* Last is sentinel */
		if (i == ((int)nitems(tokens) - 1)) {
			warnx("%s: too many tokens", __func__);
			free(arg_dsl);
			return (NULL);
		}
		tokens[i++] = p;
	}
	tokens[i] = NULL;
	if (asprintf(&kstr, "%%%s", karg->reg) == -1) {
		free(arg_dsl);
		return (NULL);
	}
	for (pp = tokens; *pp != NULL; pp++) {
		p = *pp;
		last = kstr;
		if (kprobe_exp(p, &off) == -1 ||
		    asprintf(&kstr, "+%zd(%s)", off, last) == -1) {
			free(arg_dsl);
			free(last);
			return (NULL);
		}
		free(last);
	}
	last = kstr;
	if (asprintf(&kstr, "%s=%s:%s", karg->name, last, karg->typ) == -1) {
		free(arg_dsl);
		free(last);
		return (NULL);
	}
	free(last);
	free(arg_dsl);

	return (kstr);
}

static char *
kprobe_build_string(struct kprobe *k)
{
	struct kprobe_arg	*karg;
	char			*p, *o, *a;
	int			 r;

	r = asprintf(&p, "%c:%s %s", k->is_kret ? 'r' : 'p', k->name,
	    k->target);
	if (r == -1)
		return (NULL);
	for (karg = k->args; karg->name != NULL; karg++) {
		a = kprobe_make_arg(karg);
		if (a == NULL) {
			free(p);
			return (NULL);
		}
		o = p;
		r = asprintf(&p, "%s %s", o, a);
		free(o);
		free(a);
		if (r == -1)
			return (NULL);
	}

	return (p);
}
#if 0
static int
kprobe_toggle(struct kprobe *k, int enable)
{
	int	fd;
	ssize_t n;

	if ((fd = open_tracing(O_WRONLY, "events/kprobes/%s/enable", k->name))
	    == -1)
		return (-1);
	if (enable)
		n = qwrite(fd, "1", 1);
	else
		n = qwrite(fd, "0", 1);
	close(fd);
	if (n == -1)
		return (-1);

	return (0);
}
#define kprobe_enable(_k)	kprobe_toggle((_k), 1)
#define kprobe_disable(_k)	kprobe_toggle((_k), 0)
#endif
static int
kprobe_uninstall(struct kprobe *k)
{
	char	buf[4096];
	ssize_t n;
	int	fd;

	if ((fd = open_tracing(O_WRONLY | O_APPEND, "kprobe_events")) == -1)
		return (-1);
	if (snprintf(buf, sizeof(buf), "-:%s", k->name) >=
	    (int)sizeof(buf)) {
		close(fd);
		return (-1);
	}
	n = qwrite(fd, buf, strlen(buf));
	close(fd);
	if (n == -1)
		return (-1);

	return (0);
}

/*
 * Builds the kprobe string and "installs" in tracefs, mapping to a perf ring is
 * later and belongs to kprobe_state. This separation makes library cleanup
 * easier.
 */
static int
kprobe_install(struct kprobe *k)
{
	int	 fd;
	ssize_t	 n;
	char	*kstr;

	if (kprobe_uninstall(k) == -1 && errno != ENOENT)
		warn("kprobe_uninstall");
	if ((kstr = kprobe_build_string(k)) == NULL)
		return (-1);
	if ((fd = open_tracing(O_WRONLY, "kprobe_events")) == -1) {
		free(kstr);
		return (-1);
	}
	n = qwrite(fd, kstr, strlen(kstr));
	close(fd);
	free(kstr);
	if (n == -1)
		return (-1);

	return (0);
}

static int
kprobe_ref(void)
{
	int		 i;
	char		 pid[16];
	static int	 renamed;

	/* Other live queues already installed it */
	if (kprobe_queue_refs > 0)
		goto done;
	if (parse_probe_data_body_offset() != 0) {
		warnx("%s: can't parse host probe data offset",
		    __func__);
		return (-1);
	}
	if (quark_btf_init() != 0) {
		warnx("%s: can't initialize btf", __func__);
		return (-1);
	}
	if (!renamed) {
		snprintf(pid, sizeof(pid), "_%llu", (u64)getpid());
		for (i = 0; all_kprobes[i] != NULL; i++) {
			strlcat(all_kprobes[i]->name, pid,
			    sizeof(all_kprobes[i]->name));
		}
		renamed = 1;
	}

	for (i = 0; all_kprobes[i] != NULL; i++) {
		if (kprobe_install(all_kprobes[i]) == -1) {
			warnx("%s: kprobe %s failed", __func__,
			    all_kprobes[i]->name);
			/* Uninstall the ones that succeeded */
			while (--i >= 0)
				kprobe_uninstall(all_kprobes[i]);
			return (-1);
		}
	}
done:
	kprobe_queue_refs++;

	return (0);
}

static void
kprobe_unref(void)
{
	int	i;

	if (--kprobe_queue_refs > 0)
		return;
	for (i = 0; all_kprobes[i] != NULL; i++)
		kprobe_uninstall(all_kprobes[i]);
}

static void
perf_attr_init(struct perf_event_attr *attr, int id)
{
	bzero(attr, sizeof(*attr));

	attr->type = PERF_TYPE_TRACEPOINT;
	attr->size = sizeof(*attr);
	attr->config = id;
	/* attr->config = PERF_COUNT_SW_DUMMY; */
	attr->sample_period = 1;	/* we want all events */
	attr->sample_type =
	    PERF_SAMPLE_TID		|
	    PERF_SAMPLE_TIME		|
	    PERF_SAMPLE_CPU		|
	    PERF_SAMPLE_RAW;

	/* attr->read_format = PERF_FORMAT_LOST; */
	/* attr->mmap2 */
	/* XXX Should we set clock in the child as well? XXX */
	attr->use_clockid = 1;
	attr->clockid = CLOCK_MONOTONIC;
	attr->disabled = 1;
}

static struct perf_group_leader *
perf_open_group_leader(int cpu)
{
	struct perf_group_leader	*pgl;
	int				 id;

	pgl = calloc(1, sizeof(*pgl));
	if (pgl == NULL)
		return (NULL);
	/* By putting EXEC on group leader we save one fd per cpu */
	if ((id = fetch_tracing_id("events/sched/sched_process_exec/id"))
	    == -1) {
		free(pgl);
		return (NULL);
	}
	perf_attr_init(&pgl->attr, id);
	/*
	 * We will still get task events as long as set comm, see
	 * perf_event_to_raw()
	 */
	pgl->attr.comm = 1;
	pgl->attr.comm_exec = 1;
	pgl->attr.sample_id_all = 1;		/* add sample_id to all types */
	pgl->attr.watermark = 1;
	pgl->attr.wakeup_watermark = (PERF_MMAP_PAGES * getpagesize()) / 10;;

	pgl->fd = perf_event_open(&pgl->attr, -1, cpu, -1, 0);
	if (pgl->fd == -1) {
		free(pgl);
		return (NULL);
	}
	if (perf_mmap_init(&pgl->mmap, pgl->fd) == -1) {
		close(pgl->fd);
		free(pgl);
		return (NULL);
	}
	pgl->cpu = cpu;
	id_to_sample_kind[id] = EXEC_SAMPLE;

	return (pgl);
}

static struct kprobe_state *
perf_open_kprobe(struct kprobe *k, int cpu, int group_fd)
{
	int			 id;
	char			 buf[MAXPATHLEN];
	struct kprobe_state	*ks;

	ks = calloc(1, sizeof(*ks));
	if (ks == NULL)
		return (NULL);
	if (snprintf(buf, sizeof(buf), "events/kprobes/%s/id", k->name)
	    >= (int)sizeof(buf)) {
		free(ks);
		return (errno = ENAMETOOLONG, NULL);
	}
	if ((id = fetch_tracing_id(buf)) == -1) {
		free(ks);
		return (NULL);
	}
	perf_attr_init(&ks->attr, id);
	ks->fd = perf_event_open(&ks->attr, -1, cpu, group_fd, 0);
	if (ks->fd == -1) {
		free(ks);
		return (NULL);
	}
	/* Output our records in the group_fd */
	if (ioctl(ks->fd, PERF_EVENT_IOC_SET_OUTPUT, group_fd) == -1) {
		close(ks->fd);
		free(ks);
		return (NULL);
	}
	ks->k = k;
	ks->cpu = cpu;
	ks->group_fd = group_fd;
	id_to_sample_kind[id] = ks->k->sample_kind;

	return (ks);
}

int
kprobe_queue_open(struct quark_queue *qq)
{
	struct kprobe_queue		*kqq = &qq->kprobe_queue;
	struct perf_group_leader	*pgl;
	struct kprobe			*k;
	struct kprobe_state		*ks;
	struct epoll_event		 ev;
	int				 i;

	if ((qq->flags & QQ_KPROBE) == 0)
		return (errno = ENOTSUP, -1);

	/* Don't go to fail since it will kprobe_unref() */
	if (kprobe_ref() == -1)
		return (-1);

	for (i = 0; i < get_nprocs_conf(); i++) {
		pgl = perf_open_group_leader(i);
		if (pgl == NULL)
			goto fail;
		TAILQ_INSERT_TAIL(&kqq->perf_group_leaders, pgl, entry);
		kqq->num_perf_group_leaders++;
	}

	i = 0;
	while ((k = all_kprobes[i++]) != NULL) {
		TAILQ_FOREACH(pgl, &kqq->perf_group_leaders, entry) {
			ks = perf_open_kprobe(k, pgl->cpu, pgl->fd);
			if (ks == NULL)
				goto fail;
			TAILQ_INSERT_TAIL(&kqq->kprobe_states, ks, entry);
		}
	}

	TAILQ_FOREACH(pgl, &kqq->perf_group_leaders, entry) {
		/* XXX PERF_IOC_FLAG_GROUP see bugs */
		if (ioctl(pgl->fd, PERF_EVENT_IOC_RESET,
		    PERF_IOC_FLAG_GROUP) == -1) {
			warn("ioctl PERF_EVENT_IOC_RESET");
			goto fail;
		}
		if (ioctl(pgl->fd, PERF_EVENT_IOC_ENABLE,
		    PERF_IOC_FLAG_GROUP) == -1) {
			warn("ioctl PERF_EVENT_IOC_ENABLE");
			goto fail;
		}
	}

	qq->epollfd = epoll_create1(EPOLL_CLOEXEC);
	if (qq->epollfd == -1) {
		warn("epoll_create1");
		goto fail;
	}
	TAILQ_FOREACH(pgl, &kqq->perf_group_leaders, entry) {
		bzero(&ev, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.fd = pgl->fd;
		if (epoll_ctl(qq->epollfd, EPOLL_CTL_ADD, pgl->fd, &ev) == -1) {
			warn("epoll_ctl");
			goto fail;
		}
	}

	qq->queue_ops = &queue_ops_kprobe;

	return (0);

fail:
	kprobe_queue_close(qq);

	return (-1);
}

static int
kprobe_queue_populate(struct quark_queue *qq)
{
	struct kprobe_queue		*kqq = &qq->kprobe_queue;
	int				 empty_rings, num_rings, npop;
	struct perf_group_leader	*pgl;
	struct perf_event		*ev;
	struct raw_event		*raw;

	num_rings = kqq->num_perf_group_leaders;
	npop = 0;

	/*
	 * We stop if the queue is full, or if we see all perf ring buffers
	 * empty.
	 */
	while (qq->length < qq->max_length) {
		empty_rings = 0;
		TAILQ_FOREACH(pgl, &kqq->perf_group_leaders, entry) {
			ev = perf_mmap_read(&pgl->mmap);
			if (ev == NULL) {
				empty_rings++;
				continue;
			}
			empty_rings = 0;
			raw = perf_event_to_raw(qq, ev);
			if (raw != NULL) {
				raw_event_insert(qq, raw);
				npop++;
			}
			perf_mmap_consume(&pgl->mmap);
		}
		if (empty_rings == num_rings)
			break;
	}

	return (npop);
}

static void
kprobe_queue_close(struct quark_queue *qq)
{
	struct kprobe_queue		*kqq = &qq->kprobe_queue;
	struct perf_group_leader	*pgl;
	struct kprobe_state		*ks;

	/* Stop and close the perf rings */
	while ((pgl = TAILQ_FIRST(&kqq->perf_group_leaders)) != NULL) {
		/* XXX PERF_IOC_FLAG_GROUP see bugs */
		if (pgl->fd != -1) {
			if (ioctl(pgl->fd, PERF_EVENT_IOC_DISABLE,
			    PERF_IOC_FLAG_GROUP) == -1)
				warnx("ioctl PERF_EVENT_IOC_DISABLE:");
			close(pgl->fd);
		}
		if (pgl->mmap.metadata != NULL)
			if (munmap(pgl->mmap.metadata, pgl->mmap.mapped_size) != 0)
				warn("munmap");
		TAILQ_REMOVE(&kqq->perf_group_leaders, pgl, entry);
		free(pgl);
	}
	/* Clean up all state allocated to kprobes */
	while ((ks = TAILQ_FIRST(&kqq->kprobe_states)) != NULL) {
		if (ks->fd != -1)
			close(ks->fd);
		TAILQ_REMOVE(&kqq->kprobe_states, ks, entry);
		free(ks);
	}
	/* Clean up epoll instance */
	if (qq->epollfd != -1) {
		close(qq->epollfd);
		qq->epollfd = -1;
	}
	/* Remove kprobes from tracefs */
	kprobe_unref();
}
