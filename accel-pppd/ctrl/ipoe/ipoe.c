#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>

#include <pcre.h>

#include "events.h"
#include "list.h"
#include "triton.h"
#include "log.h"
#include "mempool.h"
#include "utils.h"
#include "cli.h"
#include "ap_session.h"
#include "pwdb.h"
#include "ipdb.h"

#include "iplink.h"
#include "connlimit.h"

#include "ipoe.h"

#include "memdebug.h"

#define USERNAME_IFNAME 0
#define USERNAME_LUA 1

static int conf_dhcpv4 = 1;
//static int conf_dhcpv6;
static int conf_username;

#ifdef USE_LUA
static const char *conf_lua_username_func;
#endif

static int conf_offer_timeout = 3;
static in_addr_t conf_gw_address;
static int conf_netmask = 24;
static int conf_lease_time = 600;
static int conf_lease_timeout = 660;
static int conf_verbose;

static unsigned int stat_starting;
static unsigned int stat_active;

static mempool_t ses_pool;

static LIST_HEAD(serv_list);

struct iplink_arg
{
	pcre *re;
	const char *opt;
};

static void ipoe_session_finished(struct ap_session *s);

static struct ipoe_session *ipoe_session_lookup(struct ipoe_serv *serv, struct dhcpv4_packet *pack)
{
	struct ipoe_session *ses;

	list_for_each_entry(ses, &serv->sessions, entry) {
		if (pack->hdr->xid != ses->xid)
			continue;

		if (pack->hdr->giaddr != ses->giaddr)
			continue;

		if (pack->agent_circuit_id && !ses->agent_circuit_id)
			continue;
		
		if (pack->agent_remote_id && !ses->agent_remote_id)
			continue;
		
		if (pack->client_id && !ses->client_id)
			continue;
		
		if (!pack->agent_circuit_id && ses->agent_circuit_id)
			continue;
		
		if (!pack->agent_remote_id && ses->agent_remote_id)
			continue;
		
		if (!pack->client_id && ses->client_id)
			continue;

		if (pack->agent_circuit_id) {
			if (pack->agent_circuit_id->len != ses->agent_circuit_id->len)
				continue;
			if (memcmp(pack->agent_circuit_id->data, ses->agent_circuit_id->data, pack->agent_circuit_id->len))
				continue;
		}
		
		if (pack->agent_remote_id) {
			if (pack->agent_remote_id->len != ses->agent_remote_id->len)
				continue;
			if (memcmp(pack->agent_remote_id->data, ses->agent_remote_id->data, pack->agent_remote_id->len))
				continue;
		}
		
		if (pack->client_id) {
			if (pack->client_id->len != ses->client_id->len)
				continue;
			if (memcmp(pack->client_id->data, ses->client_id->data, pack->client_id->len))
				continue;
		}

		if (memcmp(pack->hdr->chaddr, ses->hwaddr, 6))
			continue;

		return ses;
	}

	return NULL;
}

static void ipoe_session_timeout(struct triton_timer_t *t)
{
	struct ipoe_session *ses = container_of(t, typeof(*ses), timer);

	triton_timer_del(t);

	log_ppp_info2("session timed out\n");

	ap_session_terminate(&ses->ses, TERM_LOST_CARRIER, 0);
}

static void ipoe_session_set_username(struct ipoe_session *ses)
{
#ifdef USE_LUA
	if (conf_username == USERNAME_LUA) {
		ipoe_lua_set_username(ses, conf_lua_username_func);
	} else
#endif
	ses->ses.username = _strdup(ses->ses.ifname);
}

