#include <stdio.h>
#include "system.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "altera_avalon_spi.h"
#include "altera_avalon_pio_regs.h"

#include "FreeRTOS.h"
#include "task.h"
#include "i2c.h"
#include "mdio.h"
#include "alt_types.h"
#include "dump_hex.h"
#include "iomap.h"
#include "itu.h"
#include "profiler.h"
#include "usb_nano.h"
#include "u2p.h"
#include "usb_base.h"
#include "filemanager.h"
#include "stream_textlog.h"

extern "C" {
	#include "jtag.h"
	#include "audio_test.h"
	#include "analog.h"
	#include "digital_io.h"
	#include "flash_switch.h"
	#include "ethernet_test.h"
	void codec_init(void);
	int getNetworkPacket(uint8_t **payload, int *length);
}

#include "usb_base.h"

extern unsigned char _dut_b_start;
extern unsigned char _dut_b_size;
extern unsigned char _dut_application_start;

typedef struct {
	const char *fileName;
	const char *romName;
	const uint32_t flashAddress;
	uint32_t *buffer;
	uint32_t  size;
} BinaryImage_t;

BinaryImage_t toBeFlashed[] = {
		{ "/Usb?/flash/ultimate_recovery.swp", "Recovery FPGA Image",  0x00000000, 0, 0 },
		{ "/Usb?/flash/recovery.app",          "Recovery Application", 0x00080000, 0, 0 },
		{ "/Usb?/flash/ultimate_run.swp",      "Runtime FPGA Image",   0x80000000, 0, 0 },
		{ "/Usb?/flash/ultimate.app",          "Runtime Application",  0x800C0000, 0, 0 },
		{ "/Usb?/flash/rompack.bin",           "ROM Pack",             0x80200000, 0, 0 }
};

BinaryImage_t dutFpga = { "/Usb?/tester/dut.b",   "DUT FPGA Image", 0, 0, 0 };
BinaryImage_t dutAppl = { "/Usb?/tester/dut.app", "DUT Application Image", 0, 0, 0 };

#define APPLICATION_RUN  (0x20000780)
#define BUFFER_LOCATION  (0x20000784)
#define BUFFER_SIZE      (0x20000788)
#define BUFFER_HEAD		 (0x2000078C)
#define BUFFER_TAIL		 (0x20000790)
#define DUT_TO_TESTER    (0x20000794)
#define TESTER_TO_DUT	 (0x20000798)
#define TEST_STATUS		 (0x2000079C)
#define PROGRAM_DATALOC  (0x200007A0)
#define PROGRAM_DATALEN  (0x200007A4)
#define PROGRAM_ADDR     (0x200007A8)
#define TIMELOC          (0x200007B0)


void run_application_on_dut(JTAG_Access_t *target, uint32_t *app)
{
	uint8_t reset = 0x80;
	uint8_t download = 0x01;
	vji_write(target, 2, &reset, 1);
	vji_write(target, 2, &download, 1);
	vTaskDelay(100);
	int length = (int)app[1];
	uint32_t dest = app[0];
	uint32_t runaddr = app[2];
	uint32_t *src = &app[3];
	//printf("Copying %d words to %08x.. ", length >> 2, dest);
	vji_write_memory(target, dest, length >> 2, src);
	//printf("Setting run address to %08x..", runaddr);
	vji_write_memory(target, APPLICATION_RUN, 1, &runaddr);
	//printf("Done!\n");
}

void read_bytes(JTAG_Access_t *target, uint32_t address, int bytes, uint8_t *dest)
{
	//("ReadBytes %x %d => ", address, bytes);
	uint32_t first_word = address >> 2;
	uint32_t last_word = (address + bytes -1) >> 2;
	int words = last_word - first_word + 1;
	int offset = address & 3;
	address &= -4;

	uint32_t buffer[256]; // 1K transfer per time
	while(bytes > 0) {
		int words_now = (words > 256) ? 256 : words;
		//printf("Read VJI: %08x %d ", address, words_now);
		vji_read_memory(target, address, words_now, buffer);
		int bytes_now = (words_now * 4) - offset;
		if (bytes_now > bytes)
			bytes_now = bytes;
		memcpy(dest, ((uint8_t *)buffer) + offset, bytes_now);
		bytes -= bytes_now;
		dest += bytes_now;
		address += bytes_now;
		offset = 0;
	}
}

