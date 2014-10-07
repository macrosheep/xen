/*
 * Copyright (C) 2014 FUJITSU LIMITED
 * Author Wen Congyang <wency@cn.fujitsu.com>
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

#ifndef LIBXL_REMUS_H
#define LIBXL_REMUS_H

void libxl__remus_setup(libxl__egc *egc,
                        libxl__domain_suspend_state *dss);
void libxl__remus_teardown(libxl__egc *egc,
                           libxl__domain_suspend_state *dss,
                           int rc);
void libxl__remus_domain_suspend_callback(void *data);
void libxl__remus_domain_resume_callback(void *data);
void libxl__remus_domain_checkpoint_callback(void *data);

#endif
