/*
 * Copyright (C) 2014 OpenHeadend S.A.R.L.
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
 * @short Upipe module synchronizing latencies of flows belonging to a program
 */

#include <upipe/ubase.h>
#include <upipe/ulist.h>
#include <upipe/uprobe.h>
#include <upipe/uclock.h>
#include <upipe/uref.h>
#include <upipe/uref_clock.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe-modules/upipe_play.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>

/* default output latency */
#define DEFAULT_OUTPUT_LATENCY (UCLOCK_FREQ / 50)

/** @internal @This is the private context of a play pipe. */
struct upipe_play {
    /** refcount management structure */
    struct urefcount urefcount;

    /** max latency of the subpipes */
    uint64_t input_latency;
    /** output latency */
    uint64_t output_latency;
    /** total latency */
    uint64_t latency;

    /** list of subs */
    struct uchain subs;

    /** manager to create subs */
    struct upipe_mgr sub_mgr;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_play, upipe, UPIPE_PLAY_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_play, urefcount, upipe_play_free)
UPIPE_HELPER_VOID(upipe_play)

/** @internal @This is the private context of an output of a play pipe. */
struct upipe_play_sub {
    /** refcount management structure */
    struct urefcount urefcount;
    /** structure for double-linked lists */
    struct uchain uchain;

    /** pipe acting as output */
    struct upipe *output;
    /** flow definition packet on this output */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_play_sub, upipe, UPIPE_PLAY_SUB_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_play_sub, urefcount, upipe_play_sub_free)
UPIPE_HELPER_VOID(upipe_play_sub)
UPIPE_HELPER_OUTPUT(upipe_play_sub, output, flow_def, flow_def_sent)

UPIPE_HELPER_SUBPIPE(upipe_play, upipe_play_sub, sub, sub_mgr, subs, uchain)

static void upipe_play_set_input_latency(struct upipe *upipe, uint64_t latency);

/** @internal @This allocates an output subpipe of a play pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_play_sub_alloc(struct upipe_mgr *mgr,
                                          struct uprobe *uprobe,
                                          uint32_t signature,
                                          va_list args)
{
    struct upipe *upipe = upipe_play_sub_alloc_void(mgr, uprobe, signature,
                                                    args);
    if (unlikely(upipe == NULL))
        return NULL;
    upipe_play_sub_init_urefcount(upipe);
    upipe_play_sub_init_output(upipe);
    upipe_play_sub_init_sub(upipe);
    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This builds the output flow definition.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_play_sub_build_flow_def(struct upipe *upipe)
{
    struct upipe_play_sub *upipe_play_sub = upipe_play_sub_from_upipe(upipe);
    struct uref *flow_def = upipe_play_sub->flow_def;
    if (flow_def == NULL)
        return;
    upipe_play_sub->flow_def = NULL;

    struct upipe_play *upipe_play = upipe_play_from_sub_mgr(upipe->mgr);
    if (!ubase_check(uref_clock_set_latency(flow_def, upipe_play->latency)))
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);

    upipe_play_sub_store_flow_def(upipe, flow_def);
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return an error code
 */
static int upipe_play_sub_set_flow_def(struct upipe *upipe,
                                       struct uref *flow_def)
{
    if (flow_def == NULL)
        return UBASE_ERR_INVALID;
    flow_def = uref_dup(flow_def);
    if (unlikely(flow_def == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }
    upipe_play_sub_store_flow_def(upipe, flow_def);

    uint64_t latency = 0;
    uref_clock_get_latency(flow_def, &latency);
    /* we never lower latency */
    struct upipe_play *upipe_play =
        upipe_play_from_sub_mgr(upipe->mgr);
    if (latency > upipe_play->input_latency)
        upipe_play_set_input_latency(upipe_play_to_upipe(upipe_play), latency);
    else
        upipe_play_sub_build_flow_def(upipe);

    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on an output subpipe of a
 * play pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_play_sub_control(struct upipe *upipe,
                                  int command, va_list args)
{
    switch (command) {
        case UPIPE_AMEND_FLOW_FORMAT: {
            struct uref *flow_format = va_arg(args, struct uref *);
            return upipe_throw_new_flow_format(upipe, flow_format, NULL);
        }
        case UPIPE_GET_FLOW_DEF: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_play_sub_get_flow_def(upipe, p);
        }
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_play_sub_set_flow_def(upipe, flow_def);
        }
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_play_sub_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_play_sub_set_output(upipe, output);
        }
        case UPIPE_SUB_GET_SUPER: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_play_sub_get_super(upipe, p);
        }

        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_play_sub_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);

    upipe_play_sub_clean_output(upipe);
    upipe_play_sub_clean_sub(upipe);
    upipe_play_sub_clean_urefcount(upipe);
    upipe_play_sub_free_void(upipe);
}

