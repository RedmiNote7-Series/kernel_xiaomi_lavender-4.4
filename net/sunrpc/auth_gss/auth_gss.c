/*
 * linux/net/sunrpc/auth_gss/auth_gss.c
 *
 * RPCSEC_GSS client authentication.
 *
 *  Copyright (c) 2000 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Dug Song       <dugsong@monkey.org>
 *  Andy Adamson   <andros@umich.edu>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/pagemap.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/auth.h>
#include <linux/sunrpc/auth_gss.h>
#include <linux/sunrpc/svcauth_gss.h>
#include <linux/sunrpc/gss_err.h>
#include <linux/workqueue.h>
#include <linux/sunrpc/rpc_pipe_fs.h>
#include <linux/sunrpc/gss_api.h>
#include <asm/uaccess.h>
#include <linux/hashtable.h>

#include "auth_gss_internal.h"
#include "../netns.h"

static const struct rpc_authops authgss_ops;

static const struct rpc_credops gss_credops;
static const struct rpc_credops gss_nullops;

#define GSS_RETRY_EXPIRED 5
static unsigned int gss_expired_cred_retry_delay = GSS_RETRY_EXPIRED;

#define GSS_KEY_EXPIRE_TIMEO 240
static unsigned int gss_key_expire_timeo = GSS_KEY_EXPIRE_TIMEO;

#if IS_ENABLED(CONFIG_SUNRPC_DEBUG)
# define RPCDBG_FACILITY	RPCDBG_AUTH
#endif

#define GSS_CRED_SLACK		(RPC_MAX_AUTH_SIZE * 2)
/* length of a krb5 verifier (48), plus data added before arguments when
 * using integrity (two 4-byte integers): */
#define GSS_VERF_SLACK		100

static DEFINE_HASHTABLE(gss_auth_hash_table, 4);
static DEFINE_SPINLOCK(gss_auth_hash_lock);

struct gss_pipe {
	struct rpc_pipe_dir_object pdo;
	struct rpc_pipe *pipe;
	struct rpc_clnt *clnt;
	const char *name;
	struct kref kref;
};

struct gss_auth {
	struct kref kref;
	struct hlist_node hash;
	struct rpc_auth rpc_auth;
	struct gss_api_mech *mech;
	enum rpc_gss_svc service;
	struct rpc_clnt *client;
	struct net *net;
	/*
	 * There are two upcall pipes; dentry[1], named "gssd", is used
	 * for the new text-based upcall; dentry[0] is named after the
	 * mechanism (for example, "krb5") and exists for
	 * backwards-compatibility with older gssd's.
	 */
	struct gss_pipe *gss_pipe[2];
	const char *target_name;
};

/* pipe_version >= 0 if and only if someone has a pipe open. */
static DEFINE_SPINLOCK(pipe_version_lock);
static struct rpc_wait_queue pipe_version_rpc_waitqueue;
static DECLARE_WAIT_QUEUE_HEAD(pipe_version_waitqueue);
static void gss_put_auth(struct gss_auth *gss_auth);

static void gss_free_ctx(struct gss_cl_ctx *);
static const struct rpc_pipe_ops gss_upcall_ops_v0;
static const struct rpc_pipe_ops gss_upcall_ops_v1;

static inline struct gss_cl_ctx *
gss_get_ctx(struct gss_cl_ctx *ctx)
{
	atomic_inc(&ctx->count);
	return ctx;
}

static inline void
gss_put_ctx(struct gss_cl_ctx *ctx)
{
	if (atomic_dec_and_test(&ctx->count))
		gss_free_ctx(ctx);
}

/* gss_cred_set_ctx:
 * called by gss_upcall_callback and gss_create_upcall in order
 * to set the gss context. The actual exchange of an old context
 * and a new one is protected by the pipe->lock.
 */
static void
gss_cred_set_ctx(struct rpc_cred *cred, struct gss_cl_ctx *ctx)
{
	struct gss_cred *gss_cred = container_of(cred, struct gss_cred, gc_base);

	if (!test_bit(RPCAUTH_CRED_NEW, &cred->cr_flags))
		return;
	gss_get_ctx(ctx);
	rcu_assign_pointer(gss_cred->gc_ctx, ctx);
	set_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags);
	smp_mb__before_atomic();
	clear_bit(RPCAUTH_CRED_NEW, &cred->cr_flags);
}

static struct gss_cl_ctx *
gss_cred_get_ctx(struct rpc_cred *cred)
{
	struct gss_cred *gss_cred = container_of(cred, struct gss_cred, gc_base);
	struct gss_cl_ctx *ctx = NULL;

	rcu_read_lock();
	ctx = rcu_dereference(gss_cred->gc_ctx);
	if (ctx)
		gss_get_ctx(ctx);
	rcu_read_unlock();
	return ctx;
}

static struct gss_cl_ctx *
gss_alloc_context(void)
{
	struct gss_cl_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
	if (ctx != NULL) {
		ctx->gc_proc = RPC_GSS_PROC_DATA;
		ctx->gc_seq = 1;	/* NetApp 6.4R1 doesn't accept seq. no. 0 */
		spin_lock_init(&ctx->gc_seq_lock);
		atomic_set(&ctx->count,1);
	}
	return ctx;
}

#define GSSD_MIN_TIMEOUT (60 * 60)
static const void *
gss_fill_context(const void *p, const void *end, struct gss_cl_ctx *ctx, struct gss_api_mech *gm)
{
	const void *q;
	unsigned int seclen;
	unsigned int timeout;
	unsigned long now = jiffies;
	u32 window_size;
	int ret;

	/* First unsigned int gives the remaining lifetime in seconds of the
	 * credential - e.g. the remaining TGT lifetime for Kerberos or
	 * the -t value passed to GSSD.
	 */
	p = simple_get_bytes(p, end, &timeout, sizeof(timeout));
	if (IS_ERR(p))
		goto err;
	if (timeout == 0)
		timeout = GSSD_MIN_TIMEOUT;
	ctx->gc_expiry = now + ((unsigned long)timeout * HZ);
	/* Sequence number window. Determines the maximum number of
	 * simultaneous requests
	 */
	p = simple_get_bytes(p, end, &window_size, sizeof(window_size));
	if (IS_ERR(p))
		goto err;
	ctx->gc_win = window_size;
	/* gssd signals an error by passing ctx->gc_win = 0: */
	if (ctx->gc_win == 0) {
		/*
		 * in which case, p points to an error code. Anything other
		 * than -EKEYEXPIRED gets converted to -EACCES.
		 */
		p = simple_get_bytes(p, end, &ret, sizeof(ret));
		if (!IS_ERR(p))
			p = (ret == -EKEYEXPIRED) ? ERR_PTR(-EKEYEXPIRED) :
						    ERR_PTR(-EACCES);
		goto err;
	}
	/* copy the opaque wire context */
	p = simple_get_netobj(p, end, &ctx->gc_wire_ctx);
	if (IS_ERR(p))
		goto err;
	/* import the opaque security context */
	p  = simple_get_bytes(p, end, &seclen, sizeof(seclen));
	if (IS_ERR(p))
		goto err;
	q = (const void *)((const char *)p + seclen);
	if (unlikely(q > end || q < p)) {
		p = ERR_PTR(-EFAULT);
		goto err;
	}
	ret = gss_import_sec_context(p, seclen, gm, &ctx->gc_gss_ctx, NULL, GFP_NOFS);
	if (ret < 0) {
		p = ERR_PTR(ret);
		goto err;
	}

	/* is there any trailing data? */
	if (q == end) {
		p = q;
		goto done;
	}

	/* pull in acceptor name (if there is one) */
	p = simple_get_netobj(q, end, &ctx->gc_acceptor);
	if (IS_ERR(p))
		goto err;
done:
	dprintk("RPC:       %s Success. gc_expiry %lu now %lu timeout %u acceptor %.*s\n",
		__func__, ctx->gc_expiry, now, timeout, ctx->gc_acceptor.len,
		ctx->gc_acceptor.data);
	return p;
err:
	dprintk("RPC:       %s returns error %ld\n", __func__, -PTR_ERR(p));
	return p;
}

#define UPCALL_BUF_LEN 128

struct gss_upcall_msg {
	atomic_t count;
	kuid_t	uid;
	struct rpc_pipe_msg msg;
	struct list_head list;
	struct gss_auth *auth;
	struct rpc_pipe *pipe;
	struct rpc_wait_queue rpc_waitqueue;
	wait_queue_head_t waitqueue;
	struct gss_cl_ctx *ctx;
	char databuf[UPCALL_BUF_LEN];
};

static int get_pipe_version(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	int ret;

	spin_lock(&pipe_version_lock);
	if (sn->pipe_version >= 0) {
		atomic_inc(&sn->pipe_users);
		ret = sn->pipe_version;
	} else
		ret = -EAGAIN;
	spin_unlock(&pipe_version_lock);
	return ret;
}

