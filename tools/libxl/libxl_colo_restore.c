/*
 * Copyright (C) 2014 FUJITSU LIMITED
 * Author: Wen Congyang <wency@cn.fujitsu.com>
 *         Yang Hongyang <yanghy@cn.fujitsu.com>
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
#include "libxl_sr_stream_format.h"

enum {
    LIBXL_COLO_SETUPED,
    LIBXL_COLO_SUSPENDED,
    LIBXL_COLO_RESUMED,
};

typedef struct libxl__colo_restore_checkpoint_state libxl__colo_restore_checkpoint_state;
struct libxl__colo_restore_checkpoint_state {
    libxl__domain_suspend_state dsps;
    libxl__logdirty_switch lds;
    libxl__colo_restore_state *crs;
    libxl__stream_write_state sws;
    int status;
    bool preresume;
    /* used for teardown */
    int teardown_devices;
    int saved_rc;

    void (*callback)(libxl__egc *,
                     libxl__colo_restore_checkpoint_state *,
                     int);
};


static void libxl__colo_restore_domain_resume_callback(void *data);
static void libxl__colo_restore_domain_checkpoint_callback(void *data);
static void libxl__colo_restore_domain_should_checkpoint_callback(void *data);
static void libxl__colo_restore_domain_suspend_callback(void *data);

static const libxl__checkpoint_device_instance_ops *colo_restore_ops[] = {
    NULL,
};

/* ===================== colo: common functions ===================== */
static void colo_enable_logdirty(libxl__colo_restore_state *crs, libxl__egc *egc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    const uint32_t domid = crs->domid;
    libxl__logdirty_switch *const lds = &crcs->lds;

    STATE_AO_GC(crs->ao);

    /* we need to know which pages are dirty to restore the guest */
    if (xc_shadow_control(CTX->xch, domid,
                          XEN_DOMCTL_SHADOW_OP_ENABLE_LOGDIRTY,
                          NULL, 0, NULL, 0, NULL) < 0) {
        LOG(ERROR, "cannot enable secondary vm's logdirty");
        lds->callback(egc, lds, ERROR_FAIL);
        return;
    }

    if (crs->hvm) {
        libxl__domain_common_switch_qemu_logdirty(egc, domid, 1, lds);
        return;
    }

    lds->callback(egc, lds, 0);
}

static void colo_disable_logdirty(libxl__colo_restore_state *crs,
                                  libxl__egc *egc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    const uint32_t domid = crs->domid;
    libxl__logdirty_switch *const lds = &crcs->lds;

    STATE_AO_GC(crs->ao);

    /* we need to know which pages are dirty to restore the guest */
    if (xc_shadow_control(CTX->xch, domid, XEN_DOMCTL_SHADOW_OP_OFF,
                          NULL, 0, NULL, 0, NULL) < 0)
        LOG(WARN, "cannot disable secondary vm's logdirty");

    if (crs->hvm) {
        libxl__domain_common_switch_qemu_logdirty(egc, domid, 0, lds);
        return;
    }

    lds->callback(egc, lds, 0);
}

static void colo_resume_vm(libxl__egc *egc,
                           libxl__colo_restore_checkpoint_state *crcs,
                           int restore_device_model)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    int rc;

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;

    STATE_AO_GC(crs->ao);

    if (!crs->saved_cb) {
        /* TODO: sync mmu for hvm? */
        if (restore_device_model) {
            rc = libxl__domain_restore(gc, crs->domid);
            if (rc) {
                LOG(ERROR, "cannot restore device model for secondary vm");
                crcs->callback(egc, crcs, rc);
                return;
            }
        }
        rc = libxl__domain_resume(gc, crs->domid, 0);
        if (rc)
            LOG(ERROR, "cannot resume secondary vm");

        crcs->callback(egc, crcs, rc);
        return;
    }

    libxl__xc_domain_restore_done(egc, dcs, 0, 0, 0);

    return;
}

static int init_device_subkind(libxl__checkpoint_devices_state *cds)
{
    /* init device subkind-specific state in the libxl ctx */
    int rc;
    STATE_AO_GC(cds->ao);

    rc = 0;
    return rc;
}

