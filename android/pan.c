/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <net/if.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <linux/if_bridge.h>

#include "btio/btio.h"
#include "lib/bluetooth.h"
#include "lib/bnep.h"
#include "lib/sdp.h"
#include "lib/sdp_lib.h"
#include "src/uuid-helper.h"
#include "profiles/network/bnep.h"
#include "src/log.h"

#include "hal-msg.h"
#include "ipc-common.h"
#include "ipc.h"
#include "utils.h"
#include "bluetooth.h"
#include "pan.h"

#define SVC_HINT_NETWORKING 0x02

#define BNEP_BRIDGE "bt-pan"
#define BNEP_PANU_INTERFACE "bt-pan"
#define BNEP_NAP_INTERFACE "bt-pan%d"

static bdaddr_t adapter_addr;
GSList *devices = NULL;
uint8_t local_role = HAL_PAN_ROLE_NONE;
static struct ipc *hal_ipc = NULL;

struct pan_device {
	char		iface[16];
	bdaddr_t	dst;
	uint8_t		conn_state;
	uint8_t		role;
	GIOChannel	*io;
	struct bnep	*session;
	guint		watch;
};

static struct {
	uint32_t	record_id;
	GIOChannel	*io;
	bool		bridge;
} nap_dev = {
	.record_id = 0,
	.io = NULL,
	.bridge = false,
};

static int set_forward_delay(int sk)
{
	unsigned long args[4] = { BRCTL_SET_BRIDGE_FORWARD_DELAY, 0 , 0, 0 };
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, BNEP_BRIDGE, IFNAMSIZ);
	ifr.ifr_data = (char *) args;

	if (ioctl(sk, SIOCDEVPRIVATE, &ifr) < 0) {
		error("pan: setting forward delay failed: %d (%s)",
							errno, strerror(errno));
		return -1;
	}

	return 0;
}

static int nap_create_bridge(void)
{
	int sk, err;

	DBG("%s", BNEP_BRIDGE);

	if (nap_dev.bridge)
		return 0;

	sk = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sk < 0)
		return -EOPNOTSUPP;

	if (ioctl(sk, SIOCBRADDBR, BNEP_BRIDGE) < 0) {
		err = -errno;
		if (err != -EEXIST) {
			close(sk);
			return -EOPNOTSUPP;
		}
	}

	err = set_forward_delay(sk);
	if (err < 0)
		ioctl(sk, SIOCBRDELBR, BNEP_BRIDGE);

	close(sk);

	nap_dev.bridge = err == 0;

	return err;
}

static int bridge_if_down(void)
{
	struct ifreq ifr;
	int sk, err;

	sk = socket(AF_INET, SOCK_DGRAM, 0);

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, BNEP_BRIDGE, IF_NAMESIZE - 1);

	ifr.ifr_flags &= ~IFF_UP;

	/* Bring down the interface */
	err = ioctl(sk, SIOCSIFFLAGS, (caddr_t) &ifr);

	close(sk);

	if (err < 0) {
		error("pan: Could not bring down %s", BNEP_BRIDGE);
		return err;
	}

	return 0;
}

static int nap_remove_bridge(void)
{
	int sk, err;

	DBG("%s", BNEP_BRIDGE);

	if (!nap_dev.bridge)
		return 0;

	bridge_if_down();

	sk = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sk < 0)
		return -EOPNOTSUPP;

	err = ioctl(sk, SIOCBRDELBR, BNEP_BRIDGE);
	if (err < 0)
		err = -errno;

	close(sk);

	if (err < 0)
		return err;

	nap_dev.bridge = false;

	return 0;
}

static int device_cmp(gconstpointer s, gconstpointer user_data)
{
	const struct pan_device *dev = s;
	const bdaddr_t *dst = user_data;

	return bacmp(&dev->dst, dst);
}

static void pan_device_free(void *data)
{
	struct pan_device *dev = data;

	if (dev->watch > 0) {
		bnep_server_delete(BNEP_BRIDGE, dev->iface, &dev->dst);
		g_source_remove(dev->watch);
	}

	if (dev->io) {
		g_io_channel_shutdown(dev->io, FALSE, NULL);
		g_io_channel_unref(dev->io);
	}

	if (dev->session)
		bnep_free(dev->session);

	g_free(dev);
}

static void pan_device_remove(struct pan_device *dev)
{
	devices = g_slist_remove(devices, dev);

	if (g_slist_length(devices) == 0) {
		local_role = HAL_PAN_ROLE_NONE;
		nap_remove_bridge();
	}

	pan_device_free(dev);
}

