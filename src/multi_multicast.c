/*
 * Copyright 2013 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of Multi Network Manager (MNM). MNM is free software: you
 * can redistribute it and/or modify it under the terms of the Lesser GNU
 * General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * MNM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Network Device Listener. If not, see http://www.gnu.org/licenses/.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/netlink.h>
#include <glib.h>

#include "multi_common.h"
#include "multi_core.h"
#include "multi_shared.h"

extern char *optarg;
extern int32_t opterr;
extern struct multi_config* initialize_config(uint8_t *cfg_file, 
        uint8_t unique);
extern pthread_t multi_start(struct multi_config *mc);

struct iface{
    int32_t ifi_idx;
    struct nlmsghdr *nlmsg;
};

static gint test_match_idx(gconstpointer a, gconstpointer b){
    struct iface *ni = (struct iface*) a;
    int32_t *ifi_idx = (int32_t *) b;

    if(ni->ifi_idx == *ifi_idx)
        return 0;
    
    return 1;
}

void multi_test_visible_loop(struct multi_config *mc){
    uint8_t buf[MAX_BUFSIZE];
    int32_t retval;
    int32_t i;
    int32_t netlink_sock = 0;
    int32_t ifi_idx = 0;
    struct iface *ni = NULL;
    GSList *ifaces = NULL, *ifaces_tmp = NULL;

    /* Needed to create the netlink messages  */
    struct sockaddr_nl src_addr, dest_addr;
    struct iovec iov;
    struct msghdr msg;

    /* Select is used for easier timeouts  */
    fd_set copy, master;
    int32_t fdmax;
    struct timeval tv;

    memset(buf, 0, MAX_BUFSIZE);

    /* Define a private constant later! Needs to be set in netlink.h so that the
     * kernel will allow the socket to be created! */
    if((netlink_sock = socket(PF_NETLINK, SOCK_RAW, NETLINK_GENERIC)) < 0){
        perror("Could not create netlink socket: ");
        exit(EXIT_FAILURE);
    }

    /* These are both constant!  */
    memset(&src_addr, 0, sizeof(src_addr));
    memset(&dest_addr, 0, sizeof(dest_addr));
    memset(&msg, 0, sizeof(msg));

    src_addr.nl_family = AF_NETLINK;
    //If PID is set to zero, then the kernel assigns the unique value
    src_addr.nl_pid = 0; 
    //This is the source, it only multicasts, so it does not have to be 
    //member of any groups!
    src_addr.nl_groups = 0; 

    if(bind(netlink_sock, (struct sockaddr *) &src_addr, sizeof(src_addr)) < 0){
        perror("Could not bind netlink socket: ");
        exit(EXIT_FAILURE);
    }

    dest_addr.nl_family = AF_NETLINK;
    dest_addr.nl_pid = 0; 
    dest_addr.nl_groups = 1;

    MULTI_DEBUG_PRINT(stderr, "Multi manager is ready! Netlink socket %d\n", 
            netlink_sock);
    MULTI_DEBUG_PRINT(stderr, "M S\n");

    //Look at the Wikipedia site for iovec and man(7) netlink for examples on
    //how to properly parse netlink and have multiple iovec-entries
    msg.msg_name = (void *) &dest_addr; //This is the message's destination
    msg.msg_namelen = sizeof(dest_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1; 

    //Initialise everything related to select
    FD_ZERO(&master);
    FD_ZERO(&copy);
    FD_SET(mc->socket_pipe[0], &master);
    fdmax = mc->socket_pipe[0];
    tv.tv_sec = 30;
    tv.tv_usec = 0;

    pthread_t multi_thread = multi_start(mc);

    while(1){
        copy = master;
        retval = select(fdmax + 1, &copy, NULL, NULL, &tv);

        //Repeat every UP message
        if(retval == 0){
            ifaces_tmp = ifaces;

            while(ifaces_tmp){
                ni = (struct iface*) ifaces_tmp->data;
                iov.iov_base = (void *) ni->nlmsg;
                iov.iov_len = ni->nlmsg->nlmsg_len;
                sendmsg(netlink_sock, &msg, 0);

                ifaces_tmp = g_slist_next(ifaces_tmp);
            }

            tv.tv_sec = 30;
            tv.tv_usec = 0;
            continue;
        }

        memset(buf, 0, MAX_BUFSIZE);

        //Sufficient to just memcpy this one and broadcast the netlink message
        retval = read(mc->socket_pipe[0], buf, MAX_BUFSIZE);

        if(retval == -1)
            perror("Failed to read from pipe");
        else {
            //Need to use int for storing index. It can grow much larger than
            //255 due to usb devices going up and down
            memcpy(&ifi_idx, (buf+1), sizeof(int32_t));

            if(buf[0] == LINK_UP){
                if(g_slist_find_custom(ifaces, &ifi_idx, test_match_idx))
                    continue;

                //Create a new iface, buffer the up message and add to list
                ni = (struct iface *) malloc(sizeof(struct iface));
                ni->ifi_idx = ifi_idx;
                ni->nlmsg = (struct nlmsghdr *) malloc(NLMSG_SPACE(retval));
                memset(ni->nlmsg, 0, NLMSG_SPACE(retval));
                ni->nlmsg->nlmsg_pid = getpid();
                ni->nlmsg->nlmsg_flags = 0;
                ni->nlmsg->nlmsg_len = NLMSG_SPACE(retval);
                memcpy(NLMSG_DATA(ni->nlmsg), buf, retval);
                ifaces = g_slist_prepend(ifaces, (void *) ni);

                //Adjust the base pointer of the message and broadcast message
                iov.iov_base = (void *) ni->nlmsg;
                iov.iov_len = ni->nlmsg->nlmsg_len;
                retval = sendmsg(netlink_sock, &msg, 0);
                MULTI_DEBUG_PRINT(stderr,"Broadcasted %d bytes about an UP "
                        "change in network state\n", retval);
            } else {
                if(ifaces_tmp = g_slist_find_custom(ifaces, &ifi_idx, 
                            test_match_idx)){
                    //Forward message from MULTI
                    ni = (struct iface *) ifaces_tmp->data;
                    ni->nlmsg->nlmsg_len = NLMSG_SPACE(retval);
                    memcpy(NLMSG_DATA(ni->nlmsg), buf, retval);
                    iov.iov_base = (void *) ni->nlmsg;
                    iov.iov_len = ni->nlmsg->nlmsg_len;
                    retval = sendmsg(netlink_sock, &msg, 0);
                    MULTI_DEBUG_PRINT(stderr,"Broadcasted %d bytes about a "
                            "DOWN change in network state\n", retval);
                    
                    //Remove iface from list, only interested in interfaces that
                    //are up
                    free(ni);
                    ifaces = g_slist_remove_link(ifaces, ifaces_tmp);
                    g_slist_free_1(ifaces_tmp);

                }
            }
        }

    }
}

