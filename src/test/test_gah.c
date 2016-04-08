#include <ios_gah.h>
#include <assert.h>

int main(void)
{
	int rc;
	struct ios_gah *ios_gah;
	char *ios_str;
	struct ios_gah_store *ios_gah_store;
	int ii;
	int num_handles = 1024*9;
	void *data = NULL;

	assert(sizeof(struct ios_gah)*8 == 128);
	ios_gah_store = ios_gah_init();
	if (ios_gah_store == NULL)
		return -1;
	ios_gah = (struct ios_gah *) calloc(num_handles, sizeof(struct
					      ios_gah));
	assert(ios_gah != NULL);

	for (ii = 0; ii < num_handles; ii++) {
		data = malloc(512);
		assert(data != NULL);
		rc = ios_gah_allocate(ios_gah_store, ios_gah + ii, 0, 0, data);
		if (rc != IOS_SUCCESS)
			return rc;
	}

	rc = ios_gah_check_crc(ios_gah);
	fprintf(stderr, "checkcrc %d\n", rc);
	if (rc != IOS_SUCCESS)
		return rc;
	rc = ios_gah_check_version(ios_gah);
	fprintf(stderr, "check_version %d\n", rc);
	if (rc != IOS_SUCCESS)
		return rc;
	rc = ios_gah_is_self_root(ios_gah, 0);
	fprintf(stderr, "am I root? %d\n", rc);
	if (rc != IOS_SUCCESS)
		return rc;
	ios_str = ios_gah_to_str(ios_gah);
	fprintf(stderr, "ios_gah[%5d] %s\n", 0, ios_str);
	free(ios_str);

	for (ii = 0; ii < num_handles; ii++) {
		free(ios_gah_store->ptr_array[(ios_gah + ii)->fid]->internal);
		if (ios_gah_deallocate(ios_gah_store, ios_gah + ii) !=
				IOS_SUCCESS) {
			return -1;
		}
	}

	ios_gah_destroy(ios_gah_store);
	free(ios_gah);

	return 0;
}
