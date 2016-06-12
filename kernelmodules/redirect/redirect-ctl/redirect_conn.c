#include <linux/init.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_bridge.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/net.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>		/* for proc_net_* */
#include <linux/seq_file.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/slab.h>
#include "redirect_core.h"

/*
 *  Client hash table: for input and output packets lookups of REDIRECT
 */
static struct list_head *redirect_clt_tab;

/*  SLAB cache for REDIRECT clients */
static kmem_cache_t *redirect_clt_cachep __read_mostly;

/*  counter for current REDIRECT clients */
static atomic_t redirect_clt_count = ATOMIC_INIT(0);

/* random value for REDIRECT client hash */
static unsigned int redirect_clt_rnd;

/*  counter for  REDIRECT times */
static atomic_t redirect_times = ATOMIC_INIT(24*6);

/*
 *  Fine locking granularity for big clients hash table
 */
#define CT_LOCKARRAY_BITS  2
#define CT_LOCKARRAY_SIZE  (1<<CT_LOCKARRAY_BITS)///4
#define CT_LOCKARRAY_MASK  (CT_LOCKARRAY_SIZE-1)///3

struct redirect_aligned_lock
{
	rwlock_t	l;
} __attribute__((__aligned__(SMP_CACHE_BYTES)));

/* lock array for clt table */
//���ӱ�������
static struct redirect_aligned_lock
__redirect_clttbl_lock_array[CT_LOCKARRAY_SIZE] __cacheline_aligned;

