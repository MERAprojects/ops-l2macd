/*
 * Copyright (C) 2000 Kunihiro Ishiguro
 * Copyright (C) 2015 Hewlett Packard Enterprise Development LP
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
/****************************************************************************
 * @ingroup cli/vtysh
 *
 * @file mac_vty.c
 *
 ***************************************************************************/

#include <sys/un.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <pwd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "vtysh/lib/version.h"
#include "vtysh/command.h"
#include "vtysh/vtysh.h"
#include "vtysh/vtysh_ovsdb_if.h"
#include "vtysh/vtysh_ovsdb_config.h"
#include "vswitch-idl.h"
#include "ovsdb-idl.h"
#include "mac_vty.h"
#include "openvswitch/vlog.h"
#include "openswitch-idl.h"
#include "smap.h"

VLOG_DEFINE_THIS_MODULE (vtysh_mac_cli);

extern struct ovsdb_idl *idl;
static struct ovsdb_idl_index_cursor cursor;

/*-----------------------------------------------------------------------------
 | Function: print_mactable
 | Responsibility: print the mac table entries
 | Parameters:
 |      nodes :sorted shash nodes
 |      count : number of entries to print
 | Return:
 |      None
 ------------------------------------------------------------------------------
 */
void
print_mactable(const struct shash_node **nodes, int count)
{
    int idx;
    const struct ovsrec_mac *row = NULL;
    char vlan_id[MAX_VLAN_ID_SIZE] = {0};

    DISPLAY_MACTABLE_HEADER(vty, count);
    for (idx = 0; idx < count; idx++)
    {
        row = (const struct ovsrec_mac *)nodes[idx]->data;
        snprintf(vlan_id, 5, "%"PRId64, (int64_t)ops_mac_get_vlan(row));
        DISPLAY_MACTABLE_ROW(vty, row, vlan_id);
    }

}

/*-----------------------------------------------------------------------------
 | Function: mactable_show
 | Responsibility: Display mac entries based on filters applied
 | Parameters:
 |      from : ogirin of the mac
 |      mac  : mac address
 | Return:
 |      CMD_SUCCESS - Command executed successfully.
 ------------------------------------------------------------------------------
 */
static int
mactable_show (const char *mac_from, const char *mac, bool show_count)
{
    const struct ovsrec_mac *row = NULL;
    struct shash sorted_mac_addr;
    const struct shash_node **nodes;
    int count = 0;

    ovsdb_idl_run (idl);

    row = ovsrec_mac_first (idl);
    if (!row)
    {
        /* no mac entries in the mac table */
        vty_out (vty, "No MAC entries found.%s", VTY_NEWLINE);
        return CMD_SUCCESS;
    }

    shash_init(&sorted_mac_addr);

    OVSREC_MAC_FOR_EACH_BYINDEX (row, &cursor)
    {
        if (NULL == row->port)
            continue;
        else if ((mac != NULL) && (strcmp(row->mac_addr, mac)) != 0)
        {
           /* skip this mac entry as this is not matching with the requested mac address */
            continue;
        }
        else if ((mac_from != NULL) && ((strcmp(row->from, mac_from)) != 0))
        {
            /* skip this mac entry as the from field is not matching with the requested */
            continue;
        }
        /* add this mac entry to hash */
        shash_add(&sorted_mac_addr, row->mac_addr, (void *)row);
    }

    /* sort based on mac address */
    nodes = shash_sort(&sorted_mac_addr);
    count = shash_count(&sorted_mac_addr);

    if (show_count)
    {
        DISPLAY_MACTABLE_COUNT(count);
    }
    else
    {
        print_mactable(nodes, count);
    }
    shash_destroy(&sorted_mac_addr);
    if (nodes != NULL)
        free(nodes);

    return CMD_SUCCESS;
}

