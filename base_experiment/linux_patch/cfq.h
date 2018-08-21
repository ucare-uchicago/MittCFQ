/*
 *  CFQ, or complete fairness queueing, disk scheduler.
 *
 *  Based on ideas from a previously unfinished io
 *  scheduler (round robin per-process disk scheduling) and Andrea Arcangeli.
 *
 *  Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/ktime.h>
#include <linux/rbtree.h>
#include <linux/ioprio.h>
#include <linux/blktrace_api.h>
#include <linux/blk-cgroup.h>
#include "blk.h"

/*
 * tunables
 */
/* max queue in one round of service */
static const int cfq_quantum = 8;
static const u64 cfq_fifo_expire[2] = { NSEC_PER_SEC / 4, NSEC_PER_SEC / 8 };
/* maximum backwards seek, in KiB */
static const int cfq_back_max = 16 * 1024;
/* penalty of a backwards seek */
static const int cfq_back_penalty = 2;
static const u64 cfq_slice_sync = NSEC_PER_SEC / 10;
//static u64 cfq_slice_async = NSEC_PER_SEC / 25;
//static const int cfq_slice_async_rq = 2;
//static u64 cfq_slice_idle = NSEC_PER_SEC / 125;
//static u64 cfq_group_idle = NSEC_PER_SEC / 125;
static const u64 cfq_target_latency = (u64)NSEC_PER_SEC * 3/10; /* 300 ms */
static const int cfq_hist_divisor = 4;

//static int count_order = 0;


/*
 * offset from end of service tree
 */
#define CFQ_IDLE_DELAY		(NSEC_PER_SEC / 5)

/*
 * below this threshold, we consider thinktime immediate
 */
#define CFQ_MIN_TT		(2 * NSEC_PER_SEC / HZ)

#define CFQ_SLICE_SCALE		(5)
#define CFQ_HW_QUEUE_MIN	(5)
#define CFQ_SERVICE_SHIFT       12

#define CFQQ_SEEK_THR		(sector_t)(8 * 100)
#define CFQQ_CLOSE_THR		(sector_t)(8 * 1024)
#define CFQQ_SECT_THR_NONROT	(sector_t)(2 * 32)
#define CFQQ_SEEKY(cfqq)	(hweight32(cfqq->seek_history) > 32/8)

#define RQ_CIC(rq)		icq_to_cic((rq)->elv.icq)
#define RQ_CFQQ(rq)		(struct cfq_queue *) ((rq)->elv.priv[0])
#define RQ_CFQG(rq)		(struct cfq_group *) ((rq)->elv.priv[1])

//static struct kmem_cache *cfq_pool;

#define CFQ_PRIO_LISTS		IOPRIO_BE_NR
#define cfq_class_idle(cfqq)	((cfqq)->ioprio_class == IOPRIO_CLASS_IDLE)
#define cfq_class_rt(cfqq)	((cfqq)->ioprio_class == IOPRIO_CLASS_RT)

#define sample_valid(samples)	((samples) > 80)
#define rb_entry_cfqg(node)	rb_entry((node), struct cfq_group, rb_node)

/* blkio-related constants */
#define CFQ_WEIGHT_LEGACY_MIN	10
#define CFQ_WEIGHT_LEGACY_DFL	500
#define CFQ_WEIGHT_LEGACY_MAX	1000

struct request_data {
        u64 start_pos;
        int sectors;
        struct request_data *next;
        struct request_data *prev;
};

struct cfq_ttime {
	u64 last_end_request;

	u64 ttime_total;
	u64 ttime_mean;
	unsigned long ttime_samples;
};

/*
 * Most of our rbtree usage is for sorting with min extraction, so
 * if we cache the leftmost node we don't have to walk down the tree
 * to find it. Idea borrowed from Ingo Molnars CFS scheduler. We should
 * move this into the elevator for the rq sorting as well.
 */
