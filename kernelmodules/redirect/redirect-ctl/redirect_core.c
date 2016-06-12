#include <linux/init.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/if_vlan.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_bridge.h>
#include "redirect_core.h"
#include "redirect_control.h"
#define CLIENT
#define K_2_6_18
#ifdef K_2_6_18
#define CSUM_MANGLED_0 ((__force __u16)0xffff)
#endif
struct sk_buff* tcp_newpack( u32 saddr, u32 daddr,
        u16 sport, u16 dport,
        u32 seq, u32 ack_seq,
        u8 *msg, int len );

int _tcp_send_pack( struct sk_buff *skb, struct iphdr *iph,
        struct tcphdr *th, gbuffer_t *p );

#ifndef MAX_URL_LEN
#define MAX_URL_LEN  253
#endif

#define DEFAULT_REDIRECT_URL "www.126.com"

int http_build_redirect_url( const char *url, gbuffer_t *p );
int http_send_redirect(struct sk_buff *skb, struct iphdr *iph,
        struct tcphdr *th, const char *url);
int _http_send_redirect(struct sk_buff *skb, struct iphdr *iph,

        struct tcphdr *th );

int setup_redirect_url( const char *url );
void clear_redirect_url(void);
int redirect_url_init(void);
void redirect_url_fini(void);
char *get_redirect_url(void);

/*****************************************************************************/
/*  counter for current REDIRECT clients */
static atomic_t do_rediect = ATOMIC_INIT(1);

static char fqdn_redirect_url[MAX_URL_LEN + 1] = {0};

static gbuffer_t *url_redirect_data = NULL;

static gbuffer_t *url_redirect_default = NULL;

static spinlock_t url_redirect_lock;
struct tcphdr * tcp_hdr2(struct sk_buff *skb){
	struct iphdr *iph = skb->nh.iph;
	return (struct tcphdr *)&(((char *)iph)[iph->ihl*4]);
}
#if 0
struct iphdr * ip_hdr(struct sk_buff *skb){
	return skb->nh.iph;
}

struct tcphdr * tcp_hdr(struct sk_buff *skb){
	struct iphdr *iph = skb->nh.iph;
	return (struct tcphdr *)&(((char *)iph)[iph->ihl*4]);
}

static inline void skb_copy_to_linear_data(struct sk_buff *skb,
					   const void *from,
					   const unsigned int len)
{
	memcpy(skb->data, from, len);
}
#endif
static inline void skb_copy_to_linear_data2(struct sk_buff *skb,
					   const void *from,
					   const unsigned int len)
{
	memcpy(skb->data, from, len);
}
/*

 * ��ʼ��Ĭ���ض���DEFAULT_REDIRECT_URL HTML����

 */
int redirect_url_init(void)
{
    spin_lock_init( &url_redirect_lock );
    url_redirect_default = __gbuffer_alloc();

    if ( NULL == url_redirect_default ) {
        printk("__gbuffer_alloc for default redirect URL failed./n" );
        return -1;
    }

    if ( http_build_redirect_url( DEFAULT_REDIRECT_URL,
            url_redirect_default ) ){
        _gbuffer_free( url_redirect_default );
        url_redirect_default = NULL;

        printk("http_build_redirect_url %s failed.\n",
            DEFAULT_REDIRECT_URL );
        return -1;
    }

    return 0;
}

/*

 * �ͷ��ض�������

 */
void redirect_url_fini(void)
{
    gbuffer_t *p = NULL;
    _gbuffer_free( url_redirect_default );

    url_redirect_default = NULL;
    p = url_redirect_data;
    rcu_assign_pointer( url_redirect_data, NULL );

    _gbuffer_free( p );
}

int get_redirect_tag()
{
	int tag = atomic_read(&do_rediect);
	return tag;
}

void set_redirect_tag(int tag)
{
	atomic_set(&do_rediect, tag);
}

/*

 * �����ض���URL, �����ض�������

 */
int setup_redirect_url( const char *url )
{
    int len;
    gbuffer_t *p = NULL, *ptr;

    if ( NULL == url )
        return -1;

    len = strlen(url);
    if ( len > MAX_URL_LEN )
        return -1;

    memset( fqdn_redirect_url, 0x0, MAX_URL_LEN );
    memcpy( fqdn_redirect_url, url, len );
    p = __gbuffer_alloc();

    if ( NULL == p ) {
        printk("__gbuffer_alloc failed.\n" );
        return -1;
    }

    if ( http_build_redirect_url( fqdn_redirect_url,
            p ) ) {
        printk("http_build_redirect_url %s failed.\n",
            fqdn_redirect_url );
        _gbuffer_free( p );
        return -1;
    }

    printk(KERN_INFO "Setup Redirect URL http://%s\n", fqdn_redirect_url );

    spin_lock_bh( &url_redirect_lock );
    ptr = url_redirect_data;
    rcu_assign_pointer( url_redirect_data, p );
    spin_unlock_bh( &url_redirect_lock );

    synchronize_rcu();
    _gbuffer_free( ptr );

    return 0;

}

