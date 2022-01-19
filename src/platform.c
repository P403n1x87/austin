#include <stdio.h>

#include "hints.h"
#include "platform.h"


#if defined PL_LINUX
static size_t max_pid = 0;
#endif

// ----------------------------------------------------------------------------
size_t
pid_max() {
  #if defined PL_LINUX                                               /* LINUX */
  if (max_pid)
    return max_pid;
  
  FILE * pid_max_file = fopen("/proc/sys/kernel/pid_max", "rb");
  if (!isvalid(pid_max_file))
    return 0;

  int has_pid_max = (fscanf(pid_max_file, "%ld", &max_pid) == 1);
  fclose(pid_max_file);
  if (!has_pid_max)
    return 0;

  return max_pid;

  #elif defined PL_MACOS                                             /* MACOS */
  return PID_MAX;

  #elif defined PL_WIN                                                 /* WIN */
  return (1 << 22);  // 4M.  WARNING: This could potentially be violated!

  #endif
}