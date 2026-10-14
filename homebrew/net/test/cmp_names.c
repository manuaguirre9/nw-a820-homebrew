/* compares rss_filename() with the PC script's names (<feed>.xml.py.txt, one per line, same item order) */
#include "../rss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
	int bad = 0;
	for (int a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb"); if (!f) return 1;
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		char *x = malloc(n); fread(x, 1, n, f); fclose(f);
		static struct rss_item it[50];
		int c = rss_parse(x, n, it, 50);
		char pyn[600]; snprintf(pyn, sizeof pyn, "%s.py.txt", argv[a]);
		FILE *py = fopen(pyn, "rb");
		char line[600]; int i = 0, same = 0;
		while (py && fgets(line, sizeof line, py) && i < c) {
			line[strcspn(line, "\r\n")] = 0;
			char fn[400]; rss_filename(&it[i], fn, sizeof fn);
			/* the device keeps .m4a/.aac extensions; compare the stem only */
			char *d1 = strrchr(fn, '.'), *d2 = strrchr(line, '.');
			if (d1) *d1 = 0; if (d2) *d2 = 0;
			if (!strcmp(fn, line)) same++; else { bad++; printf("DIFF\n  C : [%s]\n  PY: [%s]\n", fn, line); }
			i++;
		}
		printf("%s: %d/%d identical\n", argv[a], same, i);
		if (py) fclose(py);
	}
	return bad != 0;
}
