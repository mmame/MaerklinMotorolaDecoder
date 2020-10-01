//Download the MaerklinMotorola Library from https://github.com/mmame/MaerklinMotorola
//Add additional board manager url: https://mcudude.github.io/MiniCore/package_MCUdude_MiniCore_index.json
#include <MaerklinMotorola.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#if not defined (__AVR_ATtiny85__)
  #include "SoftwareSerial.h"
#endif
#include "DFPlayerMini_Fast.h"

/* Default settings when no valid EEPROMData is found */
#define DEFAULT_MODE   MODE_FUNCTION_DECODER
//#define DEFAULT_MODE   MODE_SIGNAL
//#define DEFAULT_MODE   MODE_SOUND_MODULE
#define DEFAULT_ADDRESS 78                  //1...80  MM2 base address where the function/sound decoder is listening on (only applicable for MODE_FUNCTION_DECODER and MODE_SOUND_MODULE)
#define DEFAULT_SWITCH_NUMBER 1             //1...320 MM2 switch number where the switch module is listening on (only applicable for MODE_SWITCH and MODE_SIGNAL)
#define DEFAULT_VOLUME 20                   //0...30  Volume of the played sounds (only applicable for MODE_SOUND_MODULE)
#define DEFAULT_EXTENDED_ADDRESS_COUNT 1    //0...1   Number of MM2 addresses where the function/sound decoder is listening to i.e. Base address is 78 and DEFAULT_EXTENDED_ADDRESS_COUNT is 2, so we're listening on (base) Address 78 plus 79 and 80 (only applicable for MODE_SOUND_MODULE and MODE_FUNCTION_DECODER)

//Sorry, no room for debugging on ATTiny...
#if not defined (__AVR_ATtiny85__)
  //comment out the following line to disable serial traces
  #define SERIAL_TRACE
#endif

//#define TRACE_ALL_MSG
  
#ifdef SERIAL_TRACE
#define BAUDRATE            115200
#define  TRACELN(x)         Serial.println(x)
#define  TRACE(x)           Serial.print(x)
#define  TRACELNF(x, y)     Serial.println(x, y)
#define  TRACEF(x, y)       Serial.print(x, y)
#define  TRACEHEX(x)        if(0x10 > x)Serial.print("0");Serial.print(x, HEX)
#else
#define TRACELN(x)
#define TRACE(x)
#define TRACELNF(x, y)
#define TRACEF(x, y)
#define  TRACEHEX(x)
#endif

#if defined (__AVR_ATtiny85__)
  //#define REDUCE_PROGMEM 1
  #define INVERTED_LOGIC 1
  #define MM_INPUT_PIN  2
  #define OUTPUT_1_PIN  1
  #define OUTPUT_2_PIN  0
  #define BUTTON_FCT    4
  #define DFPLAYER_RX   3
  #define DFPLAYER_TX   5
#else
  #define INVERTED_LOGIC 0
  #define BUTTON_FCT    4
  #define BUTTON_FCT_INVERTED 1
  #define MM_INPUT_PIN  2
  #define OUTPUT_1_PIN  3
  #define OUTPUT_2_PIN  5
  #define OUTPUT_3_PIN  6
  #define OUTPUT_4_PIN  9
  #define OUTPUT_5_PIN  10
  #define OUTPUT_6_PIN  11
  #define DFPLAYER_RX   A0 
  #define DFPLAYER_TX   A1
#endif

  
#define EEPROM_MAGIC "\x59\x41\x4D\x01"

typedef enum _MODE{
  MODE_SWITCH = 1,
  MODE_FUNCTION_DECODER,
  MODE_SIGNAL,
  MODE_SOUND_MODULE
} MODE;

typedef enum _CVWRITESTATE
{
  CVWRITESTATE_IDLE1,
  CVWRITESTATE_IDLE2,
  CVWRITESTATE_IDLE3,
  CVWRITESTATE_ADDRESS1,
  CVWRITESTATE_ADDRESS2,
  CVWRITESTATE_ADDRESS3,
  CVWRITESTATE_VALUE1,
  CVWRITESTATE_VALUE2,
  CVWRITESTATE_VALUE3,
} CVWRITESTATE;

