/*
 * Copyright (c) 2015 Jiri Svoboda
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup volsrv
 * @{
 */
/**
 * @file Volume service
 */

#include <async.h>
#include <errno.h>
#include <str_error.h>
#include <io/log.h>
#include <ipc/services.h>
#include <ipc/vol.h>
#include <loc.h>
#include <macros.h>
#include <stdio.h>
#include <stdlib.h>
#include <task.h>
#include <types/vol.h>

#include "mkfs.h"
#include "part.h"
#include "volume.h"

#define NAME  "volsrv"

static void vol_client_conn(ipc_call_t *, void *);

static errno_t vol_init(void)
{
	errno_t rc;
	vol_volumes_t *volumes = NULL;
	vol_parts_t *parts = NULL;

	log_msg(LOG_DEFAULT, LVL_DEBUG, "vol_init()");

	rc = vol_volumes_create(&volumes);
	if (rc != EOK)
		goto error;

	rc = vol_parts_create(volumes, &parts);
	if (rc != EOK)
		goto error;

	rc = vol_part_discovery_start(parts);
	if (rc != EOK)
		goto error;

	async_set_fallback_port_handler(vol_client_conn, parts);

	rc = loc_server_register(NAME);
	if (rc != EOK) {
		log_msg(LOG_DEFAULT, LVL_ERROR, "Failed registering server: %s.", str_error(rc));
		rc = EEXIST;
	}

	service_id_t sid;
	rc = loc_service_register(SERVICE_NAME_VOLSRV, &sid);
	if (rc != EOK) {
		log_msg(LOG_DEFAULT, LVL_ERROR, "Failed registering service: %s.", str_error(rc));
		rc = EEXIST;
		goto error;
	}

	return EOK;
error:
	vol_volumes_destroy(volumes);
	vol_parts_destroy(parts);
	return rc;
}

static void vol_get_parts_srv(vol_parts_t *parts, ipc_call_t *icall)
{
	ipc_call_t call;
	size_t size;
	size_t act_size;
	errno_t rc;

	if (!async_data_read_receive(&call, &size)) {
		async_answer_0(&call, EREFUSED);
		async_answer_0(icall, EREFUSED);
		return;
	}

	service_id_t *id_buf = (service_id_t *) malloc(size);
	if (id_buf == NULL) {
		async_answer_0(&call, ENOMEM);
		async_answer_0(icall, ENOMEM);
		return;
	}

	rc = vol_part_get_ids(parts, id_buf, size, &act_size);
	if (rc != EOK) {
		async_answer_0(&call, rc);
		async_answer_0(icall, rc);
		return;
	}

	errno_t retval = async_data_read_finalize(&call, id_buf, size);
	free(id_buf);

	async_answer_1(icall, retval, act_size);
}

static void vol_part_add_srv(vol_parts_t *parts, ipc_call_t *icall)
{
	service_id_t sid;
	errno_t rc;

	sid = IPC_GET_ARG1(*icall);

	rc = vol_part_add_part(parts, sid);
	if (rc != EOK) {
		async_answer_0(icall, rc);
		return;
	}

	async_answer_0(icall, EOK);
}

static void vol_part_info_srv(vol_parts_t *parts, ipc_call_t *icall)
{
	service_id_t sid;
	vol_part_t *part;
	vol_part_info_t pinfo;
	errno_t rc;

	sid = IPC_GET_ARG1(*icall);
	log_msg(LOG_DEFAULT, LVL_DEBUG, "vol_part_info_srv(%zu)",
	    sid);
	rc = vol_part_find_by_id_ref(parts, sid, &part);
	if (rc != EOK) {
		async_answer_0(icall, ENOENT);
		return;
	}

	rc = vol_part_get_info(part, &pinfo);
	if (rc != EOK) {
		async_answer_0(icall, EIO);
		goto error;
	}

	ipc_call_t call;
	size_t size;
	if (!async_data_read_receive(&call, &size)) {
		async_answer_0(&call, EREFUSED);
		async_answer_0(icall, EREFUSED);
		goto error;
	}

	if (size != sizeof(vol_part_info_t)) {
		async_answer_0(&call, EINVAL);
		async_answer_0(icall, EINVAL);
		goto error;
	}

	rc = async_data_read_finalize(&call, &pinfo,
	    min(size, sizeof(pinfo)));
	if (rc != EOK) {
		async_answer_0(&call, rc);
		async_answer_0(icall, rc);
		goto error;
	}

	async_answer_0(icall, EOK);
error:
	vol_part_del_ref(part);
}

