#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>

enum ios_return {
	IOS_SUCCESS = 0,
	IOS_ERR_INVALID_PARAM,		/**< Invalid parameter */
	IOS_ERR_CRC_MISMATCH,		/**< CRC mismatch */
	IOS_ERR_VERSION_MISMATCH,	/**< version mismatch */
	IOS_ERR_NOMEM,			/**< Memory not available */
	IOS_ERR_OUT_OF_RANGE,		/**< Handle ID out of range */
	IOS_ERR_EXPIRED,		/**< Handle has expired */
	IOS_ERR_OTHER			/**< All other errors */
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
/** 128 bit GAH handle */
struct ios_gah {
	uint64_t revision:48;	/**< 0-based revision NO. of the fid */
	uint8_t root;		/**< The rank who owns the GAH */
	uint8_t base;		/**< Rank who owns the first byte of the file */
	uint8_t version;	/**< The version of the protocol */
	uint64_t fid:24;	/**< The file id of the file*/
	uint64_t reserved:24;	/**< Reserved for future use */
	uint8_t crc;		/**< CRC of the rest of the structure */
};
#pragma GCC diagnostic pop

/** metadata associated with the file */
struct ios_gah_ent {
	void *internal;
	struct ios_gah_ent *next;
	uint64_t in_use;
	uint64_t revision;
	uint64_t fid;	/**< The file id of the file*/
};

/**
 * structure with dynamically-sized storage to keep the file metadata.
 */
struct ios_gah_store {
	/** number of used slots */
	int size;
	/** total number of slots including used and unused */
	int capacity;
	/** storage for the actual file entries */
	struct ios_gah_ent *data;
	/** array of pointers to file entries */
	struct ios_gah_ent **ptr_array;
	/** list of available file entries */
	struct ios_gah_ent avail_entries;
	/** pointer to the tail of the list of free entries */
	struct ios_gah_ent *tail;
};

/**
 * initialize struct ios_gah_ent ios_gah_store.
 */
struct ios_gah_store *ios_gah_init(void);

/**
 * \param ios_gah_store [IN]		Global access handle data structure.
 *
 * return				On success, returns IOS_SUCCESS
 */
enum ios_return ios_gah_destroy(struct ios_gah_store *ios_gah_store);

/**
 * Allocates a global access handle, computes the crc.
 *
 * \param ios_gah_store [IN]		Global access handle data structure.
 * \param gah		[OUT]		On exit, *gah contains a global access
 *					handle.
 * \param self_rank	[IN]		Rank of the calling process.
 * \param base		[IN]		Rank of the process which serves the
 *					first byte of the file.
 *
 * \return				On success, returns IOS_SUCCESS.
 */
enum ios_return ios_gah_allocate(struct ios_gah_store *ios_gah_store,
		struct ios_gah *gah, int self_rank, int base, void *internal);

/**
 * Deallocates a global access handle.
 *
 * \param ios_gah_store [IN]		Global access handle data structure.
 * \param gah		[IN/OUT]	On exit, the global access handle
 *					contained in *gah is marked available
 *					again.
 *
 * \return				On success, returns IOS_SUCCESS.
 */
enum ios_return ios_gah_deallocate(struct ios_gah_store *ios_gah_store,
		struct ios_gah *gah);

/**
 * Retrieve opaque data structure corresponding to a given global access handle.
 *
 * \param gah_store [IN]		Global access handle data structure
 * \param gah [IN]			Global access handle
 * \param info [OUT]			On success, *info contains the opaque
 *					data struture associated with gah. On
 *					failure, equals NULL
 *
 * \return				On success returns IOS_SUCCESS
 */
enum ios_return ios_gah_get_info(struct ios_gah_store *gah_store,
		struct ios_gah *gah, void **internal);
/**
 * Validates if the crc in *gah is correct.
 *
 * \param gah		[IN]		On entry, *gah contains a global access
 *					handle.
 * \return				crc is correct ---  returns 0
 *					crc is incorrect ---  returns 1
 *					NULL input --- returns
 *					IOS_ERR_INVALID_PARAM
 */
enum ios_return ios_gah_check_crc(struct ios_gah *gah);

/**
 * Validates if the version in *gah and the and the version of the protocol in
 * use match.
 *
 * \param gah		[IN]		On entry, *gah contains a global access
 *					handle.
 * \return				version match ---  returns 0
 *					version mismatch ---  returns 1
 *					NULL input --- returns
 *					IOS_ERR_INVALID_PARAM
 */
enum ios_return ios_gah_check_version(struct ios_gah *gah);

/**
 * Check if the calling process is the root of the global access handle.
 *
 * \param gah		[IN]		On entry, contains a global access
 *					handle.
 * \param self_rank	[IN]		The rank of the calling process.
 *
 * \return				Returns 0 if I am the root, 1 if I am
 *					not.
 */
enum ios_return ios_gah_is_self_root(struct ios_gah *gah, int self_rank);

/**
 * Debugging utility. Converts the global access handle to a string for print.
 *
 * \param gah		[IN]		On entry, contains a global access
 *					handle.
 * \return				A string format of the global access
 *					handle.
 */
char *ios_gah_to_str(struct ios_gah *gah);