//Common CV indexes
#define CV_ADDRESS      0
#define CV_MODE         1
#define CV_VOLUME       2
#define CV_EXTENDED_ADDRESS_COUNT 3

#if defined (__AVR_ATtiny85__)
  #define CV_COUNT        4
#else
  #define CV_COUNT        80
#endif


typedef enum MODE_SIGNAL_STATE{
  MODE_SIGNAL_STATE_1,
  MODE_SIGNAL_STATE_2,
  MODE_SIGNAL_STATE_UNKNOWN,
};

/* EEPROM defines */
typedef struct _EEPROM_DATA {
  byte Magic[4];
  unsigned int SwitchNumber;
  unsigned char CV[CV_COUNT];
  bool Reverse:1;
  MODE_SIGNAL_STATE LastSignalState;
} EEPROM_DATA, *PEEPROM_DATA;

EEPROM_DATA EEPROMData;

volatile MaerklinMotorola mm(MM_INPUT_PIN);

bool IsAddressLearningMode = false;
unsigned char currentlyPlayingSoundIndex = 0;

typedef enum _LED_MODE{
  LED_MODE_IDLE,
  LED_MODE_OFF,
  LED_MODE_BLINK_INTERVAL,
  LED_MODE_BLINK_NUMBER,
  LED_MODE_ON
} LED_MODE;

typedef enum FADE_STATE{
  FADE_STATE_IDLE,
  FADE_STATE_HIGH_LOW,
  FADE_STATE_LOW_HIGH,
};

MODE_SIGNAL_STATE ModeSignalState = MODE_SIGNAL_STATE_UNKNOWN;
unsigned char FadeStage = 1;

FADE_STATE FadeState = FADE_STATE_IDLE;
unsigned char FadeValueCurrent = 0;
unsigned char FadePin = 0;
unsigned int LastFadeTime = 0;

#define FADE_STEP_SIZE 15
#define FADE_STEP_TIME 20
#define FADE_LOW_VALUE 0
#define FADE_HIGH_VALUE 1023

unsigned int LastLedSwitchTime = millis();
int LedBlinkIntervalMillis = 500;
unsigned char LedNumberOfBlinks = 0;
LED_MODE LedMode = LED_MODE_IDLE;

unsigned int SwitchCoilOnTime = millis();

bool LedBlinkState = false;
bool  SwitchedOn = false;

#define MAX_COIL_TIME 250

CVWRITESTATE CVWriteState = CVWRITESTATE_IDLE1;
unsigned char CVWriteAddress = 0;
unsigned char CVWriteValue = 0;

/* On ATTIny85, we use the builtin Serial port to have more program space */
#if defined (__AVR_ATtiny85__)
  #define DFPLAYER_SERIAL Serial
#else
  SoftwareSerial dfPlayerSoftwareSerial(DFPLAYER_RX, DFPLAYER_TX);
  #define DFPLAYER_SERIAL dfPlayerSoftwareSerial
#endif
DFPlayerMini_Fast dfPlayer;

unsigned int getSwitchNumber(MaerklinMotorolaData* md)
{
  unsigned int SwitchNumber = (md->SubAddress & ~0x01)/2 + (md->Address - 1) * 4 + 1;
  if(0 == md->Address)
  {
    SwitchNumber += 320;
  }

  return SwitchNumber;
}

//Sorry, not enough flash on ATTiny, so we don't support fancy LED blinky stuff
void setLEDMode(LED_MODE newMode, int intervalMillis, unsigned char numberOfBlinks)
{
  #if not defined (REDUCE_PROGMEM)
  if(newMode != LedMode)
  LedMode = newMode;
  LastLedSwitchTime = millis();
  switch(LedMode)
  {
    case LED_MODE_OFF:
    digitalWrite(LED_BUILTIN, 0);
    break;

    case LED_MODE_ON:
    digitalWrite(LED_BUILTIN, 1);
    break;

    case LED_MODE_BLINK_INTERVAL:
    case LED_MODE_BLINK_NUMBER:
    digitalWrite(LED_BUILTIN, 0);
    LedBlinkIntervalMillis = intervalMillis;
    LedNumberOfBlinks = numberOfBlinks;
    LedBlinkState = false;
    break;

    default:
    break;
  }
  #endif
}

