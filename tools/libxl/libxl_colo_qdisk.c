/*
 * Copyright (C) 2015 FUJITSU LIMITED
 * Author: Wen Congyang <wency@cn.fujitsu.com>
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

typedef struct libxl__colo_qdisk {
    libxl__checkpoint_device *dev;
} libxl__colo_qdisk;

/* ========== init() and cleanup() ========== */
int init_subkind_qdisk(libxl__checkpoint_devices_state *cds)
{
    /*
     * We don't know if we use qemu block replication, so
     * we cannot start block replication here.
     */
    return 0;
}

void cleanup_subkind_qdisk(libxl__checkpoint_devices_state *cds)
{
}

/* ========== setup() and teardown() ========== */
static void colo_qdisk_setup(libxl__egc *egc, libxl__checkpoint_device *dev,
                             bool primary)
{
    const libxl_device_disk *disk = dev->backend_dev;
    const char *addr = NULL;
    const char *export_name;
    int ret, rc = 0;

    /* Convenience aliases */
    libxl__checkpoint_devices_state *const cds = dev->cds;
    const char *colo_params = disk->colo_params;
    const int domid = cds->domid;

    EGC_GC;

    if (disk->backend != LIBXL_DISK_BACKEND_QDISK ||
        !libxl_defbool_val(disk->colo_enable)) {
        rc = ERROR_CHECKPOINT_DEVOPS_DOES_NOT_MATCH;
        goto out;
    }

    export_name = strstr(colo_params, ":exportname=");
    if (!export_name) {
        rc = ERROR_CHECKPOINT_DEVOPS_DOES_NOT_MATCH;
        goto out;
    }
    export_name += strlen(":exportname=");
    if (export_name[0] == 0) {
        rc = ERROR_CHECKPOINT_DEVOPS_DOES_NOT_MATCH;
        goto out;
    }

    dev->matched = 1;

    if (primary) {
        /* NBD server is not ready, so we cannot start block replication now */
        goto out;
    } else {
        libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);
        int len;

        if (crs->qdisk_setuped)
            goto out;

        crs->qdisk_setuped = true;

        len = export_name - strlen(":exportname=") - colo_params;
        addr = libxl__strndup(gc, colo_params, len);
    }

    ret = libxl__qmp_block_start_replication(gc, domid, primary, addr);
    if (ret)
        rc = ERROR_FAIL;

out:
    dev->aodev.rc = rc;
    dev->aodev.callback(egc, &dev->aodev);
}

static void colo_qdisk_teardown(libxl__egc *egc, libxl__checkpoint_device *dev,
                                bool primary)
{
    int ret, rc = 0;

    /* Convenience aliases */
    libxl__checkpoint_devices_state *const cds = dev->cds;
    const int domid = cds->domid;

    EGC_GC;

    if (primary) {
        libxl__colo_save_state *css = CONTAINER_OF(cds, *css, cds);

        if (!css->qdisk_setuped)
            goto out;

        css->qdisk_setuped = false;
    } else {
        libxl__colo_restore_state *crs = CONTAINER_OF(cds, *crs, cds);

        if (!crs->qdisk_setuped)
            goto out;

        crs->qdisk_setuped = false;
    }

    ret = libxl__qmp_block_stop_replication(gc, domid, primary);
    if (ret)
        rc = ERROR_FAIL;

out:
    dev->aodev.rc = rc;
    dev->aodev.callback(egc, &dev->aodev);
}

/* ========== checkpointing APIs ========== */
/* should be called after libxl__checkpoint_device_instance_ops.preresume */
int colo_qdisk_preresume(libxl_ctx *ctx, domid_t domid)
{
    GC_INIT(ctx);
    int ret;

    ret = libxl__qmp_block_do_checkpoint(gc, domid);

    GC_FREE;
    return ret;
}

static void colo_qdisk_save_preresume(libxl__egc *egc,
                                      libxl__checkpoint_device *dev)
{
    libxl__colo_save_state *css = CONTAINER_OF(dev->cds, *css, cds);
    int ret, rc = 0;

    /* Convenience aliases */
    const int domid = dev->cds->domid;

    EGC_GC;

    if (css->qdisk_setuped)
        goto out;

    css->qdisk_setuped = true;

    ret = libxl__qmp_block_start_replication(gc, domid, true, NULL);
    if (ret)
        rc = ERROR_FAIL;

out:
    dev->aodev.rc = rc;
    dev->aodev.callback(egc, &dev->aodev);
}

/* ======== primary ======== */
static void colo_qdisk_save_setup(libxl__egc *egc,
                                  libxl__checkpoint_device *dev)
{
    colo_qdisk_setup(egc, dev, true);
}

static void colo_qdisk_save_teardown(libxl__egc *egc,
                                   libxl__checkpoint_device *dev)
{
    colo_qdisk_teardown(egc, dev, true);
}

const libxl__checkpoint_device_instance_ops colo_save_device_qdisk = {
    .kind = LIBXL__DEVICE_KIND_VBD,
    .setup = colo_qdisk_save_setup,
    .teardown = colo_qdisk_save_teardown,
    .preresume = colo_qdisk_save_preresume,
};

/* ======== secondary ======== */
static void colo_qdisk_restore_setup(libxl__egc *egc,
                                     libxl__checkpoint_device *dev)
{
    colo_qdisk_setup(egc, dev, false);
}

static void colo_qdisk_restore_teardown(libxl__egc *egc,
                                      libxl__checkpoint_device *dev)
{
    colo_qdisk_teardown(egc, dev, false);
}

const libxl__checkpoint_device_instance_ops colo_restore_device_qdisk = {
    .kind = LIBXL__DEVICE_KIND_VBD,
    .setup = colo_qdisk_restore_setup,
    .teardown = colo_qdisk_restore_teardown,
};
