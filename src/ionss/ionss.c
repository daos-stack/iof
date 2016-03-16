
#include <stdio.h>
#include "version.h"

int main(void)
{
	char *version = iof_get_version();

	printf("IONSS version: %s\n", version);
	return 0;
}
