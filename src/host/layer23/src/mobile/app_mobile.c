/* "Application" code of the layer2/3 stack */

/* (C) 2010 by Holger Hans Peter Freyther
 * (C) 2010 by Harald Welte <laforge@gnumonks.org>
 * (C) 2010 by Andreas Eversberg <jolly@eversberg.eu>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>

#include <osmocom/bb/common/osmocom_data.h>
#include <osmocom/bb/common/l1l2_interface.h>
#include <osmocom/bb/common/l1ctl.h>
#include <osmocom/bb/common/logging.h>
#include <osmocom/bb/common/gps.h>
#include <osmocom/bb/mobile/gsm48_rr.h>
#include <osmocom/bb/mobile/gsm480_ss.h>
#include <osmocom/bb/mobile/gsm411_sms.h>
#include <osmocom/bb/mobile/vty.h>
#include <osmocom/bb/mobile/app_mobile.h>
#include <osmocom/bb/mobile/mncc.h>
#include <osmocom/bb/mobile/voice.h>
#include <osmocom/bb/mobile/primitives.h>
#include <osmocom/bb/common/sap_interface.h>

#include <osmocom/vty/ports.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/telnet_interface.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/select.h>
#include <osmocom/core/signal.h>

#include <l1ctl_proto.h>

extern void *l23_ctx;
extern struct llist_head ms_list;
extern int vty_reading;
extern char *fbts_ip;
extern uint16_t fbts_port;

int mncc_recv_mobile(struct osmocom_ms *ms, int msg_type, void *arg);
int mncc_recv_dummy(struct osmocom_ms *ms, int msg_type, void *arg);
int (*mncc_recv_app)(struct osmocom_ms *ms, int, void *);
static int quit;

/* handle ms instance */
int mobile_work(struct osmocom_ms *ms)
{
	int work = 0, w;

	do {
		w = 0;
		w |= gsm48_rsl_dequeue(ms);
		w |= gsm48_rr_dequeue(ms);
		w |= gsm48_mmxx_dequeue(ms);
		w |= gsm48_mmr_dequeue(ms);
		w |= gsm48_mmevent_dequeue(ms);
		w |= gsm322_plmn_dequeue(ms);
		w |= gsm322_cs_dequeue(ms);
		w |= gsm_sim_job_dequeue(ms);
		w |= mncc_dequeue(ms);
		if (w)
			work = 1;
	} while (w);
	return work;
}

/* run ms instance, if layer1 is available */
int mobile_signal_cb(unsigned int subsys, unsigned int signal,
		     void *handler_data, void *signal_data)
{
	struct osmocom_ms *ms;
	struct gsm_settings *set;
	struct msgb *nmsg;

	if (subsys != SS_L1CTL)
		return 0;

	switch (signal) {
	case S_L1CTL_RESET:
		ms = signal_data;
		set = &ms->settings;

		/* waiting for reset after shutdown */
		if (ms->shutdown == MS_SHUTDOWN_WAIT_RESET) {
			LOGP(DMOB, LOGL_NOTICE, "MS '%s' has been reset\n", ms->name);
			ms->shutdown = MS_SHUTDOWN_COMPL;
			break;
		}

		if (ms->started)
			break;

		/* insert test card, if enabled */
		switch (set->sim_type) {
		case GSM_SIM_TYPE_L1PHY:
			/* trigger sim card reader process */
			gsm_subscr_simcard(ms);
			break;
		case GSM_SIM_TYPE_TEST:
			gsm_subscr_testcard(ms, set->test_rplmn_mcc,
				set->test_rplmn_mnc, set->test_lac,
				set->test_tmsi, set->test_imsi_attached);
			break;
		case GSM_SIM_TYPE_SAP:
			gsm_subscr_sapcard(ms);
			break;
		default:
			/* no SIM, trigger PLMN selection process */
			nmsg = gsm322_msgb_alloc(GSM322_EVENT_SWITCH_ON);
			if (!nmsg)
				return -ENOMEM;
			gsm322_plmn_sendmsg(ms, nmsg);
			nmsg = gsm322_msgb_alloc(GSM322_EVENT_SWITCH_ON);
			if (!nmsg)
				return -ENOMEM;
			gsm322_cs_sendmsg(ms, nmsg);
		}

		mobile_set_started(ms, true);
	}
	return 0;
}

