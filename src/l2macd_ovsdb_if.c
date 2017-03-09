/*
 *Copyright (C) 2016 Hewlett-Packard Development Company, L.P.
 *All Rights Reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 */

/*************************************************************************//**
 * @ingroup l2macd
 *
 * @file
 * Main source file for the implementation of l2macd's OVSDB interface.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dynamic-string.h>
#include <vswitch-idl.h>
#include <openswitch-idl.h>
#include <openvswitch/vlog.h>
#include <hash.h>
#include <shash.h>
#include "hmap.h"
#include "l2macd.h"
#include "poll-loop.h"
#include "util.h"
#include "timeval.h"

VLOG_DEFINE_THIS_MODULE(l2macd_ovsdb_if);

struct vlan_data {
    struct hmap_node hmap_node;  /* In struct l2mac_table "vlans" hmap. */
    int vlan_id;                 /* VLAN ID */
    bool op_state;              /* VLAN operational status */
};

struct port_data {
    struct hmap_node hmap_node;     /* In struct l2mac_table "port" hmap. */
    char *name;                     /* Port name*/
    bool link_state;                /* Link status . */
};

/* L2MACD Internal data cache. */
struct l2macd_data_cache {
    struct hmap port_table;     /* Port table.cache */
    struct hmap vlan_table;     /* VLAN table cache */
};

struct ovsdb_idl *idl;
static unsigned int idl_seqno;

static int system_configured = false;

static struct l2macd_data_cache *g_l2macd_cache = NULL;

#define IS_CHANGED(x,y) (x != y)

/*-----------------------------------------------------------------------------
 | Function: l2macd_ovsdb_init
 | Responsibility: Create a connection to the OVSDB at db_path and create a DB cache
 | Parameters:
 |      db_path : OVSDB db path
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
void
l2macd_ovsdb_init(const char *db_path)
{
    /* Initialize IDL through a new connection to the DB. */
    idl = ovsdb_idl_create(db_path, &ovsrec_idl_class, false, true);
    idl_seqno = ovsdb_idl_get_seqno(idl);
    ovsdb_idl_set_lock(idl, "ops_l2macd");

    /* Cache System table. */
    ovsdb_idl_add_table(idl, &ovsrec_table_system);
    ovsdb_idl_add_column(idl, &ovsrec_system_col_cur_cfg);

    /* Cache Interface table columns. */
    ovsdb_idl_add_table(idl, &ovsrec_table_interface);
    ovsdb_idl_add_column(idl, &ovsrec_interface_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_interface_col_link_state);
    ovsdb_idl_add_column(idl, &ovsrec_interface_col_type);

    /* Cache Port table columns. */
    ovsdb_idl_add_table(idl, &ovsrec_table_port);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_vlan_mode);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_vlan_tag);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_vlan_trunks);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_interfaces);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_macs_invalid);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_macs_invalid_on_vlans);

    /* Track port table columns. */
    ovsdb_idl_track_add_column(idl, &ovsrec_port_col_name);
    ovsdb_idl_track_add_column(idl, &ovsrec_port_col_vlan_mode);
    ovsdb_idl_track_add_column(idl, &ovsrec_port_col_vlan_tag);
    ovsdb_idl_track_add_column(idl, &ovsrec_port_col_vlan_trunks);
    ovsdb_idl_track_add_column(idl, &ovsrec_port_col_interfaces);
    ovsdb_idl_track_add_column(idl, &ovsrec_port_col_macs_invalid);
    ovsdb_idl_track_add_column(idl, &ovsrec_port_col_macs_invalid_on_vlans);

    /* Cache VLAN table columns. */
    ovsdb_idl_add_table(idl, &ovsrec_table_vlan);
    ovsdb_idl_add_column(idl, &ovsrec_vlan_col_id);
    ovsdb_idl_add_column(idl, &ovsrec_vlan_col_oper_state);
    ovsdb_idl_add_column(idl, &ovsrec_vlan_col_macs_invalid);

    /* Track VLAN table columns. */
    ovsdb_idl_track_add_column(idl, &ovsrec_vlan_col_id);
    ovsdb_idl_track_add_column(idl, &ovsrec_vlan_col_oper_state);
    ovsdb_idl_track_add_column(idl, &ovsrec_vlan_col_macs_invalid);
} /* l2macd_ovsdb_init */