static void bt_pan_notify_conn_state(struct pan_device *dev, uint8_t state)
{
	struct hal_ev_pan_conn_state ev;
	char addr[18];

	if (dev->conn_state == state)
		return;

	dev->conn_state = state;
	ba2str(&dev->dst, addr);
	DBG("device %s state %u", addr, state);

	bdaddr2android(&dev->dst, ev.bdaddr);
	ev.state = state;
	ev.local_role = local_role;
	ev.remote_role = dev->role;
	ev.status = HAL_STATUS_SUCCESS;

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_PAN, HAL_EV_PAN_CONN_STATE,
							sizeof(ev), &ev);
	if (dev->conn_state == HAL_PAN_STATE_DISCONNECTED)
		pan_device_remove(dev);
}

static void bt_pan_notify_ctrl_state(struct pan_device *dev, uint8_t state)
{
	struct hal_ev_pan_ctrl_state ev;

	DBG("");

	ev.state = state;
	ev.local_role = local_role;
	ev.status = HAL_STATUS_SUCCESS;

	memset(ev.name, 0, sizeof(ev.name));

	if (local_role == HAL_PAN_ROLE_NAP)
		memcpy(ev.name, BNEP_BRIDGE, sizeof(BNEP_BRIDGE));
	else
		memcpy(ev.name, dev->iface, sizeof(dev->iface));

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_PAN, HAL_EV_PAN_CTRL_STATE,
							sizeof(ev), &ev);
}

static void bnep_disconn_cb(void *data)
{
	struct pan_device *dev = data;

	DBG("%s disconnected", dev->iface);

	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_DISCONNECTED);
}

static void bnep_conn_cb(char *iface, int err, void *data)
{
	struct pan_device *dev = data;

	DBG("");

	if (err < 0) {
		error("bnep connect req failed: %s", strerror(-err));
		bt_pan_notify_conn_state(dev, HAL_PAN_STATE_DISCONNECTED);
		return;
	}

	memcpy(dev->iface, iface, sizeof(dev->iface));

	DBG("%s connected", dev->iface);

	bt_pan_notify_ctrl_state(dev, HAL_PAN_CTRL_ENABLED);
	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_CONNECTED);
}

static void connect_cb(GIOChannel *chan, GError *err, gpointer data)
{
	struct pan_device *dev = data;
	uint16_t l_role, r_role;
	int perr, sk;

	DBG("");

	if (err) {
		error("%s", err->message);
		goto fail;
	}

	l_role = (local_role == HAL_PAN_ROLE_NAP) ? BNEP_SVC_NAP :
								BNEP_SVC_PANU;
	r_role = (dev->role == HAL_PAN_ROLE_NAP) ? BNEP_SVC_NAP : BNEP_SVC_PANU;

	sk = g_io_channel_unix_get_fd(dev->io);

	dev->session = bnep_new(sk, l_role, r_role, BNEP_PANU_INTERFACE);
	if (!dev->session)
		goto fail;

	perr = bnep_connect(dev->session, bnep_conn_cb, dev);
	if (perr < 0) {
		error("bnep connect req failed: %s", strerror(-perr));
		goto fail;
	}

	bnep_set_disconnect(dev->session, bnep_disconn_cb, dev);

	if (dev->io) {
		g_io_channel_unref(dev->io);
		dev->io = NULL;
	}

	return;

fail:
	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_DISCONNECTED);
}

static void bt_pan_connect(const void *buf, uint16_t len)
{
	const struct hal_cmd_pan_connect *cmd = buf;
	struct pan_device *dev;
	uint8_t status;
	bdaddr_t dst;
	char addr[18];
	GSList *l;
	GError *gerr = NULL;

	DBG("");

	switch (cmd->local_role) {
	case HAL_PAN_ROLE_NAP:
		if (cmd->remote_role != HAL_PAN_ROLE_PANU) {
			status = HAL_STATUS_UNSUPPORTED;
			goto failed;
		}
		break;
	case HAL_PAN_ROLE_PANU:
		if (cmd->remote_role != HAL_PAN_ROLE_NAP &&
					cmd->remote_role != HAL_PAN_ROLE_PANU) {
			status = HAL_STATUS_UNSUPPORTED;
			goto failed;
		}
		break;
	default:
		status = HAL_STATUS_UNSUPPORTED;
		goto failed;
	}

	android2bdaddr(&cmd->bdaddr, &dst);

	l = g_slist_find_custom(devices, &dst, device_cmp);
	if (l) {
		status = HAL_STATUS_FAILED;
		goto failed;
	}

	dev = g_new0(struct pan_device, 1);
	bacpy(&dev->dst, &dst);
	local_role = cmd->local_role;
	dev->role = cmd->remote_role;

	ba2str(&dev->dst, addr);
	DBG("connecting to %s %s", addr, dev->iface);

	dev->io = bt_io_connect(connect_cb, dev, NULL, &gerr,
					BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
					BT_IO_OPT_DEST_BDADDR, &dev->dst,
					BT_IO_OPT_PSM, BNEP_PSM,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
					BT_IO_OPT_OMTU, BNEP_MTU,
					BT_IO_OPT_IMTU, BNEP_MTU,
					BT_IO_OPT_INVALID);
	if (!dev->io) {
		error("%s", gerr->message);
		g_error_free(gerr);
		g_free(dev);
		status = HAL_STATUS_FAILED;
		goto failed;
	}

	devices = g_slist_append(devices, dev);
	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_CONNECTING);

	status = HAL_STATUS_SUCCESS;