void setupFadeHighLow()
{
  //TRACELN(F("setupFadeHighLow"));
  FadeState = FADE_STATE_HIGH_LOW;
  FadeValueCurrent = FADE_HIGH_VALUE;
}


void setupFadeLowHigh()
{
  //TRACELN(F("setupFadeLowHigh"));
  FadeState = FADE_STATE_LOW_HIGH;
  FadeValueCurrent = FADE_LOW_VALUE;
}

void processFade()
{
  if(MODE_SIGNAL == EEPROMData.CV[CV_MODE])
  {
    switch(FadeState)
    {
      case FADE_STATE_IDLE:
      //Check if we need to start stage 1
      if(0 == FadeStage)
      {
        TRACELN(F("FadeStage 1"));
        FadeStage = 1;
        if(INVERTED_LOGIC)
        {
          setupFadeHighLow();
        }
        else
        {
          setupFadeLowHigh();
        }

        switch(ModeSignalState)
        {
          case MODE_SIGNAL_STATE_1:
          //Finish fading from 2 to 1 (Fadein)
          FadePin = OUTPUT_1_PIN;      
          break;

          case MODE_SIGNAL_STATE_2:
          //Finish fading from 1 to 2 (Fadein)
          FadePin = OUTPUT_2_PIN;      
          break;
        }
      }
      break;
      
      case FADE_STATE_HIGH_LOW:
      if (millis() - LastFadeTime >= FADE_STEP_TIME)
      {
        //TRACE(F("FADE_STATE_HIGH_LOW "));
        //TRACELN(FadeValueCurrent);
        LastFadeTime = millis();
        if(FadeValueCurrent > FADE_STEP_SIZE + FADE_LOW_VALUE)
        {
          FadeValueCurrent -= FADE_STEP_SIZE;
          analogWrite(FadePin, FadeValueCurrent);
        }
        else
        {
          analogWrite(FadePin, 0);
          FadeState = FADE_STATE_IDLE;
        }
      }
      break;
      
      case FADE_STATE_LOW_HIGH:
      if (millis() - LastFadeTime >= FADE_STEP_TIME)
      {
        //TRACE(F("FADE_STATE_LOW_HIGH "));
        //TRACELN(FadeValueCurrent);
        LastFadeTime = millis();
        if(FadeValueCurrent < FADE_HIGH_VALUE - FADE_STEP_SIZE)
        {
          FadeValueCurrent += FADE_STEP_SIZE;
          analogWrite(FadePin, FadeValueCurrent);
        }
        else
        {
          analogWrite(FadePin, FADE_HIGH_VALUE);
          FadeState = FADE_STATE_IDLE;
        }
      }
      break;    
    }
  }
}

//Sorry, not enough flash on ATTiny, so we don't support fancy LED blinky stuff
void processLED()
{
  
#if not defined (REDUCE_PROGMEM)
  switch(LedMode)
  {
    case LED_MODE_BLINK_INTERVAL:
    case LED_MODE_BLINK_NUMBER:
      if (millis() - LastLedSwitchTime >= LedBlinkIntervalMillis)
      {
        LedBlinkState = !LedBlinkState;
        digitalWrite(LED_BUILTIN, LedBlinkState);
        LastLedSwitchTime = millis();
        
        if(LED_MODE_BLINK_NUMBER == LedMode)
        {
          if(0 == LedNumberOfBlinks)
          {
            //stop - this happens in LED off mode
            setLEDMode(LED_MODE_OFF, 0, 0);
          }
          else if(LedBlinkState)
          {
            if(LedNumberOfBlinks > 0)
            {
              LedNumberOfBlinks--;
            }
          }
        }
      }
    break;

    default:
    break;
  }
#endif
}

