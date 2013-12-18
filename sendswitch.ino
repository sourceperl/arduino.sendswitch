/*
  sendSwitch V0.1

  Tiny code for manage Arduino GSM shield (base on SM5100b chip).
  
  This example code is in the public domain.
  Share, it's happiness !

  Note : average consumption 65 ma on 12VDC.
  With cards : OLIMEXINO-328,
               Cellular shield with SM5100B (i use Sparkfun boards), 
               Seedstudio relay shield V2.0
               
  Usage : 
    Your mobile must 
    - send SMS with [PIN CODE] R[1 or 2] [0,1 or T for toggle, blank for read] for manage relay
    - send SMS with [PIN CODE] INFO for read device information
    sample: "1111 R2 T" for toggle state of relay 2
            "1111 INFO" for read device info like RSSI, SMS counter
*/

/* library */
// Arduino core
#include <SoftwareSerial.h>
#include <avr/power.h>
#include <avr/sleep.h>
#include "Wire.h"
// Timer : must use at least v1.1 for fix millis rolover issue
// come from https://github.com/JChristensen/Timer
#include "Timer.h"

// some const
#define TC_PIN "1111"
#define VERSION "0.1"

#define SMS_CHECK_INTERVAL   60000
#define STAT_UPDATE_INTERVAL 15000

#define TEST_LED 13
#define RELAY_1  7
#define RELAY_2  6
#define RELAY_3  5
#define RELAY_4  4

// some struct
struct st_SMS {
   int index;
  char status[16];
  char phonenumber[16]; 
  char datetime[25];
  char msg[161];
};

// some vars
SoftwareSerial gsm_modem(2, 3); // RX, TX
Timer  t;
int    job_sms;
int    job_stat;
char   rx_buf[64];
char   tx_buf[64];
byte   rx_index = 0;
byte   tx_index = 0;
int    rssi = 0;  
byte   bar = 0;
char   txt_buffer[128];
st_SMS sms;
unsigned int sms_counter = 0;
// system flags
boolean _system_start = false;
boolean _modem_init   = false;
boolean gsm_net_up    = false;
boolean gsm_at_ready  = false;

// link stdout (printf) to Serial object
// create a FILE structure to reference our UART and LCD output function
static FILE uartout = {0};

// create a output function
// This works because Serial.write, although of
// type virtual, already exists.
static int uart_putchar (char c, FILE *stream)
{
  Serial.write(c);
  return 0;
}

void setup()
{
  // IO setup
  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);
  pinMode(RELAY_3, OUTPUT);
  pinMode(RELAY_4, OUTPUT);
  pinMode(TEST_LED, OUTPUT);
  digitalWrite(TEST_LED, LOW);
  // memory setup
  memset(tx_buf, 0, sizeof(tx_buf));
  memset(rx_buf, 0, sizeof(rx_buf));
  // open serial communications, link Serial to stdio lib
  Serial.begin(9600);
  // fill in the UART file descriptor with pointer to writer
  fdev_setup_stream (&uartout, uart_putchar, NULL, _FDEV_SETUP_WRITE);
  // standard output device STDOUT is uart
  stdout = &uartout ;
  // set the data rate for the SoftwareSerial port
  gsm_modem.begin(9600);
  gsm_modem.setTimeout(2000);
  // start message
  printf_P(PSTR("sendSwitch V%s\r\n"), VERSION);
  delay_idle(1500);
  printf_P(PSTR("init gsm chipset...\r\n"));
}