/* will be enabled after tunnel support is added */
#ifdef HW_VTEP_SUPPORT
/*-----------------------------------------------------------------------------
 | Function: mactable_tunnel_show
 | Responsibility: Display mac entries for given tunnel
 | Parameters:
 |      tunnel : tunnel key
 | Return:
 |      CMD_SUCCESS - Command executed successfully.
 ------------------------------------------------------------------------------
 */
static int
mactable_tunnel_show (const char *tunnel)
{
    const struct ovsrec_mac *row = NULL;
    struct shash sorted_mac_addr;
    const struct shash_node **nodes;
    char tunnel_key = atoi(tunnel);
    int count = 0;

    ovsdb_idl_run (idl);

    row = ovsrec_mac_first (idl);
    if (!row)
    {
        /* no mac entries in the mac table */
        vty_out (vty, "No MAC entries found.%s", VTY_NEWLINE);
        return CMD_SUCCESS;
    }

    shash_init(&sorted_mac_addr);

    OVSREC_MAC_FOR_EACH_BYINDEX (row, &cursor)
    {
        if (tunnel_key != (*(row->tunnel_key)))
        {
            /* skip the entries with different tunnel key */
            continue;
        }
        if (NULL == row->port)
            continue;
        shash_add(&sorted_mac_addr, row->mac_addr, (void *)row);
    }

    nodes = shash_sort(&sorted_mac_addr);
    count = shash_count(&sorted_mac_addr);

    print_mactable(nodes, count);
    shash_destroy(&sorted_mac_addr);
    if (nodes != NULL)
        free(nodes);

    return CMD_SUCCESS;
}
#endif

/*-----------------------------------------------------------------------------
 | Function: mactable_vlan_show
 | Responsibility: Display mac entries based on filters applied
 | Parameters:
 |      vlan_list : list of vlans
 |      from : ogirin of the mac
 | Return:
 |      CMD_SUCCESS - Command executed successfully.
 ------------------------------------------------------------------------------
 */
static int
mactable_vlan_show(const char *vlan_list, const char *mac_from, bool show_count)
{
    const struct ovsrec_mac *row = NULL;
    struct range_list *list_temp, *list = NULL;
    struct shash sorted_mac_addr;
    const struct shash_node **nodes;
    int count = 0;

    ovsdb_idl_run (idl);

    row = ovsrec_mac_first (idl);
    if (!row)
    {
        vty_out (vty, "No MAC entries found.%s", VTY_NEWLINE);
        return CMD_SUCCESS;
    }

    /* get the vlans in a link list */
    list = cmd_get_range_value(vlan_list, 0);
    list_temp = list;

    if (list == NULL)
        return CMD_ERR_NO_MATCH;

    shash_init(&sorted_mac_addr);

    OVSREC_MAC_FOR_EACH_BYINDEX (row, &cursor)
    {
        if (mac_from != NULL && ((strcmp(row->from, mac_from) != 0)))
        {
            continue;
        }

        while (list != NULL)
        {
            int vlan_id = atoi(list->value);
            if (row->port &&
                ops_mac_get_vlan(row) == vlan_id)
            {
                shash_add(&sorted_mac_addr, row->mac_addr, (void *)row);
                break;
            }
            else
            {
                list = list->link;
            }
        }
        list = list_temp;
    }
    nodes = shash_sort(&sorted_mac_addr);
    count = shash_count(&sorted_mac_addr);

    if (show_count)
    {
        DISPLAY_MACTABLE_COUNT(count);
    }
    else
    {
        print_mactable(nodes, count);
    }

    shash_destroy(&sorted_mac_addr);
    if (nodes != NULL)
        free(nodes);

    return CMD_SUCCESS;

}

/*-----------------------------------------------------------------------------
 | Function: mactable_port_show
 | Responsibility: Display mac entries based on filters applied
 | Parameters:
 |      port_list : list of ports
 |      from : ogirin of the mac
 | Return:
 |      CMD_SUCCESS - Command executed successfully.
 ------------------------------------------------------------------------------
 */