/*-----------------------------------------------------------------------------
 | Function: l2macd_cache_init
 | Responsibility: create a local cache to store ports data and vlan data
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
void
l2macd_cache_init(void)
{
    /* Allocate Memory */
    g_l2macd_cache = xzalloc(sizeof *g_l2macd_cache);
    ovs_assert(g_l2macd_cache != NULL);
    hmap_init(&g_l2macd_cache->vlan_table);
    hmap_init(&g_l2macd_cache->port_table);
}   /* l2macd_cache_init */

/*-----------------------------------------------------------------------------
 | Function: l2macd_ovsdb_exit
 | Responsibility: l2macd exit function
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
void
l2macd_ovsdb_exit(void)
{
    struct port_data *port, *next_port;
    struct vlan_data *vlan, *next_vlan;

    /* Free port table. */
    HMAP_FOR_EACH_SAFE (port, next_port, hmap_node,
                        &g_l2macd_cache->port_table) {
        free(port);
    }

    /* Free vlan table.*/
    HMAP_FOR_EACH_SAFE (vlan, next_vlan, hmap_node,
                        &g_l2macd_cache->vlan_table) {
        free(vlan);
    }

    hmap_destroy(&g_l2macd_cache->vlan_table);
    hmap_destroy(&g_l2macd_cache->port_table);
    free(g_l2macd_cache);
    ovsdb_idl_destroy(idl);
} /* l2macd_ovsdb_exit */


/*-----------------------------------------------------------------------------
 | Function: mac_flush_by_port
 | Responsibility: Trigger mac flush on specific port by setting mac_invalid column
 | Parameters:
 |      port_row: port row in the idl
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
mac_flush_by_port(const struct ovsrec_port *port_row)
{
    struct ovsdb_idl_txn *txn = NULL;
    enum ovsdb_idl_txn_status status = TXN_SUCCESS;
    bool mac_invalid = true;

    if (port_row == NULL)   {
        return;
    }

    txn = ovsdb_idl_txn_create(idl);

    /* Set MAC flush request on this port */
    ovsrec_port_set_macs_invalid(port_row, &mac_invalid, 1);
    ovsrec_port_verify_macs_invalid(port_row);

    ovsdb_idl_txn_add_comment(txn, "l2macd-%s-flush", port_row->name);
    status = ovsdb_idl_txn_commit_block(txn);

    VLOG_DBG("%s: flush %s \n", __FUNCTION__, port_row->name);

    if (status != TXN_SUCCESS)    {
        ovsdb_idl_txn_abort(txn);
        VLOG_ERR("%s: txn_commit status %d \n",
                 __FUNCTION__, status);
    }

    ovsdb_idl_txn_destroy(txn);

}/* mac_flush_by_port */

/*-----------------------------------------------------------------------------
 | Function: check_system_iface
 | Responsibility: Checks interface is system type
 | Parameters:
 |      port_row: port row in the idl
 | Return:
 |      bool : if the interface type is system, return true
     ------------------------------------------------------------------------------
 */
static bool
check_system_iface(const struct ovsrec_port *port)
{
    int i = 0;
    bool rc = false;

    if (port == NULL)   {
        return rc;
    }

    for (i = 0; i < port->n_interfaces; i++) {
        const char *type = port->interfaces[i]->type;
        if (!strncmp(type, OVSREC_INTERFACE_TYPE_SYSTEM,
                     strlen(OVSREC_INTERFACE_TYPE_SYSTEM))) {
            rc = true;
        }
    }

    return rc;
}   /* check_system_iface */

/*-----------------------------------------------------------------------------
 | Function: port_lookup
 | Responsibility: port lookup in the global cache
 | Parameters:
 |      port_hmap: ports hash map
 |      name: port name
 | Return:
 |      port_data: return the port_data, if the specified port name exists
     ------------------------------------------------------------------------------
 */
