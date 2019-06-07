#include <errno.h> // perror
#include <netdb.h> // getaddrinfo

#include "common.h"
#include "utils.h"

#define PRINT_USAGE() fprintf(stderr, "USAGE: %s", argv[0]);

int
main(int argc, char *argv[]) {
    if (argc != 1) {
        PRINT_USAGE();
        exit(EXIT_FAILURE);
    }
    struct cmn_peer *SCOPED_PEER server = cmn_peer_create();
    if (server == NULL) {
        fprintf(stderr, "can't create server\n");
        exit(EXIT_FAILURE);
    }
    printf("listening\n");
    if (cmn_listen(server) == -1) {
        printf("stopped listening\n");
    }
}
