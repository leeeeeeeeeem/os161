#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <unistd.h>

int main() {
	char buf[256];
	read(0, buf, 256);
	printf("Read from stdin: %s", buf);
}
