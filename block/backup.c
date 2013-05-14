/*
 * QEMU backup
 *
 * Copyright (C) 2013 Proxmox Server Solutions
 *
 * Authors:
 *  Dietmar Maurer (dietmar@proxmox.com)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "block/block.h"
#include "block/block_int.h"
#include "block/blockjob.h"
#include "qemu/ratelimit.h"

#define DEBUG_BACKUP 0

#define DPRINTF(fmt, ...) \
    do { \
        if (DEBUG_BACKUP) { \
            fprintf(stderr, "backup: " fmt, ## __VA_ARGS__); \
        } \
    } while (0)

#define BACKUP_CLUSTER_BITS 16
#define BACKUP_CLUSTER_SIZE (1 << BACKUP_CLUSTER_BITS)
#define BACKUP_SECTORS_PER_CLUSTER (BACKUP_CLUSTER_SIZE / BDRV_SECTOR_SIZE)

#define SLICE_TIME 100000000ULL /* ns */

typedef struct CowRequest {
    int64_t start;
    int64_t end;
    QLIST_ENTRY(CowRequest) list;
    CoQueue wait_queue; /* coroutines blocked on this request */
} CowRequest;

typedef struct BackupBlockJob {
    BlockJob common;
    BlockDriverState *target;
    RateLimit limit;
    BlockdevOnError on_source_error;
    BlockdevOnError on_target_error;
    CoRwlock flush_rwlock;
    uint64_t sectors_read;
    HBitmap *bitmap;
    QLIST_HEAD(, CowRequest) inflight_reqs;
} BackupBlockJob;

/* See if in-flight requests overlap and wait for them to complete */
static void coroutine_fn wait_for_overlapping_requests(BackupBlockJob *job,
                                                       int64_t start,
                                                       int64_t end)
{
    CowRequest *req;
    bool retry;

    do {
        retry = false;
        QLIST_FOREACH(req, &job->inflight_reqs, list) {
            if (end > req->start && start < req->end) {
                qemu_co_queue_wait(&req->wait_queue);
                retry = true;
                break;
            }
        }
    } while (retry);
}

/* Keep track of an in-flight request */
static void cow_request_begin(CowRequest *req, BackupBlockJob *job,
                                     int64_t start, int64_t end)
{
    req->start = start;
    req->end = end;
    qemu_co_queue_init(&req->wait_queue);
    QLIST_INSERT_HEAD(&job->inflight_reqs, req, list);
}

/* Forget about a completed request */
static void cow_request_end(CowRequest *req)
{
    QLIST_REMOVE(req, list);
    qemu_co_queue_restart_all(&req->wait_queue);
}

static int coroutine_fn backup_do_cow(BlockDriverState *bs,
                                      int64_t sector_num, int nb_sectors,
                                      bool *error_is_read)
{
    BackupBlockJob *job = (BackupBlockJob *)bs->job;
    CowRequest cow_request;
    struct iovec iov;
    QEMUIOVector bounce_qiov;
    void *bounce_buffer = NULL;
    int ret = 0;
    int64_t start, end, total_sectors;
    int n;

    qemu_co_rwlock_rdlock(&job->flush_rwlock);

    start = sector_num / BACKUP_SECTORS_PER_CLUSTER;
    end = DIV_ROUND_UP(sector_num + nb_sectors, BACKUP_SECTORS_PER_CLUSTER);

    DPRINTF("%s enter %s C%" PRId64 " %" PRId64 " %d\n",
            __func__, bdrv_get_device_name(bs), start, sector_num, nb_sectors);

    wait_for_overlapping_requests(job, start, end);
    cow_request_begin(&cow_request, job, start, end);

    total_sectors = bdrv_getlength(bs);
    if (total_sectors < 0) {
        if (error_is_read) {
            *error_is_read = true;
        }
        ret = total_sectors;
        goto out;
    }
    total_sectors /= BDRV_SECTOR_SIZE;

    for (; start < end; start++) {
        if (hbitmap_get(job->bitmap, start)) {
            DPRINTF("%s skip C%" PRId64 "\n", __func__, start);
            continue; /* already copied */
        }

        DPRINTF("%s C%" PRId64 "\n", __func__, start);

        n = MIN(BACKUP_SECTORS_PER_CLUSTER,
                total_sectors - start * BACKUP_SECTORS_PER_CLUSTER);

        if (!bounce_buffer) {
            bounce_buffer = qemu_blockalign(bs, BACKUP_CLUSTER_SIZE);
        }
        iov.iov_base = bounce_buffer;
        iov.iov_len = n * BDRV_SECTOR_SIZE;
        qemu_iovec_init_external(&bounce_qiov, &iov, 1);

        ret = bdrv_co_readv(bs, start * BACKUP_SECTORS_PER_CLUSTER, n,
                            &bounce_qiov);
        if (ret < 0) {
            DPRINTF("%s bdrv_co_readv C%" PRId64 " failed\n", __func__, start);
            if (error_is_read) {
                *error_is_read = true;
            }
            goto out;
        }

        if (buffer_is_zero(iov.iov_base, iov.iov_len)) {
            ret = bdrv_co_write_zeroes(job->target,
                                       start * BACKUP_SECTORS_PER_CLUSTER, n);
        } else {
            ret = bdrv_co_writev(job->target,
                                 start * BACKUP_SECTORS_PER_CLUSTER, n,
                                 &bounce_qiov);
        }
        if (ret < 0) {
            DPRINTF("%s write C%" PRId64 " failed\n", __func__, start);
            if (error_is_read) {
                *error_is_read = false;
            }
            goto out;
        }

        hbitmap_set(job->bitmap, start, 1);

        /* Publish progress */
        job->sectors_read += n;
        job->common.offset += n * BDRV_SECTOR_SIZE;

        DPRINTF("%s done C%" PRId64 "\n", __func__, start);
    }

out:
    if (bounce_buffer) {
        qemu_vfree(bounce_buffer);
    }

    cow_request_end(&cow_request);

    qemu_co_rwlock_unlock(&job->flush_rwlock);

    return ret;
}