static void put_pipe_version(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);

	if (atomic_dec_and_lock(&sn->pipe_users, &pipe_version_lock)) {
		sn->pipe_version = -1;
		spin_unlock(&pipe_version_lock);
	}
}

static void
gss_release_msg(struct gss_upcall_msg *gss_msg)
{
	struct net *net = gss_msg->auth->net;
	if (!atomic_dec_and_test(&gss_msg->count))
		return;
	put_pipe_version(net);
	BUG_ON(!list_empty(&gss_msg->list));
	if (gss_msg->ctx != NULL)
		gss_put_ctx(gss_msg->ctx);
	rpc_destroy_wait_queue(&gss_msg->rpc_waitqueue);
	gss_put_auth(gss_msg->auth);
	kfree(gss_msg);
}

static struct gss_upcall_msg *
__gss_find_upcall(struct rpc_pipe *pipe, kuid_t uid, const struct gss_auth *auth)
{
	struct gss_upcall_msg *pos;
	list_for_each_entry(pos, &pipe->in_downcall, list) {
		if (!uid_eq(pos->uid, uid))
			continue;
		if (pos->auth->service != auth->service)
			continue;
		atomic_inc(&pos->count);
		dprintk("RPC:       %s found msg %p\n", __func__, pos);
		return pos;
	}
	dprintk("RPC:       %s found nothing\n", __func__);
	return NULL;
}

/* Try to add an upcall to the pipefs queue.
 * If an upcall owned by our uid already exists, then we return a reference
 * to that upcall instead of adding the new upcall.
 */
static inline struct gss_upcall_msg *
gss_add_msg(struct gss_upcall_msg *gss_msg)
{
	struct rpc_pipe *pipe = gss_msg->pipe;
	struct gss_upcall_msg *old;

	spin_lock(&pipe->lock);
	old = __gss_find_upcall(pipe, gss_msg->uid, gss_msg->auth);
	if (old == NULL) {
		atomic_inc(&gss_msg->count);
		list_add(&gss_msg->list, &pipe->in_downcall);
	} else
		gss_msg = old;
	spin_unlock(&pipe->lock);
	return gss_msg;
}

static void
__gss_unhash_msg(struct gss_upcall_msg *gss_msg)
{
	list_del_init(&gss_msg->list);
	rpc_wake_up_status(&gss_msg->rpc_waitqueue, gss_msg->msg.errno);
	wake_up_all(&gss_msg->waitqueue);
	atomic_dec(&gss_msg->count);
}

static void
gss_unhash_msg(struct gss_upcall_msg *gss_msg)
{
	struct rpc_pipe *pipe = gss_msg->pipe;

	if (list_empty(&gss_msg->list))
		return;
	spin_lock(&pipe->lock);
	if (!list_empty(&gss_msg->list))
		__gss_unhash_msg(gss_msg);
	spin_unlock(&pipe->lock);
}

static void
gss_handle_downcall_result(struct gss_cred *gss_cred, struct gss_upcall_msg *gss_msg)
{
	switch (gss_msg->msg.errno) {
	case 0:
		if (gss_msg->ctx == NULL)
			break;
		clear_bit(RPCAUTH_CRED_NEGATIVE, &gss_cred->gc_base.cr_flags);
		gss_cred_set_ctx(&gss_cred->gc_base, gss_msg->ctx);
		break;
	case -EKEYEXPIRED:
		set_bit(RPCAUTH_CRED_NEGATIVE, &gss_cred->gc_base.cr_flags);
	}
	gss_cred->gc_upcall_timestamp = jiffies;
	gss_cred->gc_upcall = NULL;
	rpc_wake_up_status(&gss_msg->rpc_waitqueue, gss_msg->msg.errno);
}

static void
gss_upcall_callback(struct rpc_task *task)
{
	struct gss_cred *gss_cred = container_of(task->tk_rqstp->rq_cred,
			struct gss_cred, gc_base);
	struct gss_upcall_msg *gss_msg = gss_cred->gc_upcall;
	struct rpc_pipe *pipe = gss_msg->pipe;

	spin_lock(&pipe->lock);
	gss_handle_downcall_result(gss_cred, gss_msg);
	spin_unlock(&pipe->lock);
	task->tk_status = gss_msg->msg.errno;
	gss_release_msg(gss_msg);
}

static void gss_encode_v0_msg(struct gss_upcall_msg *gss_msg)
{
	uid_t uid = from_kuid(&init_user_ns, gss_msg->uid);
	memcpy(gss_msg->databuf, &uid, sizeof(uid));
	gss_msg->msg.data = gss_msg->databuf;
	gss_msg->msg.len = sizeof(uid);

	BUILD_BUG_ON(sizeof(uid) > sizeof(gss_msg->databuf));
}

static int gss_encode_v1_msg(struct gss_upcall_msg *gss_msg,
				const char *service_name,
				const char *target_name)
{
	struct gss_api_mech *mech = gss_msg->auth->mech;
	char *p = gss_msg->databuf;
	size_t buflen = sizeof(gss_msg->databuf);
	int len;

	len = scnprintf(p, buflen, "mech=%s uid=%d ", mech->gm_name,
			from_kuid(&init_user_ns, gss_msg->uid));
	buflen -= len;
	p += len;
	gss_msg->msg.len = len;
	if (target_name) {
		len = scnprintf(p, buflen, "target=%s ", target_name);
		buflen -= len;
		p += len;
		gss_msg->msg.len += len;
	}
	if (service_name != NULL) {
		len = scnprintf(p, buflen, "service=%s ", service_name);
		buflen -= len;
		p += len;
		gss_msg->msg.len += len;
	}
	if (mech->gm_upcall_enctypes) {
		len = scnprintf(p, buflen, "enctypes=%s ",
				mech->gm_upcall_enctypes);
		buflen -= len;
		p += len;
		gss_msg->msg.len += len;
	}
	len = scnprintf(p, buflen, "\n");
	if (len == 0)
		goto out_overflow;
	gss_msg->msg.len += len;

	gss_msg->msg.data = gss_msg->databuf;
	return 0;
out_overflow:
	WARN_ON_ONCE(1);
	return -ENOMEM;
}

static struct gss_upcall_msg *
gss_alloc_msg(struct gss_auth *gss_auth,
		kuid_t uid, const char *service_name)
{
	struct gss_upcall_msg *gss_msg;
	int vers;
	int err = -ENOMEM;

	gss_msg = kzalloc(sizeof(*gss_msg), GFP_NOFS);
	if (gss_msg == NULL)
		goto err;
	vers = get_pipe_version(gss_auth->net);
	err = vers;
	if (err < 0)
		goto err_free_msg;
	gss_msg->pipe = gss_auth->gss_pipe[vers]->pipe;
	INIT_LIST_HEAD(&gss_msg->list);
	rpc_init_wait_queue(&gss_msg->rpc_waitqueue, "RPCSEC_GSS upcall waitq");
	init_waitqueue_head(&gss_msg->waitqueue);
	atomic_set(&gss_msg->count, 1);
	gss_msg->uid = uid;
	gss_msg->auth = gss_auth;
	switch (vers) {
	case 0:
		gss_encode_v0_msg(gss_msg);
		break;
	default:
		err = gss_encode_v1_msg(gss_msg, service_name, gss_auth->target_name);
		if (err)
			goto err_put_pipe_version;
	};
	kref_get(&gss_auth->kref);
	return gss_msg;
err_put_pipe_version:
	put_pipe_version(gss_auth->net);
err_free_msg:
	kfree(gss_msg);
err:
	return ERR_PTR(err);
}

static struct gss_upcall_msg *
gss_setup_upcall(struct gss_auth *gss_auth, struct rpc_cred *cred)
{
	struct gss_cred *gss_cred = container_of(cred,
			struct gss_cred, gc_base);
	struct gss_upcall_msg *gss_new, *gss_msg;
	kuid_t uid = cred->cr_uid;

	gss_new = gss_alloc_msg(gss_auth, uid, gss_cred->gc_principal);
	if (IS_ERR(gss_new))
		return gss_new;
	gss_msg = gss_add_msg(gss_new);
	if (gss_msg == gss_new) {
		int res;
		atomic_inc(&gss_msg->count);
		res = rpc_queue_upcall(gss_new->pipe, &gss_new->msg);
		if (res) {
			gss_unhash_msg(gss_new);
			atomic_dec(&gss_msg->count);
			gss_release_msg(gss_new);
			gss_msg = ERR_PTR(res);
		}
	} else
		gss_release_msg(gss_new);
	return gss_msg;
}

static void warn_gssd(void)
{
	dprintk("AUTH_GSS upcall failed. Please check user daemon is running.\n");
}

