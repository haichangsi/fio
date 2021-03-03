/*
 * librpma_aof: librpma AOF engine (XXX)
 *
 * Copyright 2021, Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2 as published by the Free Software Foundation..
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "librpma_fio.h"

#include <libpmem.h>

/* Generated by the protocol buffer compiler from: librpma_aof_update.proto */
#include "librpma_aof_update.pb-c.h"

#define MAX_MSG_SIZE	(512)
#define IO_U_BUF_LEN	(2 * MAX_MSG_SIZE)
#define SEND_OFFSET	(0)
#define RECV_OFFSET	(SEND_OFFSET + MAX_MSG_SIZE)

#define AOF_PTR_SIZE 	(sizeof(uint64_t))

/*
 * 'Update_req_last' is the last update request the client has to send to
 * the server to indicate that the client is done.
 */
static const AOFUpdateRequest Update_req_last = AOF_UPDATE_REQUEST__INIT;

#define IS_NOT_THE_LAST_MESSAGE(update_req) \
	(memcmp(update_req, &Update_req_last, sizeof(Update_req_last)) != 0)

struct fio_option librpma_aof_options[] = {
	LIBRPMA_FIO_OPTIONS_COMMON,
	{
		.name	= "mode",
		.lname	= LIBRPMA_AOF_MODE_LNAME,
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct librpma_fio_options_values, aof_mode),
		.help	= LIBRPMA_AOF_MODE_HELP,
		.posval = {
			{
				.ival = "sw",
				.oval = LIBRPMA_AOF_MODE_SW,
				.help = LIBRPMA_AOF_MODE_SW_HELP,
			},
			{
				.ival = "hw",
				.oval = LIBRPMA_AOF_MODE_HW,
				.help = LIBRPMA_AOF_MODE_HW_HELP,
			},
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBRPMA,
	},
	{
		.name	= NULL,
	},
};

/* client side implementation */

/* get next io_u message buffer in the round-robin fashion */
#define IO_U_NEXT_BUF_OFF_CLIENT(cd) \
	(IO_U_BUF_LEN * ((cd->msg_curr++) % cd->msg_num))

struct client_data_sw {
	/* the messaging buffer (sending and receiving) */
	char *io_us_msgs;

	/* resources for the messaging buffer */
	uint32_t msg_num;
	uint32_t msg_curr;
	struct rpma_mr_local *msg_mr;
};

struct client_data_hw {
	/* AOF pointer */
	uint64_t *aof_ptr;

	/* AOF pointer base address memory registration */
	struct rpma_mr_local *aof_ptr_mr;
};

static int client_sw_init(struct thread_data *td);
static void client_sw_cleanup(struct thread_data *td);

static int client_hw_init(struct thread_data *td);
static void client_hw_cleanup(struct thread_data *td);

static int client_init(struct thread_data *td)
{
	struct librpma_fio_options_values *o = td->eo;
	int ret = -1;

	/* only sequential writes are allowed in AOF */
	if (td_random(td) || td_read(td) || td_trim(td)) {
		td_verror(td, EINVAL,
			"Not supported mode (only sequential writes are allowed in AOF).");
		return -1;
	}

	librpma_td_log(td, LIBRPMA_AOF_MODE_HELP "\n");

	if (o->aof_mode == LIBRPMA_AOF_MODE_SW) {
		if ((ret = client_sw_init(td)))
			return ret;

		td->io_ops->cleanup = client_sw_cleanup;

		librpma_td_log(td,
			LIBRPMA_AOF_MODE_LNAME ": " LIBRPMA_AOF_MODE_SW_HELP "\n");
	} else { /* LIBRPMA_AOF_MODE_HW */
		if ((ret = client_hw_init(td)))
			return ret;

		td->io_ops->cleanup = client_hw_cleanup;

		librpma_td_log(td,
			LIBRPMA_AOF_MODE_LNAME ": " LIBRPMA_AOF_MODE_HW_HELP "\n");
	}

	return 0;
}

static int client_sw_io_append(struct thread_data *td,
	struct io_u *first_io_u, struct io_u *last_io_u,
	unsigned long long int len);

static int client_sw_get_io_u_index(struct rpma_completion *cmpl,
	unsigned int *io_u_index);