void loadEEPROM()
{
  EEPROM.get(0, EEPROMData);
  if(0 != memcmp(EEPROM_MAGIC, EEPROMData.Magic, 4))
  {
    //No valid EEPROM data found - load setup defaults (Switch mode, use switch number 1)
    TRACELN(F(" Loading Setup Defaults..."));
    memcpy(EEPROMData.Magic, EEPROM_MAGIC, 4);
    EEPROMData.CV[CV_MODE] = DEFAULT_MODE;
    EEPROMData.CV[CV_ADDRESS] = DEFAULT_ADDRESS;
    EEPROMData.CV[CV_VOLUME] = DEFAULT_VOLUME;
    EEPROMData.CV[CV_EXTENDED_ADDRESS_COUNT] = DEFAULT_EXTENDED_ADDRESS_COUNT;
    EEPROMData.SwitchNumber = DEFAULT_SWITCH_NUMBER;
    EEPROMData.Reverse = false;
    EEPROMData.LastSignalState = MODE_SIGNAL_STATE_1;
    saveEEPROM();
  }
}

void saveEEPROM()
{
  EEPROM.put(0, EEPROMData);
}

void setup() {
  pinMode(OUTPUT_1_PIN, OUTPUT);
  pinMode(OUTPUT_2_PIN, OUTPUT);
  //disable all ports
  digitalWrite(OUTPUT_1_PIN, INVERTED_LOGIC);
  digitalWrite(OUTPUT_2_PIN, INVERTED_LOGIC);

#if not defined (__AVR_ATtiny85__)
  pinMode(OUTPUT_3_PIN, OUTPUT);
  pinMode(OUTPUT_4_PIN, OUTPUT);
  pinMode(OUTPUT_5_PIN, OUTPUT);
  pinMode(OUTPUT_6_PIN, OUTPUT);
  digitalWrite(OUTPUT_3_PIN, INVERTED_LOGIC);
  digitalWrite(OUTPUT_4_PIN, INVERTED_LOGIC);
  digitalWrite(OUTPUT_5_PIN, INVERTED_LOGIC);
  digitalWrite(OUTPUT_6_PIN, INVERTED_LOGIC);
#endif

  attachInterrupt(digitalPinToInterrupt(MM_INPUT_PIN), isr, CHANGE);

  pinMode(BUTTON_FCT, INPUT_PULLUP);
  
  pinMode(LED_BUILTIN, OUTPUT);
  setLEDMode(LED_MODE_OFF, 0, 0);

#ifdef SERIAL_TRACE
  delay(1000);
  if (Serial)
  {
    Serial.begin(BAUDRATE);
  }
#endif

 loadEEPROM();

 if(MODE_SOUND_MODULE == EEPROMData.CV[CV_MODE])
 {
    DFPLAYER_SERIAL.begin(9600);
    if (dfPlayer.begin(DFPLAYER_SERIAL))
    {
      TRACELN(F("DFPlayer online"));
      dfPlayer.volume(EEPROMData.CV[CV_VOLUME]);
    }
    else
    {
      //sorry, no sounds available
      TRACELN(F("Unable to begin:"));
      TRACELN(F("1.Please recheck the connection!"));
      TRACELN(F("2.Please insert the SD card!"));
    }
 }
 else
 {
 }

 delay(500);
 TRACELN(F("YAMMD - Yet Another Maerklin Motorola Decoder - ready"));

 if(MODE_SIGNAL == EEPROMData.CV[CV_MODE])
 {
    prepareSignalState(EEPROMData.LastSignalState);
 }
}

bool isButtonPressed()
{
  #if BUTTON_FCT_INVERTED
    return !digitalRead(BUTTON_FCT);
  #else
    return digitalRead(BUTTON_FCT);
  #endif
}

