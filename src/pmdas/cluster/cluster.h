/*
 * Copyright (c) 2005,2007-2009 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */


#include "../linux/indom.h"
#include "../linux/domain.h"

/*************************
 * PMDA / CLIENT INTERFACE
 */

#define LINUX_DOMAIN		LINUX
#define CLUSTER_CLIENT_PORT	44320	/* currently unassigned */
#define CLUSTER_CLIENT_VERSION	101

/* singular instance domain translation */
#define CLUSTER_INDOM		0x3fffff

/* cluster for local (non-translated) metrics */
#define CLUSTER_CLUSTER		0xfff

#define CLUSTER_NUM_CONTROL_METRICS 3

/*
 * Protocol exchange:
 *   Server listens on CLUSTER_CLIENT_PORT
 *   client connects
 *   client sends CLUSTER_PDU_VERSION
 *   client sends CLUSTER_PDU_ID
 *   client blocks
 *   server sends CLUSTER_PDU_CONFIG
 *   client sends one or more CLUSTER_PDU_INSTANCE
 *   forever: client sends CLUSTER_PDU_RESULT
 *
 * See individual PDU handlers in server.c and pmclusterd.c.
 *
 */
#define CLUSTER_PDU_VERSION	0 /* client sends: unsigned version number */
#define CLUSTER_PDU_ID		1 /* client sends: unsigned int hostnamelength, hostname */
#define CLUSTER_PDU_CONFIG	2 /* server sends metric config to client */
#define CLUSTER_PDU_RESULT	3 /* client sends an encoded pmResult PDU */
#define CLUSTER_PDU_INSTANCE	4 /* client sends: indom instance name[] */
#define CLUSTER_PDU_SUSPEND	5 /* no args */
#define CLUSTER_PDU_RESUME	6 /* no args */

#define CLUSTER_INSTANCENAME_MAXLEN 24

 /* Need > 2 + 24/sizeof(int) .. */
#define CMDBUFLEN 16 


/**************************************
 * INTERFACE BETWEEN pmda.c & server.c
 */

#define CLUSTER_CLIENT_FLAG_WANT_CONFIG		(1 << 0) /* needs config sent */
#define CLUSTER_CLIENT_FLAG_SUSPENDED		(1 << 1) /* has been sent PDU_SUSPEND */
#define CLUSTER_CLIENT_FLAG_STALE_RESULT	(1 << 2) /* non-zero means a new result needs to be decoded */


struct cluster_inst_s;
typedef struct cluster_inst_s cluster_inst_t;

typedef struct {
    int			version;	/* client protocol version */
    int                 fd;             /* Socket descriptor, -1 => free */
    int			flags;
    char		*host;		/* client name */
    char		*metrics;	/* metrics to be monitored */
    cluster_inst_t	*instlist;	/* instance list for pmdaCache reaping */
    __pmPDU		*pb;
    pmResult		*result;	/* cached result, or NULL if not decoded */
    struct sockaddr_in  addr;           /* Address of client */
} cluster_client_t;

struct cluster_inst_s {
    int			node_inst;	/* instance on node */
    pmInDom		indom;
    int			inst;		/* instance on leader */
    cluster_inst_t	*next;    
    int			client; /* index into cluster_clients */
};

extern int		ncluster_mtab;
extern pmdaMetric	cluster_mtab[];		/* length: nmetrictab + num.control metrics */
extern pmID		subcluster_mtab[];	/* length: nmetrictab  */

extern int              n_cluster_clients;
extern cluster_client_t   *cluster_clients;


/*
 * Index this array with a subdomain number (0..14) to get the real domain
 * number of the correspomding sub-PMDA.  The subdomain numbers are encoded
 * into the high four bits of the PMID for cluster metrics when the cluster
 * namespace is generated by clusterns.pl.
 */

extern unsigned int subdom_dom_map[];
extern int num_subdom_dom_map;

/*
 * The PMDA names corresponding to entries in subdomain_domains may be handy
 * for debugging, etc.  Indexed by subdomain.
 */

extern const char* subdom_name_map[];
extern int num_subdom_name_map;

/*
 * Index using a __pmID_int.domain or __pmInDom_int.domain to find the
 * corresponding pmdacluster subdomain, or 0xff (no sub_PMDA available
 * for that domain).  Use a char (byte) to match dest bits and save
 * space (instead of int).  Don't squeeze to 2 x 4-bits per char and
 * shift/mask (extra code+time not worth space saved).
 */

extern unsigned char dom_subdom_map[];
extern int num_dom_subdom_map;



extern int cluster_monitoring_suspended(void);
extern void cluster_suspend_monitoring(int);

