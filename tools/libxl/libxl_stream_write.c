/*
 * Copyright (C) 2015      Citrix Ltd.
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

/*
 * Infrastructure for writing a domain to a libxl migration v2 stream.
 *
 * Entry points from outside:
 *  - libxl__stream_write_start()
 *     - Start writing a stream from the start.
 *
 *  - libxl__stream_write_start()
 *     - Write the records which form a checkpoint into a stream.
 *
 * In normal operation, there are two tasks running at once; this stream
 * processing, and the the libxl-save-helper.  check_stream_finished() is used
 * to join all the tasks in both success and error cases.
 *
 * Nomenclature for event callbacks:
 *  - $FOO_done(): Completion callback for $FOO
 *  - write_$FOO(): Set up writing a $FOO
 *  - $BAR_header(): A $BAR record header only
 *  - $BAR_record(): A complete $BAR record with header and content
 *
 * The main loop for a plain VM writes:
 *  - Stream header
 *  - Libxc record
 *  - Toolstack record
 *  - if (hvm), Qemu record
 *  - End record
 *
 * For checkpointed stream, there is a second loop which is triggered by a
 * save-helper checkpoint callback.  It writes:
 *  - Toolstack record
 *  - if (hvm), Qemu record
 *  - Checkpoint end record
 */

static const uint8_t zero_padding[1U << REC_ALIGN_ORDER] = { 0 };

static void stream_success(libxl__egc *egc,
                           libxl__stream_write_state *stream);
static void stream_failed(libxl__egc *egc,
                          libxl__stream_write_state *stream, int ret);
static void stream_done(libxl__egc *egc,
                        libxl__stream_write_state *stream);

static void check_stream_finished(libxl__egc *egc,
                                  libxl__domain_save_state *dcs,
                                  int rc, const char *what);

/* Event callbacks for plain VM. */
static void stream_header_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval);
static void libxc_header_done(libxl__egc *egc,
                              libxl__datacopier_state *dc,
                              int onwrite, int errnoval);
/* libxl__xc_domain_save_done() lives here, event-order wise. */
static void write_toolstack_record(libxl__egc *egc,
                                   libxl__stream_write_state *stream);
static void toolstack_record_done(libxl__egc *egc,
                                  libxl__datacopier_state *dc,
                                  int onwrite, int errnoval);
static void write_emulator_record(libxl__egc *egc,
                                  libxl__stream_write_state *stream);
static void emulator_body_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval);
static void emulator_padding_done(libxl__egc *egc,
                                  libxl__datacopier_state *dc,
                                  int onwrite, int errnoval);
static void write_end_record(libxl__egc *egc,
                             libxl__stream_write_state *stream);
static void end_record_done(libxl__egc *egc,
                            libxl__datacopier_state *dc,
                            int onwrite, int errnoval);

/* Event callbacks unique to checkpointed streams. */
static void checkpoint_done(libxl__egc *egc,
                            libxl__stream_write_state *stream,
                            int rc);
static void write_checkpoint_end_record(libxl__egc *egc,
                                        libxl__stream_write_state *stream);
static void checkpoint_end_record_done(libxl__egc *egc,
                                       libxl__datacopier_state *dc,
                                       int onwrite, int errnoval);