void checkForCVWriteMode(MaerklinMotorolaData* Data)
{
  //TODO: WIP, so that doesn't work so far :->

  
    /*Programming Sequence:
      1. Send the following sequence to current decoder address:
        Stop 1, ChangeDir 0
        Stop 0, ChangeDir 1
        Stop 1, ChangeDir 0

      2. Set the message address to the desired parameter address and repeat Stop/ChangeDir sequence as described in step 1 (but with different address)
      3. Set the message address to the desired parameter value and repeat Stop/ChangeDir sequence as described in step 1 (but with different address)
    */

  if(Data)
  {
  switch(CVWriteState)
    {
      case CVWRITESTATE_IDLE1:
        if(EEPROMData.CV[CV_ADDRESS] == Data->Address
            && Data->Stop && !Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_IDLE2;
        }
      break;
  
      case CVWRITESTATE_IDLE2:
      if(EEPROMData.CV[CV_ADDRESS] == Data->Address
            && Data->Stop && !Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_IDLE3;
        }
        else
        {
           CVWriteState = CVWRITESTATE_IDLE1;
        }
      break;
  
      case CVWRITESTATE_IDLE3:
      if(EEPROMData.CV[CV_ADDRESS] == Data->Address
            && !Data->Stop && Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_ADDRESS1;
          TRACELN(F("CV Programming - Wait for address"));
        }
        else
        {
           CVWriteState = CVWRITESTATE_IDLE1;
        }
      break;
      
      case CVWRITESTATE_ADDRESS1:
        if(Data->Stop && !Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_ADDRESS2;
          CVWriteAddress = Data->Address;
        }
      break;
      
      case CVWRITESTATE_ADDRESS2:
        if(CVWriteAddress == Data->Address
              && !Data->Stop && Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_ADDRESS3;
        }
        else
        {
           CVWriteState = CVWRITESTATE_IDLE1;
        }
      break;

      case CVWRITESTATE_ADDRESS3:
        if(CVWriteAddress == Data->Address
              && Data->Stop && !Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_VALUE1;
          TRACELN(F("CV Programming - Wait for value"));
        }
        else
        {
           CVWriteState = CVWRITESTATE_IDLE1;
        }
      break;      
      
      case CVWRITESTATE_VALUE1:
        if(Data->Stop && !Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_VALUE2;
          CVWriteValue = Data->Address;
        }
      break;
      
      case CVWRITESTATE_VALUE2:
        if(CVWriteValue == Data->Address
              && !Data->Stop && Data->ChangeDir)
        {
          CVWriteState = CVWRITESTATE_VALUE3;
        }
        else
        {
           CVWriteState = CVWRITESTATE_IDLE1;
        }
      break;

      case CVWRITESTATE_VALUE3:
        if(CVWriteValue == Data->Address
              && Data->Stop && !Data->ChangeDir)
        {
          writeCV(CVWriteAddress, CVWriteValue);
        }
        else
        {
           CVWriteState = CVWRITESTATE_IDLE1;
        }
      break;

      default:
      break;
    }
  }
}

void writeCV(unsigned char address, unsigned char value)
{
  TRACELN(F("CV Programming - Set CV ")); TRACE((address)); TRACE((" value ")); TRACE((value));
  EEPROMData.CV[address] = value;
  saveEEPROM();
  //blink n times
  setLEDMode(LED_MODE_BLINK_NUMBER, 500, 3);
}

void checkForAddressLearningMode()
{
  switch(EEPROMData.CV[CV_MODE])
  {
    case MODE_SWITCH:
    case MODE_SIGNAL:
      //set IsAddressLearningMode when program "switch" is pressed (does not work on ATTiny85 so far)
      if(isButtonPressed() && !IsAddressLearningMode)
      {
        TRACE(F("Enable address learing mode ")); 
        IsAddressLearningMode = true;
        setLEDMode(LED_MODE_BLINK_INTERVAL, 200, 0);
      }
    break;
    
    case MODE_FUNCTION_DECODER:
    case MODE_SOUND_MODULE:
    //Todo: Set IsAddressLearningMode i.e. when switching direction of train for 5 times
    break;
    
    default:
    TRACE(F("Invalid Mode ")); 
    TRACELN(EEPROMData.CV[CV_MODE]);
    break;
  }    
}

