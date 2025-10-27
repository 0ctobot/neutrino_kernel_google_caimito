// SPDX-License-Identifier: GPL-2.0
/*
 * Interface to access and set fields in VII packets, regardless of their format.
 *
 * Copyright (C) 2024 Google LLC
 */

#include "edgetpu-config.h"
#include "edgetpu-vii-packet.h"
#if EDGETPU_USE_LITEBUF_VII
#include "edgetpu-vii-litebuf.h"
#else
#include "edgetpu.h"
#endif

#if EDGETPU_USE_LITEBUF_VII
#define command_t struct edgetpu_vii_litebuf_command
#define response_t struct edgetpu_vii_litebuf_response
#else
#define command_t struct edgetpu_vii_command
#define response_t struct edgetpu_vii_response
#endif

size_t edgetpu_vii_command_packet_size(void)
{
	return sizeof(command_t);
}

size_t edgetpu_vii_response_packet_size(void)
{
	return sizeof(response_t);
}

/* Command accessors */

u64 edgetpu_vii_command_get_seq_number(void *cmd)
{
	command_t *vii_cmd = cmd;

	return vii_cmd->seq;
}

void  edgetpu_vii_command_set_seq_number(void *cmd, u64 seq_number)
{
	command_t *vii_cmd = cmd;

	vii_cmd->seq = seq_number;
}

u16 edgetpu_vii_command_get_code(void *cmd)
{
	command_t *vii_cmd = cmd;

#if EDGETPU_USE_LITEBUF_VII
	/*
	 * The value normally thought of as the "command code" for VII is within the litebuf and not
	 * exposed to the driver. Since this value is only used for logging under VII, just return
	 * the type of command.
	 */
	return vii_cmd->type;
#else
	return vii_cmd->code;
#endif
}

u32 edgetpu_vii_command_get_client_id(void *cmd)
{
	command_t *vii_cmd = cmd;

	return vii_cmd->client_id;
}

void edgetpu_vii_command_set_client_id(void *cmd, u32 client_id)
{
	command_t *vii_cmd = cmd;

	vii_cmd->client_id = client_id;
}

u32 edgetpu_vii_command_get_additional_info(void *cmd, u16 *additional_info_size)
{
#if EDGETPU_USE_LITEBUF_VII
	command_t *vii_cmd = cmd;

	if (additional_info_size)
		*additional_info_size = vii_cmd->additional_info_size;
	return vii_cmd->additional_info_address;
#else
	/* Flatbuffer VII does not support additional info. */
	if (additional_info_size)
		*additional_info_size = 0;
	return 0;
#endif
}

void edgetpu_vii_command_set_additional_info(void *cmd, u32 additional_info_addr,
					     u16 additional_info_size)
{
#if EDGETPU_USE_LITEBUF_VII
	command_t *vii_cmd = cmd;

	vii_cmd->additional_info_address = additional_info_addr;
	vii_cmd->additional_info_size = additional_info_size;
#else
	/* Flatbuffer VII does not support additional info. */
#endif
}

/* Response accessors */

u64 edgetpu_vii_response_get_seq_number(void *resp)
{
	response_t *vii_resp = resp;

	return vii_resp->seq;
}

void edgetpu_vii_response_set_seq_number(void *resp, u64 seq_number)
{
	response_t *vii_resp = resp;

	vii_resp->seq = seq_number;
}

u16 edgetpu_vii_response_get_code(void *resp)
{
	response_t *vii_resp = resp;

	return vii_resp->code;
}

void edgetpu_vii_response_set_code(void *resp, u16 code)
{
	response_t *vii_resp = resp;

	vii_resp->code = code;
}

u32 edgetpu_vii_response_get_client_id(void *resp)
{
	response_t *vii_resp = resp;

	return vii_resp->client_id;
}

void edgetpu_vii_response_set_client_id(void *resp, u32 client_id)
{
	response_t *vii_resp = resp;

	vii_resp->client_id = client_id;
}

u64 edgetpu_vii_response_get_retval(void *resp)
{
#if EDGETPU_USE_LITEBUF_VII
	/* Litebuf VII does not have a retval field. */
	return 0;
#else
	response_t *vii_resp = resp;

	return vii_resp->retval;
#endif
}

void edgetpu_vii_response_set_retval(void *resp, u64 retval)
{
#if EDGETPU_USE_LITEBUF_VII
	/* Litebuf VII does not have a retval field. */
#else
	response_t *vii_resp = resp;

	vii_resp->retval = retval;
#endif
}
