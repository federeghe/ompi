/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2016 Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/mca/backtrace/backtrace.h"
#include "opal/mca/event/event.h"

#if OPAL_ENABLE_FT_CR == 1
#include "orte/mca/rml/rml.h"
#include "orte/mca/state/state.h"
#endif
#include "orte/mca/rml/base/base.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/oob/oob.h"
#include "orte/mca/oob/base/base.h"
#include "rml_oob.h"

static int rml_oob_open(void);
static int rml_oob_close(void);
static orte_rml_base_module_t* open_conduit(opal_list_t *attributes);
static orte_rml_pathway_t* query_transports(void);
static char* get_contact_info(void);
static void set_contact_info(const char *uri);
static void close_conduit(orte_rml_base_module_t *mod);
/**
 * component definition
 */
orte_rml_component_t mca_rml_oob_component = {
      /* First, the mca_base_component_t struct containing meta
         information about the component itself */

    .base = {
        ORTE_RML_BASE_VERSION_3_0_0,

        .mca_component_name = "oob",
        MCA_BASE_MAKE_VERSION(component, ORTE_MAJOR_VERSION, ORTE_MINOR_VERSION,
                              ORTE_RELEASE_VERSION),
        .mca_open_component = rml_oob_open,
        .mca_close_component = rml_oob_close,

    },
    .data = {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
    .priority = 5,
    .open_conduit = open_conduit,
    .query_transports = query_transports,
    .get_contact_info = get_contact_info,
    .set_contact_info = set_contact_info,
    .close_conduit = close_conduit
};

/* Local variables */
static orte_rml_pathway_t pathway;
static orte_rml_base_module_t base_module = {
    .component = (struct orte_rml_component_t*)&mca_rml_oob_component,
    .ping = NULL,
    .send_nb = orte_rml_oob_send_nb,
    .send_buffer_nb = orte_rml_oob_send_buffer_nb,
    .purge = NULL
};

static int rml_oob_open(void)
{
    /* ask our OOB transports for their info */
    OBJ_CONSTRUCT(&pathway, orte_rml_pathway_t);
    pathway.component = strdup("oob");
    ORTE_OOB_GET_TRANSPORTS(&pathway.transports);
    /* add any component attributes of our own */

    return ORTE_SUCCESS;
}


static int rml_oob_close(void)
{
    /* cleanup */
    OBJ_DESTRUCT(&pathway);

    return ORTE_SUCCESS;
}

static orte_rml_base_module_t* make_module(void)
{
    orte_rml_oob_module_t *mod;

    /* create a new module */
    mod = (orte_rml_oob_module_t*)malloc(sizeof(orte_rml_oob_module_t));
    if (NULL == mod) {
        return NULL;
    }

    /* copy the APIs over to it */
    memcpy(mod, &base_module, sizeof(base_module));

    /* initialize its internal storage */
    OBJ_CONSTRUCT(&mod->queued_routing_messages, opal_list_t);
    mod->timer_event = NULL;

    /* return the result */
    return (orte_rml_base_module_t*)mod;
}

static orte_rml_base_module_t* open_conduit(opal_list_t *attributes)
{
    char *comp_attrib = NULL;
    char **comps;
    int i;
    orte_attribute_t *attr;

    opal_output_verbose(20,orte_rml_base_framework.framework_output,
                    "%s - Entering rml_oob_open_conduit()",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* someone may require this specific component, so look for "oob" */
    if (orte_get_attribute(attributes, ORTE_RML_INCLUDE_COMP_ATTRIB, (void**)&comp_attrib, OPAL_STRING) &&
        NULL != comp_attrib) {
        /* they specified specific components - could be multiple */
        comps = opal_argv_split(comp_attrib, ',');
        for (i=0; NULL != comps[i]; i++) {
            if (0 == strcmp(comps[i], "oob")) {
                /* we are a candidate */
                opal_argv_free(comps);
                return make_module();
            }
        }
        /* we are not a candidate */
        opal_argv_free(comps);
        return NULL;
    } else if (orte_get_attribute(attributes, ORTE_RML_EXCLUDE_COMP_ATTRIB, (void**)&comp_attrib, OPAL_STRING) &&
               NULL != comp_attrib) {
        /* see if we are on the list */
        comps = opal_argv_split(comp_attrib, ',');
        for (i=0; NULL != comps[i]; i++) {
            if (0 == strcmp(comps[i], "oob")) {
                /* we cannot be a candidate */
                opal_argv_free(comps);
                return NULL;
            }
        }
    }

    /* Alternatively, check the attributes to see if we qualify - we only handle
     * "routed", "Ethernet", and "TCP" */
    OPAL_LIST_FOREACH(attr, attributes, orte_attribute_t) {

    }

    /* if we get here, we cannot handle it */
    return NULL;
}

static orte_rml_pathway_t* query_transports(void)
{
    /* if we have any available transports, make them available */
    if (0 < opal_list_get_size(&pathway.transports)) {
        return &pathway;
    }
    /* if not, then return NULL */
    return NULL;
}

static void close_conduit(orte_rml_base_module_t *md)
{
    orte_rml_oob_module_t *mod = (orte_rml_oob_module_t*)md;

    /* cleanup the list of messages */
    OBJ_DESTRUCT(&mod->queued_routing_messages);

    /* the rml_base_stub takes care of clearing the base receive
     * and free'ng the module */
    return;
}

static char* get_contact_info(void)
{
    char *ret;

    ORTE_OOB_GET_URI(&ret);
    return ret;
}

static void set_contact_info(const char *uri)
{
    ORTE_OOB_SET_URI(uri);
}
