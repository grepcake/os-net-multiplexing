#include <assert.h> // assert

#include "common.h"
#include "utils.h"

#define PRINT_USAGE() fprintf(stderr, "USAGE: %s message", argv[0]);

int
main(int argc, char *argv[]) {
    if (argc != 2) {
        PRINT_USAGE();
        exit(EXIT_FAILURE);
    }
    char const *message                 = argv[1];
    struct cmn_peer *SCOPED_PEER client = cmn_peer_create();
    char buf[PACKET_SIZE + 1];
    size_t buf_len;
    int rc = cmp_exchange(client, message, buf, &buf_len);
    if (rc == -1) {
        fprintf(stderr, "exchange failed\n");
        exit(EXIT_FAILURE);
    }
    assert(buf_len < PACKET_SIZE + 1);
    assert(buf[buf_len] == '\0');

    printf("sent: '%s', received: '%s'\n", message, buf);
}
