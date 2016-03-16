
#include <stdio.h>
#include "version.h"

int main(void)
{
	char *version = iof_get_version();

	printf("CNSS version: %s\n", version);
	return 0;
}
