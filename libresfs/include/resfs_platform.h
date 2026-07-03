/*
*  resfs/libresfs/include/resfs_platform.h
*  SPDX-License-Identifier: MIT
*  Copyright (c) 2026 Andrei Kovalenko
*/

/*  ResFS platform compatibility layer.
*  
*  ResFS Library (libresfs) is a standalone library which can be
*  compiled as freestanding code. The only C standard library
*  requirement is <string.h>, which must be provided by your target
*  environment. For userspace programs, it uses <string.h> directly 
*  (in this case library should not be compiled as freestanding code). 
*  
*  This header file provides access to the appropriate <string.h> header
*  for the following environments:
*   
*  Linux: <linux/string.h>
*  RhK: <rhk/string.h>
*  Userspace: <string.h>
*  
*/

#ifndef RESFS_PLATFORM_H
#define RESFS_PLATFORM_H

#ifdef __KERNEL__
#include <linux/string.h>

#elif defined(__RHK__)
#include <rhk/string.h>

#else
#include <string.h>

#endif
#endif /* RESFS_PLATFORM_H */