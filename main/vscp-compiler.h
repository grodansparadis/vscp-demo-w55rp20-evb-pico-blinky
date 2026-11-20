/*
  vscp-compiler.h - Memory allocation for the vscp-firmware code
*/
#ifndef _VSCP_COMPILER_H_
#define _VSCP_COMPILER_H_

#include <stdio.h>
#include <stdlib.h>

#define VSCP_MALLOC(s)      malloc(s)
#define VSCP_REMALLOC(p, s) realloc(p, s)
#define VSCP_FREE(x)        free(x)

#endif
