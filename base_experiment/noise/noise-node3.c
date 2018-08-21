#include <string>
#include <thread>
#include <iostream>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

#define BLOCK_SIZE 1024*1024
#define BLOCK_COUNT 1024*900
#define LOOPSIZE 600
#define IDLETIME 5000
#define PERIOD 10000

static int maxIDs[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,12,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,7,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,1,0,0,0,0,0,0,0,0,0,0,0,6,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,15,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0};

void perform_noise(int id){
	//printf("Mingzhe: perform_noise %i\n",id);
        void *buff;
        int i;
        int fd;
        long rand_byte;
        int read_result;
        long all_byte = (long) BLOCK_SIZE * BLOCK_COUNT;
        fd = open("/dev/sdb", O_DIRECT|O_RDONLY|O_SYNC);
        if(fd < 0) {
                printf("Cannot open\n");
                exit(1);
        }
        srand(time(NULL));
        posix_memalign(&buff, BLOCK_SIZE, BLOCK_SIZE);
        rand_byte = (rand() / (double) RAND_MAX) * BLOCK_COUNT;
        rand_byte *= BLOCK_SIZE;
        read_result = pread(fd, buff, BLOCK_SIZE,rand_byte);
        if(read_result<=0){
                perror("read_failure\n");
        }
        close(fd);
}

void serviceLoop(int id)
{
	while(true){
		struct timeval tv;
                gettimeofday(&tv,NULL);
                long int usec = tv.tv_usec+tv.tv_sec*1000000;
		int index = (usec/PERIOD)%LOOPSIZE;
		int maxID = maxIDs[index];
		if(maxID>id){
			long int remain = PERIOD-(usec%PERIOD);
                        if(remain>=maxID*PERIOD/100){
                                perform_noise(id);
                        }
			//perform_noise(id);
                        /*int actual_sleep = 7700*maxID;
                        if(actual_sleep>SLEEPTIME){
                                actual_sleep = SLEEPTIME;
                        }
                        usleep(actual_sleep);*/
		}else{
			usleep(IDLETIME);
		}
	}
}



int main()  
{
    int i = 0;
    for(i=0;i<64;i++){
    	std::thread threadObj(serviceLoop, i);
	threadObj.detach();
    }
    while(true){
	sleep(1);
    }

    return 0;
}