static void cleanup_device_subkind(libxl__checkpoint_devices_state *cds)
{
    /* cleanup device subkind-specific state in the libxl ctx */
    STATE_AO_GC(cds->ao);
}


/* ================ colo: setup restore environment ================ */
static void libxl__colo_domain_create_cb(libxl__egc *egc,
                                         libxl__domain_create_state *dcs,
                                         int rc, uint32_t domid);

static int init_dsps(libxl__domain_suspend_state *dsps)
{
    int rc = ERROR_FAIL;
    libxl_domain_type type;

    STATE_AO_GC(dsps->ao);

    type = libxl__domain_type(gc, dsps->domid);
    if (type == LIBXL_DOMAIN_TYPE_INVALID)
        goto out;

    libxl__xswait_init(&dsps->pvcontrol);
    libxl__ev_evtchn_init(&dsps->guest_evtchn);
    libxl__ev_xswatch_init(&dsps->guest_watch);
    libxl__ev_time_init(&dsps->guest_timeout);

    if (type == LIBXL_DOMAIN_TYPE_HVM)
        dsps->hvm = 1;
    else
        dsps->hvm = 0;

    dsps->guest_evtchn.port = -1;
    dsps->guest_evtchn_lockfd = -1;
    dsps->guest_responded = 0;
    dsps->dm_savefile = libxl__device_model_savefile(gc, dsps->domid);

    /* Secondary vm is not created, so we cannot get evtchn port */

    rc = 0;

out:
    return rc;
}

void libxl__colo_restore_setup(libxl__egc *egc,
                               libxl__colo_restore_state *crs)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs;
    int rc = ERROR_FAIL;

    /* Convenience aliases */
    libxl__srm_restore_autogen_callbacks *const callbacks =
        &dcs->shs.callbacks.restore.a;
    const int domid = crs->domid;

    STATE_AO_GC(crs->ao);

    GCNEW(crcs);
    crs->crcs = crcs;
    crcs->crs = crs;

    /* setup dsps */
    crcs->dsps.ao = ao;
    crcs->dsps.domid = domid;
    if (init_dsps(&crcs->dsps))
        goto err;

    callbacks->suspend = libxl__colo_restore_domain_suspend_callback;
    callbacks->postcopy = libxl__colo_restore_domain_resume_callback;
    callbacks->checkpoint = libxl__colo_restore_domain_checkpoint_callback;
    callbacks->should_checkpoint = libxl__colo_restore_domain_should_checkpoint_callback;

    /*
     * Secondary vm is running in colo mode, so we need to call
     * libxl__xc_domain_restore_done() to create secondary vm.
     * But we will exit in domain_create_cb(). So replace the
     * callback here.
     */
    crs->saved_cb = dcs->callback;
    dcs->callback = libxl__colo_domain_create_cb;
    crcs->status = LIBXL_COLO_SETUPED;

    libxl__logdirty_init(&crcs->lds);
    crcs->lds.ao = ao;

    crcs->sws.fd = crs->send_fd;
    crcs->sws.ao = ao;
    crcs->sws.back_channel = true;

    libxl__stream_write_start(egc, &crcs->sws);

    rc = 0;

out:
    crs->callback(egc, crs, rc);
    return;

err:
    goto out;
}

static void libxl__colo_domain_create_cb(libxl__egc *egc,
                                         libxl__domain_create_state *dcs,
                                         int rc, uint32_t domid)
{
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;

    crcs->callback(egc, crcs, rc);
}


/* ================ colo: teardown restore environment ================ */
static void colo_restore_teardown_done(libxl__egc *egc,
                                       libxl__checkpoint_devices_state *cds,
                                       int rc);
static void do_failover_done(libxl__egc *egc,
                             libxl__colo_restore_checkpoint_state* crcs,
                             int rc);
static void colo_disable_logdirty_done(libxl__egc *egc,
                                       libxl__logdirty_switch *lds,
                                       int rc);

static void do_failover(libxl__egc *egc, libxl__colo_restore_state *crs)
{
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    const int status = crcs->status;
    libxl__logdirty_switch *const lds = &crcs->lds;

    STATE_AO_GC(crs->ao);

    switch(status) {
    case LIBXL_COLO_SETUPED:
        /* We don't enable logdirty now */
        colo_resume_vm(egc, crcs, 0);
        return;
    case LIBXL_COLO_SUSPENDED:
    case LIBXL_COLO_RESUMED:
        /* disable logdirty first */
        lds->callback = colo_disable_logdirty_done;
        colo_disable_logdirty(crs, egc);
        return;
    default:
        LOG(ERROR, "invalid status: %d", status);
        crcs->callback(egc, crcs, ERROR_FAIL);
    }
}

