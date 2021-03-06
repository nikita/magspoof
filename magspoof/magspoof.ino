/*
 * MagSpoof - "wireless" magnetic stripe/credit card emulator
*
 * by Samy Kamkar
 *
 * http://samy.pl/magspoof/
 *
 * - Allows you to store all of your credit cards and magstripes in one device
 * - Works on traditional magstripe readers wirelessly (no NFC/RFID required)
 * - Can disable Chip-and-PIN
 * - Correctly predicts Amex credit card numbers + expirations from previous card number (code not included)
 * - Supports all three magnetic stripe tracks, and even supports Track 1+2 simultaneously
 * - Easy to build using Arduino or ATtiny
 *
 * Resources:
 * Attiny85 pinout: https://hackster.imgix.net/uploads/image/file/50820/ATtiny45-85.png?auto=compress%2Cformat&w=680&h=510&fit=max
 * Add board manager URL:  
 * 
 * I personally decided to use a Digispark board as it will be much easier to reprogram and test.
 * DRV8833 as the motor controller, and repurposed a wireless chargine receiver's coil. 
 * Testing is done via a square POS.
 * 
 * DISCLAIMER:
 * I personally thought this could be a fun project as a Samsung MST alternative and
 * to consolidate some of my cards together into one small package.
 * No matter what reason that led you here, PLEASE don't harm others for your own 
 * personal gain using this project. 
 * 
 * SETUP INSTRUCTIONS:
 * Install "Digistump AVR Boards" in Board Manager
 * Select Digistump (Default) in boards.
 * Follow samyk's schematic shown here: https://raw.githubusercontent.com/samyk/magspoof/master/magspoof-schematic-dip.png
 * 
 */

#include <avr/sleep.h>
#include <avr/interrupt.h>

#define PIN_A 0
#define PIN_B 1
#define ENABLE_PIN 3 // also green LED
#define SWAP_PIN 4 // unused
#define BUTTON_PIN 2

// Delay, corresponds to 5 kHz at 200 us
// Try lowering to 10-60 us according to patent?
#define CLOCK_US 200

#define PADDING 25 // number of 0's before and after tracks

#define TRACKS 2

// This 2D array holds multiple cards' tracks. It is a constant as it would not be changed during runtime. 
const char* cards[][2] = {
//  Card 1
  {"%B123456781234567^LASTNAME/FIRST^YYMMSSSDDDDDDDDDDDDDDDDDDDDDDDDD?\0", // Track 1
  ";123456781234567=YYMMSSSDDDDDDDDDDDDDD?\0"}, // Track 2
//  Card 2
  {"%B123456781234567^LASTNAME/FIRST^YYMMSSSDDDDDDDDDDDDDDDDDDDDDDDDD?\0", // Track 1
  ";123456781234567=YYMMSSSDDDDDDDDDDDDDD?\0"}
//  Add more cards here.
};

// tracks char array holds the currently selected track. 
char* tracks[2] = {
"%B123456781234567^LASTNAME/FIRST^YYMMSSSDDDDDDDDDDDDDDDDDDDDDDDDD?\0", // Track 1
";123456781234567=YYMMSSSDDDDDDDDDDDDDD?\0" // Track 2
};


char revTrack1[80];
char revTrack2[41];

char* revTracks[] = { revTrack1, revTrack2 };

// service code to make requirements more lax and disable chip-and-PIN (third digit)
const char *sc_rep = {"101"};

/* Track 1 is written in DEC SIXBIT plus odd parity (an odd number of 1's in each character)
For our purposes, we can convert from ASCII to this code by subtracting 32
See: https://en.wikipedia.org/wiki/Six-bit_character_code#DEC_six-bit_code

 Track 2 is written with 4 data bits + 1 parity bit
These simply map to the ASCII range 0x30 to 0x3f (in decimal: 48 to 63)
*/
const int sublen[] = {
  32, 48, 48 };
  

// How many bits are in each character for each track
const int bitlen[] = {
  7, 5, 5 };

unsigned int curTrack = 0;
int dir;

void setup()
{
  pinMode(PIN_A, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(9600);
  // blink to show we started up
  blink(ENABLE_PIN, 200, 3);

  // switch current card to index 0.
  switchCard(0);
  storeRevTrack(1);
  storeRevTrack(2);
}

// Blink the LED
void blink(int pin, int msdelay, int times)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(pin, HIGH);
    delay(msdelay);
    digitalWrite(pin, LOW);
    delay(msdelay);
  }
}

// send a single bit out
void playBit(int sendBit)
{
	// Need to make the output pins on the driver either high or low
  dir ^= 1;
  digitalWrite(PIN_A, dir);
  digitalWrite(PIN_B, !dir);
  delayMicroseconds(CLOCK_US);

  if (sendBit)
  {
    dir ^= 1;
    digitalWrite(PIN_A, dir);
    digitalWrite(PIN_B, !dir);
  }
  delayMicroseconds(CLOCK_US);

}

//plays tracks in different ways for maximum compatibility among magstripe readers
void playTracks()
{
  curTrack++;
  if((curTrack % 4)==1)
  {
    //forward then reverse swipe
    playTrack(1);
    reverseTrack(2);
  } else if ((curTrack % 4)==2){
    //forward consecutive swipes
    playTrack(1);
    playTrack(2);
  } else if ((curTrack % 4)==3) {
    //reverse then forward swipe
    reverseTrack(1);
    playTrack(2);
  } else {
    //reverse consecutive swipes
    reverseTrack(1);
    reverseTrack(2);
  }
}

