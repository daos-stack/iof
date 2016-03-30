#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "include/process_set.h"
#include "proto_common.h"

static int mcl_get_uri(char **uri)
{
	int socketfd;
	struct sockaddr_in tmp_socket;
	char name[MCL_URI_LEN_MAX + 1];
	char hname[HOST_NAME_MAX + 1];
	socklen_t slen = sizeof(struct sockaddr);
	int rc;

	socketfd = socket(AF_INET, SOCK_STREAM, 0);
	if (socketfd == -1)
		return MCL_ERR_URI_GEN;
	tmp_socket.sin_family = AF_INET;
	tmp_socket.sin_addr.s_addr = INADDR_ANY;
	tmp_socket.sin_port = 0;

	rc = bind(socketfd, (const struct sockaddr *) &tmp_socket,
		   sizeof(tmp_socket));
	if (rc != 0) {
		close(socketfd);
		return MCL_ERR_URI_GEN;
	}
	rc = getsockname(socketfd, (struct sockaddr *) &tmp_socket, &slen);
	if (rc != 0) {
		close(socketfd);
		return MCL_ERR_URI_GEN;
	}
	rc = gethostname(hname, HOST_NAME_MAX);
	if (rc != 0) {
		close(socketfd);
		return MCL_ERR_URI_GEN;
	}
	snprintf(name, MCL_URI_LEN_MAX + 1, "bmi+tcp://%s:%d", hname,
		 ntohs(tmp_socket.sin_port));
	*uri = strndup(name, MCL_URI_LEN_MAX);
	if (*uri == NULL)
		return MCL_ERR_NOMEM;
	rc = close(socketfd);
	if (rc != 0) {
		close(socketfd);
		free(*uri);
		*uri = NULL;
		return MCL_ERR_URI_GEN;
	}

	return MCL_SUCCESS;
}

static int mcl_fence(pmix_proc_t *myproc)
{
	pmix_proc_t proc;
	pmix_info_t *info;
	int rc;
	bool flag;

	PMIX_PROC_CONSTRUCT(&proc);
	strncpy(proc.nspace, myproc->nspace, PMIX_MAX_NSLEN);
	proc.rank = PMIX_RANK_WILDCARD;
	PMIX_INFO_CREATE(info, 1);
	flag = true;
	PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
	rc = PMIx_Fence(&proc, 1, info, 1);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n",
			myproc->nspace, myproc->rank, rc);
		PMIX_INFO_FREE(info, 1);
		return MCL_ERR_PMIX_FAILED;
	}
	PMIX_INFO_FREE(info, 1);

	return MCL_SUCCESS;
}

void mcl_set_free(na_class_t *na_class, struct mcl_set *mcl_set_p)
{
	int ii;

	for (ii = 0; ii < mcl_set_p->size; ii++) {
		if (mcl_set_p->cached[ii].visited == 1) {
			NA_Addr_free(na_class, mcl_set_p->cached[ii].na_addr);
			free(mcl_set_p->cached[ii].uri);
		}
	}
	pthread_mutex_destroy(&mcl_set_p->lookup_lock);
	free(mcl_set_p->name);
	free(mcl_set_p);
}

struct mcl_state *mcl_init(char **uri)
{
	struct mcl_state *mystate;
	pmix_value_t *val;
	int rc;

	mystate = (struct mcl_state *) calloc(1, sizeof(struct mcl_state));
	if (mystate == NULL)
		return NULL;
	rc = mcl_get_uri(uri);
	if (rc != MCL_SUCCESS) {
		free(mystate);
		return NULL;
	}
	strncpy(mystate->self_uri, *uri, MCL_URI_LEN_MAX);
	rc = PMIx_Init(&mystate->myproc, NULL, 0);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n",
			mystate->myproc.nspace, mystate->myproc.rank, rc);
		free(mystate);
		return NULL;
	}
	/* get our universe size */
	rc = PMIx_Get(&mystate->myproc, PMIX_UNIV_SIZE, NULL, 0, &val);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr,
			"Client ns %s rank %d: PMIx_Get universe size failed: %d\n",
			mystate->myproc.nspace, mystate->myproc.rank, rc);
		free(mystate);
		return NULL;
	}
	mystate->univ_size = val->data.uint32;
	mystate->num_sets = 0;
	PMIX_VALUE_RELEASE(val);

	return (struct mcl_state *) mystate;
}

int mcl_startup(struct mcl_state *mystate, char *my_set_name, int is_service,
		struct mcl_set **set)
{
	struct mcl_set *myset;
	pmix_info_t *info;
	int ret;
	int rc;
	int ii;
	int jj = 0;
	pmix_pdata_t *pdata;

