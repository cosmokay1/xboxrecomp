#include <stdlib.h>
void xbox_log(int level,const char *subsystem,const char *format,...) {(void)level;(void)subsystem;(void)format;}
int xbox_verbose(void) {return 0;}
void *recomp_lookup(unsigned a) {(void)a;abort();}
void *recomp_lookup_manual(unsigned a) {(void)a;abort();}