void traceMMMessage(MaerklinMotorolaData* Data)
{
    if(Data)
    {
      TRACE(millis());
      TRACE(F(" Bits: "));
      for(unsigned char i=0;i<18;i++) 
      {
        TRACEF(IsBitSet(Data->Bits, i), DEC);
      }

      TRACE(F(" - Address: ")); TRACE(Data->Address);
      TRACE(F(" - SubAddress: ")); TRACE(Data->SubAddress);
      TRACE(F(" - Function: ")); TRACE(Data->Function);
      TRACE(F(" - Stop: ")); TRACE(Data->Stop);
      TRACE(F(" - ChangeDir: ")); TRACE(Data->ChangeDir);
      TRACE(F(" - Speed: ")); TRACE(Data->Speed);
      TRACE(F(" - Step: ")); TRACE(Data->Step);
      TRACE(F(" - MM2: ")); TRACE(Data->IsMM2);
      if(Data->IsMM2)
      {
        if(Data->MM2Direction != MM2DirectionState_Unavailable)
        {
          TRACE(F(" - MM2Dir: ")); TRACE(Data->MM2Direction);
        }
        if(Data->MM2FunctionIndex != 0)
        {
          TRACE(F(" - MM2Fct: ")); TRACE(Data->MM2FunctionIndex); TRACE(Data->IsMM2FunctionOn ? " ON" : " OFF");
        }
      }
      TRACELN();
    }
}

void prepareSignalState(MODE_SIGNAL_STATE newState)
{
  if(ModeSignalState != newState)
  {
    TRACE(F("prepareSignalState "));
    TRACELN(newState);
    FadeStage = 0;
    if(INVERTED_LOGIC)
    {
      setupFadeLowHigh();
    }
    else
    {
      setupFadeHighLow();
    }

    switch(newState)
    {
      case MODE_SIGNAL_STATE_2:
        FadePin = OUTPUT_1_PIN;                      
        
      break;

      case MODE_SIGNAL_STATE_1:
        FadePin = OUTPUT_2_PIN;                      
      break;
    }

    ModeSignalState = newState;
    EEPROMData.LastSignalState = newState;
    saveEEPROM();
  }
}

void processMMDataAsSwitch(MaerklinMotorolaData* Data)
{
  checkForAddressLearningMode();
  checkForCVWriteMode(Data);

  if(MODE_SWITCH == EEPROMData.CV[CV_MODE] && SwitchedOn && millis() - SwitchCoilOnTime >= MAX_COIL_TIME)
  {
    TRACELN(F("Timeout - disable coils"));
    digitalWrite(OUTPUT_1_PIN, INVERTED_LOGIC);
    digitalWrite(OUTPUT_2_PIN, INVERTED_LOGIC);
    SwitchedOn = false;
  }
  
  if(Data) 
  {
    if(Data->IsMagnet)
    {
        if(Data->MagnetState)
        {
          if(IsAddressLearningMode)
          {
              TRACE(F("SwitchNumber: ")); TRACE(getSwitchNumber(Data));
              EEPROMData.SwitchNumber = getSwitchNumber(Data);
              if(MODE_SIGNAL == EEPROMData.CV[CV_MODE])
              {
                //No reverse logic available for signals
                EEPROMData.Reverse = false;
              }
              else
              {
                EEPROMData.Reverse = !!(Data->SubAddress & 0x01);                
              }
              
              saveEEPROM();
              TRACE(F(" Address Learned "));
              if(EEPROMData.Reverse)
              {
                TRACE(F("(Reverse mode)"));
              }
              //blink 3 times
              setLEDMode(LED_MODE_BLINK_NUMBER, 500, 3);
              IsAddressLearningMode = false;
              TRACELN();      
          }
          else
          {
            if(EEPROMData.SwitchNumber == getSwitchNumber(Data))
            {
              TRACE(F("SwitchNumber: ")); TRACE(getSwitchNumber(Data));
              if(MODE_SIGNAL == EEPROMData.CV[CV_MODE] || !SwitchedOn)
              {
                if(Data->SubAddress & 0x01)
                {
                  if(MODE_SIGNAL == EEPROMData.CV[CV_MODE])
                  {
                    //Start fading from 1 to 2 (Fadeout)
                    prepareSignalState(MODE_SIGNAL_STATE_2);
                  }
                  else
                  {
                    TRACE(F(" Powering COIL 1 "));
                    digitalWrite(OUTPUT_2_PIN, INVERTED_LOGIC);
                  }
                }
                else
                {
                  if(MODE_SIGNAL == EEPROMData.CV[CV_MODE])
                  {
                    //Start fading from 2 to 1 (Fadeout)
                    prepareSignalState(MODE_SIGNAL_STATE_1);
                  }
                  else
                  {
                    TRACE(F(" Powering COIL 0 "));
                    digitalWrite(OUTPUT_1_PIN, INVERTED_LOGIC);
                  }
                }
                SwitchedOn = true;
                SwitchCoilOnTime = millis();
              }
              TRACELN();      
            }    
          }
        }
        else
        {
          if(MODE_SWITCH == EEPROMData.CV[CV_MODE] && SwitchedOn)
          {
            TRACE(F(" All COILS off"));
            digitalWrite(OUTPUT_1_PIN, INVERTED_LOGIC);
            digitalWrite(OUTPUT_2_PIN, INVERTED_LOGIC);
            SwitchedOn = false;
            TRACELN();      
          }
        }
        
    }
  }
}