static void ipoe_session_start(struct ipoe_session *ses)
{
	int r;
	char *passwd;

	if (ses->serv->opt_single)
		strncpy(ses->ses.ifname, ses->serv->ifname, AP_IFNAME_LEN);
	
	ipoe_session_set_username(ses);
	if (!ses->ses.username) {
		ipoe_session_finished(&ses->ses);
		return;
	}
	
	triton_event_fire(EV_CTRL_STARTING, &ses->ses);
	triton_event_fire(EV_CTRL_STARTED, &ses->ses);

	ap_session_starting(&ses->ses);
	
	r = pwdb_check(&ses->ses, ses->ses.username, 0);
	if (r == PWDB_NO_IMPL) {
		passwd = pwdb_get_passwd(&ses->ses, ses->ses.username);
		if (!passwd)
			r = PWDB_DENIED;
		else {
			r = PWDB_SUCCESS;
			_free(passwd);
		}
	}

	if (r == PWDB_DENIED) {
		if (conf_ppp_verbose)
			log_ppp_warn("authentication failed\n");
		ap_session_terminate(&ses->ses, TERM_AUTH_ERROR, 0);
		return;
	}

	if (ses->dhcpv4_request) {
		ses->ses.ipv4 = ipdb_get_ipv4(&ses->ses);
		if (!ses->ses.ipv4) {
			log_ppp_warn("no free IPv4 address\n");
			ap_session_terminate(&ses->ses, TERM_AUTH_ERROR, 0);
			return;
		}

		if (conf_gw_address)
			ses->ses.ipv4->addr = conf_gw_address;
		
		if (conf_netmask)
			ses->ses.ipv4->mask = conf_netmask;
		else if (!ses->ses.ipv4->mask)
			ses->ses.ipv4->mask = 24;

		dhcpv4_send_reply(DHCPOFFER, ses->serv->dhcpv4, ses->dhcpv4_request, &ses->ses, conf_lease_time);

		dhcpv4_packet_free(ses->dhcpv4_request);
		ses->dhcpv4_request = NULL;
	}

	ses->timer.expire = ipoe_session_timeout;
	ses->timer.period = conf_offer_timeout * 1000;
	triton_timer_add(&ses->ctx, &ses->timer, 0);
}

static void ipoe_session_activate(struct ipoe_session *ses)
{
	ap_session_activate(&ses->ses);

	if (ses->ses.state == AP_STATE_ACTIVE)
		dhcpv4_send_reply(DHCPACK, ses->serv->dhcpv4, ses->dhcpv4_request, &ses->ses, conf_lease_time);
	else
		dhcpv4_send_nak(ses->serv->dhcpv4, ses->dhcpv4_request);

	dhcpv4_packet_free(ses->dhcpv4_request);
	ses->dhcpv4_request = NULL;
}

static void ipoe_session_started(struct ap_session *s)
{
	struct ipoe_session *ses = container_of(s, typeof(*ses), ses);
	
	log_ppp_debug("ipoe: session started\n");

	triton_timer_del(&ses->timer);

	ses->timer.expire = ipoe_session_timeout;
	ses->timer.period = conf_lease_timeout * 1000;
	triton_timer_add(&ses->ctx, &ses->timer, 0);
}

static void ipoe_session_free(struct ipoe_session *ses)
{
	if (ses->timer.tpd)
		triton_timer_del(&ses->timer);

	triton_context_unregister(&ses->ctx);
	
	if (ses->data)
		_free(ses->data);

	mempool_free(ses);
}

static void ipoe_session_finished(struct ap_session *s)
{
	struct ipoe_session *ses = container_of(s, typeof(*ses), ses);

	log_ppp_debug("ipoe: session finished\n");

	pthread_mutex_lock(&ses->serv->lock);
	list_del(&ses->entry);
	pthread_mutex_unlock(&ses->serv->lock);

	triton_context_call(&ses->ctx, (triton_event_func)ipoe_session_free, ses);
}

static void ipoe_session_terminate(struct ap_session *s, int hard)
{
	ap_session_finished(s);
}


static void ipoe_session_close(struct triton_context_t *ctx)
{
	struct ipoe_session *ses = container_of(ctx, typeof(*ses), ctx);
	
	if (ses->ses.state)
		ap_session_terminate(&ses->ses, TERM_ADMIN_RESET, 1);
	else
		ipoe_session_finished(&ses->ses);
}

static struct ipoe_session *ipoe_session_create(struct ipoe_serv *serv, struct dhcpv4_packet *pack)
{
	struct ipoe_session *ses;
	int dlen = 0;
	uint8_t *ptr;

	ses = mempool_alloc(ses_pool);
	if (!ses) {
		log_emerg("out of memery\n");
		return NULL;
	}

	memset(ses, 0, sizeof(*ses));

	ap_session_init(&ses->ses);

	ses->serv = serv;
	ses->dhcpv4_request = pack;
	
	ses->xid = pack->hdr->xid;
	memcpy(ses->hwaddr, pack->hdr->chaddr, 6);
	ses->giaddr = pack->hdr->giaddr;

