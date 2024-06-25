/*  Cheapolino V1.0 18.06.2024 - Nikolai Radke

    Sketch for budget Nokolino with JQ8400 module
    For ATtiny45/85 - set to 8 Mhz | B.O.D disabled | No bootloader.
    Remember to flash the "bootloader" first!
    Only for Spence Konde's ATTinyCore - it provides hardware serial emulation.
    Software serial is not working with pin change interrupt!

    Flash usage: 3.540 (IDE 2.3.0 | AVR 1.8.6 | ATTinyCore 1.5.2 | Linux X86_64 | ATtiny85)
    Power:       21mA (idle) | 3.8mA (light sleep) | 2μA (deep sleep)


    1: RST | PB5  Free
    2: D3  | PB3  Power  - MOSFET Gate
    3: A2  | PB4  Busy   - 2 JQ8400 
    4: GND |      GND
    5: D0  | PB0  RX     - 3 JQ8400 
    6: D1  | PB1  TX     - 4 JQ8400
    7: D2  | PB2  Button - GND
    8: VCC        VCC

  To do: Portmanipulation, Code optimieren
*/

#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <EEPROM.h>

//-------------------------------------------------------------------------------------------------
// User Configuation
#define Time             10                      // Say something every statistical 10 minutes
#define Volume           15                      // Volume 0-30 - 25 is recommended 

// Optional - comment out with // to disable o remove // to enable
#define StartupBeep                              // Will say "beep" when turned on
#define BatteryWarning                           // Gives a warning when battery is low
#define CountFiles                               // Count files for music box mode - buggy

//-------------------------------------------------------------------------------------------------
// Internal configuration
#define Button_event     40                      // Last button event number (XX.mp3)
#define Time_event       163                     // Last time event number -> Very last file is "beep"

#define Offset           0.1                     // Battery measuring error
#define minInput         500                     // Mininal value from busy line 

#define Cycles           443                     // Loop cycles per minute
#define DeepSleep        120                     // Minutes to go to deep sleep - 2 hours default
#define LightSleep       5                       // Minutes to go to light sleep - 5 minutes default

// Optional battery warning
#define minCurrent       3.30 + Offset           // Low power warning current + measuring error
#define battLow          3.10 + Offset           // Minimal voltage before JQ8400 fails

// Hardware pins
#define Busy             A2                      // Pin 3
#define JQ8400           PB3                     // Pin 4
#define Button           PB2                     // Pin 6

#define Button_pressed   !(PINB & (1 << Button))

// Variables
uint16_t seed;                                   // Random seed and helping variable
uint16_t files         = 164;                    // Files on module
uint16_t address       = 1;                      // EEPROM address and later file number for music box mode
uint16_t Dsleeptime    = 1;                      // Set deep sleep time to 1 minute for music box mode   
uint16_t Lsleeptime    = 1;                      // And light sleep time too
uint32_t counterSleep  = 0;                      // Cycles to sleep modes
bool     low           = false;                  // Battery to low for JQ8400
bool     musicbox      = true;                   // Music box mode
bool     lightsleep    = false;                  // Light sleep mode ater 5 minutes
bool     deepsleep     = false;                  // Deep sleep mode after defined minutes -> EXTERN
bool     wakeup        = false;                  // Talk after deep sleep
char     count_files;                            // Helping variable for conversion
uint8_t  files_byte[6];                          // Result array for file counting

#ifdef BatteryWarning                  
  uint16_t current;                              // Mesured current
  double   vref;                                 // Helping variable
  uint16_t counterBattery = 10;                  // Check shortly after startup
#endif

