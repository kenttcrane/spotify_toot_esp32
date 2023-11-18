#include <map>
#include <vector>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <time.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <UrlEncode.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

#define JST (9 * 3600)
#define TOOT_NO_PARENT "-1"

class MusicInfo {
public:
  String date;
  int multiNum;
  String title;
  std::vector<String> artists;
  String musicUrl;
  String playlistId;
  String playlistUrl;
};

TaskHandle_t thp[1];

WebServer server(80);
std::map<String, String> configs;


FirebaseData fbdo;
FirebaseAuth fbauth;
FirebaseConfig fbconfig;

void setup() {
  Serial.begin(115200);
  LittleFS.begin();
  configTime(JST, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");

  // Import API token etc. from config.csv (see example of config_sample.csv)
  File f = LittleFS.open("/config.csv", "r");
  while (f.available()) {
    String readStr = f.readStringUntil('\n');
    readStr.trim();
    int splitIndex = readStr.indexOf(",");
    String key = readStr.substring(0, splitIndex);
    String value = readStr.substring(splitIndex + 1);
    configs.insert(std::make_pair(key, value));
  }

  // WiFi setup
  WiFi.begin(
    configs["wifi_ssid"],
    configs["wifi_password"]
  );
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  Serial.println("WiFi Connected.");
  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());

  // Firebase Setup
  Firebase.reconnectNetwork(true);
  fbdo.setResponseSize(2048);
  fbdo.setBSSLBufferSize(4096, 1024);
  fbconfig.token_status_callback = tokenStatusCallback;
  fbconfig.api_key = configs["firebase_api_key"];
  fbauth.user.email = configs["firebase_email"];
  fbauth.user.password = configs["firebase_password"];

  // DDNS Update (Parallel)
  xTaskCreatePinnedToCore(renewDDNS, "renewDDNS", 8192, NULL, 1, &thp[0], 0);

  // Server Setup
  server.on("/", handleRoot);
  server.on("/done", handleDone);
  server.onNotFound(handleNotFound);

  server.begin();
}

void loop() {
  server.handleClient();
}

/////////////////////////////////////////////////

void renewDDNS(void *args) {
  HTTPClient http;
  while (1) {
    if (WiFi.status() == WL_CONNECTED) {
      http.begin("https://f5.si/update.php?domain=" + configs["ddns_domain"] + "&password=" + configs["ddns_password"]);
      int httpCode = http.GET();

      if (httpCode > 0) {
        String payload = http.getString();
        Serial.println("DDNS: '" + String(httpCode) + " " + payload + "'");
      } else {
        Serial.println("DDNS: Error on HTTP request");
      }
    }

    delay(70 * 1000);
  }
}

void handleRoot() {
  // Digest Authentication
  if (!server.authenticate(configs["server_auth_user_name"].c_str(), configs["server_auth_password"].c_str())) {
    return server.requestAuthentication(DIGEST_AUTH);
  }

  File f = LittleFS.open("/index.html", "r");
  String html = f.readString();
  server.send(200, "text/html", html);
}

void handleDone() {
  // Digest Authentication
  if (!server.authenticate(configs["server_auth_user_name"].c_str(), configs["server_auth_password"].c_str())) {
    return server.requestAuthentication(DIGEST_AUTH);
  }

  if (server.method() == HTTP_POST) {
    int playlistNum = server.arg("playlist_num").toInt();
    String spotifyUrl = server.arg("spotify_url");
    int multiNum = server.arg("multi_num").toInt();
    
    MusicInfo musicInfo;
    updateMusicInfo(
      &musicInfo,
      configs["spotify_client_id"],
      configs["spotify_client_secret"],
      playlistNum,
      spotifyUrl,
      multiNum
    );

    String tootText = getTootText(&musicInfo);
    Serial.println(tootText);

    String inReplyToId = lastTootOf(
      configs["mastodon_access_token"],
      musicInfo.playlistUrl
    );
    
    String tootId = toot(
      configs["mastodon_access_token"],
      tootText,
      inReplyToId
    );
    
    insertIntoFirestore(
      tootId,
      &musicInfo
    );
  }

  File f = LittleFS.open("/done.html", "r");
  String html = f.readString();
  server.send(200, "text/html", html);
}