struct cfq_rb_root {
	struct rb_root rb;
	struct rb_node *left;
	unsigned count;
	u64 min_vdisktime;
	struct cfq_ttime ttime;
};
#define CFQ_RB_ROOT	(struct cfq_rb_root) { .rb = RB_ROOT, \
			.ttime = {.last_end_request = ktime_get_ns(),},}

/*
 * Per process-grouping structure
 */
struct cfq_queue {
	/* reference count */
	int ref;
	/* various state flags, see below */
	unsigned int flags;
	/* parent cfq_data */
	struct cfq_data *cfqd;
	/* service_tree member */
	struct rb_node rb_node;
	/* service_tree key */
	u64 rb_key;
	/* prio tree member */
	struct rb_node p_node;
	/* prio tree root we belong to, if any */
	struct rb_root *p_root;
	/* sorted list of pending requests */
	struct rb_root sort_list;
	/* if fifo isn't expired, next request to serve */
	struct request *next_rq;
	/* requests queued in sort_list */
	int queued[2];
	/* currently allocated requests */
	int allocated[2];
	/* fifo list of requests in sort_list */
	struct list_head fifo;

	/* time when queue got scheduled in to dispatch first request. */
	u64 dispatch_start;
	u64 allocated_slice;
	u64 slice_dispatch;
	/* time when first request from queue completed and slice started. */
	u64 slice_start;
	u64 slice_end;
	s64 slice_resid;

	/* pending priority requests */
	int prio_pending;
	/* number of requests that are on the dispatch list or inside driver */
	int dispatched;

	/* io prio of this group */
	unsigned short ioprio, org_ioprio;
	unsigned short ioprio_class, org_ioprio_class;

	pid_t pid;

	u32 seek_history;
	sector_t last_request_pos;

	struct cfq_rb_root *service_tree;
	struct cfq_queue *new_cfqq;
	struct cfq_group *cfqg;
	/* Number of sectors dispatched from queue in single dispatch round */
	unsigned long nr_sectors;
};

/*
 * First index in the service_trees.
 * IDLE is handled separately, so it has negative index
 */
enum wl_class_t {
	BE_WORKLOAD = 0,
	RT_WORKLOAD = 1,
	IDLE_WORKLOAD = 2,
	CFQ_PRIO_NR,
};

/*
 * Second index in the service_trees.
 */
enum wl_type_t {
	ASYNC_WORKLOAD = 0,
	SYNC_NOIDLE_WORKLOAD = 1,
	SYNC_WORKLOAD = 2
};

struct cfqg_stats {
#ifdef CONFIG_CFQ_GROUP_IOSCHED
	/* number of ios merged */
	struct blkg_rwstat		merged;
	/* total time spent on device in ns, may not be accurate w/ queueing */
	struct blkg_rwstat		service_time;
	/* total time spent waiting in scheduler queue in ns */
	struct blkg_rwstat		wait_time;
	/* number of IOs queued up */
	struct blkg_rwstat		queued;
	/* total disk time and nr sectors dispatched by this group */
	struct blkg_stat		time;
#ifdef CONFIG_DEBUG_BLK_CGROUP
	/* time not charged to this cgroup */
	struct blkg_stat		unaccounted_time;
	/* sum of number of ios queued across all samples */
	struct blkg_stat		avg_queue_size_sum;
	/* count of samples taken for average */
	struct blkg_stat		avg_queue_size_samples;
	/* how many times this group has been removed from service tree */
	struct blkg_stat		dequeue;
	/* total time spent waiting for it to be assigned a timeslice. */
	struct blkg_stat		group_wait_time;
	/* time spent idling for this blkcg_gq */
	struct blkg_stat		idle_time;
	/* total time with empty current active q with other requests queued */
	struct blkg_stat		empty_time;
	/* fields after this shouldn't be cleared on stat reset */
	uint64_t			start_group_wait_time;
	uint64_t			start_idle_time;
	uint64_t			start_empty_time;
	uint16_t			flags;
#endif	/* CONFIG_DEBUG_BLK_CGROUP */
#endif	/* CONFIG_CFQ_GROUP_IOSCHED */
};

