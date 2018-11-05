#ifndef _SENSORS_H_
#define _SENSORS_H_

/* HIH Honeywell humidity sensor */
#define HIH_4000_TO_RH(millivolts) (((millivolts) - 826) / 31)

/* tmp36gz temperature sensor */
#define TMP36GZ_TO_C_DEGREES(millivolts) (((millivolts) - 500) / 10)
#define TMP36GZ_TO_C_CENTI_DEGREES(millivolts) ((millivolts) - 500)
#define TMP36GZ_TO_F_DEGREES(millivolts)		\
	((((millivolts) - 500) * 9) / 50 + 32)

/* LM35dz temperature sensor */
#define LM35DZ_TO_C_DEGREES(millivolts) ((millivolts) / 10)
#define LM35DZ_TO_C_CENTI_DEGREES(millivolts) (millivolts)
#define LM35DZ_TO_F_DEGREES(millivolts) (((millivolts) * 9) / 50 + 32)

#endif
