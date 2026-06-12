/*
*  resfs/libresfs/include/resfs.h
*  SPDX-License-Identifier: MIT
*  Copyright (c) 2026 Andrei Kovalenko
*/

#pragma once

#include <stdint.h>
#include <stddef.h>

#define BH_SIG "RESFS PARTITION "
#define ROOT_SIG "RESFS ROOT "
#define EOP_TAIL_TEXT "END OF RESFS PARTITION"
#define EOP_SIG "ResFSEOP"
#define SMI_SIG "ResFSSMI"
#define DLI_SIG "ResFSDLI"
#define SNAP_SIG "ResFSSNP"
#define SEG_SIG "ResFSSEG"
#define SEG_END_SIG "ResFSEND"