int main(void) {

  init(); {
    // Power saving
    ACSR = (1 << ACD);                           // Disable analog comparator
    set_watchdog();                              // Set sleep time to 128ms
    
    // Port Setup
    DDRB &= ~(1 << Button);                      // PB2 button INPUT
    PORTB |= (1 << Button);                      // PB2 INPUT_PULLUP 
    DDRB  |= (1 << JQ8400);                      // PB3 JQ8400 MOSFET switch OUTPUT
    
    // Start JQ8400 and count files
    JQ8400_init();
    _delay_ms(100);

    #ifdef CountFiles                            // Count files for music box mode
      Serial.write("\xAA\x0C");                  // Count files on module
      Serial.write((uint8_t) 0x00);              // 0x00 must be send as byte
      Serial.write(0xB6);
      _delay_ms(100);                            // Wait for answer from JQ8400
      for (seed = 0; seed < 6; seed++)           // Read 6 HEX chars from module
        files_byte[seed] = (uint8_t) Serial.read(); // and convert the chars into uint8_t
        _delay_ms(1);
      files = 16 * files_byte[3] + files_byte[4]; // Convert 2 bytes into uint16_t
    #endif

    // Cheaplino mode | else Music box mode
    if (files == Time_event + 1) {
      Dsleeptime = DeepSleep;                    // Set deep sleep time to predefined value
      Lsleeptime = LightSleep;                   // Set light sleep time too
      musicbox = false;                          // Set Cheapolino mode

      // Randomize number generator
      address = eeprom_read_word(0);             // Read EEPROM address
      if ((address < 2) || (address > (EEPROM.length() - 3))) {
        // Initialize EEPROM and size for first use or after end of cycle
        address = 2;                             // Starting address
        eeprom_write_word(0, address);           // Write starting address
        eeprom_write_word(address, 0);           // Write seed 0
      }
      seed = eeprom_read_word(address);          // Read seed
      if (seed > 900) {                          // After 900 write-cyles move to another address
        seed = 0;                                // to keep the EEPROM alive
        address += 2;                            // 2 places, adress is a word
        eeprom_write_word(0, address);           // Write address of seed
      }
      seed ++;                                   // New seed
      eeprom_write_word(address, seed);          // Save new seed for next startup
      randomSeed(seed);                          // Randomize number generator

      // Optional startup beep
      #ifdef StartupBeep
        JQ8400_play(Time_event + 1);             // Cheaplino says "beep"
      #endif
    }
  }

  // Main loop
  while (1) {
    // Check if button is pressed
    if (!low) {                                  // Quiet if battery too low    
      if ((Button_pressed) || (wakeup)) {        // Button pressed? And talk after deep sleep
        
        // Monster mode
        if (!musicbox) {                        
          counterSleep = 0;                      // Reset counter
          wakeup = false;                        // Reset wakeup flag
          seed = 2000;                           // Cycles needed to count - 2000 = 3 sec
          while ((Button_pressed) && (seed > 0 )) { // Button still pressed?
            seed --;                             // Count down
            _delay_ms(1);                        // and wait 1ms
          }
          if (seed == 0) {                       // If full count, shut down
            counterSleep = Dsleeptime * Cycles;  // Set timer to deep sleep
            JQ8400_play(Time_event);             // Say "let me sleep"
            while (Button_pressed);              // Wait until button is released
            _delay_ms(1000);                     // Wait some extra time
          }
          else JQ8400_play(random(0, Button_event + 1)); // Play button event file
          lightsleep = false;                    // Reset light sleep
        }
        
        // Music box mode
        else {
          JQ8400_play(address);                  //  Play single music box files
          (address == files) ? address = 1 : address++; // Set to next file
        }
      }

      // Check for time event
      if ((!musicbox) && (random(0, Time * Cycles) == 1)) // Time event
        JQ8400_play(random(Button_event + 1, Time_event)); // No "beep" and "let me sleep"
    
      // Check sleep state
      if (counterSleep >= Lsleeptime * Cycles) { // Check timer for light sleep
        Serial.end();                            // Close serial connection
        digitalWrite(JQ8400, LOW);               // Shut down JQ8400
        lightsleep=true;                         // Set light sleep
        if ((counterSleep >= Dsleeptime * Cycles) || (low)) // Check battery and timer for deep sleep
          deepsleep=true;                        // Set deep sleep
      }      
    }
    
    // Optional: Check battery current
    #ifdef BatteryWarning
      if (counterBattery == 0) {                 // Battery check countdown zero reached?
        low = false;                             // Reset warning flag
        current = measure_vcc();                 // Read current
        vref = 1024 * 1.1f / (double)current;    // Calculate current from reading
        if (vref <= minCurrent) {                // Current below minimum
          if (vref <= battLow) low = true;       // Power too low for J8400
          else JQ8400_play(Time_event + 1);      // Cheaplino says "beep"
        }
        counterBattery = Cycles;                 // Every minute, 433x128ms+some runtime ms for 60s
      }
      counterBattery --;
    #endif

    // Power saving ans sleep mode
    attiny_sleep();                              // Save power or go to light/deep sleep 
  }
}  