void libxl__colo_restore_teardown(libxl__egc *egc,
                                  libxl__colo_restore_state *crs,
                                  int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    EGC_GC;

    /* TODO: abort the stream it it is in use. */

    crcs->saved_rc = rc;
    if (!crcs->teardown_devices) {
        colo_restore_teardown_done(egc, &crs->cds, 0);
        return;
    }

    crs->cds.callback = colo_restore_teardown_done;
    libxl__checkpoint_devices_teardown(egc, &crs->cds);
}

static void colo_restore_teardown_done(libxl__egc *egc,
                                       libxl__checkpoint_devices_state *cds,
                                       int rc)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);

    EGC_GC;

    if (rc)
        LOG(ERROR, "COLO: failed to teardown device for guest with domid %u,"
            " rc %d", cds->domid, rc);

    if (crcs->teardown_devices)
        cleanup_device_subkind(cds);

    rc = crcs->saved_rc;
    if (!rc) {
        crcs->callback = do_failover_done;
        do_failover(egc, crs);
        return;
    }

    if (crs->saved_cb) {
        dcs->callback = crs->saved_cb;
        crs->saved_cb = NULL;
    }
    crs->callback(egc, crs, rc);
}

static void do_failover_done(libxl__egc *egc,
                             libxl__colo_restore_checkpoint_state* crcs,
                             int rc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;

    STATE_AO_GC(crs->ao);

    if (rc)
        LOG(ERROR, "cannot do failover");

    if (crs->saved_cb) {
        dcs->callback = crs->saved_cb;
        crs->saved_cb = NULL;
    }

    crs->callback(egc, crs, rc);
}

static void colo_disable_logdirty_done(libxl__egc *egc,
                                       libxl__logdirty_switch *lds,
                                       int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);

    STATE_AO_GC(lds->ao);

    if (rc)
        LOG(WARN, "cannot disable logdirty");

    if (crcs->status == LIBXL_COLO_SUSPENDED) {
        /*
         * failover when reading state from master, so no need to
         * call libxl__domain_restore().
         */
        colo_resume_vm(egc, crcs, 0);
        return;
    }

    /* If we cannot disable logdirty, we still can do failover */
    crcs->callback(egc, crcs, 0);
}

/*
 * checkpoint callbacks are called in the following order:
 * 1. resume
 * 2. should_checkpoint
 * 3. suspend
 * 4. checkpoint
 */
static void colo_common_write_stream_done(libxl__egc *egc,
                                          libxl__stream_write_state *stream,
                                          int rc);
static void colo_common_read_stream_done(libxl__egc *egc,
                                         libxl__stream_read_state *stream,
                                         int rc);
/* ===================== colo: resume secondary vm ===================== */
/*
 * Do the following things when resuming secondary vm the first time:
 *  1. resume secondary vm
 *  2. enable log dirty
 *  3. setup checkpoint devices
 *  4. write LIBXL_COLO_SVM_READY
 *  5. unpause secondary vm
 *  6. write LIBXL_COLO_SVM_RESUMED
 *
 * Do the following things when resuming secondary vm:
 *  1. write LIBXL_COLO_SVM_READY
 *  2. resume secondary vm
 *  3. write LIBXL_COLO_SVM_RESUMED
 */
static void colo_send_svm_ready(libxl__egc *egc,
                                libxl__colo_restore_checkpoint_state *crcs);
static void colo_send_svm_ready_done(libxl__egc *egc,
                                     libxl__colo_restore_checkpoint_state *crcs,
                                     int rc);
static void colo_restore_preresume_cb(libxl__egc *egc,
                                      libxl__checkpoint_devices_state *cds,
                                      int rc);
static void colo_restore_resume_vm(libxl__egc *egc,
                                   libxl__colo_restore_checkpoint_state *crcs);