int getchar_from_dut(JTAG_Access_t *target)
{
	static uint8_t message_buffer[1024];
	static int available = 0;
	static int read_pos = 0;

	struct {
		uint32_t location;
		uint32_t size;
		uint32_t head;
		uint32_t tail;
	} message_desc;

	if (available == 0) {
		vji_read_memory(target, BUFFER_LOCATION, 4, (uint32_t *)&message_desc);

		uint32_t addr = message_desc.location + message_desc.tail;
		int message_length = (int)message_desc.head - (int)message_desc.tail;

		if (message_length < 0) {
			message_length = (int)message_desc.size - (int)message_desc.tail;
		}
		if (message_length > 1024) {
			message_length = 1024;
		}
		if (message_length > 0) {
			//printf("Buffer is at %p. Size of buffer = %d. Head = %d. Tail = %d.\n", message_desc.location,
			//		message_desc.size, message_desc.head, message_desc.tail );
			read_bytes(target, addr, message_length, message_buffer);
			available = message_length;
			read_pos = 0;

			message_desc.tail += message_length;
			if (message_desc.tail >= message_desc.size) {
				message_desc.tail -= message_desc.size;
			}
			vji_write_memory(target, BUFFER_TAIL, 1, &(message_desc.tail));
		}
	}
	if (available > 0) {
		available--;
		return (int)message_buffer[read_pos++];
	}
	return -1;
}

int executeDutCommand(JTAG_Access_t *target, uint32_t commandCode, int timeout, char **log)
{
	uint32_t pre = 0;
	TickType_t tickStart = xTaskGetTickCount();
	vji_read_memory(target, DUT_TO_TESTER, 1, &pre);
	vji_write_memory(target, TESTER_TO_DUT, 1, &commandCode);
	uint32_t post;
	while(1) {
		vTaskDelay(1);
		if (!log) {
			while(1) {
				int ch = getchar_from_dut(target);
				if (ch == -1) {
					break;
				}
				putchar(ch);
			}
		}
		vji_read_memory(target, DUT_TO_TESTER, 1, &post);
		if (post != pre) {
			break;
		}
		TickType_t now = xTaskGetTickCount();
		if((now - tickStart) > (TickType_t)timeout) {
			return -99; // timeout
		}
	}
	uint32_t retval;
	vji_read_memory(target, TEST_STATUS, 1, &retval);

	char logBuffer[4096];
	int len = 0;
	while(len < 4095) {
		int ch = getchar_from_dut(target);
		if (ch == -1) {
			break;
		}
		logBuffer[len++] = (char)ch;
		// putchar(ch);
	}
	logBuffer[len] = 0;
	printf(logBuffer); // just dump, we cannot store it

	if (log) {
		if (len) {
			*log = new char[len+2];
			strcpy(*log, logBuffer);
		} else {
			*log = NULL;
		}
	}
	return (int)retval;
}

int LedTest(JTAG_Access_t *target)
{
	int errors = 0;
	uint8_t byte = 0x80; // reset!
	vji_write(target, 2, &byte, 1);
	vTaskDelay(50);
	int cur = adc_read_current();
	printf("All LEDs off: %d mA\n", cur);
	byte = 0x81;
	for(int i=0;i<4;i++) {
		vji_write(target, 2, &byte, 1);
		vTaskDelay(10);
		int ledCurrent = adc_read_current() - cur;
		printf("LED = %02x: %d mA\n", byte, ledCurrent);
		byte <<= 1;
		byte |= 0x80;
		if ((ledCurrent < 3) || (ledCurrent > 7)) {
			errors ++;
		}
	}
	byte = 0x00;
	vji_write(target, 2, &byte, 1);
	vTaskDelay(10);

	return errors;
}

