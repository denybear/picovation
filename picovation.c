/**
 * @file picovation.c
 * @brief A pico board acting as USB Host and sending session signals to external groovebox (Novation Circuit) at press of a button
 * 
 * MIT License

 * Copyright (c) 2022 denybear, rppicomidi

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 */

// Groovebox shall be setup with (press shift while powering the Novation circuit) :
// notes RX and TX : off
// CC RX and TX : off
// PC RX and TX : on
// Clock RX : on, Clock TX : off
// Lights on the Novation circuit : 00 00 11 10
//
// receiving clock signal is too resource-consuming for raspi pico
// START / STOP don't work if raspi does not provide clock signal


#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "bsp/board.h"
#include "tusb.h"

// constants
#define MIDI_CLOCK		0xF8
#define MIDI_PLAY		0xFA
#define MIDI_STOP		0xFC
#define MIDI_PAUSE		0xFB
#define MIDI_PRG_CHANGE	0xCF	// 0xC0 is program change, 0x0F is midi channel

#define LED_GPIO	25	// onboard led
#define LED2_GPIO	255	// 2nd led
const uint NO_LED_GPIO = 255;
const uint NO_LED2_GPIO = 255;

#define SWITCH_1	11
#define SWITCH_2	12
#define SWITCH_3	13
#define SWITCH_4	14
#define SWITCH_5	15

#define SWITCH_PREV		11		// previous session
#define SWITCH_NEXT		15		// next session
#define SWITCH_PLAY		14		// play
#define SWITCH_PAUSE	12		// pause
#define SWITCH_TEMPO	13		// tap tempo
#define PREV			1
#define NEXT			2
#define PLAY			4
#define PAUSE			8
#define TEMPO			16

#define FALSE			0
#define TRUE 			1

#define EXIT_FUNCTION	2000000	// 2000000 usec = 2 sec
#define NB_TICKS		24		// 24 ticks per beat (quarter note)
#define	BPM40_TICKS		62500	// 40BPM = 1 beat every 1.5 seconds = 1500000 usec / NB_TICKS = 62500 us between ticks
#define	BPM240_TICKS	10417	// 240BPM = 1 beat every .250 seconds = 250000 usec / NB_TICKS = 10417 us between ticks

// type for alarm IRQ handling
struct usr_data {
	bool connected;
	int64_t time;
};


// globals
static uint8_t song = 0;
static uint8_t midi_dev_addr = 0;
static bool play = false;


// test switches and return which switch has been pressed (FALSE if none)
int test_switch (int pedal_to_check)
{
	int result = 0;
	
	
	// test if switch has been pressed
	// in this case, line is down (level 0)
	if ((pedal_to_check & PREV) && gpio_get (SWITCH_PREV)==0) {
		result |= PREV;
	}

	if ((pedal_to_check & NEXT) && gpio_get (SWITCH_NEXT)==0) {
		result |= NEXT;
	}

	if ((pedal_to_check & PLAY) && gpio_get (SWITCH_PLAY)==0) {
		result |= PLAY;
	}

	if ((pedal_to_check & PAUSE) && gpio_get (SWITCH_PAUSE)==0) {
		result |= PAUSE;
	}

	if ((pedal_to_check & TEMPO) && gpio_get (SWITCH_TEMPO)==0) {
		result |= TEMPO;
	}

	// LED ON if a switch has been pressed
	if (result) {
		if (NO_LED_GPIO != LED_GPIO) gpio_put(LED_GPIO, true);		// if onboard led and if we are within time window, lite LED on
		if (NO_LED2_GPIO != LED2_GPIO) gpio_put(LED2_GPIO, true);	// if another led and if we are within time window, lite LED on
		// anti-bounce of 30ms
		sleep_ms (30);
		return result;
	}

	// no switch pressed : LED off
	if (NO_LED_GPIO != LED_GPIO) gpio_put(LED_GPIO, false);		// if onboard led and if we are within time window, lite LED on
	if (NO_LED2_GPIO != LED2_GPIO) gpio_put(LED2_GPIO, false);	// if another led and if we are within time window, lite LED on
	return FALSE;
}


