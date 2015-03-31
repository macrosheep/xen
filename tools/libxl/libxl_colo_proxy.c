/*
 * Copyright (C) 2015 FUJITSU LIMITED
 * Author: Yang Hongyang <yanghy@cn.fujitsu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"
#include "libxl_colo.h"
#include <linux/netlink.h>

#define NETLINK_COLO 28

enum colo_netlink_op {
    COLO_QUERY_CHECKPOINT = (NLMSG_MIN_TYPE + 1),
    COLO_CHECKPOINT,
    COLO_FAILOVER,
    COLO_PROXY_INIT,
    COLO_PROXY_RESET, /* UNUSED, will be used for continuous FT */
};

/* ========= colo-proxy: helper functions ========== */

static int colo_proxy_send(libxl__colo_proxy_state *cps, uint8_t *buff, uint64_t size, int type)
{
    struct sockaddr_nl sa;
    struct nlmsghdr msg;
    struct iovec iov;
    struct msghdr mh;
    int ret;

    STATE_AO_GC(cps->ao);

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = 0;
    sa.nl_groups = 0;

    msg.nlmsg_len = NLMSG_SPACE(0);
    msg.nlmsg_flags = NLM_F_REQUEST;
    if (type == COLO_PROXY_INIT) {
        msg.nlmsg_flags |= NLM_F_ACK;
    }
    msg.nlmsg_seq = 0;
    /* This is untrusty */
    msg.nlmsg_pid = cps->index;
    msg.nlmsg_type = type;

    iov.iov_base = &msg;
    iov.iov_len = msg.nlmsg_len;

    mh.msg_name = &sa;
    mh.msg_namelen = sizeof(sa);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = NULL;
    mh.msg_controllen = 0;
    mh.msg_flags = 0;

    ret = sendmsg(cps->sock_fd, &mh, 0);
    if (ret <= 0) {
        LOG(ERROR, "can't send msg to kernel by netlink: %s",
            strerror(errno));
    }

    return ret;
}

/* error: return -1, otherwise return 0 */
static int64_t colo_proxy_recv(libxl__colo_proxy_state *cps, uint8_t **buff, int flags)
{
    struct sockaddr_nl sa;
    struct iovec iov;
    struct msghdr mh = {
        .msg_name = &sa,
        .msg_namelen = sizeof(sa),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };
    uint32_t size = 16384;
    int64_t len = 0;
    int ret;

    STATE_AO_GC(cps->ao);
    uint8_t *tmp = libxl__malloc(NOGC, size);

    iov.iov_base = tmp;
    iov.iov_len = size;
next:
   ret = recvmsg(cps->sock_fd, &mh, flags);
    if (ret <= 0) {
        goto out;
    }

    len += ret;
    if (mh.msg_flags & MSG_TRUNC) {
        size += 16384;
        tmp = libxl__realloc(NOGC, tmp, size);
        iov.iov_base = tmp + len;
        iov.iov_len = size - len;
        goto next;
    }

    *buff = tmp;
    return len;

out:
    free(tmp);
    *buff = NULL;
    return ret;
}

/* ========= colo-proxy: setup and teardown ========== */

int colo_proxy_setup(libxl__colo_proxy_state *cps)
{
    int skfd = 0;
    struct sockaddr_nl sa;
    struct nlmsghdr *h;
    struct timeval tv = {0, 500000}; /* timeout for recvmsg from kernel */
    int i = 1;
    int ret = ERROR_FAIL;
    uint8_t *buff = NULL;
    int64_t size;

    STATE_AO_GC(cps->ao);

    skfd = socket(PF_NETLINK, SOCK_RAW, NETLINK_COLO);
    if (skfd < 0) {
        LOG(ERROR, "can not create a netlink socket: %s", strerror(errno));
        goto out;
    }
    cps->sock_fd = skfd;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 0;
retry:
    sa.nl_pid = i++;

    if (i > 10) {
        LOG(ERROR, "netlink bind error");
        goto out;
    }

    ret = bind(skfd, (struct sockaddr *)&sa, sizeof(sa));
    if (ret < 0 && errno == EADDRINUSE) {
        LOG(ERROR, "colo index %d has already in used", sa.nl_pid);
        goto retry;
    }

    cps->index = sa.nl_pid;
    ret = colo_proxy_send(cps, NULL, 0, COLO_PROXY_INIT);
    if (ret < 0) {
        goto out;
    }
    setsockopt(cps->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ret = -1;
    size = colo_proxy_recv(cps, &buff, 0);
    /* disable SO_RCVTIMEO */
    tv.tv_usec = 0;
    setsockopt(cps->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (size < 0) {
        LOG(ERROR, "Can't recv msg from kernel by netlink: %s",
            strerror(errno));
        goto out;
    }

    if (size) {
        h = (struct nlmsghdr *)buff;

        if (h->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(h);
            if (size - sizeof(*h) < sizeof(*err)) {
                goto out;
            }
            ret = -err->error;
            if (ret) {
                goto out;
            }
        }
    }

    ret = 0;

out:
    free(buff);
    if (ret) {
        close(cps->sock_fd);
        cps->sock_fd = -1;
    }
    return ret;
}

void colo_proxy_teardown(libxl__colo_proxy_state *cps)
{
    if (cps->sock_fd >= 0) {
        close(cps->sock_fd);
        cps->sock_fd = -1;
    }
}

/* ========= colo-proxy: preresume, postresume and checkpoint ========== */

void colo_proxy_preresume(libxl__colo_proxy_state *cps)
{
    colo_proxy_send(cps, NULL, 0, COLO_CHECKPOINT);
    /* TODO: need to handle if the call fails... */
}

void colo_proxy_postresume(libxl__colo_proxy_state *cps)
{
    /* nothing to do... */
}


typedef struct colo_msg {
    bool is_checkpoint;
} colo_msg;

/*
do checkpoint: return 1
error: return -1
do not checkpoint: return 0
*/
int colo_proxy_checkpoint(libxl__colo_proxy_state *cps)
{
    uint8_t *buff;
    int64_t size;
    struct nlmsghdr *h;
    struct colo_msg *m;
    int ret = -1;

    size = colo_proxy_recv(cps, &buff, MSG_DONTWAIT);

    /* timeout, return no checkpoint message. */
    if (size <= 0) {
        return 0;
    }

    h = (struct nlmsghdr *) buff;

    if (h->nlmsg_type == NLMSG_ERROR) {
        goto out;
    }

    if (h->nlmsg_len < NLMSG_LENGTH(sizeof(*m))) {
        goto out;
    }

    m = NLMSG_DATA(h);

    ret = m->is_checkpoint ? 1 : 0;

out:
    free(buff);
    return ret;
}
