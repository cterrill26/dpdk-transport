#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "dpdk_transport.h"

void exit(int sig)
{
    printf("Caught signal %d\n", sig);
    terminate();
    exit(0);
}


int main(int argc, char *argv[]){
    init(argc, argv);
    signal(SIGINT, exit);

    uint8_t recv[MAX_MSG_SIZE];
    while(1) {
        struct msginfo info;
        while (recv_dpdk(recv, &info) == 0)
            continue;

	printf("Echoer received %u bytes\n", info.length);
        uint64_t temp_mac; 
        uint32_t temp_ip; 
        temp_mac = info.dst_mac;
        info.dst_mac = info.src_mac;
        info.src_mac = temp_mac;
        temp_ip = info.dst_ip;
        info.dst_ip = info.src_ip;
        info.src_ip = temp_ip;

        send_dpdk(recv, &info);
    }

    return 0;
}