static void vol_part_eject_srv(vol_parts_t *parts, ipc_call_t *icall)
{
	service_id_t sid;
	vol_part_t *part;
	errno_t rc;

	sid = IPC_GET_ARG1(*icall);
	log_msg(LOG_DEFAULT, LVL_DEBUG, "vol_part_eject_srv(%zu)", sid);

	rc = vol_part_find_by_id_ref(parts, sid, &part);
	if (rc != EOK) {
		async_answer_0(icall, ENOENT);
		goto error;
	}

	rc = vol_part_eject_part(part);
	if (rc != EOK) {
		async_answer_0(icall, EIO);
		goto error;
	}

	async_answer_0(icall, EOK);
error:
	vol_part_del_ref(part);
}

static void vol_part_empty_srv(vol_parts_t *parts, ipc_call_t *icall)
{
	service_id_t sid;
	vol_part_t *part;
	errno_t rc;

	sid = IPC_GET_ARG1(*icall);
	log_msg(LOG_DEFAULT, LVL_DEBUG, "vol_part_empty_srv(%zu)", sid);

	rc = vol_part_find_by_id_ref(parts, sid, &part);
	if (rc != EOK) {
		async_answer_0(icall, ENOENT);
		return;
	}

	rc = vol_part_empty_part(part);
	if (rc != EOK) {
		async_answer_0(icall, EIO);
		goto error;
	}

	async_answer_0(icall, EOK);
error:
	vol_part_del_ref(part);
}

static void vol_part_get_lsupp_srv(vol_parts_t *parts, ipc_call_t *icall)
{
	vol_fstype_t fstype;
	vol_label_supp_t vlsupp;
	errno_t rc;

	fstype = IPC_GET_ARG1(*icall);
	log_msg(LOG_DEFAULT, LVL_DEBUG, "vol_part_get_lsupp_srv(%u)",
	    fstype);

	volsrv_part_get_lsupp(fstype, &vlsupp);

	ipc_call_t call;
	size_t size;
	if (!async_data_read_receive(&call, &size)) {
		async_answer_0(&call, EREFUSED);
		async_answer_0(icall, EREFUSED);
		return;
	}

	if (size != sizeof(vol_label_supp_t)) {
		async_answer_0(&call, EINVAL);
		async_answer_0(icall, EINVAL);
		return;
	}

	rc = async_data_read_finalize(&call, &vlsupp,
	    min(size, sizeof(vlsupp)));
	if (rc != EOK) {
		async_answer_0(&call, rc);
		async_answer_0(icall, rc);
		return;
	}

	async_answer_0(icall, EOK);
}

