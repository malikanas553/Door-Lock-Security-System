/*
 * CONTROL_ECU.c
 *
 *  Created on: Apr 8, 2025
 *      Author: Malik Anas
 */

#include "uart.h"
#include "interrupt.h"
#include "buzzer.h"
#include "external_eeprom.h"
#include "dcmotor.h"
#include "timer.h"
#include "pir_sensor.h"
#include <util/delay.h>

/* ---------------------- MACROS AND CONSTANTS ---------------------- */

/* Convert milliseconds to timer ticks based on CPU frequency and prescaler */
#define MILLISECONDS_TO_TICKS(ms)  (((F_CPU / 1024) * (ms)) / 1000)

/* Password configuration */
#define PASSWORD_DIGITS 	5

/* UART commands and responses */
#define MC2_READY      		0xE0
#define GET_STATUS     		0xE1
#define SET_NEW_PASS   		0xE2
#define NEXT_DIGIT     		0xE3
#define CONFIRM_PASS   		0xE4
#define PASS_MATCH     		0xE5
#define PASS_NO_MATCH  		0xE6
#define CHECK_PASS          0xE7
#define RECIEVED            0xE9
#define ATTEMPTS_ENDED      0xF0
#define UNLOCK_DOOR         0xF1
#define LOCK_DOOR           0xF2
#define PASSWORD_SAVED      0x23

/* EEPROM base address to store the password */
#define PASSWORD_BASE_ADDRESS 0x0311

/* ---------------------- GLOBAL VARIABLES ---------------------- */

/* Pointer to EEPROM base address where password is stored */
static volatile uint8 * ptr_to_pass = (uint8 *)PASSWORD_BASE_ADDRESS;

/* Stores the last received command from HMI ECU */
volatile uint8 received_key;

/* Timer counter for delays */
static volatile uint8 time = 0;

/* UART configuration structure */
UART_ConfigType UART_CONFIG = {EIGHT_BITS, NO_PARITY, ONE_STOP_BIT, 2400};

/* Timer configuration structure for 1-second delay using CTC mode */
Timer_ConfigType TIMER1_CONFIG = {0, (uint16)(MILLISECONDS_TO_TICKS(1000)), TIMER_1, F_CPU_1024, CTC_MODE_OC_DISABLED};

/* ---------------------- FUNCTION DEFINITIONS ---------------------- */

/*
 * Callback function for Timer1, increments the time counter.
 */
void CONTROL_countSeconds(void)
{
	time++;
}

/*
 * Delay function using Timer1 for a given number of seconds.
 */
void CONTROL_delaySeconds(uint8 seconds)
{
	time = 0;
	Timer_setCallBack(CONTROL_countSeconds, TIMER_1);
	Timer_init(&TIMER1_CONFIG);
	while (time < seconds);
	time = 0;
	Timer_deInit(TIMER_1);
}

/*
 * Read the current password from EEPROM and store it into a buffer.
 */
void CONTROL_updatePassword(uint8 * password)
{
	volatile uint8 digits = 0;
	while (digits < PASSWORD_DIGITS)
	{
		EEPROM_readByte((uint16)(ptr_to_pass + digits), password + digits);
		_delay_ms(10);
		digits++;
	}

	/* Add terminator character at the end */
	password[digits] = '#';
}

/*
 * Receive password from UART (sent by HMI ECU).
 */
void CONTROL_receivePassword(uint8 * password)
{
	volatile uint8 digits = 0;
	while (digits < PASSWORD_DIGITS)
	{
		password[digits] = UART_recieveByte();
		UART_sendByte(NEXT_DIGIT);
		digits++;
	}

	/* Read the final terminator byte */
	password[digits] = UART_recieveByte();
}

/*
 * Save the password into EEPROM memory.
 */
