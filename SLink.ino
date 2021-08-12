#include <WiFiManager.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

//#define DEBUG_MODE

#define AP_NAME "SLink Setup"
#define INFO_LED LED_BUILTIN
#define RST_BUTTON 0
#define RST_BUTTON_HI LOW
#define LISTEN_PORT 23

#ifdef DEBUG_MODE
#define DO_ECHO
#define DBG_print(msg) Serial.print(msg)
#define DBG_println(msg) Serial.println(msg)
#else
#define DBG_print(msg)
#define DBG_println(msg)
#endif

WiFiManager wm;
WiFiServer server = WiFiServer(LISTEN_PORT);
WiFiClient client;

int escapes = 0;
bool isSetupMode = false;
String baudInput = "";

void setup()
{
  // setup serial
  Serial.begin(115200);
  delay(500);
  DBG_println("SLink Boot...");

  // enable WiFiManager debug prints
#ifdef DEBUG_MODE
  wm.setDebugOutput(true);
#else
  wm.setDebugOutput(false);
#endif

  // connect wifi
  bool shouldReset = readResetButton();
  setupWifi(shouldReset);

  // ensure wifi is now connected
  if (WiFi.status() != WL_CONNECTED)
  {
    err(8, "wifi not connected after setupWifi()");
  }

  // start telnet
  setupTelnet();

  // turn on LED
  digitalWrite(INFO_LED, HIGH);
}

void loop()
{
  telnetLoop();
  serialLoop();
  wm.process();
}

//region pre- start setup
/**
 * read the RST_BUTTON
 * 
 * @return was RST_BUTTON pressed during wait?
 */
bool readResetButton()
{
  DBG_println("waiting for reset button input...");
  pinMode(INFO_LED, OUTPUT);
  pinMode(RST_BUTTON, INPUT);
  for (int i = 0; i < 20; i++)
  {
    digitalWrite(INFO_LED, i % 2 == 0);
    delay(250);
    DBG_print(".");
    if (digitalRead(RST_BUTTON) == RST_BUTTON_HI)
    {
      return true;
    }
  }

  digitalWrite(INFO_LED, LOW);
  DBG_println();
  return false;
}

/**
 * setup and connect wifi using WifiManager
 * 
 * @paran resetSaved reset saved wifi credentials
 */
void setupWifi(bool resetSaved)
{
  DBG_println("setupWifi()");

  // setup some wifi parameters
  WiFi.mode(WIFI_STA);
  WiFi.forceSleepWake();
  WiFi.setAutoReconnect(true);
  delay(200);

  // reset if requested
  if (resetSaved)
  {
    DBG_println("reset wifi settings by user request");
    wm.resetSettings();
  }

  // do the wifi manager stuff
  // this tries to connect to the saved network, and if connect fails
  // opens a AP without password for configuration
  // this blocks until we're connected
  bool isConnected = wm.autoConnect(AP_NAME);

  if (isConnected)
  {
    DBG_println("am connected now, all A-OK");

    // manually re- enable web portal for later access
    // (in non- blocking mode)
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal(AP_NAME);
  }
  else
  {
    err(5, "wifimanger failed to connect");
  }
}

//endregion

// region telnet
/**
 * setup telnet server
 */
void setupTelnet()
{
  server.begin();
  server.setNoDelay(true);
}

/**
 * telnet loop hook
 */
void telnetLoop()
{
  if (!client || !client.connected())
  {
    client = server.available();
    client.setNoDelay(true);
    client.flush();

    welcomeClient();
  }

  while (client.available())
  {
    int r = client.read();

    // check if IAC
    if (r == 0xFF)
    {
      onReceiveIAC();
      continue;
    }

    // is a normal char
    char c = r;
    onReceiveChar(&c, &r);
  }
}

/**
 * client connected, welcome them
 */
