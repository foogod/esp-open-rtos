#include "onewire.h"
#include "string.h"
#include "task.h"

// Platform specific I/O definitions
#define noInterrupts taskENTER_CRITICAL
#define interrupts taskEXIT_CRITICAL

#define DIRECT_MODE_INPUT(pin)   gpio_enable(pin, GPIO_INPUT)
#define DIRECT_MODE_OUTPUT(pin)  gpio_enable(pin, GPIO_OUTPUT)
#define DIRECT_WRITE_LOW(pin)    gpio_write(pin, 0)
#define DIRECT_WRITE_HIGH(pin)   gpio_write(pin, 1)

// Perform the onewire reset function.  We will wait up to 250uS for
// the bus to come high, if it doesn't then it is broken or shorted
// and we return a 0;
//
// Returns 1 if a device asserted a presence pulse, 0 otherwise.
//
uint8_t onewire_reset(uint8_t pin)
{
	uint8_t r;
	uint8_t retries = 125;

	gpio_enable(pin, GPIO_OUT_OPEN_DRAIN);
	gpio_write(pin, 1);
	// wait until the wire is high... just in case
	do {
		if (--retries == 0) return 0;
		sdk_os_delay_us(2);
	} while (!gpio_read(pin));

	gpio_write(pin, 0);
	sdk_os_delay_us(480);

	noInterrupts();
	gpio_write(pin, 1);	// allow it to float
	sdk_os_delay_us(70);
	r = !gpio_read(pin);
	interrupts();

	sdk_os_delay_us(410);
	return r;
}

static void onewire_write_bit(uint8_t pin, uint8_t v)
{
	if (v & 1) {
		noInterrupts();
		gpio_write(pin, 0);  // drive output low
		sdk_os_delay_us(10);
		gpio_write(pin, 1);  // allow output high
		interrupts();
		sdk_os_delay_us(55);
	} else {
		noInterrupts();
		gpio_write(pin, 0);  // drive output low
		sdk_os_delay_us(65);
		gpio_write(pin, 1);	// allow output high
		interrupts();
		sdk_os_delay_us(5);
	}
}

static uint8_t onewire_read_bit(uint8_t pin)
{
	uint8_t r;

	noInterrupts();
	gpio_write(pin, 0);
	sdk_os_delay_us(3);
	gpio_write(pin, 1);  // let pin float, pull up will raise
	sdk_os_delay_us(10);
	r = gpio_read(pin);
	interrupts();
	sdk_os_delay_us(53);
	return r;
}

// Write a byte. The writing code uses open-drain mode and expects the pullup
// resistor to pull the line high when not driven low.  If you need strong
// power after the write (e.g. DS18B20 in parasite power mode) then call
// onewire_power() after this is complete to actively drive the line high.
//
void onewire_write(uint8_t pin, uint8_t v) {
  uint8_t bitMask;

  for (bitMask = 0x01; bitMask; bitMask <<= 1) {
	  onewire_write_bit(pin, (bitMask & v)?1:0);
  }
}

void onewire_write_bytes(uint8_t pin, const uint8_t *buf, uint16_t count) {
  uint16_t i;
  for (i = 0 ; i < count ; i++) {
    onewire_write(pin, buf[i]);
  }
}

// Read a byte
//
uint8_t onewire_read(uint8_t pin) {
  uint8_t bitMask;
  uint8_t r = 0;

  for (bitMask = 0x01; bitMask; bitMask <<= 1) {
  	if (onewire_read_bit(pin)) r |= bitMask;
  }
  return r;
}

void onewire_read_bytes(uint8_t pin, uint8_t *buf, uint16_t count) {
  uint16_t i;
  for (i = 0 ; i < count ; i++)
    buf[i] = onewire_read(pin);
}

// Do a ROM select
//
void onewire_select(uint8_t pin, onewire_addr_t rom)
{
    uint8_t i;

    onewire_write(pin, 0x55);           // Choose ROM

    for (i = 0; i < 8; i++) {
        onewire_write(pin, rom & 0xff);
        rom >>= 8;
    }
}

// Do a ROM skip
//
void onewire_skip_rom(uint8_t pin)
{
    onewire_write(pin, 0xCC);           // Skip ROM
}

void onewire_power(uint8_t pin)
{
    gpio_enable(pin, GPIO_OUTPUT);
    gpio_write(pin, 1);
}

void onewire_depower(uint8_t pin)
{
    gpio_enable(pin, GPIO_OUT_OPEN_DRAIN);
}

void onewire_search_start(onewire_search_t *search)
{
  // reset the search state
  memset(search, 0, sizeof(*search));
}

// Setup the search to find the device type 'family_code' on the next call
// to search(*newAddr) if it is present.
//
void onewire_search_prefix(onewire_search_t *search, uint8_t family_code)
{
   uint8_t i;

   search->rom_no[0] = family_code;
   for (i = 1; i < 8; i++) {
      search->rom_no[i] = 0;
   }
   search->last_discrepancy = 64;
   search->last_device_found = false;
}

