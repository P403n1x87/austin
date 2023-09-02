#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "libaustin.h"

void unwind(pid_t pid, pid_t tid) {
  printf("\n\npid: %d, tid: %d\n\n", pid, tid);

  austin_frame_t *frame = NULL;
  while ((frame = austin_pop_frame()) != NULL) {
    printf("  %s (%s:%d)\n", frame->scope, frame->filename, frame->line);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: test_libaustin <pid>\n");
    return -1;
  }

  pid_t pid = atoi(argv[1]);

  if (austin_up() != 0) {
    perror("austin_up() failed");
    return -1;
  }

  austin_handle_t proc_handle = austin_attach(pid);

  if (proc_handle == NULL) {
    perror("Failed to attach to process");
    return -1;
  }

  austin_sample(proc_handle, unwind);

  austin_detach(proc_handle);

  austin_down();

  return 0;
}