static int client_sw_init(struct thread_data *td)
{
	struct librpma_fio_client_data *ccd;
	struct client_data_sw *cd;
	uint32_t write_num;
	struct rpma_conn_cfg *cfg = NULL;
	unsigned int io_us_msgs_size;
	int ret;

	/* allocate client's data */
	cd = calloc(1, sizeof(*cd));
	if (cd == NULL) {
		td_verror(td, errno, "calloc");
		return -1;
	}

	/*
	 * Calculate the required number of WRITEs and UPDATEes.
	 *
	 * Note: Each update is a request (SEND) and response (RECV) pair.
	 */
	if (td->o.sync_io) {
		write_num = 1; /* WRITE */
		cd->msg_num = 1; /* AOF update */
	} else {
		write_num = td->o.iodepth; /* WRITE * N */
		/*
		 * AOF update * B where:
		 * - B == ceil(iodepth / iodepth_batch)
		 *   which is the number of batches for N writes
		 */
		cd->msg_num = LIBRPMA_FIO_CEIL(td->o.iodepth,
				td->o.iodepth_batch);
	}

	/* create a connection configuration object */
	if ((ret = rpma_conn_cfg_new(&cfg))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_new");
		goto err_free_cd;
	}

	/*
	 * Calculate the required queue sizes where:
	 * - the send queue (SQ) has to be big enough to accommodate
	 *   all io_us (WRITEs) and all AOF update requests (SENDs)
	 * - the receive queue (RQ) has to be big enough to accommodate
	 *   all AOF update responses (RECVs)
	 * - the completion queue (CQ) has to be big enough to accommodate all
	 *   success and error completions (sq_size + rq_size)
	 */
	if ((ret = rpma_conn_cfg_set_sq_size(cfg, write_num + cd->msg_num))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_sq_size");
		goto err_cfg_delete;
	}
	if ((ret = rpma_conn_cfg_set_rq_size(cfg, cd->msg_num))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_rq_size");
		goto err_cfg_delete;
	}
	if ((ret = rpma_conn_cfg_set_cq_size(cfg, write_num + cd->msg_num * 2))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_cq_size");
		goto err_cfg_delete;
	}

	if (librpma_fio_client_init(td, cfg))
		goto err_cfg_delete;

	ccd = td->io_ops_data;

	/* validate the server's RQ capacity */
	if (cd->msg_num > ccd->ws->max_msg_num) {
		log_err(
			"server's RQ size (iodepth) too small to handle the client's workspace requirements (%u < %u)\n",
			ccd->ws->max_msg_num, cd->msg_num);
		goto err_cleanup_common;
	}

	/* message buffers initialization and registration */
	io_us_msgs_size = cd->msg_num * IO_U_BUF_LEN;
	if ((ret = posix_memalign((void **)&cd->io_us_msgs, page_size,
			io_us_msgs_size))) {
		td_verror(td, ret, "posix_memalign");
		goto err_cleanup_common;
	}

	if ((ret = rpma_mr_reg(ccd->peer, cd->io_us_msgs, io_us_msgs_size,
			RPMA_MR_USAGE_SEND | RPMA_MR_USAGE_RECV,
			&cd->msg_mr))) {
		librpma_td_verror(td, ret, "rpma_mr_reg");
		goto err_free_io_us_msgs;
	}

	if ((ret = rpma_conn_cfg_delete(&cfg))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_delete");
		/* non fatal error - continue */
	}

	ccd->flush = client_sw_io_append;
	ccd->get_io_u_index = client_sw_get_io_u_index;
	ccd->client_data = cd;

	return 0;

err_free_io_us_msgs:
	free(cd->io_us_msgs);

err_cleanup_common:
	librpma_fio_client_cleanup(td);

err_cfg_delete:
	(void) rpma_conn_cfg_delete(&cfg);

err_free_cd:
	free(cd);

	return -1;
}

static int client_hw_io_append(struct thread_data *td,
	struct io_u *first_io_u, struct io_u *last_io_u,
	unsigned long long int len);

static int client_hw_get_io_u_index(struct rpma_completion *cmpl,
	unsigned int *io_u_index);