// Perform a search. If the next device has been successfully enumerated, its
// ROM address will be returned.  If there are no devices, no further
// devices, or something horrible happens in the middle of the
// enumeration then ONEWIRE_NONE is returned.  Use OneWire::reset_search() to
// start over.
//
// --- Replaced by the one from the Dallas Semiconductor web site ---
//--------------------------------------------------------------------------
// Perform the 1-Wire Search Algorithm on the 1-Wire bus using the existing
// search state.
// Return 1 : device found, ROM number in ROM_NO buffer
//        0 : device not found, end of search
//
onewire_addr_t onewire_search_next(onewire_search_t *search, uint8_t pin)
{
   uint8_t id_bit_number;
   uint8_t last_zero, search_result;
   int rom_byte_number;
   uint8_t id_bit, cmp_id_bit;
   onewire_addr_t addr;

   unsigned char rom_byte_mask, search_direction;

   // initialize for search
   id_bit_number = 1;
   last_zero = 0;
   rom_byte_number = 0;
   rom_byte_mask = 1;
   search_result = 0;
   
   // if the last call was not the last one
   if (!search->last_device_found)
   {
      // 1-Wire reset
      if (!onewire_reset(pin))
      {
         // reset the search
         search->last_discrepancy = 0;
         search->last_device_found = false;
         return ONEWIRE_NONE;
      }

      // issue the search command
      onewire_write(pin, 0xF0);

      // loop to do the search
      do
      {
         // read a bit and its complement
         id_bit = onewire_read_bit(pin);
         cmp_id_bit = onewire_read_bit(pin);

         // check for no devices on 1-wire
         if ((id_bit == 1) && (cmp_id_bit == 1))
            break;
         else
         {
            // all devices coupled have 0 or 1
            if (id_bit != cmp_id_bit)
               search_direction = id_bit;  // bit write value for search
            else
            {
               // if this discrepancy if before the Last Discrepancy
               // on a previous next then pick the same as last time
               if (id_bit_number < search->last_discrepancy)
                  search_direction = ((search->rom_no[rom_byte_number] & rom_byte_mask) > 0);
               else
                  // if equal to last pick 1, if not then pick 0
                  search_direction = (id_bit_number == search->last_discrepancy);

               // if 0 was picked then record its position in LastZero
               if (search_direction == 0)
               {
                  last_zero = id_bit_number;
               }
            }

            // set or clear the bit in the ROM byte rom_byte_number
            // with mask rom_byte_mask
            if (search_direction == 1)
              search->rom_no[rom_byte_number] |= rom_byte_mask;
            else
              search->rom_no[rom_byte_number] &= ~rom_byte_mask;

            // serial number search direction write bit
            onewire_write_bit(pin, search_direction);

            // increment the byte counter id_bit_number
            // and shift the mask rom_byte_mask
            id_bit_number++;
            rom_byte_mask <<= 1;

            // if the mask is 0 then go to new SerialNum byte rom_byte_number and reset mask
            if (rom_byte_mask == 0)
            {
                rom_byte_number++;
                rom_byte_mask = 1;
            }
         }
      }
      while(rom_byte_number < 8);  // loop until through all ROM bytes 0-7

      // if the search was successful then
      if (!(id_bit_number < 65))
      {
         // search successful so set last_discrepancy,last_device_found,search_result
         search->last_discrepancy = last_zero;

         // check for last device
         if (search->last_discrepancy == 0)
            search->last_device_found = true;

         search_result = 1;
      }
   }

   // if no device found then reset counters so next 'search' will be like a first
   if (!search_result || !search->rom_no[0])
   {
      search->last_discrepancy = 0;
      search->last_device_found = false;
      return ONEWIRE_NONE;
   }
   else
   {
      addr = 0;
      for (rom_byte_number = 7; rom_byte_number >= 0; rom_byte_number--)
      {
         addr = (addr << 8) | search->rom_no[rom_byte_number];
      }
      printf("Ok I found something at %08x%08x...\n", (uint32_t)(addr >> 32), (uint32_t)addr);
   }
   return addr;
}

// The 1-Wire CRC scheme is described in Maxim Application Note 27:
// "Understanding and Using Cyclic Redundancy Checks with Maxim iButton Products"
//

