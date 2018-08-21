#include <linux/time.h>
#include <linux/slab.h>



#ifndef global_history_variable
#define global_history_variable
struct sla_timestamp {
	struct timeval start_tv;
};

struct history {
	int index;
	int count;
	int latency_threshold;
	int slowcount_threshold;
	int slow_count;
	long total_diff;
	int latencies[10000];
	int predicted_latencies[10000];
	int diff_latencies[10000];
};
#endif

