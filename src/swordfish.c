#include <config.h>

#include <sys/queue.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include <event.h>
#include <evhttp.h>

#include <tchdb.h>
#include <tcutil.h>

#include "swordfish.h"

struct stats stats;
struct config config;

extern char *optarg;

TCHDB *db = NULL;

void
send_reply(struct evhttp_request *request, struct evbuffer *databuf, int errorcode, const char *reason)
{

	evhttp_add_header(request->output_headers,
		"Content-Type", "text/plain"); // "application/json");
	evhttp_add_header(request->output_headers,
		"Server", PACKAGE_NAME "/" PACKAGE_VERSION);
	evhttp_send_reply(request, errorcode, reason, databuf);

#ifdef DEBUG
	const char *method;

	switch (request->type) {
	case EVHTTP_REQ_GET:
		method = "GET";
		break;
	case EVHTTP_REQ_POST:
		method = "POST";
		break;
	case EVHTTP_REQ_HEAD:
		method = "HEAD";
		break;
	}
	
	fprintf(stderr, "[%s] %s %s %d\n",
		request->remote_host, method, request->uri, errorcode);
#endif
}

void
append_json_value(struct evbuffer *databuf, const char *value)
{
	int pos = 0;
	unsigned char c;

	evbuffer_add_printf(databuf, "\"");

	do {
		c = value[pos];

		switch (c) {
		case '\0':
			break;
		case '\b':
			evbuffer_add_printf(databuf, "\\b");
			break;
		case '\n':
			evbuffer_add_printf(databuf, "\\n");
			break;
		case '\r':
			evbuffer_add_printf(databuf, "\\r");
			break;
		case '\t':
			evbuffer_add_printf(databuf, "\\t");
			break;
		case '"':
			evbuffer_add_printf(databuf, "\\\"");
			break;
		case '\\':
			evbuffer_add_printf(databuf, "\\\\");
			break;
		case '/': evbuffer_add_printf(databuf, "\\/");
			break;
		default:
			if (c < ' ') {
				evbuffer_add_printf(databuf, "\\u00%c%c",
					"0123456789abcdef"[c >> 4],
					"0123456789abcdef"[c & 0xf]
				);
			} else {
				evbuffer_add_printf(databuf, "%c", c);
			}
		}

		++pos;
	} while (c);

	evbuffer_add_printf(databuf, "\"");
}

void
handler_sync(struct evhttp_request *request)
{
	struct evbuffer *databuf = evbuffer_new();

	switch (request->type) {
	case EVHTTP_REQ_POST:
		tchdbsync(db);
		evbuffer_add_printf(databuf, "true\n");
		REPLY_OK(request, databuf);
		break;
	default:
		evbuffer_add_printf(databuf,
			"{\"err\": \"/sync should be POST\"}\n");
		REPLY_BADMETHOD(request, databuf);
	}

	evbuffer_free(databuf);
}

void
handler_stats(struct evhttp_request *request)
{
	struct stat file_status;
	struct evbuffer *databuf = evbuffer_new();

	char *db_realpath = tcrealpath(tchdbpath(db));
	if (stat(db_realpath, &file_status)) {
		perror("stat");
		goto fail;
	}

	evbuffer_add_printf(databuf, "{");
	evbuffer_add_printf(databuf,   "\"started\": %lu", stats.started);
	evbuffer_add_printf(databuf, ", \"total_cmds\": %lu", stats.total_cmds);
	evbuffer_add_printf(databuf, ", \"version\": \"%s\"", PACKAGE_VERSION);
	evbuffer_add_printf(databuf, ", \"database\": \"%s\"", db_realpath);
	evbuffer_add_printf(databuf, ", \"num_items\": %zu", tchdbrnum(db));
	evbuffer_add_printf(databuf, ", \"database_bytes\": %jd", (intmax_t)file_status.st_size);
	evbuffer_add_printf(databuf, ", \"tokyocabinet_version\": \"%s\"", tcversion);
	evbuffer_add_printf(databuf, "}\n");

	REPLY_OK(request, databuf);

	goto end;
fail:	
	REPLY_INTERR(request, databuf);
end:
	free(db_realpath);
	evbuffer_free(databuf);
}