static int
mactable_port_show(const char *port_list, const char *mac_from, bool show_count)
{
    const struct ovsrec_mac *row = NULL;
    struct shash sorted_mac_addr;
    const struct shash_node **nodes;
    int count = 0;
    struct range_list *list, *list_temp = NULL;

    ovsdb_idl_run (idl);

    row = ovsrec_mac_first (idl);
    if (!row)
    {
        vty_out (vty, "No MAC entries found.%s", VTY_NEWLINE);
        return CMD_SUCCESS;
    }

    /* get the ports in a link list */
    list = list_temp = cmd_get_range_value(port_list, 1);

    if (list == NULL)
    {
        return CMD_ERR_NO_MATCH;
    }

    shash_init(&sorted_mac_addr);

    OVSREC_MAC_FOR_EACH_BYINDEX (row, &cursor)
    {
        if (mac_from != NULL && ((strcmp(row->from, mac_from) != 0)))
        {
            continue;
        }

        while (list != NULL)
        {

            if (row->port &&
                (strcmp(list->value, row->port->name)) == 0)
            {
                shash_add(&sorted_mac_addr, row->mac_addr, (void *)row);
                break;
            }
            else
            {
                list = list->link;
            }
        }
        list = list_temp;
    }

    nodes = shash_sort(&sorted_mac_addr);
    count = shash_count(&sorted_mac_addr);

    if (show_count)
    {
        DISPLAY_MACTABLE_COUNT(count);
    }
    else
    {
        print_mactable(nodes, count);
    }

    shash_destroy(&sorted_mac_addr);
    if (nodes != NULL)
        free(nodes);

    return CMD_SUCCESS;

}

DEFUN (cli_mactable_show,
       cli_mactable_show_cmd,
       "show mac-address-table",
       SHOW_STR
       SHOW_MAC_TABLE_STR)
{

    return mactable_show(NULL, NULL, false);
}

DEFUN (cli_mactable_from_show,
       cli_mactable_from_show_cmd,
       "show mac-address-table (dynamic)",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       SHOW_MAC_DYN_STR)
{

    return mactable_show(argv[0], NULL, false);
}

DEFUN (cli_mactable_vlan_show,
       cli_mactable_vlan_show_cmd,
       "show mac-address-table vlan <A:1-4094>",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       SHOW_MAC_VLAN_STR
       MAC_VLAN_STR)
{
    return mactable_vlan_show(argv[0], NULL, false);
}

DEFUN (cli_mactable_port_show,
       cli_mactable_port_show_cmd,
       "show mac-address-table port PORTS",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       SHOW_MAC_PORT_STR
       MAC_PORT_STR)
{

    return mactable_port_show(argv[0], NULL, false);
}


DEFUN (cli_mactable_from_vlan_show,
       cli_mactable_from_vlan_show_cmd,
       "show mac-address-table (dynamic) vlan <A:1-4094>",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       SHOW_MAC_DYN_STR
       SHOW_MAC_VLAN_STR
       MAC_VLAN_STR)
{
    return mactable_vlan_show(argv[1], argv[0], false);
}

DEFUN (cli_mactable_from_port_show,
       cli_mactable_from_port_show_cmd,
       "show mac-address-table (dynamic) port PORTS",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       SHOW_MAC_DYN_STR
       SHOW_MAC_PORT_STR
       MAC_PORT_STR)
{

    return mactable_port_show(argv[1], argv[0], false);
}

DEFUN (cli_mactable_address_show,
       cli_mactable_address_show_cmd,
       "show mac-address-table address A:B:C:D:E:F",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       SHOW_MAC_ADDR_STR
       "MAC address\n")
{
    return mactable_show(NULL, argv[0], false);
}

DEFUN (cli_mactable_count_show,
       cli_mactable_count_show_cmd,
       "show mac-address-table count",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       MAC_COUNT_STR)
{
    return mactable_show(NULL, NULL, true);
}

DEFUN (cli_mactable_dyn_count_show,
       cli_mactable_dyn_count_show_cmd,
       "show mac-address-table count (dynamic)",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       MAC_COUNT_STR
       SHOW_MAC_DYN_STR)
{
    return mactable_show(argv[0], NULL, true);
}

