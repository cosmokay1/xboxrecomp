#include "file_read_observer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls;
static XboxFileReadEvent last;
static unsigned char copied[8];
static void require(int ok) { if(!ok) abort(); }
static void observe(const XboxFileReadEvent *event) {
    ++calls; last=*event;last.bytes=NULL;
    memset(copied,0,sizeof(copied));
    if(event->bytes) {require(event->completed<=sizeof(copied));memcpy(copied,event->bytes,event->completed);}
    xbox_file_read_notify(99,"nested",6,6,0); /* Nested I/O must not recurse. */
}
int main(void) {
    const char bytes[]="abc";
    xbox_file_read_notify(12,bytes,3,3,0);require(!calls);
    xbox_SetFileReadObserver(observe);
    xbox_file_read_notify(12,bytes,3,3,0);
    require(calls==1 && last.native_handle==12 && last.requested==3 && last.completed==3 && !last.status && !memcmp(copied,bytes,3));
    xbox_file_read_notify(12,bytes,3,1,0);require(calls==2 && last.completed==1 && copied[0]=='a' && !copied[1]);
    xbox_file_read_notify(12,bytes,3,0,0);require(calls==3 && !last.completed && !copied[0]);
    xbox_file_read_notify(12,bytes,3,99,(int32_t)0xC0000008u);
    require(calls==4 && last.status==(int32_t)0xC0000008u && !last.completed && !copied[0]);
    xbox_file_read_notify(12,bytes,1,3,0);require(calls==5 && last.completed==3 && !copied[0]);
    xbox_file_read_notify(12,NULL,3,3,0);require(calls==6 && !copied[0]);
    xbox_SetFileReadObserver(NULL);xbox_file_read_notify(12,bytes,3,3,0);require(calls==6);
    require(!strcmp(bytes,"abc"));
    puts("PASS: file-read observation, bounded borrowed bytes, failures, detach and nonrecursive delivery");return 0;
}