// write lg bytes stored in buffer to midi out
static void send_midi (bool connected, uint8_t * buffer, uint32_t lg)
{
	uint32_t nwritten;

	if (connected && tuh_midih_get_num_tx_cables(midi_dev_addr) >= 1)
	{
		nwritten = tuh_midi_stream_write(midi_dev_addr, 0, buffer, lg);
		if (nwritten != lg) {
			TU_LOG1("Warning: Dropped %ld byte\r\n", (lg-nwritten));
		}
	}
}



// poll USB receive and process received bytes accordingly
static void poll_usb_rx (bool connected)
{
	// device must be attached and have at least one endpoint ready to receive a message
	if (!connected || tuh_midih_get_num_rx_cables(midi_dev_addr) < 1)
	{
		return;
	}
	tuh_midi_read_poll(midi_dev_addr);
}


// sends a midi clock signal when "when_to_send" time has elapsed, and returns true
// returns false if not elapsed
static bool send_clock (bool connected, uint64_t when_to_send)
{
	uint64_t time;
	uint8_t data;

	// check whether it is time to send midi clock signal or not
	time = to_us_since_boot (get_absolute_time());
	if (time < when_to_send) return false;

	// send MIDI CLOCK signal
	data = MIDI_CLOCK;
	send_midi (connected, &data, 1);
printf ("send clock\n");

	// flush outgoing data
	if (connected) tuh_midi_stream_flush(midi_dev_addr);
	return true;
}


// called at regular time (alert); this is used to send MIDI clock signal at regular intervals (tap tempo functionality) 
// shall return <0 to reschedule the same alarm this many us from the time the alarm was previously scheduled to fire
int64_t alarm_callback (alarm_id_t id, void *alarm_data)
{
	int64_t time_interval, tutu;
	bool connected;
	uint8_t data [3];	// midi data to send

	// get values from alarm data structure
	connected = ((struct usr_data*) alarm_data)->connected;
	time_interval = -(((struct usr_data*) alarm_data)->time);		// must be negative
	
	// send MIDI CLOCK signal
	data [0] = MIDI_CLOCK;
	send_midi (connected, data, 1);

	// flush outgoing data
//	if (connected) tuh_midi_stream_flush(midi_dev_addr);

	return time_interval;
}