static inline int
gss_refresh_upcall(struct rpc_task *task)
{
	struct rpc_cred *cred = task->tk_rqstp->rq_cred;
	struct gss_auth *gss_auth = container_of(cred->cr_auth,
			struct gss_auth, rpc_auth);
	struct gss_cred *gss_cred = container_of(cred,
			struct gss_cred, gc_base);
	struct gss_upcall_msg *gss_msg;
	struct rpc_pipe *pipe;
	int err = 0;

	dprintk("RPC: %5u %s for uid %u\n",
		task->tk_pid, __func__, from_kuid(&init_user_ns, cred->cr_uid));
	gss_msg = gss_setup_upcall(gss_auth, cred);
	if (PTR_ERR(gss_msg) == -EAGAIN) {
		/* XXX: warning on the first, under the assumption we
		 * shouldn't normally hit this case on a refresh. */
		warn_gssd();
		task->tk_timeout = 15*HZ;
		rpc_sleep_on(&pipe_version_rpc_waitqueue, task, NULL);
		return -EAGAIN;
	}
	if (IS_ERR(gss_msg)) {
		err = PTR_ERR(gss_msg);
		goto out;
	}
	pipe = gss_msg->pipe;
	spin_lock(&pipe->lock);
	if (gss_cred->gc_upcall != NULL)
		rpc_sleep_on(&gss_cred->gc_upcall->rpc_waitqueue, task, NULL);
	else if (gss_msg->ctx == NULL && gss_msg->msg.errno >= 0) {
		task->tk_timeout = 0;
		gss_cred->gc_upcall = gss_msg;
		/* gss_upcall_callback will release the reference to gss_upcall_msg */
		atomic_inc(&gss_msg->count);
		rpc_sleep_on(&gss_msg->rpc_waitqueue, task, gss_upcall_callback);
	} else {
		gss_handle_downcall_result(gss_cred, gss_msg);
		err = gss_msg->msg.errno;
	}
	spin_unlock(&pipe->lock);
	gss_release_msg(gss_msg);
out:
	dprintk("RPC: %5u %s for uid %u result %d\n",
		task->tk_pid, __func__,
		from_kuid(&init_user_ns, cred->cr_uid),	err);
	return err;
}

static inline int
gss_create_upcall(struct gss_auth *gss_auth, struct gss_cred *gss_cred)
{
	struct net *net = gss_auth->net;
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct rpc_pipe *pipe;
	struct rpc_cred *cred = &gss_cred->gc_base;
	struct gss_upcall_msg *gss_msg;
	DEFINE_WAIT(wait);
	int err;

	dprintk("RPC:       %s for uid %u\n",
		__func__, from_kuid(&init_user_ns, cred->cr_uid));
retry:
	err = 0;
	/* if gssd is down, just skip upcalling altogether */
	if (!gssd_running(net)) {
		warn_gssd();
		return -EACCES;
	}
	gss_msg = gss_setup_upcall(gss_auth, cred);
	if (PTR_ERR(gss_msg) == -EAGAIN) {
		err = wait_event_interruptible_timeout(pipe_version_waitqueue,
				sn->pipe_version >= 0, 15 * HZ);
		if (sn->pipe_version < 0) {
			warn_gssd();
			err = -EACCES;
		}
		if (err < 0)
			goto out;
		goto retry;
	}
	if (IS_ERR(gss_msg)) {
		err = PTR_ERR(gss_msg);
		goto out;
	}
	pipe = gss_msg->pipe;
	for (;;) {
		prepare_to_wait(&gss_msg->waitqueue, &wait, TASK_KILLABLE);
		spin_lock(&pipe->lock);
		if (gss_msg->ctx != NULL || gss_msg->msg.errno < 0) {
			break;
		}
		spin_unlock(&pipe->lock);
		if (fatal_signal_pending(current)) {
			err = -ERESTARTSYS;
			goto out_intr;
		}
		schedule();
	}
	if (gss_msg->ctx)
		gss_cred_set_ctx(cred, gss_msg->ctx);
	else
		err = gss_msg->msg.errno;
	spin_unlock(&pipe->lock);
out_intr:
	finish_wait(&gss_msg->waitqueue, &wait);
	gss_release_msg(gss_msg);
out:
	dprintk("RPC:       %s for uid %u result %d\n",
		__func__, from_kuid(&init_user_ns, cred->cr_uid), err);
	return err;
}

static struct gss_upcall_msg *
gss_find_downcall(struct rpc_pipe *pipe, kuid_t uid)
{
	struct gss_upcall_msg *pos;
	list_for_each_entry(pos, &pipe->in_downcall, list) {
		if (!uid_eq(pos->uid, uid))
			continue;
		if (!rpc_msg_is_inflight(&pos->msg))
			continue;
		refcount_inc(&pos->count);
		return pos;
	}
	return NULL;
}

#define MSG_BUF_MAXSIZE 1024

static ssize_t
gss_pipe_downcall(struct file *filp, const char __user *src, size_t mlen)
{
	const void *p, *end;
	void *buf;
	struct gss_upcall_msg *gss_msg;
	struct rpc_pipe *pipe = RPC_I(file_inode(filp))->pipe;
	struct gss_cl_ctx *ctx;
	uid_t id;
	kuid_t uid;
	ssize_t err = -EFBIG;

	if (mlen > MSG_BUF_MAXSIZE)
		goto out;
	err = -ENOMEM;
	buf = kmalloc(mlen, GFP_NOFS);
	if (!buf)
		goto out;

	err = -EFAULT;
	if (copy_from_user(buf, src, mlen))
		goto err;

	end = (const void *)((char *)buf + mlen);
	p = simple_get_bytes(buf, end, &id, sizeof(id));
	if (IS_ERR(p)) {
		err = PTR_ERR(p);
		goto err;
	}

	uid = make_kuid(&init_user_ns, id);
	if (!uid_valid(uid)) {
		err = -EINVAL;
		goto err;
	}

	err = -ENOMEM;
	ctx = gss_alloc_context();
	if (ctx == NULL)
		goto err;

	err = -ENOENT;
	/* Find a matching upcall */
	spin_lock(&pipe->lock);
	gss_msg = gss_find_downcall(pipe, uid);
	if (gss_msg == NULL) {
		spin_unlock(&pipe->lock);
		goto err_put_ctx;
	}
	list_del_init(&gss_msg->list);
	spin_unlock(&pipe->lock);

	p = gss_fill_context(p, end, ctx, gss_msg->auth->mech);
	if (IS_ERR(p)) {
		err = PTR_ERR(p);
		switch (err) {
		case -EACCES:
		case -EKEYEXPIRED:
			gss_msg->msg.errno = err;
			err = mlen;
			break;
		case -EFAULT:
		case -ENOMEM:
		case -EINVAL:
		case -ENOSYS:
			gss_msg->msg.errno = -EAGAIN;
			break;
		default:
			printk(KERN_CRIT "%s: bad return from "
				"gss_fill_context: %zd\n", __func__, err);
			BUG();
		}
		goto err_release_msg;
	}
	gss_msg->ctx = gss_get_ctx(ctx);
	err = mlen;

err_release_msg:
	spin_lock(&pipe->lock);
	__gss_unhash_msg(gss_msg);
	spin_unlock(&pipe->lock);
	gss_release_msg(gss_msg);
err_put_ctx:
	gss_put_ctx(ctx);
err:
	kfree(buf);
out:
	dprintk("RPC:       %s returning %Zd\n", __func__, err);
	return err;
}

static int gss_pipe_open(struct inode *inode, int new_version)
{
	struct net *net = inode->i_sb->s_fs_info;
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	int ret = 0;

	spin_lock(&pipe_version_lock);
	if (sn->pipe_version < 0) {
		/* First open of any gss pipe determines the version: */
		sn->pipe_version = new_version;
		rpc_wake_up(&pipe_version_rpc_waitqueue);
		wake_up(&pipe_version_waitqueue);
	} else if (sn->pipe_version != new_version) {
		/* Trying to open a pipe of a different version */
		ret = -EBUSY;
		goto out;
	}
	atomic_inc(&sn->pipe_users);
out:
	spin_unlock(&pipe_version_lock);
	return ret;

}

static int gss_pipe_open_v0(struct inode *inode)
{
	return gss_pipe_open(inode, 0);
}

static int gss_pipe_open_v1(struct inode *inode)
{
	return gss_pipe_open(inode, 1);
}

static void
gss_pipe_release(struct inode *inode)
{
	struct net *net = inode->i_sb->s_fs_info;
	struct rpc_pipe *pipe = RPC_I(inode)->pipe;
	struct gss_upcall_msg *gss_msg;

restart:
	spin_lock(&pipe->lock);
	list_for_each_entry(gss_msg, &pipe->in_downcall, list) {

		if (!list_empty(&gss_msg->msg.list))
			continue;
		gss_msg->msg.errno = -EPIPE;
		atomic_inc(&gss_msg->count);
		__gss_unhash_msg(gss_msg);
		spin_unlock(&pipe->lock);
		gss_release_msg(gss_msg);
		goto restart;
	}
	spin_unlock(&pipe->lock);

	put_pipe_version(net);
}

