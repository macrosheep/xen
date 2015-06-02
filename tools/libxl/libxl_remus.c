/*
 * Copyright (C) 2015 FUJITSU LIMITED
 * Author: Yang Hongyang <yanghy@cn.fujitsu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2 and later. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"

/*----- Remus setup and teardown -----*/

static void libxl_remus_setup_done(libxl__egc *egc,
                                   libxl__checkpoint_devices_state *cds, int rc);
static void libxl_remus_setup_failed(libxl__egc *egc,
                                     libxl__checkpoint_devices_state *cds, int rc);
void libxl__remus_setup(libxl__egc *egc, libxl__domain_save_state *dss)
{
    /* Convenience aliases */
    libxl__checkpoint_devices_state *const cds = &dss->cds;
    const libxl_domain_remus_info *const info = dss->remus;

    STATE_AO_GC(dss->ao);

    if (libxl_defbool_val(info->netbuf)) {
        if (!libxl__netbuffer_enabled(gc)) {
            LOG(ERROR, "Remus: No support for network buffering");
            goto out;
        }
        cds->device_kind_flags |= (1 << LIBXL__DEVICE_KIND_VIF);
    }

    if (libxl_defbool_val(info->diskbuf))
        cds->device_kind_flags |= (1 << LIBXL__DEVICE_KIND_VBD);

    cds->ao = ao;
    cds->domid = dss->domid;
    cds->callback = libxl_remus_setup_done;

    libxl__checkpoint_devices_setup(egc, cds);
    return;

out:
    libxl_remus_setup_failed(egc, cds, ERROR_FAIL);
}

static void libxl_remus_setup_done(libxl__egc *egc,
                                   libxl__checkpoint_devices_state *cds, int rc)
{
    libxl__domain_save_state *dss = CONTAINER_OF(cds, *dss, cds);
    STATE_AO_GC(dss->ao);

    if (!rc) {
        libxl__domain_save(egc, dss);
        return;
    }

    LOG(ERROR, "Remus: failed to setup device for guest with domid %u, rc %d",
        dss->domid, rc);
    cds->callback = libxl_remus_setup_failed;
    libxl__checkpoint_devices_teardown(egc, cds);
}

static void libxl_remus_setup_failed(libxl__egc *egc,
                                     libxl__checkpoint_devices_state *cds, int rc)
{
    libxl__domain_save_state *dss = CONTAINER_OF(cds, *dss, cds);
    STATE_AO_GC(dss->ao);

    if (rc)
        LOG(ERROR, "Remus: failed to teardown device after setup failed"
            " for guest with domid %u, rc %d", dss->domid, rc);

    dss->callback(egc, dss, rc);
}

static void remus_teardown_done(libxl__egc *egc,
                                libxl__checkpoint_devices_state *cds,
                                int rc);
void libxl__remus_teardown(libxl__egc *egc,
                           libxl__domain_save_state *dss,
                           int rc)
{
    EGC_GC;

    LOG(WARN, "Remus: Domain suspend terminated with rc %d,"
        " teardown Remus devices...", rc);
    dss->cds.callback = remus_teardown_done;
    libxl__checkpoint_devices_teardown(egc, &dss->cds);
}

static void remus_teardown_done(libxl__egc *egc,
                                libxl__checkpoint_devices_state *cds,
                                int rc)
{
    libxl__domain_save_state *dss = CONTAINER_OF(cds, *dss, cds);
    STATE_AO_GC(dss->ao);

    if (rc)
        LOG(ERROR, "Remus: failed to teardown device for guest with domid %u,"
            " rc %d", dss->domid, rc);

    dss->callback(egc, dss, rc);
}

/*----- remus callbacks -----*/
static void remus_domain_suspend_callback_common_done(libxl__egc *egc,
                                libxl__domain_suspend_state *dss, int ok);
static void remus_devices_postsuspend_cb(libxl__egc *egc,
                                         libxl__checkpoint_devices_state *cds,
                                         int rc);
static void remus_devices_preresume_cb(libxl__egc *egc,
                                       libxl__checkpoint_devices_state *cds,
                                       int rc);

void libxl__remus_domain_suspend_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    libxl__domain_suspend_state *dss2 = &dss->dss;

    dss2->callback_common_done = remus_domain_suspend_callback_common_done;
    libxl__domain_suspend(egc, dss2);
}

