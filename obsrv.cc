
#include "cscpp-config.h"

#include <string>
#include <vector>
#include <map>
#include <locale>
#include <stdio.h>
#include <stdlib.h>
#include <univalue.h>
#include <time.h>
#include <argp.h>
#include <evhtp.h>
#include <ctype.h>
#include <assert.h>
#include "Market.h"
#include "Util.h"
#include "HttpUtil.h"
#include "srvapi.h"
#include "srv.h"

using namespace std;
using namespace orderentry;

#define PROGRAM_NAME "obsrv"

static const char doc[] =
PROGRAM_NAME " - order book server";

static struct argp_option options[] = {
	{ "config", 'c', "json-file", 0,
	  "JSON server configuration file (default: config-obsrv.json)" },
	{ }
};

static error_t parse_opt (int key, char *arg, struct argp_state *state);
static const struct argp argp = { options, parse_opt, NULL, doc };

static std::string opt_configfn = "config-obsrv.json";
static UniValue serverCfg;

Market market;

static void
logRequest(evhtp_request_t *req, ReqState *state)
{
	assert(req != NULL);
	assert(state != NULL);

	// IP address
	string addrStr = addressToStr(req->conn->saddr,
				      sizeof(struct sockaddr)); // TODO verify

	// request timestamp.  use request-completion (not request-start)
	// time instead?
	struct tm tm;
	gmtime_r(&state->tstamp.tv_sec, &tm);

	// get http method, build timestamp str
	string timeStr = isoTimeStr(state->tstamp.tv_sec);
	htp_method method = evhtp_request_get_method(req);
	const char *method_name = htparser_get_methodstr_m(method);

	// output log line
	printf("%s - - [%s] \"%s %s\" ? %lld\n",
		addrStr.c_str(),
		timeStr.c_str(),
		method_name,
		req->uri->path->full,
		(long long) get_content_length(req));
}

static evhtp_res
upload_read_cb(evhtp_request_t * req, evbuf_t * buf, void * arg)
{
	ReqState *state = (ReqState *) arg;
	assert(state != NULL);

	// remove data from evbuffer to malloc'd buffer
	size_t bufsz = evbuffer_get_length(buf);
	char *chunk = (char *) malloc(bufsz);
	int rc = evbuffer_remove(buf, chunk, bufsz);
	assert(rc == (int) bufsz);

	// append chunk to total body
	state->body.append(chunk, bufsz);

	// release malloc'd buffer
	free(chunk);

	return EVHTP_RES_OK;
}

static evhtp_res
req_finish_cb(evhtp_request_t * req, void * arg)
{
	ReqState *state = (ReqState *) arg;
	assert(state != NULL);

	// log request, following processing
	logRequest(req, state);

	// release our per-request state
	delete state;

	return EVHTP_RES_OK;
}

static void reqInit(evhtp_request_t *req, ReqState *state)
{
	// standard Date header
	evhtp_headers_add_header(req->headers_out,
		evhtp_header_new("Date",
			 httpDateHdr(state->tstamp.tv_sec).c_str(),
			 0, 1));

	// standard Server header
	const char *serverVer = PROGRAM_NAME "/" PACKAGE_VERSION;
	evhtp_headers_add_header(req->headers_out,
		evhtp_header_new("Server", serverVer, 0, 0));

	// assign our global (to a request) state
	req->cbarg = state;

	// assign request completion hook
	evhtp_request_set_hook (req, evhtp_hook_on_request_fini, (evhtp_hook) req_finish_cb, state);
}

static evhtp_res
upload_headers_cb(evhtp_request_t * req, evhtp_headers_t * hdrs, void * arg)
{
	// handle OPTIONS
	if (evhtp_request_get_method(req) == htp_method_OPTIONS) {
		return EVHTP_RES_OK;
	}

	// alloc new per-request state
	ReqState *state = new ReqState();
	assert(state != NULL);

	// common per-request state
	reqInit(req, state);

	// special incoming-data hook
	evhtp_request_set_hook (req, evhtp_hook_on_read, (evhtp_hook) upload_read_cb, state);

	return EVHTP_RES_OK;
}

static evhtp_res
no_upload_headers_cb(evhtp_request_t * req, evhtp_headers_t * hdrs, void * arg)
{
	// handle OPTIONS
	if (evhtp_request_get_method(req) == htp_method_OPTIONS) {
		return EVHTP_RES_OK;
	}

	// alloc new per-request state
	ReqState *state = new ReqState();
	assert(state != NULL);

	// common per-request state
	reqInit(req, state);

	return EVHTP_RES_OK;
}

void reqDefault(evhtp_request_t * req, void * a)
{
	evhtp_send_reply(req, EVHTP_RES_NOTFOUND);
}

void reqInfo(evhtp_request_t * req, void * a)
{
	UniValue obj(UniValue::VOBJ);

	// some information about this server
	obj.pushKV("name", "obsrv");
	obj.pushKV("apiversion", 100);
	obj.pushKV("unixtime", time(NULL));

	// successful operation.  Return JSON output.
	httpJsonReply(req, obj);
}

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'c':
		opt_configfn.assign(arg);
		break;

	case ARGP_KEY_END:
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static bool read_config_init()
{
	if (!readJsonFile(opt_configfn, serverCfg)) {
		perror(opt_configfn.c_str());
		return false;
	}

	if (!serverCfg.exists("bindAddress"))
		serverCfg.pushKV("bindAddress", "0.0.0.0");
	if (!serverCfg.exists("bindPort"))
		serverCfg.pushKV("bindPort", (int64_t) 7979);

	return true;
}

static std::vector<struct HttpApiEntry> apiRegistry = {
	{ "/info", reqInfo, false, false },
	{ "/marketAdd", reqMarketAdd, true, true },
	{ "/marketList", reqMarketList, false, false },
	{ "/book", reqOrderBookList, true, true },
	{ "/orderAdd", reqOrderAdd, true, true },
	{ "/orderCancel", reqOrderCancel, true, true },
	{ "/orderModify", reqOrderModify, true, true },
};

int main(int argc, char ** argv)
{
	// parse command line
	error_t argp_rc = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (argp_rc) {
		fprintf(stderr, "%s: argp_parse failed: %s\n",
			argv[0], strerror(argp_rc));
		return EXIT_FAILURE;
	}

	// read json configuration, and initialize early defaults
	if (!read_config_init())
		return EXIT_FAILURE;

	// initialize libevent, libevhtp
	evbase_t * evbase = event_base_new();
	evhtp_t  * htp    = evhtp_new(evbase, NULL);
	evhtp_callback_t *cb = NULL;

	// default callback, if not matched
	evhtp_set_gencb(htp, reqDefault, NULL);

	// register our list of API calls and their handlers
	for (auto& it : apiRegistry) {
		const struct HttpApiEntry& apiEnt = it;

		// register evhtp hook
		cb = evhtp_set_cb(htp, apiEnt.path, apiEnt.cb, NULL);

		// set standard per-callback initialization hook
		evhtp_callback_set_hook(cb, evhtp_hook_on_headers,
			apiEnt.wantInput ?
				((evhtp_hook) upload_headers_cb) :
				((evhtp_hook) no_upload_headers_cb), NULL);
	}

	// bind to socket and start server main loop
	evhtp_bind_socket(htp,
			  serverCfg["bindAddress"].getValStr().c_str(),
			  atoi(serverCfg["bindPort"].getValStr().c_str()),
			  1024);
	event_base_loop(evbase, 0);
	return 0;
}
