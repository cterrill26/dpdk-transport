#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <iostream>
#include "dpdk_transport.h"

#define NUM_MSGS 4000

int main(int argc, char *argv[]){
    init(argc, argv, F_SINGLE_SEND | F_SINGLE_RECV);
    std::cout << "Hello" << std::endl;
    uint16_t recv[MAX_MSG_SIZE/sizeof(uint16_t)];
    for(int i = 0; i < NUM_MSGS; i++) {
        struct msg_info info;
        while (recv_dpdk(recv, &info, NULL) == 0)
            continue;

        uint64_t temp_mac; 
        uint32_t temp_ip; 
        temp_mac = info.dst_mac;
        info.dst_mac = info.src_mac;
        info.src_mac = temp_mac;
        temp_ip = info.dst_ip;
        info.dst_ip = info.src_ip;
        info.src_ip = temp_ip;

        while (send_dpdk(recv, &info) < 0)
		continue;
    }

    sleep(2);
    terminate();

    return 0;
}
