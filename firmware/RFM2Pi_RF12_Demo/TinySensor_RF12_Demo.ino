// RF12Demo for Attiny84_RFM12b
// Configure some values in EEPROM for easy config of the RF12 later on.
// 2009-05-06 <jc@wippler.nl> http://opensource.org/licenses/mit-license.php

#include <SoftwareSerial.h>
#define rxPin 7 //PA3
#define txPin 3 //PA7

SoftwareSerial mySerial(rxPin, txPin);

/*
                     +-\/-+
               VCC  1|    |14  GND
          (D0) PB0  2|    |13  AREF (D10)
          (D1) PB1  3|    |12  PA1 (D9)
             RESET  4|    |11  PA2 (D8)
INT0  PWM (D2) PB2  5|    |10  PA3 (D7)
      PWM (D3) PA7  6|    |9   PA4 (D6)
      PWM (D4) PA6  7|    |8   PA5 (D5) PWM
                     +----+
*/


#include <JeeLib.h>
#include <util/crc16.h>
#include <util/parity.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>

#define LED_PIN     8   

#define COLLECT 0x20 // collect mode, i.e. pass incoming without sending acks

static unsigned long now () {
    // FIXME 49-day overflow
    return millis() / 1000;
}

static void activityLed (byte on) {

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, on);

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// RF12 configuration setup code

typedef struct {
    byte nodeId;
    byte group;
//    char msg[30-4];
    word crc;
} RF12Config;

static RF12Config config;

static char cmd;
static byte value, stack[20], top, sendLen, dest, quiet;
static byte testbuf[20], testCounter;


static void saveConfig () {
    // save to EEPROM

    eeprom_write_byte(RF12_EEPROM_ADDR ,config.nodeId);
    eeprom_write_byte(RF12_EEPROM_ADDR +1 ,config.group);

    config.crc = ~0;
    config.crc = _crc16_update(config.crc, config.nodeId);     
    config.crc = _crc16_update(config.crc, config.group);        
        
    for (int i=2; i < RF12_EEPROM_SIZE-2; i++) {
        eeprom_write_byte(RF12_EEPROM_ADDR + i, 0);
        config.crc = _crc16_update(config.crc, 0);        
    }

    eeprom_write_byte(RF12_EEPROM_ADDR + RF12_EEPROM_SIZE-2 ,config.crc);
    eeprom_write_byte(RF12_EEPROM_ADDR + RF12_EEPROM_SIZE-1 ,config.crc>>8);
    
    if (!rf12_config())
        mySerial.println("config save failed");
}


char helpText1[] PROGMEM = 
    "\n"
    "Available commands:" "\n"
    "  <nn> i     - set node ID (standard node ids are 1..26)" "\n"
    "               (or enter an uppercase 'A'..'Z' to set id)" "\n"
    "  <n> b      - set MHz band (4 = 433, 8 = 868, 9 = 915)" "\n"
    "  <nnn> g    - set network group (RFM12 only allows 212, 0 = any)" "\n"
    "  <n> c      - set collect mode (advanced, normally 0)" "\n"
    "  ...,<nn> a - send data packet to node <nn>, with ack" "\n"
    "  ...,<nn> s - send data packet to node <nn>, no ack" "\n"
    "  <n> l      - turn activity LED on DIG8 on or off" "\n"
    "  <n> q      - set quiet mode (1 = don't report bad packets)" "\n"
;

static void showString (PGM_P s) {
    for (;;) {
        char c = pgm_read_byte(s++);
        if (c == 0)
            break;
        if (c == '\n')
            mySerial.print('\r');
        mySerial.print(c);
    }
}

static void showHelp () {
    showString(helpText1);
    mySerial.println("Current configuration:");
    config.nodeId = eeprom_read_byte(RF12_EEPROM_ADDR);
    config.group = eeprom_read_byte(RF12_EEPROM_ADDR + 1);

/*
    mySerial.println("EEPROM config: ");        
    uint16_t crc = ~0;
    for (uint8_t i = 0; i < RF12_EEPROM_SIZE; ++i){
        crc = _crc16_update(crc, eeprom_read_byte(RF12_EEPROM_ADDR + i));
       mySerial.print(eeprom_read_byte(RF12_EEPROM_ADDR + i),HEX);
    }
    mySerial.println("");        
    
    mySerial.print("crc:");        
    mySerial.print(crc);        
    mySerial.print(" ");            
 */
 
    byte id = config.nodeId & 0x1F;
    mySerial.print('@' + id,DEC);
    mySerial.print(" i");
    mySerial.print( id,DEC);
    if (config.nodeId & COLLECT)
         mySerial.print('*');
    
    mySerial.print(" g");
    mySerial.print(config.group,DEC);
    
    mySerial.print(" @ ");
    static word bands[4] = { 315, 433, 868, 915 };
    word band = config.nodeId >> 6;
    mySerial.print(bands[band],DEC);
    mySerial.println( " MHz ");


    rf12_config();
}

