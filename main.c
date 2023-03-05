#include <stdio.h>
#include "dpdk_transport.h"
#define MSG_LEN 100000

int main(int argc, char *argv[]){
    

    init(argc, argv);

    int8_t msg1[MSG_LEN];
    int8_t recv1[MSG_LEN];
    int8_t msg2[MSG_LEN];
    int8_t recv2[MSG_LEN];
    for (int i = 0; i < MSG_LEN; i++){
        msg1[i] = (i+2)%256;
        msg2[i] = i%256;
    }
    struct msginfo info;
    info.length = MSG_LEN*sizeof(int8_t);
    printf("Sending\n");
    send_dpdk(msg1, &info);
    send_dpdk(msg2, &info);

    printf("Receiving\n");
    while(recv_dpdk(recv1, &info) == 0)
        continue;

    while(recv_dpdk(recv2, &info) == 0)
        continue;

    for (int i = 0; i < MSG_LEN; i++){
        if (msg1[i] != recv1[i])
            printf("Differs 1: %d/n", i);
        if (msg2[i] != recv2[i])
            printf("Differs 2: %d/n", i);
    }

    terminate();

    return 0;
}