static void
gss_pipe_destroy_msg(struct rpc_pipe_msg *msg)
{
	struct gss_upcall_msg *gss_msg = container_of(msg, struct gss_upcall_msg, msg);

	if (msg->errno < 0) {
		dprintk("RPC:       %s releasing msg %p\n",
			__func__, gss_msg);
		atomic_inc(&gss_msg->count);
		gss_unhash_msg(gss_msg);
		if (msg->errno == -ETIMEDOUT)
			warn_gssd();
		gss_release_msg(gss_msg);
	}
	gss_release_msg(gss_msg);
}

static void gss_pipe_dentry_destroy(struct dentry *dir,
		struct rpc_pipe_dir_object *pdo)
{
	struct gss_pipe *gss_pipe = pdo->pdo_data;
	struct rpc_pipe *pipe = gss_pipe->pipe;

	if (pipe->dentry != NULL) {
		rpc_unlink(pipe->dentry);
		pipe->dentry = NULL;
	}
}

static int gss_pipe_dentry_create(struct dentry *dir,
		struct rpc_pipe_dir_object *pdo)
{
	struct gss_pipe *p = pdo->pdo_data;
	struct dentry *dentry;

	dentry = rpc_mkpipe_dentry(dir, p->name, p->clnt, p->pipe);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	p->pipe->dentry = dentry;
	return 0;
}

static const struct rpc_pipe_dir_object_ops gss_pipe_dir_object_ops = {
	.create = gss_pipe_dentry_create,
	.destroy = gss_pipe_dentry_destroy,
};

static struct gss_pipe *gss_pipe_alloc(struct rpc_clnt *clnt,
		const char *name,
		const struct rpc_pipe_ops *upcall_ops)
{
	struct gss_pipe *p;
	int err = -ENOMEM;

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL)
		goto err;
	p->pipe = rpc_mkpipe_data(upcall_ops, RPC_PIPE_WAIT_FOR_OPEN);
	if (IS_ERR(p->pipe)) {
		err = PTR_ERR(p->pipe);
		goto err_free_gss_pipe;
	}
	p->name = name;
	p->clnt = clnt;
	kref_init(&p->kref);
	rpc_init_pipe_dir_object(&p->pdo,
			&gss_pipe_dir_object_ops,
			p);
	return p;
err_free_gss_pipe:
	kfree(p);
err:
	return ERR_PTR(err);
}

struct gss_alloc_pdo {
	struct rpc_clnt *clnt;
	const char *name;
	const struct rpc_pipe_ops *upcall_ops;
};

static int gss_pipe_match_pdo(struct rpc_pipe_dir_object *pdo, void *data)
{
	struct gss_pipe *gss_pipe;
	struct gss_alloc_pdo *args = data;

	if (pdo->pdo_ops != &gss_pipe_dir_object_ops)
		return 0;
	gss_pipe = container_of(pdo, struct gss_pipe, pdo);
	if (strcmp(gss_pipe->name, args->name) != 0)
		return 0;
	if (!kref_get_unless_zero(&gss_pipe->kref))
		return 0;
	return 1;
}

static struct rpc_pipe_dir_object *gss_pipe_alloc_pdo(void *data)
{
	struct gss_pipe *gss_pipe;
	struct gss_alloc_pdo *args = data;

	gss_pipe = gss_pipe_alloc(args->clnt, args->name, args->upcall_ops);
	if (!IS_ERR(gss_pipe))
		return &gss_pipe->pdo;
	return NULL;
}

static struct gss_pipe *gss_pipe_get(struct rpc_clnt *clnt,
		const char *name,
		const struct rpc_pipe_ops *upcall_ops)
{
	struct net *net = rpc_net_ns(clnt);
	struct rpc_pipe_dir_object *pdo;
	struct gss_alloc_pdo args = {
		.clnt = clnt,
		.name = name,
		.upcall_ops = upcall_ops,
	};

	pdo = rpc_find_or_alloc_pipe_dir_object(net,
			&clnt->cl_pipedir_objects,
			gss_pipe_match_pdo,
			gss_pipe_alloc_pdo,
			&args);
	if (pdo != NULL)
		return container_of(pdo, struct gss_pipe, pdo);
	return ERR_PTR(-ENOMEM);
}

static void __gss_pipe_free(struct gss_pipe *p)
{
	struct rpc_clnt *clnt = p->clnt;
	struct net *net = rpc_net_ns(clnt);

	rpc_remove_pipe_dir_object(net,
			&clnt->cl_pipedir_objects,
			&p->pdo);
	rpc_destroy_pipe_data(p->pipe);
	kfree(p);
}

static void __gss_pipe_release(struct kref *kref)
{
	struct gss_pipe *p = container_of(kref, struct gss_pipe, kref);

	__gss_pipe_free(p);
}

static void gss_pipe_free(struct gss_pipe *p)
{
	if (p != NULL)
		kref_put(&p->kref, __gss_pipe_release);
}

/*
 * NOTE: we have the opportunity to use different
 * parameters based on the input flavor (which must be a pseudoflavor)
 */
static struct gss_auth *
gss_create_new(struct rpc_auth_create_args *args, struct rpc_clnt *clnt)
{
	rpc_authflavor_t flavor = args->pseudoflavor;
	struct gss_auth *gss_auth;
	struct gss_pipe *gss_pipe;
	struct rpc_auth * auth;
	int err = -ENOMEM; /* XXX? */

	dprintk("RPC:       creating GSS authenticator for client %p\n", clnt);

	if (!try_module_get(THIS_MODULE))
		return ERR_PTR(err);
	if (!(gss_auth = kmalloc(sizeof(*gss_auth), GFP_KERNEL)))
		goto out_dec;
	INIT_HLIST_NODE(&gss_auth->hash);
	gss_auth->target_name = NULL;
	if (args->target_name) {
		gss_auth->target_name = kstrdup(args->target_name, GFP_KERNEL);
		if (gss_auth->target_name == NULL)
			goto err_free;
	}
	gss_auth->client = clnt;
	gss_auth->net = get_net(rpc_net_ns(clnt));
	err = -EINVAL;
	gss_auth->mech = gss_mech_get_by_pseudoflavor(flavor);
	if (!gss_auth->mech) {
		dprintk("RPC:       Pseudoflavor %d not found!\n", flavor);
		goto err_put_net;
	}
	gss_auth->service = gss_pseudoflavor_to_service(gss_auth->mech, flavor);
	if (gss_auth->service == 0)
		goto err_put_mech;
	if (!gssd_running(gss_auth->net))
		goto err_put_mech;
	auth = &gss_auth->rpc_auth;
	auth->au_cslack = GSS_CRED_SLACK >> 2;
	auth->au_rslack = GSS_VERF_SLACK >> 2;
	auth->au_ops = &authgss_ops;
	auth->au_flavor = flavor;
	atomic_set(&auth->au_count, 1);
	kref_init(&gss_auth->kref);

	err = rpcauth_init_credcache(auth);
	if (err)
		goto err_put_mech;
	/*
	 * Note: if we created the old pipe first, then someone who
	 * examined the directory at the right moment might conclude
	 * that we supported only the old pipe.  So we instead create
	 * the new pipe first.
	 */
	gss_pipe = gss_pipe_get(clnt, "gssd", &gss_upcall_ops_v1);
	if (IS_ERR(gss_pipe)) {
		err = PTR_ERR(gss_pipe);
		goto err_destroy_credcache;
	}
	gss_auth->gss_pipe[1] = gss_pipe;

	gss_pipe = gss_pipe_get(clnt, gss_auth->mech->gm_name,
			&gss_upcall_ops_v0);
	if (IS_ERR(gss_pipe)) {
		err = PTR_ERR(gss_pipe);
		goto err_destroy_pipe_1;
	}
	gss_auth->gss_pipe[0] = gss_pipe;

	return gss_auth;
err_destroy_pipe_1:
	gss_pipe_free(gss_auth->gss_pipe[1]);
err_destroy_credcache:
	rpcauth_destroy_credcache(auth);
err_put_mech:
	gss_mech_put(gss_auth->mech);
err_put_net:
	put_net(gss_auth->net);
err_free:
	kfree(gss_auth->target_name);
	kfree(gss_auth);
out_dec:
	module_put(THIS_MODULE);
	return ERR_PTR(err);
}

static void
gss_free(struct gss_auth *gss_auth)
{
	gss_pipe_free(gss_auth->gss_pipe[0]);
	gss_pipe_free(gss_auth->gss_pipe[1]);
	gss_mech_put(gss_auth->mech);
	put_net(gss_auth->net);
	kfree(gss_auth->target_name);

	kfree(gss_auth);
	module_put(THIS_MODULE);
}

static void
gss_free_callback(struct kref *kref)
{
	struct gss_auth *gss_auth = container_of(kref, struct gss_auth, kref);

	gss_free(gss_auth);
}

static void
gss_put_auth(struct gss_auth *gss_auth)
{
	kref_put(&gss_auth->kref, gss_free_callback);
}