static int client_hw_init(struct thread_data *td)
{
	struct librpma_fio_client_data *ccd;
	struct rpma_conn_cfg *cfg = NULL;
	struct rpma_peer_cfg *pcfg = NULL;
	struct client_data_hw *cd;
	uint32_t write_num;
	uint32_t update_num;
	int ret;

	/* allocate client's data */
	cd = calloc(1, sizeof(*cd));
	if (cd == NULL) {
		td_verror(td, errno, "calloc");
		return -1;
	}

	/*
	 * Calculate the required number of WRITEs and AOF updates.
	 *
	 * Note: each AOF update is a sequence of FLUSH + ATOMIC_WRITE + FLUSH.
	 */
	if (td->o.sync_io) {
		write_num = 1; /* WRITE */
		update_num = 1; /* AOF update */
	} else {
		write_num = td->o.iodepth; /* WRITE * N */
		/*
		 * AOF update * B where:
		 * - B == ceil(iodepth / iodepth_batch)
		 *   which is the number of batches for N writes
		 */
		update_num = LIBRPMA_FIO_CEIL(td->o.iodepth,
				td->o.iodepth_batch);
	}

	/* create a connection configuration object */
	if ((ret = rpma_conn_cfg_new(&cfg))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_new");
		goto err_free_cd;
	}

	/*
	 * Calculate the required queue sizes where:
	 * - the send queue (SQ) has to be big enough to accommodate
	 *   all io_us (WRITEs) and all AOF updates (FLUSH + ATOMIC_WRITE + FLUSH)
	 * - the receive queue (RQ) is not used
	 * - the completion queue (CQ) has to be big enough to accommodate all
	 *   success and error completions (sq_size)
	 */
	if ((ret = rpma_conn_cfg_set_sq_size(cfg, write_num + 3 * update_num))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_sq_size");
		goto err_cfg_delete;
	}
	if ((ret = rpma_conn_cfg_set_rq_size(cfg, 0))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_rq_size");
		goto err_cfg_delete;
	}
	if ((ret = rpma_conn_cfg_set_cq_size(cfg, write_num + 3 * update_num))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_cq_size");
		goto err_cfg_delete;
	}

	if (librpma_fio_client_init(td, cfg))
		goto err_cfg_delete;

	ccd = td->io_ops_data;

	if (ccd->server_mr_flush_type == RPMA_FLUSH_TYPE_PERSISTENT) {
		if (!ccd->ws->direct_write_to_pmem) {
			librpma_td_log(td,
					"Fio librpma engine will not work in the 'hw` mode until the Direct Write to PMem on the server side is possible (direct_write_to_pmem)\n");
			goto err_cleanup_common;
		}

		/* configure peer's direct write to pmem support */
		if ((ret = rpma_peer_cfg_new(&pcfg))) {
			librpma_td_verror(td, ret, "rpma_peer_cfg_new");
			goto err_cleanup_common;
		}

		if ((ret = rpma_peer_cfg_set_direct_write_to_pmem(pcfg, true))) {
			librpma_td_verror(td, ret,
				"rpma_peer_cfg_set_direct_write_to_pmem");
			(void) rpma_peer_cfg_delete(&pcfg);
			goto err_cleanup_common;
		}

		if ((ret = rpma_conn_apply_remote_peer_cfg(ccd->conn, pcfg))) {
			librpma_td_verror(td, ret,
				"rpma_conn_apply_remote_peer_cfg");
			(void) rpma_peer_cfg_delete(&pcfg);
			goto err_cleanup_common;
		}

		(void) rpma_peer_cfg_delete(&pcfg);
	} else {
		librpma_td_log(td,
			"Note: Direct Write to PMem is not supported by default nor required if you use DRAM instead of PMem on the server side (direct_write_to_pmem).\n"
			"Remember that flushing to DRAM does not make your data persistent and may be used only for experimental purposes.\n");
	}

	/* AOF pointer buffer initialization */
	if ((ret = posix_memalign((void **)&cd->aof_ptr, page_size, AOF_PTR_SIZE))) {
		td_verror(td, ret, "posix_memalign");
		goto err_cleanup_common;
	}

	/* AOF pointer buffer registration */
	if ((ret = rpma_mr_reg(ccd->peer, cd->aof_ptr, AOF_PTR_SIZE,
			RPMA_MR_USAGE_WRITE_SRC, &cd->aof_ptr_mr))) {
		librpma_td_verror(td, ret, "rpma_mr_reg");
		goto err_free_aof_ptr;
	}

	if ((ret = rpma_conn_cfg_delete(&cfg))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_delete");
		/* non fatal error - continue */
	}

	ccd->flush = client_hw_io_append;
	ccd->get_io_u_index = client_hw_get_io_u_index;
	ccd->client_data = cd;

	return 0;

err_free_aof_ptr:
	free(cd->aof_ptr);

err_cleanup_common:
	librpma_fio_client_cleanup(td);

err_cfg_delete:
	(void) rpma_conn_cfg_delete(&cfg);

err_free_cd:
	free(cd);

	return -1;
}

static int client_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct librpma_fio_client_data *ccd = td->io_ops_data;

	/* reserve space for the AOF pointer */
	ccd->ws_size -= AOF_PTR_SIZE;

	f->real_file_size = ccd->ws_size;
	fio_file_set_size_known(f);

	return 0;
}