int checkFpgaIdCode(volatile uint32_t *jtag)
{
	uint8_t idcode[4] = {0, 0, 0, 0};
	jtag_reset_to_idle(jtag);
	jtag_instruction(jtag, 6);
	jtag_senddata(jtag, idcode, 32, idcode);
	printf("ID code found: %02x %02x %02x %02x\n", idcode[3], idcode[2], idcode[1], idcode[0]);
	if ((idcode[3] != 0x02) || (idcode[2] != 0x0F) || (idcode[1] != 0x30) || (idcode[0] != 0xdd)) {
		return 1;
	}
	return 0;
}


int program_flash(JTAG_Access_t *target, BinaryImage_t *flashFile)
{
	uint32_t dut_addr = 0xA00000; // 10 MB from start - arbitrary! Should use malloc
	uint32_t destination = flashFile->flashAddress;
	uint32_t length = flashFile->size;
	int words = (length + 3) >> 2;
	vji_write_memory(target, dut_addr, words, flashFile->buffer);
	vji_write_memory(target, PROGRAM_ADDR, 1, &destination);
	vji_write_memory(target, PROGRAM_DATALEN, 1, &length);
	vji_write_memory(target, PROGRAM_DATALOC, 1, &dut_addr);

	printf("Programming %s\n", flashFile->romName);
	return executeDutCommand(target, 12, 60*200, NULL);
}



/*
int test(void)
{
	IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, (0x0F << 4));
	vTaskDelay(100);
	configure_adc();

	// jig_test();
	// usb2.initHardware();

	return 0;
	//TestAudio();
	//return 0;

	for(int i=0;i<5;i++) {
		report_analog();
		for(int i=0;i<500000;i++)
			;
	}

//	printf("SVF file @ %p (%d)\n", &_dut_b_start, (int)&_dut_b_size);
//	play_svf((char *)(&_dut_b_start), (volatile uint32_t *)JTAG_1_BASE);

	uint8_t idcode[4] = {0, 0, 0, 0};

	volatile uint32_t *jtag0 = (volatile uint32_t *)JTAG_0_BASE;
	volatile uint32_t *jtag1 = (volatile uint32_t *)JTAG_1_BASE;

	jtag_reset_to_idle(jtag0);
	jtag_instruction(jtag0, 6);
	jtag_senddata(jtag0, idcode, 32, idcode);

	printf("ID code of DUT: %02x %02x %02x %02x\n", idcode[3], idcode[2], idcode[1], idcode[0]);

	printf("Going to configure the FPGA.\n");
	if (!dutFpga.buffer) {
		printf("No FPGA image loaded from USB.\n");
		return -8;
	}
	jtag_configure_fpga(jtag1, (uint8_t *)dutFpga.buffer, (int)dutFpga.size);

	for(int i=0;i<1000000;i++)
		;
	report_analog();

	JTAG_Access_t jig;
	if (FindJtagAccess(jtag0, &jig) < 0) {
		return -1;
	}
	for(int i=0;i<5;i++) {
		report_analog();
		for(int i=0;i<500000;i++)
			;
	}
	uint8_t leds_on = 0x0F;
	vji_write(&jig, 2, &leds_on, 1);
	for(int i=0;i<5;i++) {
		report_analog();
		for(int i=0;i<500000;i++)
			;
	}

	jtag_reset_to_idle(jtag1);
	jtag_instruction(jtag1, 6);
	jtag_senddata(jtag1, idcode, 32, idcode);

	printf("ID code of DUT: %02x %02x %02x %02x\n", idcode[3], idcode[2], idcode[1], idcode[0]);

	JTAG_Access_t access;
	if (FindJtagAccess(jtag1, &access) < 0) {
		return -1;
	}

	uint8_t vji_data[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

	vji_read(&access, 0, vji_data, 8);
	for(int i=0;i<8;i++) {
		printf("%02x ", vji_data[i]);
	}
	printf("\n");

	vji_read(&access, 1, vji_data, 8);
	for(int i=0;i<8;i++) {
		printf("%02x ", vji_data[i]);
	}
	printf("\n");

	uint8_t led = 0x05;
	vji_write(&access, 2, &led, 1);


	uint32_t mem_block[64];
	for(int i=0;i<64;i++) {
		mem_block[i] = 0x01010101 * i;
	}
	mem_block[0] = 0xDEADBABE;

	vji_write_memory(&access, 0x00020020, 32, mem_block);
	vji_write_memory(&access, 0x000200A0, 32, mem_block);

	uint32_t mem_block_read[64];
	vji_read_memory(&access, 0x00020020, 64, mem_block_read);

	for(int i=0;i<64;i++) {
		printf("%08x ", mem_block_read[i]);
		if ((i & 3) == 3)
			printf("\n");
	}
	int err = 0;
	for(int i=0;i<32;i++) {
		if (mem_block_read[i] != mem_block[i]) {
			err++;
		}
		if (mem_block_read[i+32] != mem_block[i]) {
			err++;
		}
	}
	printf("%d Errors in readback.\n", err);

}
*/