static void
gss_destroy(struct rpc_auth *auth)
{
	struct gss_auth *gss_auth = container_of(auth,
			struct gss_auth, rpc_auth);

	dprintk("RPC:       destroying GSS authenticator %p flavor %d\n",
			auth, auth->au_flavor);

	if (hash_hashed(&gss_auth->hash)) {
		spin_lock(&gss_auth_hash_lock);
		hash_del(&gss_auth->hash);
		spin_unlock(&gss_auth_hash_lock);
	}

	gss_pipe_free(gss_auth->gss_pipe[0]);
	gss_auth->gss_pipe[0] = NULL;
	gss_pipe_free(gss_auth->gss_pipe[1]);
	gss_auth->gss_pipe[1] = NULL;
	rpcauth_destroy_credcache(auth);

	gss_put_auth(gss_auth);
}

/*
 * Auths may be shared between rpc clients that were cloned from a
 * common client with the same xprt, if they also share the flavor and
 * target_name.
 *
 * The auth is looked up from the oldest parent sharing the same
 * cl_xprt, and the auth itself references only that common parent
 * (which is guaranteed to last as long as any of its descendants).
 */
static struct gss_auth *
gss_auth_find_or_add_hashed(struct rpc_auth_create_args *args,
		struct rpc_clnt *clnt,
		struct gss_auth *new)
{
	struct gss_auth *gss_auth;
	unsigned long hashval = (unsigned long)clnt;

	spin_lock(&gss_auth_hash_lock);
	hash_for_each_possible(gss_auth_hash_table,
			gss_auth,
			hash,
			hashval) {
		if (gss_auth->client != clnt)
			continue;
		if (gss_auth->rpc_auth.au_flavor != args->pseudoflavor)
			continue;
		if (gss_auth->target_name != args->target_name) {
			if (gss_auth->target_name == NULL)
				continue;
			if (args->target_name == NULL)
				continue;
			if (strcmp(gss_auth->target_name, args->target_name))
				continue;
		}
		if (!atomic_inc_not_zero(&gss_auth->rpc_auth.au_count))
			continue;
		goto out;
	}
	if (new)
		hash_add(gss_auth_hash_table, &new->hash, hashval);
	gss_auth = new;
out:
	spin_unlock(&gss_auth_hash_lock);
	return gss_auth;
}

static struct gss_auth *
gss_create_hashed(struct rpc_auth_create_args *args, struct rpc_clnt *clnt)
{
	struct gss_auth *gss_auth;
	struct gss_auth *new;

	gss_auth = gss_auth_find_or_add_hashed(args, clnt, NULL);
	if (gss_auth != NULL)
		goto out;
	new = gss_create_new(args, clnt);
	if (IS_ERR(new))
		return new;
	gss_auth = gss_auth_find_or_add_hashed(args, clnt, new);
	if (gss_auth != new)
		gss_destroy(&new->rpc_auth);
out:
	return gss_auth;
}

static struct rpc_auth *
gss_create(struct rpc_auth_create_args *args, struct rpc_clnt *clnt)
{
	struct gss_auth *gss_auth;
	struct rpc_xprt *xprt = rcu_access_pointer(clnt->cl_xprt);

	while (clnt != clnt->cl_parent) {
		struct rpc_clnt *parent = clnt->cl_parent;
		/* Find the original parent for this transport */
		if (rcu_access_pointer(parent->cl_xprt) != xprt)
			break;
		clnt = parent;
	}

	gss_auth = gss_create_hashed(args, clnt);
	if (IS_ERR(gss_auth))
		return ERR_CAST(gss_auth);
	return &gss_auth->rpc_auth;
}

/*
 * gss_destroying_context will cause the RPCSEC_GSS to send a NULL RPC call
 * to the server with the GSS control procedure field set to
 * RPC_GSS_PROC_DESTROY. This should normally cause the server to release
 * all RPCSEC_GSS state associated with that context.
 */
static int
gss_destroying_context(struct rpc_cred *cred)
{
	struct gss_cred *gss_cred = container_of(cred, struct gss_cred, gc_base);
	struct gss_auth *gss_auth = container_of(cred->cr_auth, struct gss_auth, rpc_auth);
	struct gss_cl_ctx *ctx = rcu_dereference_protected(gss_cred->gc_ctx, 1);
	struct rpc_task *task;

	if (test_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags) == 0)
		return 0;

	ctx->gc_proc = RPC_GSS_PROC_DESTROY;
	cred->cr_ops = &gss_nullops;

	/* Take a reference to ensure the cred will be destroyed either
	 * by the RPC call or by the put_rpccred() below */
	get_rpccred(cred);

	task = rpc_call_null(gss_auth->client, cred, RPC_TASK_ASYNC|RPC_TASK_SOFT);
	if (!IS_ERR(task))
		rpc_put_task(task);

	put_rpccred(cred);
	return 1;
}

/* gss_destroy_cred (and gss_free_ctx) are used to clean up after failure
 * to create a new cred or context, so they check that things have been
 * allocated before freeing them. */
static void
gss_do_free_ctx(struct gss_cl_ctx *ctx)
{
	dprintk("RPC:       %s\n", __func__);

	gss_delete_sec_context(&ctx->gc_gss_ctx);
	kfree(ctx->gc_wire_ctx.data);
	kfree(ctx->gc_acceptor.data);
	kfree(ctx);
}

static void
gss_free_ctx_callback(struct rcu_head *head)
{
	struct gss_cl_ctx *ctx = container_of(head, struct gss_cl_ctx, gc_rcu);
	gss_do_free_ctx(ctx);
}

static void
gss_free_ctx(struct gss_cl_ctx *ctx)
{
	call_rcu(&ctx->gc_rcu, gss_free_ctx_callback);
}

static void
gss_free_cred(struct gss_cred *gss_cred)
{
	dprintk("RPC:       %s cred=%p\n", __func__, gss_cred);
	kfree(gss_cred);
}

static void
gss_free_cred_callback(struct rcu_head *head)
{
	struct gss_cred *gss_cred = container_of(head, struct gss_cred, gc_base.cr_rcu);
	gss_free_cred(gss_cred);
}

static void
gss_destroy_nullcred(struct rpc_cred *cred)
{
	struct gss_cred *gss_cred = container_of(cred, struct gss_cred, gc_base);
	struct gss_auth *gss_auth = container_of(cred->cr_auth, struct gss_auth, rpc_auth);
	struct gss_cl_ctx *ctx = rcu_dereference_protected(gss_cred->gc_ctx, 1);

	RCU_INIT_POINTER(gss_cred->gc_ctx, NULL);
	call_rcu(&cred->cr_rcu, gss_free_cred_callback);
	if (ctx)
		gss_put_ctx(ctx);
	gss_put_auth(gss_auth);
}

static void
gss_destroy_cred(struct rpc_cred *cred)
{

	if (gss_destroying_context(cred))
		return;
	gss_destroy_nullcred(cred);
}

/*
 * Lookup RPCSEC_GSS cred for the current process
 */
static struct rpc_cred *
gss_lookup_cred(struct rpc_auth *auth, struct auth_cred *acred, int flags)
{
	return rpcauth_lookup_credcache(auth, acred, flags);
}

static struct rpc_cred *
gss_create_cred(struct rpc_auth *auth, struct auth_cred *acred, int flags)
{
	struct gss_auth *gss_auth = container_of(auth, struct gss_auth, rpc_auth);
	struct gss_cred	*cred = NULL;
	int err = -ENOMEM;

	dprintk("RPC:       %s for uid %d, flavor %d\n",
		__func__, from_kuid(&init_user_ns, acred->uid),
		auth->au_flavor);

	if (!(cred = kzalloc(sizeof(*cred), GFP_NOFS)))
		goto out_err;

	rpcauth_init_cred(&cred->gc_base, acred, auth, &gss_credops);
	/*
	 * Note: in order to force a call to call_refresh(), we deliberately
	 * fail to flag the credential as RPCAUTH_CRED_UPTODATE.
	 */
	cred->gc_base.cr_flags = 1UL << RPCAUTH_CRED_NEW;
	cred->gc_service = gss_auth->service;
	cred->gc_principal = NULL;
	if (acred->machine_cred)
		cred->gc_principal = acred->principal;
	kref_get(&gss_auth->kref);
	return &cred->gc_base;

out_err:
	dprintk("RPC:       %s failed with error %d\n", __func__, err);
	return ERR_PTR(err);
}

static int
gss_cred_init(struct rpc_auth *auth, struct rpc_cred *cred)
{
	struct gss_auth *gss_auth = container_of(auth, struct gss_auth, rpc_auth);
	struct gss_cred *gss_cred = container_of(cred,struct gss_cred, gc_base);
	int err;

	do {
		err = gss_create_upcall(gss_auth, gss_cred);
	} while (err == -EAGAIN);
	return err;
}

