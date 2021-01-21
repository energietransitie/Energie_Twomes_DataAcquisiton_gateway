#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <utils.h>
#include <ArduinoJson.h> //this is used for parsing json

#define MAXTELEGRAMLENGTH 1500    // Length of chars to read for decoding
#define MAXSENDMESSAGELENGTH 1500 //review this, maybe this could be dynamic allocated

// Backoffice URL endpoint

//defines for selecting send_mode
#define numberOfModes 3
#define smartMeter_send_mode 0
#define roomTemp_send_mode 1
#define boilerTemp_send_mode 2

struct serverCredentials
{
  const char *serverAddress = defaultserverAddress;
  const uint16_t serverPort = defaultServerPort;
  const char *serverURI[numberOfModes] = {smartMeterUri, roomTempUri, boilerTempUri};
} serverCredentials1;

//Pin setup
HardwareSerial P1Poort(2);     // Use UART2
const int dataReceivePin = 16; // Data receive pin
const int dataReqPin = 26;     // Request pin on 26
const int redLed = 18;
const int greenLed = 23; // Tijdelijke GPIO 5, moet GPIO 23 zijn!
const int button = 19;

char telegram[MAXTELEGRAMLENGTH]; // Variable for telegram data

char sendMessage[MAXSENDMESSAGELENGTH]; //review this: this memory should be allocated in sender function

//defines for max amount of memory allocated for measurements
#define maxMeasurements_smartMeter 10
#define maxMeasurements_roomTemp 10
#define maxMeasurements_boiler 10

struct smartMeterData
{
  byte dsmrVersion;            // DSMR versie zonder punt
  long elecUsedT1;             // Elektriciteit verbruikt tarief 1 in Watt
  long elecUsedT2;             // Elektriciteit verbruikt tarief 2 in Watt
  long elecDeliveredT1;        // Elektriciteit geleverd tarief 1 in Watt
  long elecDeliveredT2;        // Elektriciteit geleverd tarief 1 in Watt
  byte currentTarrif;          // Huidig tafief
  long elecCurrentUsage;       // Huidig elektriciteitsverbruik In Watt
  long elecCurrentDeliver;     // Huidig elektriciteit levering in Watt
  long gasUsage;               // Gasverbruik in dm3
  char timeGasMeasurement[12]; // Tijdstip waarop gas voor het laats is gemeten YY:MM:DD:HH:MM:SS
  //char timeRead[12];           // Tijdstip waarop meter voor het laats is uitgelezen YY:MM:DD:HH:MM:SS
};
struct smartMeterData smartMeter_measurement[maxMeasurements_smartMeter];

struct roomTempData
{ //add variables for receiving data from roomTemp monitor device
};
struct roomTempData roomTemp_measurement[maxMeasurements_roomTemp];

struct boilerData
{ //add variables for receiving data from boiler monitor device
};
struct boilerData boiler_measurement[maxMeasurements_boiler];

// current_measurement counter
uint8_t current_measurementNumber = 0;

// Debug values:
bool printAllData = true;
bool printDecodedData = true;

// CRC
unsigned int currentCRC = 0;

// Function declaration
void getData(int);
void printData(int, int);
int FindCharInArrayRev(char[], char, int, int);
void printValue(int, int);
bool checkValues(int, bool);
void decodeTelegram(int, int);
bool procesData(int);
bool crcCheck(int, int);
unsigned int CRC16(unsigned int, unsigned char *, int);
boolean sender(uint8_t sendMode);
String convertToString(char[]);
void makePostRequest();
boolean makePostRequest2(uint8_t *sendMode);
void parse_data_into_json(uint8_t *numberMeasurements, uint8_t *sendMode);
void interruptButton();

void setup()
{
  // Start terminal communication
  Serial.begin(115200);
  Serial.println("Starting...");

  // // Pin setup
  pinMode(dataReqPin, OUTPUT);
  pinMode(redLed, OUTPUT);
  pinMode(greenLed, OUTPUT);
  pinMode(button, INPUT);
  digitalWrite(dataReqPin, HIGH);
  digitalWrite(redLed, HIGH);
  digitalWrite(greenLed, HIGH);

  // Attatch interrupt to button
  attachInterrupt(button, interruptButton, FALLING);

  // Configure P1Poort serial connection
  P1Poort.begin(115200, SERIAL_8N1, dataReceivePin, -1); // Start HardwareSerial. RX, TX

  // Connecting to Wi-Fi
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  Serial.println("WiFi in station mode");
  delay(1000);
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(500);
  }

  Serial.println("\nConnected to WiFi");
}

