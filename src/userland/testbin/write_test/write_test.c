#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <unistd.h>

int main() {
	char buf[256] = "Swaggy lifestyle\n";
	write(STDOUT_FILENO, buf, 256);
}