/** @internal @This initializes the output manager for a play pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_play_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_play *upipe_play =
        upipe_play_from_upipe(upipe);
    struct upipe_mgr *sub_mgr = &upipe_play->sub_mgr;
    sub_mgr->refcount = upipe_play_to_urefcount(upipe_play);
    sub_mgr->signature = UPIPE_PLAY_SUB_SIGNATURE;
    sub_mgr->upipe_alloc = upipe_play_sub_alloc;
    sub_mgr->upipe_input = upipe_play_sub_output;
    sub_mgr->upipe_control = upipe_play_sub_control;
    sub_mgr->upipe_mgr_control = NULL;
}

/** @internal @This allocates a play pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_play_alloc(struct upipe_mgr *mgr,
                                      struct uprobe *uprobe,
                                      uint32_t signature, va_list args)
{
    struct upipe *upipe = upipe_play_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;
    upipe_play_init_urefcount(upipe);
    upipe_play_init_sub_mgr(upipe);
    upipe_play_init_sub_subs(upipe);
    struct upipe_play *upipe_play = upipe_play_from_upipe(upipe);
    upipe_play->input_latency = 0;
    upipe_play->output_latency = upipe_play->latency = DEFAULT_OUTPUT_LATENCY;
    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This sets the latency and rebuilds flow definitions.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_play_set_latency(struct upipe *upipe)
{
    struct upipe_play *upipe_play = upipe_play_from_upipe(upipe);
    upipe_play->latency = upipe_play->input_latency +
                          upipe_play->output_latency;

    struct uchain *uchain;
    ulist_foreach (&upipe_play->subs, uchain) {
        struct upipe_play_sub *upipe_play_sub =
            upipe_play_sub_from_uchain(uchain);
        upipe_play_sub_build_flow_def(upipe_play_sub_to_upipe(upipe_play_sub));
    }
}

/** @internal @This sets the input latency.
 *
 * @param upipe description structure of the pipe
 * @param latency input latency
 */
static void upipe_play_set_input_latency(struct upipe *upipe, uint64_t latency)
{
    struct upipe_play *upipe_play = upipe_play_from_upipe(upipe);
    upipe_play->input_latency = latency;
    upipe_play_set_latency(upipe);
}

/** @This returns the current output latency.
 *
 * @param upipe description structure of the pipe
 * @param latency_p filled with the current output latency
 * @return an error code
 */
static inline int _upipe_play_get_output_latency(struct upipe *upipe,
                                                 uint64_t *latency_p)
{
    struct upipe_play *upipe_play = upipe_play_from_upipe(upipe);
    *latency_p = upipe_play->output_latency;
    return UBASE_ERR_NONE;
}

/** @This sets the playing output latency.
 *
 * @param upipe description structure of the pipe
 * @param latency new output latency
 * @return an error code
 */
static inline int _upipe_play_set_output_latency(struct upipe *upipe,
                                                 uint64_t latency)
{
    struct upipe_play *upipe_play = upipe_play_from_upipe(upipe);
    upipe_play->output_latency = latency;
    upipe_play_set_latency(upipe);
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on a play pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_play_control(struct upipe *upipe, int command, va_list args)
{
    switch (command) {
        case UPIPE_GET_SUB_MGR: {
            struct upipe_mgr **p = va_arg(args, struct upipe_mgr **);
            return upipe_play_get_sub_mgr(upipe, p);
        }
        case UPIPE_ITERATE_SUB: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_play_iterate_sub(upipe, p);
        }

        case UPIPE_PLAY_GET_OUTPUT_LATENCY: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_PLAY_SIGNATURE)
            uint64_t *p = va_arg(args, uint64_t *);
            return _upipe_play_get_output_latency(upipe, p);
        }
        case UPIPE_PLAY_SET_OUTPUT_LATENCY: {
            UBASE_SIGNATURE_CHECK(args, UPIPE_PLAY_SIGNATURE)
            uint64_t latency = va_arg(args, uint64_t);
            return _upipe_play_set_output_latency(upipe, latency);
        }

        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_play_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);
    upipe_play_clean_sub_subs(upipe);
    upipe_play_clean_urefcount(upipe);
    upipe_play_free_void(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_play_mgr = {
    .refcount = NULL,
    .signature = UPIPE_PLAY_SIGNATURE,

    .upipe_alloc = upipe_play_alloc,
    .upipe_input = NULL,
    .upipe_control = upipe_play_control,

    .upipe_mgr_control = NULL
};

/** @This returns the management structure for all play pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_play_mgr_alloc(void)
{
    return &upipe_play_mgr;
}