void handleNotFound() {
  server.send(404, "text/plain", "404 not found");
}

void updateMusicInfo(MusicInfo *music, String &clientId, String &clientSecret, int playlistNum, String &url, int multiNum) {
  // Get date
  time_t t;
  struct tm *tm;
  t = time(NULL);
  tm = localtime(&t);

  // Get spotify access_token
  String spotifyRequestAuth = "https://accounts.spotify.com/api/token";
  HTTPClient http;
  http.begin(spotifyRequestAuth);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", String("Basic ") + String(base64::encode(clientId + ":" + clientSecret)));
  String requestBody = "grant_type=client_credentials";
  int httpCode = http.POST(requestBody);

  String payload = http.getString();
  Serial.println("Spotify: '" + String(httpCode) + "'");

  DynamicJsonDocument doc(8192);
  deserializeJson(doc, payload);
  JsonObject obj = doc.as<JsonObject>();

  String spotifyAccessToken = obj["access_token"];

  // Get track data
  int startIndex = url.indexOf("track/") + 6;
  int endIndex = url.indexOf("?");
  String spotifyMusicId = url.substring(startIndex, endIndex);
  http.begin("https://api.spotify.com/v1/tracks/" + spotifyMusicId + "?market=JP");
  http.addHeader("Authorization", "Bearer " + spotifyAccessToken);
  http.addHeader("Accept-Language", "ja");

  httpCode = http.GET();

  payload = http.getString();
  DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("lastTootOf: JsonDocument: ");
      Serial.println(error.c_str());
    }
  obj = doc.as<JsonObject>();

  // Update music data
  music->date = String(tm->tm_mon + 1) + "/" + String(tm->tm_mday - (tm->tm_hour < 4 ? 1 : 0));
  music->multiNum = multiNum;
  String title = obj["name"];
  music->title = title;
  std::vector<String> artists;
  for (int i = 0; i < obj["artists"].size(); i++) {
    music->artists.push_back(obj["artists"][i]["name"]);
  }
  String urlWithoutQueryParam = obj["external_urls"]["spotify"];
  music->musicUrl = urlWithoutQueryParam;

  // Get playlist URL from Firestore
  Firebase.begin(&fbconfig, &fbauth);
  Firebase.ready();
  String projectId = configs["firebase_project_id"];
  String documentPath = "/playlist";
  FirebaseJson newTransaction;
  newTransaction.set("readWrite/retryTransaction", "");
  if (Firebase.Firestore.getDocument(&fbdo,
                                     projectId,
                                     "",
                                     documentPath.c_str(),
                                     ""
                                     )) {
    DeserializationError error =  deserializeJson(doc, fbdo.payload());
    if (error) {
      Serial.print("updateMusicInfo: JsonDocument: ");
      Serial.println(error.c_str());
    }
    obj = doc.as<JsonObject>();
    String playlistId = obj["documents"][playlistNum - 1]["fields"]["id"]["stringValue"];
    (*music).playlistId = playlistId;
    String playlistUrl = obj["documents"][playlistNum - 1]["fields"]["spotify_url"]["stringValue"];
    (*music).playlistUrl = playlistUrl;
  } else {
    Serial.print("UpdateMusicInfo: FireBase error: ");
    Serial.println(fbdo.errorReason());
  }
}

String getTootText(MusicInfo *music) {
  String str = "";
  str += music->date;
  if (music->multiNum > 0) {
    str += " (" + String(music->multiNum) + ")";
  }
  str += "\n";
  str += music->title + " - ";
  for (int i = 0; i < music->artists.size(); i++) {
    if (i != 0) {
      str += ", ";
    }
    str += music->artists[i];
  }
  str += "\n";
  str += music->musicUrl + "\n\n";
  str += "プレイリストはこちら↓\n";
  str += music->playlistUrl;

  return str;
}