DEFUN (cli_mactable_vlan_count_show,
       cli_mactable_vlan_count_show_cmd,
       "show mac-address-table count vlan <A:1-4094>",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       MAC_COUNT_STR
       SHOW_MAC_VLAN_STR
       MAC_VLAN_STR)
{
    return mactable_vlan_show(argv[0], NULL, true);
}

DEFUN (cli_mactable_port_count_show,
       cli_mactable_port_count_show_cmd,
       "show mac-address-table count port PORTS",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       MAC_COUNT_STR
       SHOW_MAC_PORT_STR
       MAC_PORT_STR)
{
    return mactable_port_show(argv[0], NULL, true);
}

/* will be enabled after tunnel support is added */
#ifdef HW_VTEP_SUPPORT
DEFUN (cli_mactable_tunnel_show,
       cli_mactable_tunnel_show_cmd,
       "show mac-address-table tunnel <0-4294967295>",
       SHOW_STR
       SHOW_MAC_TABLE_STR
       SHOW_MAC_TUNNEL_STR
       "tunnel key\n")
{
    return mactable_tunnel_show(argv[0]);
}
#endif
/*-----------------------------------------------------------------------------
 | Function: mac_ovsdb_init
 | Responsibility: Add mac table and columns to idl cache
 | Parameters:
 |      None
 | Return:
 |      None
 ------------------------------------------------------------------------------
 */
static void
mac_ovsdb_init(void)
{
    struct ovsdb_idl_index *index = NULL;

    ovsdb_idl_add_table(idl, &ovsrec_table_mac);
    ovsdb_idl_add_column(idl, &ovsrec_mac_col_mac_vlan);
    ovsdb_idl_add_column(idl, &ovsrec_mac_col_mac_addr);
    ovsdb_idl_add_column(idl, &ovsrec_mac_col_from);
    ovsdb_idl_add_column(idl, &ovsrec_mac_col_port);
    ovsdb_idl_add_column(idl, &ovsrec_mac_col_tunnel_key);

    /* Initialize Compound Indexes */
    index = ovsdb_idl_create_index(idl, &ovsrec_table_mac, "by_macVidFrom");
    if (index) {
        /* add indexing columns */
        ovsdb_idl_index_add_column(index, &ovsrec_mac_col_mac_addr,
                                          OVSDB_INDEX_ASC, ovsrec_mac_index_mac_addr_cmp);
        ovsdb_idl_index_add_column(index, &ovsrec_mac_col_from,
                                          OVSDB_INDEX_ASC, ovsrec_mac_index_from_cmp);
    }
    else {
        VLOG_ERR ("%s: index creation failed", __FUNCTION__);
        return;
    }
    ovsdb_idl_initialize_cursor(idl, &ovsrec_table_mac, "by_macVidFrom", &cursor);

    return;
}

/*-----------------------------------------------------------------------------
 | Function: mac_ovsdb_init
 | Responsibility: Initialize hpe-l2macd cli node
 | Parameters:
 |      None
 | Return:
 |      None
 ------------------------------------------------------------------------------
 */
void cli_pre_init(void)
{
    mac_ovsdb_init();
    return;
}

/*-----------------------------------------------------------------------------
 | Function: mac_ovsdb_init
 | Responsibility: Initialize hpe-l2macd cli element
 | Parameters:
 |      None
 | Return:
 |      None
 ------------------------------------------------------------------------------
 */
void cli_post_init(void)
{
    install_element (ENABLE_NODE, &cli_mactable_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_vlan_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_port_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_address_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_from_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_from_vlan_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_from_port_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_count_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_dyn_count_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_vlan_count_show_cmd);
    install_element (ENABLE_NODE, &cli_mactable_port_count_show_cmd);
#ifdef HW_VTEP_SUPPORT
    install_element (ENABLE_NODE, &cli_mactable_tunnel_show_cmd);
#endif
    return;
}
