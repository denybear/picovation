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

// MISSING : CHECK WHETHER DATA IS SENT CORRECTLY AND RECEIVED CORRECTLY
// ISSUES WITH while LOOPS : WAIT_MS SHOULD BE AVOIDED

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "bsp/board.h"
#include "tusb.h"

// constants
#define MIDI_CLOCK		0xF8
#define MIDI_PLAY		0xFA
#define MIDI_STOP		0xFC
#define MIDI_CONTINUE	0xFB
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
#define SWITCH_CONTINUE	12		// pause
#define SWITCH_TEMPO	13		// tap tempo
#define PREV			1
#define NEXT			2
#define PLAY			4
#define CONTINUE		8
#define TEMPO			16

#define FALSE			0
#define TRUE 			1

#define EXIT_FUNCTION	2000000	// 2000000 usec = 2 sec
#define NB_TICKS		24		// 24 ticks per beat (quarter note)
#define	BPM40_TICKS		62500	// 40BPM = 1 beat every 1.5 seconds = 1500000 usec / NB_TICKS = 62500 us between ticks
#define	BPM240_TICKS	10417	// 240BPM = 1 beat every .250 seconds = 250000 usec / NB_TICKS = 10417 us between ticks


// globals
static uint8_t song = 0;
static uint8_t midi_dev_addr = 0;
static bool connected = false;
static bool play = false;
static bool pause = false;

// tempo fct
static int64_t time_interval_between_ticks = 21000;				// time to wait between 2 MIDI clock ticks; initialized to 0.5 sec/24 (120BPM)
static int64_t new_time_interval_between_ticks = 21000;			// time to wait between 2 MIDI clock ticks; initialized to 0.5 sec/24 (120BPM)
static uint64_t time_to_send_next_clock = 0xffffffffffffffff;	// time when to sent next midi clock; initialized to end of times
static uint64_t time_of_last_clock = 0;							// time when the last midi clock was sent

// midi buffers
#define MIDI_BUF_SIZE	5000
static uint8_t midi_rx [MIDI_BUF_SIZE];		// large midi buffer to avoid override when receiving midi
static uint8_t midi_tx [MIDI_BUF_SIZE];		// large midi buffer to avoid override when receiving midi
static int index_tx = 0;

// poll USB receive and process received bytes accordingly
void poll_usb_rx ()
{
	// device must be attached and have at least one endpoint ready to receive a message
	if (!connected || tuh_midih_get_num_rx_cables(midi_dev_addr) < 1)
	{
		return;
	}
	tuh_midi_read_poll(midi_dev_addr);
}


// write lg bytes stored in buffer to midi out
void send_midi (uint8_t * buffer, uint32_t lg)
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


// sends a midi clock signal when "when_to_send" time has elapsed, and returns true
// returns false if not elapsed
bool send_clock (uint64_t when_to_send)
{
	uint64_t time;

	// check whether it is time to send midi clock signal or not
	time = to_us_since_boot (get_absolute_time());
	if (time < when_to_send) return false;

	// send MIDI CLOCK signal
	midi_tx [index_tx++] = MIDI_CLOCK;
	// set time of last midi clock was sent
	time_of_last_clock = time;
	return true;
}


// test switches and return which switch has been pressed (FALSE if none)
int test_switch (int pedal_to_check)
{
	int result = 0;
	int i;
	
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

	if ((pedal_to_check & CONTINUE) && gpio_get (SWITCH_CONTINUE)==0) {
		result |= CONTINUE;
	}

	if ((pedal_to_check & TEMPO) && gpio_get (SWITCH_TEMPO)==0) {
		result |= TEMPO;
	}

	// LED ON if a switch has been pressed
	if (result) {
		if (NO_LED_GPIO != LED_GPIO) gpio_put(LED_GPIO, true);		// if onboard led and if we are within time window, lite LED on
		if (NO_LED2_GPIO != LED2_GPIO) gpio_put(LED2_GPIO, true);	// if another led and if we are within time window, lite LED on
		// anti-bounce of 30ms, but send clock during this time if required
		for (i = 0; i < 30; i++) {
			// do not use sleep as it blocks all the background processes
			busy_wait_ms (1);
			if (send_clock (time_to_send_next_clock)) time_to_send_next_clock = time_of_last_clock + time_interval_between_ticks;
		}
		return result;
	}

	// no switch pressed : LED off
	if (NO_LED_GPIO != LED_GPIO) gpio_put(LED_GPIO, false);		// if onboard led and if we are within time window, lite LED on
	if (NO_LED2_GPIO != LED2_GPIO) gpio_put(LED2_GPIO, false);	// if another led and if we are within time window, lite LED on
	return FALSE;
}