static void vol_part_mkfs_srv(vol_parts_t *parts, ipc_call_t *icall)
{
	service_id_t sid;
	vol_part_t *part;
	vol_fstype_t fstype;
	char *label = NULL;
	char *mountp = NULL;
	errno_t rc;

	log_msg(LOG_DEFAULT, LVL_NOTE, "vol_part_mkfs_srv()");

	sid = IPC_GET_ARG1(*icall);
	fstype = IPC_GET_ARG2(*icall);

	rc = async_data_write_accept((void **)&label, true, 0, VOL_LABEL_MAXLEN,
	    0, NULL);
	if (rc != EOK) {
		async_answer_0(icall, rc);
		goto error;
	}

	if (label != NULL) {
		log_msg(LOG_DEFAULT, LVL_NOTE, "vol_part_mkfs_srv: label='%s'",
		    label);
	}

	rc = async_data_write_accept((void **)&mountp, true, 0, VOL_MOUNTP_MAXLEN,
	    0, NULL);
	if (rc != EOK) {
		async_answer_0(icall, rc);
		goto error;
	}

	if (mountp != NULL) {
		log_msg(LOG_DEFAULT, LVL_NOTE, "vol_part_mkfs_srv: mountp='%s'",
		    mountp);
	}

	rc = vol_part_find_by_id_ref(parts, sid, &part);
	if (rc != EOK) {
		async_answer_0(icall, ENOENT);
		goto error;
	}

	rc = vol_part_mkfs_part(part, fstype, label, mountp);
	if (rc != EOK) {
		async_answer_0(icall, rc);
		vol_part_del_ref(part);
		goto error;
	}

	free(label);
	free(mountp);
	async_answer_0(icall, EOK);

	return;
error:
	if (label != NULL)
		free(label);
	if (mountp != NULL)
		free(mountp);
}

static void vol_part_set_mountp_srv(vol_parts_t *parts,
    ipc_call_t *icall)
{
	service_id_t sid;
	vol_part_t *part;
	char *mountp;
	errno_t rc;

	log_msg(LOG_DEFAULT, LVL_NOTE, "vol_part_set_mountp_srv()");

	sid = IPC_GET_ARG1(*icall);

	rc = async_data_write_accept((void **)&mountp, true, 0,
	    VOL_MOUNTP_MAXLEN, 0, NULL);
	if (rc != EOK) {
		async_answer_0(icall, rc);
		return;
	}

	if (mountp != NULL) {
		log_msg(LOG_DEFAULT, LVL_NOTE,
		    "vol_part_set_mountp_srv: mountp='%s'", mountp);
	}

	rc = vol_part_find_by_id_ref(parts, sid, &part);
	if (rc != EOK) {
		free(mountp);
		async_answer_0(icall, ENOENT);
		return;
	}

	rc = vol_part_set_mountp_part(part, mountp);
	if (rc != EOK) {
		free(mountp);
		async_answer_0(icall, rc);
		vol_part_del_ref(part);
		return;
	}

	free(mountp);
	async_answer_0(icall, EOK);
}

static void vol_client_conn(ipc_call_t *icall, void *arg)
{
	vol_parts_t *parts = (vol_parts_t *) arg;

	log_msg(LOG_DEFAULT, LVL_DEBUG, "vol_client_conn()");

	/* Accept the connection */
	async_answer_0(icall, EOK);

	while (true) {
		ipc_call_t call;
		async_get_call(&call);
		sysarg_t method = IPC_GET_IMETHOD(call);

		if (!method) {
			/* The other side has hung up */
			async_answer_0(&call, EOK);
			return;
		}

		switch (method) {
		case VOL_GET_PARTS:
			vol_get_parts_srv(parts, &call);
			break;
		case VOL_PART_ADD:
			vol_part_add_srv(parts, &call);
			break;
		case VOL_PART_INFO:
			vol_part_info_srv(parts, &call);
			break;
		case VOL_PART_EJECT:
			vol_part_eject_srv(parts, &call);
			break;
		case VOL_PART_EMPTY:
			vol_part_empty_srv(parts, &call);
			break;
		case VOL_PART_LSUPP:
			vol_part_get_lsupp_srv(parts, &call);
			break;
		case VOL_PART_MKFS:
			vol_part_mkfs_srv(parts, &call);
			break;
		case VOL_PART_SET_MOUNTP:
			vol_part_set_mountp_srv(parts, &call);
			break;
		default:
			async_answer_0(&call, EINVAL);
		}
	}
}

int main(int argc, char *argv[])
{
	errno_t rc;

	printf("%s: Volume service\n", NAME);

	if (log_init(NAME) != EOK) {
		printf(NAME ": Failed to initialize logging.\n");
		return 1;
	}

	rc = vol_init();
	if (rc != EOK)
		return 1;

	printf(NAME ": Accepting connections.\n");
	task_retval(0);
	async_manager();

	return 0;
}

/** @}
 */
