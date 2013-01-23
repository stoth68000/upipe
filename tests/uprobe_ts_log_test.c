/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short unit tests for uprobe ts log implementation
 */

#undef NDEBUG

#include <upipe/uprobe.h>
#include <upipe-ts/uprobe_ts_log.h>
#include <upipe/upipe.h>
#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/uref.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_std.h>
#include <upipe/ulog.h>
#include <upipe/ulog_stdio.h>

#include <upipe-ts/upipe_ts_demux.h>
#include <upipe-ts/upipe_ts_decaps.h>
#include <upipe-ts/upipe_ts_split.h>
#include <upipe-ts/upipe_ts_patd.h>
#include <upipe-ts/upipe_ts_pmtd.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>

#define UDICT_POOL_DEPTH 1
#define UREF_POOL_DEPTH 1

static struct upipe test_pipe;

int main(int argc, char **argv)
{
    struct uprobe *uprobe = uprobe_ts_log_alloc(NULL, ULOG_DEBUG);
    assert(uprobe != NULL);
    test_pipe.uprobe = uprobe;
    test_pipe.ulog = ulog_stdio_alloc(stdout, ULOG_DEBUG, "ts probe");

    upipe_throw_aerror(&test_pipe);

    upipe_throw(&test_pipe, UPROBE_TS_DECAPS_PCR, UPIPE_TS_DECAPS_SIGNATURE,
                NULL, 42);

    upipe_throw(&test_pipe, UPROBE_TS_SPLIT_SET_PID, UPIPE_TS_SPLIT_SIGNATURE,
                42);
    upipe_throw(&test_pipe, UPROBE_TS_SPLIT_UNSET_PID, UPIPE_TS_SPLIT_SIGNATURE,
                42);

    upipe_throw(&test_pipe, UPROBE_TS_PATD_TSID, UPIPE_TS_PATD_SIGNATURE, NULL,
                42);
    upipe_throw(&test_pipe, UPROBE_TS_PATD_ADD_PROGRAM, UPIPE_TS_PATD_SIGNATURE,
                NULL, 42, 12);
    upipe_throw(&test_pipe, UPROBE_TS_PATD_DEL_PROGRAM, UPIPE_TS_PATD_SIGNATURE,
                NULL, 42);

    upipe_throw(&test_pipe, UPROBE_TS_PMTD_HEADER, UPIPE_TS_PMTD_SIGNATURE,
                NULL, 42);
    upipe_throw(&test_pipe, UPROBE_TS_PMTD_ADD_ES, UPIPE_TS_PMTD_SIGNATURE,
                NULL, 42, 12);
    upipe_throw(&test_pipe, UPROBE_TS_PMTD_DEL_ES, UPIPE_TS_PMTD_SIGNATURE,
                NULL, 42);

    uprobe_log_free(uprobe);
    ulog_free(test_pipe.ulog);
    return 0;
}