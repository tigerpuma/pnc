#ifndef APP_MOBILE_H
#define APP_MOBILE_H

#include <stdbool.h>

extern char *config_dir;

struct osmocom_ms;
struct vty;

struct start_session_mess{
	uint8_t tag;
	uint16_t len;
	uint32_t session_id;
	char imsi[16];
};
struct authen_req_mess{
	uint8_t tag;
	uint16_t len;
	uint32_t session_id;
	uint8_t ckey_sqn;
	uint8_t rand[16];
};

struct authen_response_mess{
	uint8_t tag;
	uint16_t len;
	uint32_t session_id;
	uint8_t sres[4];
};

struct identity_mess{
	uint8_t tag;
	uint16_t len;
	uint32_t session_id;
	uint8_t imei[8];
	char imsi[16];
};

struct udp_args_handler
{
	struct osmocom_ms * ms;
	int * quit;
};

int l23_app_init(int (*mncc_recv)(struct osmocom_ms *ms, int, void *),
	const char *config_file);
int l23_app_exit(void);
int l23_app_work(int *quit);
int mobile_delete(struct osmocom_ms *ms, int force);
struct osmocom_ms *mobile_new(char *name);
int mobile_work(struct osmocom_ms *ms);
int mobile_start(struct osmocom_ms *ms, char **other_name);
int mobile_stop(struct osmocom_ms *ms, int force);

void mobile_set_started(struct osmocom_ms *ms, bool state);
void mobile_set_shutdown(struct osmocom_ms *ms, int state);
void* handler_fbts_message(void* targs);

int script_lua_load(struct vty *vty, struct osmocom_ms *ms, const char *filename);
int script_lua_close(struct osmocom_ms *ms);


/* Internal code. Don't call directly */
int mobile_exit(struct osmocom_ms *ms, int force);
#endif