failed:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_PAN, HAL_OP_PAN_CONNECT, status);
}

static void bt_pan_disconnect(const void *buf, uint16_t len)
{
	const struct hal_cmd_pan_disconnect *cmd = buf;
	struct pan_device *dev;
	uint8_t status;
	GSList *l;
	bdaddr_t dst;

	DBG("");

	android2bdaddr(&cmd->bdaddr, &dst);

	l = g_slist_find_custom(devices, &dst, device_cmp);
	if (!l) {
		status = HAL_STATUS_FAILED;
		goto failed;
	}

	dev = l->data;

	if (dev->conn_state == HAL_PAN_STATE_CONNECTED && dev->session)
		bnep_disconnect(dev->session);

	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_DISCONNECTED);
	status = HAL_STATUS_SUCCESS;

failed:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_PAN, HAL_OP_PAN_DISCONNECT,
									status);
}

static gboolean nap_watchdog_cb(GIOChannel *chan, GIOCondition cond,
							gpointer user_data)
{
	struct pan_device *dev = user_data;

	DBG("disconnected");

	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_DISCONNECTED);

	return FALSE;
}
static gboolean nap_setup_cb(GIOChannel *chan, GIOCondition cond,
							gpointer user_data)
{
	struct pan_device *dev = user_data;
	uint8_t packet[BNEP_MTU];
	struct bnep_setup_conn_req *req = (void *) packet;
	uint16_t src_role, dst_role, rsp = BNEP_CONN_NOT_ALLOWED;
	int sk, n;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		error("Hangup or error or inval on BNEP socket");
		return FALSE;
	}

	sk = g_io_channel_unix_get_fd(chan);

	/* Reading BNEP_SETUP_CONNECTION_REQUEST_MSG */
	n = read(sk, packet, sizeof(packet));
	if (n  < 0) {
		error("read(): %s(%d)", strerror(errno), errno);
		goto failed;
	}

	/* Highest known control command id BNEP_FILTER_MULT_ADDR_RSP 0x06 */
	if (req->type == BNEP_CONTROL &&
			req->ctrl > BNEP_FILTER_MULT_ADDR_RSP) {
		error("cmd not understood");
		bnep_send_ctrl_rsp(sk, BNEP_CONTROL, BNEP_CMD_NOT_UNDERSTOOD,
								req->ctrl);
		goto failed;
	}

	if (req->type != BNEP_CONTROL || req->ctrl != BNEP_SETUP_CONN_REQ) {
		error("cmd is not BNEP_SETUP_CONN_REQ %02X %02X", req->type,
								req->ctrl);
		goto failed;
	}

	rsp = bnep_setup_decode(req, &dst_role, &src_role);
	if (rsp) {
		error("bnep_setup_decode failed");
		goto failed;
	}

	rsp = bnep_setup_chk(dst_role, src_role);
	if (rsp) {
		error("benp_setup_chk failed");
		goto failed;
	}

	if (nap_create_bridge() < 0)
		goto failed;

	if (bnep_server_add(sk, dst_role, BNEP_BRIDGE, dev->iface,
							&dev->dst) < 0) {
		nap_remove_bridge();
		error("server_connadd failed");
		rsp = BNEP_CONN_NOT_ALLOWED;
		goto failed;
	}

	rsp = BNEP_SUCCESS;
	bnep_send_ctrl_rsp(sk, BNEP_CONTROL, BNEP_SETUP_CONN_RSP, rsp);

	dev->watch = g_io_add_watch(chan, G_IO_HUP | G_IO_ERR | G_IO_NVAL,
							nap_watchdog_cb, dev);
	g_io_channel_unref(dev->io);
	dev->io = NULL;

	bt_pan_notify_ctrl_state(dev, HAL_PAN_CTRL_ENABLED);
	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_CONNECTED);

	return FALSE;

