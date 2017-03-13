#include "zExec.h"

#include <sys/types.h>  
#include <sys/wait.h>  
#include <errno.h>  
#include <unistd.h>  


int doExecCommand(char * command)
{
    if(!command){
        return -1;
    }

    pid_t pid;
    int status;

    pid = fork();

    if(pid < 0){
        status = -1;
    }else if(pid == 0){
        execl("/bin/sh", "sh", "-c", command, (char *)0);
        exit(127);

    }else{
        while(waitpid(pid, &status, 0) < 0){  
                status = -1;  
                break;  
        
        }  
    }


    return status;

}


int main(void)
{
    return doExecCommand("bash test.sh");
}