static struct port_data *
port_lookup(const struct hmap* port_hmap, const char *name)
{
    struct port_data *local_port = NULL;

    if (port_hmap == NULL || name == NULL)   {
        return local_port;
    }

    HMAP_FOR_EACH_WITH_HASH (local_port, hmap_node, hash_string(name, 0),
                             port_hmap) {
        if (!strncmp(local_port->name, name,
                     strlen(name))) {
            return local_port;
        }
    }

    return local_port;
}   /* port_lookup */


/*-----------------------------------------------------------------------------
 | Function: update_port_state
 | Responsibility: Update the port details in the global cache and flush the mac
 |                      if port state is down
 | Parameters:
 |      port_row: port row in the idl
 |      port_data: port_data cache
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
update_port_state(const struct ovsrec_port *port_row,
                     struct port_data *port_data)
{
    int i = 0;
    bool link_up = false;
    bool flush = false;

    if (port_row == NULL || port_data == NULL)   {
        return;
    }

    /* Make sure all the interfaces part of the logical port is down.*/
    for (i = 0; i < port_row->n_interfaces; i++) {
        struct ovsrec_interface *iface_row = port_row->interfaces[i];

        if (iface_row && iface_row->link_state &&
            !strncmp(iface_row->link_state, OVSREC_INTERFACE_LINK_STATE_UP,
                     strlen(OVSREC_INTERFACE_LINK_STATE_UP))) {
            link_up = true;
        }
    }

    if (IS_CHANGED(port_data->link_state, link_up)) {
        flush = true;
    }

    VLOG_DBG("%s: %s flush %d %d",
              __FUNCTION__,
              port_row->name, flush, link_up);

    /* Flush only link down cases */
    if (flush && !link_up){
        mac_flush_by_port(port_row);
    }

    /* Update link status */
    port_data->link_state = link_up;
}/* update_port_state */

/*-----------------------------------------------------------------------------
 | Function: update_port
 | Responsibility: Create/Update the port details in the global cache
 | Parameters:
 |      port_row: port row in the idl
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
update_port(const struct ovsrec_port *port_row)
{
    struct port_data *port_data = NULL;

    /* Check interface table for valid physical interface */
    if (!check_system_iface(port_row))  {
       VLOG_DBG("%s: %s interface type is not system",
                 __FUNCTION__,
                 port_row->name);
       return;
    }

    port_data = port_lookup(&g_l2macd_cache->port_table, port_row->name);

    if (!port_data)  {
        port_data = xzalloc(sizeof *port_data);
        port_data->name = xstrdup(port_row->name);
        hmap_insert(&g_l2macd_cache->port_table, &port_data->hmap_node,
                    hash_string(port_row->name, 0));
        port_data->link_state= false;
    }

    update_port_state(port_row, port_data);

    VLOG_DBG("%s: %s added count %zu", __FUNCTION__,
              port_data->name, hmap_count(&g_l2macd_cache->port_table));
} /* update_port */

/*-----------------------------------------------------------------------------
 | Function: del_old_port
 | Responsibility: Delete the port from the global cache
 | Parameters:
 |      None
 | Return:
 |      None
 |Note : Don't access port_row, Track looses deleted port_row information except UUID
     ------------------------------------------------------------------------------
 */
static void
del_old_port(void)
{
    struct shash all_ports;
    const struct ovsrec_port *ports = NULL;
    struct port_data *port_data = NULL, *next_port_data = NULL;

    shash_init(&all_ports);
    OVSREC_PORT_FOR_EACH(ports, idl)    {
        const char *name = ports->name;
        if (!shash_add_once(&all_ports, name, ports)) {
            VLOG_WARN("port %s specified twice ", name);
        }
    }

    HMAP_FOR_EACH_SAFE (port_data, next_port_data, hmap_node,
                        &g_l2macd_cache->port_table) {
        /* Check port is deleted */
        if (shash_find_data(&all_ports, port_data->name) == NULL)   {
            VLOG_DBG("%s: %s ports count %zu", __FUNCTION__,
                      port_data->name,
                      hmap_count(&g_l2macd_cache->port_table));
            hmap_remove(&g_l2macd_cache->port_table, &port_data->hmap_node);
            free(port_data->name);
            free(port_data);
        }
    }

    /* Destroy ports hash */
    shash_destroy(&all_ports);
} /* del_old_port */