static void colo_resume_vm_done(libxl__egc *egc,
                                libxl__colo_restore_checkpoint_state *crcs,
                                int rc);
static void colo_write_svm_resumed(libxl__egc *egc,
                                   libxl__colo_restore_checkpoint_state *crcs);
static void colo_enable_logdirty_done(libxl__egc *egc,
                                      libxl__logdirty_switch *lds,
                                      int retval);
static void colo_reenable_logdirty(libxl__egc *egc,
                                   libxl__logdirty_switch *lds,
                                   int rc);
static void colo_reenable_logdirty_done(libxl__egc *egc,
                                        libxl__logdirty_switch *lds,
                                        int rc);
static void colo_setup_checkpoint_devices(libxl__egc *egc,
                                          libxl__colo_restore_state *crs);
static void colo_restore_setup_cds_done(libxl__egc *egc,
                                        libxl__checkpoint_devices_state *cds,
                                        int rc);
static void colo_unpause_svm(libxl__egc *egc,
                             libxl__colo_restore_checkpoint_state *crcs);

static void libxl__colo_restore_domain_resume_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;

    if (crcs->teardown_devices)
        colo_send_svm_ready(shs->egc, crcs);
    else
        colo_restore_resume_vm(shs->egc, crcs);
}

static void colo_send_svm_ready(libxl__egc *egc,
                               libxl__colo_restore_checkpoint_state *crcs)
{
    libxl_sr_colo_context colo_context = { .id = COLO_SVM_READY };

    crcs->callback = colo_send_svm_ready_done;
    crcs->sws.write_records_callback = colo_common_write_stream_done;
    libxl__stream_write_colo_context(egc, &crcs->sws, &colo_context);
}

static void colo_send_svm_ready_done(libxl__egc *egc,
                                     libxl__colo_restore_checkpoint_state *crcs,
                                     int rc)
{
    /* Convenience aliases */
    libxl__checkpoint_devices_state *cds = &crcs->crs->cds;

    if (!crcs->preresume) {
        crcs->preresume = true;
        colo_unpause_svm(egc, crcs);
        return;
    }

    cds->callback = colo_restore_preresume_cb;
    libxl__checkpoint_devices_preresume(egc, cds);
}

static void colo_restore_preresume_cb(libxl__egc *egc,
                                      libxl__checkpoint_devices_state *cds,
                                      int rc)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    if (rc) {
        LOG(ERROR, "preresume fails");
        goto out;
    }

    colo_restore_resume_vm(egc, crcs);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_restore_resume_vm(libxl__egc *egc,
                                   libxl__colo_restore_checkpoint_state *crcs)
{

    crcs->callback = colo_resume_vm_done;
    colo_resume_vm(egc, crcs, 1);
}