int load_file(BinaryImage_t *flashFile)
{
	FRESULT fres;
	File *file;
	uint32_t transferred;

	FileManager *fm = FileManager :: getFileManager();
	fres = fm->fopen(flashFile->fileName, FA_READ, &file);
	if (fres == FR_OK) {
		flashFile->size = file->get_size();
		flashFile->buffer = (uint32_t *)malloc(flashFile->size + 8);
		fres = file->read(flashFile->buffer, flashFile->size, &transferred);
		if (transferred != flashFile->size) {
			printf("Expected to read %d bytes, but got %d bytes.\n", flashFile->size, transferred);
			return -1;
		}
	} else {
		printf("Warning: Could not open file '%s'! %s\n", flashFile->fileName, FileSystem :: get_error_string(fres));
		return -2;
	}
	printf("Successfully read %-35s. Size = %8d. Stored at %p.\n", flashFile->fileName, flashFile->size, flashFile->buffer);
	return 0;
}

#include "report.h"
int jigPowerSwitchOverTest(JTAG_Access_t *target, int timeout, char **log)
{
	IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, 0xF0); // turn off DUT
	vTaskDelay(200);

	// 1 = vcc
	// 2 = v50

	int errors = 0;
	// first turn power on through VCC.. This should immediately result in power on V50.
	IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, (1 << 6));
	vTaskDelay(5);
	{
		uint32_t v50 = adc_read_channel_corrected(2);
		uint32_t vcc = adc_read_channel_corrected(1);
		printf("C64VCC: V50=%d, VCC=%d.\n", v50, vcc);
		if ((vcc < 4800) || (v50 < 4800)) {
			errors |= 1;
		}
	}
	vTaskDelay(200);
	jtag_clear_fpga(target->host);
	vTaskDelay(100);
	{
		uint32_t v50 = adc_read_channel_corrected(2);
		uint32_t vcc = adc_read_channel_corrected(1);
		printf("C64VCC: V50=%d, VCC=%d.\n", v50, vcc);
		if ((vcc < 4800) || (v50 < 4800)) {
			errors |= 2;
		}
	}
	IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, (1 << 6)); // turn off power on VCC
	vTaskDelay(200);


	IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, (1 << 7)); // turn on ext vcc
	vTaskDelay(5);
	{
		uint32_t v50 = adc_read_channel_corrected(2);
		uint32_t vcc = adc_read_channel_corrected(1);
		printf("ExtVCC: V50=%d, VCC=%d.\n", v50, vcc);
		if ((vcc > 750) || (v50 > 750)) {
			errors |= 4;
		}
	}
	vTaskDelay(200);
	jtag_clear_fpga(target->host);
	vTaskDelay(100);
	{
		uint32_t v50 = adc_read_channel_corrected(2);
		uint32_t vcc = adc_read_channel_corrected(1);
		printf("ExtVCC: V50=%d, VCC=%d.\n", v50, vcc);
		if ((vcc > 750) || (v50 < 4800)) {
			errors |= 8;
		}
	}
	IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, (1 << 7));
	vTaskDelay(100);

	return errors;
}