/* Per-cgroup data */
struct cfq_group_data {
	/* must be the first member */
	struct blkcg_policy_data cpd;

	unsigned int weight;
	unsigned int leaf_weight;
};

/* This is per cgroup per device grouping structure */
struct cfq_group {
	/* must be the first member */
	struct blkg_policy_data pd;

	/* group service_tree member */
	struct rb_node rb_node;

	/* group service_tree key */
	u64 vdisktime;

	/*
	 * The number of active cfqgs and sum of their weights under this
	 * cfqg.  This covers this cfqg's leaf_weight and all children's
	 * weights, but does not cover weights of further descendants.
	 *
	 * If a cfqg is on the service tree, it's active.  An active cfqg
	 * also activates its parent and contributes to the children_weight
	 * of the parent.
	 */
	int nr_active;
	unsigned int children_weight;

	/*
	 * vfraction is the fraction of vdisktime that the tasks in this
	 * cfqg are entitled to.  This is determined by compounding the
	 * ratios walking up from this cfqg to the root.
	 *
	 * It is in fixed point w/ CFQ_SERVICE_SHIFT and the sum of all
	 * vfractions on a service tree is approximately 1.  The sum may
	 * deviate a bit due to rounding errors and fluctuations caused by
	 * cfqgs entering and leaving the service tree.
	 */
	unsigned int vfraction;

	/*
	 * There are two weights - (internal) weight is the weight of this
	 * cfqg against the sibling cfqgs.  leaf_weight is the wight of
	 * this cfqg against the child cfqgs.  For the root cfqg, both
	 * weights are kept in sync for backward compatibility.
	 */
	unsigned int weight;
	unsigned int new_weight;
	unsigned int dev_weight;

	unsigned int leaf_weight;
	unsigned int new_leaf_weight;
	unsigned int dev_leaf_weight;

	/* number of cfqq currently on this group */
	int nr_cfqq;

	/*
	 * Per group busy queues average. Useful for workload slice calc. We
	 * create the array for each prio class but at run time it is used
	 * only for RT and BE class and slot for IDLE class remains unused.
	 * This is primarily done to avoid confusion and a gcc warning.
	 */
	unsigned int busy_queues_avg[CFQ_PRIO_NR];
	/*
	 * rr lists of queues with requests. We maintain service trees for
	 * RT and BE classes. These trees are subdivided in subclasses
	 * of SYNC, SYNC_NOIDLE and ASYNC based on workload type. For IDLE
	 * class there is no subclassification and all the cfq queues go on
	 * a single tree service_tree_idle.
	 * Counts are embedded in the cfq_rb_root
	 */
	struct cfq_rb_root service_trees[2][3];
	struct cfq_rb_root service_tree_idle;

	u64 saved_wl_slice;
	enum wl_type_t saved_wl_type;
	enum wl_class_t saved_wl_class;

	/* number of requests that are on the dispatch list or inside driver */
	int dispatched;
	struct cfq_ttime ttime;
	struct cfqg_stats stats;	/* stats for this cfqg */

	/* async queue for each priority case */
	struct cfq_queue *async_cfqq[2][IOPRIO_BE_NR];
	struct cfq_queue *async_idle_cfqq;

};

struct cfq_io_cq {
	struct io_cq		icq;		/* must be the first member */
	struct cfq_queue	*cfqq[2];
	struct cfq_ttime	ttime;
	int			ioprio;		/* the current ioprio */
#ifdef CONFIG_CFQ_GROUP_IOSCHED
	uint64_t		blkcg_serial_nr; /* the current blkcg serial */
#endif
};

/*
 * Per block device queue structure
 */
struct cfq_data {
	long active_time;
	int serial_number;
        u64 rq_completed_sector;
        struct request_data *driver_head;
	struct request_queue *queue;
	/* Root service tree for cfq_groups */
	struct cfq_rb_root grp_service_tree;
	struct cfq_group *root_group;

