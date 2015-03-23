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

static const libxl__checkpoint_device_instance_ops *colo_ops[] = {
    NULL,
};

/* ================= helper functions ================= */
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

/* ================= colo: setup save environment ================= */
static void colo_save_setup_done(libxl__egc *egc,
                                 libxl__checkpoint_devices_state *cds,
                                 int rc);
static void colo_save_setup_failed(libxl__egc *egc,
                                   libxl__checkpoint_devices_state *cds,
                                   int rc);

void libxl__colo_save_setup(libxl__egc *egc, libxl__colo_save_state *css)
{
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    /* Convenience aliases */
    libxl__checkpoint_devices_state *const cds = &css->cds;

    STATE_AO_GC(dss->ao);

    if (dss->type != LIBXL_DOMAIN_TYPE_HVM) {
        LOG(ERROR, "COLO only supports hvm now");
        goto out;
    }

    css->send_fd = dss->fd;
    css->recv_fd = dss->recv_fd;
    css->svm_running = false;

    /* TODO: disk/nic support */
    cds->device_kind_flags = 0;
    cds->ops = colo_ops;
    cds->callback = colo_save_setup_done;
    cds->ao = ao;
    cds->domid = dss->domid;

    if (init_device_subkind(cds))
        goto out;

    libxl__checkpoint_devices_setup(egc, &css->cds);

    return;

out:
    libxl__ao_complete(egc, ao, ERROR_FAIL);
}

static void colo_save_setup_done(libxl__egc *egc,
                                 libxl__checkpoint_devices_state *cds,
                                 int rc)
{
    libxl__colo_save_state *css = CONTAINER_OF(cds, *css, cds);
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);
    STATE_AO_GC(cds->ao);

    if (!rc) {
        libxl__domain_save(egc, dss);
        return;
    }

    LOG(ERROR, "COLO: failed to setup device for guest with domid %u",
        dss->domid);
    css->cds.callback = colo_save_setup_failed;
    libxl__checkpoint_devices_teardown(egc, &css->cds);
}

static void colo_save_setup_failed(libxl__egc *egc,
                                   libxl__checkpoint_devices_state *cds,
                                   int rc)
{
    STATE_AO_GC(cds->ao);

    if (rc)
        LOG(ERROR, "COLO: failed to teardown device after setup failed"
            " for guest with domid %u, rc %d", cds->domid, rc);

    cleanup_device_subkind(cds);
    libxl__ao_complete(egc, ao, rc);
}


/* ================= colo: teardown save environment ================= */
static void colo_teardown_done(libxl__egc *egc,
                               libxl__checkpoint_devices_state *cds,
                               int rc);

void libxl__colo_save_teardown(libxl__egc *egc,
                               libxl__colo_save_state *css,
                               int rc)
{
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    STATE_AO_GC(css->cds.ao);

    LOG(WARN, "COLO: Domain suspend terminated with rc %d,"
        " teardown COLO devices...", rc);
    dss->css.cds.callback = colo_teardown_done;
    libxl__checkpoint_devices_teardown(egc, &dss->css.cds);
    return;
}

static void colo_teardown_done(libxl__egc *egc,
                               libxl__checkpoint_devices_state *cds,
                               int rc)
{
    libxl__colo_save_state *css = CONTAINER_OF(cds, *css, cds);
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    cleanup_device_subkind(cds);
    dss->callback(egc, dss, rc);
}

/*
 * checkpoint callbacks are called in the following order:
 * 1. suspend
 * 2. resume
 * 3. checkpoint
 */
static void colo_common_read_send_data_done(libxl__egc *egc,
                                            libxl__datacopier_state *dc,
                                            int onwrite, int errnoval);
/* ===================== colo: suspend primary vm ===================== */
/*
 * Do the following things when suspending primary vm:
 * 1. suspend primary vm
 * 2. do postsuspend
 * 3. read LIBXL_COLO_SVM_SUSPENDED
 * 4. read secondary vm's dirty pages
 */