void
handler_tree_intersection(struct evhttp_request *request, char *left_key, char *right_key, int count, int skip, int limit)
{
	int size;
	int cmp_val;
	int result_count = 0;

	char* value = NULL;
	const char* left_val = NULL;
	const char* right_val = NULL;

	TCTREE *left = NULL;
	TCTREE *right = NULL;

	struct evbuffer *databuf = evbuffer_new();

	value = tchdbget(db, left_key, strlen(left_key), &size);
	if (!value) {
		goto no_items;
	}
	left = tctreeload(value, size, SWORDFISH_KEY_CMP, NULL);
	free(value);

	value = tchdbget(db, right_key, strlen(right_key), &size);
	if (!value) {
		goto no_items;
	}
	right = tctreeload(value, size, SWORDFISH_KEY_CMP, NULL);
	free(value);

	evbuffer_add_printf(databuf, (count) ? "{\"count\": " : "{\"items\": [");

	tctreeiterinit(left);
	tctreeiterinit(right);

	left_val = tctreeiternext2(left);
	right_val = tctreeiternext2(right);

	if (limit == 0) {
		limit = -1;
	}

	while (left_val && right_val)
	{
		if (result_count == limit)
			break;

		cmp_val = SWORDFISH_KEY_CMP(left_val, strlen(left_val),
			right_val, strlen(right_val), NULL);

		switch ((cmp_val > 0) - (cmp_val < 0))
		{
		case 0:
			/* left == right; Element intersects */
			if (skip == 0) {
				if (!count) {
					if (result_count)
						evbuffer_add_printf(databuf, ", ");
					append_json_value(databuf, left_val);
				}

				++result_count;
			} else {
				/* Skip this element */
				skip--;
			}

			left_val = tctreeiternext2(left);
			right_val = tctreeiternext2(right);
			break;

		case -1:
			/* left < right */
			left_val = tctreeiternext2(left);
			break;

		case 1:
			/* left > right */
			right_val = tctreeiternext2(right);
			break;
		}

	}

	if (count) {
		evbuffer_add_printf(databuf, "%d}", result_count);
	} else {
		evbuffer_add_printf(databuf, "]}");
	}

	REPLY_OK(request, databuf);

	goto end;

no_items:
	evbuffer_add_printf(databuf,
		(count) ? "{\"count\": 0}" : "{\"items\": []}");
	REPLY_OK(request, databuf);

end:
	if (left) tctreedel(left);
	if (right) tctreedel(right);

	evbuffer_free(databuf);
	++stats.total_cmds;
}

void
handler_tree_set_item(struct evhttp_request *request, char *tree_key, char *value_key)
{
	int size;
	int ecode;
	int result_count = 0;

	int rawtree_size;
	void *rawtree;

	struct evbuffer *databuf = evbuffer_new();

	TCTREE *tree = NULL;
	rawtree = tchdbget(db, tree_key, strlen(tree_key), &rawtree_size);

	if (rawtree) {
		tree = tctreeload(rawtree, rawtree_size, SWORDFISH_KEY_CMP, NULL);
		free(rawtree);
	} else {
		ecode = tchdbecode(db);

		if (ecode != TCENOREC) {
			evbuffer_add_printf(databuf,
				"{\"msg\": \"%s\"}", tchdberrmsg(ecode));
			REPLY_INTERR(request, databuf);
			goto end;
		}

		tree = tctreenew2(SWORDFISH_KEY_CMP, NULL);
	}

	if (request->ntoread) {
		evbuffer_add_printf(databuf,
			"{\"err\": \"Not enough POST data\"}");
		REPLY_BADMETHOD(request, databuf);
		goto end;
	}

	size = EVBUFFER_LENGTH(request->input_buffer);

	if (size)
		tctreeput(tree, value_key, strlen(value_key),
			EVBUFFER_DATA(request->input_buffer), size);
	else
		tctreeout2(tree, value_key);

	rawtree = tctreedump(tree, &size);
	if (!tchdbput(db, tree_key, strlen(tree_key), rawtree, size)) {
		ecode = tchdbecode(db);
		free(rawtree);

		evbuffer_add_printf(databuf,
			"{\"msg\": \"%s\"}", tchdberrmsg(ecode));
		REPLY_INTERR(request, databuf);
		goto end;
	}

	free(rawtree);

	evbuffer_add_printf(databuf, "true");
	REPLY_OK(request, databuf);

end:
	if (tree)
		tctreedel(tree);

	evbuffer_free(databuf);
	++stats.total_cmds;
}

