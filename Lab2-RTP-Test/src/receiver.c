#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "rtp.h"
#include "receiver_def.h"

int main(int argc, char** argv){
    int init = initReceiver(12346, atoi(argv[1]));
    printf("init = %d\n", init);
    if(!init){
        recvMessage(argv[2]);
        terminateReceiver();
    }
    return 0;
}