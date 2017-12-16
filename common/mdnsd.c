#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>

#include <avahi-common/alternative.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#include <avahi-common/thread-watch.h>

#include "log_util.h"
#include "mdnsd.h"
#include "raopcore.h"

static AvahiThreadedPoll *threaded_poll = NULL;

int mdnsd_start() {
	if (!(threaded_poll = avahi_threaded_poll_new())) {
		LOG_ERROR("Failed to create simple poll object.\n");
		return -1;
	}

	avahi_threaded_poll_start(threaded_poll);

	return 0;
}

void mdnsd_stop() {
	avahi_threaded_poll_stop(threaded_poll);

	if (threaded_poll)
		avahi_threaded_poll_free(threaded_poll);
}

static void service_resolver_callback(
		AvahiServiceResolver *r,
		AvahiIfIndex interface,
		AvahiProtocol protocol,
		AvahiResolverEvent event,
		const char *name,
		const char *type,
		const char *domain,
		const char *host_name,
		const AvahiAddress *a,
		uint16_t port,
		AvahiStringList *txt,
		AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
		void *userdata) {
	struct mdnsd_userdata *ud = userdata;
	char address[AVAHI_ADDRESS_STR_MAX];

	switch (event) {
	case AVAHI_RESOLVER_FOUND:
		avahi_address_snprint(address, sizeof(address), a);
		if (ud->query_cb) ud->query_cb(false, (char *)name, address, port, ud->ctx);
		break;

	case AVAHI_RESOLVER_FAILURE:
		LOG_ERROR("Failed to resolve service '%s' of type '%s' in domain '%s'\n",
				name, type, domain);
		if (ud->query_cb) ud->query_cb(true, NULL, NULL, 0, ud->ctx);
		break;
	}
}

static void service_browser_callback(
		AvahiServiceBrowser *b,
		AvahiIfIndex interface,
		AvahiProtocol protocol,
		AvahiBrowserEvent event,
		const char *name,
		const char *type,
		const char *domain,
		AvahiLookupResultFlags flags,
		void *userdata) {
	struct mdnsd_userdata *ud = userdata;

	switch (event) {
	case AVAHI_BROWSER_NEW:
		if (!(avahi_service_resolver_new(avahi_service_browser_get_client(b),
						interface, protocol, name, type, domain,
						AVAHI_PROTO_INET, 0, service_resolver_callback, userdata))) {
			LOG_ERROR("Failed to resolve service '%s' of type '%s' in domain '%s'\n",
					name, type, domain);
			if (ud->query_cb) ud->query_cb(true, NULL, NULL, 0, ud->ctx);
			return;
		}
		break;

	case AVAHI_BROWSER_FAILURE:
		if (ud->query_cb) ud->query_cb(true, NULL, NULL, 0, ud->ctx);
		break;

	case AVAHI_BROWSER_REMOVE:
	case AVAHI_BROWSER_CACHE_EXHAUSTED:
	case AVAHI_BROWSER_ALL_FOR_NOW:
		break;
	}
}

static void browser_client_callback(
		AvahiClient *client,
		AvahiClientState state,
		void * userdata) {
	struct mdnsd_userdata *ud = userdata;

	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		if (!avahi_service_browser_new(client, AVAHI_IF_UNSPEC,
					AVAHI_PROTO_INET, "_dacp._tcp", NULL, 0,
					service_browser_callback, userdata)) {
			LOG_ERROR("avahi_service_browser_new() failed: %s\n",
					avahi_strerror(avahi_client_errno(client)));
			if (ud->query_cb) ud->query_cb(true, NULL, NULL, 0, ud->ctx);
		}
		break;

	case AVAHI_CLIENT_FAILURE:
		if (ud->query_cb) ud->query_cb(true, NULL, NULL, 0, ud->ctx);
		break;

	case AVAHI_CLIENT_S_COLLISION:
	case AVAHI_CLIENT_S_REGISTERING:
	case AVAHI_CLIENT_CONNECTING:
	default:
		break;
	}
}

