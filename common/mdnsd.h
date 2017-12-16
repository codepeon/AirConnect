#ifndef _MDNSD_H_
#define _MDNSD_H_

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>
#include <stdbool.h>

typedef void (*query_cb)(bool error, char *name, char *address, short port, void *userdata);
typedef void (*error_cb)(void *userdata);

struct mdnsd_userdata {
	struct raop_ctx_s *ctx;
	query_cb query_cb;
	error_cb error_cb;
};

int mdnsd_start();
void mdnsd_stop();

AvahiClient * mdnsd_register_service(struct mdnsd_userdata *userdata, bool lock);
AvahiClient * mdnsd_query_new(struct mdnsd_userdata *userdata, bool lock);
void mdnsd_free_handle(AvahiClient *client, bool lock);

#endif