static void handleInput (char c) {
    if ('0' <= c && c <= '9')
        value = 10 * value + c - '0';
    else if (c == ',') {
        if (top < sizeof stack)
            stack[top++] = value;
        value = 0;
    } else if ('a' <= c && c <='z') {
        mySerial.print("> ");
        mySerial.print((int) value);
        mySerial.println(c);
        switch (c) {
            default:
                showHelp();
                break;
            case 'i': // set node id
                config.nodeId = (config.nodeId & 0xE0) + (value & 0x1F);
                saveConfig();
                break;
            case 'b': // set band: 4 = 433, 8 = 868, 9 = 915
                value = value == 8 ? RF12_868MHZ :
                        value == 9 ? RF12_915MHZ : RF12_433MHZ;
                config.nodeId = (value << 6) + (config.nodeId & 0x3F);
                saveConfig();
                break;
            case 'g': // set network group
                config.group = value;
                saveConfig();
                break;
            case 'c': // set collect mode (off = 0, on = 1)
                if (value)
                    config.nodeId |= COLLECT;
                else
                    config.nodeId &= ~COLLECT;
                saveConfig();
                break;
            case 'a': // send packet to node ID N, request an ack
            case 's': // send packet to node ID N, no ack
                cmd = c;
                sendLen = top;
                dest = value;
                memcpy(testbuf, stack, top);
                break;
            case 'l': // turn activity LED on or off
                activityLed(value);
                break;
            case 'q': // turn quiet mode on or off (don't report bad packets)
                quiet = value;
                break;
        }
        value = top = 0;
        memset(stack, 0, sizeof stack);
    } else if ('A' <= c && c <= 'Z') {
        config.nodeId = (config.nodeId & 0xE0) + (c & 0x1F);
        saveConfig();
    } else if (c > ' ')
        showHelp();
}

void setup() {
    
    activityLed(1);
    pinMode(rxPin, INPUT);
    pinMode(txPin, OUTPUT);
    
    // set the data rate for the NewSoftmymySerial port
    mySerial.begin(9600);
    
    mySerial.print("\n[RF12demo.Attiny84]\n\r");

    delay(2000);

    if (rf12_config()) {
        config.nodeId = eeprom_read_byte(RF12_EEPROM_ADDR);
        config.group = eeprom_read_byte(RF12_EEPROM_ADDR + 1);
    } else {
        config.nodeId = 0x81; // node A1 @ 868 MHz
        config.group = 0xD2;  //210
        rf12_initialize(config.nodeId&0x1F, config.nodeId >> 6 ,config.group);        
        saveConfig();
    }
   
    showHelp();
    activityLed(0);
}

void loop() {
    if (mySerial.available())
        handleInput(mySerial.read());

    if (rf12_recvDone() & (rf12_crc == 0) ) {
        activityLed(1);      
        byte n = rf12_len;

        if (config.group == 0) {
            mySerial.print("G ");
            mySerial.print((int) rf12_grp);
        }
        mySerial.print(' ');
        mySerial.print((int) rf12_hdr);
        for (byte i = 0; i < n; ++i) {
            mySerial.print(' ');
            mySerial.print((int) rf12_data[i]);
        }
        mySerial.println();
        
        activityLed(0);
           
        if (rf12_crc == 0) {
            activityLed(1);
            
            if (RF12_WANTS_ACK && (config.nodeId & COLLECT) == 0) {
                mySerial.println(" -> ack");
                rf12_sendStart(RF12_ACK_REPLY, 0, 0);
            }
            
            activityLed(0);
        }
    }

    if (cmd && rf12_canSend()) {
        activityLed(1);

        mySerial.print(" -> ");
        mySerial.print((int) sendLen);
        mySerial.println(" b");
        byte header = cmd == 'a' ? RF12_HDR_ACK : 0;
        if (dest)
            header |= RF12_HDR_DST | dest;
        rf12_sendStart(header, testbuf, sendLen);
        cmd = 0;

        activityLed(0);
    }
}