	/*
	 * The priority currently being served
	 */
	enum wl_class_t serving_wl_class;
	enum wl_type_t serving_wl_type;
	u64 workload_expires;
	struct cfq_group *serving_group;

	/*
	 * Each priority tree is sorted by next_request position.  These
	 * trees are used when determining if two or more queues are
	 * interleaving requests (see cfq_close_cooperator).
	 */
	struct rb_root prio_trees[CFQ_PRIO_LISTS];

	unsigned int busy_queues;
	unsigned int busy_sync_queues;

	int rq_in_driver;
	int rq_in_flight[2];

	/*
	 * queue-depth detection
	 */
	int rq_queued;
	int hw_tag;
	/*
	 * hw_tag can be
	 * -1 => indeterminate, (cfq will behave as if NCQ is present, to allow better detection)
	 *  1 => NCQ is present (hw_tag_est_depth is the estimated max depth)
	 *  0 => no NCQ
	 */
	int hw_tag_est_depth;
	unsigned int hw_tag_samples;

	/*
	 * idle window management
	 */
	struct hrtimer idle_slice_timer;
	struct work_struct unplug_work;

	struct cfq_queue *active_queue;
	struct cfq_io_cq *active_cic;

	sector_t last_position;

	/*
	 * tunables, see top of file
	 */
	unsigned int cfq_quantum;
	unsigned int cfq_back_penalty;
	unsigned int cfq_back_max;
	unsigned int cfq_slice_async_rq;
	unsigned int cfq_latency;
	u64 cfq_fifo_expire[2];
	u64 cfq_slice[2];
	u64 cfq_slice_idle;
	u64 cfq_group_idle;
	u64 cfq_target_latency;

	/*
	 * Fallback dummy cfqq for extreme OOM conditions
	 */
	struct cfq_queue oom_cfqq;

	u64 last_delayed_sync;
};


enum cfqq_state_flags {
	CFQ_CFQQ_FLAG_on_rr = 0,	/* on round-robin busy list */
	CFQ_CFQQ_FLAG_wait_request,	/* waiting for a request */
	CFQ_CFQQ_FLAG_must_dispatch,	/* must be allowed a dispatch */
	CFQ_CFQQ_FLAG_must_alloc_slice,	/* per-slice must_alloc flag */
	CFQ_CFQQ_FLAG_fifo_expire,	/* FIFO checked in this slice */
	CFQ_CFQQ_FLAG_idle_window,	/* slice idling enabled */
	CFQ_CFQQ_FLAG_prio_changed,	/* task priority has changed */
	CFQ_CFQQ_FLAG_slice_new,	/* no requests dispatched in slice */
	CFQ_CFQQ_FLAG_sync,		/* synchronous queue */
	CFQ_CFQQ_FLAG_coop,		/* cfqq is shared */
	CFQ_CFQQ_FLAG_split_coop,	/* shared cfqq will be splitted */
	CFQ_CFQQ_FLAG_deep,		/* sync cfqq experienced large depth */
	CFQ_CFQQ_FLAG_wait_busy,	/* Waiting for next request */
};

