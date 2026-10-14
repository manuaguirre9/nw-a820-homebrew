#ifndef RSS_H
#define RSS_H
#include <stddef.h>
struct rss_item { char title[160]; char url[1024]; char date[12]; long bytes;
	char transcript[384]; char tr_ext[6]; char chapters[384]; };	/* Podcasting 2.0 (may be empty) */
int rss_parse(const char *xml, size_t len, struct rss_item *items, int max);	/* items in feed order (newest first) */
int rss_channel_image(const char *xml, size_t len, char *url, size_t max);	/* show artwork URL (itunes:image or <image><url>) */
void rss_filename(const struct rss_item *it, char *out, size_t max);
#endif
