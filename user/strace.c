#include "user/user.h"

void strace(int mask, char *command, char *args[]){
    // forking
    int pid = fork();
    // successful forking
    if(pid == 0){
        trace(mask);
        exec(command, args);
        exit(0);
    }
    // if forking is not successful
    else{
        wait(0);
    }
}

int main(int argc, char *argv[]){
    // number of arguments are less - Error Handling
    if(argc < 3){
        printf("Usage: strace mask command [args]...\n");
        exit(1);
    }
    else{
        int mask;
        mask = atoi(argv[1]);
        strace(mask, argv[2], argv+2);
        exit(0);
    }
}


