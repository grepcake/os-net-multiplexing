#include <stddef.h> // size_t

#define SOCKET_DIR "/tmp/os-net/"
#define SOCKET_NAME SOCKET_DIR "socket-0"
#define DEFAULT_PORT "25001"
#define PACKET_SIZE 506

struct cmn_peer;

struct cmn_peer *
cmn_peer_create();

void
cmn_peer_destroy(struct cmn_peer *peer);

int
cmn_listen(struct cmn_peer *server);

int
cmp_exchange(struct cmn_peer *client, char const *message, char buf[static PACKET_SIZE], size_t *buf_len);