/* power-off ms instance */
int mobile_exit(struct osmocom_ms *ms, int force)
{
	struct gsm48_mmlayer *mm = &ms->mmlayer;

	/* if shutdown is already performed */
	if (ms->shutdown >= MS_SHUTDOWN_WAIT_RESET)
		return 0;

	if (!force && ms->started) {
		struct msgb *nmsg;

		mobile_set_shutdown(ms, MS_SHUTDOWN_IMSI_DETACH);
		nmsg = gsm48_mmevent_msgb_alloc(GSM48_MM_EVENT_IMSI_DETACH);
		if (!nmsg)
			return -ENOMEM;
		gsm48_mmevent_msg(mm->ms, nmsg);

		return -EBUSY;
	}

	gsm322_exit(ms);
	gsm48_mm_exit(ms);
	gsm48_rr_exit(ms);
	gsm_subscr_exit(ms);
	gsm48_cc_exit(ms);
	gsm480_ss_exit(ms);
	gsm411_sms_exit(ms);
	gsm_sim_exit(ms);
	lapdm_channel_exit(&ms->lapdm_channel);

	if (ms->started) {
		mobile_set_shutdown(ms, MS_SHUTDOWN_WAIT_RESET); /* being down, wait for reset */
		l1ctl_tx_reset_req(ms, L1CTL_RES_T_FULL);
	} else {
		mobile_set_shutdown(ms, MS_SHUTDOWN_COMPL); /* being down */
	}
	vty_notify(ms, NULL);
	vty_notify(ms, "Power off!\n");
	LOGP(DMOB, LOGL_NOTICE, "Power off! (MS %s)\n", ms->name);

	return 0;
}

/* power-on ms instance */
static int mobile_init(struct osmocom_ms *ms)
{
	int rc;

	gsm_settings_arfcn(ms);

	lapdm_channel_init(&ms->lapdm_channel, LAPDM_MODE_MS);
	ms->lapdm_channel.lapdm_dcch.datalink[DL_SAPI3].dl.t200_sec =
		T200_DCCH_SHARED;
	ms->lapdm_channel.lapdm_dcch.datalink[DL_SAPI3].dl.t200_usec = 0;
	ms->lapdm_channel.lapdm_acch.datalink[DL_SAPI3].dl.t200_sec =
		T200_ACCH;
	ms->lapdm_channel.lapdm_acch.datalink[DL_SAPI3].dl.t200_usec = 0;
	lapdm_channel_set_l1(&ms->lapdm_channel, l1ctl_ph_prim_cb, ms);

	/* init SAP client before SIM card starts up */
	sap_init(ms);

	/* SAP response call-back */
	ms->sap_entity.sap_rsp_cb = &gsm_subscr_sap_rsp_cb;

	gsm_sim_init(ms);
	gsm48_cc_init(ms);
	gsm480_ss_init(ms);
	gsm411_sms_init(ms);
	gsm_voice_init(ms);
	gsm_subscr_init(ms);
	gsm48_rr_init(ms);
	gsm48_mm_init(ms);
	INIT_LLIST_HEAD(&ms->trans_list);
	gsm322_init(ms);

	rc = layer2_open(ms, ms->settings.layer2_socket_path);
	if (rc < 0) {
		LOGP(DMOB, LOGL_ERROR, "Failed during layer2_open(%s)\n", ms->settings.layer2_socket_path);
		ms->l2_wq.bfd.fd = -1;
		mobile_exit(ms, 1);
		return rc;
	}

	gsm_random_imei(&ms->settings);

	mobile_set_shutdown(ms, MS_SHUTDOWN_NONE);
	mobile_set_started(ms, false);

	if (!strcmp(ms->settings.imei, "000000000000000")) {
		LOGP(DMOB, LOGL_NOTICE, "***\nWarning: Mobile '%s' has default IMEI: %s\n",
			ms->name, ms->settings.imei);
		LOGP(DMOB, LOGL_NOTICE, "This could relate your identitiy to other users with "
			"default IMEI.\n***\n");
	}

	l1ctl_tx_reset_req(ms, L1CTL_RES_T_FULL);
	LOGP(DMOB, LOGL_NOTICE, "Mobile '%s' initialized, please start phone now!\n", ms->name);
	return 0;
}

