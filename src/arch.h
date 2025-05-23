// ------------------------------------------------------------------
//   arch.h
//   Copyright (C) 2019-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#pragma once

extern void arch_initialize (rom argv0);
extern unsigned arch_get_num_cores (void);
extern double arch_get_physical_mem_size (void);
extern StrText arch_get_filesystem_type (FileP file);
extern StrText arch_get_txt_filesystem (void);
extern StrText arch_get_z_filesystem (void);

extern rom arch_get_endianity (void);
extern void arch_set_locale (void);

#define NET_ID_SIZE 32
extern rom arch_get_os (void);
extern rom arch_get_scheduler (void);
extern rom arch_get_glibc (void);
extern StrTextSuperLong arch_get_executable (void);
extern StrTextSuperLong arch_get_genozip_executable (void);
extern rom arch_get_argv0 (void);
extern bool arch_is_valgrind (void);
extern bool arch_is_docker (void);
extern bool arch_is_first_compression (void);
extern Timestamp arch_timestamp (void);
extern bool arch_is_process_alive (uint32_t pid);
extern uint64_t arch_get_max_resident_set (void);
extern bool wget_available (void);
extern bool curl_available (void);

extern rom get_distribution (void);
extern bool dist_is_conda (void);
extern bool dist_is_installforge (void);
extern bool dist_is_github (void);

static inline uint32_t arch_time_lap (uint128_t ts_start) // in msec
{ return (uint32_t)((arch_timestamp() - ts_start) / 1000000); }

extern Timestamp arch_start_time;
