#ifndef __COMPEL_INFECT_RPC_H__
#define __COMPEL_INFECT_RPC_H__

#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>

struct parasite_ctl;
extern int compel_rpc_sync(unsigned int cmd, struct parasite_ctl *ctl);
extern int compel_rpc_call(unsigned int cmd, struct parasite_ctl *ctl);
extern int compel_rpc_call_sync(unsigned int cmd, struct parasite_ctl *ctl);
extern int compel_rpc_sock(struct parasite_ctl *ctl);

struct ctl_msg {
	uint32_t	cmd;			/* command itself */
	uint32_t	ack;			/* ack on command */
	int32_t		err;			/* error code on reply */
};

#define ctl_msg_cmd(_cmd)		\
	(struct ctl_msg){.cmd = _cmd, }

#define ctl_msg_ack(_cmd, _err)	\
	(struct ctl_msg){.cmd = _cmd, .ack = _cmd, .err = _err, }

/*
 * NOTE: each command's args should be arch-independed sized.
 * If you want to use one of the standard types, declare
 * alternative type for compatible tasks in parasite-compat.h
 */
enum {
	PARASITE_CMD_IDLE		= 0,
	PARASITE_CMD_ACK,

	PARASITE_CMD_INIT_DAEMON,
	PARASITE_CMD_UNMAP,

	/*
	 * This must be greater than INITs.
	 */
	PARASITE_CMD_FINI,

	PARASITE_USER_CMDS,
};

struct parasite_unmap_args {
	uint64_t	parasite_start;
	uint64_t	parasite_len;
};

#endif