static void client_sw_cleanup(struct thread_data *td)
{
	struct librpma_fio_client_data *ccd = td->io_ops_data;
	struct client_data_sw *cd;
	size_t update_req_size;
	size_t io_u_buf_off;
	size_t send_offset;
	void *send_ptr;
	int ret;

	if (ccd == NULL)
		return;

	cd = ccd->client_data;
	if (cd == NULL) {
		librpma_fio_client_cleanup(td);
		return;
	}

	/*
	 * Make sure all SEND completions are collected ergo there are free
	 * slots in the SQ for the last SEND message.
	 *
	 * Note: If any operation will fail we still can send the termination
	 * notice.
	 */
	(void) librpma_fio_client_io_complete_all_sends(td);

	/* prepare the last update message and pack it to the send buffer */
	update_req_size = aof_update_request__get_packed_size(&Update_req_last);
	if (update_req_size > MAX_MSG_SIZE) {
		log_err(
			"Packed update request size is bigger than available send buffer space (%zu > %d\n",
			update_req_size, MAX_MSG_SIZE);
	} else {
		io_u_buf_off = IO_U_NEXT_BUF_OFF_CLIENT(cd);
		send_offset = io_u_buf_off + SEND_OFFSET;
		send_ptr = cd->io_us_msgs + send_offset;
		(void) aof_update_request__pack(&Update_req_last, send_ptr);

		/* send the update message */
		if ((ret = rpma_send(ccd->conn, cd->msg_mr, send_offset,
				update_req_size, RPMA_F_COMPLETION_ALWAYS,
				NULL)))
			librpma_td_verror(td, ret, "rpma_send");

		++ccd->op_send_posted;

		/* Wait for the SEND to complete */
		(void) librpma_fio_client_io_complete_all_sends(td);
	}

	/* deregister the messaging buffer memory */
	if ((ret = rpma_mr_dereg(&cd->msg_mr)))
		librpma_td_verror(td, ret, "rpma_mr_dereg");

	free(ccd->client_data);

	librpma_fio_client_cleanup(td);
}

static int client_sw_io_append(struct thread_data *td,
		struct io_u *first_io_u, struct io_u *last_io_u,
		unsigned long long int len)
{
	struct librpma_fio_client_data *ccd = td->io_ops_data;
	struct client_data_sw *cd = ccd->client_data;
	size_t io_u_buf_off = IO_U_NEXT_BUF_OFF_CLIENT(cd);
	size_t send_offset = io_u_buf_off + SEND_OFFSET;
	size_t recv_offset = io_u_buf_off + RECV_OFFSET;
	void *send_ptr = cd->io_us_msgs + send_offset;
	void *recv_ptr = cd->io_us_msgs + recv_offset;
	AOFUpdateRequest update_req = AOF_UPDATE_REQUEST__INIT;
	size_t update_req_size = 0;
	int ret;

	/* prepare a response buffer */
	if ((ret = rpma_recv(ccd->conn, cd->msg_mr, recv_offset, MAX_MSG_SIZE,
			recv_ptr))) {
		librpma_td_verror(td, ret, "rpma_recv");
		return -1;
	}

	/* prepare the AOF update message and pack it to a send buffer */
	update_req.append_offset = first_io_u->offset;
	update_req.append_length = len;
	update_req.pointer_offset = ccd->ws_size; /* AOF pointer */
	update_req.op_context = last_io_u->index;
	update_req_size = aof_update_request__get_packed_size(&update_req);
	if (update_req_size > MAX_MSG_SIZE) {
		log_err(
			"Packed AOF update request size is bigger than available send buffer space (%"
			PRIu64 " > %d\n", update_req_size, MAX_MSG_SIZE);
		return -1;
	}
	(void) aof_update_request__pack(&update_req, send_ptr);

	/* send the AOF update message */
	if ((ret = rpma_send(ccd->conn, cd->msg_mr, send_offset, update_req_size,
			RPMA_F_COMPLETION_ALWAYS, NULL))) {
		librpma_td_verror(td, ret, "rpma_send");
		return -1;
	}

	++ccd->op_send_posted;

	return 0;
}

static int client_sw_get_io_u_index(struct rpma_completion *cmpl,
		unsigned int *io_u_index)
{
	AOFUpdateResponse *update_resp;

	if (cmpl->op != RPMA_OP_RECV)
		return 0;

	/* unpack a response from the received buffer */
	update_resp = aof_update_response__unpack(NULL,
			cmpl->byte_len, cmpl->op_context);
	if (update_resp == NULL) {
		log_err("Cannot unpack the update response buffer\n");
		return -1;
	}

	memcpy(io_u_index, &update_resp->op_context, sizeof(*io_u_index));

	aof_update_response__free_unpacked(update_resp, NULL);

	return 1;
}

