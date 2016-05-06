#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pmix.h>
#include <mercury.h>

#include "my_rpc_common.h"

static int get_uri(char **uri);

int main(int argc, char **argv)
{
	hg_id_t my_rpc_id;
	na_class_t *na_class = NULL;
	na_context_t *na_context = NULL;
	hg_class_t *hg_class = NULL;
	hg_context_t *hg_context = NULL;
	int ret = 0;
	unsigned int act_count = 0, total_count = 0;
	pmix_proc_t myproc, proc;
	int rc;
	bool flag;
	pmix_info_t *info;
	char *uri;

	get_uri(&uri);
	fprintf(stderr, "server uri: %s\n", uri);

	rc = PMIx_Init(&myproc, NULL, 0);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n",
			myproc.nspace, myproc.rank, rc);
		ret = rc;
		goto done;
	}

	PMIX_INFO_CREATE(info, 1);
	(void)strncpy(info[0].key, "server-addr", PMIX_MAX_KEYLEN);
	info[0].value.type = PMIX_STRING;
	info[0].value.data.string = strdup(uri);
	rc = PMIx_Publish(info, 1);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr,
			"Client ns %s rank %d: PMIx_Publish failed: %d\n",
			myproc.nspace, myproc.rank, rc);
		ret = rc;
		PMIX_INFO_FREE(info, 1);
		goto done;
	}

	PMIX_INFO_FREE(info, 1);

	PMIX_PROC_CONSTRUCT(&proc);
	(void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
	proc.rank = PMIX_RANK_WILDCARD;
	PMIX_INFO_CREATE(info, 1);
	flag = true;
	PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
	rc = PMIx_Fence(&proc, 1, info, 1);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr,
			"Client ns %s rank %d: PMIx_Fence failed: %d\n",
			myproc.nspace, myproc.rank, rc);
		ret = rc;
		PMIX_INFO_FREE(info, 1);
		goto done;
	}

	PMIX_INFO_FREE(info, 1);

	na_class = NA_Initialize(uri, NA_TRUE);
	free(uri);
	assert(na_class);
	na_context = NA_Context_create(na_class);
	assert(na_context);
	hg_class = HG_Init_na(na_class, na_context);
	assert(hg_class);
	hg_context = HG_Context_create(hg_class);
	assert(hg_context);

	my_rpc_id = HG_Register_name(hg_class,
				     "rpc_test",
				     my_in_proc_cb,
				     my_out_proc_cb, my_rpc_test_handler);

	printf("Id registered on Server is %u\n", my_rpc_id);

	while (1) {
		do {
			ret = HG_Trigger(hg_context, 0, 1, &act_count);
			total_count += act_count;
		} while (ret == HG_SUCCESS && act_count);
		HG_Progress(hg_context, 100);
		if (test_bulk_cb_done_g)
			break;
	}

	rc = PMIx_Finalize(NULL, 0);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr,
			"Client ns %s rank %d: PMIx_Finalize failed: %d\n",
			myproc.nspace, myproc.rank, rc);
		ret = rc;
		goto done;
	}

done:
	HG_Context_destroy(hg_context);
	HG_Finalize(hg_class);
	NA_Context_destroy(na_class, na_context);
	NA_Finalize(na_class);

	return ret;
}

static int get_uri(char **uri)
{
	int socketfd;
	struct sockaddr_in tmp_socket;
	char name[256 + 10 + 1];

	socklen_t slen = sizeof(struct sockaddr);

#ifdef __APPLE__
	/* This is a bit of a hack because of the way that OS X handles
	 * hostnames, however it does allow the code to run on a single
	 * apple machine.
	 */
	char *hname = "localhost";
#else
	char hname[256];

	gethostname(hname, 256);
#endif

	socketfd = socket(AF_INET, SOCK_STREAM, 0);
	tmp_socket.sin_family = AF_INET;
	tmp_socket.sin_addr.s_addr = INADDR_ANY;
	tmp_socket.sin_port = 0;

	bind(socketfd,
	     (const struct sockaddr *)&tmp_socket, sizeof(tmp_socket));
	getsockname(socketfd, (struct sockaddr *)&tmp_socket, &slen);
	snprintf(name, 256 + 10 + 1,
		 "bmi+tcp://%s:%d", hname, ntohs(tmp_socket.sin_port));
	*uri = strndup(name, 256 + 10);
	close(socketfd);

	return 0;
}