failed:
	bnep_send_ctrl_rsp(sk, BNEP_CONTROL, BNEP_SETUP_CONN_RSP, rsp);
	pan_device_remove(dev);

	return FALSE;
}

static void nap_connect_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	struct pan_device *dev = user_data;

	DBG("");

	if (err) {
		error("%s", err->message);
		bt_pan_notify_conn_state(dev, HAL_PAN_STATE_DISCONNECTED);
		return;
	}

	g_io_channel_set_close_on_unref(chan, TRUE);
	dev->watch = g_io_add_watch(chan,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				nap_setup_cb, dev);
}

static void nap_confirm_cb(GIOChannel *chan, gpointer data)
{
	struct pan_device *dev;
	bdaddr_t dst;
	char address[18];
	GError *err = NULL;

	DBG("");

	bt_io_get(chan, &err, BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_DEST, address, BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		return;
	}

	DBG("incoming connect request from %s", address);
	dev = g_new0(struct pan_device, 1);
	bacpy(&dev->dst, &dst);
	local_role = HAL_PAN_ROLE_NAP;
	dev->role = HAL_PAN_ROLE_PANU;

	strncpy(dev->iface, BNEP_NAP_INTERFACE, 16);
	dev->iface[15] = '\0';

	dev->io = g_io_channel_ref(chan);
	g_io_channel_set_close_on_unref(dev->io, TRUE);

	if (!bt_io_accept(dev->io, nap_connect_cb, dev, NULL, &err)) {
		error("bt_io_accept: %s", err->message);
		g_error_free(err);
		goto failed;
	}

	devices = g_slist_append(devices, dev);
	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_CONNECTING);

	return;

failed:
	bt_pan_notify_conn_state(dev, HAL_PAN_STATE_DISCONNECTED);
}

static void destroy_nap_device(void)
{
	DBG("");

	nap_remove_bridge();

	if (nap_dev.io) {
		g_io_channel_shutdown(nap_dev.io, FALSE, NULL);
		g_io_channel_unref(nap_dev.io);
		nap_dev.io = NULL;
	}
}

static int register_nap_server(void)
{
	GError *gerr = NULL;

	DBG("");

	nap_dev.io = bt_io_listen(NULL, nap_confirm_cb, NULL, NULL, &gerr,
					BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
					BT_IO_OPT_PSM, BNEP_PSM,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
					BT_IO_OPT_OMTU, BNEP_MTU,
					BT_IO_OPT_IMTU, BNEP_MTU,
					BT_IO_OPT_INVALID);

	if (!nap_dev.io) {
		destroy_nap_device();
		error("%s", gerr->message);
		g_error_free(gerr);
		return -EINVAL;
	}

	return 0;
}

static void bt_pan_enable(const void *buf, uint16_t len)
{
	const struct hal_cmd_pan_enable *cmd = buf;
	uint8_t status;
	int err;

	DBG("");

	if (local_role == cmd->local_role) {
		status = HAL_STATUS_SUCCESS;
		goto reply;
	}

	/* destroy existing server */
	destroy_nap_device();

	switch (cmd->local_role) {
	case HAL_PAN_ROLE_NAP:
		break;
	case HAL_PAN_ROLE_NONE:
		status = HAL_STATUS_SUCCESS;
		goto reply;
	default:
		status = HAL_STATUS_UNSUPPORTED;
		goto reply;
	}

	local_role = cmd->local_role;
	err = register_nap_server();
	if (err < 0) {
		status = HAL_STATUS_FAILED;
		destroy_nap_device();
		goto reply;
	}

	status = HAL_STATUS_SUCCESS;

reply:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_PAN, HAL_OP_PAN_ENABLE, status);
}

static void bt_pan_get_role(const void *buf, uint16_t len)
{
	struct hal_rsp_pan_get_role rsp;

	DBG("");

	rsp.local_role = local_role;
	ipc_send_rsp_full(hal_ipc, HAL_SERVICE_ID_PAN, HAL_OP_PAN_GET_ROLE,
							sizeof(rsp), &rsp, -1);
}

static const struct ipc_handler cmd_handlers[] = {
	/* HAL_OP_PAN_ENABLE */
	{ bt_pan_enable, false, sizeof(struct hal_cmd_pan_enable) },
	/* HAL_OP_PAN_GET_ROLE */
	{ bt_pan_get_role, false, 0 },
	/* HAL_OP_PAN_CONNECT */
	{ bt_pan_connect, false, sizeof(struct hal_cmd_pan_connect) },
	/* HAL_OP_PAN_DISCONNECT */
	{ bt_pan_disconnect, false, sizeof(struct hal_cmd_pan_disconnect) },
};