static char *
gss_stringify_acceptor(struct rpc_cred *cred)
{
	char *string = NULL;
	struct gss_cred *gss_cred = container_of(cred, struct gss_cred, gc_base);
	struct gss_cl_ctx *ctx;
	unsigned int len;
	struct xdr_netobj *acceptor;

	rcu_read_lock();
	ctx = rcu_dereference(gss_cred->gc_ctx);
	if (!ctx)
		goto out;

	len = ctx->gc_acceptor.len;
	rcu_read_unlock();

	/* no point if there's no string */
	if (!len)
		return NULL;
realloc:
	string = kmalloc(len + 1, GFP_KERNEL);
	if (!string)
		return NULL;

	rcu_read_lock();
	ctx = rcu_dereference(gss_cred->gc_ctx);

	/* did the ctx disappear or was it replaced by one with no acceptor? */
	if (!ctx || !ctx->gc_acceptor.len) {
		kfree(string);
		string = NULL;
		goto out;
	}

	acceptor = &ctx->gc_acceptor;

	/*
	 * Did we find a new acceptor that's longer than the original? Allocate
	 * a longer buffer and try again.
	 */
	if (len < acceptor->len) {
		len = acceptor->len;
		rcu_read_unlock();
		kfree(string);
		goto realloc;
	}

	memcpy(string, acceptor->data, acceptor->len);
	string[acceptor->len] = '\0';
out:
	rcu_read_unlock();
	return string;
}

/*
 * Returns -EACCES if GSS context is NULL or will expire within the
 * timeout (miliseconds)
 */
static int
gss_key_timeout(struct rpc_cred *rc)
{
	struct gss_cred *gss_cred = container_of(rc, struct gss_cred, gc_base);
	struct gss_cl_ctx *ctx;
	unsigned long timeout = jiffies + (gss_key_expire_timeo * HZ);
	int ret = 0;

	rcu_read_lock();
	ctx = rcu_dereference(gss_cred->gc_ctx);
	if (!ctx || time_after(timeout, ctx->gc_expiry))
		ret = -EACCES;
	rcu_read_unlock();

	return ret;
}

static int
gss_match(struct auth_cred *acred, struct rpc_cred *rc, int flags)
{
	struct gss_cred *gss_cred = container_of(rc, struct gss_cred, gc_base);
	struct gss_cl_ctx *ctx;
	int ret;

	if (test_bit(RPCAUTH_CRED_NEW, &rc->cr_flags))
		goto out;
	/* Don't match with creds that have expired. */
	rcu_read_lock();
	ctx = rcu_dereference(gss_cred->gc_ctx);
	if (!ctx || time_after(jiffies, ctx->gc_expiry)) {
		rcu_read_unlock();
		return 0;
	}
	rcu_read_unlock();
	if (!test_bit(RPCAUTH_CRED_UPTODATE, &rc->cr_flags))
		return 0;
out:
	if (acred->principal != NULL) {
		if (gss_cred->gc_principal == NULL)
			return 0;
		ret = strcmp(acred->principal, gss_cred->gc_principal) == 0;
		goto check_expire;
	}
	if (gss_cred->gc_principal != NULL)
		return 0;
	ret = uid_eq(rc->cr_uid, acred->uid);

check_expire:
	if (ret == 0)
		return ret;

	/* Notify acred users of GSS context expiration timeout */
	if (test_bit(RPC_CRED_NOTIFY_TIMEOUT, &acred->ac_flags) &&
	    (gss_key_timeout(rc) != 0)) {
		/* test will now be done from generic cred */
		test_and_clear_bit(RPC_CRED_NOTIFY_TIMEOUT, &acred->ac_flags);
		/* tell NFS layer that key will expire soon */
		set_bit(RPC_CRED_KEY_EXPIRE_SOON, &acred->ac_flags);
	}
	return ret;
}

/*
* Marshal credentials.
* Maybe we should keep a cached credential for performance reasons.
*/
static __be32 *
gss_marshal(struct rpc_task *task, __be32 *p)
{
	struct rpc_rqst *req = task->tk_rqstp;
	struct rpc_cred *cred = req->rq_cred;
	struct gss_cred	*gss_cred = container_of(cred, struct gss_cred,
						 gc_base);
	struct gss_cl_ctx	*ctx = gss_cred_get_ctx(cred);
	__be32		*cred_len;
	u32             maj_stat = 0;
	struct xdr_netobj mic;
	struct kvec	iov;
	struct xdr_buf	verf_buf;

	dprintk("RPC: %5u %s\n", task->tk_pid, __func__);

	*p++ = htonl(RPC_AUTH_GSS);
	cred_len = p++;

	spin_lock(&ctx->gc_seq_lock);
	req->rq_seqno = ctx->gc_seq++;
	spin_unlock(&ctx->gc_seq_lock);

	*p++ = htonl((u32) RPC_GSS_VERSION);
	*p++ = htonl((u32) ctx->gc_proc);
	*p++ = htonl((u32) req->rq_seqno);
	*p++ = htonl((u32) gss_cred->gc_service);
	p = xdr_encode_netobj(p, &ctx->gc_wire_ctx);
	*cred_len = htonl((p - (cred_len + 1)) << 2);

	/* We compute the checksum for the verifier over the xdr-encoded bytes
	 * starting with the xid and ending at the end of the credential: */
	iov.iov_base = xprt_skip_transport_header(req->rq_xprt,
					req->rq_snd_buf.head[0].iov_base);
	iov.iov_len = (u8 *)p - (u8 *)iov.iov_base;
	xdr_buf_from_iov(&iov, &verf_buf);

	/* set verifier flavor*/
	*p++ = htonl(RPC_AUTH_GSS);

	mic.data = (u8 *)(p + 1);
	maj_stat = gss_get_mic(ctx->gc_gss_ctx, &verf_buf, &mic);
	if (maj_stat == GSS_S_CONTEXT_EXPIRED) {
		clear_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags);
	} else if (maj_stat != 0) {
		printk("gss_marshal: gss_get_mic FAILED (%d)\n", maj_stat);
		goto out_put_ctx;
	}
	p = xdr_encode_opaque(p, NULL, mic.len);
	gss_put_ctx(ctx);
	return p;
out_put_ctx:
	gss_put_ctx(ctx);
	return NULL;
}

static int gss_renew_cred(struct rpc_task *task)
{
	struct rpc_cred *oldcred = task->tk_rqstp->rq_cred;
	struct gss_cred *gss_cred = container_of(oldcred,
						 struct gss_cred,
						 gc_base);
	struct rpc_auth *auth = oldcred->cr_auth;
	struct auth_cred acred = {
		.uid = oldcred->cr_uid,
		.principal = gss_cred->gc_principal,
		.machine_cred = (gss_cred->gc_principal != NULL ? 1 : 0),
	};
	struct rpc_cred *new;

	new = gss_lookup_cred(auth, &acred, RPCAUTH_LOOKUP_NEW);
	if (IS_ERR(new))
		return PTR_ERR(new);
	task->tk_rqstp->rq_cred = new;
	put_rpccred(oldcred);
	return 0;
}

static int gss_cred_is_negative_entry(struct rpc_cred *cred)
{
	if (test_bit(RPCAUTH_CRED_NEGATIVE, &cred->cr_flags)) {
		unsigned long now = jiffies;
		unsigned long begin, expire;
		struct gss_cred *gss_cred; 

		gss_cred = container_of(cred, struct gss_cred, gc_base);
		begin = gss_cred->gc_upcall_timestamp;
		expire = begin + gss_expired_cred_retry_delay * HZ;

		if (time_in_range_open(now, begin, expire))
			return 1;
	}
	return 0;
}

/*
* Refresh credentials. XXX - finish
*/
static int
gss_refresh(struct rpc_task *task)
{
	struct rpc_cred *cred = task->tk_rqstp->rq_cred;
	int ret = 0;

	if (gss_cred_is_negative_entry(cred))
		return -EKEYEXPIRED;

	if (!test_bit(RPCAUTH_CRED_NEW, &cred->cr_flags) &&
			!test_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags)) {
		ret = gss_renew_cred(task);
		if (ret < 0)
			goto out;
		cred = task->tk_rqstp->rq_cred;
	}

	if (test_bit(RPCAUTH_CRED_NEW, &cred->cr_flags))
		ret = gss_refresh_upcall(task);
out:
	return ret;
}

/* Dummy refresh routine: used only when destroying the context */
static int
gss_refresh_null(struct rpc_task *task)
{
	return 0;
}