static void colo_suspend_primary_vm_done(libxl__egc *egc,
                                         libxl__domain_suspend_state *dsps,
                                         int ok);
static void colo_postsuspend_cb(libxl__egc *egc,
                                libxl__checkpoint_devices_state *cds,
                                int rc);
static void colo_read_pfn(libxl__egc *egc, libxl__colo_save_state *css);

void libxl__colo_save_domain_suspend_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);

    /* Convenience aliases */
    libxl__domain_suspend_state *dsps = &dss->dsps;

    dsps->callback_common_done = colo_suspend_primary_vm_done;
    libxl__domain_suspend(egc, dsps);
}

static void colo_suspend_primary_vm_done(libxl__egc *egc,
                                         libxl__domain_suspend_state *dsps,
                                         int ok)
{
    libxl__domain_save_state *dss = CONTAINER_OF(dsps, *dss, dsps);

    STATE_AO_GC(dsps->ao);

    if (!ok) {
        LOG(ERROR, "cannot suspend primary vm");
        goto out;
    }

    /* Convenience aliases */
    libxl__checkpoint_devices_state *const cds = &dss->css.cds;

    cds->callback = colo_postsuspend_cb;
    libxl__checkpoint_devices_postsuspend(egc, cds);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, ok);
}

static void colo_postsuspend_cb(libxl__egc *egc,
                                libxl__checkpoint_devices_state *cds,
                                int rc)
{
    int ok = 0;
    libxl__colo_save_state *css = CONTAINER_OF(cds, *css, cds);
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    /* Convenience aliases */
    libxl__datacopier_state *const dc = &css->dc;

    STATE_AO_GC(cds->ao);

    if (rc) {
        LOG(ERROR, "postsuspend fails");
        goto out;
    }

    if (!css->svm_running) {
        ok = 1;
        goto out;
    }

    /*
     * read LIBXL_COLO_SVM_SUSPENDED and the count of
     * secondary vm's dirty pages.
     */
    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = css->recv_fd;
    dc->writefd = -1;
    dc->maxsz = INT_MAX;
    dc->copywhat = "secondary vm is suspended";
    dc->readwhat = "colo stream";
    dc->callback = colo_common_read_send_data_done;
    dc->readbuf = css->temp_buff;
    dc->bytes_to_read = sizeof(css->temp_buff);
    css->callback = colo_read_pfn;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, ok);
}

static void colo_read_pfn(libxl__egc *egc, libxl__colo_save_state *css)
{
    int ok = 0;
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);
    int rc;

    STATE_AO_GC(css->cds.ao);

    /* Convenience aliases */
    libxl__datacopier_state *const dc = &css->dc;

    assert(!css->buff);
    css->section = css->temp_buff[0];
    css->count = *(uint64_t *)(&css->temp_buff[1]);

    if (css->section != LIBXL_COLO_SVM_SUSPENDED) {
        LOG(ERROR, "invalid section: %d, expected: %d",
            css->section, LIBXL_COLO_SVM_SUSPENDED);
        goto out;
    }

    css->buff = libxl__zalloc(NOGC, sizeof(uint64_t) * (css->count + 1));
    css->buff[0] = css->count;

    if (css->count == 0) {
        /* no dirty pages */
        ok = 1;
        goto out;
    }

    /* read the pfn of secondary vm's dirty pages */
    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = css->recv_fd;
    dc->writefd = -1;
    dc->maxsz = INT_MAX;
    dc->copywhat = "secondary vm's dirty bitmap";
    dc->readwhat = "colo stream";
    dc->callback = colo_common_read_send_data_done;
    dc->readbuf = css->buff + 1;
    dc->bytes_to_read = css->count * sizeof(uint64_t);
    css->callback = NULL;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, ok);
}


/* ===================== colo: get dirty pfn ===================== */
void libxl__colo_save_get_dirty_pfn_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    uint64_t size;

    /* Convenience aliases */
    libxl__colo_save_state *const css = &dss->css;

    assert(css->buff);
    size = sizeof(uint64_t) * (css->count + 1);

    libxl__xc_domain_saverestore_async_callback_done_with_data(egc, shs,
                                                               (uint8_t *)css->buff,
                                                               size);
    free(css->buff);
    css->buff = NULL;
}