static void remus_domain_suspend_callback_common_done(libxl__egc *egc,
                                libxl__domain_suspend_state *dss, int ok)
{
    libxl__domain_save_state *dsvs = CONTAINER_OF(dss, *dsvs, dss);

    if (!ok)
        goto out;

    libxl__checkpoint_devices_state *const cds = &dsvs->cds;
    cds->callback = remus_devices_postsuspend_cb;
    libxl__checkpoint_devices_postsuspend(egc, cds);
    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dsvs->shs, ok);
}

static void remus_devices_postsuspend_cb(libxl__egc *egc,
                                         libxl__checkpoint_devices_state *cds,
                                         int rc)
{
    int ok = 0;
    libxl__domain_save_state *dss = CONTAINER_OF(cds, *dss, cds);

    if (rc)
        goto out;

    ok = 1;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, ok);
}

void libxl__remus_domain_resume_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__egc *egc = shs->egc;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    STATE_AO_GC(dss->ao);

    libxl__checkpoint_devices_state *const cds = &dss->cds;
    cds->callback = remus_devices_preresume_cb;
    libxl__checkpoint_devices_preresume(egc, cds);
}

static void remus_devices_preresume_cb(libxl__egc *egc,
                                       libxl__checkpoint_devices_state *cds,
                                       int rc)
{
    int ok = 0;
    libxl__domain_save_state *dss = CONTAINER_OF(cds, *dss, cds);
    STATE_AO_GC(dss->ao);

    if (rc)
        goto out;

    /* Resumes the domain and the device model */
    rc = libxl__domain_resume(gc, dss->domid, /* Fast Suspend */1);
    if (rc)
        goto out;

    ok = 1;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, ok);
}

/*----- remus asynchronous checkpoint callback -----*/

static void remus_checkpoint_dm_saved(libxl__egc *egc,
                                      libxl__domain_save_state *dss, int rc);
static void remus_devices_commit_cb(libxl__egc *egc,
                                    libxl__checkpoint_devices_state *cds,
                                    int rc);
static void remus_next_checkpoint(libxl__egc *egc, libxl__ev_time *ev,
                                  const struct timeval *requested_abs);

void libxl__remus_domain_checkpoint_callback(void *data)
{
    libxl__save_helper_state *shs = data;
    libxl__domain_save_state *dss = CONTAINER_OF(shs, *dss, shs);
    libxl__egc *egc = dss->shs.egc;
    STATE_AO_GC(dss->ao);

    /* This would go into tailbuf. */
    if (dss->hvm) {
        libxl__domain_save_device_model(egc, dss, remus_checkpoint_dm_saved);
    } else {
        remus_checkpoint_dm_saved(egc, dss, 0);
    }
}

static void remus_checkpoint_dm_saved(libxl__egc *egc,
                                      libxl__domain_save_state *dss, int rc)
{
    /* Convenience aliases */
    libxl__checkpoint_devices_state *const cds = &dss->cds;

    STATE_AO_GC(dss->ao);

    if (rc) {
        LOG(ERROR, "Failed to save device model. Terminating Remus..");
        goto out;
    }

    cds->callback = remus_devices_commit_cb;
    libxl__checkpoint_devices_commit(egc, cds);

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 0);
}

static void remus_devices_commit_cb(libxl__egc *egc,
                                    libxl__checkpoint_devices_state *cds,
                                    int rc)
{
    libxl__domain_save_state *dss = CONTAINER_OF(cds, *dss, cds);

    STATE_AO_GC(dss->ao);

    if (rc) {
        LOG(ERROR, "Failed to do device commit op."
            " Terminating Remus..");
        goto out;
    }

    /*
     * At this point, we have successfully checkpointed the guest and
     * committed it at the backup. We'll come back after the checkpoint
     * interval to checkpoint the guest again. Until then, let the guest
     * continue execution.
     */

    /* Set checkpoint interval timeout */
    rc = libxl__ev_time_register_rel(gc, &dss->checkpoint_timeout,
                                     remus_next_checkpoint,
                                     dss->interval);

    if (rc)
        goto out;

    return;

out:
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 0);
}

static void remus_next_checkpoint(libxl__egc *egc, libxl__ev_time *ev,
                                  const struct timeval *requested_abs)
{
    libxl__domain_save_state *dss =
                            CONTAINER_OF(ev, *dss, checkpoint_timeout);

    STATE_AO_GC(dss->ao);

    /*
     * Time to checkpoint the guest again. We return 1 to libxc
     * (xc_domain_save.c). in order to continue executing the infinite loop
     * (suspend, checkpoint, resume) in xc_domain_save().
     */
    libxl__xc_domain_saverestore_async_callback_done(egc, &dss->shs, 1);
}
/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
