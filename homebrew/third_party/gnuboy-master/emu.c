


#include "defs.h"
#include "regs.h"
#include "hw.h"
#include "cpu.h"
#include "mem.h"
#include "lcd.h"
#include "rc.h"
#include "rtc.h"
#include "sys.h"
#include "sound.h"
#include "cpu.h"


static int framelen = 16743;
static int framecount;
static int paused;

rcvar_t emu_exports[] =
{
	RCV_INT("framelen", &framelen, ""),
	RCV_INT("framecount", &framecount, ""),
	RCV_END
};



void emu_pause(int dopause) {
	paused = dopause;
}

int emu_paused(void) {
	return paused;
}

void emu_init()
{
	
}


/*
 * emu_reset is called to initialize the state of the emulated
 * system. It should set cpu registers, hardware registers, etc. to
 * their appropriate values at powerup time.
 */

void emu_reset()
{
	hw_reset();
	lcd_reset();
	cpu_reset();
	mbc_reset();
	sound_reset();
	mem_mapbootrom();
}





void emu_step()
{
	cpu_emulate(cpu.lcdc);
}



/* This mess needs to be moved to another module; it's just here to
 * make things work in the mean time. */

void *sys_timer();

void emu_run()
{
	void *timer = sys_timer();
	int lag = 0;	/* us behind the frame schedule (negative = ahead) */

	vid_begin();
	lcd_begin();
	for (;;)
	{
		cpu_emulate(2280);
		while (R_LY > 0 && R_LY < 144)
			emu_step();
		
		vid_end();
		rtc_tick();
		sound_mix();
		if (!pcm_submit())
		{
			/* pace to an absolute schedule: on a HZ=100 kernel usleep rounds up to 10-20 ms, and sleeping
			 * framelen - elapsed then dropping the overshoot ran games at ~57% speed. The overshoot now
			 * counts against the next frames: sleep only when > 10 ms ahead, resync when > 100 ms behind. */
			lag += sys_elapsed(timer) - framelen;
			if (lag > 100000) lag = 0;
			else if (lag < -10000) sys_sleep(-lag);
		}
		else { sys_elapsed(timer); lag = 0; }	/* the sound write paced this frame */
		doevents();
		if (paused) return;
		vid_begin();
		if (framecount) { if (!--framecount) die("finished\n"); }
		if (!(R_LCDC & 0x80))
			cpu_emulate(32832);
		
		while (R_LY > 0) /* wait for next frame */
			emu_step();
	}
}