int jigVoltageRegulatorTest(JTAG_Access_t *target, int timeout, char **log)
{
	//IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, (1 << 7));
	IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0xE0);
	vTaskDelay(200);
	jtag_clear_fpga(target->host);
	vTaskDelay(100);
	report_analog();
	int errors = validate_analog(1);
	if (errors) {
		printf("Please verify power supplies before continuing.\n");
		return -1;
	}
	return 0;
}

int bringupIdCode(JTAG_Access_t *target, int timeout, char **log)
{
	if (checkFpgaIdCode(target->host)) {
		printf("No FPGA / Wrong FPGA type.\n");
		return -2;
	}
	return 0;
}

int bringupConfigure(JTAG_Access_t *target, int timeout, char **log)
{
	// Configure FPGA
	if (!dutFpga.buffer) {
		printf("No FPGA image loaded from USB.\n");
		return -1;
	}
	jtag_configure_fpga(target->host, (uint8_t *)dutFpga.buffer, (int)dutFpga.size);
	vTaskDelay(100);
	report_analog();

	if (FindJtagAccess(target->host, target) < 0) {
		return -2;
	}
	printf("FPGA seems to be correctly configured.\n");
	return 0;
}

int checkReferenceClock(JTAG_Access_t *target, int timeout, char **log)
{
	uint8_t clock1[2] = { 0, 0 };
	int retries = 5;
	do {
		vji_read(target, 8, &clock1[0], 1);
		vji_read(target, 8, &clock1[1], 1);
		retries --;
	} while((clock1[1] == clock1[0]) && (retries));

	printf("50 MHz clock detection: %02x %02x\n", clock1[0], clock1[1]);
	if (clock1[0] == clock1[1]) {
		printf("Failed to detect 50 MHz reference clock from Ethernet PHY.\n");
		return -4;
	}
	return 0;
}

int checkApplicationRun(JTAG_Access_t *target, int timeout, char **log)
{
	run_application_on_dut(target, dutAppl.buffer);

	int started = executeDutCommand(target, 99, timeout, log);
	if (started) {
		printf("Seems that the DUT did not start test software.\n");
	}
	return started;
}

int checkLEDs(JTAG_Access_t *target, int timeout, char **log)
{
	// Functional tests start here
	int leds = LedTest(target);
	if (leds) {
		printf("One or more of the LEDs failed.\n");
	}
	return leds;
}

int checkSpeaker(JTAG_Access_t *target, int timeout, char **log)
{
	int mono = executeDutCommand(target, 1, timeout, log);
	if (mono) {
		printf("Request for mono sound failed.\n");
		return -1;
	} else {
		int speaker = testDutSpeaker();
		if (speaker) {
			printf("Speaker output failed.");
			return -2;
		}
	}
	return 0;
}

int checkUsbClock(JTAG_Access_t *target, int timeout, char **log)
{
	int retries = 5;
	uint8_t clock2[2] = { 0, 0 };
	vji_read(target, 9, &clock2[0], 1);
	do {
		vji_read(target, 9, &clock2[0], 1);
		vji_read(target, 9, &clock2[1], 1);
		retries --;
	} while((clock2[1] == clock2[0]) && (retries));

	printf("60 MHz clock detection: %02x %02x\n", clock2[0], clock2[1]);
	if (clock2[0] == clock2[1]) {
		printf("Failed to detect 60 MHz Clock from PHY.\n");
		return -1;
	}
	return 0;
}

int checkUsbPhy(JTAG_Access_t *target, int timeout, char **log)
{
	int phy = executeDutCommand(target, 7, timeout, log);
	if (phy) {
		printf("USB PHY detection failed.\n");
	}
	return phy;
}

int checkRtcAccess(JTAG_Access_t *target, int timeout, char **log)
{
	int rtc = executeDutCommand(target, 13, timeout, log); // RTCAccessTst
	if (rtc) {
		printf("Failed to access RTC chip.\n");
	}
	return rtc;
}

int dutPowerOn(JTAG_Access_t *target, int timeout, char **log)
{
	codec_init();
	IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, 0xF0); // disable JIG
	IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x10); // enable DUT

	vTaskDelay(200);
	jtag_clear_fpga(target->host);
	vTaskDelay(100);
	// TODO: Test current ~45-85 mA; not working on Tester V1.
	return 0;
}

