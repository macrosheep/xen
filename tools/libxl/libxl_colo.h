/*
 * Copyright (C) 2014 FUJITSU LIMITED
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

#ifndef LIBXL_COLO_H
#define LIBXL_COLO_H

/*
 * values to control suspend/resume primary vm and secondary vm
 * at the same time
 */
enum {
    LIBXL_COLO_NEW_CHECKPOINT = 1,
    LIBXL_COLO_SVM_SUSPENDED,
    LIBXL_COLO_SVM_READY,
    LIBXL_COLO_SVM_RESUMED,
};

extern void libxl__colo_restore_done(libxl__egc *egc, void *dcs_void,
                                     int ret, int retval, int errnoval);
extern void libxl__colo_restore_setup(libxl__egc *egc,
                                      libxl__colo_restore_state *crs);
extern void libxl__colo_restore_teardown(libxl__egc *egc,
                                         libxl__colo_restore_state *crs,
                                         int rc);

#endif