void loop()
{
  
  Serial.println();
  memset(telegram, 0, sizeof(telegram)); // Empty telegram
  int maxRead = 0;
  getData(maxRead);

  // for (byte sendMode = smartMeter_send_mode; sendMode <= boilerTemp_send_mode; sendMode++) //this is used for testing all send modes
  // {
  //   sender(sendMode);
  //   delay(8000);
  // }
  sender(smartMeter_send_mode);
  delay(3000);
}

// This function gets called when button is pressed
void interruptButton()
{
}

// Reads data and calls function procesData(int) to proces the data
void getData(int maxRead)
{
  while (P1Poort.available())
  { // Buffer leegmaken
    P1Poort.read();
  }
  digitalWrite(dataReqPin, LOW); // Request data

  int counter = 0;
  int timeOutCounter = 0;
  bool startFound = false;
  bool endFound = false;
  bool crcFound = false;

  while (!crcFound && timeOutCounter < 15000)
  { // Reads data untill one telegram is fully readed and copied, after not receiving data for 15 seconds the loop exits
    if (P1Poort.available())
    {
      char c = P1Poort.read();
      char inChar = (char)c;

      if (!startFound)
      { // If start is not found yes, wait for start symbol
        if (inChar == '/')
        {
          startFound = true;
          telegram[counter] = inChar;
          counter++;
        }
      }
      else if (!endFound)
      { // If start is found, copy incoming char into telegram until end symbol is found
        telegram[counter] = inChar;
        counter++;
        if (inChar == '!')
        {
          endFound = true;
        }
      }
      else
      { // If end symbol is found, copy incoming char into telegram until \n is found
        telegram[counter] = inChar;
        counter++;
        if (inChar == '\n')
        {
          crcFound = true;
        }
      }
    }
    else
    { // Wait 1 ms if no data available
      timeOutCounter++;
      delay(1);
    }
  }
  maxRead++;
  digitalWrite(dataReqPin, HIGH); // Stop requesting data
  procesData(maxRead);
}

// Checks if data a fully telegram is readed including CRC. If not, data is reread by calling function getData(int) for a maximum of 10 times per loop.
// Also calls function decodeTelegram to attach the readed parameters to variables
bool procesData(int maxRead)
{
  int startChar = FindCharInArrayRev(telegram, '/', sizeof(telegram), 0);       // Plaats van eerste char uit één diagram
  int endChar = FindCharInArrayRev(telegram, '!', sizeof(telegram), startChar); // Plaats van laatste char uit één diagram

  if (startChar >= 0 && endChar >= 0 && (endChar + 5) < MAXTELEGRAMLENGTH)
  {

    if (printAllData) // For debugging!
      printData(startChar, endChar);

    if (printDecodedData)
    { // For debugging!
      decodeTelegram(startChar, endChar);
      if (crcCheck(startChar, endChar))
      {
        current_measurementNumber++;
        return true; // Return true if CRC check is valid
      }
      else
      {
        if (maxRead < 10)
        {
          Serial.println("CRC check not valid, reading data again");
          getData(maxRead);
        }
        else
        {
          Serial.println("CRC check not valid, maxRead has reached it's limit");
          return false; // Return false if CRC check is invalid
        }
      }
    }
  }
  else
  {
    if (maxRead < 10)
    {
      Serial.println("Not all data found, reading data again");
      //client.println("Not all data found, reading data again");
      //Data was not read correctly, try reading again
      getData(maxRead);
    }
    else
    {
      Serial.println("Not all data found, maxRead has reached it's limit");
      //client.println("Not all data found, maxRead has reached it's limit");
      return false;
    }
  }
  return false;
}

// Seperates each line and calls the function checkValues(int, bool) to attach the readed parameters to a variable
void decodeTelegram(int startChar, int endChar)
{
  int placeOfNewLine = startChar;
  bool foundStart = false;
  while (placeOfNewLine < endChar && placeOfNewLine >= 0)
  {
    yield();

    if (!foundStart)
    {
      foundStart = checkValues(placeOfNewLine, foundStart);
    }
    else
    {
      checkValues(placeOfNewLine, foundStart);
    }

    placeOfNewLine = FindCharInArrayRev(telegram, '\n', endChar - startChar, placeOfNewLine) + 1;
  }
}

