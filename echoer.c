#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "dpdk_transport.h"
#include <rte_malloc.h>

#define NUM_MSGS 1000

int main(int argc, char *argv[]){
    init(argc, argv);

    uint16_t recv[MAX_MSG_SIZE/sizeof(uint16_t)];
    for(int i = 0; i < NUM_MSGS; i++) {
        struct msginfo info;
        while (recv_dpdk(recv, &info) == 0)
            continue;

        uint64_t temp_mac; 
        uint32_t temp_ip; 
        temp_mac = info.dst_mac;
        info.dst_mac = info.src_mac;
        info.src_mac = temp_mac;
        temp_ip = info.dst_ip;
        info.dst_ip = info.src_ip;
        info.src_ip = temp_ip;

        if (send_dpdk(recv, &info) < 0)
	        printf("Echoer failed to echo %u bytes from msg %u\n", info.length, recv[0]);
        else
	        printf("Echoer echoed %u bytes from msg %u\n", info.length, recv[0]);
    }

    FILE *fptr = fopen("stats.txt","w");

    rte_malloc_dump_stats(fptr, NULL);
    fclose(fptr);
    terminate();

    return 0;
}
