/* iv_err.c —— 错误码文本（core 层，仅 libc，可在开发机单测） */
#include "ivsbox/iv_err.h"

const char *ivs_strerror(int code)
{
    switch (code) {
    case IVS_OK:               return "ok";
    case IVS_ERR_INVAL:        return "invalid argument";
    case IVS_ERR_NOMEM:        return "out of memory";
    case IVS_ERR_TIMEOUT:      return "timeout";
    case IVS_ERR_BUSY:         return "busy";
    case IVS_ERR_AGAIN:        return "try again";
    case IVS_ERR_IO:           return "io error";
    case IVS_ERR_PROTO:        return "protocol error";
    case IVS_ERR_NOTREADY:     return "not ready";
    case IVS_ERR_UNSUPPORTED:  return "unsupported";
    case IVS_ERR_NOSPC:        return "no space left";
    case IVS_ERR_PERM:         return "permission denied";
    case IVS_ERR_CANCELED:     return "canceled";
    default:                   return "unknown error";
    }
}