// Checks to what variable the readed parameter belongs and assigns the parameter to the variable by calling the function updateValue(int, int)
// Also does CRC for each line
bool checkValues(int placeOfNewLine, bool foundStart)
{
  int endChar = FindCharInArrayRev(telegram, '\n', placeOfNewLine + 300, placeOfNewLine);
  if ((strncmp(telegram + placeOfNewLine, "/", strlen("/")) == 0) && !foundStart)
  {
    // start found. Reset CRC calculation
    currentCRC = CRC16(0x0000, (unsigned char *)telegram + placeOfNewLine, endChar + 1 - placeOfNewLine);
    return true;
  }
  else
  {
    currentCRC = CRC16(currentCRC, (unsigned char *)telegram + placeOfNewLine, endChar + 1 - placeOfNewLine);
  }

  long round;
  long decimal;
  if (strncmp(telegram + placeOfNewLine, "1-3:0.2.8", strlen("1-3:0.2.8")) == 0)
  { // DSMR-versie
    Serial.print("DSMR-versie: ");
    sscanf(telegram + placeOfNewLine, "1-3:0.2.8(%ld", &round);
    smartMeter_measurement[current_measurementNumber].dsmrVersion = round;
    Serial.println(smartMeter_measurement[current_measurementNumber].dsmrVersion);
  }
  else if (strncmp(telegram + placeOfNewLine, "1-0:1.8.1", strlen("1-0:1.8.1")) == 0)
  { // Elektriciteit verbruikt T1
    Serial.print("Elektriciteit verbruikt T1: ");
    sscanf(telegram + placeOfNewLine, "1-0:1.8.1(%ld.%ld", &round, &decimal);
    smartMeter_measurement[current_measurementNumber].elecUsedT1 = round * 1000 + decimal;
    Serial.println(smartMeter_measurement[current_measurementNumber].elecUsedT1);
  }
  else if (strncmp(telegram + placeOfNewLine, "1-0:1.8.2", strlen("1-0:1.8.2")) == 0)
  { // Elektriciteit verbruikt T2
    Serial.print("Elektriciteit verbruikt T2: ");
    sscanf(telegram + placeOfNewLine, "1-0:1.8.2(%ld.%ld", &round, &decimal);
    smartMeter_measurement[current_measurementNumber].elecUsedT2 = round * 1000 + decimal;
    Serial.println(smartMeter_measurement[current_measurementNumber].elecUsedT2);
  }
  else if (strncmp(telegram + placeOfNewLine, "1-0:2.8.1", strlen("1-0:2.8.1")) == 0)
  { // Elektriciteit geleverd T1
    Serial.print("Elektriciteit geleverd T1: ");
    sscanf(telegram + placeOfNewLine, "1-0:2.8.1(%ld.%ld", &round, &decimal);
    smartMeter_measurement[current_measurementNumber].elecDeliveredT1 = round * 1000 + decimal;
    Serial.println(smartMeter_measurement[current_measurementNumber].elecDeliveredT1);
  }
  else if (strncmp(telegram + placeOfNewLine, "1-0:2.8.2", strlen("1-0:2.8.2")) == 0)
  { // Elektriciteit geleverd T2
    Serial.print("Elektriciteit geleverd T2: ");
    sscanf(telegram + placeOfNewLine, "1-0:2.8.2(%ld.%ld", &round, &decimal);
    smartMeter_measurement[current_measurementNumber].elecDeliveredT2 = round * 1000 + decimal;
    Serial.println(smartMeter_measurement[current_measurementNumber].elecDeliveredT2);
  }
  else if (strncmp(telegram + placeOfNewLine, "0-0:96.14.0", strlen("0-0:96.14.0")) == 0)
  { // Huidig tarief
    Serial.print("Hudig tarief: ");
    sscanf(telegram + placeOfNewLine, "0-0:96.14.0(%ld", &round);
    smartMeter_measurement[current_measurementNumber].currentTarrif = round;
    Serial.println(smartMeter_measurement[current_measurementNumber].currentTarrif);
  }
  else if (strncmp(telegram + placeOfNewLine, "1-0:1.7.0", strlen("1-0:1.7.0")) == 0)
  { // Actueel energie verbruik
    Serial.print("Actueel elektriciteits verbruik: ");
    sscanf(telegram + placeOfNewLine, "1-0:1.7.0(%ld.%ld", &round, &decimal);
    smartMeter_measurement[current_measurementNumber].elecCurrentUsage = round * 1000 + decimal;
    Serial.println(smartMeter_measurement[current_measurementNumber].elecCurrentUsage);
  }
  else if (strncmp(telegram + placeOfNewLine, "1-0:2.7.0", strlen("1-0:2.7.0")) == 0)
  { // Actueel energie levering
    Serial.print("Actueel elektriciteit teruglevering: ");
    sscanf(telegram + placeOfNewLine, "1-0:2.7.0(%ld.%ld", &round, &decimal);
    smartMeter_measurement[current_measurementNumber].elecCurrentDeliver = round * 1000 + decimal;
    Serial.println(smartMeter_measurement[current_measurementNumber].elecCurrentDeliver);
  }
  else if (strncmp(telegram + placeOfNewLine, "0-1:24.2.1", strlen("0-1:24.2.1")) == 0)
  { // Gasverbruik + tijdstip gemeten
    Serial.print("Gasverbruik: ");
    sscanf(telegram + placeOfNewLine + strlen("0-1:24.2.1") + 15, "(%ld.%ld", &round, &decimal);
    smartMeter_measurement[current_measurementNumber].gasUsage = round * 1000 + decimal;
    Serial.println(smartMeter_measurement[current_measurementNumber].gasUsage);
    Serial.print("Tijdstip gas gemeten: ");
    int j = 0;
    int countUntil = placeOfNewLine + strlen("0-1:24.2.1") + sizeof(smartMeter_measurement[current_measurementNumber].timeGasMeasurement) + 1;
    for (int i = placeOfNewLine + strlen("0-1:24.2.1") + 1; i < countUntil; i++)
    {
      smartMeter_measurement[current_measurementNumber].timeGasMeasurement[j] = telegram[i];
      Serial.print(smartMeter_measurement[current_measurementNumber].timeGasMeasurement[j]);
      j++;
    }
    Serial.print("\n");
  }
  /*
  else if (strncmp(telegram + placeOfNewLine, "0-0:1.0.0", strlen("0-0:1.0.0")) == 0)
  { // Tijd data uitgelezen
    Serial.print("Tijd meter uitgelezen: ");
    int j = 0;
    int countUntil = placeOfNewLine + strlen("0-0:1.0.0") + sizeof(smartMeter_measurement[current_measurementNumber].timeRead) + 1;
    for (int i = placeOfNewLine + strlen("0-0:1.0.0") + 1; i < countUntil; i++)
    {
      smartMeter_measurement[current_measurementNumber].timeRead[j] = telegram[i];
      Serial.print(smartMeter_measurement[current_measurementNumber].timeRead[j]);
      j++;
    }
    Serial.print("\n");
  }*/

  return false;
}