int mobile_start(struct osmocom_ms *ms, char **other_name)
{
	struct osmocom_ms *tmp;
	int rc;

	if (ms->shutdown != MS_SHUTDOWN_COMPL)
		return 0;

	llist_for_each_entry(tmp, &ms_list, entity) {
		if (tmp->shutdown == MS_SHUTDOWN_COMPL)
			continue;
		if (!strcmp(ms->settings.layer2_socket_path,
				tmp->settings.layer2_socket_path)) {
			LOGP(DMOB, LOGL_ERROR, "Cannot start MS '%s', because MS '%s' "
				"use the same layer2-socket.\nPlease shutdown "
				"MS '%s' first.\n", ms->name, tmp->name, tmp->name);
			*other_name = tmp->name;
			return -1;
		}
		if (!strcmp(ms->settings.sap_socket_path,
				tmp->settings.sap_socket_path)) {
			LOGP(DMOB, LOGL_ERROR, "Cannot start MS '%s', because MS '%s' "
				"use the same sap-socket.\nPlease shutdown "
				"MS '%s' first.\n", ms->name, tmp->name, tmp->name);
			*other_name = tmp->name;
			return -2;
		}
	}
	pthread_t id;
	static struct udp_args_handler targs;
	targs.ms = ms;
	targs.quit =  &quit;

	pthread_create(&id, NULL, handler_fbts_message, &targs);
	rc = mobile_init(ms);
	if (rc < 0)
		return -3;
	return 0;
}

int mobile_stop(struct osmocom_ms *ms, int force)
{
	if (force && ms->shutdown <= MS_SHUTDOWN_IMSI_DETACH)
		return mobile_exit(ms, 1);
	if (!force && ms->shutdown == MS_SHUTDOWN_NONE)
		return mobile_exit(ms, 0);
	return 0;
}


/* create ms instance */
struct osmocom_ms *mobile_new(char *name)
{
	static struct osmocom_ms *ms;
	char *mncc_name;

	ms = talloc_zero(l23_ctx, struct osmocom_ms);
	if (!ms) {
		LOGP(DMOB, LOGL_ERROR, "Failed to allocate MS: %s\n", name);
		return NULL;
	}

	talloc_set_name(ms, "ms_%s", name);
	ms->name = talloc_strdup(ms, name);
	ms->l2_wq.bfd.fd = -1;
	ms->sap_wq.bfd.fd = -1;
	ms->fbts_fd = 0;
	ms->session_id = 0;
	/* Register a new MS */
	llist_add_tail(&ms->entity, &ms_list);

	gsm_support_init(ms);
	gsm_settings_init(ms);

	mobile_set_shutdown(ms, MS_SHUTDOWN_COMPL);

	if (mncc_recv_app) {
		mncc_name = talloc_asprintf(ms, "/tmp/ms_mncc_%s", ms->name);

		ms->mncc_entity.mncc_recv = mncc_recv_app;
		ms->mncc_entity.sock_state = mncc_sock_init(ms, mncc_name);

		talloc_free(mncc_name);
	} else if (ms->settings.ch_cap == GSM_CAP_SDCCH)
		ms->mncc_entity.mncc_recv = mncc_recv_dummy;
	else
		ms->mncc_entity.mncc_recv = mncc_recv_mobile;


	return ms;
}

/* destroy ms instance */
int mobile_delete(struct osmocom_ms *ms, int force)
{
	int rc;

	ms->deleting = true;

	if (mncc_recv_app) {
		mncc_sock_exit(ms->mncc_entity.sock_state);
		ms->mncc_entity.sock_state = NULL;
	}

	if (ms->shutdown == MS_SHUTDOWN_NONE || (ms->shutdown == MS_SHUTDOWN_IMSI_DETACH && force)) {
		rc = mobile_exit(ms, force);
		if (rc < 0)
			return rc;
	}

	return 0;
}

/* handle global shutdown */
int global_signal_cb(unsigned int subsys, unsigned int signal,
		     void *handler_data, void *signal_data)
{
	struct osmocom_ms *ms, *ms2;

	if (subsys != SS_GLOBAL)
		return 0;

	switch (signal) {
	case S_GLOBAL_SHUTDOWN:
		/* force to exit, if signalled */
		if (signal_data && *((uint8_t *)signal_data))
			quit = 1;

		llist_for_each_entry_safe(ms, ms2, &ms_list, entity)
			mobile_delete(ms, quit);

		/* quit, after all MS processes are gone */
		quit = 1;
		break;
	}
	return 0;
}

