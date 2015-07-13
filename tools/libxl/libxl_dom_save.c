/*
 * Copyright (C) 2009      Citrix Ltd.
 * Author Vincent Hanquez <vincent.hanquez@eu.citrix.com>
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

struct libxl__physmap_info {
    uint64_t phys_offset;
    uint64_t start_addr;
    uint64_t size;
    uint32_t namelen;
    char name[];
};

/* Bump version every time when toolstack saved data changes.
 * Different types of data are arranged in the specified order.
 *
 * Version 1:
 *   uint32_t version
 *   QEMU physmap data:
 *     uint32_t count
 *     libxl__physmap_info * count
 */
#define TOOLSTACK_SAVE_VERSION 1

/*========================= Domain save ============================*/

static void stream_done(libxl__egc *egc,
                        libxl__stream_write_state *sws, int rc);
static void domain_save_done(libxl__egc *egc,
                             libxl__domain_save_state *dss, int rc);

/*----- complicated callback, called by xc_domain_save -----*/

/*
 * We implement the other end of protocol for controlling qemu-dm's
 * logdirty.  There is no documentation for this protocol, but our
 * counterparty's implementation is in
 * qemu-xen-traditional.git:xenstore.c in the function
 * xenstore_process_logdirty_event
 */

static void switch_logdirty_timeout(libxl__egc *egc, libxl__ev_time *ev,
                                    const struct timeval *requested_abs,
                                    int rc);
static void switch_logdirty_xswatch(libxl__egc *egc, libxl__ev_xswatch*,
                            const char *watch_path, const char *event_path);
static void switch_logdirty_done(libxl__egc *egc,
                                 libxl__logdirty_switch *lds, int rc);

static void logdirty_init(libxl__logdirty_switch *lds)
{
    lds->cmd_path = 0;
    libxl__ev_xswatch_init(&lds->watch);
    libxl__ev_time_init(&lds->timeout);
}

static void domain_suspend_switch_qemu_xen_traditional_logdirty
                               (libxl__egc *egc, int domid, unsigned enable,
                                libxl__logdirty_switch *lds)
{
    STATE_AO_GC(lds->ao);
    int rc;
    xs_transaction_t t = 0;
    const char *got;

    if (!lds->cmd_path) {
        uint32_t dm_domid = libxl_get_stubdom_id(CTX, domid);
        lds->cmd_path = libxl__device_model_xs_path(gc, dm_domid, domid,
                                                    "/logdirty/cmd");
        lds->ret_path = libxl__device_model_xs_path(gc, dm_domid, domid,
                                                    "/logdirty/ret");
    }
    lds->cmd = enable ? "enable" : "disable";

    rc = libxl__ev_xswatch_register(gc, &lds->watch,
                                switch_logdirty_xswatch, lds->ret_path);
    if (rc) goto out;

    rc = libxl__ev_time_register_rel(ao, &lds->timeout,
                                switch_logdirty_timeout, 10*1000);
    if (rc) goto out;

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        rc = libxl__xs_read_checked(gc, t, lds->cmd_path, &got);
        if (rc) goto out;

        if (got) {
            const char *got_ret;
            rc = libxl__xs_read_checked(gc, t, lds->ret_path, &got_ret);
            if (rc) goto out;

            if (!got_ret || strcmp(got, got_ret)) {
                LOG(ERROR,"controlling logdirty: qemu was already sent"
                    " command `%s' (xenstore path `%s') but result is `%s'",
                    got, lds->cmd_path, got_ret ? got_ret : "<none>");
                rc = ERROR_FAIL;
                goto out;
            }
            rc = libxl__xs_rm_checked(gc, t, lds->cmd_path);
            if (rc) goto out;
        }

        rc = libxl__xs_rm_checked(gc, t, lds->ret_path);
        if (rc) goto out;

        rc = libxl__xs_write_checked(gc, t, lds->cmd_path, lds->cmd);
        if (rc) goto out;

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc<0) goto out;
    }

    /* OK, wait for some callback */
    return;

 out:
    LOG(ERROR,"logdirty switch failed (rc=%d), abandoning suspend",rc);
    libxl__xs_transaction_abort(gc, &t);
    switch_logdirty_done(egc,lds,rc);
}

static void domain_suspend_switch_qemu_xen_logdirty
                               (libxl__egc *egc, int domid, unsigned enable,
                                libxl__logdirty_switch *lds)
{
    STATE_AO_GC(lds->ao);
    int rc;

    rc = libxl__qmp_set_global_dirty_log(gc, domid, enable);
    if (rc)
        LOG(ERROR,"logdirty switch failed (rc=%d), abandoning suspend",rc);

    lds->callback(egc, lds, rc);
}