	*set = NULL;
	myset = (struct mcl_set *) calloc(1, sizeof(struct mcl_set));
	if (myset == NULL)
		return MCL_ERR_ALLOC;
	/*
	* every process publishes its own address string using (PMIx_rank,
	* setname/addrString)
	*/
	PMIX_INFO_CREATE(info, 2);
	snprintf(info[0].key, PMIX_MAX_KEYLEN + 1, "%s-%d-psname",
		 mystate->myproc.nspace, mystate->myproc.rank);
	info[0].value.type = PMIX_STRING;
	info[0].value.data.string =
		strndup(my_set_name, MCL_NAME_LEN_MAX);
	if (info[0].value.data.string == NULL) {
		PMIX_INFO_FREE(info, 2);
		free(myset);
		return MCL_ERR_NOMEM;
	}
	snprintf(info[1].key, PMIX_MAX_KEYLEN + 1, "%s-%d-uri",
		 mystate->myproc.nspace, mystate->myproc.rank);
	info[1].value.type = PMIX_STRING;
	info[1].value.data.string =
		strndup(mystate->self_uri, MCL_URI_LEN_MAX);
	if (info[1].value.data.string == NULL) {
		PMIX_INFO_FREE(info, 2);
		free(myset);
		return MCL_ERR_NOMEM;
	}
	if (is_service == 1)
		rc = PMIx_Publish(info, 2);
	else
		rc = PMIx_Publish(info, 1);
	if (rc != PMIX_SUCCESS) {
		fprintf(stderr,
			"Client ns %s rank %d: PMIx_Publish failed: %d\n",
			mystate->myproc.nspace, mystate->myproc.rank, rc);
		mcl_set_free(NULL, myset);
		PMIX_INFO_FREE(info, 2);
		free(myset);
		return MCL_ERR_PMIX_FAILED;
	}
	PMIX_INFO_FREE(info, 2);
	/* call fence to ensure the data is received */
	rc = mcl_fence(&mystate->myproc);
	if (rc != MCL_SUCCESS) {
		ret = rc;
		free(myset);
		return ret;
	}
	/* loop over universe size, parse address string and set size */
	PMIX_PDATA_CREATE(pdata, 1);
	for (ii = 0; ii < mystate->univ_size; ii++) {
		/* generate the key to query my process set name */
		snprintf(pdata[0].key, PMIX_MAX_KEYLEN + 1, "%s-%d-psname",
			 mystate->myproc.nspace, ii);
		rc = PMIx_Lookup(&pdata[0], 1, NULL, 0);
		if (rc != PMIX_SUCCESS) {
			PMIX_PDATA_FREE(pdata, 1);
			free(myset);
			return MCL_ERR_PMIX_FAILED;
		}
		/* store the set name locally and create rank mapping */
		for (jj = 0; jj < mystate->num_sets; jj++) {
			/* have encountered this set before */
			if (strncmp(pdata[0].value.data.string,
				    mystate->psnames[jj],
				    MCL_NAME_LEN_MAX) == 0) {
				mystate->mapping[jj][mystate->size_of_set[jj]]
					= ii;
				if (mystate->myproc.rank == ii)
					myset->self =  mystate->size_of_set[jj];

				mystate->size_of_set[jj]++;
				break;
			}
		}
		/* create a new entry for newly found set names */
		if (jj == mystate->num_sets) {
			if (mystate->num_sets >= MCL_NUM_SETS_MAX) {
				PMIX_PDATA_FREE(pdata, 1);
				free(myset);
				return MCL_ERR_TOO_MANY_SETS;
			}
			mystate->psnames[jj] =
				strndup(pdata[0].value.data.string,
						MCL_NAME_LEN_MAX);
			if (mystate->psnames[jj] == NULL) {
				PMIX_PDATA_FREE(pdata, 1);
				free(myset);
				return MCL_ERR_NOMEM;
			}
			mystate->num_sets++;
			/* create the local rank <--> pmix_rank maping */
			mystate->mapping[jj][mystate->size_of_set[jj]] = ii;
			if (mystate->myproc.rank == ii)
				myset->self =  mystate->size_of_set[jj];
			mystate->size_of_set[jj]++;
		}
	}
	PMIX_PDATA_FREE(pdata, 1);
	/* determine if it's a service set or client set */
	PMIX_PDATA_CREATE(pdata, 1);
	for (ii = 0; ii < mystate->num_sets; ii++) {
		snprintf(pdata[0].key, PMIX_MAX_KEYLEN + 1, "%s-%d-uri",
			 mystate->myproc.nspace, mystate->mapping[ii][0]);
		rc = PMIx_Lookup(&pdata[0], 1, NULL, 0);
		if (rc == PMIX_SUCCESS)
			mystate->is_service_set[ii] = 1;
		else
			mystate->is_service_set[ii] = 0;
		if (strncmp(my_set_name, mystate->psnames[ii],
			    MCL_NAME_LEN_MAX) == 0)
			myset->mapping_index = ii;
	}
	PMIX_PDATA_FREE(pdata, 1);