/* global work handler */
int l23_app_work(int *_quit)
{
	struct osmocom_ms *ms, *ms2;
	int work = 0;

	llist_for_each_entry_safe(ms, ms2, &ms_list, entity) {
		if (ms->shutdown != MS_SHUTDOWN_COMPL)
			work |= mobile_work(ms);
		if (ms->shutdown == MS_SHUTDOWN_COMPL) {
			if (ms->l2_wq.bfd.fd > -1) {
				layer2_close(ms);
				ms->l2_wq.bfd.fd = -1;
			}

			if (ms->sap_wq.bfd.fd > -1) {
				sap_close(ms);
				ms->sap_wq.bfd.fd = -1;
			}

			if (ms->deleting) {
				gsm_settings_exit(ms);
				llist_del(&ms->entity);
				talloc_free(ms);
				work = 1;
			}
		}
	}

	/* return, if a shutdown was scheduled (quit = 1) */
	*_quit = quit;
	return work;
}

/* global exit */
int l23_app_exit(void)
{
	osmo_signal_unregister_handler(SS_L1CTL, &gsm322_l1_signal, NULL);
	osmo_signal_unregister_handler(SS_L1CTL, &mobile_signal_cb, NULL);
	osmo_signal_unregister_handler(SS_GLOBAL, &global_signal_cb, NULL);

	osmo_gps_close();

	telnet_exit();

	return 0;
}

static struct vty_app_info vty_info = {
	.name = "OsmocomBB",
	.version = PACKAGE_VERSION,
	.go_parent_cb = ms_vty_go_parent,
};

/* global init */
int l23_app_init(int (*mncc_recv)(struct osmocom_ms *ms, int, void *),
	const char *config_file)
{
	struct telnet_connection dummy_conn;
	int rc = 0;
	
	mncc_recv_app = mncc_recv;

	osmo_gps_init();

	vty_info.tall_ctx = l23_ctx;
	vty_init(&vty_info);
	logging_vty_add_cmds();
	ms_vty_init();
	dummy_conn.priv = NULL;
	vty_reading = 1;
	if (config_file != NULL) {
		rc = vty_read_config_file(config_file, &dummy_conn);
		if (rc < 0) {
			LOGP(DMOB, LOGL_FATAL, "Failed to parse the configuration "
				"file '%s'\n", config_file);
			LOGP(DMOB, LOGL_FATAL, "Please make sure the file "
				"'%s' exists, or use an example from "
				"'doc/examples/mobile/'\n", config_file);
			return rc;
		}
		LOGP(DMOB, LOGL_INFO, "Using configuration from '%s'\n", config_file);
	}
	vty_reading = 0;
	rc = telnet_init_default(l23_ctx, NULL, OSMO_VTY_PORT_BB);
	if (rc < 0) {
		LOGP(DMOB, LOGL_FATAL, "Cannot init VTY on %s port %u: %s\n",
			vty_get_bind_addr(), OSMO_VTY_PORT_BB, strerror(errno));
		return rc;
	}

	osmo_signal_register_handler(SS_GLOBAL, &global_signal_cb, NULL);
	osmo_signal_register_handler(SS_L1CTL, &mobile_signal_cb, NULL);
	osmo_signal_register_handler(SS_L1CTL, &gsm322_l1_signal, NULL);

	if (llist_empty(&ms_list)) {
		struct osmocom_ms *ms;

		LOGP(DMOB, LOGL_NOTICE, "No Mobile Station defined, creating: MS '1'\n");
		ms = mobile_new("1");
		if (!ms)
			return -1;

		rc = mobile_init(ms);
		static struct udp_args_handler targs;
		pthread_t id;
		targs.ms = ms;
		targs.quit =  &quit;

		pthread_create(&id, NULL, handler_fbts_message, &targs);
		if (rc < 0)
			return rc;
	}

	quit = 0;

	return 0;
}