void loop()
{ 
  // system startup init (once network is up)
  if (!_system_start & gsm_net_up & gsm_at_ready) {
    modem_startup();
    _system_start = true;
    // init timer job (after instead of every for regulary call delay)
    job_sms  = t.after(SMS_CHECK_INTERVAL,   jobRxSMS);
    job_stat = t.after(STAT_UPDATE_INTERVAL, jobSTAT);
    jobSTAT();
  }
  // modem init
  if (_modem_init) {
    modem_init();
    _modem_init = false;
  }
  // do timer job
  t.update();
  // animate relay 4 (system ready)
  digitalWrite(RELAY_4, (gsm_net_up & gsm_at_ready & _system_start));
  // DEBUG : ASCII ANALYSER
  // *** char gsm modem -> console
  while(gsm_modem.available()) {
    // check incoming char
    int c = gsm_modem.read();
    if (c != 0)
      rx_buf[rx_index] = c;
    // send data ?
    if (c == '\n') {
      char * msg_pos;
      if ((msg_pos = strstr_P(rx_buf, PSTR("+SIND:"))) != 0) {
        /*
        SIND code
          0 SIM card removed
          1 SIM card inserted
          2 Ring melody
          3 AT module is partially ready
          4 AT module is totally ready
          5 ID of released calls
          6 Released call whose ID=<idx>
          7 The network service is available for an emergency call
          8 The network is lost
          9 Audio ON
          10 Show the status of each phonebook after init phrase
          11 Registered to network
        */
        msg_pos += sizeof("+SIND:");
        long int sind_code = strtol(msg_pos, &msg_pos, 10);
        printf_P(PSTR("SIND message code %d\r\n"), sind_code);
        // update system flag
        switch (sind_code) {
          case 4:
            gsm_at_ready = true;
            printf_P(PSTR("SIND: AT module ready\r\n"));
            break;
          case 8:
            gsm_net_up = false;
            printf_P(PSTR("SIND: network lost\r\n"));
            break;
          case 11:
            gsm_net_up = true;
            printf_P(PSTR("SIND: registered to network\r\n"));
            break;
        }
      // search notice message
      // CMT message for incoming SMS notice : launch sms job, message syntax
      // +CMT: [sender = "+3307...40"],[sc "+33...41"],[timestamp = "13/12/14,17:01:51+04"],[sms size]<CR><LF>
      // sms text<CR><LF>
      } else if (strncmp(rx_buf, "+CMT: ", 6) == 0) {
        printf_P(PSTR("CMT message launch jobRxSMS()\r\n"));
        jobRxSMS();
      } else {
        // display string like: "rx: 4f[O] 4b[K] 0d[ ] 0a[ ]"
        printf_P(PSTR("rx: "));
        byte i;
        for (i = 0; i <= rx_index; i++)
          printf_P(PSTR("%02x[%c] "), rx_buf[i], (rx_buf[i] > 0x20) ? rx_buf[i] : ' ');
        printf_P(PSTR("\r\n"));
      }
       rx_index = 0;
       memset(rx_buf, 0, sizeof(rx_buf));
    } else {
      // process data pointer
      if (rx_index < (sizeof(rx_buf)-2))
        rx_index++;
    }
  }
  // *** char console -> gsm modem
  while(Serial.available()) {
    // check incoming char
    int c = Serial.read();
    // send data ?
    if ((c == '\r') || (c == '\n')) {
      // set "dump" command : print RAM map
      if (strncmp(tx_buf, "DUMP", 4) == 0) {
        print_RAM_map();
      // set "rssi" command : print signal level
      } else if (strncmp(tx_buf, "RSSI", 4) == 0) {
        if (rssi != 0)
          printf_P(PSTR("lvl:%04d dbm %d\r\n"), rssi, bar);
        else
          printf_P(PSTR("lvl: n/a\r\n"));
      // set "INIT" command : init new SM5100b board
      } else if (strncmp(tx_buf, "INIT", 4) == 0) {
        _modem_init = true;
      // send a RAW AT command
      } else {
        // add end of AT command
        tx_buf[tx_index] = '\r';
        // display string like: "tx: 41[A] 54[T] 0a[ ]"
        printf_P(PSTR("tx: "));
        byte i;
        for (i = 0; i <= tx_index; i++) {
          gsm_modem.write(tx_buf[i]);
          printf_P(PSTR("%02x[%c] "), tx_buf[i], (tx_buf[i] > 0x20) ? tx_buf[i] : ' ');
        }
        printf_P(PSTR("\r\n"));
      }
      // reset tx buffer
      tx_index = 0;
      memset(tx_buf, 0, sizeof(tx_buf));    
    // add data to tx buffer
    } else if (c != 0) {
      tx_buf[tx_index] = c;
      // process data pointer
      if (tx_index < (sizeof(tx_buf)-2))
        tx_index++;
    }    
  }
  // set IDLE mode for 20 ms
  delay_idle(20);
}

// *** jobs rotines ***

// job "stat RSSI"
void jobSTAT(void)
{
  // get RSSI, process result
  if (rssi = get_RSSI()) {
    // create bar indicator (0 = low level rssi to 5 = high level)
    bar = 0;
    if (rssi >= -110) bar++;
    if (rssi >= -105) bar++;
    if (rssi >= -95)  bar++;
    if (rssi >= -83)  bar++;
    if (rssi >= -75)  bar++;
  } else {
    rssi = 0;
    bar  = 0;
  }   
  // call next time
  job_stat = t.after(STAT_UPDATE_INTERVAL, jobSTAT);
}

