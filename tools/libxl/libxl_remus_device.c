/*
 * Copyright (C) 2014 FUJITSU LIMITED
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

extern const libxl__remus_device_instance_ops remus_device_nic;
static const libxl__remus_device_instance_ops *remus_ops[] = {
    &remus_device_nic,
    NULL,
};

/*----- helper functions -----*/

static int init_device_subkind(libxl__remus_devices_state *rds)
{
    /* init device subkind-specific state in the libxl ctx */
    int rc;
    STATE_AO_GC(rds->ao);

    if (libxl__netbuffer_enabled(gc)) {
        rc = init_subkind_nic(rds);
        if (rc) goto out;
    }

    rc = 0;
out:
    return rc;
}

static void cleanup_device_subkind(libxl__remus_devices_state *rds)
{
    /* cleanup device subkind-specific state in the libxl ctx */
    STATE_AO_GC(rds->ao);

    if (libxl__netbuffer_enabled(gc))
        cleanup_subkind_nic(rds);
}

/*----- setup() and teardown() -----*/

/* callbacks */

static void devices_setup_cb(libxl__egc *egc,
                             libxl__multidev *multidev,
                             int rc);
static void devices_teardown_cb(libxl__egc *egc,
                                libxl__multidev *multidev,
                                int rc);

/* remus device setup and teardown */

static libxl__remus_device* remus_device_init(libxl__egc *egc,
                                              libxl__remus_devices_state *rds,
                                              libxl__device_kind kind,
                                              void *libxl_dev)
{
    libxl__remus_device *dev = NULL;

    STATE_AO_GC(rds->ao);
    GCNEW(dev);
    dev->backend_dev = libxl_dev;
    dev->kind = kind;
    dev->rds = rds;
    dev->ops_index = -1;

    return dev;
}

static void remus_devices_setup(libxl__egc *egc,
                                libxl__remus_devices_state *rds);

void libxl__remus_devices_setup(libxl__egc *egc, libxl__remus_devices_state *rds)
{
    int i, rc;

    STATE_AO_GC(rds->ao);

    rc = init_device_subkind(rds);
    if (rc)
        goto out;

    rds->num_devices = 0;
    rds->num_nics = 0;
    rds->num_disks = 0;

    if (rds->device_kind_flags & (1 << LIBXL__DEVICE_KIND_REMUS_NIC))
        rds->nics = libxl_device_nic_list(CTX, rds->domid, &rds->num_nics);

    if (rds->device_kind_flags & (1 << LIBXL__DEVICE_KIND_REMUS_DISK))
        rds->disks = libxl_device_disk_list(CTX, rds->domid, &rds->num_disks);

    if (rds->num_nics == 0 && rds->num_disks == 0)
        goto out;

    GCNEW_ARRAY(rds->devs, rds->num_nics + rds->num_disks);

    for (i = 0; i < rds->num_nics; i++) {
        rds->devs[rds->num_devices++] = remus_device_init(egc, rds,
                                                LIBXL__DEVICE_KIND_REMUS_NIC,
                                                &rds->nics[i]);
    }

    for (i = 0; i < rds->num_disks; i++) {
        rds->devs[rds->num_devices++] = remus_device_init(egc, rds,
                                                LIBXL__DEVICE_KIND_REMUS_DISK,
                                                &rds->disks[i]);
    }

    remus_devices_setup(egc, rds);

    return;

out:
    rds->callback(egc, rds, rc);
}

static void remus_devices_setup(libxl__egc *egc,
                                libxl__remus_devices_state *rds)
{
    int i, rc;
    libxl__remus_device *dev;

    STATE_AO_GC(rds->ao);

    libxl__multidev_begin(ao, &rds->multidev);
    rds->multidev.callback = devices_setup_cb;
    for (i = 0; i < rds->num_devices; i++) {
        dev = rds->devs[i];
        if (dev->matched)
            continue;

        /* find avaliable ops */
        do {
            dev->ops = remus_ops[++dev->ops_index];
            if (!dev->ops) {
                rc = ERROR_REMUS_DEVICE_NOT_SUPPORTED;
                goto out;
            }
        } while (dev->ops->kind != dev->kind);

        libxl__multidev_prepare_with_aodev(&rds->multidev, &dev->aodev);
        dev->ops->setup(dev);
    }

    rc = 0;
out:
    libxl__multidev_prepared(egc, &rds->multidev, rc);
}