/* ===================== colo: resume primary vm ===================== */
/*
 * Do the following things when resuming primary vm:
 *  1. read LIBXL_COLO_SVM_READY
 *  2. do preresume
 *  3. resume primary vm
 *  4. read LIBXL_COLO_SVM_RESUMED
 */
static void colo_preresume_dm_saved(libxl__egc *egc,
                                    libxl__domain_save_state *dss, int rc);
static void colo_read_svm_ready_done(libxl__egc *egc,
                                     libxl__colo_save_state *css);
static void colo_preresume_cb(libxl__egc *egc,
                              libxl__checkpoint_devices_state *cds,
                              int rc);
static void colo_read_svm_resumed_done(libxl__egc *egc,
                                       libxl__colo_save_state *css);

void libxl__colo_save_domain_resume_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);

    /* This would go into tailbuf. */
    if (dss->hvm) {
        libxl__domain_save_device_model(egc, dss, colo_preresume_dm_saved);
    } else {
        colo_preresume_dm_saved(egc, dss, 0);
    }

    return;
}

static void colo_preresume_dm_saved(libxl__egc *egc,
                                    libxl__domain_save_state *dss, int rc)
{
    /* Convenience aliases */
    libxl__colo_save_state *const css = &dss->css;
    libxl__datacopier_state *const dc = &css->dc;

    STATE_AO_GC(css->cds.ao);

    if (rc) {
        LOG(ERROR, "Failed to save device model. Terminating COLO..");
        goto out;
    }

    /* read LIBXL_COLO_SVM_READY */
    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = css->recv_fd;
    dc->writefd = -1;
    dc->maxsz = INT_MAX;
    dc->copywhat = "secondary vm is ready";
    dc->readwhat = "colo stream";
    dc->callback = colo_common_read_send_data_done;
    dc->readbuf = &css->section;
    dc->bytes_to_read = sizeof(css->section);
    css->callback = colo_read_svm_ready_done;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 0);
}

static void colo_read_svm_ready_done(libxl__egc *egc,
                                     libxl__colo_save_state *css)
{
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    STATE_AO_GC(css->cds.ao);

    if (css->section != LIBXL_COLO_SVM_READY) {
        LOG(ERROR, "invalid section: %d, expected: %d",
            css->section, LIBXL_COLO_SVM_READY);
        goto out;
    }

    css->svm_running = true;
    css->cds.callback = colo_preresume_cb;
    libxl__checkpoint_devices_preresume(egc, &css->cds);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 0);
}

static void colo_preresume_cb(libxl__egc *egc,
                              libxl__checkpoint_devices_state *cds,
                              int rc)
{
    libxl__colo_save_state *css = CONTAINER_OF(cds, *css, cds);
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    /* Convenience aliases */
    libxl__datacopier_state *const dc = &css->dc;

    STATE_AO_GC(cds->ao);

    if (rc) {
        LOG(ERROR, "preresume fails");
        goto out;
    }

    /* Resumes the domain and the device model */
    if (libxl__domain_resume(gc, dss->domid, /* Fast Suspend */1)) {
        LOG(ERROR, "cannot resume primary vm");
        goto out;
    }

    /* read LIBXL_COLO_SVM_RESUMED */
    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = css->recv_fd;
    dc->writefd = -1;
    dc->maxsz = INT_MAX;
    dc->copywhat = "secondary vm is resumed";
    dc->readwhat = "colo stream";
    dc->callback = colo_common_read_send_data_done;
    dc->readbuf = &css->section;
    dc->bytes_to_read = sizeof(css->section);
    css->callback = colo_read_svm_resumed_done;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 0);
}