static void colo_resume_vm_done(libxl__egc *egc,
                                libxl__colo_restore_checkpoint_state *crcs,
                                int rc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;
    libxl__logdirty_switch *const lds = &crcs->lds;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    if (rc) {
        LOG(ERROR, "cannot resume secondary vm");
        goto out;
    }

    crcs->status = LIBXL_COLO_RESUMED;

    /* avoid calling libxl__xc_domain_restore_done() more than once */
    if (crs->saved_cb) {
        dcs->callback = crs->saved_cb;
        crs->saved_cb = NULL;

        lds->callback = colo_enable_logdirty_done;
        colo_enable_logdirty(crs, egc);
        return;
    }

    colo_write_svm_resumed(egc, crcs);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_write_svm_resumed(libxl__egc *egc,
                                   libxl__colo_restore_checkpoint_state *crcs)
{
    libxl_sr_colo_context colo_context = { .id = COLO_SVM_RESUMED };

    crcs->callback = NULL;
    crcs->sws.write_records_callback = colo_common_write_stream_done;
    libxl__stream_write_colo_context(egc, &crcs->sws, &colo_context);
}

static void colo_enable_logdirty_done(libxl__egc *egc,
                                      libxl__logdirty_switch *lds,
                                      int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;

    STATE_AO_GC(crs->ao);

    if (rc) {
        /*
         * log-dirty already enabled? There's no test op,
         * so attempt to disable then reenable it
         */
        lds->callback = colo_reenable_logdirty;
        colo_disable_logdirty(crs, egc);
        return;
    }

    colo_setup_checkpoint_devices(egc, crs);
}

static void colo_reenable_logdirty(libxl__egc *egc,
                                   libxl__logdirty_switch *lds,
                                   int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__colo_restore_state *const crs = crcs->crs;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    if (rc) {
        LOG(ERROR, "cannot enable logdirty");
        goto out;
    }

    lds->callback = colo_reenable_logdirty_done;
    colo_enable_logdirty(crs, egc);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_reenable_logdirty_done(libxl__egc *egc,
                                        libxl__logdirty_switch *lds,
                                        int rc)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(lds, *crcs, lds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);

    /* Convenience aliases */
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crcs->crs->ao);

    if (rc) {
        LOG(ERROR, "cannot enable logdirty");
        goto out;
    }

    colo_setup_checkpoint_devices(egc, crcs->crs);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

/*
 * We cannot setup checkpoint devices in libxl__colo_restore_setup(),
 * because the guest is not ready.
 */
static void colo_setup_checkpoint_devices(libxl__egc *egc,
                                          libxl__colo_restore_state *crs)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    libxl__checkpoint_devices_state *cds = &crs->cds;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crs->ao);

    /* TODO: disk/nic support */
    cds->device_kind_flags = 0;
    cds->callback = colo_restore_setup_cds_done;
    cds->ao = ao;
    cds->domid = crs->domid;
    cds->ops = colo_restore_ops;

    if (init_device_subkind(cds))
        goto out;

    crcs->teardown_devices = 1;

    libxl__checkpoint_devices_setup(egc, cds);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_restore_setup_cds_done(libxl__egc *egc,
                                        libxl__checkpoint_devices_state *cds,
                                        int rc)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    /* Convenience aliases */
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(cds->ao);

    if (rc) {
        LOG(ERROR, "COLO: failed to setup device for guest with domid %u",
            cds->domid);
        goto out;
    }

    colo_send_svm_ready(egc, crcs);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}

static void colo_unpause_svm(libxl__egc *egc,
                             libxl__colo_restore_checkpoint_state *crcs)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    int rc;

    /* Convenience aliases */
    const uint32_t domid = crcs->crs->domid;
    libxl__save_helper_state *const shs = &dcs->shs;

    STATE_AO_GC(crcs->crs->ao);

    /* We have enabled secondary vm's logdirty, so we can unpause it now */
    rc = libxl_domain_unpause(CTX, domid);
    if (rc) {
        LOG(ERROR, "cannot unpause secondary vm");
        goto out;
    }

    colo_write_svm_resumed(egc, crcs);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, shs, 0);
}


/* ===================== colo: wait new checkpoint ===================== */
static void colo_restore_commit_cb(libxl__egc *egc,
                                   libxl__checkpoint_devices_state *cds,
                                   int rc);
static void colo_stream_read_done(libxl__egc *egc,
                                  libxl__colo_restore_checkpoint_state *crcs,
                                  int real_size);

static void libxl__colo_restore_domain_should_checkpoint_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);

    /* Convenience aliases */
    libxl__checkpoint_devices_state *cds = &dcs->crs.cds;

    cds->callback = colo_restore_commit_cb;
    libxl__checkpoint_devices_commit(shs->egc, cds);
}

static void colo_restore_commit_cb(libxl__egc *egc,
                                   libxl__checkpoint_devices_state *cds,
                                   int rc)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;

    STATE_AO_GC(cds->ao);

    if (rc) {
        LOG(ERROR, "commit fails");
        goto out;
    }

    crcs->callback = colo_stream_read_done;
    dcs->srs.read_records_callback = colo_common_read_stream_done;
    libxl__stream_read_colo_context(egc, &dcs->srs);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, 0);
}

static void colo_stream_read_done(libxl__egc *egc,
                                  libxl__colo_restore_checkpoint_state *crcs,
                                  int id)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    int ok = 0;

    STATE_AO_GC(dcs->ao);

    if (id != COLO_NEW_CHECKPOINT) {
        LOG(ERROR, "invalid section: %d", id);
        goto out;
    }

    ok = 1;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, ok);
}


/* ===================== colo: suspend secondary vm ===================== */
/*
 * Do the following things when resuming secondary vm:
 *  1. suspend secondary vm
 *  2. send LIBXL_COLO_SVM_SUSPENDED
 */
