#include <linux/time.h>
#include <linux/slab.h>
#include <linux/history.h>

struct sla_timestamp *alloc_ts(void){
	struct sla_timestamp *x = kmalloc(sizeof(struct sla_timestamp),GFP_KERNEL);
	do_gettimeofday(&(x->start_tv));
	return x;
};

void reset_history(struct history *x){
	x->index=0;
	x->count=0;
	x->slow_count=0;
	memset(x->latencies, 0, sizeof(x->latencies));
};

struct history* alloc_history(void){
	struct history *x = kmalloc(sizeof(struct history),GFP_KERNEL);
	reset_history(x);
	return x;
};

int cur_value(struct history *x){
	return x->latencies[x->index];
}

void set_parameter(struct history *x, int lat_threshold, int sc_threshold){
	x->latency_threshold = lat_threshold;
	x->slowcount_threshold = sc_threshold;
	reset_history(x);
};

bool can_accept(struct history *x, int value){
	if(x->count<history_capacity){
		return true;
	}else{
		if(x->slow_count<x->slowcount_threshold){
			return true;
		}else if(x->slow_count==x->slowcount_threshold){
			int curV = cur_value(x);
			if(curV > x->latency_threshold){
				return true;
			}
			return false;
		}else{
			return false;
		}
	}
};

void accept(struct history *x, int value){
	int prev_value = x->latencies[x->index];
	x->latencies[x->index]=value;
	x->count++;
	if(x->count>history_capacity){
		x->count = history_capacity;
	}
	x->index=(x->index+1)%history_capacity;
	if(value<=x->latency_threshold){
		if(prev_value<=x->latency_threshold){
			//
		}else{
			x->slow_count--;
		}
	}else{
		if(prev_value<=x->latency_threshold){
                        x->slow_count++;
                }else{
                        //
                }
	}
};

