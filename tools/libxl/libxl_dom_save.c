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

/*----- complicated callback, called by xc_domain_save -----*/

/*
 * We implement the other end of protocol for controlling qemu-dm's
 * logdirty.  There is no documentation for this protocol, but our
 * counterparty's implementation is in
 * qemu-xen-traditional.git:xenstore.c in the function
 * xenstore_process_logdirty_event
 */

static void switch_logdirty_timeout(libxl__egc *egc, libxl__ev_time *ev,
                                    const struct timeval *requested_abs);
static void switch_logdirty_xswatch(libxl__egc *egc, libxl__ev_xswatch*,
                            const char *watch_path, const char *event_path);
static void switch_logdirty_done(libxl__egc *egc,
                                 libxl__domain_save_state *dss, int ok);

static void logdirty_init(libxl__logdirty_switch *lds)
{
    lds->cmd_path = 0;
    libxl__ev_xswatch_init(&lds->watch);
    libxl__ev_time_init(&lds->timeout);
}

static void domain_suspend_switch_qemu_xen_traditional_logdirty
                               (int domid, unsigned enable,
                                libxl__save_helper_state *shs)
{
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    libxl__logdirty_switch *lds = &dss->logdirty;
    STATE_AO_GC(dss->ao);
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

    rc = libxl__ev_time_register_rel(gc, &lds->timeout,
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
    LOG(ERROR,"logdirty switch failed (rc=%d), aborting suspend",rc);
    libxl__xs_transaction_abort(gc, &t);
    switch_logdirty_done(egc,dss,-1);
}

static void domain_suspend_switch_qemu_xen_logdirty
                               (int domid, unsigned enable,
                                libxl__save_helper_state *shs)
{
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    STATE_AO_GC(dss->ao);
    int rc;

    rc = libxl__qmp_set_global_dirty_log(gc, domid, enable);
    if (!rc) {
        libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
    } else {
        LOG(ERROR,"logdirty switch failed (rc=%d), aborting suspend",rc);
        libxl__xc_domain_saverestore_async_callback_done(egc, shs, -1);
    }
}

void libxl__domain_suspend_common_switch_qemu_logdirty
                               (int domid, unsigned enable, void *user)
{
    libxl__save_helper_state *shs = user;
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    STATE_AO_GC(dss->ao);

    switch (libxl__device_model_version_running(gc, domid)) {
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
        domain_suspend_switch_qemu_xen_traditional_logdirty(domid, enable, shs);
        break;
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
        domain_suspend_switch_qemu_xen_logdirty(domid, enable, shs);
        break;
    default:
        LOG(ERROR,"logdirty switch failed"
            ", no valid device model version found, aborting suspend");
        libxl__xc_domain_saverestore_async_callback_done(egc, shs, -1);
    }
}
static void switch_logdirty_timeout(libxl__egc *egc, libxl__ev_time *ev,
                                    const struct timeval *requested_abs)
{
    libxl__domain_save_state *dss = CONTAINER_OF(ev, *dss, logdirty.timeout);
    STATE_AO_GC(dss->ao);
    LOG(ERROR,"logdirty switch: wait for device model timed out");
    switch_logdirty_done(egc,dss,-1);
}

static void switch_logdirty_xswatch(libxl__egc *egc, libxl__ev_xswatch *watch,
                            const char *watch_path, const char *event_path)
{
    libxl__domain_save_state *dss =
        CONTAINER_OF(watch, *dss, logdirty.watch);
    libxl__logdirty_switch *lds = &dss->logdirty;
    STATE_AO_GC(dss->ao);
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

    if (!rc) {
        switch_logdirty_done(egc,dss,0);
    } else if (rc < 0) {
        LOG(ERROR,"logdirty switch: failed (rc=%d)",rc);
        switch_logdirty_done(egc,dss,-1);
    }
}

static void switch_logdirty_done(libxl__egc *egc,
                                 libxl__domain_save_state *dss,
                                 int broke)
{
    STATE_AO_GC(dss->ao);
    libxl__logdirty_switch *lds = &dss->logdirty;

    libxl__ev_xswatch_deregister(gc, &lds->watch);
    libxl__ev_time_deregister(gc, &lds->timeout);

    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, broke);
}

/*----- main code for saving, in order of execution -----*/

static void domain_save_done(libxl__egc *egc,
                             libxl__domain_save_state *dss, int rc);