extern char *cluster_client_metrics_get(char* /*hostname*/);
extern int cluster_client_metrics_set(pmValue*, int /*delete*/);

/************
 * Subdomain management for sub-PMDAs
 *
 * PMDAs that operate with pmdacluster (sub-PMDAs) must have the most
 * significant 4 bits of __pmID_int.cluster set to zero for all metrics and
 * the most significant 4 bits of __pmInDom_int.seral set to zero for all
 * indoms.
 *
 * pmdacluster maintains its own version of the sub-PMDA's metrics, with the
 * __pmID_int.domain and __pmInDom_int.domain set to CLUSTER.  The two 4-bit
 * fields are used to distinguish up to 15 sub-PMDAs within pmdacluster.
 *
 * For an indom, a subdomain with all bits set (0xf) indicates either
 * PM_INDOM_NULL (serial has all bits set, 0x3fffff) or that the sub-PMDA for
 * this domain is using the 4 bits reserved for subdomains in a PMID (serial
 * has all bits zero).
 */

/*
 * A version of PCP's __pmInDom_int from impl.h with a 4-bit subdomain in the
 * most significant bits of the cluster field.
 */

#define FIELD_PAD_WIDTH		2
#define FIELD_DOMAIN_WIDTH	8
#define FIELD_SUBDOMAIN_WIDTH	4
#define FIELD_CLUSTER_WIDTH	8
#define FIELD_ITEM_WIDTH	10
#define FIELD_SERIAL_WIDTH	18

typedef struct {
#ifdef HAVE_BITFIELDS_LTOR
    int             pad		: FIELD_PAD_WIDTH;
    unsigned int    domain	: FIELD_DOMAIN_WIDTH;
    unsigned int    subdomain	: FIELD_SUBDOMAIN_WIDTH;
    unsigned int    cluster	: FIELD_CLUSTER_WIDTH;
    unsigned int    item	: FIELD_ITEM_WIDTH;
#else
    unsigned int    item	: FIELD_ITEM_WIDTH;
    unsigned int    cluster	: FIELD_CLUSTER_WIDTH;
    unsigned int    subdomain	: FIELD_SUBDOMAIN_WIDTH;
    unsigned int    domain	: FIELD_DOMAIN_WIDTH;
    int             pad		: FIELD_PAD_WIDTH;
#endif
} __pmID_int_subdomain;

static inline __pmID_int_subdomain *
__pmid_int_subdomain(pmID *idp)
{
    /* avoid gcc's warning about dereferencing type-punned pointers */
    return (__pmID_int_subdomain *)idp;
}

typedef struct {
#ifdef HAVE_BITFIELDS_LTOR
    int             pad		: FIELD_PAD_WIDTH;
    unsigned int    domain	: FIELD_DOMAIN_WIDTH;
    unsigned int    subdomain	: FIELD_SUBDOMAIN_WIDTH;
    unsigned int    serial	: FIELD_SERIAL_WIDTH;
#else
    unsigned int    serial	: FIELD_SERIAL_WIDTH;
    unsigned int    subdomain	: FIELD_SUBDOMAIN_WIDTH;
    unsigned int    domain	: FIELD_DOMAIN_WIDTH;
    int             pad		: FIELD_PAD_WIDTH;
#endif
} __pmInDom_int_subdomain;

static inline __pmInDom_int_subdomain *
__pmindom_int_subdomain(pmInDom *idp)
{
    /* avoid gcc's warning about dereferencing type-punned pointers */
    return (__pmInDom_int_subdomain *)idp;
}

/* Used to mark metrics and indoms as not working */

#define CLUSTER_BAD_CLUSTER	(0xf << FIELD_CLUSTER_WIDTH)
#define CLUSTER_BAD_INDOM	(0xf << FIELD_SERIAL_WIDTH)

/* Macro that can be used to create each metrics' PMID. */
#define CLUSTER_PMID(d,c,i) \
 ((( (d << (FIELD_SUBDOMAIN_WIDTH + FIELD_CLUSTER_WIDTH) \
      ) | c) << FIELD_ITEM_WIDTH) | i)


/************
 * UTILITIES
 */

extern size_t cluster_node_read(int fd, void *buf, size_t len);
extern size_t cluster_node_write(int fd, void *buf, size_t len);
static inline int cluster_node_read_ok(int fd, void *buf, size_t len)
{
    return (cluster_node_read(fd, buf, len) == len);
}

static inline int cluster_node_write_ok(int fd, void *buf, size_t len)
{
    return (cluster_node_write(fd, buf, len) == len);
}