static void colo_suspend_vm_done(libxl__egc *egc,
                                 libxl__domain_suspend_state *dsps,
                                 int ok);
static void colo_restore_postsuspend_cb(libxl__egc *egc,
                                        libxl__checkpoint_devices_state *cds,
                                        int rc);

static void libxl__colo_restore_domain_suspend_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;

    STATE_AO_GC(dcs->ao);

    /* Convenience aliases */
    libxl__domain_suspend_state *const dsps = &crcs->dsps;

    /* suspend secondary vm */
    dsps->callback_common_done = colo_suspend_vm_done;

    libxl__domain_suspend(shs->egc, dsps);
}

static void colo_suspend_vm_done(libxl__egc *egc,
                                 libxl__domain_suspend_state *dsps,
                                 int ok)
{
    libxl__colo_restore_checkpoint_state *crcs = CONTAINER_OF(dsps, *crcs, dsps);
    libxl__colo_restore_state *crs = crcs->crs;
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);

    /* Convenience aliases */
    libxl__checkpoint_devices_state *cds = &crs->cds;

    STATE_AO_GC(crs->ao);

    if (!ok) {
        LOG(ERROR, "cannot suspend secondary vm");
        goto out;
    }

    crcs->status = LIBXL_COLO_SUSPENDED;

    cds->callback = colo_restore_postsuspend_cb;
    libxl__checkpoint_devices_postsuspend(egc, cds);

    return;

out:
    ok = 0;
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, ok);
}

static void colo_restore_postsuspend_cb(libxl__egc *egc,
                                        libxl__checkpoint_devices_state *cds,
                                        int rc)
{
    libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
    libxl__domain_create_state *dcs = CONTAINER_OF(crs, *dcs, crs);
    libxl__colo_restore_checkpoint_state *crcs = crs->crcs;
    libxl_sr_colo_context colo_context = { .id = COLO_SVM_SUSPENDED };
    int ok = 0;

    STATE_AO_GC(crs->ao);

    if (rc) {
        LOG(ERROR, "postsuspend fails");
        goto out;
    }

    crcs->callback = NULL;
    crcs->sws.write_records_callback = colo_common_write_stream_done;
    libxl__stream_write_colo_context(egc, &crcs->sws, &colo_context);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, ok);
}


/* ======================== colo: checkpoint ======================= */
/*
 * Do the following things when resuming secondary vm:
 *  1. read toolstack context
 *  2. read emulator context
 */
static void libxl__colo_restore_domain_checkpoint_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;

    crcs->callback = NULL;
    dcs->srs.read_records_callback = colo_common_read_stream_done;
    libxl__stream_read_colo_context(shs->egc, &dcs->srs);
}

/* ===================== colo: common callback ===================== */
static void colo_common_write_stream_done(libxl__egc *egc,
                                          libxl__stream_write_state *stream,
                                          int rc)
{
    libxl__colo_restore_checkpoint_state *crcs =
        CONTAINER_OF(stream, *crcs, sws);
    libxl__domain_create_state *dcs = CONTAINER_OF(crcs->crs, *dcs, crs);
    int ok;

    STATE_AO_GC(stream->ao);

    if (rc < 0) {
        /* TODO: it may be a internal error, but we don't know */
        LOG(ERROR, "sending data fails");
        ok = 2;
        goto out;
    }

    if (!crcs->callback) {
        /* Everythins is OK */
        ok = 1;
        goto out;
    }

    crcs->callback(egc, crcs, 0);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, ok);
}

static void colo_common_read_stream_done(libxl__egc *egc,
                                         libxl__stream_read_state *stream,
                                         int rc)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(stream, *dcs, srs);
    libxl__colo_restore_checkpoint_state *crcs = dcs->crs.crcs;
    int ok;

    STATE_AO_GC(stream->ao);

    if (rc < 0) {
        /* TODO: it may be a internal error, but we don't know */
        LOG(ERROR, "sending data fails");
        ok = 2;
        goto out;
    }

    if (!crcs->callback) {
        /* Everythins is OK */
        ok = 1;
        goto out;
    }

    /* rc contains the id */
    crcs->callback(egc, crcs, rc);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dcs->shs, ok);
}