static void colo_read_svm_resumed_done(libxl__egc *egc,
                                       libxl__colo_save_state *css)
{
    int ok = 0;
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    STATE_AO_GC(css->cds.ao);

    if (css->section != LIBXL_COLO_SVM_RESUMED) {
        LOG(ERROR, "invalid section: %d, expected: %d",
            css->section, LIBXL_COLO_SVM_RESUMED);
        goto out;
    }

    ok = 1;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, ok);
}


/* ===================== colo: wait new checkpoint ===================== */
/*
 * Do the following things:
 * 1. do commit
 * 2. wait for a new checkpoint
 * 3. write LIBXL_COLO_NEW_CHECKPOINT
 */
static void colo_device_commit_cb(libxl__egc *egc,
                                  libxl__checkpoint_devices_state *cds,
                                  int rc);
static void colo_start_new_checkpoint(libxl__egc *egc,
                                      libxl__checkpoint_devices_state *cds,
                                      int rc);

void libxl__colo_save_domain_checkpoint_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    libxl__egc *egc = dss->shs.egc;

    /* Convenience aliases */
    libxl__checkpoint_devices_state *const cds = &dss->css.cds;

    cds->callback = colo_device_commit_cb;
    libxl__checkpoint_devices_commit(egc, cds);
}

static void colo_device_commit_cb(libxl__egc *egc,
                                  libxl__checkpoint_devices_state *cds,
                                  int rc)
{
    libxl__colo_save_state *css = CONTAINER_OF(cds, *css, cds);
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    STATE_AO_GC(cds->ao);

    if (rc) {
        LOG(ERROR, "commit fails");
        goto out;
    }

    /* TODO: wait a new checkpoint */
    colo_start_new_checkpoint(egc, cds, 0);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 0);
}

static void colo_start_new_checkpoint(libxl__egc *egc,
                                      libxl__checkpoint_devices_state *cds,
                                      int rc)
{
    libxl__colo_save_state *css = CONTAINER_OF(cds, *css, cds);
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);
    uint8_t section = LIBXL_COLO_NEW_CHECKPOINT;

    /* Convenience aliases */
    libxl__datacopier_state *const dc = &css->dc;

    STATE_AO_GC(cds->ao);

    if (rc)
        goto out;

    /* write LIBXL_COLO_NEW_CHECKPOINT */
    memset(dc, 0, sizeof(*dc));
    dc->ao = ao;
    dc->readfd = -1;
    dc->writefd = css->send_fd;
    dc->maxsz = INT_MAX;
    dc->copywhat = "new checkpoint is triggered";
    dc->writewhat = "colo stream";
    dc->callback = colo_common_read_send_data_done;
    css->callback = NULL;

    rc = libxl__datacopier_start(dc);
    if (rc) {
        LOG(ERROR, "libxl__datacopier_start() fails");
        goto out;
    }

    /* tell slave that a new checkpoint is triggered */
    libxl__datacopier_prefixdata(egc, dc, &section, sizeof(section));
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 0);
}


/* ===================== colo: common callback ===================== */
static void colo_common_read_send_data_done(libxl__egc *egc,
                                            libxl__datacopier_state *dc,
                                            int onwrite, int errnoval)
{
    int ok = 0;
    libxl__colo_save_state *css = CONTAINER_OF(dc, *css, dc);
    libxl__domain_save_state *dss = CONTAINER_OF(css, *dss, css);

    STATE_AO_GC(dc->ao);

    if (onwrite == -1) {
        LOG(ERROR, "reading/sending data fails");
        ok = 0;
        goto out;
    }

    if (errnoval < 0 || (onwrite == 1 && errnoval)) {
        /* failure happens when reading/writing, do failover? */
        ok = 2;
        goto out;
    }

    if (dc->bytes_to_read != 0) {
        /* EOF is read */
        LOG(ERROR, "reading EOF unexpectedly");
        ok = 0;
        goto out;
    }

    if (!css->callback) {
        /* Everything is OK */
        ok = 1;
        goto out;
    }

    if (onwrite == 0)
        css->callback(egc, css);
    else
        css->callback(egc, css);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, ok);
}