void welcomeClient()
{
  //region telnet client config
  // http://www.tcpipguide.com/free/t_TelnetProtocolCommands-3.htm

  // https://stackoverflow.com/a/1068894/13942493
  client.write(255); //IAC
  client.write(251); //WILL
  client.write(3);   //SUPPRESS_GO_AHEAD
  client.flush();

  client.write(255); //IAC
  client.write(254); //DONT
  client.write(34);  //LINEMODE
  client.flush();

  client.write(255); //IAC
  client.write(252); //WONT
  client.write(34);  //LINEMODE
  client.flush();

  // https://stackoverflow.com/a/28571812/13942493
  client.write(255); //IAC
  client.write(251); //WILL
  client.write(1);   //ECHO
  client.flush();
  //endregion

  // print MOTD
  client.println("[SLink]Welcome to SLink");
  client.println("[SLink]Press 3x ESC to enter setup mode");

  // enter setup at the start
  onEnterSetupMode();
}

/**
 * telnet client sent a command (IAC)
 * 
 * this implementation does support receiving commands, but ignores them in a 
 * really crappy way and assumes that every command = 3 bytes total (IAC DO ...)
 * but it is enough for PuTTY...
 */
void onReceiveIAC()
{
  int a = client.read();
  int b = client.read();

  DBG_print("IAC: ");
  DBG_print(a);
  DBG_print(", ");
  DBG_println(b);
}

/**
 * telnet client sent a char
 */
void onReceiveChar(char *c, int *raw)
{
  // add to input in setup mode
  if (isSetupMode)
  {
    if (*c >= '!' && *c <= '}')
    {
      // append
      baudInput += *c;
    }
    else if (*c == 127)
    {
      // DEL, remove last char
      if (baudInput.length() <= 0)
      {
        client.print(7); //BEL
        return;
      }

      baudInput.remove(baudInput.length() - 1, 1);
    }
    else if (*c == 10)
    {
      // ENTER, set baud
      int baud = setBaud(&baudInput);
      if (baud == 0)
      {
        client.println("\r\n[SLink]Baud rate failed to set!");
      }
      else
      {
        client.print("\r\n[SLink]Baud rate set to ");
        client.println(baud);
      }

      baudInput = "";
      isSetupMode = false;
    }

    client.print(*c);
    return;
  }

  // check for ESC in normal mode
  if (*raw == 27)
  {
    //ESC pressed, count up
    escapes++;
    if (escapes > 2)
    {
      onEnterSetupMode();
      client.print(" ");
      escapes = 0;
    }
  }
  else
  {
    // print captured escapes
    for (int i = 0; i < escapes; i++)
    {
      DBG_println("CAP-ESC");
    }
    escapes = 0;
  }

#ifdef DO_ECHO
  // echo
  client.write(*raw);
#endif

  // send to serial
  Serial.write(*raw);
}

/**
 * enter baud rate setup mode
 */
void onEnterSetupMode()
{
  isSetupMode = true;
  client.print("\r\n[SLink]Setup Mode. press ENTER to confirm. \r\n[SLink]baudrate: ");
}

/**
 * set the baud rate from setup mode
 * 
 * @param baudStr baud rate string input. may be invalid
 * @return baud rate that was set. 0 if invalid input
 */
int setBaud(String *baudStr)
{
  DBG_print("setBaud ");
  DBG_println(*baudStr);

  int baud = (*baudStr).toInt();
  if (baud == 0)
  {
    DBG_println("invalid baud input!");
    return 0;
  }

  Serial.end();
  Serial.begin(baud);
  return baud;
}

//endregion

// region serial

/**
 * HW serial loop hook
 */
void serialLoop()
{
  while (Serial.available())
  {
    int r = Serial.read();
    char c = r;
    onReceiveSerialChar(&c, &r);
  }
}

/**
 * HW serial received a char
 */
void onReceiveSerialChar(char *c, int *raw)
{
  if (client)
  {
    client.write(*raw);
  }
}

//endregion

/**
 * something went wrong, restart the esp
 * 
 * @param no error code number
 * @param msg error message, logged to serial
 */
void err(int no, const char *msg)
{
  // write to serial
  DBG_print("E(");
  DBG_print(no);
  DBG_print(") ");
  DBG_println(msg);

  // blink LED
  pinMode(INFO_LED, OUTPUT);
  for (int i = 0; i < (no * 2); i++)
  {
    digitalWrite(INFO_LED, i % 2 == 0);
    delay(500);
  }

  // restart
  ESP.restart();
}