	if (pack->agent_circuit_id)
		dlen += sizeof(struct dhcp_opt) + pack->agent_circuit_id->len;
	
	if (pack->agent_remote_id)
		dlen += sizeof(struct dhcp_opt) + pack->agent_remote_id->len;
	
	if (pack->client_id)
		dlen += sizeof(struct dhcp_opt) + pack->client_id->len;
	
	if (dlen) {
		ses->data = _malloc(dlen);
		if (!ses->data) {
			log_emerg("out of memery\n");
			mempool_free(ses);
			return NULL;
		}
		ptr = ses->data;
	}

	if (pack->agent_circuit_id) {
		ses->agent_circuit_id = (struct dhcp_opt *)ptr;
		ses->agent_circuit_id->len = pack->agent_circuit_id->len;
		memcpy(ses->agent_circuit_id->data, pack->agent_circuit_id->data, pack->agent_circuit_id->len);
		ptr += sizeof(struct dhcp_opt) + pack->agent_circuit_id->len;
	}

	if (pack->agent_remote_id) {
		ses->agent_remote_id = (struct dhcp_opt *)ptr;
		ses->agent_remote_id->len = pack->agent_remote_id->len;
		memcpy(ses->agent_remote_id->data, pack->agent_remote_id->data, pack->agent_remote_id->len);
		ptr += sizeof(struct dhcp_opt) + pack->agent_remote_id->len;
	}
	
	if (pack->client_id) {
		ses->client_id = (struct dhcp_opt *)ptr;
		ses->client_id->len = pack->client_id->len;
		memcpy(ses->client_id->data, pack->client_id->data, pack->client_id->len);
		ptr += sizeof(struct dhcp_opt) + pack->client_id->len;
	}

	ses->ctx.before_switch = log_switch;
	ses->ctx.close = ipoe_session_close;
	ses->ctrl.ctx = &ses->ctx;
	ses->ctrl.started = ipoe_session_started;
	ses->ctrl.finished = ipoe_session_finished;
	ses->ctrl.terminate = ipoe_session_terminate;
	ses->ctrl.type = CTRL_TYPE_IPOE;
	ses->ctrl.name = "ipoe";
	
	ses->ctrl.calling_station_id = _malloc(19);
	ses->ctrl.called_station_id = serv->ifname;
	
	ptr = ses->hwaddr;
	sprintf(ses->ctrl.calling_station_id, "%02x:%02x:%02x:%02x:%02x:%02x",
		ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5]);
	
	ses->ses.ctrl = &ses->ctrl;
	ses->ses.chan_name = ses->ctrl.calling_station_id;

	triton_context_register(&ses->ctx, &ses->ses);

	triton_context_wakeup(&ses->ctx);

	//pthread_mutex_lock(&serv->lock);
	list_add_tail(&ses->entry, &serv->sessions);
	//pthread_mutex_unlock(&serv->lock);

	triton_context_call(&ses->ctx, (triton_event_func)ipoe_session_start, ses);

	return ses;
}