// Assign readed parameters to it's corresponding variable TEMP FUNCTION
void printValue(int placeOfNewLine, int valuePlace)
{
  for (int i = placeOfNewLine + valuePlace; i < placeOfNewLine + 200; i++)
  {
    Serial.print(telegram[i]);
    if (telegram[i] == '\n')
    {
      break;
    }
  }
}

// Returns the place of a certain char in a char array
int FindCharInArrayRev(char array[], char c, int len, int startCountingFrom)
{
  for (int i = startCountingFrom; i < len; i++)
  {
    if (array[i] == c)
    {
      return i;
    }
  }
  return -1;
}

// Converts Char to String and returns it TEMP FUNCTION
String convertToString(char c[11])
{
  String s = "";
  for (int i = 0; i < 12; i++)
  {
    s += c[i];
  }
  return s;
}

// Prints all the data of one telegram TEMP FUNCTION
void printData(int startChar, int endChar)
{
  Serial.println("Data is:");
  for (int i = startChar; i < endChar + 5; i++)
  {
    Serial.print(telegram[i]);
  }
  Serial.println("");
}

// Performs the final CRC
bool crcCheck(int startChar, int endChar)
{
  currentCRC = CRC16(currentCRC, (unsigned char *)telegram + endChar, 1);
  char messageCRC[5];
  strncpy(messageCRC, telegram + endChar + 1, 4);
  messageCRC[4] = 0;
  bool validCRCFound = (strtol(messageCRC, NULL, 16) == currentCRC);
  currentCRC = 0;

  if (validCRCFound)
  {
    Serial.println("\nVALID CRC FOUND!");
    return true;
  }
  else
  {
    Serial.println("\n===INVALID CRC FOUND!===");
    return false;
  }
  return false;
}

//Returns CRC
unsigned int CRC16(unsigned int crc, unsigned char *buf, int len)
{
  for (int pos = 0; pos < len; pos++)
  {
    crc ^= (unsigned int)buf[pos]; // XOR byte into least sig. byte of crc

    for (int i = 8; i != 0; i--)
    { // Loop over each bit
      if ((crc & 0x0001) != 0)
      {            // If the LSB is set
        crc >>= 1; // Shift right and XOR 0xA001
        crc ^= 0xA001;
      }
      else         // Else LSB is not set
        crc >>= 1; // Just shift right
    }
  }

  return crc;
}

