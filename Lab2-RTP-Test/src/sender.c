#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "rtp.h"
#include "sender_def.h"

int main(int argc, char** argv){
    int init = initSender(argv[1], 12346, atoi(argv[2]));
    printf("init = %d\n", init);
    if(!init){
        sendMessage(argv[3]);
        terminateSender();
    }
    return 0;
}