/*

 * ����ض�������

 */
void clear_redirect_url(void)
{
    gbuffer_t *ptr;
    memset( fqdn_redirect_url, 0x0, MAX_URL_LEN );

    spin_lock_bh( &url_redirect_lock );
    ptr = url_redirect_data;
    rcu_assign_pointer( url_redirect_data, NULL );
    spin_unlock_bh( &url_redirect_lock );
    synchronize_rcu();
    _gbuffer_free( ptr );
}

/*

 * ��ȡ�ض������ݻ���

 */
char *get_redirect_url(void)
{
    if ( 0 == *fqdn_redirect_url )
        return DEFAULT_REDIRECT_URL;

    return fqdn_redirect_url;
}

/*

 * �ض���HTML�ļ��ָ�ʽ

 */
const char *http_redirect_header =
    "HTTP/1.1 301 Moved Permanently\r\n"
    "Location: http://%s\r\n"
    "Content-Type: text/html; charset=iso-8859-1\r\n"
    "Content-length: 0\r\n"
    "Cache-control: no-cache\r\n"
    "\r\n";

/*

 * ����һ���ض���HTML����

 */
int http_build_redirect_url( const char *url, gbuffer_t *p )
{
    char *header = NULL;
    char *body  = NULL;
    char *buf   = NULL;
    int header_len;
    int rc = -1;

    if ( NULL == p )
        goto _out;

    header = kzalloc( PATH_MAX, GFP_KERNEL );
    if ( NULL == header ) {
        goto _out;
    }

    header_len = snprintf( header, PATH_MAX,
                    http_redirect_header,
                    url
                    );

    buf = kzalloc( header_len , GFP_KERNEL );
    if ( NULL == buf ){
        goto _out;
    }

    p->buf = buf;
    p->len = header_len ;
    memcpy( buf, header, header_len );
#if 0
    {

        int i = 0;

        for( ; i < p->len; i ++ ){

            printk( "%c", buf[i] );

        }

        printk( "\n" );

    }

#endif

    rc = 0;

_out:
    if ( header ){
        kfree( header );
    }

    if ( body ) {
        kfree( body );
    }

    return rc;
}

int skb_iphdr_init( struct sk_buff *skb, u8 protocol,

                    u32 saddr, u32 daddr, int ip_len )
{
    struct iphdr *iph = NULL;
    // skb->data �ƶ���ip�ײ�

    skb_push( skb, sizeof(struct iphdr) );
    //skb_reset_network_header( skb );

    iph = skb->data;

    /* iph->version = 4; iph->ihl = 5; */
#if 0

    put_unaligned( 0x45, ( unsigned char * )iph );

    iph->tos      = 0;

    put_unaligned( htons( ip_len ), &( iph->tot_len ) );

    iph->id       = 0;

    iph->frag_off = htons(IP_DF);

    iph->ttl      = 64;

    iph->protocol = IPPROTO_UDP;

    iph->check    = 0;

    put_unaligned( saddr, &( iph->saddr ) );

    put_unaligned( daddr, &( iph->daddr ) );

    iph->check    = ip_fast_csum( ( unsigned char * )iph, iph->ihl );

#else

    iph->version  = 4;
    iph->ihl      = 5;
    iph->tos      = 0;
    iph->tot_len  = htons( ip_len );
    iph->id       = 0;
    iph->frag_off = htons(IP_DF);
    iph->ttl      = 64;
    iph->protocol = protocol;
    iph->check    = 0;
    iph->saddr    = saddr;
    iph->daddr    = daddr;
    iph->check    = ip_fast_csum( ( unsigned char * )iph, iph->ihl );

#endif

    return 0;
}

/*

 * ����һ��tcp���ݰ�

 */