static sdp_record_t *pan_record(void)
{
	sdp_list_t *svclass, *pfseq, *apseq, *root, *aproto;
	uuid_t root_uuid, pan, l2cap, bnep;
	sdp_profile_desc_t profile[1];
	sdp_list_t *proto[2];
	sdp_data_t *v, *p;
	uint16_t psm = BNEP_PSM, version = 0x0100;
	uint16_t security = 0x0001, type = 0xfffe;
	uint32_t rate = 0;
	const char *desc = "Network Access Point", *name = "Network Service";
	sdp_record_t *record;
	uint16_t ptype[] = { 0x0800, /* IPv4 */ 0x0806,  /* ARP */ };
	sdp_data_t *head, *pseq, *data;

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	record->attrlist = NULL;
	record->pattern = NULL;

	sdp_uuid16_create(&pan, NAP_SVCLASS_ID);
	svclass = sdp_list_append(NULL, &pan);
	sdp_set_service_classes(record, svclass);

	sdp_uuid16_create(&profile[0].uuid, NAP_PROFILE_ID);
	profile[0].version = 0x0100;
	pfseq = sdp_list_append(NULL, &profile[0]);
	sdp_set_profile_descs(record, pfseq);
	sdp_set_info_attr(record, name, NULL, desc);
	sdp_attr_add_new(record, SDP_ATTR_NET_ACCESS_TYPE, SDP_UINT16, &type);
	sdp_attr_add_new(record, SDP_ATTR_MAX_NET_ACCESSRATE,
							SDP_UINT32, &rate);

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(NULL, &l2cap);
	p = sdp_data_alloc(SDP_UINT16, &psm);
	proto[0] = sdp_list_append(proto[0], p);
	apseq = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&bnep, BNEP_UUID);
	proto[1] = sdp_list_append(NULL, &bnep);
	v = sdp_data_alloc(SDP_UINT16, &version);
	proto[1] = sdp_list_append(proto[1], v);

	head = sdp_data_alloc(SDP_UINT16, &ptype[0]);
	data = sdp_data_alloc(SDP_UINT16, &ptype[1]);
	sdp_seq_append(head, data);

	pseq = sdp_data_alloc(SDP_SEQ16, head);
	proto[1] = sdp_list_append(proto[1], pseq);
	apseq = sdp_list_append(apseq, proto[1]);
	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto);
	sdp_add_lang_attr(record);
	sdp_attr_add_new(record, SDP_ATTR_SECURITY_DESC, SDP_UINT16, &security);

	sdp_data_free(p);
	sdp_data_free(v);
	sdp_list_free(apseq, NULL);
	sdp_list_free(root, NULL);
	sdp_list_free(aproto, NULL);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(svclass, NULL);
	sdp_list_free(pfseq, NULL);

	return record;
}

bool bt_pan_register(struct ipc *ipc, const bdaddr_t *addr, uint8_t mode)
{
	sdp_record_t *rec;
	int err;

	DBG("");

	bacpy(&adapter_addr, addr);

	rec = pan_record();
	if (!rec) {
		error("Failed to allocate PAN record");
		return false;
	}

	if (bt_adapter_add_record(rec, SVC_HINT_NETWORKING) < 0) {
		error("Failed to register PAN record");
		sdp_record_free(rec);
		return false;
	}

	err = bnep_init();
	if (err < 0) {
		error("bnep init failed");
		bt_adapter_remove_record(rec->handle);
		return false;
	}

	err = register_nap_server();
	if (err < 0) {
		error("Failed to register NAP");
		bt_adapter_remove_record(rec->handle);
		bnep_cleanup();
		return false;
	}

	nap_dev.record_id = rec->handle;

	hal_ipc = ipc;
	ipc_register(hal_ipc, HAL_SERVICE_ID_PAN, cmd_handlers,
						G_N_ELEMENTS(cmd_handlers));

	return true;
}

void bt_pan_unregister(void)
{
	DBG("");

	g_slist_free_full(devices, pan_device_free);
	devices = NULL;
	local_role = HAL_PAN_ROLE_NONE;

	bnep_cleanup();

	ipc_unregister(hal_ipc, HAL_SERVICE_ID_PAN);
	hal_ipc = NULL;

	bt_adapter_remove_record(nap_dev.record_id);
	nap_dev.record_id = 0;
	destroy_nap_device();
}