static void client_hw_cleanup(struct thread_data *td)
{
	struct librpma_fio_client_data *ccd = td->io_ops_data;
	struct client_data_hw *cd;
	int ret;

	if (ccd == NULL)
		return;

	cd = ccd->client_data;
	if (cd == NULL) {
		librpma_fio_client_cleanup(td);
		return;
	}

	/* deregister the AOF pointer memory */
	if ((ret = rpma_mr_dereg(&cd->aof_ptr_mr)))
		librpma_td_verror(td, ret, "rpma_mr_dereg");

	/* free the AOF pointer's memory */
	free(cd->aof_ptr);

	free(ccd->client_data);

	librpma_fio_client_cleanup(td);
}

static int client_hw_io_append(struct thread_data *td,
		struct io_u *first_io_u, struct io_u *last_io_u,
		unsigned long long int len)
{
	struct librpma_fio_client_data *ccd = td->io_ops_data;
	struct client_data_hw *cd = ccd->client_data;
	uint64_t *pointer_ptr;
	size_t src_offset;
	size_t dst_offset;
	int ret;

	/* flush the appended data */
	if ((ret = rpma_flush(ccd->conn, ccd->server_mr, first_io_u->offset, len,
			ccd->server_mr_flush_type, RPMA_F_COMPLETION_ON_ERROR,
			(void *)(uintptr_t)last_io_u->index))) {
		librpma_td_verror(td, ret, "rpma_flush");
		return -1;
	}

	/* update the pointer */
	dst_offset = ccd->ws_size; /* destination offset of the AOF pointer */
	src_offset = 0; /* source offset of the AOF pointer */
	pointer_ptr = cd->aof_ptr; /* address of the AOF pointer */
	*pointer_ptr = first_io_u->offset + len; /* value of the AOF pointer */

	if ((ret = rpma_write_atomic(ccd->conn, ccd->server_mr, dst_offset,
			cd->aof_ptr_mr, src_offset, RPMA_F_COMPLETION_ON_ERROR,
			(void *)(uintptr_t)last_io_u->index))) {
		librpma_td_verror(td, ret, "rpma_write_atomic");
		return -1;
	}

	/* flush the AOF pointer */
	if ((ret = rpma_flush(ccd->conn, ccd->server_mr, dst_offset, AOF_PTR_SIZE,
			ccd->server_mr_flush_type, RPMA_F_COMPLETION_ALWAYS,
			(void *)(uintptr_t)last_io_u->index))) {
		librpma_td_verror(td, ret, "rpma_flush");
		return -1;
	}

	return 0;
}

static int client_hw_get_io_u_index(struct rpma_completion *cmpl,
		unsigned int *io_u_index)
{
	*io_u_index = (unsigned)(uintptr_t)cmpl->op_context;

	return 1;
}

FIO_STATIC struct ioengine_ops ioengine_client = {
	.name			= "librpma_aof_client",
	.version		= FIO_IOOPS_VERSION,
	.init			= client_init,
	.post_init		= librpma_fio_client_post_init,
	.get_file_size		= client_get_file_size,
	.open_file		= librpma_fio_file_nop,
	.queue			= librpma_fio_client_queue,
	.commit			= librpma_fio_client_commit,
	.getevents		= librpma_fio_client_getevents,
	.event			= librpma_fio_client_event,
	.errdetails		= librpma_fio_client_errdetails,
	.close_file		= librpma_fio_file_nop,
	.cleanup		= NULL, /* see the (*) notice below */
	.flags			= FIO_DISKLESSIO,
	.options		= librpma_aof_options,
	.option_struct_size	= sizeof(struct librpma_fio_options_values),

	/*
	 * (*) the actual implementation is picked in the client_init hook
	 *     according to the chosen aof_mode value
	 */
};

/* server side implementation */

#define IO_U_BUFF_OFF_SERVER(i) (i * IO_U_BUF_LEN)

struct server_data {
	/* aligned td->orig_buffer - the messaging buffer (sending and receiving) */
	char *orig_buffer_aligned;

	/* resources for the messaging buffer */
	struct rpma_mr_local *msg_mr;

	uint32_t msg_sqe_available; /* # of free SQ slots */

	/* in-memory queues */
	struct rpma_completion *msgs_queued;
	uint32_t msg_queued_nr;
};

static int server_sw_init(struct thread_data *td);
static int server_sw_post_init(struct thread_data *td);
static int server_sw_open_file(struct thread_data *td, struct fio_file *f);
static enum fio_q_status server_sw_queue(struct thread_data *td,
		struct io_u *io_u);
static void server_sw_cleanup(struct thread_data *td);

static int server_hw_open_file(struct thread_data *td, struct fio_file *f);
static enum fio_q_status server_hw_queue(struct thread_data *td,
		struct io_u *io_u);