void CONTROL_savePassword(uint8 * password)
{
	uint8 digits = 0;
	while (digits < PASSWORD_DIGITS)
	{
		EEPROM_writeByte((uint16)(ptr_to_pass + digits), password[digits]);
		_delay_ms(10);
		digits++;
	}

	/* Store terminator */
	EEPROM_writeByte((uint16)(ptr_to_pass + PASSWORD_DIGITS), password[digits]);
}

/*
 * Compare two passwords, return TRUE if they match, FALSE otherwise.
 */
uint8 CONTROL_comparePasswords(uint8 * password, uint8 * other_password)
{
	int i = 0;
	while (i < PASSWORD_DIGITS)
	{
		if (password[i] != other_password[i])
			return FALSE;
		i++;
	}
	return TRUE;
}

/* ---------------------- MAIN FUNCTION ---------------------- */

int main(void)
{
	/* System status, password buffers */
	uint8 status = 0;
	uint8 new_password[PASSWORD_DIGITS + 1];
	uint8 current_password[PASSWORD_DIGITS + 1];
	uint8 confirm_password[PASSWORD_DIGITS + 1];

	/* Initialize system peripherals */
	Enable_Global_Interrupt();
	UART_init(&UART_CONFIG);
	UART_sendByte(MC2_READY);    /* Notify HMI ECU we're ready */
	Buzzer_init();
	DcMotor_Init();
	PIR_init();

	while (1)
	{
		/* Wait for command from HMI ECU */
		received_key = UART_recieveByte();

		if (received_key != 0xFF)
		{
			if (received_key == GET_STATUS)
			{
				/* Respond with system status */
				EEPROM_readByte((uint16)(ptr_to_pass + 5), &status);
				_delay_ms(10);
				UART_sendByte(status);

				/* If password is already saved, update the buffer */
				if (status == PASSWORD_SAVED)
				{
					CONTROL_updatePassword(current_password);
				}
			}
			else if (received_key == SET_NEW_PASS)
			{
				/* Receive new password and confirmation */
				CONTROL_receivePassword(new_password);
				while (UART_recieveByte() != CONFIRM_PASS);
				CONTROL_receivePassword(confirm_password);

				/* Compare passwords and update if matched */
				if (CONTROL_comparePasswords(new_password, confirm_password) == TRUE)
				{
					UART_sendByte(PASS_MATCH);
					CONTROL_savePassword(new_password);
					_delay_ms(30);
					CONTROL_updatePassword(current_password);
				}
				else
				{
					UART_sendByte(PASS_NO_MATCH);
				}
			}
			else if (received_key == CHECK_PASS)
			{
				/* Validate entered password against saved one */
				CONTROL_receivePassword(confirm_password);

				if (CONTROL_comparePasswords(confirm_password, current_password) == TRUE)
				{
					UART_sendByte(PASS_MATCH);
				}
				else
				{
					UART_sendByte(PASS_NO_MATCH);
				}
			}
			else if (received_key == ATTEMPTS_ENDED)
			{
				/* Trigger buzzer and wait 60 seconds after 3 failed attempts */
				Buzzer_on();
				CONTROL_delaySeconds(60);
				Buzzer_off();
			}
			else if (received_key == UNLOCK_DOOR)
			{
				/* Rotate motor clockwise to unlock door */
				DcMotor_Rotate(CLOCKWISE, 100);
				CONTROL_delaySeconds(15);

				/* Stop motor after door is unlocked */
				DcMotor_Rotate(STOP, 0);

				/* Wait until no motion detected */
				while (PIR_getState() == MOTION);

				/* Notify HMI to lock door */
				UART_sendByte(LOCK_DOOR);

				/* Rotate motor anti-clockwise to lock door */
				DcMotor_Rotate(ANTICLOCKWISE, 100);
				CONTROL_delaySeconds(15);

				/* Stop motor */
				DcMotor_Rotate(STOP, 0);
			}

			/* Reset received key */
			received_key = 0xFF;
		}
	}
}