//��дʱ�����ӵ�HASHֵ��������ȡģ���͵õ���Ӧ��
static inline void clt_read_lock(int key)
{
	read_lock(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

static inline void clt_read_unlock(int key)
{
	read_unlock(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

static inline void clt_write_lock(int key)
{
	write_lock(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

static inline void clt_write_unlock(int key)
{
	write_unlock(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

static inline void clt_read_lock_bh(int key)
{
	read_lock_bh(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

static inline void clt_read_unlock_bh(int key)
{
	read_unlock_bh(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

static inline void clt_write_lock_bh(int key)
{
	write_lock_bh(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

static inline void clt_write_unlock_bh(int key)
{
	write_unlock_bh(&__redirect_clttbl_lock_array[key&CT_LOCKARRAY_MASK].l);
}

int get_redirect_user_count()
{
	int count = atomic_read(&redirect_clt_count);
	return count;
}

void get_redirect_user(struct redirect_user *ru, int *len)
{
	int idx, i=0;
	struct redirect_client *rc;

  	//ѭ����������HASH��
	for (idx=0; idx<REDIRECT_CLT_TAB_SIZE; idx++) {
		/*
		 *  Lock is actually needed in this loop.
		 */
		clt_read_lock_bh(idx);
		//ѭ������
		list_for_each_entry(rc, &redirect_clt_tab[idx], c_list) {
			//��ȡ�û���Ϣ
			memcpy(ru[i].mac, rc->mac, sizeof(rc->mac));
			memcpy(ru[i].user_agent, rc->user_agent, rc->ag_len);
			i++;
			if (i == *len){
				clt_read_unlock_bh(idx);
				goto out;
			}
				
		}
		clt_read_unlock_bh(idx);
	}
out:
		*len = i;
}

int get_redirect_times()
{
	int tag = atomic_read(&redirect_times);
	return tag;
}

void set_redirect_times(int times)
{
	if (times<=0 || times > 24*60)
		times = 24*60;
	atomic_set(&redirect_times, times);
}


/*
 *	Returns hash value for REDIRECT client entry
 */
 static __inline__ int redirect_clt_hashkey(const unsigned char *mac)
{
	return jhash(mac, ETH_ALEN, redirect_clt_rnd)&REDIRECT_CLT_TAB_MASK;
}

/*
���ͻ��˽ṹ��ӵ�����HASH��
 */
static inline int redirect_clt_hash(struct redirect_client *rc)
{
	int hash;
	int ret;

	/* Hash by mac */
	hash = redirect_clt_hashkey(rc->mac);

	clt_write_lock(hash);

	if (!(rc->flags & REDIRECT_CLT_F_HASHED)) {
		list_add(&rc->c_list, &redirect_clt_tab[hash]);
		//����HASH��־
		rc->flags |= REDIRECT_CLT_F_HASHED;
		//�ٴ��������ü���
		atomic_inc(&rc->refcnt);
		ret = 1;
	} else {
		printk("redirect_clt_hash(): request for already hashed, "
			  "called from %p\n", __builtin_return_address(0));
		ret = 0;
	}

	clt_write_unlock(hash);

	return ret;
}

/*
�ӿͻ���HASH���жϿ�
 */
static inline int redirect_clt_unhash(struct redirect_client *rc)
{
	int hash;
	int ret;

	hash = redirect_clt_hashkey(rc->mac);

	clt_write_lock(hash);

	if (rc->flags & REDIRECT_CLT_F_HASHED) {
		//��������ɾ��
		list_del(&rc->c_list);
		rc->flags &= ~REDIRECT_CLT_F_HASHED;
		atomic_dec(&rc->refcnt);//�����������ü���
		ret = 1;
	} else
		ret = 0;

	clt_write_unlock(hash);

	return ret;
}

struct redirect_client *redirect_clt_get(unsigned char *mac, char *ag, int len)
{
	int hash;
	struct redirect_client *rc;

	hash = redirect_clt_hashkey(mac);

	clt_read_lock(hash);

	list_for_each_entry(rc, &redirect_clt_tab[hash], c_list) {
		len = len < rc->ag_len?len:rc->ag_len;
		if (0==memcmp(mac, rc->mac, ETH_ALEN)&&
			0==memcmp(ag, rc->user_agent, len)) {
			/* HIT */
			atomic_inc(&rc->refcnt);
			clt_read_unlock(hash);
			return rc;
		}
	}

	clt_read_unlock(hash);

	return NULL;
}

/*
 *      Put back the client
 */
void redirect_clt_put(struct redirect_client *rc)
{
	atomic_dec(&rc->refcnt);
}

//���ӳ�ʱ����
static void redirect_clt_expire(unsigned long data)
{
	struct redirect_client *rc = (struct redirect_client *)data;

	/*
	 *	unhash it if it is hashed in the conn table
	 */
	 //�����ӽṹ������HASH���жϿ�
	if (!redirect_clt_unhash(rc))
		goto expire_later;

	/*
	 *	refcnt==1 implies I'm the only one referrer
	����Ϊ1��ʾ����ɾ��������
	 	*/
	if (likely(atomic_read(&rc->refcnt) == 0)) {
		/* delete the timer if it is activated by other users */
		//ɾ��ʱ�ӣ�����Ƕ�ʱ�����ڵĻ���
		//��ʱ�����Ѿ�ɾ���˵�
		if (timer_pending(&rc->timer))
			del_timer(&rc->timer);

		//���ٿͻ�������
		atomic_dec(&redirect_clt_count);

		//�ͷ����ӵ�cache�ڴ�
		kmem_cache_free(redirect_clt_cachep, rc);
		printk("die... \n");
		return;
	}

	/* hash it back to the table */
	//������ɾ�������°ѽṹ�һ�����HASH��
	redirect_clt_hash(rc);

  expire_later:
	//�޸Ķ�ʱ
	mod_timer(&rc->timer, jiffies+10*HZ);
}

//�ͻ��˶�ʱ����������
static void redirect_clt_expire_now(struct redirect_client *rc)
{
	if (del_timer(&rc->timer))
		mod_timer(&rc->timer, jiffies);
}

/*
 *	Create a new client entry and hash it into the redirect_clt_tab
�ͻ��Ľ���
 */
struct redirect_client *redirect_clt_new(unsigned char *mac, char *ag, int len)
{
	struct redirect_client *rc;

	/*����conn���ڴ�*/
	rc = kmem_cache_alloc(redirect_clt_cachep, GFP_ATOMIC);
	if (rc == NULL) {
		printk("redirect_clt_new: no memory available.\n");
		return NULL;
	}

	memset(rc, 0, sizeof(*rc));
	INIT_LIST_HEAD(&rc->c_list);
	init_timer(&rc->timer);
	rc->timer.data     = (unsigned long)rc;
	rc->timer.function = redirect_clt_expire;//���ӳ�ʱ����
	rc->timeout = 24*60*60*HZ/atomic_read(&redirect_times);
	/*���ӻ���������ֵ*/
	memcpy(rc->mac, mac, ETH_ALEN);
	rc->ag_len = (len<sizeof(rc->user_agent)?len:(sizeof(rc->user_agent)-1));
	memcpy(rc->user_agent, ag, rc->ag_len);
	/*
	 * Set the entry is referenced by the current thread before hashing
	 * it in the table, so that other thread run ip_vs_random_dropentry
	 * but cannot drop this entry.
	 */
	atomic_set(&rc->refcnt, 0);//���ó�ʼֵΪ0
	atomic_inc(&redirect_clt_count);//���Ӽ���

	/* Hash it in the ip_vs_conn_tab finally */
	//�������ӽڵ���뵽REDIRECT���ӱ���
	redirect_clt_hash(rc);

	//������ʱ��
	mod_timer(&rc->timer, jiffies + rc->timeout);
	return rc;
}

/*
�ͷ����пͻ�����ɾ��REDIRECTģ��ʱ���ã�������
�����пͻ���ʱ�����ڶ��Զ����ö�ʱ���ں���
 */
static void redirect_clt_flush(void)
{
	int idx;
	struct redirect_client *rc;

  flush_again:
  	//ѭ����������HASH��
	for (idx=0; idx<REDIRECT_CLT_TAB_SIZE; idx++) {
		/*
		 *  Lock is actually needed in this loop.
		 */
		clt_write_lock_bh(idx);
		//ѭ������
		list_for_each_entry(rc, &redirect_clt_tab[idx], c_list) {
			//��ʱ����������
			redirect_clt_expire_now(rc);
		}
		clt_write_unlock_bh(idx);
	}

	if (atomic_read(&redirect_clt_count) != 0) {
		//����ͻ�����Ϊ0�����µ��Ƚ��̣������ͷ�
		schedule();
		goto flush_again;
	}
}


int redirect_clt_init(void)
{
	int idx;

	 /*Լ8192���ͻ���*/
	redirect_clt_tab = vmalloc(REDIRECT_CLT_TAB_SIZE*sizeof(struct list_head));
	if (!redirect_clt_tab)
		return -ENOMEM;

	/* Allocate ip_vs_conn slab cache */
	/*����ר�ö��󻺳���*/
	redirect_clt_cachep = kmem_cache_create("redirect_client",
					      sizeof(struct redirect_client), 0,
					      SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!redirect_clt_cachep) {
		vfree(redirect_clt_tab);
		return -ENOMEM;
	}

	/*ÿ��������Ҫ120�ֽ�*/
	printk("Each Client entry needs %Zd bytes at least\n",
		  sizeof(struct redirect_client));

	for (idx = 0; idx < REDIRECT_CLT_TAB_SIZE; idx++) {
		INIT_LIST_HEAD(&redirect_clt_tab[idx]);
	}

	for (idx = 0; idx < CT_LOCKARRAY_SIZE; idx++)  {
		rwlock_init(&__redirect_clttbl_lock_array[idx].l);
	}

	/* calculate the random value for connection hash */
	get_random_bytes(&redirect_clt_rnd, sizeof(redirect_clt_rnd));

	return 0;
}


void redirect_clt_exit(void)
{
	/* flush all the client entries first */
	redirect_clt_flush();

	/* Release the empty cache */
	kmem_cache_destroy(redirect_clt_cachep);
	vfree(redirect_clt_tab);
}