void
handler_tree_get_item(struct evhttp_request *request, char *tree_key, char *value_key)
{
	int ecode;
	int result_count = 0;

	int rawtree_size;
	char *rawtree;

	const char *value;

	struct evbuffer *databuf = evbuffer_new();

	TCTREE *tree = NULL;
	rawtree = tchdbget(db, tree_key, strlen(tree_key), &rawtree_size);

	if (!rawtree) {
		ecode = tchdbecode(db);

		if (ecode != TCENOREC) {
			evbuffer_add_printf(databuf,
				"{\"msg\": \"%s\"}", tchdberrmsg(ecode));
			REPLY_INTERR(request, databuf);
			goto end;
		}

		evbuffer_add_printf(databuf, "false");
		REPLY_NOTFOUND(request, databuf);
		goto end;
	}

	tree = tctreeload(rawtree, rawtree_size, SWORDFISH_KEY_CMP, NULL);
	free(rawtree);

	value = tctreeget2(tree, value_key);

	if (!value) {
		evbuffer_add_printf(databuf, "false");
		REPLY_NOTFOUND(request, databuf);
		goto end;
	}

	evbuffer_add_printf(databuf, "{\"item\": ");
	append_json_value(databuf, value);
	evbuffer_add_printf(databuf, "}");
	REPLY_OK(request, databuf);

end:
	if (tree)
		tctreedel(tree);

	evbuffer_free(databuf);
	++stats.total_cmds;
}

void
handler_tree_get(struct evhttp_request *request, char *key, int count, int skip, int limit, int just)
{
	int ecode;
	int result_count = 0;

	int rawtree_size;
	char *rawtree;

	const char *keyval;

	struct evbuffer *databuf = evbuffer_new();

	TCTREE *tree = NULL;
	rawtree = tchdbget(db, key, strlen(key), &rawtree_size);

	if (!rawtree) {
		ecode = tchdbecode(db);

		if (ecode != TCENOREC) {
			evbuffer_add_printf(databuf,
				"{\"msg\": \"%s\"}", tchdberrmsg(ecode));
			REPLY_INTERR(request, databuf);
			goto end;
		}

		evbuffer_add_printf(databuf,
			(count) ? "{\"count\": 0}" : "{\"items\": []}");

		REPLY_OK(request, databuf);
		
		goto end;
	}

	tree = tctreeload(rawtree, rawtree_size, SWORDFISH_KEY_CMP, NULL);
	free(rawtree);

	if (count) {
		evbuffer_add_printf(databuf,
			"{\"count\": %llu}", (long long)tctreernum(tree));
		REPLY_OK(request, databuf);
		goto end;
	}

	evbuffer_add_printf(databuf, "{\"items\": [");

	tctreeiterinit(tree);
	while ((keyval = tctreeiternext2(tree)) != NULL) {
		if (skip-- > 0)
			continue;

		switch (just) {
		case VALUES_KEYS:
			if (result_count)
				evbuffer_add_printf(databuf, ",");
			append_json_value(databuf, keyval);
			break;
		case VALUES_VALUES:
			if (result_count)
				evbuffer_add_printf(databuf, ",");
			append_json_value(databuf, tctreeget2(tree, keyval));
			break;
		case VALUES_ALL:
			evbuffer_add_printf(databuf,
				(result_count == 0) ? "[" : ",[");
			append_json_value(databuf, keyval);
			evbuffer_add_printf(databuf, ",");
			append_json_value(databuf, tctreeget2(tree, keyval));
			evbuffer_add_printf(databuf, "]");
		}

		if (++result_count == limit)
			break;
	}

	evbuffer_add_printf(databuf, "]}");

	REPLY_OK(request, databuf);

end:
	if (tree)
		tctreedel(tree);

	evbuffer_free(databuf);
	++stats.total_cmds;
}

