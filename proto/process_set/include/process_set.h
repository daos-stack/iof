#ifndef PROCESS_SET_H
#define PROCESS_SET_H

#include <stdbool.h>
#include <pthread.h>
#include <mercury.h>
#include <pmix.h>

/**
 * Initial API for creating and accessing primary service process sets and
 * client process sets.
 *
 * This is a prototype intended for developing ideas.
 */

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 128
#endif
#define MCL_NAME_LEN_MAX 256
#define MCL_NUM_SETS_MAX 1024
#define MCL_PS_SIZE_MAX 1024

/** Error codes */
#define MCL_SUCCESS			 0
#define MCL_ERR_ALLOC			-1 /**< for the malloc family */
#define MCL_ERR_TOO_MANY_SETS		-2
#define MCL_ERR_INVALID_SET_NAME	-3
#define MCL_ERR_PMIX_FAILED		-4
#define MCL_ERR_URI_GEN			-5
#define MCL_ERR_NOMEM			-6 /**< for strdup/strndup */
#define MCL_ERR_PTHREAD_FAILED		-7 /**< for pthred calls */
#define MCL_ERR_INVALID_RANK		-8

/** POSIX HOST_NAME_MAX + bmi+tcp://...:... */
#define MCL_URI_LEN_MAX (HOST_NAME_MAX + 11)

/** Global handle for process-local state */
struct mcl_state {
	na_class_t	*na_class;
	na_context_t	*na_context;
	uint32_t	univ_size;
	char		self_uri[MCL_URI_LEN_MAX + 1];
	/**< names of all existing process sets */
	char		*psnames[MCL_NUM_SETS_MAX];
	/**< number of all existing process sets */
	int		num_sets;
	/**< local rank <--> global rank mappings of existing process sets */
	int		mapping[MCL_NUM_SETS_MAX][MCL_PS_SIZE_MAX];
	/**< sizes of all existing process sets */
	int		size_of_set[MCL_NUM_SETS_MAX];
	/**< wether a set is a service set or not */
	int		is_service_set[MCL_NUM_SETS_MAX];
	pmix_proc_t	myproc;
};

/** cached info about a remote rank */
struct uri_entry {
	int		visited;
	/**< na_addr_t of the remote rank */
	na_addr_t	na_addr;
	/**< uri of the remote rank */
	char		*uri;
};

/**
 * Per process set handle, describes a process set which the current process may
 * either be a member of or attached to.
 */
struct mcl_set {
	int			size;	/**< size of the set */
	int			self;	/**< my rank in the process set */
	/**< index into the rank mapping array in *state below */
	int			mapping_index;
	/**< name of the set */
	char			*name;
	/**< points to the local process state  */
	struct			mcl_state *state;
	/**< cache URIs that have been looked up before */
	struct uri_entry	cached[MCL_PS_SIZE_MAX];
	/**< protects the cache */
	pthread_mutex_t		lookup_lock;
	/**< 1 -- service set, 0 -- client set */
	unsigned int		is_service:1;
	/**< 1 -- member of, 0 -- attached to */
	unsigned int		is_local:1;
};

/**
 * Initialize the MCL library, to be called once in every process on startup.
 * This is entirely a local operation and should do memory allocation etc.
 *
 * \param uri	[OUT]	On success, *uri points to an uri to be passed to
 *			Mercury.  The uri is in the format of
 *			plugin+protocol://host:port The caller is responsible to
 *			free this string.
 *
 * \return		Returns a pointer to a (struct mcl_state) structure. On
 *			error, returns a NULL pointer.
 */
struct mcl_state *mcl_init(char **uri);

/**
 * Form the initial process set, to be called once in every process on startup.
 *
 * This is a collective operation that needs to be called exactly once in every
 * process.  It performs network operations and may block until all other
 * processes have also called this function (in effect a barrier)
 *
 * Provided arguments are the name of the local process set and a boolean
 * indicating if this is a client or service process set.
 *
 * Returns a set descriptor for the process set of which this process is a
 * member.
 *
 * \param state		[IN]	process-local state
 * \param my_set_name	[IN]	the name of the process set
 * \param is_service	[IN]	wether this is a service set
 * \param set		[OUT]	On exit, *set points to a struct mcl_set
 *				structure. The memory must be free'ed using
 *				mcl_set_free() when no longer needed.
 *
 * \return			On success, returns MCL_SUCCESS
 */
int mcl_startup(struct mcl_state *state, char *my_set_name, int is_service,
		struct mcl_set **set);

/**
 * Attach to a remote service process set.
 *
 * The process set name provided should be that of a service process set which
 * was created through the mcl_startup() function.
 *
 * This process can be called multiple times if multiple service process sets
 * exist and should be called collectively across all members of the process set
 * performing the attach.
 *
 * This returns a descriptor of a remote process set.
 *
 * \param state			[IN]	process-local state
 * \param remote_set_name	[IN]	name of the target process set
 * \param set			[OUT]	On exit, *set points to a struct mcl_set
 *					structure. The caller is responsible to
 *					free the memory when it's no longer
 *					used.
 *
 * \return				On success, returns MCL_SUCCESS
 */
int mcl_attach(struct mcl_state *state, char *remote_set_name,
	       struct mcl_set **set);

/**
 * Deallocates the memory pointed to by a (struct mcl_set *) pointer.
 *
 * \param na_class	[IN]	Pointer returned by NA_Initialize(), which
 *				points to the na_class_t object.
 * \param mcl_set_p	[IN]	Memory pointed to by mcl_set_p is free'ed
 */

void mcl_set_free(na_class_t *na_class, struct mcl_set *mcl_set_p);

/**
 * Lookup the network address of the member of a process set.
 *
 * This takes a process set descriptor, either local or remote and returns the
 * Mercury handle for the remote endpoint.
 *
 * \param dest_set	[IN]	descriptor of the remote process set
 * \param rank		[IN]	local rank of the target process
 * \param na_class	[IN]	Mercury na_class of the calling process
 * \param addr		[OUT]	On exit, *addr points to the na address of the
 *				target process
 *
 * \return			On success, returns MCL_SUCCESS
 */
int mcl_lookup(struct mcl_set *dest_set, int rank, na_class_t *na_class,
	       na_addr_t *addr);

/**
 * Finalize the library, cleanup memory.
 *
 * \param state	[IN]	process-local state
 *
 */
int mcl_finalize(struct mcl_state *state);

#endif  /* PROCESS_SET_H */