/*-----------------------------------------------------------------------------
 | Function: update_port_cache
 | Responsibility: Track the port changes and update cache
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
update_port_cache(void)
{
    const struct ovsrec_port *port_row = NULL;
    int i = 0;
    unsigned int new_idl_seqno = ovsdb_idl_get_seqno(idl);

    /* Track all the ports changes in the DB. */
    OVSREC_PORT_FOR_EACH_TRACKED(port_row, idl) {
        /* Add new ports to the cache. */
        if(ovsrec_port_row_get_seqno(port_row, OVSDB_IDL_CHANGE_INSERT)
                           >= new_idl_seqno)  {
            update_port(port_row);
        }

        /* Delete ports from the cache. */
        if(ovsrec_port_row_get_seqno(port_row, OVSDB_IDL_CHANGE_DELETE)
                   >= new_idl_seqno)  {
            del_old_port();
        }

        /* Update modified ports to the cache. */
        if(ovsrec_port_row_get_seqno(port_row, OVSDB_IDL_CHANGE_MODIFY)
                   >= new_idl_seqno)  {
            update_port(port_row);
        }
    }

    /* Update port table cache for interface row changes also. */
    OVSREC_PORT_FOR_EACH(port_row, idl) {
        /* Handle Interface changes */
        for (i = 0; i < port_row->n_interfaces; i++) {
            struct ovsrec_interface *iface_row = port_row->interfaces[i];
            if (OVSREC_IDL_IS_ROW_MODIFIED(iface_row, idl_seqno)) {
                update_port(port_row);
            }
        }
    }

} /* update_port_cache */

/*-----------------------------------------------------------------------------
 | Function: mac_flush_by_vlan
 | Responsibility: Triggers the mac flush on the spcified vlan by setting mac_invalid column
 | Parameters:
 |      vlan_row: VLAN row in the IDL
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
mac_flush_by_vlan(const struct ovsrec_vlan *vlan_row)
{
    struct ovsdb_idl_txn *txn;
    bool mac_invalid = true;
    enum ovsdb_idl_txn_status status = TXN_SUCCESS;

    if (vlan_row == NULL)   {
        return;
    }

    txn = ovsdb_idl_txn_create(idl);

    /* Set MAC flush request on this VLAN. */
    ovsrec_vlan_set_macs_invalid(vlan_row, &mac_invalid, 1);
    ovsrec_vlan_verify_macs_invalid(vlan_row);
    ovsdb_idl_txn_add_comment(txn, "l2macd-%"PRId64"-flush", vlan_row->id);

    status = ovsdb_idl_txn_commit_block(txn);
    VLOG_DBG("%s: vlan %"PRId64" status %d \n", __FUNCTION__,
             vlan_row->id, status);

    if (status != TXN_SUCCESS)    {
        ovsdb_idl_txn_abort(txn);
        VLOG_ERR("%s: txn_commit status %d \n",
                 __FUNCTION__, status);
    }

    ovsdb_idl_txn_destroy(txn);
}