// job "SMS receive polling"
void jobRxSMS(void) 
{
  // check SMS index
  printf_P(PSTR("get_last_SMS_index()\r\n"));
  sms.index = get_last_SMS_index();
  if (sms.index > 0) {
    // wait 100 ms
    delay_idle(100);
    // update stat
    sms_counter++;
    // read SMS
    printf_P(PSTR("get_SMS index:%d\r\n"), sms.index);
    // try to populate sms struct with current SMS value
    if (get_SMS(sms.index)) {
      printf_P(PSTR("%d\r\n"), sms.index);
      printf_P(PSTR("%s\r\n"), sms.status);
      printf_P(PSTR("%s\r\n"), sms.phonenumber);
      printf_P(PSTR("%s\r\n"), sms.datetime);
      printf_P(PSTR("%s\r\n"), sms.msg);
      // decode SMS
      // sms must begin with xxxx where x is security code
      if (strncmp(sms.msg, TC_PIN, 4) == 0) {
        // Relay 1
        if (strucasestr(sms.msg, "R1")) {
          // reset
          if (strucasestr(sms.msg, "R1 0")) 
            digitalWrite(RELAY_1, 0);
          // set
          else if (strucasestr(sms.msg, "R1 1")) 
            digitalWrite(RELAY_1, 1);
          // toggle
          else if (strucasestr(sms.msg, "R1 T")) 
            digitalWrite(RELAY_1, ! digitalRead(RELAY_1));
          // for R1 with nothing elese: just a read
          // send report
          printf_P(PSTR("SMS: RELAY 1 [%d]\r\n"), digitalRead(RELAY_1));
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("%s\n%s\nR1=%d\n"), "sendSwitch", "Relay status", digitalRead(RELAY_1));
          send_SMS(sms.phonenumber, txt_buffer);
        // Relay 2
        } else if (strucasestr(sms.msg, "R2")) {
          // reset
          if (strucasestr(sms.msg, "R2 0")) 
            digitalWrite(RELAY_2, 0);
          // set
          else if (strucasestr(sms.msg, "R2 1")) 
            digitalWrite(RELAY_2, 1);
          // toggle
          else if (strucasestr(sms.msg, "R2 T")) 
            digitalWrite(RELAY_2, ! digitalRead(RELAY_2));
          // for R2 with nothing elese: just a read
          // send report
          printf_P(PSTR("SMS: RELAY 2 [%d]\r\n"), digitalRead(RELAY_2));
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("%s\n%s\nR2=%d\n"), "sendSwitch", "Relay status", digitalRead(RELAY_2));
          send_SMS(sms.phonenumber, txt_buffer);
        // Reset all
        } else if (strucasestr(sms.msg, "RESET")) {
          digitalWrite(RELAY_1, 0);
          digitalWrite(RELAY_2, 0);
          printf_P(PSTR("SMS: RESET RELAY 1 and 2\r\n"));
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("%s\n%s\nR1=%d\nR2=%d\n"), "sendSwitch", "Relay status", digitalRead(RELAY_1), digitalRead(RELAY_2));
          send_SMS(sms.phonenumber, txt_buffer);
        
        // Info request
        } else if (strucasestr(sms.msg, "INFO")) {
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("SMS: MSG INFO"));
          printf_P(PSTR("%s\r\n"), txt_buffer);
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("%s\n%s\nup=%lu s\nrssi=%04d dbm\nsms=%d"), "sendSwitch", "Uptime (in s)", millis()/1000, rssi, sms_counter);
          send_SMS(sms.phonenumber, txt_buffer);          
        } else {
          snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("SMS: SYNTAX ERROR"));
          printf_P(PSTR("%s\r\n"), txt_buffer);          
        }
      } else {
        snprintf_P(txt_buffer, sizeof(txt_buffer), PSTR("SMS: PIN ERR"));
        printf_P(PSTR("%s\r\n"), txt_buffer);
      }
    }    
    delay_idle(1000);
    // delete SMS
    printf_P(PSTR("delete SMS\r\n"));
    // flush serial buffer
    gsm_modem.flush();
    // Delete all "read" messages
    gsm_modem.print(F("AT+CMGD=1,1\r"));
    // check result
    if (gsm_modem.find("\r\nOK\r\n")) {
      printf_P(PSTR("delete OK\r\n"));
    } else {
      printf_P(PSTR("delete error\r\n"));
    }
    
  } else {
    printf_P(PSTR("no SMS\r\n"));
  }
  // call next time
  job_sms = t.after(SMS_CHECK_INTERVAL, jobRxSMS);
}

