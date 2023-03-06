#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "dpdk_transport.h"

#define MSG_LEN 10
#define NUM_MSGS 2


int main(int argc, char *argv[]){
    int ret = init(argc, argv);
    argc -= ret;
    argv += ret;

    uint64_t dst_mac = 0;
    uint32_t src_ip = 0, dst_ip = 0;
    int dst_mac_flag = 0, dst_ip_flag = 0, src_ip_flag = 0;
    char c;

    while ((c = getopt(argc, argv, "m:s:d:h")) != -1)
        switch (c)
        {
        case 'm':
            dst_mac = string_to_mac(optarg) << 16;
            dst_mac_flag = 1;
            break;
        case 's':
            src_ip = string_to_ip(optarg);
            src_ip_flag = 1;
            break;
        case 'd':
            dst_ip = string_to_ip(optarg);
            dst_ip_flag = 1;
            break;
        case 'h':
            printf("usage -- -m [dst MAC] -s [src IP] -d [dst IP]\n");
            exit(0);
            break;
        }

    if (dst_mac_flag == 0)
    {
        fprintf(stderr, "missing -m for destination MAC adress\n");
        exit(1);
    }
    if (src_ip_flag == 0)
    {
        fprintf(stderr, "missing -s for IP source adress\n");
        exit(1);
    }
    if (dst_ip_flag == 0)
    {
        fprintf(stderr, "missing -d for IP destination adress\n");
        exit(1);
    }


    int8_t msg[MSG_LEN];
    printf("Initiator Sending\n");
    for (int i = 0; i < NUM_MSGS; i++) {
        for (int j = 0; j < MSG_LEN; j++){
            msg[j] = (i+j)%256;
        }
        struct msginfo info;
        info.length = MSG_LEN*sizeof(int8_t);
        info.src_ip = src_ip;
        info.dst_ip = dst_ip;
        info.src_mac = port_to_mac(0);
        info.dst_mac = dst_mac;
        info.portid = 0;
        send_dpdk(msg, &info);
        printf("Initiator sent msg %d\n", i);
    }

    int8_t recv[MAX_MSG_SIZE];
    printf("Initiator Receiving\n");
    for (int i = 0; i < NUM_MSGS; i++) {
        struct msginfo info;
        while (recv_dpdk(recv, &info) == 0)
            continue;
        
        if (info.length != MSG_LEN*sizeof(int8_t))
            printf("Initiator received wrong message length: %u\n", info.length);
        if (info.src_ip != dst_ip)
            printf("Initiator received wrong src ip: %u\n", info.src_ip);
        if (info.dst_ip != src_ip)
            printf("Initiator received wrong dst ip: %u\n", info.dst_ip);
        if (info.src_mac != dst_mac)
            printf("Initiator received wrong src msc: %lu\n", info.src_mac);
        if (info.dst_mac != port_to_mac(0))
            printf("Initiator received wrong dst msc: %lu\n", info.dst_mac);

        for (int j = 1; j < MSG_LEN; j++){
            if (recv[j] != (recv[j-1] + 1)%256)
                printf("Initiator received wrong data at index %u: %u\n", j, recv[j]);
        }
        
        printf("Initiator received msg %u\n", recv[0]);
    }

    terminate();

    return 0;
}