int main() {
	
	int pedal;
	uint64_t press_on, press_off;				// time for tap tempo function, to enable / disable tap tempo functionality
	uint64_t this_press, previous_press = 0;	// time for tap tempo function, to measure timing between 1st and 2nd press


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

	gpio_init(SWITCH_CONTINUE);
	gpio_set_dir(SWITCH_CONTINUE, GPIO_IN);
	gpio_pull_up (SWITCH_CONTINUE);		 // switch pull-up

	gpio_init(SWITCH_TEMPO);
	gpio_set_dir(SWITCH_TEMPO, GPIO_IN);
	gpio_pull_up (SWITCH_TEMPO);		 // switch pull-up


	// main loop
	while (1) {

		tuh_task();
		// check connection to USB slave
		connected = midi_dev_addr != 0 && tuh_midi_configured(midi_dev_addr);

		// test pedal and check if one of them is pressed
		pedal = test_switch (PREV | NEXT | PLAY | CONTINUE | TEMPO);


		if (pedal & (NEXT | PREV)) {
			// previous or next session
			if (pedal & NEXT)
				song = (song == 31) ? 0 : song + 1;		// test boundaries
			if (pedal & PREV)
				song = (song == 0) ? 31 : song - 1;		// test boundaries
			midi_tx [index_tx++] = MIDI_PRG_CHANGE;
			midi_tx [index_tx++] = song; 

			// send stop then pause/continue so music don't stop
			midi_tx [index_tx++] = MIDI_STOP;
			if (play || pause) midi_tx [index_tx++] = MIDI_PLAY;
		}


		if (pedal & PLAY) {
			// play / stop
			if (play || pause) {		// if play or pause, then stop
				midi_tx [index_tx++] = MIDI_STOP;
				play = false;
				pause = false;
			}
			else {
				midi_tx [index_tx++] = MIDI_PLAY;
				play = true;
			}
		}


		if (pedal & CONTINUE) {
			// pause / stop
			if (play || pause) {		// if pause or play, then stop
				midi_tx [index_tx++] = MIDI_STOP;
				play = false;
				pause = false;
			}
			else {
				midi_tx [index_tx++] = MIDI_CONTINUE;
				pause = true;
			}
		}

		if (pedal & TEMPO) {
			// Tap tempo functionality
			
			// get current time
			this_press = to_us_since_boot (get_absolute_time());
			press_on = this_press;

			// In case this is the first time we press the tempo pedal, then previous_press will be 0
			// otherwise previous_press will have another value

			// calculate time difference between 2 press of tap tempo pedal, and from this calculate corresponding interval between MIDI ticks
			new_time_interval_between_ticks = (this_press - previous_press) / NB_TICKS;
			// in case time between ticks is too short, do not take press into account: do not change alarms, and consider this is the first press of pedal
			// in case time between ticks is too large, do not take press into account: do not change alarms, and consider this is the first press of pedal

			// in case there have been 2 presses within the correct timing boundaries
			if ((new_time_interval_between_ticks <= BPM40_TICKS) && (new_time_interval_between_ticks >= BPM240_TICKS)) {

				// validate new time interval as time between ticks
				// goal of having new time interval is that it allows to keep previous time interval in case of 1st press
				time_interval_between_ticks = new_time_interval_between_ticks;
				// send stop then pause/continue so music don't stop
				midi_tx [index_tx++] = MIDI_STOP;
				if (play || pause) midi_tx [index_tx++] = MIDI_CONTINUE;
				// set new time to send midi_clock
				time_to_send_next_clock = this_press + time_interval_between_ticks;
				if (send_clock (time_to_send_next_clock)) time_to_send_next_clock = time_of_last_clock + time_interval_between_ticks;
			}

			// in any case, current time (time of this press) becomes time of previous press, in order to prepare for next press
			previous_press = this_press;

			// wait for pedal to be unpressed; during that time, make sure we receive incoming midi events though
			while (test_switch (PREV | NEXT | PLAY | CONTINUE | TEMPO)) {
// useless, this is also done in test_switch
//				if (send_clock (time_to_send_next_clock)) time_to_send_next_clock = time_of_last_clock + time_interval_between_ticks;
			}
			// here we could set pedal to 0 as we know no pedal is pressed; however it does not hurt to keep it though

			// if the pedal has been pressed for 2+ seconds, then disable tap tempo functionality
			// get current time
			press_off = to_us_since_boot (get_absolute_time());

			// check whether pedal has been pressed for 2+ seconds in which case we disable the function
			if (press_off - press_on >= EXIT_FUNCTION) {
				// set unreachable value for time to send next clock signal: nothing will be sent then
				time_to_send_next_clock = 0xffffffffffffffff;

				// set functionality off (this is not really necessary)
				previous_press = 0;
			}
		}


		// if some data is present, send midi data and flush buffer
		if (index_tx) {
			send_midi (midi_tx, index_tx);
			if (connected) tuh_midi_stream_flush(midi_dev_addr);
			index_tx = 0;
		}

		// check that pedal is released
		if (pedal) {
			// wait for pedal to be unpressed; during that time, make sure we receive incoming midi events though
			while (test_switch (PREV | NEXT | PLAY | CONTINUE | TEMPO)) {
// useless, this is also done in test_switch
//				if (send_clock (time_to_send_next_clock)) time_to_send_next_clock = time_of_last_clock + time_interval_between_ticks;
			}
		}

		// send midi clock if required (in particular if no pedal is pressed)
		if (send_clock (time_to_send_next_clock)) time_to_send_next_clock = time_of_last_clock + time_interval_between_ticks;
		// if some data is present, send midi data and flush buffer
		if (index_tx) {
			send_midi (midi_tx, index_tx);
			index_tx = 0;
		}

		// read MIDI events coming from groovebox and manage accordingly
		if (connected) tuh_midi_stream_flush(midi_dev_addr);
		poll_usb_rx ();
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
	uint8_t cable_num;
	uint8_t *buffer;
	uint32_t i;
	uint32_t bytes_read;

	// set midi_rx as buffer
	buffer = midi_rx;
	
	if (midi_dev_addr == dev_addr)
	{
		if (num_packets != 0)
		{
			while (1) {
				bytes_read = tuh_midi_stream_read(dev_addr, &cable_num, buffer, MIDI_BUF_SIZE);
				if (bytes_read == 0) return;
				if (cable_num == 0) {
					i = 0;
					while (i < bytes_read) {
						// test values received from groovebox via MIDI
						switch (buffer [i]) {
// This part is not needed as we don't receive MIDI CLOCK signals (R-PICO cannot cope with the speed)
//							case MIDI_CLOCK:
//								midi_tx [index_tx++] = MIDI_CLOCK;
//								break;
							case MIDI_CONTINUE:
								pause = true;
								break;
							case MIDI_PLAY:
								play = true;
								break;
							case MIDI_STOP:
								play = false;
								pause = false;
								break;
							case MIDI_PRG_CHANGE:
								if (buffer [i+1] <= 31) song = buffer [i+1];		// make sure song number is inside boudaries (0 to 31)
								break;
						}
						switch (buffer [i] & 0xF0) {	// control only MS nibble to increment index in buffer; event sorting is approximative, but should be enough
							case 0x80:
							case 0x90:
							case 0xA0:
							case 0xB0:
							case 0xE0:
								i+=3;
								break;
							case 0xC0:
							case 0xD0:
								i+=2;
								break;
							case 0xF0:
								i+=1;
								break;
							default:
								i+=1;
								break;
						}
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