int slotAudioInput(JTAG_Access_t *target, int timeout, char **log)
{
	// Audio test
	// Request stereo output from DUT
	IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, (1 << 31));
	int aud;

	// Now test the stereo input of the DUT
	aud = startAudioOutput(128);
	if (aud) {
		printf("Request to play audio locally failed.\n");
		return -1;
	}
	vTaskDelay(200);
	aud = executeDutCommand(target, 3, timeout, NULL);
	if (aud) {
		printf("DUT Audio input failed.\n");
	}
	return aud;
}

int slotAudioOutput(JTAG_Access_t *target, int timeout, char **log)
{
	// Now test the stereo output of the DUT
	int aud = executeDutCommand(target, 2, timeout, log);
	if (aud) {
		printf("Request to play audio on DUT failed.\n");
		return aud;
	}
	aud = TestAudio(11, 7, 1);
	if (aud) {
		printf("Audio signal check failed.\n");
		return aud;
	}
	return 0;
}

int checkRtcAdvance(JTAG_Access_t *target, int timeout, char **log)
{
	int errors = 0;

	// RTC Read
	uint32_t timebuf1[3];
	int rtc = executeDutCommand(target, 14, timeout, log); // readRTC
	if (rtc) {
		printf("Failed to read RTC for the first time.\n");
		errors ++;
	} else {
		vji_read_memory(target, TIMELOC, 3, timebuf1);
	}

	vTaskDelay(300);

	// Read RTC and verify with previous result
	rtc = executeDutCommand(target, 14, timeout, log); // readRTC
	if (rtc) {
		printf("Failed to read RTC for the second time.\n");
		errors ++;
	} else {
		uint32_t timebuf2[3];
		vji_read_memory(target, TIMELOC, 3, timebuf2);
		uint8_t *t1, *t2;
		t1 = (uint8_t *)timebuf1;
		t2 = (uint8_t *)timebuf2;
		int same = 0;
		for(int i=0; i<11; i++) {
			if (t1[i] == t2[i]) {
				same++;
			}
		}
		if (same == 0) {
			printf("RTC did not advance.\n");
			errors ++;
		} else if (same < 6) {
			printf("Many bytes differ, RTC faulty.\n");
			errors ++;
		}
	}
	return errors;
}

int checkNetworkTx(JTAG_Access_t *target, int timeout, char **log)
{
	int eth;
	eth = executeDutCommand(target, 5, timeout, log);
	if (eth) {
		printf("Request to send network packet failed.\n");
		return -1;
	}
	vTaskDelay(3);
	uint8_t *packet;
	int length;
	if (getNetworkPacket(&packet, &length) == 0) {
		printf("No network packet received from DUT.\n");
		return -2;
	} else {
		printf("Network packet received with length = %d ", length);
		if (length != 900) {
			printf("OH no! \n");
			return -3;
		} else {
			printf("YES! \n");
		}
	}
	return 0;
}

int checkNetworkRx(JTAG_Access_t *target, int timeout, char **log)
{
	// Now the other way around! We do the sending.
	int retval = sendEthernetPacket(1000);
	if (retval) {
		printf("Tester failed to send network packet.\n");
		return -1;
	}
	vTaskDelay(10);
	int eth = executeDutCommand(target, 6, timeout, log);
	if (eth) {
		printf("DUT failed to receive network packet.\n");
	}
	return eth;
}

int checkUsbHub(JTAG_Access_t *target, int timeout, char **log)
{
	// Now, let's start the USB stack on the DUT
	int usb = executeDutCommand(target, 11, timeout, log);
	if (usb) {
		printf("Failed to start USB on DUT.\n");
		return usb;
	}

	vTaskDelay(800);

	// Check for USB Hub
	usb = executeDutCommand(target, 8, timeout, log);
	if (usb) {
		printf("USB HUB check failed.\n");
	}
	return usb;
}