boolean sender(uint8_t sendMode)
{
  //uint8_t temporyTotal = 2;
  parse_data_into_json(&current_measurementNumber, &sendMode); // Changed to current_measurementNumber. This is for smart meter only!
  if (makePostRequest2(&sendMode)){
    
    if(sendMode == smartMeter_send_mode){
      current_measurementNumber = 0; // Needs to be set to zero only if request was succesfull!
    }
    return true;
  }
  return false;
}

void parse_data_into_json(uint8_t *numberMeasurements, uint8_t *sendMode)
{
  StaticJsonDocument<MAXSENDMESSAGELENGTH> parsedJsonDoc; //maybe we could make this dynamic

  parsedJsonDoc["id"] = "AA:AA:AA:AA:AA"; //connect to correct data

  JsonObject dataSpec = parsedJsonDoc.createNestedObject("dataSpec");
  dataSpec["lastTime"] = "1610112946"; //connect to correct data
  dataSpec["interval"] = 10;           //correct to current data
  dataSpec["total"] = *numberMeasurements;

  JsonObject data = parsedJsonDoc.createNestedObject("data");

  switch (*sendMode)
  {

  case smartMeter_send_mode:
  {
    JsonArray dsmr = data.createNestedArray("dsmr");
    JsonArray evt1 = data.createNestedArray("evt1");
    JsonArray evt2 = data.createNestedArray("evt2");
    JsonArray egt1 = data.createNestedArray("egt1");
    JsonArray egt2 = data.createNestedArray("egt2");
    JsonArray ht = data.createNestedArray("ht");
    JsonArray ehv = data.createNestedArray("ehv");
    JsonArray ehl = data.createNestedArray("ehl");
    JsonArray gas = data.createNestedArray("gas");
    JsonArray tgas = data.createNestedArray("tgas");
    for (byte index1 = 0; index1 < *numberMeasurements; index1++)
    {
      dsmr.add(smartMeter_measurement[index1].dsmrVersion);
      evt1.add(smartMeter_measurement[index1].elecUsedT1);
      evt2.add(smartMeter_measurement[index1].elecUsedT2);
      egt1.add(smartMeter_measurement[index1].elecDeliveredT1);
      egt2.add(smartMeter_measurement[index1].elecDeliveredT2);
      ht.add(smartMeter_measurement[index1].currentTarrif);
      ehv.add(smartMeter_measurement[index1].elecCurrentUsage);
      ehl.add(smartMeter_measurement[index1].elecCurrentDeliver);
      gas.add(smartMeter_measurement[index1].gasUsage);
      tgas.add(smartMeter_measurement[index1].timeGasMeasurement);
    }
  }
  break;

  case roomTemp_send_mode:
  {
    JsonArray roomTemp = data.createNestedArray("roomTemp");
    for (byte index1 = 0; index1 < *numberMeasurements; index1++)
    {
      roomTemp.add(smartMeter_measurement[index1].dsmrVersion); //connect to correct data
    }
  }
  break;

  case boilerTemp_send_mode:
  {
    JsonArray pipeTemp1 = data.createNestedArray("pipeTemp1");
    JsonArray pipeTemp2 = data.createNestedArray("pipeTemp2");
    for (byte index1 = 0; index1 < *numberMeasurements; index1++)
    {
      pipeTemp1.add(smartMeter_measurement[index1].dsmrVersion); //connect to correct data
      pipeTemp2.add(smartMeter_measurement[index1].dsmrVersion); //connect to correct data
    }
  }
  break;
  }
  serializeJson(parsedJsonDoc, sendMessage); //
}

boolean makePostRequest2(uint8_t *sendMode)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    //Serial.printf("[HTTPS] Connected to Wifi\n");
    WiFiClientSecure *client = new WiFiClientSecure;
    if (client)
    {
      //Serial.printf("[HTTPS] Made client succesfully\n");
      //client->setCACert(rootCACertificate);

      {
        HTTPClient https;
        //Serial.printf("[HTTPS] Connected to server...\n");
        if (https.begin(*client, serverCredentials1.serverAddress, serverCredentials1.serverPort, serverCredentials1.serverURI[*sendMode], true))
        {

          int httpCode = https.POST(sendMessage);

          if (httpCode > 0)
          {
            //Serial.printf("[HTTPS] GET... code: %d\n", httpCode);

            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
            {
              //String payload = https.getString();
              //Serial.println(payload);
              https.end();
              delete client;
              return true;
            }
          }
          else
          {
            Serial.printf("[HTTPS] failed, error: %s\n", https.errorToString(httpCode).c_str());
          }

          https.end();
        }
        else
        {
          Serial.printf("[HTTPS] Unable to connect\n");
        }
      }
      delete client;
    }
    else
    {
      Serial.println("Unable to create client");
      return false;
    }
  }
  return false;
}