String lastTootOf(String &accessToken, String &playlistUrl) {
  String maxId = "9999999999999999999";
  int limit = 4;
  HTTPClient http;
  DynamicJsonDocument doc(16392);
  DeserializationError error;

  // Get user id
  http.begin("https://mstdn.jp/api/v1/accounts/verify_credentials");
  http.addHeader("Authorization", "Bearer " + accessToken);
  int httpcode = http.GET();
  String payload = http.getString();

  // Serial.println(payload);
  // Serial.println(payload.length());

  error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("lastTootOf: JsonDocument: ");
    Serial.println(error.c_str());
  }
  JsonObject obj = doc.as<JsonObject>();
  String mastodonUserId = obj["id"];

  // Search parent toot
  while (1) {
    http.begin("https://mstdn.jp/api/v1/accounts/" + mastodonUserId + "/statuses?max_id=" + maxId + "&limit=" + String(limit));
    http.addHeader("Authorization", "Bearer " + accessToken);
    int httpCode = http.GET();
    String payload = http.getString();

    // Serial.println(payload);
    // Serial.println(payload.length());

    error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("lastTootOf: JsonDocument: ");
      Serial.println(error.c_str());
    }
    JsonArray arr = doc.as<JsonArray>();

    if (arr.size() == 0) {
      break;
    }

    Serial.println("-----");
    for (JsonVariant v : arr) {
      JsonObject tootJson = v.as<JsonObject>();
      String tootId = tootJson["id"];
      String tootText = tootJson["content"];
      Serial.println(tootId + ": " + tootText);
      Serial.println("-----");

      if (tootText.indexOf(playlistUrl) != -1) {
        Serial.println("Parent:");
        Serial.println(tootText);
        return tootJson["id"];
      }

      maxId = tootJson["id"].as<String>();
    }
  }

  //If no parent, return "-1"
  Serial.println("Parent: None");
  return TOOT_NO_PARENT;
}

String toot(String &accessToken, String &text, String inReplyToId) {
  // Send toot request
  HTTPClient http;
  http.begin("https://mstdn.jp/api/v1/statuses");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", "Bearer " + accessToken);

  String requestBody = "visibility=unlisted&status=" + urlEncode(text);

  if (inReplyToId != TOOT_NO_PARENT) {
    requestBody += "&in_reply_to_id=" + inReplyToId;
  }

  int httpCode = http.POST(requestBody);

  // Get and return id of toot
  String payload = http.getString();

  // Serial.println(payload);
  // Serial.println(payload.length());

  DynamicJsonDocument doc(8192);
  DeserializationError error =  deserializeJson(doc, payload);
  if (error) {
      Serial.print("toot: JsonDocument: ");
      Serial.println(error.c_str());
    }
  JsonObject obj = doc.as<JsonObject>();

  String tootId = obj["id"];
  return tootId;
}

void insertIntoFirestore(String tootId, MusicInfo *music) {
  Firebase.begin(&fbconfig, &fbauth);
  Firebase.ready();
  
  String projectId = configs["firebase_project_id"];
  String documentPath = "/musics_" + music->playlistId + "/" + tootId;
  FirebaseJson content;
  
  content.set("fields/id/stringValue", tootId);
  content.set("fields/date/stringValue", music->date);
  content.set("fields/title/stringValue", music->title);
  for (int i = 0; i < music->artists.size(); i++) {
    String artistPath = "fields/artist/arrayValue/values/[" + String(i) + "]/stringValue";
    content.set(artistPath, music->artists[i]);
  }
  content.set("fields/url/stringValue", music->musicUrl);

  std::vector<struct firebase_firestore_document_write_t> writes;
  struct firebase_firestore_document_write_t update_write;
  update_write.type = firebase_firestore_document_write_type_update;
  update_write.update_document_content = content.raw();
  update_write.update_masks = "id,date,title,artist,url";
  update_write.update_document_path = documentPath.c_str();
  writes.push_back(update_write);

  if (Firebase.Firestore.commitDocument(&fbdo, projectId, "", writes, "")) {
    Serial.println("Firestore write ok");
  } else {
    Serial.print("Firestore write error: ");
    Serial.println(fbdo.errorReason());
  }
}