static void domain_suspend_switch_qemu_logdirty_done
                        (libxl__egc *egc, libxl__logdirty_switch *lds, int rc)
{
    libxl__domain_save_state *dss = CONTAINER_OF(lds, *dss, logdirty);

    if (rc) {
        dss->rc = rc;
        libxl__xc_domain_saverestore_async_callback_done(egc,
                                                         &dss->sws.shs, -1);
    } else
        libxl__xc_domain_saverestore_async_callback_done(egc, &dss->sws.shs, 0);
}

void libxl__domain_suspend_common_switch_qemu_logdirty
                               (int domid, unsigned enable, void *user)
{
    libxl__save_helper_state *shs = user;
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = shs->caller_state;

    /* convenience aliases */
    libxl__logdirty_switch *const lds = &dss->logdirty;

    lds->callback = domain_suspend_switch_qemu_logdirty_done;
    libxl__domain_common_switch_qemu_logdirty(egc, domid, enable, lds);
}

void libxl__domain_common_switch_qemu_logdirty(libxl__egc *egc,
                                               int domid, unsigned enable,
                                               libxl__logdirty_switch *lds)
{
    STATE_AO_GC(lds->ao);

    switch (libxl__device_model_version_running(gc, domid)) {
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
        domain_suspend_switch_qemu_xen_traditional_logdirty(egc, domid, enable,
                                                            lds);
        break;
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
        domain_suspend_switch_qemu_xen_logdirty(egc, domid, enable, lds);
        break;
    default:
        LOG(ERROR,"logdirty switch failed"
            ", no valid device model version found, abandoning suspend");
        lds->callback(egc, lds, ERROR_FAIL);
    }
}
static void switch_logdirty_timeout(libxl__egc *egc, libxl__ev_time *ev,
                                    const struct timeval *requested_abs,
                                    int rc)
{
    libxl__logdirty_switch *lds = CONTAINER_OF(ev, *lds, timeout);
    STATE_AO_GC(lds->ao);
    LOG(ERROR,"logdirty switch: wait for device model timed out");
    switch_logdirty_done(egc,lds,ERROR_FAIL);
}

static void switch_logdirty_xswatch(libxl__egc *egc, libxl__ev_xswatch *watch,
                            const char *watch_path, const char *event_path)
{
    libxl__logdirty_switch *lds = CONTAINER_OF(watch, *lds, watch);
    STATE_AO_GC(lds->ao);
    const char *got;
    xs_transaction_t t = 0;
    int rc;

    for (;;) {
        rc = libxl__xs_transaction_start(gc, &t);
        if (rc) goto out;

        rc = libxl__xs_read_checked(gc, t, lds->ret_path, &got);
        if (rc) goto out;

        if (!got) {
            rc = +1;
            goto out;
        }

        if (strcmp(got, lds->cmd)) {
            LOG(ERROR,"logdirty switch: sent command `%s' but got reply `%s'"
                " (xenstore paths `%s' / `%s')", lds->cmd, got,
                lds->cmd_path, lds->ret_path);
            rc = ERROR_FAIL;
            goto out;
        }

        rc = libxl__xs_rm_checked(gc, t, lds->cmd_path);
        if (rc) goto out;

        rc = libxl__xs_rm_checked(gc, t, lds->ret_path);
        if (rc) goto out;

        rc = libxl__xs_transaction_commit(gc, &t);
        if (!rc) break;
        if (rc<0) goto out;
    }

 out:
    /* rc < 0: error
     * rc == 0: ok, we are done
     * rc == +1: need to keep waiting
     */
    libxl__xs_transaction_abort(gc, &t);

    if (rc <= 0) {
        if (rc < 0)
            LOG(ERROR,"logdirty switch: failed (rc=%d)",rc);
        switch_logdirty_done(egc,lds,rc);
    }
}

static void switch_logdirty_done(libxl__egc *egc,
                                 libxl__logdirty_switch *lds,
                                 int rc)
{
    STATE_AO_GC(lds->ao);

    libxl__ev_xswatch_deregister(gc, &lds->watch);
    libxl__ev_time_deregister(gc, &lds->timeout);

    lds->callback(egc, lds, rc);
}

/*----- callbacks, called by xc_domain_save -----*/

static inline char *physmap_path(libxl__gc *gc, uint32_t dm_domid,
                                 uint32_t domid,
                                 char *phys_offset, char *node)
{
    return libxl__device_model_xs_path(gc, dm_domid, domid,
                                       "/physmap/%s/%s",
                                       phys_offset, node);
}

