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
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "bsp/board.h"
#include "tusb.h"

// constants
#define MIDI_CLOCK  0xF8
#define MIDI_PLAY 0xFA
#define MIDI_STOP 0xFC
#define MIDI_PAUSE 0xFB
#define MIDI_PRG_CHANGE 0xCF	// 0xC0 is program change, 0x0F is midi channel

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
#define SWITCH_PLAY		12		// play
#define SWITCH_PAUSE	14		// pause
#define PREV			1
#define NEXT			2
#define PLAY			4
#define PAUSE			8

#define FALSE			0
#define TRUE 			1

// globals
static uint8_t song = 0;
static uint8_t midi_dev_addr = 0;
bool play = false;

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

	// LED ON if a switch has been pressed
	if (result) {
        if (NO_LED_GPIO != LED_GPIO) gpio_put(LED_GPIO, true);		// if onboard led and if we are within time window, lite LED on
        if (NO_LED2_GPIO != LED2_GPIO) gpio_put(LED2_GPIO, true);	// if another led and if we are within time window, lite LED on
		return result;
	}

    return FALSE;
}


// write lg bytes stored in buffer to midi out
static void send_midi (bool connected, uint8_t * buffer, uint32_t lg)
{
    uint32_t nwritten;

    // set buffer with midi clock
   
    if (connected && tuh_midih_get_num_tx_cables(midi_dev_addr) >= 1)
    {
        nwritten = tuh_midi_stream_write(midi_dev_addr, 0, buffer, lg);
        if (nwritten != lg) {
            TU_LOG1("Warning: Dropped %ld byte\r\n", (lg-nwritten));
        }
    }
}


static void poll_usb_rx(bool connected)
{
    // device must be attached and have at least one endpoint ready to receive a message
    if (!connected || tuh_midih_get_num_rx_cables(midi_dev_addr) < 1)
    {
        return;
    }
    tuh_midi_read_poll(midi_dev_addr);
}


int main() {
    
  bool connected;
  static uint8_t midi_dev_addr = 0;
	int pedal;
	uint8_t data [3];	// midi data to send
	
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
    gpio_pull_up (SWITCH_PREV);       // switch pull-up
    gpio_init(SWITCH_NEXT);
    gpio_set_dir(SWITCH_NEXT, GPIO_IN);
    gpio_pull_up (SWITCH_NEXT);       // switch pull-up
    gpio_init(SWITCH_PREV);
    gpio_set_dir(SWITCH_PLAY, GPIO_IN);
    gpio_pull_up (SWITCH_PLAY);       // switch pull-up
    gpio_init(SWITCH_PLAY);
    gpio_set_dir(SWITCH_PAUSE, GPIO_IN);
    gpio_pull_up (SWITCH_PAUSE);       // switch pull-up


    // main loop
    while (1) {
        
        tuh_task();
        // check connection to USB slave
        connected = midi_dev_addr != 0 && tuh_midi_configured(midi_dev_addr);

		// test pedal and check if one of them is pressed
        pedal = test_switch (PREV | NEXT | PLAY | PAUSE);

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

		if (pedal) {
			// if pedal is pressed, then flush send buffer
			if (connected)
				tuh_midi_stream_flush(midi_dev_addr);
			// wait for pedal to be unpressed
			while (test_switch (PREV | NEXT | PLAY | PAUSE));
			// when unpressed, wait 50ms to avoid bouncing
			sleep_ms (50);
		}
		
		// read MIDI events coming from groovebox and manage accordingly
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
            uint8_t buffer[48];
            while (1) {
                uint32_t bytes_read = tuh_midi_stream_read(dev_addr, &cable_num, buffer, sizeof(buffer));
                if (bytes_read == 0) return;
                if (cable_num == 0) {
					// test values received from groovebox via MIDI
					if (bytes_read == 1){		// test MIDI PLAY or STOP
						if (buffer [0] == MIDI_PLAY) play = true;
						if (buffer [0] == MIDI_STOP) play = false;
					}
					if (bytes_read == 2){		// test MIDI PROGRAM CHANGE
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