static void ipoe_dhcpv4_recv(struct dhcpv4_serv *dhcpv4, struct dhcpv4_packet *pack)
{
	struct ipoe_serv *serv = container_of(dhcpv4->ctx, typeof(*serv), ctx);
	struct ipoe_session *ses;
	//struct dhcpv4_packet *reply;

	pthread_mutex_lock(&serv->lock);
	if (pack->msg_type == DHCPDISCOVER) {
		ses = ipoe_session_lookup(serv, pack);
		if (!ses) {
			ses = ipoe_session_create(serv, pack);

			if (conf_verbose &&  ses) {
				log_switch(dhcpv4->ctx, &ses->ses);
				log_ppp_info2("recv ");
				dhcpv4_print_packet(pack, log_ppp_info2);
			}
		}	else {
			log_switch(dhcpv4->ctx, &ses->ses);

			if (conf_verbose) {
				log_ppp_info2("recv ");
				dhcpv4_print_packet(pack, log_ppp_info2);
			}

			if (ses->ses.state == AP_STATE_ACTIVE)
				dhcpv4_send_reply(DHCPOFFER, dhcpv4, pack, &ses->ses, conf_lease_time);

			dhcpv4_packet_free(pack);
		}
	} else if (pack->msg_type == DHCPREQUEST) {
		ses = ipoe_session_lookup(serv, pack);	

		if (!ses) {
			if (conf_verbose) {
				log_info2("recv ");
				dhcpv4_print_packet(pack, log_info2);
			}

			dhcpv4_send_nak(dhcpv4, pack);
		} else {
			if (!ses->ses.ipv4 || pack->server_id != ses->ses.ipv4->addr || pack->request_ip != ses->ses.ipv4->peer_addr) {
				if (conf_verbose) {
					log_info2("recv ");
					dhcpv4_print_packet(pack, log_info2);
				}

				if (ses->ses.ipv4 && pack->request_ip != ses->ses.ipv4->peer_addr)
					dhcpv4_send_nak(dhcpv4, pack);
				ap_session_terminate(&ses->ses, TERM_USER_REQUEST, 0);
			} else {
				if (conf_verbose) {
					log_switch(dhcpv4->ctx, &ses->ses);
					log_ppp_info2("recv ");
					dhcpv4_print_packet(pack, log_ppp_info2);
				}

				if (ses->ses.state == AP_STATE_STARTING && !ses->dhcpv4_request) {
					ses->dhcpv4_request = pack;
					pack = NULL;
					triton_context_call(&ses->ctx, (triton_event_func)ipoe_session_activate, ses);
				}
			}
		}
		if (pack)
			dhcpv4_packet_free(pack);
	} else if (pack->msg_type == DHCPDECLINE || pack->msg_type == DHCPRELEASE) {
		ses = ipoe_session_lookup(serv, pack);
		if (ses) {
			if (conf_verbose) {
				log_switch(dhcpv4->ctx, &ses->ses);
				log_ppp_info2("recv ");
				dhcpv4_print_packet(pack, log_ppp_info2);
			}

			ap_session_terminate(&ses->ses, TERM_USER_REQUEST, 0);
		}
		dhcpv4_packet_free(pack);
	}
	pthread_mutex_unlock(&serv->lock);
}

static void ipoe_serv_close(struct triton_context_t *ctx)
{
	struct ipoe_serv *serv = container_of(ctx, typeof(*serv), ctx);

	if (serv->dhcpv4)
		dhcpv4_free(serv->dhcpv4);

	triton_context_unregister(ctx);

	_free(serv->ifname);
	_free(serv);
}

static int show_stat_exec(const char *cmd, char * const *fields, int fields_cnt, void *client)
{
	cli_send(client, "ipoe:\r\n");
	cli_sendv(client,"  starting: %u\r\n", stat_starting);
	cli_sendv(client,"  active: %u\r\n", stat_active);

	return CLI_CMD_OK;
}

void __export ipoe_get_stat(unsigned int **starting, unsigned int **active)
{
	*starting = &stat_starting;
	*active = &stat_active;
}

static void ipoe_drop_sessions(struct ipoe_serv *serv)
{

}

static void add_interface(const char *ifname, int ifindex, const char *opt)
{
	int opt_single;
	const char *ptr;
	struct ipoe_serv *serv;

	ptr = strstr(opt, ",single");
	if (ptr) {
		if (ptr[7] && ptr[7] != ',')
			goto out_err_parse;
		opt_single = 1;
	} else
		opt_single = 0;

	
	list_for_each_entry(serv, &serv_list, entry) {
		if (strcmp(ifname, serv->ifname) == 0) {
			serv->active = 1;
			serv->ifindex = ifindex;
			if (opt_single && !serv->opt_single)
				ipoe_drop_sessions(serv);
			serv->opt_single = opt_single;
			return;
		}
	}

	serv = _malloc(sizeof(*serv));
	memset(serv, 0, sizeof(*serv));
	serv->ifname = _strdup(ifname);
	serv->ifindex = ifindex;
	serv->opt_single = opt_single;
	serv->opt_dhcpv4 = conf_dhcpv4;
	INIT_LIST_HEAD(&serv->sessions);
	pthread_mutex_init(&serv->lock, NULL);

	triton_context_register(&serv->ctx, NULL);

	if (serv->opt_dhcpv4) {
		serv->dhcpv4 = dhcpv4_create(&serv->ctx, serv->ifname);
		if (serv->dhcpv4)
			serv->dhcpv4->recv = ipoe_dhcpv4_recv;
	}

	triton_context_wakeup(&serv->ctx);

	return;

out_err_parse:
	log_error("ipoe: failed to parse '%s'\n", opt);
}

