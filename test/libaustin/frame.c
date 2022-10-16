#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "libaustin.h"

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: frame <pid> <frame_addr>\n");
        return -1;
    }

    pid_t pid = atoi(argv[1]);
    void *frame_addr = (void *)atoll(argv[2]);

    if (austin_up() != 0)
    {
        perror("austin_up() failed");
        return -1;
    }

    austin_handle_t proc_handle = austin_attach(pid);

    if (proc_handle == NULL)
    {
        perror("Failed to attach to process");
        return -1;
    }

    austin_frame_t *frame = austin_read_frame(proc_handle, frame_addr);

    printf("%s (%s:%d)\n", frame->scope, frame->filename, frame->line);

    austin_detach(proc_handle);

    austin_down();

    return 0;
}
