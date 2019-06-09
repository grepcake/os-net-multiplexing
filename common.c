#include "common.h"

#include <assert.h>     // assert
#include <errno.h>      // errno
#include <fcntl.h>      // fcntl
#include <poll.h>       // pollfd
#include <stdbool.h>    // bool
#include <stddef.h>     // NULL, size_t
#include <stdlib.h>     // calloc
#include <string.h>     // strerror
#include <sys/socket.h> // socket, msghdr, cmsghdr
#include <sys/stat.h>   // mkdir
#include <sys/un.h>     // struct sockaddr_un
#include <unistd.h>     // close, pipe

#include "utils.h"

int
send_fd(int sfd, int fd_to_send) {
    char iobuf[1];
    struct iovec io = {.iov_base = iobuf, .iov_len = sizeof(iobuf)};
    union {
        char buf[CMSG_SPACE(sizeof(fd_to_send))];
        struct cmsghdr align;
    } u;

    struct msghdr msg = {
        .msg_iov        = &io,
        .msg_iovlen     = 1,
        .msg_control    = u.buf,
        .msg_controllen = sizeof(u.buf)};

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    *cmsg                = (struct cmsghdr){
        .cmsg_level = SOL_SOCKET,
        .cmsg_type  = SCM_RIGHTS,
        .cmsg_len   = CMSG_LEN(sizeof(fd_to_send))};
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(fd_to_send));

    if (sendmsg(sfd, &msg, 0) == -1) {
        err_log("sendmsg failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int
recv_fd(int sfd) {
    char iobuf[1];
    struct iovec io = {.iov_base = iobuf, .iov_len = sizeof(iobuf)};
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;

    struct msghdr msg = {
        .msg_iov        = &io,
        .msg_iovlen     = 1,
        .msg_control    = u.buf,
        .msg_controllen = sizeof(u.buf)};

    if (recvmsg(sfd, &msg, 0) == -1) {
        err_log("recvmsg failed: %s", strerror(errno));
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));

    return fd;
}

int
cmn_init() {
    int rc = mkdir(SOCKET_DIR, 0700);
    if (rc == -1) {
        if (errno != EEXIST) {
            err_log("couldn't create directory %s: %s", SOCKET_DIR, strerror(errno));
            return -1;
        }
    }
    return 0;
}

struct cmn_peer {
    int sfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_len;
};

struct cmn_peer *
cmn_peer_create() {
    struct cmn_peer *peer = calloc(1, sizeof(struct cmn_peer));
    if (peer == NULL) {
        err_log("calloc failed: %s", strerror(errno));
        free(peer);
        return NULL;
    }
    peer->sfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (peer->sfd == -1) {
        err_log("could not create socket");
        free(peer);
        return NULL;
    }
    peer->peer_addr     = (struct sockaddr_un){.sun_family = AF_UNIX, .sun_path = SOCKET_NAME};
    peer->peer_addr_len = sizeof(struct sockaddr_un);
    return peer;
}

void
cmn_peer_destroy(struct cmn_peer *peer) {
    if (peer == NULL) {
        return;
    }
    close(peer->sfd);
    free(peer);
}

int
set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    int rc    = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (rc == -1) {
        err_log("can't set fd non blocking: fcntl failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int
cmn_listen(struct cmn_peer *server) {
    if (cmn_init() == -1) {
        return -1;
    }
    unlink(SOCKET_NAME);
    int rc;
    rc = bind(server->sfd, (struct sockaddr const *)&server->peer_addr, server->peer_addr_len);
    if (rc == -1) {
        err_log("bind failed: %s", strerror(errno));
        return -1;
    }
    listen(server->sfd, 2048);

    int open_max = get_open_max();
    if (open_max == -1) {
        return -1;
    }
    size_t pfds_cap = open_max;
    assert(pfds_cap >= 1);
    size_t pfds_len                = 1;
    struct pollfd *SCOPED_MEM pfds = malloc(pfds_cap * sizeof(struct pollfd));
    if (pfds == NULL) {
        err_log("malloc failed");
        return -1;
    }
    pfds[0] = (struct pollfd){.fd = server->sfd, .events = POLLIN};
    for (size_t i = 1; i < pfds_cap; ++i) {
        pfds[i].fd = -1;
    }

    struct msg_info {
        bool reading_finished;
        size_t read_pos;
        char buf[PACKET_SIZE + 1]; // + 1 for zero-terminator
    };
    struct msg_info *SCOPED_MEM infos = calloc(pfds_cap, sizeof(struct msg_info));
    while (true) {
        rc = poll(pfds, pfds_len, -1);
        if (rc == -1) {
            err_log("poll failed: %s", strerror(errno));
            return -1;
        }
        if (pfds[0].revents & POLLIN) {
            printf("new connection\n");
            int connfd = accept(server->sfd, NULL, NULL);
            if (connfd == -1) {
                printf("failed to accept connection, continuing…\n");
            } else {
                size_t i;
                for (i = 1; i < pfds_cap; ++i) {
                    if (pfds[i].fd < 0) {
                        pfds[i] = (struct pollfd){.fd = connfd, .events = POLLOUT};
                        break;
                    }
                }
                if (i == pfds_cap) {
                    printf("can't accept any more clients\n");
                } else {
                    pfds_len = MAX(pfds_len, i + 1);
                }
            }
        }
        for (size_t i = 1; i < pfds_len; ++i) {
            if (pfds[i].fd < 0) {
                continue;
            }
            if (pfds[i].revents == POLLOUT) {
                int pipe_fds[2];
                pipe(pipe_fds);
                if (set_non_blocking(pipe_fds[0]) == -1) {
                    return -1;
                }
                if (set_non_blocking(pipe_fds[1]) == -1) {
                    return -1;
                }
                if (send_fd(pfds[i].fd, pipe_fds[1]) == -1) { // send writing end
                    return -1;
                }
                close(pfds[i].fd);
                close(pipe_fds[1]);
                pfds[i].fd        = pipe_fds[0]; // wait on reading end
                pfds[i].events    = POLLIN;
                infos[i].read_pos = 0;
                continue;
            }
            if (pfds[i].revents == POLLIN) {
                char *buf        = infos[i].buf;
                size_t *read_pos = &infos[i].read_pos;
                ssize_t nread    = read(pfds[i].fd, buf + *read_pos, PACKET_SIZE - *read_pos);
                *read_pos += nread;
                bool read_failed = nread == -1;
                bool eof_reached =
                    nread == 0 || *read_pos == PACKET_SIZE || buf[*read_pos - 1] == '\0';
                if (read_failed || eof_reached) {
                    infos[i].reading_finished = true;
                    pfds[i].events            = POLLOUT;
                    if (read_failed) {
                        printf("read failed: %s\n", strerror(errno));
                    } else {
                        assert(eof_reached);
                        if (*read_pos == PACKET_SIZE && buf[*read_pos - 1] != '\0') {
                            printf("%u bytes received, rejecting remainder (if any)\n", PACKET_SIZE);
                        }
                        buf[*read_pos] = '\0';
                        printf("received: %s\n", buf);
                    }
                }
            }
            bool has_error = true;
            if (pfds[i].revents & POLLHUP) {
                printf("client closed its end of channel\n");
            } else if (pfds[i].revents & POLLNVAL) {
                printf("invalid request, fd not open\n");
            } else if (pfds[i].revents & POLLERR) {
                printf("error condition\n");
            } else {
                has_error = false;
            }
            if (infos[i].reading_finished || has_error) {
                close(pfds[i].fd);
                pfds[i].fd                = -1;
                infos[i].read_pos         = 0;
                infos[i].reading_finished = false;
            }
        }
    }
}

int
cmp_exchange(struct cmn_peer *client, char const *message) {
    int rc;
    rc                   = connect(client->sfd, (struct sockaddr const *)&client->peer_addr, client->peer_addr_len);
    struct pollfd pfds[] = {{.fd = client->sfd, .events = POLLOUT}};
    size_t msg_len       = strlen(message) + 1;
    size_t sent_pos      = 0;
    if (rc == -1) {
        if (errno != EINPROGRESS) {
            fprintf(stderr, "failed to connect: %s\n", strerror(errno));
            return -1;
        }
        printf("waiting for connect completion\n");
    }
    pfds[0].events = POLLOUT;
    bool got_pipe  = false;
    while (true) {
        rc = poll(pfds, 1, -1);
        if (rc == -1) {
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            return -1;
        }
        assert(rc == 1);
        if (pfds[0].revents & POLLHUP) {
            // ignore
        }
        if (pfds[0].revents & POLLNVAL) {
            printf("invalid request, fd not open\n");
            return -1;
        }
        if (pfds[0].revents & POLLERR) {
            printf("error condition\n");
            return -1;
        }
        if (pfds[0].revents & POLLIN) {
            int write_fd = recv_fd(pfds[0].fd);
            if (write_fd == -1) {
                return -1;
            }
            printf("received writing end of pipe\n");
            got_pipe       = true;
            pfds[0].fd     = write_fd;
            pfds[0].events = POLLOUT;
        }
        if (pfds[0].revents & POLLOUT) {
            if (!got_pipe) {
                int error         = 0;
                socklen_t err_len = sizeof(error);
                if (getsockopt(pfds[0].fd, SOL_SOCKET, SO_ERROR, &error, &err_len) == -1) {
                    printf("getsockopt failed: %s\n", strerror(errno));
                    return -1;
                }
                if (error == 0) {
                    printf("connected!\n");
                    sleep(3);
                    pfds[0].events = POLLIN;
                } else {
                    fprintf(stderr, "failed to connect\n");
                    return -1;
                }
            } else {
                ssize_t nsent;
                nsent = write(pfds[0].fd, message + sent_pos, msg_len - sent_pos);
                if (nsent == -1) {
                    fprintf(stderr, "send failed: %s\n", strerror(errno));
                    return -1;
                }
                sent_pos += nsent;
                if (sent_pos == msg_len) {
                    close(pfds[0].fd);
                    return 0;
                }
            }
        }
    }
}