/* Parse arguments, start multi-thread and get on with life */
int main(int argc, char *argv[]){
    int32_t c;
    char *conf_file = NULL;
    struct multi_config *mc = NULL; //Do NOT free this struct
    uint8_t daemon_mode = 0;
    uint8_t unique_mode = 0;

    if(geteuid() != 0){
        fprintf(stderr, "Application MUST be run as root\n");
        exit(EXIT_FAILURE);
    }

    /* Supress any error-messages from getopt */
    opterr = 0;

    while((c = getopt(argc, argv, "c:du")) != -1){
        switch(c){
            case 'c':
                conf_file = optarg;
                break;
            case'd':
                daemon_mode = 1;
                break;
            case 'u':
                unique_mode = 1;
                break;
            default:
                abort();
        }
    }

    if((mc = multi_core_initialize_config(conf_file, unique_mode)) == NULL){
        printf("Could not initialize configuration struct\n");
        abort();
    }

    if(daemon_mode){
        if(daemon(0, 0) == -1){
            perror("Could not daemonize MULTI: ");
            exit(EXIT_FAILURE);
        }

        if(freopen("/var/log/multi.log", "a", stderr) == NULL){
            perror("freopen failed: ");
            exit(EXIT_FAILURE);
        } 
    }

    multi_test_visible_loop(mc);
}