int main() {
	
	int pedal;
	uint8_t data [3];	// midi data to send
	bool connected;

	uint64_t press_on, press_off;				// time for tap tempo function, to enable / disable tap tempo functionality
	uint64_t this_press, previous_press = 0;	// time for tap tempo function, to measure timing between 1st and 2nd press
	int64_t time_interval_between_ticks;		// time to wait between 2 MIDI clock ticks
	int64_t time_to_send_next_clock;			// time when to sent next midi clock
	alarm_id_t alarm_id;						// id of alarm used to send MIDI clock at regular intervals
	struct usr_data alarm_data;					// data struct to be passed to alarm callback
	bool active_alarm = false;					// indicates whether there is an ongoing alarm

	stdio_init_all();
	board_init();
	printf("Picovation\r\n");
	tusb_init();

	// Map the pins to functions
	gpio_init(LED_GPIO);
	gpio_set_dir(LED_GPIO, GPIO_OUT);

	gpio_init(LED2_GPIO);
	gpio_set_dir(LED2_GPIO, GPIO_OUT);

	gpio_init(SWITCH_PREV);
	gpio_set_dir(SWITCH_PREV, GPIO_IN);
	gpio_pull_up (SWITCH_PREV);		 // switch pull-up

	gpio_init(SWITCH_NEXT);
	gpio_set_dir(SWITCH_NEXT, GPIO_IN);
	gpio_pull_up (SWITCH_NEXT);		 // switch pull-up

	gpio_init(SWITCH_PLAY);
	gpio_set_dir(SWITCH_PLAY, GPIO_IN);
	gpio_pull_up (SWITCH_PLAY);		 // switch pull-up

	gpio_init(SWITCH_PAUSE);
	gpio_set_dir(SWITCH_PAUSE, GPIO_IN);
	gpio_pull_up (SWITCH_PAUSE);		 // switch pull-up

	gpio_init(SWITCH_TEMPO);
	gpio_set_dir(SWITCH_TEMPO, GPIO_IN);
	gpio_pull_up (SWITCH_TEMPO);		 // switch pull-up



	// main loop
	while (1) {

		tuh_task();
		// check connection to USB slave
		connected = midi_dev_addr != 0 && tuh_midi_configured(midi_dev_addr);

		// test pedal and check if one of them is pressed
		pedal = test_switch (PREV | NEXT | PLAY | PAUSE | TEMPO);


		if (pedal & (NEXT | PREV)) {
			// previous or next session
			if (pedal & NEXT)
				song = (song == 31) ? 0 : song + 1;		// test boundaries
			if (pedal & PREV)
				song = (song == 0) ? 31 : song - 1;		// test boundaries
			data [0] = MIDI_PRG_CHANGE;
			data [1] = song; 
			send_midi (connected, data, 2);
		}


		if (pedal & PLAY) {
			// play / stop
			if (play) {		// if play, then stop
				data [0] = MIDI_STOP;
				play = false;
			}
			else {
				data [0] = MIDI_PLAY;
				play = true;
			}
			send_midi (connected, data, 1);
		}


		if (pedal & PAUSE) {
			// pause
			data [0] = MIDI_PAUSE;
			send_midi (connected, data, 1);
		}


		if (pedal & TEMPO) {
			// Tap tempo functionality
			
			// get current time
			this_press = to_us_since_boot (get_absolute_time());
			press_on = this_press;

			// In case this is the first time we press the tempo pedal, then previous_press will be 0
			// otherwise previous_press will have another value

			// calculate time difference between 2 press of tap tempo pedal, and from this calculate corresponding interval between MIDI ticks
			time_interval_between_ticks = (this_press - previous_press) / NB_TICKS;
			// in case time between ticks is too short, do not take press into account: do not change alarms, and consider this is the first press of pedal
			// in case time between ticks is too large, do not take press into account: do not change alarms, and consider this is the first press of pedal

			// in case there have been 2 presses within the correct timing boundaries
			if ((time_interval_between_ticks <= BPM40_TICKS) && (time_interval_between_ticks >= BPM240_TICKS)) {
				// set new time to send midi_clock
				time_to_send_next_clock = this_press + time_interval_between_ticks;
				if (send_clock (connected, time_to_send_next_clock)) time_to_send_next_clock = to_us_since_boot (get_absolute_time()) + time_interval_between_ticks;

/*
				// remove current alarm; will do nothing if no alarm exist already
				if (active_alarm) cancel_alarm (alarm_id);

				// set a new alarm to send MIDI clock for each MIDI tick
				alarm_data.connected = connected;
				alarm_data.time = time_interval_between_ticks;

				alarm_id = add_alarm_in_us (time_interval_between_ticks, alarm_callback, (void *) &alarm_data, true);
				if (alarm_id > 0) active_alarm = true;
				else {
					active_alarm = false;
					printf ("could not set alarm\n");
				}
*/
			}

			// in any case, current time (time of this press) becomes time of previous press, in order to prepare for next press
			previous_press = this_press;

			// wait for pedal to be unpressed; during that time, make sure we receive incoming midi events though
			while (test_switch (PREV | NEXT | PLAY | PAUSE | TEMPO)) {
				// read MIDI events coming from groovebox and manage accordingly
				// commented as this may not be necessary
				// poll_usb_rx (connected);
				if (send_clock (connected, time_to_send_next_clock)) time_to_send_next_clock = to_us_since_boot (get_absolute_time()) + time_interval_between_ticks;
			}
			// here we could set pedal to 0 as we know no pedal is pressed; however it does not hurt to keep it though

			// if the pedal has been pressed for 2+ seconds, then disable tap tempo functionality
			// get current time
			press_off = to_us_since_boot (get_absolute_time());
printf ("pressoff - presson: %lld \n",press_off - press_on );
			// check whether pedal has been pressed for 3+ seconds in which case we disable the function
			if (press_off - press_on >= EXIT_FUNCTION) {
				// set unreachable value for time to send next clock signal: nothing will be sent then
				time_to_send_next_clock = 0xffffffffffffffff;

/*
				// remove alarm
				if (active_alarm) cancel_alarm (alarm_id);
				active_alarm = false;
*/
				// set functionality off (this is not really necessary)
				previous_press = 0;
			}
		}


		// check that pedal is released
		if (pedal) {
			// if pedal is pressed, then flush send buffer
			if (connected) tuh_midi_stream_flush(midi_dev_addr);
			// wait for pedal to be unpressed; during that time, make sure we receive incoming midi events though
			while (test_switch (PREV | NEXT | PLAY | PAUSE | TEMPO)) {
				// read MIDI events coming from groovebox and manage accordingly
				// commented as this may not be necessary
				// poll_usb_rx (connected);
				if (send_clock (connected, time_to_send_next_clock)) time_to_send_next_clock = to_us_since_boot (get_absolute_time()) + time_interval_between_ticks;
			}
		}
		
		// send midi clock if required
		if (send_clock (connected, time_to_send_next_clock)) time_to_send_next_clock = to_us_since_boot (get_absolute_time()) + time_interval_between_ticks;
		// flush outgoing data, and read MIDI events coming from groovebox and manage accordingly
		if (connected) tuh_midi_stream_flush(midi_dev_addr);
		poll_usb_rx (connected);
		
	}
}


