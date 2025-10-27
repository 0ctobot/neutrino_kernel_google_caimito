/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Interface to access and set fields in VII packets, encapsulating knowledge of which VII format
 * is in use as well as the layout of each format's packets.
 *
 * Copyright (C) 2024 Google LLC
 */
#ifndef __EDGETPU_VII_PACKET_H__
#define __EDGETPU_VII_PACKET_H__

#include <linux/types.h>

#include "edgetpu-internal.h"

size_t edgetpu_vii_command_packet_size(void);
size_t edgetpu_vii_response_packet_size(void);

/* Command accessors */
u64 edgetpu_vii_command_get_seq_number(void *cmd);
void edgetpu_vii_command_set_seq_number(void *cmd, u64 seq_number);

u16 edgetpu_vii_command_get_code(void *cmd);

u32 edgetpu_vii_command_get_client_id(void *cmd);
void edgetpu_vii_command_set_client_id(void *cmd, u32 client_id);

/*
 * Returns daddr of the additional_info of @cmd.
 * @additional_info_size is an optional pointer (can be NULL) to get the size of it.
 */
u32 edgetpu_vii_command_get_additional_info(void *cmd, u16 *additional_info_size);
void edgetpu_vii_command_set_additional_info(void *cmd, u32 additional_info_addr,
					     u16 additional_info_size);

/* Response accessors */
u64 edgetpu_vii_response_get_seq_number(void *resp);
void edgetpu_vii_response_set_seq_number(void *resp, u64 seq_number);

u16 edgetpu_vii_response_get_code(void *resp);
void edgetpu_vii_response_set_code(void *resp, u16 code);

u32 edgetpu_vii_response_get_client_id(void *resp);
void edgetpu_vii_response_set_client_id(void *resp, u32 client_id);

u64 edgetpu_vii_response_get_retval(void *resp);
void edgetpu_vii_response_set_retval(void *resp, u64 retval);

#endif /* __EDGETPU_VII_PACKET_H__*/