#define CFQ_CFQQ_FNS(name)						\
static inline void cfq_mark_cfqq_##name(struct cfq_queue *cfqq)		\
{									\
	(cfqq)->flags |= (1 << CFQ_CFQQ_FLAG_##name);			\
}									\
static inline void cfq_clear_cfqq_##name(struct cfq_queue *cfqq)	\
{									\
	(cfqq)->flags &= ~(1 << CFQ_CFQQ_FLAG_##name);			\
}									\
static inline int cfq_cfqq_##name(const struct cfq_queue *cfqq)		\
{									\
	return ((cfqq)->flags & (1 << CFQ_CFQQ_FLAG_##name)) != 0;	\
}

CFQ_CFQQ_FNS(on_rr);
CFQ_CFQQ_FNS(wait_request);
CFQ_CFQQ_FNS(must_dispatch);
CFQ_CFQQ_FNS(must_alloc_slice);
CFQ_CFQQ_FNS(fifo_expire);
CFQ_CFQQ_FNS(idle_window);
CFQ_CFQQ_FNS(prio_changed);
CFQ_CFQQ_FNS(slice_new);
CFQ_CFQQ_FNS(sync);
CFQ_CFQQ_FNS(coop);
CFQ_CFQQ_FNS(split_coop);
CFQ_CFQQ_FNS(deep);
CFQ_CFQQ_FNS(wait_busy);
#undef CFQ_CFQQ_FNS


/* cfqg stats flags */
enum cfqg_stats_flags {
	CFQG_stats_waiting = 0,
	CFQG_stats_idling,
	CFQG_stats_empty,
};


#define req_lat 60000L

static long request_latency(u64 last_pos, u64 start_pos, long sectors){
	return req_lat;
}

// Need to verify on real machine, check elevator.c elv_dispatch_sort
static long driver_latency(struct cfq_data *cfqd){
	u64 last_pos = cfqd->rq_completed_sector;
	long result = 0L;
	struct request_data *cur = cfqd->driver_head->next;
	while(cur!=cfqd->driver_head){
		//printk(KERN_DEBUG "last_pos %lld, start_pos %lld, sectors %lu\n",last_pos,cur->start_pos,cur->sectors);
		result+=request_latency(last_pos,cur->start_pos,cur->sectors);
		last_pos = cur->start_pos+cur->sectors;
		cur=cur->next;
	}
	return result;
}


static struct cfq_data *get_cfq_data(struct request_queue* q){
	if(q!=NULL){
                struct elevator_queue *elev_q = q->elevator;
                if(elev_q!=NULL){

                        struct elevator_type *elev_t = elev_q->type;
                        if(elev_t!=NULL){
                                char* elev_name = elev_t->elevator_name;
                                if(strcmp(elev_name,"cfq")==0){
                                        void *data = elev_q->elevator_data;
                                        struct cfq_data *cfqd = (struct cfq_data*)data;
                                        if(cfqd->serial_number==-99){
						return cfqd;
                                        }
                                }
                        }
                }
        }
	return NULL;
}

static long request_queue_latency(struct cfq_data *cfqd){
        struct request_queue *q = cfqd->queue;
        struct list_head *head = &(q->queue_head);
        if(list_empty(head)){
                return 0;
        }
        long result = 0L;
        struct list_head *cur = head;
        u64 last_pos = cfqd->last_position;
        while(list_is_last(cur,head)!=1){
                cur = cur->next;
                struct request *rq = list_entry_rq(cur);
                result = result + request_latency(last_pos, blk_rq_pos(rq), blk_rq_sectors(rq));
                last_pos = blk_rq_pos(rq)+blk_rq_sectors(rq);
        }
        return result;
}

static long cfqg_latency(u64 last_pos, struct cfq_group *cfqg, int index){
        u64 last_p = last_pos;
        struct cfq_rb_root *rt_sync = &(cfqg->service_trees[index][2]);
        int i;
        struct rb_node* q_node = rb_first(&rt_sync->rb);
        long total_latency = 0L;
        for(i=0;i<100;i++){
                if(q_node==NULL){
                        break;
                }else{
                        struct cfq_queue* cfqq = rb_entry(q_node, struct cfq_queue, rb_node);
                        if(cfqq!=NULL){
                                struct rb_node *rq_node = rb_first(&cfqq->sort_list);
                                int j;
                                for(j=0;j<100;j++){
                                        if(rq_node==NULL){
                                                break;
                                        }
                                        struct request *rq = rb_entry_rq(rq_node);
                                        if(rq!=NULL){
                                                total_latency+=request_latency(last_p,blk_rq_pos(rq),blk_rq_sectors(rq));
                                                last_p = blk_rq_pos(rq)+blk_rq_sectors(rq);
                                        }
                                        rq_node = rb_next(rq_node);
                                }
                        }

                }
                q_node = rb_next(q_node);
        }
        return total_latency;
}