//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx, uint16_t num_cables_tx)
{
	printf("MIDI device address = %u, IN endpoint %u has %u cables, OUT endpoint %u has %u cables\r\n",
		dev_addr, in_ep & 0xf, num_cables_rx, out_ep & 0xf, num_cables_tx);

	if (midi_dev_addr == 0) {
		// then no MIDI device is currently connected
		midi_dev_addr = dev_addr;
	}

	else {
		printf("A different USB MIDI Device is already connected.\r\nOnly one device at a time is supported in this program\r\nDevice is disabled\r\n");
	}
}

// Invoked when device with hid interface is un-mounted
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance)
{
	if (dev_addr == midi_dev_addr) {
		midi_dev_addr = 0;
		printf("MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
	}
	else {
		printf("Unused MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
	}
}

// invoked when receiving some MIDI data
void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets)
{
	if (midi_dev_addr == dev_addr)
	{
		if (num_packets != 0)
		{
			uint8_t cable_num;
			uint8_t buffer[48];   // 48 should be enough as we only get Program Change data
			while (1) {
				uint32_t bytes_read = tuh_midi_stream_read(dev_addr, &cable_num, buffer, sizeof(buffer));
				if (bytes_read == 0) return;
				if (cable_num == 0) {
							// test values received from groovebox via MIDI
				  // this is a poor way of testing; in some cases, we receive much more than 1 or 2 bytes
				  // and incoming MIDI message should be parsed
				  // in the case we only receive Program Change data, this testing should be sufficient
							if (bytes_read == 1) {		// test MIDI PLAY or STOP
								if (buffer [0] == MIDI_PLAY) play = true;
								if (buffer [0] == MIDI_STOP) play = false;
							}
						  if (bytes_read == 2) {		// test MIDI PROGRAM CHANGE
							  if (buffer [0] == MIDI_PRG_CHANGE)
								  if (buffer [1] <= 31) song = buffer [1];		// make sure song number is inside boudaries (0 to 31)
						  }
				}
			}
		}
	}

	return;
}

// invoked when sending some MIDI data
void tuh_midi_tx_cb(uint8_t dev_addr)
{
	(void)dev_addr;
}