/*-----------------------------------------------------------------------------
 | Function: update_vlan_state
 | Responsibility: Update the VLAN details in the global cache and flush the mac
 |                      if VLAN state is down
 | Parameters:
 |      row: VLAN row in the idl
 |      vlan_data: vlan_data cache
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
update_vlan_state(const struct ovsrec_vlan *row, struct vlan_data *vlan_ptr)
{
    bool prev_op_up = false, op_up = false;

    if (row == NULL || vlan_ptr == NULL)   {
        return;
    }

    vlan_ptr->vlan_id = (int) row->id;
    prev_op_up = vlan_ptr->op_state;

    /* Update oper_state to unknown. */
    if (row->oper_state &&
        !strncmp(OVSREC_VLAN_OPER_STATE_UP, row->oper_state,
                 strlen(OVSREC_VLAN_OPER_STATE_UP))) {
        op_up = true;
    }

    /* Flush only VLAN operational down cases */
    if (OVSREC_IDL_IS_COLUMN_MODIFIED(ovsrec_vlan_col_oper_state,
                                      idl_seqno)
        && prev_op_up == true && op_up == false) {
        mac_flush_by_vlan(row);
    }

    /* Update the VLAN oper_state */
    vlan_ptr->op_state = op_up;
} /* update_vlan_state */


/*-----------------------------------------------------------------------------
 | Function: vlan_lookup_by_vid
 | Responsibility: VLAN lookup in the global cache
 | Parameters:
 |      vlan_hmap: VLANs hash map
 |      vid: VLAN ID
 | Return:
 |      vlan_data: return the vlan_data, if the specified vlan exists
     ------------------------------------------------------------------------------
 */
static inline struct vlan_data *
vlan_lookup_by_vid(const struct hmap* vlan_hmap, int vid)
{
    struct vlan_data *vlan = NULL;

    if (vlan_hmap == NULL)   {
        return vlan;
    }

    HMAP_FOR_EACH (vlan, hmap_node, vlan_hmap) {
        if (vlan->vlan_id == vid) {
            return vlan;
        }
    }
    return vlan;
}

/*-----------------------------------------------------------------------------
 | Function: update_vlan
 | Responsibility: Create/Update the VLAN details in the global cache
 | Parameters:
 |      vlan_row: VLAN row in the idl
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
update_vlan(const struct ovsrec_vlan *vlan_row)
{
    struct vlan_data *new_vlan = NULL;

    if (vlan_row == NULL)   {
        return;
    }

    new_vlan = vlan_lookup_by_vid(&g_l2macd_cache->vlan_table,
                                  vlan_row->id);
    if(!new_vlan)  {
        /* Allocate structure to save state information for this VLAN. */
        new_vlan = xzalloc(sizeof(struct vlan_data));
        new_vlan->vlan_id = (int) vlan_row->id;
        new_vlan->op_state= false;
        hmap_insert(&g_l2macd_cache->vlan_table, &new_vlan->hmap_node,
                        hash_int(vlan_row->id, 0));
    }

    /* Update VLAN configuration into internal format. */
    update_vlan_state(vlan_row, new_vlan);

    VLOG_DBG("%s: %d, hmap count %zu", __FUNCTION__, (int)vlan_row->id,
             hmap_count(&g_l2macd_cache->vlan_table));
} /* add_new_vlan */

/*-----------------------------------------------------------------------------
 | Function: del_old_vlan
 | Responsibility: Delete the VLAN from the global cache
 | Parameters:
 |      None
 | Return:
 |      None
 |Note : Don't access vlan_row, Track looses deleted vlan_row information except UUID
     ------------------------------------------------------------------------------
 */
static void
del_old_vlan(void)
{
    const struct ovsrec_vlan *vlan_row = NULL;
    struct vlan_data *vlan = NULL, *vlan_next = NULL;
    bool vlan_found = false;

    /* Check all the VLANs present in the DB. */
    HMAP_FOR_EACH_SAFE (vlan, vlan_next, hmap_node,
                        &g_l2macd_cache->vlan_table) {

        vlan_found = false;
        OVSREC_VLAN_FOR_EACH(vlan_row, idl) {
            if ((vlan->vlan_id == vlan_row->id)) {
                vlan_found = true;
                break;
            }
        }

        /* Handle deleted vlans */
        if (vlan_found == false)   {
            VLOG_DBG("%s: vlan_id %d hmap count %zu", __FUNCTION__,
                      vlan->vlan_id,
                      hmap_count(&g_l2macd_cache->vlan_table));
            hmap_remove(&g_l2macd_cache->vlan_table,
                        &vlan->hmap_node);
            /* Free the VLAN data */
            free(vlan);

        }
    }

    VLOG_DBG("%s: hmap count %zu", __FUNCTION__,
              hmap_count(&g_l2macd_cache->vlan_table));
} /* del_old_vlan */