void processMMDataAsFunctionDecoder(MaerklinMotorolaData* Data)
{
  unsigned char selectedFunction = 0;
  bool isFunctionOn = false;
  unsigned char addressOffset = 0;
  
  checkForCVWriteMode(Data);
  
  if(Data) 
  {
    if(!Data->IsMagnet)
    {
      addressOffset = Data->Address - EEPROMData.CV[CV_ADDRESS];
      //check if address matches base or one of the extended addresses
      if(addressOffset >= 0 && addressOffset <= EEPROMData.CV[CV_EXTENDED_ADDRESS_COUNT])
      {
        #ifndef TRACE_ALL_MSG
        traceMMMessage(Data);
        #endif

        if(Data->MM2FunctionIndex != 0)
        {
          selectedFunction = Data->MM2FunctionIndex + addressOffset * 4;
          isFunctionOn = Data->IsMM2FunctionOn;
        }
        else if(addressOffset == 0) 
        {
          //FCT 0 (mainly used for main front/back light switch)
          selectedFunction = 0;
          isFunctionOn = Data->Function;
        }
        
        if(MODE_SOUND_MODULE == EEPROMData.CV[CV_MODE])
        {
          //Sound module expects a DFPlayer Mini on DFPLAYER Pins.
          if(0 < selectedFunction && isFunctionOn)
          {
            if(currentlyPlayingSoundIndex != selectedFunction)
            {
              dfPlayer.playFromMP3Folder(selectedFunction);
            }
          }
        }
        else
        {
          if(0 <= selectedFunction)
          {
            //TODO: Implement...
            TRACE(F("SelectedFunction "));
            TRACE(selectedFunction);
            TRACE(F(" IsOn "));
            TRACELN(isFunctionOn);
          }
        }
      }
    }
  }
}

void processMMData()
{
  MaerklinMotorolaData* Data = mm.GetData();

#if defined(TRACE_ALL_MSG)
  traceMMMessage(Data);
#endif  

  switch(EEPROMData.CV[CV_MODE])
  {
    case MODE_SWITCH:
    case MODE_SIGNAL:
    processMMDataAsSwitch(Data);
    break;
    
    case MODE_FUNCTION_DECODER:
    case MODE_SOUND_MODULE:
    processMMDataAsFunctionDecoder(Data);
    break;
    
    default:
    TRACE(F("Invalid Mode ")); 
    TRACELN(EEPROMData.CV[CV_MODE]);
    break;
  }
}

void loop() {
  processMMData();
  processLED();
  processFade();
}

void processDFPlayer(){
  if(currentlyPlayingSoundIndex && !dfPlayer.isPlaying()){
    currentlyPlayingSoundIndex = 0;
  }
}

void isr() {
  mm.PinChange();
}
