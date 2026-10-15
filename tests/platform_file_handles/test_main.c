#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void require(int ok) { if(!ok) abort(); }
int main(void) {
    HANDLE event=CreateEventA(NULL,TRUE,FALSE,NULL);require(event!=NULL);
    HANDLE rejected[]={NULL,INVALID_HANDLE_VALUE,GetCurrentProcess(),(HANDLE)(LONG_PTR)-2,event};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        char bytes[4]={'x','x','x','x'};DWORD count=99;
        require(w32_handle_fd(rejected[i])==-1 && w32_handle_path(rejected[i])==NULL);
        require(!ReadFile(rejected[i],bytes,sizeof(bytes),&count,NULL) && count==0 && GetLastError()==ERROR_INVALID_HANDLE);
        require(!memcmp(bytes,"xxxx",4));count=99;
        require(!WriteFile(rejected[i],bytes,sizeof(bytes),&count,NULL) && count==0 && GetLastError()==ERROR_INVALID_HANDLE);
    }
    char path[]="/tmp/file-handles-XXXXXX";int fd=mkstemp(path);require(fd>=0);
    HANDLE file=w32_open_handle(fd,path);require(file!=NULL);
    require(w32_handle_fd(file)==fd && !strcmp(w32_handle_path(file),path));
    DWORD count=0;char read[4]={0};require(WriteFile(file,"abc",3,&count,NULL) && count==3);
    require(lseek(fd,0,SEEK_SET)==0 && ReadFile(file,read,3,&count,NULL) && count==3 && !memcmp(read,"abc",3));
    CloseHandle(file);CloseHandle(event);require(unlink(path)==0);
    puts("PASS: invalid/pseudo/non-file handles reject without touching bytes; valid file handles still read/write");return 0;
}