int libxl__toolstack_save(uint32_t domid, uint8_t **buf,
        uint32_t *len, void *dss_void)
{
    libxl__domain_save_state *dss = dss_void;
    int ret;
    STATE_AO_GC(dss->ao);
    int i = 0;
    uint32_t version = TOOLSTACK_SAVE_VERSION;
    uint8_t *ptr = NULL;

    ret = -1;

    /* Version number */
    *len = sizeof(version);
    *buf = calloc(1, *len);
    if (*buf == NULL) goto out;
    ptr = *buf;
    memcpy(ptr, &version, sizeof(version));

    /* QEMU physmap data */
    {
        char **entries = NULL, *xs_path;
        struct libxl__physmap_info *pi;
        uint32_t dm_domid;
        char *start_addr = NULL, *size = NULL, *phys_offset = NULL;
        char *name = NULL;
        unsigned int num = 0;
        uint32_t count = 0, namelen = 0;

        dm_domid = libxl_get_stubdom_id(CTX, domid);

        xs_path = libxl__device_model_xs_path(gc, dm_domid, domid,
                                              "/physmap");
        entries = libxl__xs_directory(gc, 0, xs_path, &num);
        count = num;

        *len += sizeof(count);
        *buf = realloc(*buf, *len);
        if (*buf == NULL) goto out;
        ptr = *buf + sizeof(version);
        memcpy(ptr, &count, sizeof(count));
        ptr += sizeof(count);

        for (i = 0; i < count; i++) {
            unsigned long offset;
            phys_offset = entries[i];
            if (phys_offset == NULL) {
                LOG(ERROR, "phys_offset %d is NULL", i);
                goto out;
            }

            xs_path = physmap_path(gc, dm_domid, domid, phys_offset,
                                   "start_addr");
            start_addr = libxl__xs_read(gc, 0, xs_path);
            if (start_addr == NULL) {
                LOG(ERROR, "%s is NULL", xs_path);
                goto out;
            }

            xs_path = physmap_path(gc, dm_domid, domid, phys_offset, "size");
            size = libxl__xs_read(gc, 0, xs_path);
            if (size == NULL) {
                LOG(ERROR, "%s is NULL", xs_path);
                goto out;
            }

            xs_path = physmap_path(gc, dm_domid, domid, phys_offset, "name");
            name = libxl__xs_read(gc, 0, xs_path);
            if (name == NULL)
                namelen = 0;
            else
                namelen = strlen(name) + 1;
            *len += namelen + sizeof(struct libxl__physmap_info);
            offset = ptr - (*buf);
            *buf = realloc(*buf, *len);
            if (*buf == NULL) goto out;
            ptr = (*buf) + offset;
            pi = (struct libxl__physmap_info *) ptr;
            pi->phys_offset = strtoll(phys_offset, NULL, 16);
            pi->start_addr = strtoll(start_addr, NULL, 16);
            pi->size = strtoll(size, NULL, 16);
            pi->namelen = namelen;
            memcpy(pi->name, name, namelen);
            ptr += sizeof(struct libxl__physmap_info) + namelen;
        }
    }

    LOG(DEBUG,"domain=%"PRIu32" toolstack data size=%"PRIu32, domid, *len);

    ret = 0;
out:
    return ret;
}

/*----- main code for saving, in order of execution -----*/