void libxl__domain_save(libxl__egc *egc, libxl__domain_save_state *dss)
{
    STATE_AO_GC(dss->ao);
    int port;
    int rc = ERROR_FAIL;

    /* Convenience aliases */
    const uint32_t domid = dss->domid;
    const libxl_domain_type type = dss->type;
    const int live = dss->live;
    const int debug = dss->debug;
    const libxl_domain_remus_info *const r_info = dss->remus;
    libxl__srm_save_autogen_callbacks *const callbacks =
        &dss->shs.callbacks.save.a;
    libxl__domain_suspend_state *dss2 = &dss->dss;

    logdirty_init(&dss->logdirty);
    libxl__xswait_init(&dss2->pvcontrol);
    libxl__ev_evtchn_init(&dss2->guest_evtchn);
    libxl__ev_xswatch_init(&dss2->guest_watch);
    libxl__ev_time_init(&dss2->guest_timeout);

    switch (type) {
    case LIBXL_DOMAIN_TYPE_HVM: {
        dss->hvm = 1;
        dss2->hvm = 1;
        break;
    }
    case LIBXL_DOMAIN_TYPE_PV:
        dss->hvm = 0;
        dss2->hvm = 0;
        break;
    default:
        abort();
    }

    dss->xcflags = (live ? XCFLAGS_LIVE : 0)
          | (debug ? XCFLAGS_DEBUG : 0)
          | (dss->hvm ? XCFLAGS_HVM : 0);

    dss2->guest_evtchn.port = -1;
    dss2->guest_evtchn_lockfd = -1;
    dss2->guest_responded = 0;
    dss2->dm_savefile = libxl__device_model_savefile(gc, domid);
    dss2->domid = domid;
    dss2->ao = ao;

    if (r_info != NULL) {
        dss->interval = r_info->interval;
        dss->xcflags |= XCFLAGS_CHECKPOINTED;
        if (libxl_defbool_val(r_info->compression))
            dss->xcflags |= XCFLAGS_CHECKPOINT_COMPRESS;
    }

    port = xs_suspend_evtchn_port(dss2->domid);

    if (port >= 0) {
        rc = libxl__ctx_evtchn_init(gc);
        if (rc) goto out;

        dss2->guest_evtchn.port =
            xc_suspend_evtchn_init_exclusive(CTX->xch, CTX->xce,
                                  dss2->domid, port, &dss2->guest_evtchn_lockfd);

        if (dss2->guest_evtchn.port < 0) {
            LOG(WARN, "Suspend event channel initialization failed");
            rc = ERROR_FAIL;
            goto out;
        }
    }

    memset(callbacks, 0, sizeof(*callbacks));
    if (r_info != NULL) {
        callbacks->suspend = libxl__remus_domain_suspend_callback;
        callbacks->postcopy = libxl__remus_domain_resume_callback;
        callbacks->checkpoint = libxl__remus_domain_checkpoint_callback;
    } else
        callbacks->suspend = libxl__domain_suspend_callback;

    callbacks->switch_qemu_logdirty = libxl__domain_suspend_common_switch_qemu_logdirty;
    dss->shs.callbacks.save.toolstack_save = libxl__toolstack_save;

    libxl__xc_domain_save(egc, dss);
    return;

 out:
    domain_save_done(egc, dss, rc);
}

void libxl__xc_domain_save_done(libxl__egc *egc, void *dss_void,
                                int rc, int retval, int errnoval)
{
    libxl__domain_save_state *dss = dss_void;
    STATE_AO_GC(dss->ao);

    /* Convenience aliases */
    const libxl_domain_type type = dss->type;
    libxl__domain_suspend_state *dss2 = &dss->dss;

    if (rc)
        goto out;

    if (retval) {
        LOGEV(ERROR, errnoval, "saving domain: %s",
                         dss2->guest_responded ?
                         "domain responded to suspend request" :
                         "domain did not respond to suspend request");
        if ( !dss2->guest_responded )
            rc = ERROR_GUEST_TIMEDOUT;
        else
            rc = ERROR_FAIL;
        goto out;
    }

    if (type == LIBXL_DOMAIN_TYPE_HVM) {
        rc = libxl__domain_suspend_device_model(gc, dss2);
        if (rc) goto out;

        libxl__domain_save_device_model(egc, dss, domain_save_done);
        return;
    }

    rc = 0;

out:
    domain_save_done(egc, dss, rc);
}

static void save_device_model_datacopier_done(libxl__egc *egc,
     libxl__datacopier_state *dc, int onwrite, int errnoval);

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
    const char *const filename = dss->dss.dm_savefile;
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
        goto out;
    }

    if (fstat(dc->readfd, &st))
    {
        LOGE(ERROR, "unable to fstat %s", dc->readwhat);
        goto out;
    }

    if (!S_ISREG(st.st_mode)) {
        LOG(ERROR, "%s is not a plain file!", dc->readwhat);
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
    save_device_model_datacopier_done(egc, dc, -1, 0);
}

static void save_device_model_datacopier_done(libxl__egc *egc,
     libxl__datacopier_state *dc, int onwrite, int errnoval)
{
    libxl__domain_save_state *dss =
        CONTAINER_OF(dc, *dss, save_dm_datacopier);
    STATE_AO_GC(dss->ao);

    /* Convenience aliases */
    const char *const filename = dss->dss.dm_savefile;
    int our_rc = 0;
    int rc;

    libxl__datacopier_kill(dc);

    if (onwrite || errnoval)
        our_rc = ERROR_FAIL;

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
    libxl__domain_suspend_state *dss2 = &dss->dss;

    libxl__ev_evtchn_cancel(gc, &dss2->guest_evtchn);

    if (dss2->guest_evtchn.port > 0)
        xc_suspend_evtchn_release(CTX->xch, CTX->xce, domid,
                           dss2->guest_evtchn.port, &dss2->guest_evtchn_lockfd);

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
/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
