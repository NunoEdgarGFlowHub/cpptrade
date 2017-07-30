#ifndef __HTTPUTIL_H__
#define __HTTPUTIL_H__

#include <cstdint>
#include <string>
#include <time.h>
#include <evhtp.h>

struct HttpApiEntry {
	const char		*path;
	evhtp_callback_cb	cb;
	bool			wantInput;
	bool			jsonInput;
};

int64_t get_content_length (const evhtp_request_t *req);
std::string httpDateHdr(time_t t);
void httpJsonReply(evhtp_request_t *req, const UniValue& jval);

#endif // __HTTPUTIL_H__