void libxl__domain_save(libxl__egc *egc, libxl__domain_save_state *dss)
{
    STATE_AO_GC(dss->ao);
    int rc = ERROR_FAIL;

    /* Convenience aliases */
    const uint32_t domid = dss->domid;
    const libxl_domain_type type = dss->type;
    const int live = dss->live;
    const int debug = dss->debug;
    const libxl_domain_remus_info *const r_info = dss->remus;
    libxl__srm_save_autogen_callbacks *const callbacks =
        &dss->sws.shs.callbacks.save.a;
    libxl__domain_suspend_state *dsps = &dss->dsps;

    if (dss->checkpointed_stream != LIBXL_CHECKPOINTED_STREAM_NONE &&
        !r_info) {
        LOG(ERROR, "Migration stream is checkpointed, but there's no "
                   "checkpoint info!");
        goto out;
    }

    dss->rc = 0;
    logdirty_init(&dss->logdirty);
    dss->logdirty.ao = ao;

    dsps->ao = ao;
    dsps->domid = domid;
    rc = libxl__domain_suspend_init(egc, dsps);
    if (rc) goto out;

    switch (type) {
    case LIBXL_DOMAIN_TYPE_HVM: {
        dss->hvm = 1;
        break;
    }
    case LIBXL_DOMAIN_TYPE_PV:
        dss->hvm = 0;
        break;
    default:
        abort();
    }

    dss->xcflags = (live ? XCFLAGS_LIVE : 0)
          | (debug ? XCFLAGS_DEBUG : 0)
          | (dss->hvm ? XCFLAGS_HVM : 0);

    if (dss->checkpointed_stream == LIBXL_CHECKPOINTED_STREAM_REMUS) {
        dss->interval = r_info->interval;
        if (libxl_defbool_val(r_info->compression))
            dss->xcflags |= XCFLAGS_CHECKPOINT_COMPRESS;
    }

    memset(callbacks, 0, sizeof(*callbacks));
    if (dss->checkpointed_stream == LIBXL_CHECKPOINTED_STREAM_REMUS) {
        callbacks->suspend = libxl__remus_domain_suspend_callback;
        callbacks->postcopy = libxl__remus_domain_resume_callback;
        callbacks->checkpoint = libxl__remus_domain_save_checkpoint_callback;
    } else
        callbacks->suspend = libxl__domain_suspend_callback;

    callbacks->switch_qemu_logdirty = libxl__domain_suspend_common_switch_qemu_logdirty;

    dss->sws.ao  = dss->ao;
    dss->sws.dss = dss;
    dss->sws.fd  = dss->fd;
    dss->sws.completion_callback = stream_done;

    libxl__stream_write_start(egc, &dss->sws);
    return;

 out:
    domain_save_done(egc, dss, rc);
}

static void stream_done(libxl__egc *egc,
                        libxl__stream_write_state *sws, int rc)
{
    domain_save_done(egc, sws->dss, rc);
}

static void save_device_model_datacopier_done(libxl__egc *egc,
     libxl__datacopier_state *dc, int rc, int onwrite, int errnoval);

void libxl__domain_save_device_model(libxl__egc *egc,
                                     libxl__domain_save_state *dss,
                                     libxl__save_device_model_cb *callback)
{
    STATE_AO_GC(dss->ao);
    struct stat st;
    uint32_t qemu_state_len;
    int rc;

    dss->save_dm_callback = callback;

    /* Convenience aliases */
    const char *const filename = dss->dsps.dm_savefile;
    const int fd = dss->fd;

    libxl__datacopier_state *dc = &dss->save_dm_datacopier;
    memset(dc, 0, sizeof(*dc));
    dc->readwhat = GCSPRINTF("qemu save file %s", filename);
    dc->ao = ao;
    dc->readfd = -1;
    dc->writefd = fd;
    dc->maxsz = INT_MAX;
    dc->bytes_to_read = -1;
    dc->copywhat = GCSPRINTF("qemu save file for domain %"PRIu32, dss->domid);
    dc->writewhat = "save/migration stream";
    dc->callback = save_device_model_datacopier_done;

    dc->readfd = open(filename, O_RDONLY);
    if (dc->readfd < 0) {
        LOGE(ERROR, "unable to open %s", dc->readwhat);
        rc = ERROR_FAIL;
        goto out;
    }

    if (fstat(dc->readfd, &st))
    {
        LOGE(ERROR, "unable to fstat %s", dc->readwhat);
        rc = ERROR_FAIL;
        goto out;
    }

    if (!S_ISREG(st.st_mode)) {
        LOG(ERROR, "%s is not a plain file!", dc->readwhat);
        rc = ERROR_FAIL;
        goto out;
    }

    qemu_state_len = st.st_size;
    LOG(DEBUG, "%s is %d bytes", dc->readwhat, qemu_state_len);

    rc = libxl__datacopier_start(dc);
    if (rc) goto out;

    libxl__datacopier_prefixdata(egc, dc,
                                 QEMU_SIGNATURE, strlen(QEMU_SIGNATURE));

    libxl__datacopier_prefixdata(egc, dc,
                                 &qemu_state_len, sizeof(qemu_state_len));
    return;

 out:
    save_device_model_datacopier_done(egc, dc, rc, -1, EIO);
}

static void save_device_model_datacopier_done(libxl__egc *egc,
     libxl__datacopier_state *dc, int our_rc, int onwrite, int errnoval)
{
    libxl__domain_save_state *dss =
        CONTAINER_OF(dc, *dss, save_dm_datacopier);
    STATE_AO_GC(dss->ao);

    /* Convenience aliases */
    const char *const filename = dss->dsps.dm_savefile;
    int rc;

    libxl__datacopier_kill(dc);

    if (dc->readfd >= 0) {
        close(dc->readfd);
        dc->readfd = -1;
    }

    rc = libxl__remove_file(gc, filename);
    if (!our_rc) our_rc = rc;

    dss->save_dm_callback(egc, dss, our_rc);
}

