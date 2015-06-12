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
 * Infrastructure for reading and acting on the contents of a libxl migration
 * stream. There are a lot of moving parts here.
 *
 * Entry points from outside:
 *  - libxl__stream_read_start()
 *     - Set up reading a stream from the start.
 *
 *  - libxl__stream_read_continue()
 *     - Set up reading the next record from a started stream.
 *
 * The principle loop functionality involves reading the stream header, then
 * reading a record at time and acting upon it.  It follows the callbacks:
 *
 *  - stream_header_done()
 *  - stream_record_header_done()
 *  - stream_record_body_done()
 *  - process_record()
 *
 * process_record() will choose the correct next action based upon the
 * record.  Upon completion of the action, the next record header will be read
 * from the stream.
 */

static void stream_success(libxl__egc *egc,
                           libxl__stream_read_state *stream);
static void stream_failed(libxl__egc *egc,
                          libxl__stream_read_state *stream, int rc);
static void stream_done(libxl__egc *egc,
                        libxl__stream_read_state *stream);

/* Event callbacks for main reading loop. */
static void stream_header_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval);
static void record_header_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval);
static void record_body_done(libxl__egc *egc,
                             libxl__datacopier_state *dc,
                             int onwrite, int errnoval);
static void process_record(libxl__egc *egc,
                           libxl__stream_read_state *stream);

/* Mini-event loop for splicing a emulator record out of the stream. */
static void read_emulator_body(libxl__egc *egc,
                               libxl__stream_read_state *stream);
static void emulator_body_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval);
static void emulator_padding_done(libxl__egc *egc,
                                  libxl__datacopier_state *dc,
                                  int onwrite, int errnoval);

