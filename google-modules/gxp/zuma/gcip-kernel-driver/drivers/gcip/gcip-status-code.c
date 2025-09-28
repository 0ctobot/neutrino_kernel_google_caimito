// SPDX-License-Identifier: GPL-2.0-only
/*
 * The helper functions to convert gcip_status_code
 *
 * Copyright (C) 2025 Google LLC
 */

#include <linux/errno.h>

#include <gcip/gcip-status-code.h>

enum gcip_status_code gcip_status_code_convert_from_errno(int errno)
{
	switch (errno) {
	case 0:
		return GCIP_STATUS_CODE_OK;
	case -EINVAL:
	case -ENAMETOOLONG:
	case -E2BIG:
	case -EDESTADDRREQ:
	case -EDOM:
	case -EFAULT:
	case -EILSEQ:
	case -ENOPROTOOPT:
	case -ENOTSOCK:
	case -ENOTTY:
	case -EPROTOTYPE:
	case -ESPIPE:
		return GCIP_STATUS_CODE_INVALID_ARGUMENT;
	case -ETIMEDOUT:
		return GCIP_STATUS_CODE_DEADLINE_EXCEEDED;
	case -ENODEV:
	case -ENOENT:
	case -ENOMEDIUM:
	case -ENXIO:
	case -ESRCH:
		return GCIP_STATUS_CODE_NOT_FOUND;
	case -EEXIST:
	case -EADDRNOTAVAIL:
	case -EALREADY:
	case -ENOTUNIQ:
		return GCIP_STATUS_CODE_ALREADY_EXISTS;
	case -EPERM:
	case -EACCES:
	case -ENOKEY:
	case -EROFS:
		return GCIP_STATUS_CODE_PERMISSION_DENIED;
	case -ENOTEMPTY:
	case -EISDIR:
	case -ENOTDIR:
	case -EADDRINUSE:
	case -EBADF:
	case -EBADFD:
	case -EBUSY:
	case -ECHILD:
	case -EISCONN:
	case -EISNAM:
	case -ENOTBLK:
	case -ENOTCONN:
	case -EPIPE:
	case -ESHUTDOWN:
	case -ETXTBSY:
	case -EUNATCH:
		return GCIP_STATUS_CODE_FAILED_PRECONDITION;
	case -ENOSPC:
	case -EDQUOT:
	case -EMFILE:
	case -EMLINK:
	case -ENFILE:
	case -ENOBUFS:
	case -ENODATA:
	case -ENOMEM:
	case -EUSERS:
		return GCIP_STATUS_CODE_RESOURCE_EXHAUSTED;
	case -ECHRNG:
	case -EFBIG:
	case -EOVERFLOW:
	case -ERANGE:
		return GCIP_STATUS_CODE_OUT_OF_RANGE;
	case -ENOPKG:
	case -EOPNOTSUPP:
	case -EAFNOSUPPORT:
	case -EPFNOSUPPORT:
	case -EPROTONOSUPPORT:
	case -ESOCKTNOSUPPORT:
	case -EXDEV:
		return GCIP_STATUS_CODE_UNIMPLEMENTED;
	case -EAGAIN:
	case -ECOMM:
	case -ECONNREFUSED:
	case -ECONNABORTED:
	case -ECONNRESET:
	case -EINTR:
	case -EHOSTDOWN:
	case -EHOSTUNREACH:
	case -ENETDOWN:
	case -ENETRESET:
	case -ENETUNREACH:
	case -ENOLCK:
	case -ENOLINK:
	case -ENONET:
		return GCIP_STATUS_CODE_UNAVAILABLE;
	case -EDEADLK:
	case -ESTALE:
		return GCIP_STATUS_CODE_ABORTED;
	case -ECANCELED:
		return GCIP_STATUS_CODE_CANCELLED;
	default:
		return GCIP_STATUS_CODE_UNKNOWN;
	}
}

int gcip_status_code_convert_to_errno(enum gcip_status_code status)
{
	switch (status) {
	case GCIP_STATUS_CODE_OK:
		return 0;
	case GCIP_STATUS_CODE_CANCELLED:
		return -ECANCELED;
	case GCIP_STATUS_CODE_INVALID_ARGUMENT:
		return -EINVAL;
	case GCIP_STATUS_CODE_DEADLINE_EXCEEDED:
		return -ETIMEDOUT;
	case GCIP_STATUS_CODE_NOT_FOUND:
		return -ENOENT;
	case GCIP_STATUS_CODE_ALREADY_EXISTS:
		return -EEXIST;
	case GCIP_STATUS_CODE_PERMISSION_DENIED:
		return -EACCES;
	case GCIP_STATUS_CODE_RESOURCE_EXHAUSTED:
	case GCIP_STATUS_CODE_FAILED_PRECONDITION:
	case GCIP_STATUS_CODE_ABORTED:
		return -EBUSY;
	case GCIP_STATUS_CODE_OUT_OF_RANGE:
		return -ERANGE;
	case GCIP_STATUS_CODE_UNIMPLEMENTED:
		return -EOPNOTSUPP;
	case GCIP_STATUS_CODE_UNAVAILABLE:
		return -EAGAIN;
	case GCIP_STATUS_CODE_UNAUTHENTICATED:
		return -EPERM;
	case GCIP_STATUS_CODE_DATA_LOSS:
	case GCIP_STATUS_CODE_INTERNAL:
	case GCIP_STATUS_CODE_UNKNOWN:
	default:
		return -EIO;
	}
}