int checkUsbSticks(JTAG_Access_t *target, int timeout, char **log)
{
	vTaskDelay(1500);

	// Check for USB sticks
	int usb = executeDutCommand(target, 9, timeout, log);
	if (usb) {
		printf("*** USB check for 3 devices failed.\n");
	}
	return usb;
}

int flashRoms(JTAG_Access_t *target, int timeout, char **log)
{
	int errors = 0;

	// Flash Programming
	int flash = 0;
	for (int i=0; i<5; i++) {
		if (!toBeFlashed[i].buffer) {
			printf("No Valid data for '%s' => Cannot flash.\n", toBeFlashed[i].romName);
			errors ++;
		} else {
			flash = program_flash(target, &toBeFlashed[i]);
		}
		if (flash) {
			printf("Flashing failed.\n");
			errors ++;
			break;
		}
	}
	if (!errors) {
		executeDutCommand(target, 16, timeout, log);
	}
	return errors;
}

int checkButtons(JTAG_Access_t *target, int timeout, char **log)
{
	printf(" ** PRESS EACH OF THE BUTTONS ON THE DUT **\n");
	int retval = executeDutCommand(target, 10, timeout, log);
	printf(" ** THANK YOU **\n");
	return retval;
}

int checkFlashSwitch(JTAG_Access_t *target, int timeout, char **log)
{
	int retval = executeDutCommand(target, 4, timeout, log);
	return retval;
}

int checkDigitalIO(JTAG_Access_t *target, int timeout, char **log)
{
	return TestIOPins(target);
}

TestDefinition_t jig_tests[] = {
		{ "Power SwitchOver Test",  jigPowerSwitchOverTest,  1, false, false, false },
		{ "Voltage Regulator Test", jigVoltageRegulatorTest, 1,  true, false, false },
		{ "Check FPGA ID Code",     bringupIdCode,           1,  true, false, false },
		{ "Configure the FPGA",     bringupConfigure,        1,  true, false, false },
		{ "Verify reference clock", checkReferenceClock,     1,  true, false, false },
		{ "Verify LED presence",    checkLEDs,               2, false, false, false },
		{ "Verify DUT appl running",checkApplicationRun,   150,  true, false, false },
		{ "Check Flash Types",      checkFlashSwitch,      150,  true, false,  true },
		{ "Audio amplifier test",   checkSpeaker,          150, false, false,  true },
		{ "Verify USB Phy clock",   checkUsbClock,           1, false, false,  true },
		{ "Verify USB Phy type",    checkUsbPhy,           150, false, false,  true },
		{ "RTC access test",        checkRtcAccess,        150, false, false,  true },
		{ "", NULL, 0, false, false, false }
};

TestDefinition_t slot_tests[] = {
		{ "Power up DUT in slot",   dutPowerOn,              1, false, false, false },
		{ "Check FPGA ID Code",     bringupIdCode,           1,  true, false,  true },
		{ "Configure the FPGA",     bringupConfigure,        1,  true, false,  true },
		{ "Digital I/O test",       checkDigitalIO,          1, false, false,  true },
		{ "Verify reference clock", checkReferenceClock,     1,  true, false,  true },
		{ "Verify DUT appl running",checkApplicationRun,   150,  true, false,  true },
		{ "Button Test", 			checkButtons,		  3000, false,  true,  true },
		{ "Audio input test",       slotAudioInput,        600, false, false,  true },
		{ "Audio output test",      slotAudioOutput,       600, false, false,  true },
		{ "RTC advance test",       checkRtcAdvance,       400, false, false,  true },
		{ "Ethernet Tx test",       checkNetworkTx,        120, false, false,  false },
		{ "Ethernet Rx test",       checkNetworkRx,        120, false, false,  false },
		{ "Check USB Hub type",     checkUsbHub,           600, false, false,  true },
		{ "Check USB Ports (3x)",   checkUsbSticks,        600, false, false,  true },
		{ "Program Flashes",        flashRoms,             200, false,  true,  true },
		{ "", NULL, 0, false, false, false }
};
/*
		const char *nameOfTest;
		TestFunction_t function;
		int  timeout; // in seconds
		bool breakOnFail;
		bool skipWhenErrors;
		bool logInSummary;
*/