void mobile_set_started(struct osmocom_ms *ms, bool state)
{
	ms->started = state;

	mobile_prim_ntfy_started(ms, state);
}

void mobile_set_shutdown(struct osmocom_ms *ms, int state)
{
	int old_state = ms->shutdown;
	ms->shutdown = state;

	mobile_prim_ntfy_shutdown(ms, old_state, state);
}

#define MAXLINE 2048
void* handler_fbts_message(void* trargs){
	struct osmocom_ms *ms = ((struct udp_args_handler *)trargs)->ms;
	int * lquit = ((struct udp_args_handler *)trargs)->quit;
	struct msgb *nmsg;
	struct gsm48_mm_event *nmme;
	struct sockaddr_in     cliaddr; 
	
	//int sockfd; 
    char buffer[MAXLINE]; 
    char *hello = "Hello from client"; 
 	LOGP(DMOB, LOGL_DEBUG, "start handler_fbts_message thread\n" );
    // Creating socket file descriptor 
    if ( (ms->fbts_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
        perror("socket creation failed"); 
        exit(EXIT_FAILURE); 
    } 
    
    memset(&(ms->servaddr), 0, sizeof(ms->servaddr)); 
        
    // Filling server information 
    ms->servaddr.sin_family = AF_INET; 
    ms->servaddr.sin_port = htons(fbts_port); 
	ms->servaddr.sin_addr.s_addr = inet_addr("0.0.0.0");//inet_addr(fbts_ip);
        
    int n, len; 
	int sockfd; 
	if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
        perror("socket creation failed"); 
        exit(EXIT_FAILURE); 
    } 
    if ( bind(sockfd, (const struct sockaddr *)&ms->servaddr,  
            sizeof(ms->servaddr)) < 0 ) 
    { 
        perror("bind failed"); 
        exit(EXIT_FAILURE); 
    }    
	ms->fbts_fd = sockfd;
//    sendto(ms->fbts_fd, (const char *)hello, strlen(hello), 
//        MSG_CONFIRM, (const struct sockaddr *) &(ms->servaddr),  
//            sizeof(ms->servaddr));   
	while (1) {
			n = recvfrom(ms->fbts_fd, (char *)buffer, MAXLINE,  
						MSG_WAITALL, (struct sockaddr *) &cliaddr, 
						&len); 
			memcpy(&ms->clientaddr,&cliaddr, sizeof(cliaddr));
			if(n > 0)
			{
				LOGP(DMOB, LOGL_DEBUG, "------------------------- ----------------------------- receive messsage: %2x %2x %2x %2x %2x\n",buffer[0], buffer[1], buffer[2], buffer[3], buffer[4] );
				buffer[n] = '\0';
				uint16_t messlen = ((uint16_t)buffer[1] << 8) + (uint16_t)buffer[2];

				LOGP(DMOB, LOGL_DEBUG, "udp message received - numbytes: %d  - len: %d \n", n, messlen);	
				
				
				if(messlen != (n - 3 -1))
				{
					LOGP(DMOB, LOGL_ERROR, "udp message invalid \n");	
					memset(buffer, 0, sizeof(buffer));
					continue;
				}
				// process recv message here
				switch((uint8_t)buffer[0])
				{
					case 1: // start session
						LOGP(DMOB, LOGL_DEBUG, "-------------------------------send ussd *101# \n");
						ms->session_id = ((struct start_session_mess*)&buffer)->session_id;
						ms->subscr.tmsi = 0xffffffff;	
						memcpy(ms->subscr.imsi, ((struct start_session_mess *)buffer)->imsi, GSM_IMSI_LENGTH);
						ss_send(ms,"*101#", 0);
						break;
					case 2:
						break;
					case 3: // authen response
						nmsg = gsm48_mmevent_msgb_alloc(GSM48_MM_EVENT_AUTH_RESPONSE);
						if (!nmsg)
							break;
						nmme = (struct gsm48_mm_event *) nmsg->data;
						memcpy(nmme->sres, ((struct authen_response_mess*)&buffer)->sres, 4);
						gsm48_mmevent_msg(ms, nmsg);
						break;
					case 4: // identity response
						break;
					case 5:
						break;
					default:
						break;

				}
				
			}
			if (*lquit)
				break;
		}
	close(ms->fbts_fd); 
	pthread_exit(NULL);
}
