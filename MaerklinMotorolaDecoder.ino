//Download the MaerklinMotorola Library from https://github.com/mmame/MaerklinMotorola
#include <MaerklinMotorola.h>
#include <EEPROM.h>
#include <avr/wdt.h>

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
  #define INVERTED_LOGIC 1
  #define MM_INPUT_PIN  2
  #define OUTPUT_1_PIN  1
  #define OUTPUT_2_PIN  0
  #define BUTTON_FCT    4
#else
  //use function switch on D8 for debugging purposes
  #define DEBUG_SWITCH
  #define INVERTED_LOGIC 0
  #define MM_INPUT_PIN  2
  #define OUTPUT_1_PIN  3
  #define OUTPUT_2_PIN  5
  #define OUTPUT_3_PIN  6
  #define OUTPUT_4_PIN  9
  #define OUTPUT_5_PIN  10
  #define OUTPUT_6_PIN  11
  //Direct register access for Switch button on DDB7
  #define BUTTON_INPUT_REG DDRB
  #define BUTTON_INPUT_PORT PORTB
  #define BUTTON_INPUT_PIN DDB7
  #define BUTTON_INPUT_PORTPIN PORTB7
  #define BUTTON_FCT_DEBUG 8
#endif

  
#define EEPROM_MAGIC "\x59\x41\x4D\x01"

typedef enum _MODE{
  MODE_SWITCH = 1,
  MODE_FUNCTION_DECODER,
  MODE_SIGNAL
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

#if defined (__AVR_ATtiny85__)
  #define CV_COUNT        2
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
  bool Reverse;
  MODE_SIGNAL_STATE LastSignalState;
} EEPROM_DATA, *PEEPROM_DATA;

EEPROM_DATA EEPROMData;

volatile MaerklinMotorola mm(MM_INPUT_PIN);

bool IsAddressLearningMode = false;

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
unsigned int FadeValueCurrent = 0;
int FadePin = 0;
unsigned long LastFadeTime = 0;

#define FADE_STEP_SIZE 15
#define FADE_STEP_TIME 20
#define FADE_LOW_VALUE 0
#define FADE_HIGH_VALUE 1023

bool LedBlinkState = false;
unsigned long LastLedSwitchTime = millis();
int LedBlinkIntervalMillis = 500;
int LedNumberOfBlinks = 0;
LED_MODE LedMode = LED_MODE_IDLE;

unsigned long SwitchCoilOnTime = millis();
bool  SwitchedOn = false;

#define MAX_COIL_TIME 500

CVWRITESTATE CVWriteState = CVWRITESTATE_IDLE1;
unsigned char CVWriteAddress = 0;
unsigned char CVWriteValue = 0;

unsigned int getSwitchNumber(MaerklinMotorolaData* md)
{
  unsigned int SwitchNumber = (md->SubAddress & ~0x01)/2 + (md->Address - 1) * 4 + 1;
  if(0 == md->Address)
  {
    SwitchNumber += 320;
  }

  return SwitchNumber;
}

void setLEDMode(LED_MODE newMode, int intervalMillis, int numberOfBlinks)
{
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
}

void setupFadeHighLow()
{
  TRACELN(F("setupFadeHighLow"));
  FadeState = FADE_STATE_HIGH_LOW;
  FadeValueCurrent = FADE_HIGH_VALUE;
}


void setupFadeLowHigh()
{
  TRACELN(F("setupFadeLowHigh"));
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
        TRACE(F("FADE_STATE_HIGH_LOW "));
        TRACELN(FadeValueCurrent);
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
        TRACE(F("FADE_STATE_LOW_HIGH "));
        TRACELN(FadeValueCurrent);
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

void processLED()
{
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
}

void loadEEPROM()
{
  EEPROM.get(0, EEPROMData);
  if(0 != memcmp(EEPROM_MAGIC, EEPROMData.Magic, 4))
  {
    //No valid EEPROM data found - load setup defaults (Switch mode, use switch number 1)
    TRACELN(F(" Loading Setup Defaults..."));
    memcpy(EEPROMData.Magic, EEPROM_MAGIC, 4);
    //EEPROMData.CV[CV_MODE] = MODE_FUNCTION_DECODER;
    //EEPROMData.CV[CV_ADDRESS] = 5;
    EEPROMData.CV[CV_MODE] = MODE_SIGNAL;
    EEPROMData.SwitchNumber = 1;
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

#if defined (__AVR_ATtiny85__)
  pinMode(BUTTON_FCT, INPUT);
#else
  #ifdef DEBUG_SWITCH
    pinMode(BUTTON_FCT_DEBUG, INPUT_PULLUP);
  #else
   //Button Switch Pin Configuration: Set as Input by register
    BUTTON_INPUT_REG &= ~(1 << BUTTON_INPUT_PIN);
    //Button Switch Pin Configuration: Enable Pullup
    BUTTON_INPUT_PORT |= (1 << PORTB7);  
  #endif
#endif
  
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

 delay(500);
 TRACELN(F("YAMMD - Yet Another Maerklin Motorola Decoder - ready"));

 if(MODE_SIGNAL == EEPROMData.CV[CV_MODE])
 {
    prepareSignalState(EEPROMData.LastSignalState);
 }
}

bool isButtonPressed()
{
  #if defined (__AVR_ATtiny85__)
    return digitalRead(BUTTON_FCT);
  #else
    #ifdef DEBUG_SWITCH
      return !digitalRead(BUTTON_FCT_DEBUG);
    #else
      return !(PINB & (1 << PB7));
    #endif
  #endif
}

void checkForCVWriteMode(MaerklinMotorolaData* Data)
{
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
  //blink 2 times
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
  #if defined (TRACE_ALL_MSG)
    if(Data)
    {
      TRACE(millis());
      TRACE(F(" Trits: "));
      for(int i=0;i<9;i++) 
      {
        Serial.print(Data->Trits[i]);
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
  #endif
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
  checkForCVWriteMode(Data);
  if(Data) 
  {
    if(!Data->IsMagnet)
    {
      if(EEPROMData.CV[CV_ADDRESS] == Data->Address)
      {
        //TODO: Implement...
      }
    }
  }
}

void processMMData()
{
  mm.Parse();
  MaerklinMotorolaData* Data = mm.GetData();
  traceMMMessage(Data);

  switch(EEPROMData.CV[CV_MODE])
  {
    case MODE_SWITCH:
    case MODE_SIGNAL:
    processMMDataAsSwitch(Data);
    break;
    
    case MODE_FUNCTION_DECODER:
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

void isr() {
  mm.PinChange();
}