struct sk_buff* tcp_newpack( u32 saddr, u32 daddr,
        u16 sport, u16 dport,
        u32 seq, u32 ack_seq,
        u8 *msg, int len )
{
    struct sk_buff *skb = NULL;
    int total_len, eth_len, ip_len, header_len;
    int tcp_len;
    struct tcphdr *th;
    struct iphdr *iph;
    unsigned int tcp_hdr_csum;

    // ���ø���Э�����ݳ���
    tcp_len = len + sizeof( *th );
    ip_len = tcp_len + sizeof( *iph );
    eth_len = ip_len + ETH_HLEN;
    //
    total_len = eth_len + NET_IP_ALIGN;
    total_len += LL_MAX_HEADER;
    header_len = total_len - len;

    // ����skb
    skb = alloc_skb( total_len, GFP_ATOMIC );


    if ( !skb ) {
        printk("alloc_skb length %d failed./n", total_len );
        return NULL;
    }


    // Ԥ�ȱ���skb��Э���ײ����ȴ�С
    skb_reserve( skb, header_len );


    // ������������
    skb_copy_to_linear_data2( skb, msg, len );
    skb->len += len;

    // skb->data �ƶ���tdp�ײ�
    skb_push( skb, sizeof( *th ) );

    //skb_reset_transport_header( skb );
    th = ( struct tcphdr *)skb->data;

    memset( th, 0x0, sizeof( *th ) );

    th->doff    = 5;
    th->source  = sport;
    th->dest    = dport;
    th->seq     = seq;
    th->ack_seq = ack_seq;
    th->urg_ptr = 0;
    th->psh = 0x1;
    th->ack = 0x1;
    th->window = htons( 63857 );
    th->check    = 0;

    tcp_hdr_csum = csum_partial((unsigned char *)th, tcp_len, 0 );
    th->check = csum_tcpudp_magic( saddr,
            daddr,
            tcp_len, IPPROTO_TCP,
            tcp_hdr_csum );
    skb->csum=tcp_hdr_csum;

    if ( th->check == 0 )
        th->check = CSUM_MANGLED_0;

    skb_iphdr_init( skb, IPPROTO_TCP, saddr, daddr, ip_len );

    return skb;
}

/*

 * ������Դip,tcp�˿ڷ���tcp����

 */
int _tcp_send_pack( struct sk_buff *skb, struct iphdr *iph,
        struct tcphdr *th, gbuffer_t *p )
{
    struct sk_buff *pskb = NULL;
    struct ethhdr *eth = NULL;
    struct vlan_hdr *vhdr = NULL;
    int tcp_len = 0;
    u32 ack_seq = 0;
    int rc = -1;

    // ���¼��� Acknowledgement number
    tcp_len = ntohs(iph->tot_len) - ((iph->ihl + th->doff) << 2);
    ack_seq = ntohl(th->seq) + (tcp_len);
    ack_seq = htonl(ack_seq);

    pskb = tcp_newpack( iph->daddr, iph->saddr,
                th->dest, th->source,
                th->ack_seq, ack_seq,
                p->buf, p->len );

    if ( NULL == pskb ) {
        goto _out;
    }

    // ����VLAN ��Ϣ
    if ( __constant_htons(ETH_P_8021Q) == skb->protocol ) {
        vhdr = (struct vlan_hdr *)skb_push(pskb, VLAN_HLEN );
        vhdr->h_vlan_TCI = vlan_eth_hdr(skb)->h_vlan_TCI;
        vhdr->h_vlan_encapsulated_proto = __constant_htons(ETH_P_IP);
    }

    // skb->data �ƶ���eth�ײ�
    eth = (struct ethhdr *) skb_push(pskb, ETH_HLEN);
    //skb_reset_mac_header(pskb);

    pskb->protocol  = eth_hdr(skb)->h_proto;
    eth->h_proto    = eth_hdr(skb)->h_proto;
    memcpy( eth->h_source, eth_hdr(skb)->h_dest, ETH_ALEN);
    memcpy( eth->h_dest, eth_hdr(skb)->h_source, ETH_ALEN );

    if ( skb->dev ) {
        pskb->dev = skb->dev;
        dev_queue_xmit( pskb );
        rc = 0;
    }

    else {
        kfree_skb( pskb );
        printk( "skb->dev is NULL/n" );
    }

_out:
    return rc;

}

/*

 * ������Դip,tcp�˿ڷ����ض���HTML����

 */
int _http_send_redirect(struct sk_buff *skb, struct iphdr *iph,
        struct tcphdr *th )
{
    int rc = -1;
    gbuffer_t *p = NULL;

    rcu_read_lock();

    p = rcu_dereference( url_redirect_data );
    if ( NULL == p ) {
        p = url_redirect_default;
		//printk("1111\n");
    }

    if ( NULL != p && NULL != p->buf ) {
        rc = _tcp_send_pack(skb, iph, th, p );
       // printk("2222\n");
    }
    rcu_read_unlock();

    return rc;
}

int get_user_agent(char *payload, int payload_len, char **ret)
{
	char buf[] = "User-Agent:";
	int i, j, buf_len=strlen(buf);
	*ret = NULL;
	for (i=0; i<payload_len; i++)
	{
		for (j=0; j<buf_len; j++)
		{
			if (buf[j] != payload[i+j])
				break;
		}

		if (j==buf_len)
		{
			*ret = payload+i+buf_len;
			break;
		}
	}

	if (*ret == NULL)
		return 0;

	for (j=0; j<(payload_len-i-buf_len); j++)
	{
		if ( (*ret)[j] == '(' || (*ret)[j] == '\r')
		{
			return j;
		}
	}
	return 0;
}