void libxl__stream_write_start(libxl__egc *egc,
                               libxl__stream_write_state *stream)
{
    libxl__datacopier_state *dc = &stream->dc;
    STATE_AO_GC(stream->ao);
    struct libxl_sr_hdr hdr = { 0 };
    int ret = 0;

    assert(!stream->running);
    stream->running = true;

    memset(dc, 0, sizeof(*dc));
    dc->readwhat = "";
    dc->copywhat = "suspend header";
    dc->writewhat = "save/migration stream";
    dc->ao = ao;
    dc->readfd = -1;
    dc->writefd = stream->fd;
    dc->maxsz = INT_MAX;
    dc->bytes_to_read = INT_MAX;
    dc->callback = stream_header_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    hdr.ident   = htobe64(RESTORE_STREAM_IDENT);
    hdr.version = htobe32(RESTORE_STREAM_VERSION);
    hdr.options = htobe32(0);

    libxl__datacopier_prefixdata(egc, dc, &hdr, sizeof(hdr));
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

void libxl__stream_write_start_checkpoint(libxl__egc *egc,
                                          libxl__stream_write_state *stream)
{
    assert(stream->running);
    assert(!stream->in_checkpoint);
    stream->in_checkpoint = true;

    write_toolstack_record(egc, stream);
}

void libxl__stream_write_abort(libxl__egc *egc,
                               libxl__stream_write_state *stream, int rc)
{
    stream_failed(egc, stream, rc);
}

static void stream_success(libxl__egc *egc, libxl__stream_write_state *stream)
{
    stream->rc = 0;
    stream->running = false;

    assert(!stream->in_checkpoint);
    stream_done(egc, stream);
}

static void stream_failed(libxl__egc *egc,
                          libxl__stream_write_state *stream, int rc)
{
    assert(rc);
    stream->rc = rc;

    /*
     *If we are in a checkpoint, pass the failure to libxc, which will come
     * back around to us via libxl__xc_domain_save_done().
     */
    if (stream->in_checkpoint) {
        checkpoint_done(egc, stream, rc);
        return;
    }

    if (stream->running) {
        stream->running = false;
        stream_done(egc, stream);
    }
}

static void stream_done(libxl__egc *egc,
                        libxl__stream_write_state *stream)
{
    libxl__domain_save_state *dss = CONTAINER_OF(stream, *dss, sws);

    assert(!stream->running);
    assert(!stream->in_checkpoint);

    check_stream_finished(egc, dss, stream->rc, "stream");
}

static void check_stream_finished(libxl__egc *egc,
                                  libxl__domain_save_state *dss,
                                  int rc, const char *what)
{
    libxl__stream_write_state *stream = &dss->sws;
    STATE_AO_GC(dss->ao);

    LOG(INFO, "Task '%s' joining (rc %d)", what, rc);

    if (rc && !stream->joined_rc) {
        bool skip = false;
        /* First reported failure from joining tasks.  Tear everything down */
        stream->joined_rc = rc;

        if (libxl__stream_write_inuse(&dss->sws)) {
            skip = true;
            libxl__stream_write_abort(egc, &dss->sws, rc);
        }

        if (libxl__save_helper_inuse(&dss->shs)) {
            skip = true;
            libxl__save_helper_abort(egc, &dss->shs);
        }

        /* There is at least one more active task to join - wait for its
           callback */
        if ( skip )
            return;
    }

    if (libxl__stream_write_inuse(&dss->sws))
        LOG(DEBUG, "stream still in use");
    else if (libxl__save_helper_inuse(&dss->shs))
        LOG(DEBUG, "save/restore still in use");
    else {
        LOG(INFO, "Join complete: result %d", stream->joined_rc);
        stream->completion_callback(egc, dss, stream->joined_rc);
    }
}

static void stream_header_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval)
{
    libxl__stream_write_state *stream = CONTAINER_OF(dc, *stream, dc);
    STATE_AO_GC(stream->ao);
    struct libxl_sr_rec_hdr rec = { REC_TYPE_LIBXC_CONTEXT, 0 };
    int ret = 0;

    if (onwrite || errnoval) {
        ret = ERROR_FAIL;
        goto err;
    }

    dc->copywhat = "suspend footer";
    dc->writewhat = "save/migration stream";
    dc->callback = libxc_header_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    libxl__datacopier_prefixdata(egc, dc, &rec, sizeof(rec));
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void libxc_header_done(libxl__egc *egc,
                              libxl__datacopier_state *dc,
                              int onwrite, int errnoval)
{
    libxl__stream_write_state *stream = CONTAINER_OF(dc, *stream, dc);
    libxl__domain_save_state *dss = CONTAINER_OF(stream, *dss, sws);
    STATE_AO_GC(stream->ao);
    int ret = 0;

    if (onwrite || errnoval) {
        ret = ERROR_FAIL;
        goto err;
    }

    libxl__xc_domain_save(egc, dss);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

void libxl__xc_domain_save_done(libxl__egc *egc, void *dss_void,
                                int rc, int retval, int errnoval)
{
    libxl__domain_save_state *dss = dss_void;
    libxl__stream_write_state *stream = &dss->sws;
    STATE_AO_GC(dss->ao);

    if (rc)
        goto err;

    if (retval) {
        LOGEV(ERROR, errnoval, "saving domain: %s",
                         dss->dsps.guest_responded ?
                         "domain responded to suspend request" :
                         "domain did not respond to suspend request");
        if ( !dss->dsps.guest_responded )
            rc = ERROR_GUEST_TIMEDOUT;
        else
            rc = ERROR_FAIL;
        goto err;
    }

    write_toolstack_record(egc, stream);
    return;

 err:
    assert(rc);
    check_stream_finished(egc, dss, rc, "save/restore helper");
}

static void write_toolstack_record(libxl__egc *egc,
                                   libxl__stream_write_state *stream)
{
    libxl__domain_save_state *dss = CONTAINER_OF(stream, *dss, sws);
    libxl__datacopier_state *dc = &stream->dc;
    STATE_AO_GC(stream->ao);
    struct libxl_sr_rec_hdr rec = { REC_TYPE_XENSTORE_DATA, 0 };
    int ret = 0;
    uint8_t *toolstack_buf = NULL; /* We must free this. */
    uint32_t toolstack_len, padding_len;

    ret = libxl__toolstack_save(dss->domid, &toolstack_buf,
                                &toolstack_len, dss);
    if (ret)
        goto err;

    dc->copywhat = "toolstack record";
    dc->writewhat = "save/migration stream";
    dc->callback = toolstack_record_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    rec.length = toolstack_len;

    libxl__datacopier_prefixdata(egc, dc, &rec, sizeof(rec));
    libxl__datacopier_prefixdata(egc, dc, toolstack_buf, toolstack_len);

    padding_len = ROUNDUP(rec.length, REC_ALIGN_ORDER) - rec.length;
    if (padding_len)
        libxl__datacopier_prefixdata(egc, dc, zero_padding, padding_len);

    free(toolstack_buf);
    return;

 err:
    assert(ret);
    free(toolstack_buf);
    stream_failed(egc, stream, ret);
}

static void toolstack_record_done(libxl__egc *egc,
                                  libxl__datacopier_state *dc,
                                  int onwrite, int errnoval)
{
    libxl__stream_write_state *stream = CONTAINER_OF(dc, *stream, dc);
    libxl__domain_save_state *dss = CONTAINER_OF(stream, *dss, sws);
    STATE_AO_GC(stream->ao);
    int ret = 0;

    if (onwrite || errnoval) {
        ret = ERROR_FAIL;
        goto err;
    }

    if (dss->type == LIBXL_DOMAIN_TYPE_HVM)
        write_emulator_record(egc, stream);
    else {
        if (stream->in_checkpoint)
            write_checkpoint_end_record(egc, stream);
        else
            write_end_record(egc, stream);
    }

    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void write_emulator_record(libxl__egc *egc,
                                  libxl__stream_write_state *stream)
{
    libxl__domain_save_state *dss = CONTAINER_OF(stream, *dss, sws);
    libxl__datacopier_state *dc = &stream->dc;
    STATE_AO_GC(stream->ao);
    struct libxl_sr_rec_hdr rec = { REC_TYPE_EMULATOR_CONTEXT, 0 };
    struct libxl_sr_emulator_hdr ehdr = { 0 };
    struct stat st;
    int ret = 0;
    uint32_t qemu_state_len;

    assert(dss->type == LIBXL_DOMAIN_TYPE_HVM);

    /* Convenience aliases */
    const char *const filename = dss->dsps.dm_savefile;
    const uint32_t domid = dss->domid;

    switch(libxl__device_model_version_running(gc, domid)) {
    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
        ehdr.id = EMULATOR_QEMU_TRADITIONAL;
        break;

    case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
        ehdr.id = EMULATOR_QEMU_UPSTREAM;
        break;

    default:
        ret = ERROR_FAIL;
        goto err;
    }

    ret = libxl__domain_suspend_device_model(gc, &dss->dsps);
    if (ret)
        goto err;

    dc->readwhat = GCSPRINTF("qemu save file %s", filename);
    dc->copywhat = "emulator record";
    dc->writewhat = "save/migration stream";
    dc->callback = emulator_body_done;

    dc->readfd = open(filename, O_RDONLY);
    if (dc->readfd < 0) {
        LOGE(ERROR, "unable to open %s", dc->readwhat);
        goto err;
    }

    if (fstat(dc->readfd, &st))
    {
        LOGE(ERROR, "unable to fstat %s", dc->readwhat);
        goto err;
    }

    if (!S_ISREG(st.st_mode)) {
        LOG(ERROR, "%s is not a plain file!", dc->readwhat);
        goto err;
    }

    qemu_state_len = st.st_size;
    rec.length = qemu_state_len + sizeof(ehdr);

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    libxl__datacopier_prefixdata(egc, dc, &rec, sizeof(rec));
    libxl__datacopier_prefixdata(egc, dc, &ehdr, sizeof(ehdr));

    stream->padding = ROUNDUP(qemu_state_len, REC_ALIGN_ORDER) - qemu_state_len;
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void emulator_body_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval)
{
    libxl__stream_write_state *stream = CONTAINER_OF(dc, *stream, dc);
    STATE_AO_GC(stream->ao);
    int ret = 0;

    if (onwrite || errnoval) {
        ret = ERROR_FAIL;
        goto err;
    }

    dc->readwhat = "";
    dc->readfd = -1;

    if (stream->padding) {
        assert(stream->padding < (1U << REC_ALIGN_ORDER));

        dc->copywhat = "emulator padding";
        dc->writewhat = "save/migration stream";
        dc->callback = emulator_padding_done;

        ret = libxl__datacopier_start(dc);
        if (ret)
            goto err;

        libxl__datacopier_prefixdata(egc, dc, zero_padding, stream->padding);
        return;
    }

    emulator_padding_done(egc, dc, 0, 0);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void emulator_padding_done(libxl__egc *egc,
                                  libxl__datacopier_state *dc,
                                  int onwrite, int errnoval)
{
    libxl__stream_write_state *stream = CONTAINER_OF(dc, *stream, dc);
    STATE_AO_GC(stream->ao);
    int ret = 0;

    if (onwrite || errnoval) {
        ret = ERROR_FAIL;
        goto err;
    }

    if (stream->in_checkpoint)
        write_checkpoint_end_record(egc, stream);
    else
        write_end_record(egc, stream);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void write_end_record(libxl__egc *egc,
                             libxl__stream_write_state *stream)
{
    libxl__datacopier_state *dc = &stream->dc;
    STATE_AO_GC(stream->ao);
    struct libxl_sr_rec_hdr rec = { REC_TYPE_END, 0 };
    int ret = 0;

    dc->copywhat = "suspend footer";
    dc->writewhat = "save/migration stream";
    dc->callback = end_record_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    libxl__datacopier_prefixdata(egc, dc, &rec, sizeof(rec));
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void end_record_done(libxl__egc *egc,
                            libxl__datacopier_state *dc,
                            int onwrite, int errnoval)
{
    libxl__stream_write_state *stream = CONTAINER_OF(dc, *stream, dc);
    STATE_AO_GC(stream->ao);
    int ret = 0;

    if (onwrite || errnoval) {
        ret = ERROR_FAIL;
        goto err;
    }

    stream_success(egc, stream);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void checkpoint_done(libxl__egc *egc,
                            libxl__stream_write_state *stream,
                            int rc)
{
    libxl__domain_save_state *dss = CONTAINER_OF(stream, *dss, sws);

    assert(stream->in_checkpoint);
    stream->in_checkpoint = false;
    stream->checkpoint_callback(egc, dss, rc);
}

static void write_checkpoint_end_record(libxl__egc *egc,
                                        libxl__stream_write_state *stream)
{
    libxl__datacopier_state *dc = &stream->dc;
    STATE_AO_GC(stream->ao);
    struct libxl_sr_rec_hdr rec = { REC_TYPE_CHECKPOINT_END, 0 };
    int ret = 0;

    assert(stream->in_checkpoint);

    dc->copywhat = "checkpoint record";
    dc->writewhat = "save/migration stream";
    dc->callback = checkpoint_end_record_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    libxl__datacopier_prefixdata(egc, dc, &rec, sizeof(rec));
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void checkpoint_end_record_done(libxl__egc *egc,
                                       libxl__datacopier_state *dc,
                                       int onwrite, int errnoval)
{
    libxl__stream_write_state *stream = CONTAINER_OF(dc, *stream, dc);
    STATE_AO_GC(stream->ao);
    int ret = 0;

    if (onwrite || errnoval) {
        ret = ERROR_FAIL;
        goto err;
    }

    checkpoint_done(egc, stream, 0);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
