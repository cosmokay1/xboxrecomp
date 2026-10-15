#include "kernel.h"
#include "file_read_observer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
/* Native event state assertion, not a guest blocking operation. */
#undef WaitForSingleObject
#ifndef FILE_READ_OBSERVER_FOCUSED
/* Runtime link may pull the generic dispatcher's title entry references. */
void *recomp_lookup(ULONG a) {(void)a;abort();}
void *recomp_lookup_manual(ULONG a) {(void)a;abort();}
#endif
static unsigned calls;
static HANDLE nested_handle,completion_event;
static XboxFileReadEvent observed;
static char delivered[32];
static void require(int ok) {if(!ok) abort();}
static void observe(const XboxFileReadEvent *event) {
    ++calls;observed=*event;observed.bytes=NULL;
    require(WaitForSingleObject(completion_event,0)==WAIT_TIMEOUT);
    memset(delivered,0,sizeof(delivered));
    if(event->bytes) {require(event->completed<=sizeof(delivered));memcpy(delivered,event->bytes,event->completed);}
    XBOX_IO_STATUS_BLOCK io;LARGE_INTEGER offset;char byte;
    offset.QuadPart=0;
    require(xbox_NtReadFile(nested_handle,NULL,NULL,NULL,&io,&byte,1,&offset)==STATUS_SUCCESS);
}
int main(void) {
    char root[MAX_PATH],path[512];
#ifdef _WIN32
    require(GetTempPathA(MAX_PATH,root)>0);
#else
    strcpy(root,"/tmp/");
#endif
    snprintf(path,sizeof(path),"%sfile-read-observer-%lu.dat",root,(unsigned long)GetCurrentProcessId());
    HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    require(file!=INVALID_HANDLE_VALUE);
    DWORD wrote=0;require(WriteFile(file,"abcde",5,&wrote,NULL) && wrote==5);
    nested_handle=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    require(nested_handle!=INVALID_HANDLE_VALUE);
    char bytes[16];XBOX_IO_STATUS_BLOCK io;LARGE_INTEGER offset;offset.QuadPart=0;
    require(xbox_NtReadFile(file,NULL,NULL,NULL,&io,bytes,3,&offset)==STATUS_SUCCESS && io.Information==3 && !memcmp(bytes,"abc",3) && !calls);
    completion_event=CreateEventA(NULL,TRUE,FALSE,NULL);require(completion_event!=NULL);
    xbox_SetFileReadObserver(observe);
    require(xbox_NtReadFile(file,completion_event,NULL,NULL,&io,bytes,3,&offset)==STATUS_SUCCESS);
    require(WaitForSingleObject(completion_event,0)==WAIT_OBJECT_0);ResetEvent(completion_event);
    require(calls==1 && io.Status==STATUS_SUCCESS && io.Information==3 && observed.native_handle==(uintptr_t)file && observed.requested==3 && observed.completed==3 && !memcmp(delivered,"abc",3));
    offset.QuadPart=4;
    require(xbox_NtReadFile(file,NULL,NULL,NULL,&io,bytes,8,&offset)==STATUS_SUCCESS && io.Information==1);
    require(calls==2 && observed.completed==1 && delivered[0]=='e');
    offset.QuadPart=5;
    require(xbox_NtReadFile(file,NULL,NULL,NULL,&io,bytes,8,&offset)==STATUS_END_OF_FILE && io.Information==0);
    require(calls==3 && !observed.completed && !delivered[0]);
    io.Information=99;
    require(xbox_NtReadFile(INVALID_HANDLE_VALUE,NULL,NULL,NULL,&io,bytes,8,&offset)!=STATUS_SUCCESS);
    require(calls==4 && !observed.completed && !delivered[0]);
    require(xbox_NtReadFile(file,NULL,NULL,NULL,NULL,bytes,8,&offset)==STATUS_INVALID_PARAMETER && calls==5 && !observed.completed);
    xbox_SetFileReadObserver(NULL);offset.QuadPart=0;
    require(xbox_NtReadFile(file,NULL,NULL,NULL,&io,bytes,3,&offset)==STATUS_SUCCESS && calls==5);
    CloseHandle(completion_event);CloseHandle(nested_handle);CloseHandle(file);
#ifdef _WIN32
    require(DeleteFileA(path));
#else
    require(unlink(path)==0);
#endif
    puts("PASS: actual kernel read, short read, EOF, invalid handle, missing status and nonrecursive observer");return 0;
}
