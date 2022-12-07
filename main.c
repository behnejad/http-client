#include <stdio.h>
#include <stdlib.h>
#include "mongoose.h"

const char * mg_ev_str[] = {
		"MG_EV_ERROR",
		"MG_EV_OPEN",
		"MG_EV_POLL",
		"MG_EV_RESOLVE",
		"MG_EV_CONNECT",
		"MG_EV_ACCEPT",
		"MG_EV_TLS_HS",
		"MG_EV_READ",
		"MG_EV_WRITE",
		"MG_EV_CLOSE",
		"MG_EV_HTTP_MSG",
		"MG_EV_HTTP_CHUNK",
		"MG_EV_WS_OPEN",
		"MG_EV_WS_MSG",
		"MG_EV_WS_CTL",
		"MG_EV_MQTT_CMD",
		"MG_EV_MQTT_MSG",
		"MG_EV_MQTT_OPEN",
		"MG_EV_SNTP_TIME",
		"MG_EV_USER",
};

typedef struct
{
	const char * url;
	int loop;
} config;

void mg_log_http(struct mg_http_message * http_msg)
{
	printf("<--- response --->\n\t%.*s %.*s %.*s %.*s\n",
		   (int) http_msg->method.len, http_msg->method.ptr,
		   (int) http_msg->uri.len, http_msg->uri.ptr,
		   (int) http_msg->proto.len, http_msg->proto.ptr,
		   (int) http_msg->query.len, http_msg->query.ptr);

	struct mg_str k, v, s = http_msg->query;
	printf("<--- params --->\n");

	while (mg_split(&s, &k, &v, '&'))
	{
		printf("\t%.*s -> %.*s\n", (int) k.len, k.ptr, (int) v.len, v.ptr);
	}

	printf("<--- params --->\n");
	printf("<--- headers --->\n");

	struct mg_http_header *hdr = http_msg->headers;
	while (hdr->name.len)
	{
		printf("\t%.*s -> %.*s\n", (int) hdr->name.len, hdr->name.ptr, (int) hdr->value.len, hdr->value.ptr);
		hdr++;
	}

	printf("<--- headers --->\n");
	printf("<--- chunk at %08X (%ld) --->\n", http_msg->chunk.ptr, http_msg->chunk.len);
	printf("<--- payload at %08X (%ld) --->\n", http_msg->body.ptr, http_msg->body.len);
	printf("<--- message at %08X (%ld) --->\n", http_msg->message.ptr, http_msg->message.len);
}

void http_response_handler(struct mg_connection *c, int ev, void *ev_data, void *fn_data)
{
	config * conf = (struct http_download_t *) fn_data;
	struct mg_http_message * hm = (struct mg_http_message *) ev_data;

	char time_str[30];
	if (ev != MG_EV_POLL)
	{
		time_t t = time(0);
		struct tm *time_info = localtime(&t);
		strftime(time_str, sizeof time_str, "%Y/%m/%d %H:%M:%S", time_info);
		printf("[%s.%d] ev: %s\n", time_str, t % 1000, mg_ev_str[ev]);
	}

	if (ev == MG_EV_CONNECT)
	{
		struct mg_str host = mg_url_host(conf->url);
		mg_printf(c,
				  "GET %s HTTP/1.0\r\n"
				  "Host: %.*s\r\n"
				  "User-Agent: Mongoose\r\n"
				  "\r\n",
				  mg_url_uri(conf->url), (int) host.len, host.ptr);
	}
	else if (ev == MG_EV_HTTP_CHUNK)
	{
		mg_log_http(hm);
		if (hm->chunk.len)
		{
			printf("%.*s", hm->chunk.len, hm->chunk.ptr);
		}
		mg_http_delete_chunk(c, hm);
	}
	else if (ev == MG_EV_HTTP_MSG)
	{
		if (hm->body.len)
		{
			printf("%.*s", hm->body.len, hm->body.ptr);
		}
		mg_log_http(hm);
	}
	else if (ev == MG_EV_CLOSE)
	{
		conf->loop = 0;
	}
}

int api_download(const char * url)
{
	struct mg_mgr mgr;
	config c = {.url = url, .loop = 1};
	mg_mgr_init(&mgr);
	mg_http_connect(&mgr, url, http_response_handler, &c);

	while (c.loop)
	{
		mg_mgr_poll(&mgr, 1000);
	}

	mg_mgr_free(&mgr);
	return 0;
}

int main()
{
	api_download("http://127.0.0.1:8000/doc.txt");
	puts("\n\n\n\n\n\n\n\n");
	api_download("http://google.com");
	return 0;
}