/*-----------------------------------------------------------------------------
 | Function: update_vlan_cache
 | Responsibility: Track the VLAN table changes and update cache
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
update_vlan_cache(void)
{
    const struct ovsrec_vlan *vlan_row;
    unsigned int new_idl_seqno = ovsdb_idl_get_seqno(idl);

    /* Track all the VLAN changes in the DB. */
    OVSREC_VLAN_FOR_EACH_TRACKED(vlan_row, idl) {
        /* Add new VLAN to the cache */
        if(ovsrec_vlan_row_get_seqno(vlan_row, OVSDB_IDL_CHANGE_INSERT)
                           >= new_idl_seqno)  {
            update_vlan(vlan_row);
        }

        /* Update modified VLAN to the cache */
        if(ovsrec_vlan_row_get_seqno(vlan_row, OVSDB_IDL_CHANGE_MODIFY)
                           >= new_idl_seqno)  {
            update_vlan(vlan_row);
        }

        /* Delete VLAN from the cache */
        if(ovsrec_vlan_row_get_seqno(vlan_row, OVSDB_IDL_CHANGE_DELETE)
                           >= new_idl_seqno)  {
            del_old_vlan();
        }
    }

    return;
} /* update_vlan_cache */

/*-----------------------------------------------------------------------------
 | Function: l2macd_reconfigure
 | Responsibility: Monitor IDL changes
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static void
l2macd_reconfigure(void)
{
    unsigned int new_idl_seqno = ovsdb_idl_get_seqno(idl);

    if (new_idl_seqno == idl_seqno) {
        /* There was no change in the DB. */
        return;
    }

    /* Update Port table cache. */
    update_port_cache();

    /* Update VLAN table cache. */
    update_vlan_cache();

    /* Update IDL sequence # after we've handled everything. */
    idl_seqno = new_idl_seqno;

    /* Clear all the track */
    ovsdb_idl_track_clear(idl);
} /* l2macd_reconfigure */

/*-----------------------------------------------------------------------------
 | Function: l2macd_chk_for_system_configured
 | Responsibility: Checks system configuration state
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
static inline void
l2macd_chk_for_system_configured(void)
{
    const struct ovsrec_system *sys = NULL;

    if (system_configured) {
        /* Nothing to do if we're already configured. */
        return;
    }

    sys = ovsrec_system_first(idl);

    if (sys && sys->cur_cfg > (int64_t) 0) {
        system_configured = true;
        VLOG_DBG("System is now configured (cur_cfg=%d).",
                  (int)sys->cur_cfg);
    }

} /* l2macd_chk_for_system_configured */

/*-----------------------------------------------------------------------------
 | Function: l2macd_run
 | Responsibility: Handles configuration state changes from OVSDB
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
void
l2macd_run(void)
{
    /* Process a batch of messages from OVSDB. */
    ovsdb_idl_run(idl);

    if (ovsdb_idl_is_lock_contended(idl)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);

        VLOG_ERR_RL(&rl, "Another l2macd process is running, "
                    "disabling this process until it goes away");

        return;
    } else if (!ovsdb_idl_has_lock(idl)) {
        return;
    }

    /* Update the local configuration and push any changes to the DB.
     * Only do this after the system has been configured by CFGD, i.e.
     * table System "cur_cfg" > 1.
    */
    l2macd_chk_for_system_configured();
    if (system_configured) {
        l2macd_reconfigure();
    }

    return;
} /* l2macd_run */

/*-----------------------------------------------------------------------------
 | Function: l2macd_wait
 | Responsibility: Register to poll_block for IDL changes
 | Parameters:
 |      None
 | Return:
 |      None
     ------------------------------------------------------------------------------
 */
void
l2macd_wait(void)
{
    ovsdb_idl_wait(idl);
} /* l2macd_wait */
