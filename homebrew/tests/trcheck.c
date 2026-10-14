/* trcheck.c: one text per line on stdin -> its English (or the same text when no entry matches) on stdout */
#include "../hbplayer/hb.h"
#include <string.h>
void lg(const char *fmt, ...) { (void)fmt; }
int main(void)
{
	char l[1024];
	ui_lang_set(1);
	while (fgets(l, sizeof l, stdin)) {
		l[strcspn(l, "\r\n")] = 0;
		puts(ui_tr(l));
	}
	return 0;
}
