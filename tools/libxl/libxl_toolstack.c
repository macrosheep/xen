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

#define TOOLSTACK_SAVE_VERSION 1

static inline char *restore_helper(libxl__gc *gc, uint32_t dm_domid,
                                   uint32_t domid,
                                   uint64_t phys_offset, char *node)
{
    return libxl__device_model_xs_path(gc, dm_domid, domid,
                                       "/physmap/%"PRIx64"/%s",
                                       phys_offset, node);
}

int libxl__toolstack_restore(uint32_t domid, const uint8_t *buf,
                             uint32_t size, void *user)
{
    libxl__save_helper_state *shs = user;
    libxl__domain_create_state *dcs = CONTAINER_OF(shs, *dcs, shs);
    STATE_AO_GC(dcs->ao);
    int i, ret;
    const uint8_t *ptr = buf;
    uint32_t count = 0, version = 0;
    struct libxl__physmap_info* pi;
    char *xs_path;
    uint32_t dm_domid;

    LOG(DEBUG,"domain=%"PRIu32" toolstack data size=%"PRIu32, domid, size);

    if (size < sizeof(version) + sizeof(count)) {
        LOG(ERROR, "wrong size");
        return -1;
    }

    memcpy(&version, ptr, sizeof(version));
    ptr += sizeof(version);

    if (version != TOOLSTACK_SAVE_VERSION) {
        LOG(ERROR, "wrong version");
        return -1;
    }

    memcpy(&count, ptr, sizeof(count));
    ptr += sizeof(count);

    if (size < sizeof(version) + sizeof(count) +
            count * (sizeof(struct libxl__physmap_info))) {
        LOG(ERROR, "wrong size");
        return -1;
    }

    dm_domid = libxl_get_stubdom_id(CTX, domid);
    for (i = 0; i < count; i++) {
        pi = (struct libxl__physmap_info*) ptr;
        ptr += sizeof(struct libxl__physmap_info) + pi->namelen;

        xs_path = restore_helper(gc, dm_domid, domid,
                                 pi->phys_offset, "start_addr");
        ret = libxl__xs_write(gc, 0, xs_path, "%"PRIx64, pi->start_addr);
        if (ret)
            return -1;
        xs_path = restore_helper(gc, dm_domid, domid, pi->phys_offset, "size");
        ret = libxl__xs_write(gc, 0, xs_path, "%"PRIx64, pi->size);
        if (ret)
            return -1;
        if (pi->namelen > 0) {
            xs_path = restore_helper(gc, dm_domid, domid,
                                     pi->phys_offset, "name");
            ret = libxl__xs_write(gc, 0, xs_path, "%s", pi->name);
            if (ret)
                return -1;
        }
    }
    return 0;
}

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
    libxl__domain_suspend_state *dss = dss_void;
    STATE_AO_GC(dss->ao);
    int i = 0;
    char *start_addr = NULL, *size = NULL, *phys_offset = NULL, *name = NULL;
    unsigned int num = 0;
    uint32_t count = 0, version = TOOLSTACK_SAVE_VERSION, namelen = 0;
    uint8_t *ptr = NULL;
    char **entries = NULL;
    struct libxl__physmap_info *pi;
    uint32_t dm_domid;

    dm_domid = libxl_get_stubdom_id(CTX, domid);

    entries = libxl__xs_directory(gc, 0,
                libxl__device_model_xs_path(gc, dm_domid, domid, "/physmap"),
                &num);
    count = num;

    *len = sizeof(version) + sizeof(count);
    *buf = calloc(1, *len);
    ptr = *buf;
    if (*buf == NULL)
        return -1;

    memcpy(ptr, &version, sizeof(version));
    ptr += sizeof(version);
    memcpy(ptr, &count, sizeof(count));
    ptr += sizeof(count);

    for (i = 0; i < count; i++) {
        unsigned long offset;
        char *xs_path;
        phys_offset = entries[i];
        if (phys_offset == NULL) {
            LOG(ERROR, "phys_offset %d is NULL", i);
            return -1;
        }

        xs_path = physmap_path(gc, dm_domid, domid, phys_offset, "start_addr");
        start_addr = libxl__xs_read(gc, 0, xs_path);
        if (start_addr == NULL) {
            LOG(ERROR, "%s is NULL", xs_path);
            return -1;
        }

        xs_path = physmap_path(gc, dm_domid, domid, phys_offset, "size");
        size = libxl__xs_read(gc, 0, xs_path);
        if (size == NULL) {
            LOG(ERROR, "%s is NULL", xs_path);
            return -1;
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
        if (*buf == NULL)
            return -1;
        ptr = (*buf) + offset;
        pi = (struct libxl__physmap_info *) ptr;
        pi->phys_offset = strtoll(phys_offset, NULL, 16);
        pi->start_addr = strtoll(start_addr, NULL, 16);
        pi->size = strtoll(size, NULL, 16);
        pi->namelen = namelen;
        memcpy(pi->name, name, namelen);
        ptr += sizeof(struct libxl__physmap_info) + namelen;
    }

    LOG(DEBUG,"domain=%"PRIu32" toolstack data size=%"PRIu32, domid, *len);

    return 0;
}
/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