static int server_init(struct thread_data *td)
{
	struct librpma_fio_options_values *o = td->eo;
	int ret = -1;

	if ((ret = librpma_fio_server_init(td)))
		return ret;

	librpma_td_log(td, LIBRPMA_AOF_MODE_HELP "\n");

	if (o->aof_mode == LIBRPMA_AOF_MODE_SW) {
		if ((ret = server_sw_init(td))) {
			librpma_fio_server_cleanup(td);
			return ret;
		}

		td->io_ops->post_init = server_sw_post_init;
		td->io_ops->open_file = server_sw_open_file;
		td->io_ops->queue = server_sw_queue;
		td->io_ops->cleanup = server_sw_cleanup;

		librpma_td_log(td,
			LIBRPMA_AOF_MODE_LNAME ": " LIBRPMA_AOF_MODE_SW_HELP "\n");
	} else { /* LIBRPMA_AOF_MODE_HW */
		td->io_ops->open_file = server_hw_open_file;
		td->io_ops->queue = server_hw_queue;
		td->io_ops->cleanup = librpma_fio_server_cleanup;

		librpma_td_log(td,
			LIBRPMA_AOF_MODE_LNAME ": " LIBRPMA_AOF_MODE_HW_HELP "\n");
	}

	return 0;
}

static int server_sw_init(struct thread_data *td)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct server_data *sd;

	/* allocate server's data */
	sd = calloc(1, sizeof(*sd));
	if (sd == NULL) {
		td_verror(td, errno, "calloc");
		return -1;
	}

	/* allocate in-memory queue */
	sd->msgs_queued = calloc(td->o.iodepth, sizeof(*sd->msgs_queued));
	if (sd->msgs_queued == NULL) {
		td_verror(td, errno, "calloc");
		free(sd);
		return -1;
	}

	/*
	 * Assure a single io_u buffer can store both SEND and RECV messages and
	 * an io_us buffer allocation is page-size-aligned which is required
	 * to register for RDMA. User-provided values are intentionally ignored.
	 */
	td->o.max_bs[DDIR_READ] = IO_U_BUF_LEN;
	td->o.mem_align = page_size;

	csd->server_data = sd;

	return 0;
}

static int server_sw_post_init(struct thread_data *td)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct server_data *sd = csd->server_data;
	size_t io_us_size;
	size_t io_u_buflen;
	int ret;

	/*
	 * td->orig_buffer is not aligned. The engine requires aligned io_us
	 * so FIO alignes up the address using the formula below.
	 */
	sd->orig_buffer_aligned = PTR_ALIGN(td->orig_buffer, page_mask) +
			td->o.mem_align;

	/*
	 * XXX
	 * Each io_u message buffer contains recv and send messages.
	 * Aligning each of those buffers may potentially give
	 * some performance benefits.
	 */
	io_u_buflen = td_max_bs(td);

	/* check whether io_u buffer is big enough */
	if (io_u_buflen < IO_U_BUF_LEN) {
		log_err(
			"blocksize too small to accommodate assumed maximal request/response pair size (%" PRIu64 " < %d)\n",
			io_u_buflen, IO_U_BUF_LEN);
		return -1;
	}

	/*
	 * td->orig_buffer_size beside the space really consumed by io_us
	 * has paddings which can be omitted for the memory registration.
	 */
	io_us_size = (unsigned long long)io_u_buflen *
			(unsigned long long)td->o.iodepth;

	if ((ret = rpma_mr_reg(csd->peer, sd->orig_buffer_aligned, io_us_size,
			RPMA_MR_USAGE_SEND | RPMA_MR_USAGE_RECV,
			&sd->msg_mr))) {
		librpma_td_verror(td, ret, "rpma_mr_reg");
		return -1;
	}

	return 0;
}

static void server_sw_cleanup(struct thread_data *td)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct server_data *sd;
	int ret;

	if (csd == NULL)
		return;

	sd = csd->server_data;

	if (sd != NULL) {
		/* rpma_mr_dereg(messaging buffer from DRAM) */
		if ((ret = rpma_mr_dereg(&sd->msg_mr)))
			librpma_td_verror(td, ret, "rpma_mr_dereg");

		free(sd->msgs_queued);
		free(sd);
	}

	librpma_fio_server_cleanup(td);
}