AvahiClient * mdnsd_query_new(struct mdnsd_userdata *userdata, bool lock) {
	int error;
	if (lock) avahi_threaded_poll_lock(threaded_poll);
	AvahiClient *client = avahi_client_new(avahi_threaded_poll_get(threaded_poll),
			AVAHI_CLIENT_NO_FAIL, browser_client_callback, userdata, &error);
	if (lock) avahi_threaded_poll_unlock(threaded_poll);
	if (!client) {
		LOG_ERROR("Failed to create client: %s\n", avahi_strerror(error));
	}
	return client;
}

static void entry_group_callback(
		AvahiEntryGroup *g,
		AvahiEntryGroupState state,
		void *userdata) {
	switch (state) {
	case AVAHI_ENTRY_GROUP_FAILURE :
		LOG_ERROR("Entry group failure: %s\n",
				avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
		break;

	case AVAHI_ENTRY_GROUP_ESTABLISHED:
	case AVAHI_ENTRY_GROUP_COLLISION:
	case AVAHI_ENTRY_GROUP_UNCOMMITED:
	case AVAHI_ENTRY_GROUP_REGISTERING:
		break;
	}
}

static void publish_client_callback(
		AvahiClient *client,
		AvahiClientState state,
		void * userdata) {
	static char *txt[] = {
		"tp=UDP", "sm=false", "sv=false", "ek=1",
		"et=0,1", "md=0,1,2", "cn=0,1", "ch=2",
		"ss=16", "sr=44100", "vn=3", "txtvers=1", NULL
	};
	struct mdnsd_userdata *ud = userdata;
	struct raop_ctx_s *ctx = (struct raop_ctx_s *)ud->ctx;
	AvahiEntryGroup *group;
	AvahiStringList *list;
	int ret;

	switch (state) {
	case AVAHI_CLIENT_S_RUNNING:
		group = avahi_entry_group_new(client, entry_group_callback, NULL);
		if (!group) {
			LOG_ERROR("avahi_entry_group_new() failed: %s\n",
					avahi_strerror(avahi_client_errno(client)));
			return;
		}

		list = avahi_string_list_new_from_array((const char **)txt, -1);
		list = avahi_string_list_add_printf(list, "am=%s", raop_get_model(ctx));

		ret = avahi_entry_group_add_service_strlst(group, AVAHI_IF_UNSPEC,
				AVAHI_PROTO_INET, 0, raop_get_name(ctx), "_raop._tcp", NULL, NULL,
				raop_get_port(ctx), list);
		avahi_string_list_free(list);
		if (ret < 0) {
			LOG_ERROR("Failed to add _raop._tcp service: %s\n", avahi_strerror(ret));
			avahi_entry_group_free(group);
			return;
		}

		ret = avahi_entry_group_commit(group);
		if (ret < 0) {
			LOG_ERROR("Failed to commit entry group: %s\n", avahi_strerror(ret));
			avahi_entry_group_free(group);
			return;
		}

		break;

	case AVAHI_CLIENT_FAILURE:
		if (ud->error_cb) ud->error_cb(ud->ctx);
		break;

	case AVAHI_CLIENT_S_COLLISION:
	case AVAHI_CLIENT_S_REGISTERING:
	case AVAHI_CLIENT_CONNECTING:
	default:
		break;
	}
}

AvahiClient * mdnsd_register_service(struct mdnsd_userdata *userdata, bool lock) {
	int error;
	if (lock) avahi_threaded_poll_lock(threaded_poll);
	AvahiClient *client = avahi_client_new(avahi_threaded_poll_get(threaded_poll),
			AVAHI_CLIENT_NO_FAIL, publish_client_callback, userdata, &error);
	if (lock) avahi_threaded_poll_unlock(threaded_poll);
	if (!client) {
		LOG_ERROR("Failed to create client: %s\n", avahi_strerror(error));
	}
	return client;
}

void mdnsd_free_handle(AvahiClient *client, bool lock) {
	if (lock) avahi_threaded_poll_lock(threaded_poll);
	avahi_client_free(client);
	if (lock) avahi_threaded_poll_unlock(threaded_poll);
}