static __be32 *
gss_validate(struct rpc_task *task, __be32 *p)
{
	struct rpc_cred *cred = task->tk_rqstp->rq_cred;
	struct gss_cl_ctx *ctx = gss_cred_get_ctx(cred);
	__be32		seq;
	struct kvec	iov;
	struct xdr_buf	verf_buf;
	struct xdr_netobj mic;
	u32		flav,len;
	u32		maj_stat;
	__be32		*ret = ERR_PTR(-EIO);

	dprintk("RPC: %5u %s\n", task->tk_pid, __func__);

	flav = ntohl(*p++);
	if ((len = ntohl(*p++)) > RPC_MAX_AUTH_SIZE)
		goto out_bad;
	if (flav != RPC_AUTH_GSS)
		goto out_bad;
	seq = htonl(task->tk_rqstp->rq_seqno);
	iov.iov_base = &seq;
	iov.iov_len = sizeof(seq);
	xdr_buf_from_iov(&iov, &verf_buf);
	mic.data = (u8 *)p;
	mic.len = len;

	ret = ERR_PTR(-EACCES);
	maj_stat = gss_verify_mic(ctx->gc_gss_ctx, &verf_buf, &mic);
	if (maj_stat == GSS_S_CONTEXT_EXPIRED)
		clear_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags);
	if (maj_stat) {
		dprintk("RPC: %5u %s: gss_verify_mic returned error 0x%08x\n",
			task->tk_pid, __func__, maj_stat);
		goto out_bad;
	}
	/* We leave it to unwrap to calculate au_rslack. For now we just
	 * calculate the length of the verifier: */
	cred->cr_auth->au_verfsize = XDR_QUADLEN(len) + 2;
	gss_put_ctx(ctx);
	dprintk("RPC: %5u %s: gss_verify_mic succeeded.\n",
			task->tk_pid, __func__);
	return p + XDR_QUADLEN(len);
out_bad:
	gss_put_ctx(ctx);
	dprintk("RPC: %5u %s failed ret %ld.\n", task->tk_pid, __func__,
		PTR_ERR(ret));
	return ret;
}

static void gss_wrap_req_encode(kxdreproc_t encode, struct rpc_rqst *rqstp,
				__be32 *p, void *obj)
{
	struct xdr_stream xdr;

	xdr_init_encode(&xdr, &rqstp->rq_snd_buf, p);
	encode(rqstp, &xdr, obj);
}

static inline int
gss_wrap_req_integ(struct rpc_cred *cred, struct gss_cl_ctx *ctx,
		   kxdreproc_t encode, struct rpc_rqst *rqstp,
		   __be32 *p, void *obj)
{
	struct xdr_buf	*snd_buf = &rqstp->rq_snd_buf;
	struct xdr_buf	integ_buf;
	__be32          *integ_len = NULL;
	struct xdr_netobj mic;
	u32		offset;
	__be32		*q;
	struct kvec	*iov;
	u32             maj_stat = 0;
	int		status = -EIO;

	integ_len = p++;
	offset = (u8 *)p - (u8 *)snd_buf->head[0].iov_base;
	*p++ = htonl(rqstp->rq_seqno);

	gss_wrap_req_encode(encode, rqstp, p, obj);

	if (xdr_buf_subsegment(snd_buf, &integ_buf,
				offset, snd_buf->len - offset))
		return status;
	*integ_len = htonl(integ_buf.len);

	/* guess whether we're in the head or the tail: */
	if (snd_buf->page_len || snd_buf->tail[0].iov_len)
		iov = snd_buf->tail;
	else
		iov = snd_buf->head;
	p = iov->iov_base + iov->iov_len;
	mic.data = (u8 *)(p + 1);

	maj_stat = gss_get_mic(ctx->gc_gss_ctx, &integ_buf, &mic);
	status = -EIO; /* XXX? */
	if (maj_stat == GSS_S_CONTEXT_EXPIRED)
		clear_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags);
	else if (maj_stat)
		return status;
	q = xdr_encode_opaque(p, NULL, mic.len);

	offset = (u8 *)q - (u8 *)p;
	iov->iov_len += offset;
	snd_buf->len += offset;
	return 0;
}

static void
priv_release_snd_buf(struct rpc_rqst *rqstp)
{
	int i;

	for (i=0; i < rqstp->rq_enc_pages_num; i++)
		__free_page(rqstp->rq_enc_pages[i]);
	kfree(rqstp->rq_enc_pages);
	rqstp->rq_release_snd_buf = NULL;
}

static int
alloc_enc_pages(struct rpc_rqst *rqstp)
{
	struct xdr_buf *snd_buf = &rqstp->rq_snd_buf;
	int first, last, i;

	if (rqstp->rq_release_snd_buf)
		rqstp->rq_release_snd_buf(rqstp);

	if (snd_buf->page_len == 0) {
		rqstp->rq_enc_pages_num = 0;
		return 0;
	}

	first = snd_buf->page_base >> PAGE_CACHE_SHIFT;
	last = (snd_buf->page_base + snd_buf->page_len - 1) >> PAGE_CACHE_SHIFT;
	rqstp->rq_enc_pages_num = last - first + 1 + 1;
	rqstp->rq_enc_pages
		= kmalloc(rqstp->rq_enc_pages_num * sizeof(struct page *),
				GFP_NOFS);
	if (!rqstp->rq_enc_pages)
		goto out;
	for (i=0; i < rqstp->rq_enc_pages_num; i++) {
		rqstp->rq_enc_pages[i] = alloc_page(GFP_NOFS);
		if (rqstp->rq_enc_pages[i] == NULL)
			goto out_free;
	}
	rqstp->rq_release_snd_buf = priv_release_snd_buf;
	return 0;
out_free:
	rqstp->rq_enc_pages_num = i;
	priv_release_snd_buf(rqstp);
out:
	return -EAGAIN;
}

static inline int
gss_wrap_req_priv(struct rpc_cred *cred, struct gss_cl_ctx *ctx,
		  kxdreproc_t encode, struct rpc_rqst *rqstp,
		  __be32 *p, void *obj)
{
	struct xdr_buf	*snd_buf = &rqstp->rq_snd_buf;
	u32		offset;
	u32             maj_stat;
	int		status;
	__be32		*opaque_len;
	struct page	**inpages;
	int		first;
	int		pad;
	struct kvec	*iov;
	char		*tmp;

	opaque_len = p++;
	offset = (u8 *)p - (u8 *)snd_buf->head[0].iov_base;
	*p++ = htonl(rqstp->rq_seqno);

	gss_wrap_req_encode(encode, rqstp, p, obj);

	status = alloc_enc_pages(rqstp);
	if (status)
		return status;
	first = snd_buf->page_base >> PAGE_CACHE_SHIFT;
	inpages = snd_buf->pages + first;
	snd_buf->pages = rqstp->rq_enc_pages;
	snd_buf->page_base -= first << PAGE_CACHE_SHIFT;
	/*
	 * Give the tail its own page, in case we need extra space in the
	 * head when wrapping:
	 *
	 * call_allocate() allocates twice the slack space required
	 * by the authentication flavor to rq_callsize.
	 * For GSS, slack is GSS_CRED_SLACK.
	 */
	if (snd_buf->page_len || snd_buf->tail[0].iov_len) {
		tmp = page_address(rqstp->rq_enc_pages[rqstp->rq_enc_pages_num - 1]);
		memcpy(tmp, snd_buf->tail[0].iov_base, snd_buf->tail[0].iov_len);
		snd_buf->tail[0].iov_base = tmp;
	}
	maj_stat = gss_wrap(ctx->gc_gss_ctx, offset, snd_buf, inpages);
	/* slack space should prevent this ever happening: */
	BUG_ON(snd_buf->len > snd_buf->buflen);
	status = -EIO;
	/* We're assuming that when GSS_S_CONTEXT_EXPIRED, the encryption was
	 * done anyway, so it's safe to put the request on the wire: */
	if (maj_stat == GSS_S_CONTEXT_EXPIRED)
		clear_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags);
	else if (maj_stat)
		return status;

	*opaque_len = htonl(snd_buf->len - offset);
	/* guess whether we're in the head or the tail: */
	if (snd_buf->page_len || snd_buf->tail[0].iov_len)
		iov = snd_buf->tail;
	else
		iov = snd_buf->head;
	p = iov->iov_base + iov->iov_len;
	pad = 3 - ((snd_buf->len - offset - 1) & 3);
	memset(p, 0, pad);
	iov->iov_len += pad;
	snd_buf->len += pad;

	return 0;
}

static int
gss_wrap_req(struct rpc_task *task,
	     kxdreproc_t encode, void *rqstp, __be32 *p, void *obj)
{
	struct rpc_cred *cred = task->tk_rqstp->rq_cred;
	struct gss_cred	*gss_cred = container_of(cred, struct gss_cred,
			gc_base);
	struct gss_cl_ctx *ctx = gss_cred_get_ctx(cred);
	int             status = -EIO;

	dprintk("RPC: %5u %s\n", task->tk_pid, __func__);
	if (ctx->gc_proc != RPC_GSS_PROC_DATA) {
		/* The spec seems a little ambiguous here, but I think that not
		 * wrapping context destruction requests makes the most sense.
		 */
		gss_wrap_req_encode(encode, rqstp, p, obj);
		status = 0;
		goto out;
	}
	switch (gss_cred->gc_service) {
	case RPC_GSS_SVC_NONE:
		gss_wrap_req_encode(encode, rqstp, p, obj);
		status = 0;
		break;
	case RPC_GSS_SVC_INTEGRITY:
		status = gss_wrap_req_integ(cred, ctx, encode, rqstp, p, obj);
		break;
	case RPC_GSS_SVC_PRIVACY:
		status = gss_wrap_req_priv(cred, ctx, encode, rqstp, p, obj);
		break;
	}
out:
	gss_put_ctx(ctx);
	dprintk("RPC: %5u %s returning %d\n", task->tk_pid, __func__, status);
	return status;
}