// plays out a full track, calculating CRCs and LRC
void playTrack(int track)
{
  int tmp, crc, lrc, cnt = 0;
  track--; // index 0
  dir = 0;

  // enable H-bridge and LED
  digitalWrite(ENABLE_PIN, HIGH);

  // First put out a bunch of leading zeros.
  for (int i = 0; i < PADDING; i++)
    playBit(0);

  // Play until we get to the end character
  for (int i = 0; tracks[track][i] != '\0'; i++)
  {
    crc = 1;
    
		if(cnt<0)
			cnt--;
		else { // look for FS
			if(tracks[track][i]=='^')
				cnt++;
			if(tracks[track][i]=='=')
				cnt+=2;
				
			if(cnt==2)
				cnt=-1;
		}
		if (cnt<-5 && cnt>-9) // SS is located 4 chars after the last FS
			tmp = sc_rep[8+cnt] - sublen[track];
		else
			tmp = tracks[track][i] - sublen[track];

    for (int j = 0; j < bitlen[track]-1; j++)
    {
      crc ^= tmp & 1;
      lrc ^= (tmp & 1) << j;
      playBit(tmp & 1);
      tmp >>= 1;
    }
    playBit(crc);
  }

  // finish calculating and send last "byte" (LRC)
  tmp = lrc;
  crc = 1;
  for (int j = 0; j < bitlen[track]-1; j++)
  {
    crc ^= tmp & 1;
    playBit(tmp & 1);
    tmp >>= 1;
  }
  playBit(crc);

  // finish with 0's
  for (int i = 0; i < PADDING; i++)
    playBit(0);

  digitalWrite(PIN_A, LOW);
  digitalWrite(PIN_B, LOW);
  digitalWrite(ENABLE_PIN, LOW);
}

// when reversing
void reverseTrack(int track)
{
  int i = 0;
  track--; // index 0
  dir = 0;

  digitalWrite(ENABLE_PIN, HIGH);

  // First put out a bunch of leading zeros.
  for (int i = 0; i < PADDING; i++)
    playBit(0);
  
  while (revTracks[track][i++] != '\0');
  i--;
  while (i--)
    for (int j = bitlen[track]-1; j >= 0; j--)
      playBit((revTracks[track][i] >> j) & 1);
    
  // finish with 0's
  for (int i = 0; i < PADDING; i++)
    playBit(0);

  digitalWrite(PIN_A, LOW);
  digitalWrite(PIN_B, LOW);
  digitalWrite(ENABLE_PIN, LOW);
}

// stores track for reverse usage later
void storeRevTrack(int track)
{
  int i, tmp, crc, lrc, cnt = 0;
  track--; // index 0
  dir = 0;

  for (i = 0; tracks[track][i] != '\0'; i++)
  {
    crc = 1;
    
		if(cnt<0)
			cnt--;
		else { // look for FS
			if(tracks[track][i]=='^')
				cnt++;
			if(tracks[track][i]=='=')
				cnt+=2;
				
			if(cnt==2)
				cnt=-1;
		}
		if (cnt<-5 && cnt>-9) // SS is located 4 chars after the last FS
			tmp = sc_rep[8+cnt] - sublen[track];
		else
			tmp = tracks[track][i] - sublen[track];

    for (int j = 0; j < bitlen[track]-1; j++)
    {
      crc ^= tmp & 1;
      lrc ^= (tmp & 1) << j;
      tmp & 1 ?
        (revTracks[track][i] |= 1 << j) :
        (revTracks[track][i] &= ~(1 << j));
      tmp >>= 1;
    }
    crc ?
      (revTracks[track][i] |= 1 << 4) :
      (revTracks[track][i] &= ~(1 << 4));
  }

  // finish calculating and send last "byte" (LRC)
  tmp = lrc;
  crc = 1;
  for (int j = 0; j < bitlen[track]-1; j++)
  {
    crc ^= tmp & 1;
    tmp & 1 ?
      (revTracks[track][i] |= 1 << j) :
      (revTracks[track][i] &= ~(1 << j));
    tmp >>= 1;
  }
  crc ?
    (revTracks[track][i] |= 1 << 4) :
    (revTracks[track][i] &= ~(1 << 4));

  i++;
  revTracks[track][i] = '\0';
}

void switchCard(int cardNum) {
  memcpy(tracks, cards[cardNum], sizeof(cards[cardNum]));
  // copy cards[cardNum] -> tracks.   

  Serial.println();
  Serial.print("Current selected card: ");
  Serial.println(tracks[0]);
}
void sleep()
{
  GIMSK |= _BV(PCIE);                     // Enable Pin Change Interrupts
  PCMSK |= _BV(PCINT2);                   // Use PB3 as interrupt pin
  ADCSRA &= ~_BV(ADEN);                   // ADC off
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);    // replaces above statement

  MCUCR &= ~_BV(ISC01);
  MCUCR &= ~_BV(ISC00);       // Interrupt on rising edge
  sleep_enable();                         // Sets the Sleep Enable bit in the MCUCR Register (SE BIT)
  sei();                                  // Enable interrupts
  sleep_cpu();                            // sleep

  cli();                                  // Disable interrupts
  PCMSK &= ~_BV(PCINT2);                  // Turn off PB3 as interrupt pin
  sleep_disable();                        // Clear SE bit
  ADCSRA |= _BV(ADEN);                    // ADC on

  sei();                                  // Enable interrupts
}

ISR(PCINT0_vect) {
}

void loop()
{
  sleep();

  noInterrupts();
  while (digitalRead(BUTTON_PIN) == LOW);

  delay(100);
  while (digitalRead(BUTTON_PIN) == LOW);
  playTracks();
  delay(400);

  interrupts();
}