static int coroutine_fn backup_before_write_notify(
        NotifierWithReturn *notifier,
        void *opaque)
{
    BdrvTrackedRequest *req = opaque;

    return backup_do_cow(req->bs, req->sector_num, req->nb_sectors, NULL);
}

static void backup_set_speed(BlockJob *job, int64_t speed, Error **errp)
{
    BackupBlockJob *s = container_of(job, BackupBlockJob, common);

    if (speed < 0) {
        error_set(errp, QERR_INVALID_PARAMETER, "speed");
        return;
    }
    ratelimit_set_speed(&s->limit, speed / BDRV_SECTOR_SIZE, SLICE_TIME);
}

static void backup_iostatus_reset(BlockJob *job)
{
    BackupBlockJob *s = container_of(job, BackupBlockJob, common);

    bdrv_iostatus_reset(s->target);
}

static BlockJobType backup_job_type = {
    .instance_size = sizeof(BackupBlockJob),
    .job_type = "backup",
    .set_speed = backup_set_speed,
    .iostatus_reset = backup_iostatus_reset,
};

static BlockErrorAction backup_error_action(BackupBlockJob *job,
                                            bool read, int error)
{
    if (read) {
        return block_job_error_action(&job->common, job->common.bs,
                                      job->on_source_error, true, error);
    } else {
        return block_job_error_action(&job->common, job->target,
                                      job->on_target_error, false, error);
    }
}

static void coroutine_fn backup_run(void *opaque)
{
    BackupBlockJob *job = opaque;
    BlockDriverState *bs = job->common.bs;
    BlockDriverState *target = job->target;
    BlockdevOnError on_target_error = job->on_target_error;
    NotifierWithReturn before_write = {
        .notify = backup_before_write_notify,
    };
    int64_t start, end;
    int ret = 0;

    QLIST_INIT(&job->inflight_reqs);
    qemu_co_rwlock_init(&job->flush_rwlock);

    start = 0;
    end = DIV_ROUND_UP(bdrv_getlength(bs) / BDRV_SECTOR_SIZE,
                       BACKUP_SECTORS_PER_CLUSTER);

    job->bitmap = hbitmap_alloc(end, 0);

    bdrv_set_on_error(target, on_target_error, on_target_error);
    bdrv_iostatus_enable(target);

    bdrv_add_before_write_notifier(bs, &before_write);

    DPRINTF("backup_run start %s %" PRId64 " %" PRId64 "\n",
            bdrv_get_device_name(bs), start, end);

    for (; start < end; start++) {
        bool error_is_read;

        if (block_job_is_cancelled(&job->common)) {
            break;
        }

        /* we need to yield so that qemu_aio_flush() returns.
         * (without, VM does not reboot)
         */
        if (job->common.speed) {
            uint64_t delay_ns = ratelimit_calculate_delay(
                &job->limit, job->sectors_read);
            job->sectors_read = 0;
            block_job_sleep_ns(&job->common, rt_clock, delay_ns);
        } else {
            block_job_sleep_ns(&job->common, rt_clock, 0);
        }

        if (block_job_is_cancelled(&job->common)) {
            break;
        }

        DPRINTF("backup_run loop C%" PRId64 "\n", start);

        ret = backup_do_cow(bs, start * BACKUP_SECTORS_PER_CLUSTER, 1,
                            &error_is_read);
        if (ret < 0) {
            /* Depending on error action, fail now or retry cluster */
            BlockErrorAction action =
                backup_error_action(job, error_is_read, -ret);
            if (action == BDRV_ACTION_REPORT) {
                break;
            } else {
                start--;
                continue;
            }
        }
    }

    notifier_with_return_remove(&before_write);

    /* wait until pending backup_do_cow() calls have completed */
    qemu_co_rwlock_wrlock(&job->flush_rwlock);
    qemu_co_rwlock_unlock(&job->flush_rwlock);

    hbitmap_free(job->bitmap);

    bdrv_iostatus_disable(target);
    bdrv_delete(job->target);

    DPRINTF("backup_run complete %d\n", ret);
    block_job_completed(&job->common, ret);
}

void backup_start(BlockDriverState *bs, BlockDriverState *target,
                  int64_t speed,
                  BlockdevOnError on_source_error,
                  BlockdevOnError on_target_error,
                  BlockDriverCompletionFunc *cb, void *opaque,
                  Error **errp)
{
    assert(bs);
    assert(target);
    assert(cb);

    DPRINTF("backup_start %s\n", bdrv_get_device_name(bs));

    if ((on_source_error == BLOCKDEV_ON_ERROR_STOP ||
         on_source_error == BLOCKDEV_ON_ERROR_ENOSPC) &&
        !bdrv_iostatus_is_enabled(bs)) {
        error_set(errp, QERR_INVALID_PARAMETER, "on-source-error");
        return;
    }

    BackupBlockJob *job = block_job_create(&backup_job_type, bs, speed,
                                           cb, opaque, errp);
    if (!job) {
        return;
    }

    job->on_source_error = on_source_error;
    job->on_target_error = on_target_error;
    job->target = target;
    job->common.len = bdrv_getlength(bs);
    job->common.co = qemu_coroutine_create(backup_run);
    qemu_coroutine_enter(job->common.co, job);
}