static int server_sw_prepare_connection(struct thread_data *td,
		struct rpma_conn_req *conn_req)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct server_data *sd = csd->server_data;
	int ret;
	int i;

	/* prepare buffers for update requests */
	sd->msg_sqe_available = td->o.iodepth;
	for (i = 0; i < td->o.iodepth; i++) {
		size_t offset_recv_msg = IO_U_BUFF_OFF_SERVER(i) + RECV_OFFSET;
		if ((ret = rpma_conn_req_recv(conn_req, sd->msg_mr,
				offset_recv_msg, MAX_MSG_SIZE,
				(const void *)(uintptr_t)i))) {
			librpma_td_verror(td, ret, "rpma_conn_req_recv");
			return ret;
		}
	}

	return 0;
}

static int server_sw_open_file(struct thread_data *td, struct fio_file *f)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct rpma_conn_cfg *cfg = NULL;
	uint16_t max_msg_num = td->o.iodepth;
	int ret;

	csd->prepare_connection = server_sw_prepare_connection;

	/* create a connection configuration object */
	if ((ret = rpma_conn_cfg_new(&cfg))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_new");
		return -1;
	}

	/*
	 * Calculate the required queue sizes where:
	 * - the send queue (SQ) has to be big enough to accommodate
	 *   all possible update requests (SENDs)
	 * - the receive queue (RQ) has to be big enough to accommodate
	 *   all update responses (RECVs)
	 * - the completion queue (CQ) has to be big enough to accommodate
	 *   all success and error completions (sq_size + rq_size)
	 */
	if ((ret = rpma_conn_cfg_set_sq_size(cfg, max_msg_num))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_sq_size");
		goto err_cfg_delete;
	}
	if ((ret = rpma_conn_cfg_set_rq_size(cfg, max_msg_num))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_rq_size");
		goto err_cfg_delete;
	}
	if ((ret = rpma_conn_cfg_set_cq_size(cfg, max_msg_num * 2))) {
		librpma_td_verror(td, ret, "rpma_conn_cfg_set_cq_size");
		goto err_cfg_delete;
	}

	ret = librpma_fio_server_open_file(td, f, cfg);

err_cfg_delete:
	(void) rpma_conn_cfg_delete(&cfg);

	return ret;
}

static int server_hw_open_file(struct thread_data *td, struct fio_file *f)
{
	return librpma_fio_server_open_file(td, f, NULL);
}

static int server_sw_qe_process(struct thread_data *td,
		struct rpma_completion *cmpl)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct server_data *sd = csd->server_data;
	AOFUpdateRequest *update_req;
	AOFUpdateResponse update_resp = AOF_UPDATE_RESPONSE__INIT;
	size_t update_resp_size = 0;
	size_t send_buff_offset;
	size_t recv_buff_offset;
	size_t io_u_buff_offset;
	void *send_buff_ptr;
	void *recv_buff_ptr;
	char *append_ptr;
	uint64_t *pointer_ptr;
	int msg_index;
	int ret;

	/* calculate SEND/RECV pair parameters */
	msg_index = (int)(uintptr_t)cmpl->op_context;
	io_u_buff_offset = IO_U_BUFF_OFF_SERVER(msg_index);
	send_buff_offset = io_u_buff_offset + SEND_OFFSET;
	recv_buff_offset = io_u_buff_offset + RECV_OFFSET;
	send_buff_ptr = sd->orig_buffer_aligned + send_buff_offset;
	recv_buff_ptr = sd->orig_buffer_aligned + recv_buff_offset;

	/* unpack an update request from the received buffer */
	update_req = aof_update_request__unpack(NULL, cmpl->byte_len,
			recv_buff_ptr);
	if (update_req == NULL) {
		log_err("cannot unpack the update request buffer\n");
		goto err_terminate;
	}

	if (IS_NOT_THE_LAST_MESSAGE(update_req)) {
		/* persist the append */
		append_ptr = csd->ws_ptr + update_req->append_offset;
		pmem_persist(append_ptr, update_req->append_length);

		/* update and persist the pointer */
		pointer_ptr = (uint64_t *)(csd->ws_ptr +
				update_req->pointer_offset);
		*pointer_ptr = update_req->append_offset +
				update_req->append_length;
		pmem_persist(pointer_ptr, sizeof(*pointer_ptr));
	} else {
		/*
		 * This is the last message - the client is done.
		 */
		aof_update_request__free_unpacked(update_req, NULL);
		td->done = true;
		return 0;
	}

	/* initiate the next receive operation */
	if ((ret = rpma_recv(csd->conn, sd->msg_mr, recv_buff_offset,
			MAX_MSG_SIZE,
			(const void *)(uintptr_t)msg_index))) {
		librpma_td_verror(td, ret, "rpma_recv");
		goto err_free_unpacked;
	}

	/* prepare an update response and pack it to a send buffer */
	update_resp.op_context = update_req->op_context;
	update_resp_size = aof_update_response__get_packed_size(&update_resp);
	if (update_resp_size > MAX_MSG_SIZE) {
		log_err(
			"Size of the packed update response is bigger than the available space of the send buffer (%"
			PRIu64 " > %i\n", update_resp_size, MAX_MSG_SIZE);
		goto err_free_unpacked;
	}

	(void) aof_update_response__pack(&update_resp, send_buff_ptr);

	/* send the update response */
	if ((ret = rpma_send(csd->conn, sd->msg_mr, send_buff_offset,
			update_resp_size, RPMA_F_COMPLETION_ALWAYS, NULL))) {
		librpma_td_verror(td, ret, "rpma_send");
		goto err_free_unpacked;
	}
	--sd->msg_sqe_available;

	aof_update_request__free_unpacked(update_req, NULL);

	return 0;