static void devices_setup_cb(libxl__egc *egc,
                             libxl__multidev *multidev,
                             int rc)
{
    STATE_AO_GC(multidev->ao);

    /* Convenience aliases */
    libxl__remus_devices_state *const rds =
                            CONTAINER_OF(multidev, *rds, multidev);

    /*
     * if the error is ERROR_REMUS_DEVOPS_DOES_NOT_MATCH, begin next iter
     * if there are devices that can't be set up, the rc will become
     * ERROR_FAIL or ERROR_REMUS_DEVICE_NOT_SUPPORTED at last anyway.
     */
    if (rc == ERROR_REMUS_DEVOPS_DOES_NOT_MATCH) {
        remus_devices_setup(egc, rds);
        return;
    }

    rds->callback(egc, rds, rc);
}

void libxl__remus_devices_teardown(libxl__egc *egc,
                                   libxl__remus_devices_state *rds)
{
    int i;
    libxl__remus_device *dev;

    STATE_AO_GC(rds->ao);

    libxl__multidev_begin(ao, &rds->multidev);
    rds->multidev.callback = devices_teardown_cb;
    for (i = 0; i < rds->num_devices; i++) {
        dev = rds->devs[i];
        if (!dev->ops || !dev->matched)
            continue;

        libxl__multidev_prepare_with_aodev(&rds->multidev, &dev->aodev);
        dev->ops->teardown(dev);
    }

    libxl__multidev_prepared(egc, &rds->multidev, 0);
}

static void devices_teardown_cb(libxl__egc *egc,
                                libxl__multidev *multidev,
                                int rc)
{
    int i;

    STATE_AO_GC(multidev->ao);

    /* Convenience aliases */
    libxl__remus_devices_state *const rds =
                            CONTAINER_OF(multidev, *rds, multidev);

    /* clean nic */
    for (i = 0; i < rds->num_nics; i++)
        libxl_device_nic_dispose(&rds->nics[i]);
    free(rds->nics);
    rds->nics = NULL;
    rds->num_nics = 0;

    /* clean disk */
    for (i = 0; i < rds->num_disks; i++)
        libxl_device_disk_dispose(&rds->disks[i]);
    free(rds->disks);
    rds->disks = NULL;
    rds->num_disks = 0;

    cleanup_device_subkind(rds);

    rds->callback(egc, rds, rc);
}

/*----- checkpointing APIs -----*/

/* callbacks */

static void devices_checkpoint_cb(libxl__egc *egc,
                                  libxl__multidev *multidev,
                                  int rc);

/* API implementations */

#define define_remus_checkpoint_api(api)                                \
void libxl__remus_devices_##api(libxl__egc *egc,                        \
                                libxl__remus_devices_state *rds)        \
{                                                                       \
    int i;                                                              \
    libxl__remus_device *dev;                                           \
                                                                        \
    STATE_AO_GC(rds->ao);                                               \
                                                                        \
    libxl__multidev_begin(ao, &rds->multidev);                          \
    rds->multidev.callback = devices_checkpoint_cb;                     \
    for (i = 0; i < rds->num_devices; i++) {                            \
        dev = rds->devs[i];                                             \
        if (!dev->matched || !dev->ops->api)                            \
            continue;                                                   \
        libxl__multidev_prepare_with_aodev(&rds->multidev, &dev->aodev);\
        dev->ops->api(dev);                                             \
    }                                                                   \
                                                                        \
    libxl__multidev_prepared(egc, &rds->multidev, 0);                   \
}

define_remus_checkpoint_api(postsuspend);

define_remus_checkpoint_api(preresume);

define_remus_checkpoint_api(commit);

static void devices_checkpoint_cb(libxl__egc *egc,
                                  libxl__multidev *multidev,
                                  int rc)
{
    STATE_AO_GC(multidev->ao);

    /* Convenience aliases */
    libxl__remus_devices_state *const rds =
                            CONTAINER_OF(multidev, *rds, multidev);

    rds->callback(egc, rds, rc);
}
