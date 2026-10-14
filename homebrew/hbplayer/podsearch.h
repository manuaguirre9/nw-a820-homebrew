/* podsearch.h: finding podcasts by name in the Apple Podcasts directory (see podsearch.c) */
#ifndef PODSEARCH_H
#define PODSEARCH_H
#include <stddef.h>
#define POD_RESULTS 15
struct pod_result { char name[96], author[64], genre[40], feed[512], art[384]; int count; };
extern char pod_country[4];
void pod_search_url(const char *term, char *out, size_t max);
void pod_art_url(const char *art, int px, char *out, size_t max);
int pod_search_parse(const char *json, size_t len, struct pod_result *res, int max);
void pod_folder_name(const char *name, char *out, size_t max);
#endif
