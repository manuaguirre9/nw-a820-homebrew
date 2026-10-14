#include "../rss.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *x = malloc(n); fread(x, 1, n, f); fclose(f);
	static struct rss_item it[50];
	int c = rss_parse(x, n, it, 50);
	printf("%d items\n", c);
	for (int i = 0; i < c && i < 4; i++) { char fn[256]; rss_filename(&it[i], fn, sizeof fn); printf("  %s | %ld bytes\n    %s\n    -> %s\n", it[i].date, it[i].bytes, it[i].url, fn); }
	return 0;
}