#if ONEWIRE_CRC8_TABLE
// This table comes from Dallas sample code where it is freely reusable,
// though Copyright (C) 2000 Dallas Semiconductor Corporation
static const uint8_t dscrc_table[] = {
      0, 94,188,226, 97, 63,221,131,194,156,126, 32,163,253, 31, 65,
    157,195, 33,127,252,162, 64, 30, 95,  1,227,189, 62, 96,130,220,
     35,125,159,193, 66, 28,254,160,225,191, 93,  3,128,222, 60, 98,
    190,224,  2, 92,223,129, 99, 61,124, 34,192,158, 29, 67,161,255,
     70, 24,250,164, 39,121,155,197,132,218, 56,102,229,187, 89,  7,
    219,133,103, 57,186,228,  6, 88, 25, 71,165,251,120, 38,196,154,
    101, 59,217,135,  4, 90,184,230,167,249, 27, 69,198,152,122, 36,
    248,166, 68, 26,153,199, 37,123, 58,100,134,216, 91,  5,231,185,
    140,210, 48,110,237,179, 81, 15, 78, 16,242,172, 47,113,147,205,
     17, 79,173,243,112, 46,204,146,211,141,111, 49,178,236, 14, 80,
    175,241, 19, 77,206,144,114, 44,109, 51,209,143, 12, 82,176,238,
     50,108,142,208, 83, 13,239,177,240,174, 76, 18,145,207, 45,115,
    202,148,118, 40,171,245, 23, 73,  8, 86,180,234,105, 55,213,139,
     87,  9,235,181, 54,104,138,212,149,203, 41,119,244,170, 72, 22,
    233,183, 85, 11,136,214, 52,106, 43,117,151,201, 74, 20,246,168,
    116, 42,200,150, 21, 75,169,247,182,232, 10, 84,215,137,107, 53};

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#endif

//
// Compute a Dallas Semiconductor 8 bit CRC. These show up in the ROM
// and the registers.  (note: this might better be done without to
// table, it would probably be smaller and certainly fast enough
// compared to all those delayMicrosecond() calls.  But I got
// confused, so I use this table from the examples.)
//
uint8_t onewire_crc8(const uint8_t *addr, uint8_t len)
{
	uint8_t crc = 0;

	while (len--) {
		crc = pgm_read_byte(dscrc_table + (crc ^ *addr++));
	}
	return crc;
}
#else
//
// Compute a Dallas Semiconductor 8 bit CRC directly.
// this is much slower, but much smaller, than the lookup table.
//
uint8_t onewire_crc8(const uint8_t *addr, uint8_t len)
{
	uint8_t crc = 0;
	
	while (len--) {
		uint8_t inbyte = *addr++;
    uint8_t i;
		for (i = 8; i; i--) {
			uint8_t mix = (crc ^ inbyte) & 0x01;
			crc >>= 1;
			if (mix) crc ^= 0x8C;
			inbyte >>= 1;
		}
	}
	return crc;
}
#endif

// Compute the 1-Wire CRC16 and compare it against the received CRC.
// Example usage (reading a DS2408):
    //    // Put everything in a buffer so we can compute the CRC easily.
//    uint8_t buf[13];
//    buf[0] = 0xF0;    // Read PIO Registers
//    buf[1] = 0x88;    // LSB address
//    buf[2] = 0x00;    // MSB address
//    WriteBytes(net, buf, 3);    // Write 3 cmd bytes
//    ReadBytes(net, buf+3, 10);  // Read 6 data bytes, 2 0xFF, 2 CRC16
//    if (!CheckCRC16(buf, 11, &buf[11])) {
//        // Handle error.
//    }     
//          
// @param input - Array of bytes to checksum.
// @param len - How many bytes to use.
// @param inverted_crc - The two CRC16 bytes in the received data.
//                       This should just point into the received data,
//                       *not* at a 16-bit integer.
// @param crc - The crc starting value (optional)
// @return 1, iff the CRC matches.
bool onewire_check_crc16(const uint8_t* input, uint16_t len, const uint8_t* inverted_crc, uint16_t crc)
{
    crc = ~onewire_crc16(input, len, crc);
    return (crc & 0xFF) == inverted_crc[0] && (crc >> 8) == inverted_crc[1];
}

// Compute a Dallas Semiconductor 16 bit CRC.  This is required to check
// the integrity of data received from many 1-Wire devices.  Note that the
// CRC computed here is *not* what you'll get from the 1-Wire network,
// for two reasons:
//   1) The CRC is transmitted bitwise inverted.
//   2) Depending on the endian-ness of your processor, the binary
//      representation of the two-byte return value may have a different
//      byte order than the two bytes you get from 1-Wire.
// @param input - Array of bytes to checksum.
// @param len - How many bytes to use.
// @param crc - The crc starting value (optional)
// @return The CRC16, as defined by Dallas Semiconductor.
uint16_t onewire_crc16(const uint8_t* input, uint16_t len, uint16_t crc)
{
    static const uint8_t oddparity[16] =
        { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 };

    uint16_t i;
    for (i = 0 ; i < len ; i++) {
      // Even though we're just copying a byte from the input,
      // we'll be doing 16-bit computation with it.
      uint16_t cdata = input[i];
      cdata = (cdata ^ crc) & 0xff;
      crc >>= 8;

      if (oddparity[cdata & 0x0F] ^ oddparity[cdata >> 4])
          crc ^= 0xC001;

      cdata <<= 6;
      crc ^= cdata;
      cdata <<= 1;
      crc ^= cdata;
    }
    return crc;
}