static void load_interface(const char *opt)
{
	const char *ptr;
	struct ifreq ifr;

	for (ptr = opt; *ptr && *ptr != ','; ptr++);

	if (ptr - opt >= sizeof(ifr.ifr_name))
		return;

	memcpy(ifr.ifr_name, opt, ptr - opt);
	ifr.ifr_name[ptr - opt] = 0;
	
	if (ioctl(sock_fd, SIOCGIFINDEX, &ifr)) {
		log_error("ipoe: '%s': ioctl(SIOCGIFINDEX): %s\n", ifr.ifr_name, strerror(errno));
		return;
	}

	add_interface(ifr.ifr_name, ifr.ifr_ifindex, opt);
}

static int __load_interface_re(int index, int flags, const char *name, struct iplink_arg *arg)
{
	if (pcre_exec(arg->re, NULL, name, strlen(name), 0, 0, NULL, 0) < 0)
		return 0;
	
	add_interface(name, index, arg->opt);

	return 0;
}

static void load_interface_re(const char *opt)
{
	pcre *re = NULL;
	const char *pcre_err;
	char *pattern;
	const char *ptr;
	int pcre_offset;
	struct iplink_arg arg;

	for (ptr = opt; *ptr && *ptr != ','; ptr++);
	
	pattern = _malloc(ptr - (opt + 3) + 1);
	memcpy(pattern, opt + 3, ptr - (opt + 3));
	pattern[ptr - (opt + 3)] = 0;
	
	re = pcre_compile2(pattern, 0, NULL, &pcre_err, &pcre_offset, NULL);
		
	if (!re) {
		log_error("ipoe: %s at %i\r\n", pcre_err, pcre_offset);
		return;
	}

	arg.re = re;
	arg.opt = opt;

	iplink_list((iplink_list_func)__load_interface_re, &arg);

	pcre_free(re);
	_free(pattern);
}

static void load_interfaces(struct conf_sect_t *sect)
{
	struct ipoe_serv *serv;
	struct conf_option_t *opt;
	struct list_head *pos, *n;

	list_for_each_entry(serv, &serv_list, entry)
		serv->active = 0;

	list_for_each_entry(opt, &sect->items, entry) {
		if (strcmp(opt->name, "interface"))
			continue;
		if (!opt->val)
			continue;

		if (strlen(opt->val) > 3 && memcmp(opt->val, "re:", 3) == 0)
			load_interface_re(opt->val);
		else
			load_interface(opt->val);
	}
	
	list_for_each_safe(pos, n, &serv_list) {
		serv = list_entry(pos, typeof(*serv), entry);
		if (!serv->active) {
			list_del(&serv->entry);
			triton_context_call(&serv->ctx, (triton_event_func)ipoe_serv_close, &serv->ctx);
		}
	}
}

static void load_config(void)
{
	const char *opt;
	struct conf_sect_t *s = conf_get_section("ipoe");

	if (!s)
		return;

	load_interfaces(s);

	opt = conf_get_opt("ipoe", "username");
	if (opt) {
		if (strcmp(opt, "ifname") == 0)
			conf_username = USERNAME_IFNAME;
#ifdef USE_LUA
		else if (strlen(opt) > 4 && memcmp(opt, "lua:", 4) == 0) {
			conf_username = USERNAME_LUA;
			conf_lua_username_func = opt + 4;
#endif
		} else
			log_emerg("ipoe: unknown username value '%s'\n", opt);
	}

	opt = conf_get_opt("ipoe", "gw-ip-address");
	if (opt)
		conf_gw_address = inet_addr(opt);
	else
		conf_gw_address = 0;

	opt = conf_get_opt("ipoe", "netmask");
	if (opt) {
		conf_netmask = atoi(opt);
		if (conf_netmask <= 0 || conf_netmask > 32) {
			log_error("ipoe: invalid netmask %s\n", opt);
			conf_netmask = 0;
		}
	} else
		conf_netmask = 0;
	
	opt = conf_get_opt("ipoe", "verbose");
	if (opt)
		conf_verbose = atoi(opt);
}

static void ipoe_init(void)
{
	ses_pool = mempool_create(sizeof(struct ipoe_session));

	load_config();

	cli_register_simple_cmd2(show_stat_exec, NULL, 2, "show", "stat");
	
	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
}

DEFINE_INIT(20, ipoe_init);
