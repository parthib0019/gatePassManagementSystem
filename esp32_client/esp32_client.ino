#include <HTTPClient.h>
#include <WiFi.h>

// --------------------------------------------------------------------------
// user configuration
// --------------------------------------------------------------------------
const char *ssid = "RKMV_CSMA_ELTG";
const char *password = "MerVer@2.0.3";

// replace this with your actual ngrok url, e.g.
// "https://abcd-1234.ngrok-free.app"
String ngrok_url = "https://9002b4691718.ngrok-free.app";
// --------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  // connect to wifi
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  // give it a moment
  delay(2000);

  // 1. send "Hello" (conceptually) by asking the server for its greeting
  // technically the instructions said:
  // "server will tell the esp32 'hello' and the esp 32 will say hi to the
  // server"

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    // ----------------------------------------------------------------------
    // Step 1: GET request to root
    // Server should respond with "hello"
    // ----------------------------------------------------------------------
    String serverPath = ngrok_url + "/";

    Serial.print("Connecting to: ");
    Serial.println(serverPath);

    // needed for https sites sometimes to ignore server certificate
    // for production use proper trusted certs, but for quick ngrok testing this
    // is often easiest or use http.begin(client, serverPath) if non-https
    http.begin(serverPath);

    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      String payload = http.getString();
      Serial.print("Server says: ");
      Serial.println(payload); // Expecting "hello"

      // If server said hello, we say hi back
      if (payload == "hello") {

        // ------------------------------------------------------------------
        // Step 2: POST request to /response
        // We say "hi"
        // ------------------------------------------------------------------
        http.end(); // close previous connection

        String postPath = ngrok_url + "/response";
        http.begin(postPath);
        http.addHeader("Content-Type", "text/plain");

        int postResponseCode = http.POST("hi");

        if (postResponseCode > 0) {
          Serial.println("Sent 'hi' to server.");
          String response = http.getString();
          Serial.println("Server confirmation: " + response);
        } else {
          Serial.print("Error on sending POST: ");
          Serial.println(postResponseCode);
        }
      }

    } else {
      Serial.print("Error code: ");
      Serial.println(httpResponseCode);
    }

    http.end();
  } else {
    Serial.println("WiFi Disconnected");
  }
}

void loop() {
  // putting it in setup to run once.
  // if you want it to run repeatedly, move the logic here.
  delay(10000);
}
