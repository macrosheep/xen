/*
 * Copyright (C) 2015 FUJITSU LIMITED
 * Author Yang Hongyang <yanghy@cn.fujitsu.com>
 *        Wen congyang <wency@cn.fujitsu.com>
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

/*----- main code for restoring, in order of execution -----*/

int libxl__domain_restore(libxl__gc *gc, uint32_t domid)
{
    int rc = 0;

    libxl_domain_type type = libxl__domain_type(gc, domid);
    if (type != LIBXL_DOMAIN_TYPE_HVM) {
        rc = ERROR_FAIL;
        goto out;
    }

    rc = libxl__domain_restore_device_model(gc, domid);
    if (rc)
        LOG(ERROR, "failed to restore device mode for domain %u:%d",
            domid, rc);
out:
    return rc;
}

int libxl__domain_restore_device_model(libxl__gc *gc, uint32_t domid)
{
    char *state_file;
    int rc;

    switch (libxl__device_model_version_running(gc, domid)) {
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
        /* not supported now */
        rc = ERROR_INVAL;
        break;
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
        /*
         * This function may be called too many times for the same gc,
         * so we use NOGC, and free the memory before return to avoid
         * OOM.
         */
        state_file = libxl__sprintf(NOGC,
                                    XC_DEVICE_MODEL_RESTORE_FILE".%d",
                                    domid);
        rc = libxl__qmp_restore(gc, domid, state_file);
        free(state_file);
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