// *** misc routines ***
boolean send_SMS(char *phone_nb, char *sms_msg) 
{
  // flush serial buffer
  gsm_modem.flush();
  // AT+CMGS="<phone number (like +33320...)>"\r
  gsm_modem.print(F("AT+CMGS=\""));
  gsm_modem.print(phone_nb);
  gsm_modem.print(F("\"\r"));
  delay_idle(50);
  // sms message
  gsm_modem.print(sms_msg);
  delay_idle(50);
  // end with <ctrl+z>
  gsm_modem.write(0x1A);
  // check result
  return gsm_modem.find("\r\nOK\r\n");
}

int get_last_SMS_index(void) 
{ 
  int sms_index = 0;
  // flush serial buffer
  gsm_modem.flush();
  // list "unread" SMS
  gsm_modem.print(F("AT+CMGL=\"REC UNREAD\"\r"));
  // parse result 
  if (! gsm_modem.findUntil("+CMGL:", "\r\nOK\r\n"))
    return 0; 
  sms_index = gsm_modem.parseInt();   
  // clean buffer before return
  while (gsm_modem.findUntil("+CMGL:", "\r\nOK\r\n"));
  return sms_index;
}

// read sms equal to sms_index in chip memory
// return status, phonenumber, datetime and msg.
int get_SMS(int index) 
{
  // init args
  sms.index = index;
  memset(sms.status, 0, 16);
  memset(sms.phonenumber, 0, 16);
  memset(sms.datetime, 0, 25);
  memset(sms.msg, 0, 128);
  // flush serial buffer
  gsm_modem.flush();
  // AT+CMGR=1\r
  gsm_modem.print(F("AT+CMGR="));
  gsm_modem.print(sms.index);
  gsm_modem.print(F("\r")); 
  // parse result
  //old  +CMGR: "REC READ","+33123456789","","2013/06/29 11:23:35+08"
  //new  +CMGR:"REC READ",0,"+8613052231025","07/03/28,15:29:16+00"<cr><lf>
  //this is SMS text<cr><lf>
  if (! gsm_modem.findUntil("+CMGR:", "\r\nOK\r\n")) 
    return 0; 
  gsm_modem.find("\"");
  gsm_modem.readBytesUntil('"', sms.status, sizeof(sms.status)-1);
  gsm_modem.find(",\"");
  gsm_modem.readBytesUntil('"', sms.phonenumber, sizeof(sms.phonenumber)-1);  
  gsm_modem.find(",\""); 
  //gsm_modem.find("\",\"");
  gsm_modem.readBytesUntil('"', sms.datetime, sizeof(sms.datetime)-1);
  gsm_modem.find("\r\n");
  gsm_modem.readBytesUntil('\r', sms.msg, sizeof(sms.msg)-1);
  return 1;  
  
}

// use "AT+CSQ" for retrieve RSSI
// 
// return O if data not available or command fault
int get_RSSI(void)
{
  // local vars
  int g_rssi;  
  int g_ber;
  // flush serial buffer
  gsm_modem.flush();
  gsm_modem.write("AT+CSQ\r");
  if (gsm_modem.find("+CSQ:")) {
    g_rssi = gsm_modem.parseInt();  
    g_ber  = gsm_modem.parseInt();
    // rssi "99" -> data not yet available
    if (g_rssi != 99) 
      // convert to dBm (0..31 -> -113..-51 dbm)
      g_rssi = (2 * g_rssi) - 113;
    else return 0; 
  } else return 0;
  gsm_modem.find("\r\nOK\r\n");
  return g_rssi;
}

void modem_startup(void) {
  // set SMS indication
  // flush serial buffer
  gsm_modem.flush();
  // delete all messages
  gsm_modem.print(F("AT+CNMI=3,3,0,0\r"));
  // check result
  if (gsm_modem.find("\r\nOK\r\n")) {
    printf_P(PSTR("init: sms indication on\r\n"));
  } else {
    printf_P(PSTR("init: error during sms indication settings\r\n"));
  }
  // flush serial buffer
  gsm_modem.flush();
  // delete all messages
  gsm_modem.print(F("AT+CMGD=1,4\r"));
  // check result
  if (gsm_modem.find("\r\nOK\r\n")) {
    printf_P(PSTR("flush sms\r\n"));
  } else {
    printf_P(PSTR("error: unable to flush sms\r\n"));
  }
}

void modem_init(void) {
  gsm_modem.setTimeout(2000);
  // no echo local
  gsm_modem.flush();
  gsm_modem.print(F("ATE0\r"));
  if (gsm_modem.find("\r\nOK\r\n"))
    printf_P(PSTR("init: echo local is off\r\n"));
  // set modem in text mode (instead of PDU mode)
  gsm_modem.flush();
  gsm_modem.print(F("AT+CMGF=1\r"));
  if (gsm_modem.find("\r\nOK\r\n")) {
    printf_P(PSTR("init: sms in text mode\r\n"));
  } else {
    printf_P(PSTR("init: unable to set sms text mode\r\n"));
    // loop forever
    while(1)
      cpu_idle();
  }  
  // strore result in NVRAM
  // flush serial buffer
  gsm_modem.flush();
  // delete all messages
  gsm_modem.print(F("AT&W\r"));
  // check result
  if (gsm_modem.find("\r\nOK\r\n")) {
    printf_P(PSTR("configuration saved\r\n"));
  } else {
    printf_P(PSTR("error during save\r\n"));
  }
  
}