err_free_unpacked:
	aof_update_request__free_unpacked(update_req, NULL);

err_terminate:
	td->terminate = true;

	return -1;
}

static inline int server_sw_queue_process(struct thread_data *td)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct server_data *sd = csd->server_data;
	int ret;
	int i;

	/* min(# of queue entries, # of SQ entries available) */
	uint32_t qes_to_process = min(sd->msg_queued_nr, sd->msg_sqe_available);
	if (qes_to_process == 0)
		return 0;

	/* process queued completions */
	for (i = 0; i < qes_to_process; ++i) {
		if ((ret = server_sw_qe_process(td, &sd->msgs_queued[i])))
			return ret;
	}

	/* progress the queue */
	for (i = 0; i < sd->msg_queued_nr - qes_to_process; ++i) {
		memcpy(&sd->msgs_queued[i],
			&sd->msgs_queued[qes_to_process + i],
			sizeof(sd->msgs_queued[i]));
	}

	sd->msg_queued_nr -= qes_to_process;

	return 0;
}

static int server_sw_cmpl_process(struct thread_data *td)
{
	struct librpma_fio_server_data *csd = td->io_ops_data;
	struct server_data *sd = csd->server_data;
	struct rpma_completion *cmpl = &sd->msgs_queued[sd->msg_queued_nr];
	int ret;

	ret = rpma_conn_completion_get(csd->conn, cmpl);
	if (ret == RPMA_E_NO_COMPLETION) {
		/* lack of completion is not an error */
		return 0;
	} else if (ret != 0) {
		librpma_td_verror(td, ret, "rpma_conn_completion_get");
		goto err_terminate;
	}

	/* validate the completion */
	if (cmpl->op_status != IBV_WC_SUCCESS)
		goto err_terminate;

	if (cmpl->op == RPMA_OP_RECV)
		++sd->msg_queued_nr;
	else if (cmpl->op == RPMA_OP_SEND)
		++sd->msg_sqe_available;

	return 0;

err_terminate:
	td->terminate = true;

	return -1;
}

static enum fio_q_status server_queue_temp(struct thread_data *td,
		struct io_u *io_u)
{
	/*
	 * The actual implementation is picked in the server_init hook
	 * according to the chosen aof_mode value
	 */
	return -1;
}

static enum fio_q_status server_sw_queue(struct thread_data *td,
		struct io_u *io_u)
{
	do {
		if (server_sw_cmpl_process(td))
			return FIO_Q_BUSY;

		if (server_sw_queue_process(td))
			return FIO_Q_BUSY;

	} while (!td->done);

	return FIO_Q_COMPLETED;
}

static enum fio_q_status server_hw_queue(struct thread_data *td,
		struct io_u *io_u)
{
	return FIO_Q_COMPLETED;
}

FIO_STATIC struct ioengine_ops ioengine_server = {
	.name			= "librpma_aof_server",
	.version		= FIO_IOOPS_VERSION,
	.init			= server_init,
	.post_init		= NULL, /* see the (*) notice below */
	.open_file		= NULL, /* see the (*) notice below */
	.close_file		= librpma_fio_server_close_file,
	.queue			= server_queue_temp, /* see the (*) notice below */
	.invalidate		= librpma_fio_file_nop,
	.cleanup		= NULL, /* see the (*) notice below */
	.flags			= FIO_SYNCIO,
	.options		= librpma_aof_options,
	.option_struct_size	= sizeof(struct librpma_fio_options_values),

	/*
	 * (*) the actual implementation is picked in the server_init hook
	 *     according to the chosen aof_mode value
	 */
};

/* register both engines */

static void fio_init fio_librpma_aof_register(void)
{
	register_ioengine(&ioengine_client);
	register_ioengine(&ioengine_server);
}

static void fio_exit fio_librpma_aof_unregister(void)
{
	unregister_ioengine(&ioengine_client);
	unregister_ioengine(&ioengine_server);
}
