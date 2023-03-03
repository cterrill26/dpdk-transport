#include <stdio.h>
#include "dpdk_transport.h"

int main(int argc, char *argv[]){
    
    init(argc, argv);

    long x = 7490;
    long y = 0;
    struct msginfo info;
    info.length = sizeof(x);
    printf("Sending\n");
    int res = send_dpdk(&x, &info);
    printf("Receiving\n");
    uint32_t len;
    while((len = recv_dpdk(&y, &info)) == 0)
        continue;
    printf("%d, %u, %lu, %lu\n", res, len, x, y);

    x = 830234;
    printf("Sending\n");
    res = send_dpdk(&x, &info);
    printf("Receiving\n");
    while((len = recv_dpdk(&y, &info)) == 0)
        continue;
    printf("%d, %u, %lu, %lu\n", res, len, x, y);

    terminate();

    return 0;
}