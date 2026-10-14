/* alarm.c: wake-up alarm. The unit sleeps in S3 with an RTC wake alarm (/dev/misc/rtc, RTC_WKALM_SET, the same
 * mechanism the Sony UI uses for its hourly wake); after the wake-up the player rings: it resumes what was
 * playing last with the volume rising over a minute, any key stops the ramp. Also fires while awake. */
#include "hb.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct alarm_settings alarm_cfg = { 0, 7, 30, 0 };

struct rtc_time { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
struct rtc_wkalrm { unsigned char enabled, pending; struct rtc_time time; };
#define RTC_RD_TIME 0x80247009
#define RTC_WKALM_SET 0x4028700F

#define RTC_SET_TIME 0x4024700A

/* Clock drift correction from an HTTP "Date:" header (UTC, RFC 1123, e.g. "Tue, 29 Sep 2026 18:50:08 GMT").
 * The unit keeps local time as if it were UTC (no timezone set), so the zone is taken from the difference itself,
 * rounded to 15 minutes; only the remainder (the drift) is corrected, and only when it's between 2 s and 7 min:
 * the user's clock setting (zone, and anything bigger) is never touched. System clock and RTC both. */
void clock_from_http(const char *date)
{
	static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	char wd[8], ms[8]; struct tm tm; memset(&tm, 0, sizeof tm);
	if (sscanf(date, "%7[^,], %d %7s %d %d:%d:%d", wd, &tm.tm_mday, ms, &tm.tm_year, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 7) return;
	tm.tm_mon = -1;
	for (int i = 0; i < 12; i++) if (!strcmp(ms, mon[i])) tm.tm_mon = i;
	if (tm.tm_mon < 0 || tm.tm_year < 2020) return;
	tm.tm_year -= 1900;
	time_t utc = timegm(&tm), dev = time(0);
	long diff = (long)(dev - utc);
	long zone = (diff >= 0 ? diff + 450 : diff - 450) / 900 * 900;
	long drift = diff - zone;	/* > 0: the Walkman is ahead */
	if (labs(drift) < 2) return;
	if (labs(drift) > 420) { lg("clock: %ld s off (zone %+ld min), not corrected", drift, zone / 60); return; }
	time_t fixed = utc + zone;
	struct timeval tv = { fixed, 0 };
	if (settimeofday(&tv, 0)) { lg("clock: settimeofday failed"); return; }
	struct tm g; gmtime_r(&fixed, &g);
	struct rtc_time rt = { g.tm_sec, g.tm_min, g.tm_hour, g.tm_mday, g.tm_mon, g.tm_year, g.tm_wday, g.tm_yday, 0 };
	int fd = open("/dev/misc/rtc", O_RDONLY);
	if (fd >= 0) { if (ioctl(fd, RTC_SET_TIME, &rt) < 0) lg("clock: rtc set failed"); close(fd); }
	lg("clock: corrected %+ld s (zone %+ld min)", -drift, zone / 60);
}

/* the RTC and the system clock share the same base here (hwclock at boot, no timezone set) */
time_t alarm_next(void)
{
	time_t t = time(0);
	struct tm tm = *localtime(&t);
	tm.tm_hour = alarm_cfg.hour; tm.tm_min = alarm_cfg.min; tm.tm_sec = 0;
	time_t a = mktime(&tm);
	if (a <= t + 30) a += 86400;
	return a;
}

/* program (or clear) the RTC wake alarm; call right before suspending */
void alarm_program_rtc(void)
{
	int fd = open("/dev/misc/rtc", O_RDONLY);
	if (fd < 0) { lg("alarm: no rtc"); return; }
	struct rtc_wkalrm w;
	memset(&w, 0, sizeof w);
	if (alarm_cfg.on) {
		time_t a = alarm_next();
		struct tm tm = *localtime(&a);
		w.enabled = 1;
		w.time.tm_sec = 0; w.time.tm_min = tm.tm_min; w.time.tm_hour = tm.tm_hour;
		w.time.tm_mday = tm.tm_mday; w.time.tm_mon = tm.tm_mon; w.time.tm_year = tm.tm_year;
		w.time.tm_wday = tm.tm_wday; w.time.tm_yday = tm.tm_yday; w.time.tm_isdst = -1;
	}
	if (ioctl(fd, RTC_WKALM_SET, &w) < 0) lg("alarm: RTC_WKALM_SET failed");
	else lg("alarm: rtc wake %s", alarm_cfg.on ? "set" : "cleared");
	close(fd);
}

/* 1 if the alarm is due now (within 2 minutes after its time) */
int alarm_due(void)
{
	if (!alarm_cfg.on) return 0;
	time_t t = time(0);
	struct tm tm = *localtime(&t);
	int now_m = tm.tm_hour * 60 + tm.tm_min, al_m = alarm_cfg.hour * 60 + alarm_cfg.min;
	int d = now_m - al_m;
	if (d < 0) d += 1440;
	return d >= 0 && d < 2;
}

void alarm_load(const char *line)	/* "on hour min src" */
{
	int on, h, m, s;
	if (sscanf(line, "%d %d %d %d", &on, &h, &m, &s) == 4 && h >= 0 && h < 24 && m >= 0 && m < 60) {
		alarm_cfg.on = !!on; alarm_cfg.hour = h; alarm_cfg.min = m; alarm_cfg.src = !!s;
	}
}

/* small time editor: LEFT/RIGHT choose hours/minutes, UP/DOWN change, PLAY accept, BACK cancel */
static void edit_time(void)
{
	int h = alarm_cfg.hour, m = alarm_cfg.min, field = 0, redraw = 1;
	for (;;) {
		if (redraw && backlight_on) {	/* drawn only after a change (it used to redraw every 300 ms) */
			char b[16];
			scr_screen("Despertador");
			snprintf(b, sizeof b, "%02d:%02d", h, m);
			gfx_text_center(W / 2, 120, b, C_WHITE, 2);
			gfx_fill(field ? 130 : 78, 160, 34, 3, C_ORANGE);
			gfx_text_center(W / 2, 200, "arriba/abajo: cambiar", C_GREY, 1);
			gfx_text_center(W / 2, 222, "izq/der: horas / minutos", C_GREY, 1);
			gfx_text_center(W / 2, 244, "PLAY: guardar", C_GREY, 1);
			gfx_flip();
			redraw = 0;
		}
		idle_tick(0);	/* backlight timeout, suspend, battery like every other screen */
		int k = getkey(1000);	/* getkey feeds the audio meanwhile */
		if (k == K_REDRAW) { redraw = 1; continue; }
		if (k < 0) continue;
		redraw = 1;
		if (k == K_LEFT || k == K_RIGHT) field = !field;
		else if (k == K_UP) { if (field) m = (m + 5) % 60; else h = (h + 1) % 24; }
		else if (k == K_DOWN) { if (field) m = (m + 55) % 60; else h = (h + 23) % 24; }
		else if (k == K_PLAY) { alarm_cfg.hour = h; alarm_cfg.min = m; alarm_cfg.on = 1; settings_save(); return; }
		else if (k == K_BACK) return;
	}
}

static char it[3][40], vl[3][24];
static const char *lab(int i) { return it[i]; }
static const char *val(int i) { return vl[i]; }

void alarm_menu(void)
{
	struct list_view v = { "Despertador", 3, 0, 0, lab, val, 0, LI_NONE, 0, 0 };
	for (;;) {
		snprintf(it[0], 40, "Despertador"); snprintf(vl[0], 24, "%s", alarm_cfg.on ? "activado" : "apagado");
		snprintf(it[1], 40, "Hora"); snprintf(vl[1], 24, "%02d:%02d", alarm_cfg.hour, alarm_cfg.min);
		snprintf(it[2], 40, "Sonar con"); snprintf(vl[2], 24, "%s", alarm_cfg.src ? "campanas" : "lo último");
		int s = list_run(&v);
		if (s < 0) return;
		if (s == 0) { alarm_cfg.on = !alarm_cfg.on; settings_save(); }
		else if (s == 1) edit_time();
		else { alarm_cfg.src = !alarm_cfg.src; settings_save(); }
	}
}