int
get_values_value(struct evkeyvalq *querystr)
{
	const char *c = evhttp_find_header(querystr, "values");

	if (!c)
		return VALUES_ALL;

	if (strcmp(c, "values") == 0)
		return VALUES_VALUES;

	if (strcmp(c, "keys") == 0)
		return VALUES_KEYS;

	return VALUES_ALL;
}

int
lookup(const char *resource)
{
	int i;
	int num_cmds = sizeof(lookup_table) / sizeof(*lookup_table);

	if (!resource)
		return RESOURCE_NONE;

	for (i = 0; i != num_cmds; ++i) {
		if (strcmp(resource, lookup_table[i]) == 0)
			return i;
	}

	return RESOURCE_UNKNOWN;
}

int
get_int_header(struct evkeyvalq *querystr, const char *header, int def)
{
	const char *c;
	
	c = evhttp_find_header(querystr, header);

	return (c && *c) ? atoi(c) : def;
}

void
request_handler(struct evhttp_request *request, void *arg)
{
	int just;

	char *uri;
	char *saveptr;	

	char *tree;
	char *arg_1;

	struct evkeyvalq querystr;
	struct evbuffer *databuf = evbuffer_new();

	TAILQ_INIT(&querystr);

	uri = strdup(request->uri);

	/* parse query string and then strip it off */
	evhttp_parse_query(uri, &querystr);
	strtok(uri, "?");

	switch (lookup(strtok_r(uri, "/", &saveptr))) {

	case RESOURCE_SYNC:
		handler_sync(request);
		break;

	case RESOURCE_NONE:
	case RESOURCE_STATS:
		handler_stats(request);
		break;

	case RESOURCE_TREES:
		tree = strtok_r(NULL, "/", &saveptr);
		if (!tree) {
			/* no tree specified */
			goto notfound;
		}

		switch (lookup(strtok_r(NULL, "/", &saveptr))) {

		case RESOURCE_NONE:
			handler_tree_get(request, tree, 0,
				get_int_header(&querystr, "skip", 0),
				get_int_header(&querystr, "limit", 0),
				get_values_value(&querystr));
			break;

		case RESOURCE_ITEM:
			if ((arg_1 = strtok_r(NULL, "/", &saveptr)) == NULL)
				goto notfound;

			switch (request->type) {
			case EVHTTP_REQ_POST:
				handler_tree_set_item(request, tree, arg_1);
				break;
			default:
				handler_tree_get_item(request, tree, arg_1);
			}
			break;

		case RESOURCE_COUNT:
			handler_tree_get(request, tree, 1,
				get_int_header(&querystr, "skip", 0),
				get_int_header(&querystr, "limit", 0),
				VALUES_ALL);
			break;

		case RESOURCE_INTERSECTION:
			/* return intersection of `tree` and `arg_1` */
			if ((arg_1 = strtok_r(NULL, "/", &saveptr)) == NULL)
				goto notfound;

			switch (lookup(strtok_r(NULL, "/", &saveptr))) {
			case RESOURCE_NONE:
				handler_tree_intersection(request, tree, arg_1, 0,
					get_int_header(&querystr, "skip", 0),
					get_int_header(&querystr, "limit", 0));
				break;

			case RESOURCE_COUNT:
				handler_tree_intersection(request, tree, arg_1, 1, 0, -1);
				break;
				
			default:
				/* unknown subcommand */
				goto notfound;
			}
			break;

		case RESOURCE_MAP:
			/* map `tree` onto `arg_1` */
			if ((arg_1 = strtok_r(NULL, "/", &saveptr)) == NULL)
				goto notfound;
			REPLY_OK(request, databuf);
			break;

		default:
			/* unknown subcommand */
			goto notfound;
		}

		break;

	default:
		/* unknown command */
		goto notfound;
	}

	goto end;

notfound:
	REPLY_NOTFOUND(request, databuf);

end:
	free(uri);
	evbuffer_free(databuf);
	evhttp_clear_headers(&querystr);
}