static unsigned int direct_fun(unsigned int hook,
                           struct sk_buff **skbp,
                           const struct net_device *in,
                           const struct net_device *out,
                           int (*okfn)(struct sk_buff *)
                          )
{
   struct sk_buff *skb = *skbp;
    struct iphdr *iph = skb->nh.iph;
    struct ethhdr *eth = eth_hdr(skb);
    struct tcphdr *tcph = NULL;
    struct udphdr *udph=NULL;
    unsigned int sip, dip;
    unsigned short source, dest;
    unsigned char *payload;
    int plen;
#ifdef CLIENT
    struct redirect_client *rc;
#endif
//printk("Client---------enter\n" );

	if (atomic_read(&do_rediect) == 0) {
		 return NF_ACCEPT;
	}

    if(!skb)
        return NF_ACCEPT;

    if(!eth){
        return NF_ACCEPT;
    }

    if(!iph){
        return NF_ACCEPT;
    }
//printk("Client-------3\n" );
    if(skb->pkt_type == PACKET_BROADCAST)
        return NF_ACCEPT;

    if((skb->protocol==htons(ETH_P_8021Q)||skb->protocol==htons(ETH_P_IP))&&skb->len>=sizeof(struct ethhdr)){
        if(skb->protocol==htons(ETH_P_8021Q))
        {
            iph=(struct iphdr *)((u8*)iph+4);
        }

        if(iph->version!=4)
            return NF_ACCEPT;

        if (skb->len < 20)
            return NF_ACCEPT;

        if ((iph->ihl * 4) > skb->len || skb->len < ntohs(iph->tot_len) || (iph->frag_off & htons(0x1FFF)) != 0)
            return NF_ACCEPT;

        sip = iph->saddr;
        dip = iph->daddr;

        if(iph->protocol == 6){
            tcph = (struct tcphdr *)((unsigned char *)iph+iph->ihl*4);
            source = ntohs(tcph->source);
            dest = ntohs(tcph->dest);
            if(dest == 53 || source == 53){  // dns
                return NF_ACCEPT;
            }

            plen = ntohs(iph->tot_len) - iph->ihl*4 - tcph->doff*4;

            //http
            if(dest == 80){
				/*
	printk("ehdr[h_dst(%02x:%02x:%02x:%02x:%02x:%02x)"
	       "h_source(%02x:%02x:%02x:%02x:%02x:%02x)h_proto(%04x)]\n",
	       eth->h_dest[0], eth->h_dest[1], eth->h_dest[2],
	       eth->h_dest[3], eth->h_dest[4], eth->h_dest[4],
	       eth->h_source[0], eth->h_source[1], eth->h_source[2],
	       eth->h_source[3], eth->h_source[4], eth->h_source[4],
	       eth->h_proto);*/
                payload = (unsigned char *)tcph + tcph->doff*4;
				if(plen > 10 && payload[0] == 'G' && payload[1] == 'E' && payload[2] == 'T' && payload[3] == ' '){
					char *p;
					int len = get_user_agent(payload, plen, &p);
									//printk("----GET\n");
					if (len) {
					rc = redirect_clt_get(eth->h_source, p, len);
					if (unlikely(rc != NULL)) {
						//printk("Client:%u.%u.%u.%u exist\n", NIPQUAD(iph->saddr));
						redirect_clt_put(rc);
						return NF_ACCEPT;
					}
					rc = redirect_clt_new(eth->h_source, p, len);
					if (unlikely(!rc)) {
						return NF_ACCEPT;
					}
					printk("Client:%u.%u.%u.%u [ag=%s]redirect\n", NIPQUAD(iph->saddr), rc->user_agent);
					_http_send_redirect(skb,iph,tcph);
				}
			}
           	}
        }
    }

    return NF_ACCEPT;
}

static struct nf_hook_ops auth_ops =
{
    .hook = direct_fun,
    .pf = PF_INET,
#ifdef K_2_6_18
    .hooknum = NF_IP_PRE_ROUTING,
#else
    .hooknum = NF_INET_PRE_ROUTING,
#endif
    .priority = NF_IP_PRI_FIRST,
};

static int __init auth_init(void)
{
#ifdef CLIENT
	/*��ʼ������*/
	int ret = redirect_clt_init();
	if (ret < 0) {
		printk("can't setup client table.\n");
		return -1;
	}
	#endif
    redirect_url_init();
    nf_register_hook(&auth_ops);

	redirect_ctl_module_init();
    return 0;
}

static void __exit auth_eixt(void)
{
    nf_unregister_hook(&auth_ops);
    redirect_url_fini();
	#ifdef CLIENT
	redirect_clt_exit();
	#endif

	redirect_ctl_module_exit();
}

MODULE_LICENSE("GPL");
module_init(auth_init);
module_exit(auth_eixt);