// Functions
void JQ8400_init() {
  digitalWrite(JQ8400, HIGH);                    // Power up JQ8400 - direct port manipulation doesn't work!
  _delay_ms(1000);                               // JQ8400 needs a short time to settle
  Serial.begin(9600);                            // Start serial connection to JQ8400
  Serial.write("\xAA\x04");                      // Reset JQ8400
  Serial.write((uint8_t) 0x00);                  // Needed to reset strange default settings,
  Serial.write(0xAE);                            // maybe from outer space
  _delay_ms(250);                                // Wait for JQ8400
  Serial.write("\xAA\x1A\x01\x02\xC7");          // Set equalizer to rock mode for better voice sound
  _delay_ms(100);                                // Time to settle, JQ8400 ALWAYS needs time
  Serial.write("\xAA\x13\x01");                  // Set volume
  _delay_ms(10);
  Serial.write(Volume);                          // Volume 0-30
  _delay_ms(10);
  Serial.write((uint8_t) 190 + Volume);          // Calculate and write checksum
  _delay_ms(300);                                // Wait some more extra time
}

void JQ8400_play(uint8_t f) {                    // Plays MP3 file
  if (lightsleep) JQ8400_init();                 // Restart JQ8400 if down
  Serial.write("\xAA\x07\x02");                  // Play file by number
  Serial.write((uint8_t) 0x00);                  // Again, 0x00 must be send as byte
  Serial.write(f);                               // File numer f
  Serial.write((uint8_t) 179 + f);               // Calculate und write checksum
  _delay_ms(500);                                // Wait for busy line to settle
  while (analogRead(Busy) > minInput)            // Check busy
    attiny_sleep();                              // and sleep to save power
  _delay_ms(100);                                // Busy causes strange behavior without pause
}

void attiny_sleep() {                            // Sleep to save power
  uint8_t adcsra = ADCSRA;                       // Save ADC control and status register A
  if (deepsleep) {                               // Deep sleep mode
    cli();                                       // Stop all interrupts
    WDTCR = 0x00;                                // Disable watchdog interrupt
    GIMSK |= (1 << PCIE);                        // Turns on pin change interrupts
    PCMSK |= (1 << PCINT2);                      // Turn on interrupts on PB2 button
    sei();                                       // Start interrupts
  }
  ADCSRA &= ~(1 << ADEN);                        // Switch ADC off
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);           // Deepest sleep mode
  sleep_mode();                                  // Good night
  
  // Deed sleep time
  _delay_ms(100);                                // Time needed? YES!
  if (deepsleep) {                               // Things to do after deep sleep
    deepsleep = false;                           // Reset deep sleep flag
    wakeup = true;                               // Set wake up flag to talk
    cli();                                       // Stop all interrupts
    GIMSK &= ~ (1 << PCIE);                      // Disable button interrupt
    set_watchdog();                              // Restart watchdog
    sei();                                       // Start interrupts
  }
  else counterSleep ++;                          // Countdown to sleep mode
  ADCSRA = adcsra;                               // Restore ADCSRA register and start ADC
}

void set_watchdog() {                            // Set watchdog to 128ms
  MCUSR &= ~(1 << WDRF);                         // No reset
  WDTCR |= (1 << WDCE) | (1 << WDE);             // Watchdog change enable              
  WDTCR = (1 << WDIE) | (1 << WDP1) | (1 << WDP0); // Set interrupr mode and prescaler to 128ms
}

uint16_t measure_vcc(void) {                     // Thank you, Tim!
  PRR &= ~(1 << PRADC);                          // ADC power on
  ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0); // Enable ADC, set prescaler to 128
  ADMUX = (1 << REFS2) | 0x0c;                   // VCC as reference, bandgap reference as ADC input  
  _delay_ms(1);                                  // Settle
  ADCSRA |= (1 << ADSC);                         // Start conversion
  while (!(ADCSRA & (1 << ADIF)));               // ~100 us
  ADCSRA |= (1 << ADIF);                         // Clear ADIF
  return ADC;
}

ISR(WDT_vect) {}                                 // Watchdog interrupt routine. Unused but mandatory
ISR(PCINT0_vect) {}                              // Pin interrupt routine. Unused but mandatory too