StreamTextLog logger(8192);

void outbyte_log(int c)
{
	logger.charout(c);
	// Wait for space in FIFO
	while (ioRead8(UART_FLAGS) & UART_TxFifoFull);
	ioWrite8(UART_DATA, c);
}

int writeLog(StreamTextLog *log, const char *prefix, const char *name)
{
	char total_name[100];
	strcpy(total_name, prefix);
	strcat(total_name, name);

	FileManager *fm = FileManager :: getFileManager();
	File *file;
	FRESULT fres = fm->fopen(total_name, FA_WRITE | FA_CREATE_NEW, &file);
	if (fres == FR_OK) {
		uint32_t transferred = 0;
		file->write(log->getText(), log->getLength(), &transferred);
		if (transferred != log->getLength()) {
			printf("Warning! Log file not (completely) written.\n");
		}
		fm->fclose(file);
	} else {
		printf("WARNING! Log file could not be written!\n");
	}
	return fres;
}

extern "C" {
	void main_task(void *context)
	{
		IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, 0xFF);
		IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x08); // green
		configure_adc();
		printf("Welcome to the Ultimate-II+ automated test system.\n");
		printf("Initializing local USB devices...\n");

		// rtc.set_time_in_chip(0, 2016 - 1980, 11, 19, 6, 14, 14, 0);
		custom_outbyte = outbyte_log;

		usb2.initHardware();
		FileManager *fm = FileManager :: getFileManager();

		printf("Waiting for USB storage device to become available.\n");
		FileInfo info(32);
		FRESULT res;
		do {
			vTaskDelay(100);
			res = fm->fstat("/Usb?", info);
			//printf("%s\n", FileSystem :: get_error_string(res));
		} while (res != FR_OK);

		// fm->print_directory("/Usb?");

		for (int i=0; i < 5; i++) {
			load_file(&toBeFlashed[i]);
		}

		// Initialize fpga and application structures for DUT
		dutFpga.buffer = (uint32_t *)&_dut_b_start;
		dutFpga.size   = (uint32_t)&_dut_b_size;
		dutAppl.buffer = (uint32_t *)&_dut_application_start;
		// printf("%p %d\n", (uint32_t *)&_dut_b_start, (uint32_t)&_dut_b_size);
		load_file(&dutFpga);
		load_file(&dutAppl);

		TestSuite jigSuite("JIG Test Suite", jig_tests);
		TestSuite slotSuite("Slot Test Suite", slot_tests);

		printf("\nPress Left Button to run test on JIG. Press Right button to run test in Slot.\n");
		while(1) {
			uint8_t buttons = getButtons();
			if (!buttons) {
				continue;
			}
			IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, 0x03); // red+green
			if (buttons == 0x01) {
				jigSuite.Reset();
				logger.Reset();
				IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x04); // yellow one
				jigSuite.Run((volatile uint32_t *)JTAG_0_BASE);
				IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, 0xF4);
				if (!jigSuite.Passed()) {
					IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x02); // red one
				} else {
					IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x01); // green one
				}
				jigSuite.Report();
				logger.Stop();
				writeLog(&logger, "/Usb?/logs/jig_", jigSuite.getDateTime());
			}
			if (buttons == 0x04) {
				slotSuite.Reset();
				logger.Reset();
				IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x04);
				slotSuite.Run((volatile uint32_t *)JTAG_1_BASE);
				IOWR_ALTERA_AVALON_PIO_CLEAR_BITS(PIO_1_BASE, 0xF4);
				if (!jigSuite.Passed()) {
					IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x02); // red one
				} else {
					IOWR_ALTERA_AVALON_PIO_SET_BITS(PIO_1_BASE, 0x01); // green one
				}
				slotSuite.Report();
				logger.Stop();
				writeLog(&logger, "/Usb?/logs/slot_", jigSuite.getDateTime());
			}
			vTaskDelay(20);
			printf("\nPress Left Button to run test on JIG. Press Right button to run test in Slot.\n");
		}
		printf("Test has terminated.\n");
		while(1) {
			vTaskDelay(100);
		}
	}
}