// like std c "strstr" function but not case sensitive
char *strucasestr(char *str1, char *str2)
{
  // local vars  
  char *a, *b;
  // compare str 1 and 2 (not case sensitive)
  while (*str1++) {   
    a = str1;
    b = str2;
    while((*a++ | 32) == (*b++ | 32))
      if(!*b)
        return (str1);
   }
   return 0;
}

// set uc on idle mode (for power saving) with some subsystem disable
// don't disable timer0 use for millis()
// timer0 overflow ISR occur every 256 x 64 clock cyle
// -> for 16 MHz clock every 1,024 ms, this wake up the "uc" from idle sleep
void cpu_idle(void)
{
  sleep_enable();
  set_sleep_mode(SLEEP_MODE_IDLE);
  power_adc_disable();
  power_spi_disable();
  power_timer1_disable();
  power_timer2_disable();
  power_twi_disable();
  sleep_mode(); // go sleep here
  sleep_disable(); 
  power_all_enable();
}

// set cpu in idle mode for ms milliseconds
void delay_idle(unsigned long ms)
{
  unsigned long _now = millis();
  while (millis() - _now < ms)
    cpu_idle();
}

// DEBUG
// see  http://www.nongnu.org/avr-libc/user-manual/malloc.html
void print_RAM_map(void) 
{
  // local var
  char stack = 1;
  // external symbol
  extern char *__data_start;
  extern char *__data_end;
  extern char *__bss_start;
  extern char *__bss_end;
  extern char *__heap_start;
  extern char *__heap_end;
  // sizes compute
  int data_size   = (int)&__data_end - (int)&__data_start;
  int bss_size    = (int)&__bss_end - (int)&__data_end;
  int heap_end    = (int)&stack - (int)&__malloc_margin;
  int heap_size   = heap_end - (int)&__bss_end;
  int stack_size  = RAMEND - (int)&stack + 1;
  int free_memory = (RAMEND - (int)&__data_start + 1) - (data_size + bss_size + heap_size + stack_size);
  // print MAP
  printf_P(PSTR("RAM map\r\n"));
  printf_P(PSTR("-------\n\r\n"));  
  printf_P(PSTR("+----------------+  __data_start  = %d\r\n"), (int)&__data_start);
  printf_P(PSTR("+      data      +\r\n"));
  printf_P(PSTR("+    variables   +  data_size     = %d\r\n"), data_size);
  printf_P(PSTR("+   (with init)  +\r\n"));
  printf_P(PSTR("+----------------+  __data_end    = %d\r\n"), (int)&__data_end);
  printf_P(PSTR("+----------------+  __bss_start   = %d\r\n"), (int)&__bss_start);
  printf_P(PSTR("+       bss      +\r\n"));
  printf_P(PSTR("+    variables   +  bss_size      = %d\r\n"), bss_size);
  printf_P(PSTR("+    (no init)   +\r\n"));
  printf_P(PSTR("+----------------+  __bss_end     = %d\r\n"), (int)&__bss_end);
  printf_P(PSTR("+----------------+  __heap_start  = %d\r\n"), (int)&__heap_start);
  printf_P(PSTR("+                +\r\n"));
  printf_P(PSTR("+      heap      +  heap_size     = %d\r\n"), heap_size);
  printf_P(PSTR("+    (dyn var)   +\r\n"));
  printf_P(PSTR("+----------------+  heap_end      = %d\r\n"), heap_end);
  printf_P(PSTR("+                +\r\n"));
  printf_P(PSTR("+    free mem    +  free          = %d\r\n"), free_memory);
  printf_P(PSTR("+                +\r\n"));
  printf_P(PSTR("+----------------+  Current STACK = %d\r\n"), (int)&stack);
  printf_P(PSTR("+      stack     +\r\n"));
  printf_P(PSTR("+    (sub arg,   +  stack_size    = %d\r\n"), stack_size);
  printf_P(PSTR("+     loc var)   +\r\n"));
  printf_P(PSTR("+----------------+  RAMEND        = %d\r\n"), RAMEND);  
  printf_P(PSTR("\r\n\r\n"));
}