void
usage(const char *progname)
{
	fprintf(stderr,
		"%s\n"
		"\t -D database    database file\n"
		"\t -i interface   interface to run server on\n"
		"\t -P port        port number to run server on\n"
		"\t -p pidfile     daemonise and write pid to <pidfile>\n"
		"\t -v             enable verbose mode\n",
		progname);
}

void
stats_init(void)
{
	stats.started = time(NULL);
	stats.total_cmds = 0;
}

void
exit_handler(void)
{
	int ecode;

	if (db && (!tchdbclose(db))) {
		ecode = tchdbecode(db);
		fprintf(stderr, "tchdbdel: %s\n", tchdberrmsg(ecode));
		exit(EXIT_FAILURE);
	}

	tchdbdel(db);

	if (config.pidfile)
		unlink(config.pidfile);

	_exit(EXIT_SUCCESS);
}

static void
sig_handler(const int sig)
{
	if (sig != SIGTERM && sig != SIGQUIT && sig != SIGINT)
		return;

	fprintf(stderr, "Caught signal %d, exiting...\n", sig);

	exit_handler();

	exit(EXIT_SUCCESS);
}

int
main(int argc, char** argv)
{
	int ch;
	int ecode;
	struct evhttp *http_server = NULL;

	/* set defaults */
	config.host = "127.0.0.1";
	config.port = 2929;
	config.database = "swordfish.db";
	config.pidfile = NULL;

	while ((ch = getopt(argc, argv, "p:P:D:")) != -1)
		switch(ch) {
		case 'p':
			config.pidfile = optarg;
			break;
		case 'P':
			config.port = atoi(optarg);
			if (!config.port) {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			config.host = optarg;
			break;
		case 'D':
			config.database = optarg;
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}

	event_init();
	stats_init();

	db = tchdbnew();
	if (!tchdbopen(db, config.database, HDBOWRITER | HDBOCREAT)) {
		ecode = tchdbecode(db);
		fprintf(stderr, "Open fail: %s\n", tchdberrmsg(ecode));
		exit(EXIT_FAILURE);
	}

	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	if ((sigemptyset(&sa.sa_mask) == -1) || (sigaction(SIGPIPE, &sa, 0) == -1)) {
		perror("failed to ignore SIGPIPE; sigaction");
		exit(EXIT_FAILURE);
	}

	if (signal(SIGTERM, sig_handler) == SIG_ERR)
		fprintf(stderr, "Can not catch SIGTERM\n");
	if (signal(SIGQUIT, sig_handler) == SIG_ERR)
		fprintf(stderr, "Can not catch SIGQUIT\n");
	if (signal(SIGINT, sig_handler) == SIG_ERR)
		fprintf(stderr, "Can not catch SIGINT\n");

	if (atexit(exit_handler)) {
		fprintf(stderr, "Could not register atexit(..)\n");
		exit(EXIT_FAILURE);
	}

	if ((http_server = evhttp_start(config.host, config.port)) == NULL) {
		fprintf(stderr,
			"Cannot listen on http://%s:%d/; exiting..\n",
			config.host, config.port);
		exit(EXIT_FAILURE);
	}

	evhttp_set_gencb(http_server, request_handler, NULL);

	if (config.pidfile) {
		FILE *fp;
		int fd;

		switch (fork()) {
		case -1:
			perror("fork");
			return EXIT_FAILURE;
		case 0:
			break;
		default:
			return EXIT_SUCCESS;
		}	

		if (setsid() == -1) {
			perror("setsid");
			return EXIT_FAILURE;
		}

		if (chdir("/") == -1) {
			perror("chdir");
			return EXIT_FAILURE;
		}

		fd = open("/dev/null", O_RDWR, 0);
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		close(fd);

		fp = fopen(config.pidfile, "w");
		fprintf(fp, "%d\n", getpid());
		fclose(fp);
	}

	fprintf(stderr, "Listening on http://%s:%d/ ...\n",
		config.host, config.port);

	event_dispatch();

	return EXIT_SUCCESS;
}