	myset->is_local = 1;
	myset->size = mystate->size_of_set[myset->mapping_index];
	myset->is_service = is_service;
	myset->name = strndup(my_set_name, MCL_NAME_LEN_MAX);
	if (myset->name == NULL) {
		free(myset);
		return MCL_ERR_NOMEM;
	}
	myset->state = mystate;
	rc = pthread_mutex_init(&myset->lookup_lock, NULL);
	if (rc != 0) {
		free(myset->name);
		free(myset);
	}
	*set = myset;

	return MCL_SUCCESS;
}

int mcl_attach(struct mcl_state *state, char *remote_set_name,
	       struct mcl_set **set)
{
	struct mcl_state *mystate;
	int ii;
	struct mcl_set *myset;
	int rc;

	*set = NULL;
	myset = (struct mcl_set *) calloc(1, sizeof(struct mcl_set));
	if (myset == NULL)
		return MCL_ERR_ALLOC;
	myset->name = strndup(remote_set_name, MCL_NAME_LEN_MAX);
	if (myset->name == NULL) {
		free(myset);
		return MCL_ERR_NOMEM;
	}
	myset->is_local = 0;
	myset->is_service = 1;
	myset->state = state;
	mystate = state;

	for (ii = 0; ii < mystate->num_sets; ii++) {
		if (strncmp(remote_set_name, mystate->psnames[ii],
			    MCL_NAME_LEN_MAX) == 0) {
			myset->mapping_index = ii;
			myset->size = mystate->size_of_set[ii];
			break;
		}
	}
	if (ii == mystate->num_sets) {
		fprintf(stderr,
			"Error on %s: %d target process set doesn't exist\n",
			__FILE__, __LINE__);
		free(myset->name);
		free(myset);
		return MCL_ERR_INVALID_SET_NAME;
	}

	rc = pthread_mutex_init(&myset->lookup_lock, NULL);
	if (rc != 0) {
		free(myset->name);
		free(myset);
		return MCL_ERR_PTHREAD_FAILED;
	}
	*set = myset;

	return MCL_SUCCESS;
}

int mcl_lookup(struct mcl_set *dest_set, int rank,
	       na_class_t *na_class, na_addr_t *addr_p)
{
	/*
	 * every process keeps a rank mapping for every attached process set.
	 */
	int grank;
	pmix_pdata_t *pdata;
	int rc;
	struct mcl_state *mystate;

	if (rank >= dest_set->size)
		return MCL_ERR_INVALID_RANK;

	mystate = dest_set->state;
	grank = dest_set->state->mapping[dest_set->mapping_index][rank];
	rc = pthread_mutex_lock(&dest_set->lookup_lock);
	if (rc != 0)
		return MCL_ERR_PTHREAD_FAILED;
	if (dest_set->cached[rank].visited) {
		*addr_p = dest_set->cached[rank].na_addr;
		rc = pthread_mutex_unlock(&dest_set->lookup_lock);
		if (rc != 0)
			return MCL_ERR_PTHREAD_FAILED;
		return MCL_SUCCESS;
	}
	PMIX_PDATA_CREATE(pdata, 1);
	snprintf(pdata[0].key, PMIX_MAX_NSLEN + 5, "%s-%d-uri",
		 mystate->myproc.nspace, grank);
	rc = PMIx_Lookup(&pdata[0], 1, NULL, 0);
	if (rc != PMIX_SUCCESS) {
		pthread_mutex_unlock(&dest_set->lookup_lock);
		PMIX_PDATA_FREE(pdata, 1);
		fprintf(stderr,
			"Client ns %s rank %d: PMIx_Lookup failed: %d\n",
			mystate->myproc.nspace, mystate->myproc.rank, rc);
		return MCL_ERR_PMIX_FAILED;
	}
	my_na_addr_lookup_wait(na_class, pdata[0].value.data.string, addr_p);
	dest_set->cached[rank].na_addr =  *addr_p;
	dest_set->cached[rank].uri = strndup(pdata[0].value.data.string,
					     MCL_URI_LEN_MAX);
	if (!dest_set->cached[rank].uri) {
		PMIX_PDATA_FREE(pdata, 1);
		return MCL_ERR_NOMEM;
	}
	dest_set->cached[rank].visited = 1;
	PMIX_PDATA_FREE(pdata, 1);
	rc = pthread_mutex_unlock(&dest_set->lookup_lock);
	if (rc != 0)
		return MCL_ERR_PTHREAD_FAILED;

	return MCL_SUCCESS;
}

int mcl_finalize(struct mcl_state *state)
{
	int ret;
	int rc;
	struct mcl_state *mystate;
	int ii;

	ret = MCL_SUCCESS;
	mystate = state;
	rc = PMIx_Finalize(NULL, 0);
	if (rc != PMIX_SUCCESS) {
		ret = MCL_ERR_PMIX_FAILED;
		fprintf(stderr,
			"Client ns %s rank %d:PMIx_Finalize failed: %d\n",
			mystate->myproc.nspace, mystate->myproc.rank, rc);
	}
	for (ii = 0; ii < mystate->num_sets; ii++)
		free(mystate->psnames[ii]);
	free(mystate);

	return ret;
}
