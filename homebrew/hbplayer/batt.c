/* batt.c: battery charge from the power IC's ADC, as measured on this unit (see battery_update in hbplayer.c) */
#include "hb.h"

static const short batt_curve[][2] = { { 224, 100 }, { 220, 91 }, { 216, 81 }, { 212, 68 }, { 208, 55 }, { 204, 36 },
	{ 200, 18 }, { 196, 10 }, { 190, 4 }, { 184, 2 }, { 177, 0 } };
int batt_pct(double adc)
{
	int n = sizeof batt_curve / sizeof batt_curve[0];
	if (adc >= batt_curve[0][0]) return 100;
	for (int i = 1; i < n; i++)
		if (adc >= batt_curve[i][0]) {
			double a = batt_curve[i][0], b = batt_curve[i - 1][0];
			return (int)(batt_curve[i][1] + (batt_curve[i - 1][1] - batt_curve[i][1]) * (adc - a) / (b - a) + 0.5);
		}
	return 0;
}
