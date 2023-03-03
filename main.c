#include <stdio.h>
#include "dpdk_transport.h"
#define MSG_LEN 119

int main(int argc, char *argv[]){
    

    init(argc, argv);

    int8_t msg[MSG_LEN];
    int8_t recv[MSG_LEN];
    for (int i = 0; i < MSG_LEN; i++){
        msg[i] = i%256;
    }
    struct msginfo info;
    info.length = MSG_LEN*sizeof(int8_t);
    printf("Sending\n");
    int res = send_dpdk(msg, &info);

    printf("Receiving\n");
    uint32_t len;
    while((len = recv_dpdk(recv, &info)) == 0)
        continue;
    printf("%d, %u\n", res, len);

    printf("Sending\n");
    res = send_dpdk(msg, &info);
    printf("Receiving\n");
    while((len = recv_dpdk(recv, &info)) == 0)
        continue;
    printf("%d, %u\n", res, len);

    terminate();

    return 0;
}