void libxl__stream_read_start(libxl__egc *egc,
                              libxl__stream_read_state *stream)
{
    libxl__datacopier_state *dc = &stream->dc;
    int ret = 0;

    /* State initialisation. */
    assert(!stream->running);

    memset(dc, 0, sizeof(*dc));
    dc->ao = stream->ao;
    dc->readfd = stream->fd;
    dc->writefd = -1;

    /* Start reading the stream header. */
    dc->readwhat = "stream header";
    dc->readbuf = &stream->hdr;
    stream->expected_len = dc->bytes_to_read = sizeof(stream->hdr);
    dc->used = 0;
    dc->callback = stream_header_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    stream->running = true;
    assert(!ret);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

void libxl__stream_read_continue(libxl__egc *egc,
                                 libxl__stream_read_state *stream)
{
    libxl__datacopier_state *dc = &stream->dc;
    int ret = 0;

    assert(stream->running);

    /* Read a record header. */
    dc->readwhat = "record header";
    dc->readbuf = &stream->rec_hdr;
    stream->expected_len = dc->bytes_to_read = sizeof(stream->rec_hdr);
    dc->used = 0;
    dc->callback = record_header_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;

    assert(!ret);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

void libxl__stream_read_abort(libxl__egc *egc,
                              libxl__stream_read_state *stream, int rc)
{
    stream_failed(egc, stream, rc);
}

static void stream_success(libxl__egc *egc, libxl__stream_read_state *stream)
{
    stream->rc = 0;
    stream->running = false;

    stream_done(egc, stream);
}

static void stream_failed(libxl__egc *egc,
                          libxl__stream_read_state *stream, int rc)
{
    assert(rc);
    stream->rc = rc;

    if (stream->running) {
        stream->running = false;
        stream_done(egc, stream);
    }
}

static void stream_done(libxl__egc *egc,
                        libxl__stream_read_state *stream)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(stream, *dcs, srs);

    assert(!stream->running);

    stream->completion_callback(egc, dcs, stream->rc);
}

static void stream_header_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval)
{
    libxl__stream_read_state *stream = CONTAINER_OF(dc, *stream, dc);
    libxl_sr_hdr *hdr = &stream->hdr;
    STATE_AO_GC(dc->ao);
    int ret = 0;

    if (onwrite || dc->used != stream->expected_len) {
        ret = ERROR_FAIL;
        LOG(ERROR, "write %d, err %d, expected %zu, got %zu",
            onwrite, errnoval, stream->expected_len, dc->used);
        goto err;
    }

    hdr->ident   = be64toh(hdr->ident);
    hdr->version = be32toh(hdr->version);
    hdr->options = be32toh(hdr->options);

    if (hdr->ident != RESTORE_STREAM_IDENT) {
        ret = ERROR_FAIL;
        LOG(ERROR,
            "Invalid ident: expected 0x%016"PRIx64", got 0x%016"PRIx64,
            RESTORE_STREAM_IDENT, hdr->ident);
        goto err;
    }
    if (hdr->version != RESTORE_STREAM_VERSION) {
        ret = ERROR_FAIL;
        LOG(ERROR, "Unexpected Version: expected %u, got %u",
            RESTORE_STREAM_VERSION, hdr->version);
        goto err;
    }
    if (hdr->options & RESTORE_OPT_BIG_ENDIAN) {
        ret = ERROR_FAIL;
        LOG(ERROR, "Unable to handle big endian streams");
        goto err;
    }

    LOG(INFO, "Stream v%u%s", hdr->version,
        hdr->options & RESTORE_OPT_LEGACY ? " (from legacy)" : "");

    libxl__stream_read_continue(egc, stream);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void record_header_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval)
{
    libxl__stream_read_state *stream = CONTAINER_OF(dc, *stream, dc);
    libxl_sr_rec_hdr *rec_hdr = &stream->rec_hdr;
    STATE_AO_GC(dc->ao);
    int ret = 0;

    if (onwrite || dc->used != stream->expected_len) {
        ret = ERROR_FAIL;
        LOG(ERROR, "write %d, err %d, expected %zu, got %zu",
            onwrite, errnoval, stream->expected_len, dc->used);
        goto err;
    }

    assert(stream->rec_body == NULL);

    /* No body? Process straight away. */
    if (rec_hdr->length == 0) {
        process_record(egc, stream);
        return;
    }

    /* Queue up reading the body. */
    size_t bytes_to_read;

    switch (rec_hdr->type) {
        /*
         * Emulator records want to retain the blob in the pipe, for a further
         * datacopier call to move elsewhere.  Just read the emulator header.
         */
    case REC_TYPE_EMULATOR_CONTEXT:
        bytes_to_read = sizeof(struct libxl_sr_emulator_hdr);
        break;

    default:
        bytes_to_read = rec_hdr->length;
        break;
    }

    bytes_to_read = ROUNDUP(bytes_to_read, REC_ALIGN_ORDER);

    dc->readwhat = "record body";
    stream->rec_body = dc->readbuf = libxl__malloc(NOGC, bytes_to_read);
    stream->expected_len = dc->bytes_to_read = bytes_to_read;
    dc->used = 0;
    dc->callback = record_body_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void record_body_done(libxl__egc *egc,
                             libxl__datacopier_state *dc,
                             int onwrite, int errnoval)
{
    libxl__stream_read_state *stream = CONTAINER_OF(dc, *stream, dc);
    STATE_AO_GC(dc->ao);
    int ret = 0;

    if (onwrite || dc->used != stream->expected_len) {
        ret = ERROR_FAIL;
        LOG(ERROR, "write %d, err %d, expected %zu, got %zu",
            onwrite, errnoval, stream->expected_len, dc->used);

        free(stream->rec_body);
        stream->rec_body = dc->readbuf = NULL;

        goto err;
    }

    process_record(egc, stream);
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void process_record(libxl__egc *egc,
                           libxl__stream_read_state *stream)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(stream, *dcs, srs);
    libxl_sr_rec_hdr *rec_hdr = &stream->rec_hdr;
    STATE_AO_GC(stream->ao);
    int ret = 0;

    LOG(DEBUG, "Record: 0x%08x, length %u", rec_hdr->type, rec_hdr->length);

    switch (rec_hdr->type) {

    case REC_TYPE_END:
        /* Handled later, after cleanup. */
        break;

    case REC_TYPE_XENSTORE_DATA:
        ret = libxl__toolstack_restore(dcs->guest_domid, stream->rec_body,
                                       rec_hdr->length, &dcs->shs);
        if (ret)
            goto err;

        /*
         * libxl__toolstack_restore() is a synchronous function.  Manually
         * start looking for the next record.
         */
        libxl__stream_read_continue(egc, &dcs->srs);
        break;

    case REC_TYPE_EMULATOR_CONTEXT:
        read_emulator_body(egc, stream);
        break;

    default:
        LOG(ERROR, "Unrecognised record 0x%08x", rec_hdr->type);
        ret = ERROR_FAIL;
        goto err;
    }

    assert(!ret);
    if (rec_hdr->length) {
        free(stream->rec_body);
        stream->rec_body = NULL;
    }

    if (rec_hdr->type == REC_TYPE_END)
        stream_success(egc, stream);
    return;

 err:
    assert(ret);
    if (rec_hdr->length) {
        free(stream->rec_body);
        stream->rec_body = NULL;
    }
    stream_failed(egc, stream, ret);
}

static void read_emulator_body(libxl__egc *egc,
                               libxl__stream_read_state *stream)
{
    libxl__domain_create_state *dcs = CONTAINER_OF(stream, *dcs, srs);
    libxl__datacopier_state *dc = &stream->dc;
    libxl_sr_rec_hdr *rec_hdr = &stream->rec_hdr;
    libxl_sr_emulator_hdr *emu_hdr = stream->rec_body;
    STATE_AO_GC(stream->ao);
    char path[256];
    int ret = 0;

    sprintf(path, XC_DEVICE_MODEL_RESTORE_FILE".%u", dcs->guest_domid);

    dc->readwhat = "save/migration stream";
    dc->copywhat = "emulator context";
    dc->writewhat = "qemu save file";
    dc->readbuf = NULL;
    dc->writefd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dc->writefd == -1) {
        ret = ERROR_FAIL;
        LOGE(ERROR, "Unable to open '%s'", path);
        goto err;
    }
    dc->maxsz = dc->bytes_to_read = rec_hdr->length - sizeof(*emu_hdr);
    stream->expected_len = dc->used = 0;
    dc->callback = emulator_body_done;

    ret = libxl__datacopier_start(dc);
    if (ret)
        goto err;
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void emulator_body_done(libxl__egc *egc,
                               libxl__datacopier_state *dc,
                               int onwrite, int errnoval)
{
    /* Safe to be static, as it is a write-only discard buffer. */
    static char padding[1U << REC_ALIGN_ORDER];

    libxl__stream_read_state *stream = CONTAINER_OF(dc, *stream, dc);
    libxl_sr_rec_hdr *rec_hdr = &stream->rec_hdr;
    STATE_AO_GC(dc->ao);
    unsigned int nr_padding_bytes = (1U << REC_ALIGN_ORDER);
    int ret = 0;

    if (onwrite || dc->used != stream->expected_len) {
        ret = ERROR_FAIL;
        LOG(ERROR, "write %d, err %d, expected %zu, got %zu",
            onwrite, errnoval, stream->expected_len, dc->used);
        goto err;
    }

    /* Undo modifications for splicing the emulator context. */
    memset(dc, 0, sizeof(*dc));
    dc->ao = stream->ao;
    dc->readfd = stream->fd;
    dc->writefd = -1;

    /* Do we need to eat some padding out of the stream? */
    if (rec_hdr->length & (nr_padding_bytes - 1)) {
        unsigned int bytes_to_discard =
            nr_padding_bytes - (rec_hdr->length & (nr_padding_bytes - 1));

        dc->readwhat = "padding bytes";
        dc->readbuf = padding;
        stream->expected_len = dc->bytes_to_read = bytes_to_discard;
        dc->used = 0;
        dc->callback = emulator_padding_done;

        ret = libxl__datacopier_start(dc);
        if (ret)
            goto err;
    }
    else
    {
        stream->expected_len = dc->bytes_to_read = 0;
        dc->used = 0;

        emulator_padding_done(egc, dc, 0, 0);
    }
    return;

 err:
    assert(ret);
    stream_failed(egc, stream, ret);
}

static void emulator_padding_done(libxl__egc *egc,
                                  libxl__datacopier_state *dc,
                                  int onwrite, int errnoval)
{
    libxl__stream_read_state *stream = CONTAINER_OF(dc, *stream, dc);
    STATE_AO_GC(dc->ao);
    int ret = 0;

    if (onwrite || dc->used != stream->expected_len) {
        ret = ERROR_FAIL;
        LOG(ERROR, "write %d, err %d, expected %zu, got %zu",
            onwrite, errnoval, stream->expected_len, dc->used);
        goto err;
    }

    libxl__stream_read_continue(egc, stream);
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