static inline int
gss_unwrap_resp_integ(struct rpc_cred *cred, struct gss_cl_ctx *ctx,
		struct rpc_rqst *rqstp, __be32 **p)
{
	struct xdr_buf	*rcv_buf = &rqstp->rq_rcv_buf;
	struct xdr_buf integ_buf;
	struct xdr_netobj mic;
	u32 data_offset, mic_offset;
	u32 integ_len;
	u32 maj_stat;
	int status = -EIO;

	integ_len = ntohl(*(*p)++);
	if (integ_len & 3)
		return status;
	data_offset = (u8 *)(*p) - (u8 *)rcv_buf->head[0].iov_base;
	mic_offset = integ_len + data_offset;
	if (mic_offset > rcv_buf->len)
		return status;
	if (ntohl(*(*p)++) != rqstp->rq_seqno)
		return status;

	if (xdr_buf_subsegment(rcv_buf, &integ_buf, data_offset,
				mic_offset - data_offset))
		return status;

	if (xdr_buf_read_netobj(rcv_buf, &mic, mic_offset))
		return status;

	maj_stat = gss_verify_mic(ctx->gc_gss_ctx, &integ_buf, &mic);
	if (maj_stat == GSS_S_CONTEXT_EXPIRED)
		clear_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags);
	if (maj_stat != GSS_S_COMPLETE)
		return status;
	return 0;
}

static inline int
gss_unwrap_resp_priv(struct rpc_cred *cred, struct gss_cl_ctx *ctx,
		struct rpc_rqst *rqstp, __be32 **p)
{
	struct xdr_buf  *rcv_buf = &rqstp->rq_rcv_buf;
	u32 offset;
	u32 opaque_len;
	u32 maj_stat;
	int status = -EIO;

	opaque_len = ntohl(*(*p)++);
	offset = (u8 *)(*p) - (u8 *)rcv_buf->head[0].iov_base;
	if (offset + opaque_len > rcv_buf->len)
		return status;
	/* remove padding: */
	rcv_buf->len = offset + opaque_len;

	maj_stat = gss_unwrap(ctx->gc_gss_ctx, offset, rcv_buf);
	if (maj_stat == GSS_S_CONTEXT_EXPIRED)
		clear_bit(RPCAUTH_CRED_UPTODATE, &cred->cr_flags);
	if (maj_stat != GSS_S_COMPLETE)
		return status;
	if (ntohl(*(*p)++) != rqstp->rq_seqno)
		return status;

	return 0;
}

static int
gss_unwrap_req_decode(kxdrdproc_t decode, struct rpc_rqst *rqstp,
		      __be32 *p, void *obj)
{
	struct xdr_stream xdr;

	xdr_init_decode(&xdr, &rqstp->rq_rcv_buf, p);
	return decode(rqstp, &xdr, obj);
}

static int
gss_unwrap_resp(struct rpc_task *task,
		kxdrdproc_t decode, void *rqstp, __be32 *p, void *obj)
{
	struct rpc_cred *cred = task->tk_rqstp->rq_cred;
	struct gss_cred *gss_cred = container_of(cred, struct gss_cred,
			gc_base);
	struct gss_cl_ctx *ctx = gss_cred_get_ctx(cred);
	__be32		*savedp = p;
	struct kvec	*head = ((struct rpc_rqst *)rqstp)->rq_rcv_buf.head;
	int		savedlen = head->iov_len;
	int             status = -EIO;

	if (ctx->gc_proc != RPC_GSS_PROC_DATA)
		goto out_decode;
	switch (gss_cred->gc_service) {
	case RPC_GSS_SVC_NONE:
		break;
	case RPC_GSS_SVC_INTEGRITY:
		status = gss_unwrap_resp_integ(cred, ctx, rqstp, &p);
		if (status)
			goto out;
		break;
	case RPC_GSS_SVC_PRIVACY:
		status = gss_unwrap_resp_priv(cred, ctx, rqstp, &p);
		if (status)
			goto out;
		break;
	}
	/* take into account extra slack for integrity and privacy cases: */
	cred->cr_auth->au_rslack = cred->cr_auth->au_verfsize + (p - savedp)
						+ (savedlen - head->iov_len);
out_decode:
	status = gss_unwrap_req_decode(decode, rqstp, p, obj);
out:
	gss_put_ctx(ctx);
	dprintk("RPC: %5u %s returning %d\n",
		task->tk_pid, __func__, status);
	return status;
}

static const struct rpc_authops authgss_ops = {
	.owner		= THIS_MODULE,
	.au_flavor	= RPC_AUTH_GSS,
	.au_name	= "RPCSEC_GSS",
	.create		= gss_create,
	.destroy	= gss_destroy,
	.lookup_cred	= gss_lookup_cred,
	.crcreate	= gss_create_cred,
	.list_pseudoflavors = gss_mech_list_pseudoflavors,
	.info2flavor	= gss_mech_info2flavor,
	.flavor2info	= gss_mech_flavor2info,
};

static const struct rpc_credops gss_credops = {
	.cr_name		= "AUTH_GSS",
	.crdestroy		= gss_destroy_cred,
	.cr_init		= gss_cred_init,
	.crbind			= rpcauth_generic_bind_cred,
	.crmatch		= gss_match,
	.crmarshal		= gss_marshal,
	.crrefresh		= gss_refresh,
	.crvalidate		= gss_validate,
	.crwrap_req		= gss_wrap_req,
	.crunwrap_resp		= gss_unwrap_resp,
	.crkey_timeout		= gss_key_timeout,
	.crstringify_acceptor	= gss_stringify_acceptor,
};

static const struct rpc_credops gss_nullops = {
	.cr_name		= "AUTH_GSS",
	.crdestroy		= gss_destroy_nullcred,
	.crbind			= rpcauth_generic_bind_cred,
	.crmatch		= gss_match,
	.crmarshal		= gss_marshal,
	.crrefresh		= gss_refresh_null,
	.crvalidate		= gss_validate,
	.crwrap_req		= gss_wrap_req,
	.crunwrap_resp		= gss_unwrap_resp,
	.crstringify_acceptor	= gss_stringify_acceptor,
};

static const struct rpc_pipe_ops gss_upcall_ops_v0 = {
	.upcall		= rpc_pipe_generic_upcall,
	.downcall	= gss_pipe_downcall,
	.destroy_msg	= gss_pipe_destroy_msg,
	.open_pipe	= gss_pipe_open_v0,
	.release_pipe	= gss_pipe_release,
};

static const struct rpc_pipe_ops gss_upcall_ops_v1 = {
	.upcall		= rpc_pipe_generic_upcall,
	.downcall	= gss_pipe_downcall,
	.destroy_msg	= gss_pipe_destroy_msg,
	.open_pipe	= gss_pipe_open_v1,
	.release_pipe	= gss_pipe_release,
};

static __net_init int rpcsec_gss_init_net(struct net *net)
{
	return gss_svc_init_net(net);
}

static __net_exit void rpcsec_gss_exit_net(struct net *net)
{
	gss_svc_shutdown_net(net);
}

static struct pernet_operations rpcsec_gss_net_ops = {
	.init = rpcsec_gss_init_net,
	.exit = rpcsec_gss_exit_net,
};

/*
 * Initialize RPCSEC_GSS module
 */
static int __init init_rpcsec_gss(void)
{
	int err = 0;

	err = rpcauth_register(&authgss_ops);
	if (err)
		goto out;
	err = gss_svc_init();
	if (err)
		goto out_unregister;
	err = register_pernet_subsys(&rpcsec_gss_net_ops);
	if (err)
		goto out_svc_exit;
	rpc_init_wait_queue(&pipe_version_rpc_waitqueue, "gss pipe version");
	return 0;
out_svc_exit:
	gss_svc_shutdown();
out_unregister:
	rpcauth_unregister(&authgss_ops);
out:
	return err;
}

static void __exit exit_rpcsec_gss(void)
{
	unregister_pernet_subsys(&rpcsec_gss_net_ops);
	gss_svc_shutdown();
	rpcauth_unregister(&authgss_ops);
	rcu_barrier(); /* Wait for completion of call_rcu()'s */
}

MODULE_ALIAS("rpc-auth-6");
MODULE_LICENSE("GPL");
module_param_named(expired_cred_retry_delay,
		   gss_expired_cred_retry_delay,
		   uint, 0644);
MODULE_PARM_DESC(expired_cred_retry_delay, "Timeout (in seconds) until "
		"the RPC engine retries an expired credential");

module_param_named(key_expire_timeo,
		   gss_key_expire_timeo,
		   uint, 0644);
MODULE_PARM_DESC(key_expire_timeo, "Time (in seconds) at the end of a "
		"credential keys lifetime where the NFS layer cleans up "
		"prior to key expiration");

module_init(init_rpcsec_gss)
module_exit(exit_rpcsec_gss)