static void domain_save_done(libxl__egc *egc,
                             libxl__domain_save_state *dss, int rc)
{
    STATE_AO_GC(dss->ao);

    /* Convenience aliases */
    const uint32_t domid = dss->domid;
    libxl__domain_suspend_state *dsps = &dss->dsps;

    libxl__ev_evtchn_cancel(gc, &dsps->guest_evtchn);

    if (dsps->guest_evtchn.port > 0)
        xc_suspend_evtchn_release(CTX->xch, CTX->xce, domid,
                        dsps->guest_evtchn.port, &dsps->guest_evtchn_lockfd);

    if (dss->remus) {
        /*
         * With Remus, if we reach this point, it means either
         * backup died or some network error occurred preventing us
         * from sending checkpoints. Teardown the network buffers and
         * release netlink resources.  This is an async op.
         */
        libxl__remus_teardown(egc, dss, rc);
        return;
    }

    dss->callback(egc, dss, rc);
}

/*========================= Domain restore ============================*/

static inline char *restore_helper(libxl__gc *gc, uint32_t dm_domid,
                                   uint32_t domid,
                                   uint64_t phys_offset, char *node)
{
    return libxl__device_model_xs_path(gc, dm_domid, domid,
                                       "/physmap/%"PRIx64"/%s",
                                       phys_offset, node);
}

static int libxl__toolstack_restore_qemu(libxl__gc *gc, uint32_t domid,
                                         const uint8_t *ptr, uint32_t size)
{
    int ret, i;
    uint32_t count;
    char *xs_path;
    uint32_t dm_domid;
    struct libxl__physmap_info *pi;

    if (size < sizeof(count)) {
        LOG(ERROR, "wrong size");
        ret = -1;
        goto out;
    }

    memcpy(&count, ptr, sizeof(count));
    ptr += sizeof(count);

    if (size < sizeof(count) + count*(sizeof(struct libxl__physmap_info))) {
        LOG(ERROR, "wrong size");
        ret = -1;
        goto out;
    }

    dm_domid = libxl_get_stubdom_id(CTX, domid);
    for (i = 0; i < count; i++) {
        pi = (struct libxl__physmap_info*) ptr;
        ptr += sizeof(struct libxl__physmap_info) + pi->namelen;

        xs_path = restore_helper(gc, dm_domid, domid,
                                 pi->phys_offset, "start_addr");
        ret = libxl__xs_write(gc, 0, xs_path, "%"PRIx64, pi->start_addr);
        if (ret) goto out;

        xs_path = restore_helper(gc, dm_domid, domid, pi->phys_offset, "size");
        ret = libxl__xs_write(gc, 0, xs_path, "%"PRIx64, pi->size);
        if (ret) goto out;

        if (pi->namelen > 0) {
            xs_path = restore_helper(gc, dm_domid, domid,
                                     pi->phys_offset, "name");
            ret = libxl__xs_write(gc, 0, xs_path, "%s", pi->name);
            if (ret) goto out;
        }
    }

    ret = 0;
out:
    return ret;

}

static int libxl__toolstack_restore_v1(libxl__gc *gc, uint32_t domid,
                                       const uint8_t *ptr, uint32_t size)
{
    return libxl__toolstack_restore_qemu(gc, domid, ptr, size);
}

int libxl__toolstack_restore(uint32_t domid, const uint8_t *ptr,
                             uint32_t size, void *user)
{
    libxl__save_helper_state *shs = user;
    libxl__domain_create_state *dcs = shs->caller_state;
    STATE_AO_GC(dcs->ao);
    int ret;
    uint32_t version = 0, bufsize;

    LOG(DEBUG,"domain=%"PRIu32" toolstack data size=%"PRIu32, domid, size);

    if (size < sizeof(version)) {
        LOG(ERROR, "wrong size");
        ret = -1;
        goto out;
    }

    memcpy(&version, ptr, sizeof(version));
    ptr += sizeof(version);
    bufsize = size - sizeof(version);

    switch (version) {
    case 1:
        ret = libxl__toolstack_restore_v1(gc, domid, ptr, bufsize);
        break;
    default:
        LOG(ERROR, "wrong version");
        ret = -1;
    }

out:
    return ret;
}

int libxl__domain_restore_device_model(libxl__gc *gc, uint32_t domid,
                                       const char *state_file)
{
    int rc;

    switch (libxl__device_model_version_running(gc, domid)) {
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
        /* not supported now */
        rc = ERROR_INVAL;
        break;
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
        rc = libxl__qmp_restore(gc, domid, state_file);
        break;
    default:
        rc = ERROR_INVAL;
    }

    return rc;
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
