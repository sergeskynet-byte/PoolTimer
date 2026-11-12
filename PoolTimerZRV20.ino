// PoolTimer ZR v2.0 - Hors-gel 24h/24 avec correction de la temporisation 1h (Mode sonde défectueuse en continu)
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h> 
#include "time.h"
#include <math.h>

// --- Configuration des broches ---
#define RELAY_PIN 2       // Relais Filtration
#define LIGHT_PIN 4       // Relais Éclairage
#define ONE_WIRE_BUS 5    // Capteur Température
#define LED_WIFI 8        // LED Statut WiFi

// --- Objets ---
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
AsyncWebServer server(80);
WiFiManager wm;
Preferences preferences; 

// --- Variables globales ---
float tempEauActuelle = 20.0; 
float temperatureAffichee = 20.0;
bool temperatureAfficheeValide = false;

bool defautSonde = false;
uint8_t lecturesInvalides = 0, lecturesValides = 0;
const uint8_t SEUIL_CONFIRMATION_SONDE = 3;
bool modeHivernage = false;
const float ENTREE_HIVER = 11.5f, SORTIE_HIVER = 12.5f;
const unsigned long SECURITE_PERIODE_MS = 7200000UL; // 2 heures (non utilisé si marche continue)
const unsigned long SECURITE_MARCHE_MS = 900000UL; // 15 minutes (non utilisé si marche continue)
unsigned long debutPeriodeSecuriteMs = 0;
bool securiteInitialisee = false;
unsigned long dernierEchantillonValideMs = 0;
 
int heureDemarrage = 8;
bool filtrationEnCours = false;
unsigned long debutFiltrationMs = 0;
unsigned long dureeTotaleFiltrationMs = 0;
bool ReleveFaitPourCeCycle = false;
bool premierDemarrage = true;
bool releveDebutCycleFait = false; 
int dernierJourCycleSaisonnier = -1;
int derniereAnneeCycleSaisonnier = -1;
bool repriseSaisonConfirmee = false;
int jourRepriseSaison = -1;
int anneeRepriseSaison = -1;

float dureeAfficheeInterface = 0.0;
bool cycleControleNocturneEnCours = false;
bool controleNocturneFaitAujourdhui = false;
const unsigned long INTERVALLE_CONTROLE_FROID = 14400000UL;
unsigned long dernierControleHiverMs = 0;
bool controleHiverSupplementaire = false;

// Variables pour gérer le délai d'une heure après dépassement des 4°C en hors-gel
unsigned long debutDepassement4C_Ms = 0;
bool temporisationHorsGelEnCours = false;
const unsigned long TEMPO_SORTIE_HORS_GEL = 3600000UL; // 1 heure en millisecondes

unsigned long precedentMillisLed = 0;
const long intervalleLed = 1000; 
bool etatLed = LOW;
bool wifiEstConnecte = false; 

unsigned long precedentMillisSauvegarde = 0;
bool repriseApresCoupure = false;

// --- Historique des températures ---
#define NB_POINTS_HISTORIQUE 360 
float historiqueTemps[NB_POINTS_HISTORIQUE];
String historiqueHeures[NB_POINTS_HISTORIQUE];
int indexHistorique = 0;
int nbPointsActuels = 0;
unsigned long dernierReleveGraphMs = 0;
const unsigned long INTERVALLE_RELEVE_GRAPH = 7200000; 
const unsigned long DELAI_STABILISATION_EAU = 300000UL; 

// --- Historique de filtration journalière ---
#define NB_JOURS_HISTORIQUE 30
float historiqueFiltration[NB_JOURS_HISTORIQUE];
String historiqueJours[NB_JOURS_HISTORIQUE];
int indexJours = 0;
int nbJoursActuels = 0;
int dernierJourEnregistre = -1;

// --- Sauvegarde non volatile (LittleFS) ---
bool historiqueFlashDisponible = false;
bool historiqueFiltrationModifie = false;
unsigned long dernierEnregistrementFiltrationMs = 0;
const unsigned long INTERVALLE_SAUVEGARDE_FILTRATION = 60000UL; 

bool enregistrerHistoriqueFlash(const char *chemin, const String &contenu) {
  if (!historiqueFlashDisponible) return false;
  String temporaire = String(chemin) + ".tmp";
  File fichier = LittleFS.open(temporaire, "w");
  if (!fichier) return false;
  size_t ecrits = fichier.print(contenu);
  fichier.flush();
  fichier.close();
  if (ecrits != contenu.length()) { LittleFS.remove(temporaire); return false; }
  if (!LittleFS.rename(temporaire, chemin)) {
    LittleFS.remove(temporaire);
    return false;
  }
  return true;
}

void sauvegarderTemperatures() {
  if (!historiqueFlashDisponible) return;
  String contenu;
  contenu.reserve(nbPointsActuels * 28 + 12);
  for (int i = 0; i < nbPointsActuels; i++) {
    int pos = (indexHistorique - nbPointsActuels + i + NB_POINTS_HISTORIQUE) % NB_POINTS_HISTORIQUE;
    contenu += historiqueHeures[pos] + ";" + String(historiqueTemps[pos], 2) + "\n";
  }
  if (!enregistrerHistoriqueFlash("/temperatures.csv", contenu))
    Serial.println("Erreur sauvegarde temperatures LittleFS");
}

void sauvegarderFiltration() {
  if (!historiqueFlashDisponible) return;
  String contenu;
  contenu.reserve(nbJoursActuels * 20 + 12);
  for (int i = 0; i < nbJoursActuels; i++) {
    int pos = (indexJours - nbJoursActuels + i + NB_JOURS_HISTORIQUE) % NB_JOURS_HISTORIQUE;
    contenu += historiqueJours[pos] + ";" + String(historiqueFiltration[pos], 3) + "\n";
  }
  if (enregistrerHistoriqueFlash("/filtration.csv", contenu)) {
    historiqueFiltrationModifie = false;
    dernierEnregistrementFiltrationMs = millis();
  } else Serial.println("Erreur sauvegarde filtration LittleFS");
}

void diagnosticPartitionLittleFS() {
  Serial.println("[LFS] Diagnostic partition");
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (partition == nullptr) {
    Serial.println("[LFS] ERREUR : aucune partition trouvee.");
  } else {
    Serial.printf("[LFS] Partition : label=%s, adresse=0x%lx, taille=%lu octets\n",
                  partition->label, (unsigned long)partition->address,
                  (unsigned long)partition->size);
  }
}

void testerLectureEcritureLittleFS() {
  const char* chemin = "/.pooltimer_test_lfs.tmp";
  const char* contenu = "POOL_TIMER_LFS_OK";
  File f = LittleFS.open(chemin, "w");
  if (!f) return;
  size_t n = f.print(contenu);
  f.flush();
  f.close();
  if (n != strlen(contenu)) { LittleFS.remove(chemin); return; }
  f = LittleFS.open(chemin, "r");
  if (!f) { LittleFS.remove(chemin); return; }
  String relu = f.readString();
  f.close();
  LittleFS.remove(chemin);
  if (relu == contenu) Serial.println("[LFS] OK : lecture/ecriture verifiees.");
}

void chargerHistoriquesFlash() {
  diagnosticPartitionLittleFS();
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!partition) {
    historiqueFlashDisponible = false;
    return;
  }
  historiqueFlashDisponible = LittleFS.begin(false);
  if (!historiqueFlashDisponible) {
    Serial.println("[LFS] ECHEC : historiques conserves en RAM uniquement.");
    return;
  }
  testerLectureEcritureLittleFS();
  if (LittleFS.exists("/temperatures.csv")) {
    File f = LittleFS.open("/temperatures.csv", "r");
    while (f && f.available()) {
      String ligne = f.readStringUntil('\n'); ligne.trim();
      int separateur = ligne.lastIndexOf(';');
      if (separateur <= 0) continue;
      String date = ligne.substring(0, separateur);
      float temperature = ligne.substring(separateur + 1).toFloat();
      historiqueHeures[indexHistorique] = date;
      historiqueTemps[indexHistorique] = temperature;
      indexHistorique = (indexHistorique + 1) % NB_POINTS_HISTORIQUE;
      if (nbPointsActuels < NB_POINTS_HISTORIQUE) nbPointsActuels++;
    }
    f.close();
  }
  if (LittleFS.exists("/filtration.csv")) {
    File f = LittleFS.open("/filtration.csv", "r");
    while (f && f.available()) {
      String ligne = f.readStringUntil('\n'); ligne.trim();
      int separateur = ligne.lastIndexOf(';');
      if (separateur <= 0) continue;
      historiqueJours[indexJours] = ligne.substring(0, separateur);
      historiqueFiltration[indexJours] = ligne.substring(separateur + 1).toFloat();
      indexJours = (indexJours + 1) % NB_JOURS_HISTORIQUE;
      if (nbJoursActuels < NB_JOURS_HISTORIQUE) nbJoursActuels++;
    }
    f.close();
  }
}

unsigned long cumulFiltrationJourMs = 0;
unsigned long dernierCalculCumulMs = 0;

enum ModeFiltration { MODE_AUTO, MODE_FORCE_ON, MODE_FORCE_OFF, MODE_HORS_GEL };
ModeFiltration modeActuel = MODE_AUTO;
unsigned long debutMarcheForceeMs = 0;
const unsigned long DUREE_MARCHE_FORCEE = 7200000; 

const char* ntpServer = "pool.ntp.org";
int gmtHiver = 1;

void appliquerFuseauHoraire() {
  String tz = "STD" + String(-gmtHiver) + "DST" + String(-(gmtHiver + 1)) + ",M3.5.0/2,M10.5.0/3";
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

void synchroniserHorloge() {
  appliquerFuseauHoraire();
  configTime(0, 0, ntpServer, "time.nist.gov");
  appliquerFuseauHoraire();
}

void relancerMDNS() {
  MDNS.end();
  if (MDNS.begin("pooltimer")) {
    MDNS.addService("http", "tcp", 80);
  }
}

float calculerTempsFiltration(float temp) {
  float tempsCalcule = 0.0;
  if (temp < 12.0) tempsCalcule = 0.0;                  
  else if (temp >= 12.0 && temp < 14.0) tempsCalcule = 3.0;
  else if (temp >= 14.0 && temp < 16.0) tempsCalcule = 4.0;
  else if (temp >= 16.0 && temp < 20.0) tempsCalcule = 6.0;
  else if (temp >= 20.0 && temp < 24.0) tempsCalcule = 8.0;
  else if (temp >= 24.0 && temp < 27.0) tempsCalcule = 10.0;
  else tempsCalcule = 10.0 + ((temp - 27.0) * 3.0); 

  if (tempsCalcule > 23.916) tempsCalcule = 23.916;
  return tempsCalcule;
}

int getHeureActuelle() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return -1;
  return timeinfo.tm_hour;
}

int getMinuteActuelle() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return -1;
  return timeinfo.tm_min;
}

int getJourActuel() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return -1;
  return timeinfo.tm_mday;
}

String getHeureJourShort() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "--/-- --:--:--";
  char buff[20];
  strftime(buff, sizeof(buff), "%d/%m %H:%M:%S", &timeinfo);
  return String(buff);
}

String getJourShort() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "--/--";
  char buff[10];
  strftime(buff, sizeof(buff), "%d/%m", &timeinfo);
  return String(buff);
}

String getHeureComplete() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "--:--:--";
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

const int MAX_EVENEMENTS = 200;
String journalEvenements[MAX_EVENEMENTS];
int nombreEvenements = 0, prochainEvenement = 0;
unsigned long sequenceEvenement = 0;

void journaliser(const String &message) {
  String entree = String(++sequenceEvenement) + ";" + getHeureJourShort() + ";" + message;
  journalEvenements[prochainEvenement] = entree;
  prochainEvenement = (prochainEvenement + 1) % MAX_EVENEMENTS;
  if (nombreEvenements < MAX_EVENEMENTS) nombreEvenements++;
  Serial.println("EVENT " + entree);
  if (!historiqueFlashDisponible) return;
  String texte;
  for (int i = 0; i < nombreEvenements; i++) {
    int index = (prochainEvenement - nombreEvenements + i + MAX_EVENEMENTS) % MAX_EVENEMENTS;
    texte += journalEvenements[index] + "\n";
  }
  enregistrerHistoriqueFlash("/evenements.csv", texte);
}

void chargerJournal() {
  if (!historiqueFlashDisponible || !LittleFS.exists("/evenements.csv")) return;
  File f = LittleFS.open("/evenements.csv", "r");
  if (!f) return;
  while (f.available()) {
    String ligne = f.readStringUntil('\n'); ligne.trim();
    if (ligne.isEmpty()) continue;
    journalEvenements[prochainEvenement] = ligne;
    prochainEvenement = (prochainEvenement + 1) % MAX_EVENEMENTS;
    if (nombreEvenements < MAX_EVENEMENTS) nombreEvenements++;
    unsigned long n = ligne.substring(0, ligne.indexOf(';')).toInt();
    if (n > sequenceEvenement) sequenceEvenement = n;
  }
  f.close();
}

void acquerirTemperature() {
  sensors.requestTemperatures();
  float tempLue = sensors.getTempCByIndex(0);
  bool valide = isfinite(tempLue) && tempLue != DEVICE_DISCONNECTED_C &&
                tempLue > -20.0f && tempLue < 60.0f && fabsf(tempLue - 85.0f) > 0.01f;
  if (!valide) {
    lecturesValides = 0;
    if (lecturesInvalides < 255) lecturesInvalides++;
    if (!defautSonde && lecturesInvalides >= SEUIL_CONFIRMATION_SONDE) {
      defautSonde = true;
      securiteInitialisee = false;
      journaliser("DEFAUT_SONDE;Mode securite continu (24h/24)");
    }
    return;
  }
  lecturesInvalides = 0;
  if (lecturesValides < 255) lecturesValides++;
  if (defautSonde && lecturesValides < SEUIL_CONFIRMATION_SONDE) return;
  if (defautSonde) {
    defautSonde = false;
    securiteInitialisee = false;
    journaliser("SONDE_RETABLIE;Retour au mode automatique");
  }
  tempEauActuelle = tempLue;
  dernierEchantillonValideMs = millis();
}

void actualiserSaisonApresStabilisation() {
  if (defautSonde) return;
  bool ancien = modeHivernage;
  if (modeHivernage && tempEauActuelle >= SORTIE_HIVER) modeHivernage = false;
  else if (!modeHivernage && tempEauActuelle <= ENTREE_HIVER) modeHivernage = true;
  if (ancien != modeHivernage)
    journaliser(modeHivernage ? "PASSAGE_HIVERNAGE" : "PASSAGE_SAISON");
}

void ajouterAuGraphique(float temp, String horodatage) {
  if (nbPointsActuels > 0) {
    int precedent = (indexHistorique - 1 + NB_POINTS_HISTORIQUE) % NB_POINTS_HISTORIQUE;
    if (historiqueHeures[precedent] == horodatage) {
      historiqueTemps[precedent] = temp;
      sauvegarderTemperatures();
      return;
    }
  }
  historiqueTemps[indexHistorique] = temp;
  historiqueHeures[indexHistorique] = horodatage;
  indexHistorique = (indexHistorique + 1) % NB_POINTS_HISTORIQUE;
  if (nbPointsActuels < NB_POINTS_HISTORIQUE) nbPointsActuels++;
  sauvegarderTemperatures();
}

void ajouterFiltrationAuGraphique(float heures, String jour) {
  int indexPrecedent = (indexJours - 1 + NB_JOURS_HISTORIQUE) % NB_JOURS_HISTORIQUE;
  if (nbJoursActuels > 0 && historiqueJours[indexPrecedent] == jour) {
    historiqueFiltration[indexPrecedent] = heures;
  } else {
    historiqueFiltration[indexJours] = heures;
    historiqueJours[indexJours] = jour;
    indexJours = (indexJours + 1) % NB_JOURS_HISTORIQUE;
    if (nbJoursActuels < NB_JOURS_HISTORIQUE) nbJoursActuels++;
  }
  historiqueFiltrationModifie = true;
}

int getQualiteWifi() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

// --- Fichiers Web (HTML, PWA) v2.0 ---
const char manifest_json[] PROGMEM = R"rawliteral({
  "name": "PoolTimer ZR v2.0",
  "short_name": "PoolTimer",
  "start_url": "/",
  "display": "standalone",
  "background_color": "#f4f7f9",
  "theme_color": "#0288d1",
  "orientation": "portrait",
  "icons": [
    { "src": "https://img.icons8.com/fluency/192/swimming-pool.png", "sizes": "192x192", "type": "image/png" },
    { "src": "https://img.icons8.com/fluency/512/swimming-pool.png", "sizes": "512x512", "type": "image/png" }
  ]
})rawliteral";

const char sw_js[] PROGMEM = R"rawliteral(
self.addEventListener('install', function(e) { self.skipWaiting(); });
self.addEventListener('fetch', function(e) { e.respondWith(fetch(e.request)); });
)rawliteral";

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <meta name="theme-color" content="#0288d1">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-status-bar-style" content="default">
    <meta name="apple-mobile-web-app-title" content="PoolTimer">
    <link rel="apple-touch-icon" href="https://img.icons8.com/fluency/192/swimming-pool.png">
    <link rel="manifest" href="/manifest.json">
    <title>PoolTimer ZR V 2.0</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; background-color: #f4f7f9; color: #333; margin: 0; padding: 20px; }
        .container { max-width: 600px; margin: auto; background: white; padding: 25px; border-radius: 15px; box-shadow: 0px 4px 15px rgba(0,0,0,0.05); position: relative; }
        .lang-container { position: absolute; top: 15px; right: 20px; }
        .btn-lang { background: #fff; border: 1px solid #ccc; padding: 5px 10px; border-radius: 20px; cursor: pointer; font-weight: bold; font-size: 0.85em; transition: 0.2s; box-shadow: 0 2px 5px rgba(0,0,0,0.05); }
        .btn-lang:hover { background: #ebf5fb; border-color: #0288d1; }
        .logo-container { display: flex; justify-content: center; align-items: center; width: 100%; margin: 10px auto 15px auto; }
        .logo-img { width: 100%; max-width: 320px; height: auto; border-radius: 8px; }
        h1 { color: #0288d1; margin-bottom: 25px; font-size: 1.8em; margin-top: 5px; font-weight: bold; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-bottom: 25px; }
        .card { background: #ebf5fb; padding: 15px; border-radius: 10px; text-align: center; }
        .card-full { grid-column: span 2; background: #e3f2fd; }
        .card-purple { background: #f3e5f5; grid-column: span 2; }
        .card h3 { margin: 0 0 10px 0; font-size: 0.9em; color: #5a738e; text-transform: uppercase; }
        .valeur { font-size: 1.4em; font-weight: bold; color: #01579b; }
        .valeur-purple { color: #4a148c; }
        .periode-texte { font-size: 0.8em; color: #555; margin-top: 5px; font-weight: normal; }
        .section { margin-top: 25px; padding-top: 20px; border-top: 2px dashed #ebf5fb; }
        .row-control { margin: 15px 0; display: flex; justify-content: center; align-items: center; gap: 10px; flex-wrap: wrap; }
        .btn { display: inline-block; padding: 12px 24px; margin: 5px; font-size: 1em; border: none; border-radius: 5px; cursor: pointer; transition: 0.3s; color: white; font-weight: bold;}
        .btn-blue { background-color: #0288d1; } .btn-blue:hover { background-color: #01579b; }
        .btn-red { background-color: #e53935; } .btn-red:hover { background-color: #b71c1c; }
        .btn-green { background-color: #4caf50; } .btn-green:hover { background-color: #1b5e20; }
        select { padding: 10px; font-size: 1em; border-radius: 5px; border: 1px solid #ccc; width: 100px; text-align: center; }
        .notice-gratuite { margin: 22px auto 0; padding: 16px; border-radius: 12px; background: #eaf6ff; border: 1px solid #b8ddf7; color: #174b6e; line-height: 1.55; font-size: .9em; }
        .journal-liste { text-align: left; max-height: 270px; overflow-y: auto; padding: 10px; border-radius: 9px; background: #f5f9fc; border: 1px solid #dce7ef; }
        .journal-ligne { padding: 8px 5px; border-bottom: 1px solid #dce7ef; font-size: .82em; overflow-wrap: anywhere; }
        .journal-ligne:last-child { border-bottom: 0; }
        .graph-box { background: #ffffff; border: 1px solid #e1e8ed; padding: 10px; border-radius: 10px; margin-top: 15px; }
    </style>
    <script>
        let langueActuelle = 'en'; 
        const ordreLangues = ['fr', 'en', 'it', 'de', 'es', 'pt', 'el'];
        const drapeauxLangues = { fr: '🇬🇧 EN', en: '🇮🇹 IT', it: '🇩🇪 DE', de: '🇪🇸 ES', es: '🇵🇹 PT', pt: '🇬🇷 EL', el: '🇫🇷 FR' };
        let dernierStatutBrut = '--';
        let dernierModeBrut = '--';
        let lumiereAllumee = false;
        let deferredPrompt = null;
        let monGraphique = null;

        const dictionnaire = {
            fr: { titre_journal: "Journal des événements", notice_gratuite: "PoolTimer ZR est un projet libre et gratuit, conçu dans un esprit de partage. Ses plans et son programme sont mis gratuitement à disposition de tous. Toute commercialisation du PoolTimer ZR est interdite.",
                lbl_heure_actuelle: "Heure Actuelle", lbl_mode_actuel: "Mode Actuel", lbl_temp_eau: "Température", lbl_statut_pompe: "État Pompe", lbl_temps_filtration: "Temps de Filtration", lbl_signal_wifi: "Signal WiFi", titre_prog: "Configuration Système", lbl_modif_heure: "Démarrage : ", lbl_modif_gmt: "Fuseau (GMT) : ", btn_enregistrer: "Enregistrer", titre_cmd: "Commandes Manuelles", btn_auto: "Mode Auto", btn_force_on: "Marche Forcée (2h)", btn_force_off: "Arrêt Forcé", alert_save: "Paramètres sauvegardés !", "ALLUMÉE": "ALLUMÉE", "ÉTEINTE": "ÉTEINTE", "AUTOMATIQUE": "AUTOMATIQUE", "MARCHE FORCÉE": "MARCHE FORCÉE", "ARRÊT FORCÉ": "ARRÊT FORCÉ", "SÉCURITÉ HORS-GEL": "SÉCURITÉ HORS-GEL", "TEST NOCTURNE HORS-GEL (5 min)": "TEST NOCTURNE HORS-GEL (5 min)", "REPRISE APRÈS COUPURE": "REPRISE EN COURS ⚡", "Excellent": "Excellent 📶", "Bon": "Bon 📡", "Moyen": "Moyen ⚠️", "Faible": "Faible ❌", "Déconnecté": "Déconnecté", lbl_periode: "Période : ", "Hors-gel": "🚨 Hors-gel", "Hivernage": "❄️ Hivernage", "Saison de baignade": "🏊 Saison de baignade", "Canicule": "🔥 Canicule", titre_lum: "Éclairage Piscine", btn_lum_on: "Allumer les projecteurs 💡", btn_lum_off: "Éteindre les projecteurs 🌑", btn_install: "📲 Installer l'application mobile", graph_titre: "Suivi Température & Filtration", lbl_graph_barres: "Filtration (h)", alert_ios: "Sur iPhone : Appuyez sur le bouton 'Partager' en bas de Safari, puis choisissez 'Sur l'écran d'accueil'.", alert_pwa_fail: "Si l'installation ne démarre pas, ajoutez l'application depuis le menu de votre navigateur.", lbl_ip_boitier: "Adresse IP du boîtier : ", btn_sync_ntp: "Forcer la synchro NTP 🔄", alert_ntp_ok: "Synchronisation NTP demandée !", alert_ntp_err: "Erreur lors de la synchronisation."
            },
            en: { titre_journal: "Event log", notice_gratuite: "PoolTimer ZR may not be sold: its plans and program are provided free of charge.",
                lbl_heure_actuelle: "Current Time", lbl_mode_actuel: "Current Mode", lbl_temp_eau: "Temperature", lbl_statut_pompe: "Pump Status", lbl_temps_filtration: "Filtration Runtime", lbl_signal_wifi: "WiFi Signal Quality", titre_prog: "System Configuration", lbl_modif_heure: "Start Hour: ", lbl_modif_gmt: "Timezone (GMT): ", btn_enregistrer: "Save Configuration", titre_cmd: "Manual Override", btn_auto: "Auto Mode", btn_force_on: "Forced ON (2h)", btn_force_off: "Forced OFF", alert_save: "Settings saved!", "ALLUMÉE": "RUNNING", "ÉTEINTE": "STOPPED", "AUTOMATIQUE": "AUTOMATIC", "MARCHE FORCÉE": "FORCED ON", "ARRÊT FORCÉ": "FORCED OFF", "SÉCURITÉ HORS-GEL": "ANTI-FREEZE", "TEST NOCTURNE HORS-GEL (5 min)": "NIGHTLY TEST", "REPRISE APRÈS COUPURE": "POWER RECOVERY ⚡", "Excellent": "Excellent 📶", "Bon": "Good 📡", "Moyen": "Fair ⚠️", "Faible": "Weak ❌", "Déconnecté": "Disconnected", lbl_periode: "Period: ", "Hors-gel": "🚨 Anti-freeze", "Hivernage": "❄️ Winterizing", "Saison de baignade": "🏊 Pool Season", "Canicule": "🔥 Heatwave", titre_lum: "Pool Lighting", btn_lum_on: "Turn Lights ON 💡", btn_lum_off: "Turn Lights OFF 🌑", btn_install: "📲 Install Mobile App", graph_titre: "Temperature & Filtration Logs", lbl_graph_barres: "Filtration (h)", alert_ios: "On iPhone: Tap 'Share' in Safari, then 'Add to Home Screen'.", alert_pwa_fail: "Add to home screen manually from browser menu.", lbl_ip_boitier: "Device IP Address: ", btn_sync_ntp: "Force NTP Sync 🔄", alert_ntp_ok: "NTP synchronization requested!", alert_ntp_err: "Error during synchronization."
            },
            it: { titre_journal: "Registro eventi", notice_gratuite: "PoolTimer ZR non può essere venduto: schemi e programma sono forniti gratuitamente.",
                lbl_heure_actuelle: "Ora Corrente", lbl_mode_actuel: "Modalità Corrente", lbl_temp_eau: "Temperatura", lbl_statut_pompe: "Stato Pompa", lbl_temps_filtration: "Tempo di Filtrazione", lbl_signal_wifi: "Segnale WiFi", titre_prog: "Configurazione", lbl_modif_heure: "Avvio: ", lbl_modif_gmt: "Fuso (GMT): ", btn_enregistrer: "Salva", titre_cmd: "Comandi Manuali", btn_auto: "Modalità Auto", btn_force_on: "Forzato ACCESO (2h)", btn_force_off: "Arresto Forzato", alert_save: "Impostazioni salvate!", "ALLUMÉE": "ACCESA", "ÉTEINTE": "SPENTA", "AUTOMATIQUE": "AUTOMATICO", "MARCHE FORZATA": "MARCIA FORZATA", "ARRÊT FORCÉ": "ARRESTO FORZATO", "SÉCURITÉ HORS-GEL": "ANTIGELO", "TEST NOTTURNO HORS-GEL (5 min)": "TEST NOTTURNO", "REPRISE APRÈS COUPURE": "RIPRISTINO ENERGIA ⚡", "Excellent": "Eccellente 📶", "Bon": "Buono 📡", "Moyen": "Medio ⚠️", "Faible": "Debole ❌", "Disconnesso": "Disconnesso", lbl_periode: "Periodo: ", "Hors-gel": "🚨 Antigelo", "Hivernage": "❄️ Svernamento", "Saison de baignade": "🏊 Stagione Balneare", "Canicule": "🔥 Ondata di Calore", titre_lum: "Illuminazione Piscina", btn_lum_on: "Accendi i fari 💡", btn_lum_off: "Spegni i fari 🌑", btn_install: "📲 Installa l'applicazione", graph_titre: "Cronologia Temp & Filtrazione", lbl_graph_barres: "Filtrazione (h)", alert_ios: "Su iPhone: Condividi -> Aggiungi alla schermata Home.", alert_pwa_fail: "Aggiungi alla schermata Home manualmente.", lbl_ip_boitier: "Indirizzo IP del dispositivo: ", btn_sync_ntp: "Forza NTP 🔄", alert_ntp_ok: "Sincronizzazione NTP richiesta!", alert_ntp_err: "Errore di sincronizzazione."
            },
            de: { titre_journal: "Ereignisprotokoll", notice_gratuite: "PoolTimer ZR darf nicht verkauft werden: Baupläne und Programm werden kostenlos bereitgestellt.",
                lbl_heure_actuelle: "Aktuelle Uhrzeit", lbl_mode_actuel: "Aktueller Modus", lbl_temp_eau: "Temperatur", lbl_statut_pompe: "Pumpenstatus", lbl_temps_filtration: "Filtrationszeit", lbl_signal_wifi: "WiFi Signalstärke", titre_prog: "Systemkonfiguration", lbl_modif_heure: "Startzeit: ", lbl_modif_gmt: "Zeitzone (GMT): ", btn_enregistrer: "Speichern", titre_cmd: "Manuelle Steuerung", btn_auto: "Auto-Modus", btn_force_on: "Dauer-EIN (2h)", btn_force_off: "Dauer-AUS", alert_save: "Einstellungen gespeichert!", "ALLUMÉE": "IN BETRIEB", "ÉTEINTE": "GESTOPPT", "AUTOMATSCH": "AUTOMATISCH", "MARCHE FORCÉE": "ZWANGSLAUF", "ARRÊT FORCÉ": "ZWANGS-AUS", "SÉCURITÉ HORS-GEL": "FROSTSCHUTZ", "TEST NOCTURNE HORS-GEL (5 min)": "FROSTTEST", "REPRISE APRÈS COUPURE": "STROM-WIEDERHERSTELLUNG ⚡", "Excellent": "Ausgezeichnet 📶", "Bon": "Gut 📡", "Moyen": "Mittel ⚠️", "Faible": "Schwach ❌", "Getrennt": "Getrennt", lbl_periode: "Zeitraum: ", "Hors-gel": "🚨 Frostschutz", "Hivernage": "❄️ Überwinterung", "Saison de baignade": "🏊 Badesaison", "Canicule": "🔥 Hitzewelle", titre_lum: "Poolbeleuchtung", btn_lum_on: "Scheinwerfer EIN 💡", btn_lum_off: "Scheinwerfer AUS 🌑", btn_install: "📲 App installieren", graph_titre: "Temperatur & Filterverlauf", lbl_graph_barres: "Filtration (h)", alert_ios: "Auf iPhone: Zum Home-Bildschirm.", alert_pwa_fail: "Zum Startbildschirm manuell hinzufügen.", lbl_ip_boitier: "Geräte-IP-Adresse: ", btn_sync_ntp: "NTP Sync erzwingen 🔄", alert_ntp_ok: "NTP-Synchronisierung angefordert!", alert_ntp_err: "Fehler bei der Synchronisierung."
            },
            es: { titre_journal: "Registro de eventos", notice_gratuite: "PoolTimer ZR no se puede vender: los planos y el programa se proporcionan gratuitamente.",
                lbl_heure_actuelle: "Hora Actual", lbl_mode_actuel: "Modo Actual", lbl_temp_eau: "Temperatura", lbl_statut_pompe: "Estado de la Bomba", lbl_temps_filtration: "Tiempo de Filtración", lbl_signal_wifi: "Señal WiFi", titre_prog: "Configuración", lbl_modif_heure: "Inicio: ", lbl_modif_gmt: "Zona (GMT): ", btn_enregistrer: "Guardar", titre_cmd: "Mandos Manuales", btn_auto: "Modo Auto", btn_force_on: "Marcha Forzada (2h)", btn_force_off: "Parada Forzada", alert_save: "¡Configuración guardada!", "ALLUMÉE": "ENCENDIDA", "ÉTEINTE": "APAGADA", "AUTOMATIQUE": "AUTOMÁTICO", "MARCHE FORCADA": "MARCHA FORZADA", "ARRÊT FORCÉ": "PARADA FORZADA", "SÉCURITÉ HORS-GEL": "ANTI-HELADAS", "TEST NOCTURNE HORS-GEL (5 min)": "TEST ANTI-HELADAS", "REPRISE APRÈS COUPURE": "RECUPERACIÓN ENERGÍA ⚡", "Excellent": "Excelente 📶", "Bon": "Bueno 📡", "Moyen": "Medio ⚠️", "Faible": "Débil ❌", "Desconectado": "Desconectado", lbl_periode: "Periodo: ", "Hors-gel": "🚨 Anti-heladas", "Hivernage": "❄️ Invernada", "Saison de baignade": "🏊 Temporada de Baño", "Canicule": "🔥 Ola de Calor", titre_lum: "Iluminación", btn_lum_on: "Encender focos 💡", btn_lum_off: "Apagar focos 🌑", btn_install: "📲 Instalar aplicación", graph_titre: "Historial Temp & Filtración", lbl_graph_barres: "Filtration (h)", alert_ios: "En iPhone: Añadir a la pantalla de inicio.", alert_pwa_fail: "Añadir a la pantalla de inicio manualmente.", lbl_ip_boitier: "Dirección IP del dispositivo: ", btn_sync_ntp: "Forzar NTP 🔄", alert_ntp_ok: "¡Sincronización NTP solicitada!", alert_ntp_err: "Error en la sincronización."
            },
            pt: { titre_journal: "Registo de eventos", notice_gratuite: "PoolTimer ZR não pode ser vendido: os planos e o programa são fornecidos gratuitamente.",
                lbl_heure_actuelle: "Hora Actual", lbl_mode_actuel: "Modo Actual", lbl_temp_eau: "Temperatura", lbl_statut_pompe: "Estado da Bomba", lbl_temps_filtration: "Tempo de Filtration", lbl_signal_wifi: "Sinal WiFi", titre_prog: "Configuração", lbl_modif_heure: "Início: ", lbl_modif_gmt: "Fuso (GMT): ", btn_enregistrer: "Gravar", titre_cmd: "Comandos Manuais", btn_auto: "Modo Auto", btn_force_on: "Marcha Forçada (2h)", btn_force_off: "Paragem Forçada", alert_save: "Definições guardadas!", "ALLUMÉE": "LIGADA", "ÉTEINTE": "DESLIGADA", "AUTOMATIQUE": "AUTOMÁTICO", "MARCHE FORCADA": "MARCHA FORÇADA", "ARRÊT FORCÉ": "PARAGEM FORÇADA", "SÉCURITÉ HORS-GEL": "ANTI-GELO", "TEST NOCTURNE HORS-GEL (5 min)": "TESTE ANTI-GELO", "REPRISE APRÈS COUPURE": "RECUPERAÇÃO DE ENERGIA ⚡", "Excellent": "Excelente 📶", "Bon": "Bom 📡", "Moyen": "Médio ⚠️", "Faible": "Fraco ❌", "Déconnecté": "Desconectado", lbl_periode: "Período: ", "Hors-gel": "🚨 Anti-gelo", "Hivernage": "❄️ Invernada", "Saison de baignade": "🏊 Época de Banhos", "Canicule": "🔥 Onda de Calor", titre_lum: "Iluminação", btn_lum_on: "Ligar projetores 💡", btn_lum_off: "Desligar projetores 🌑", btn_install: "📲 Instalar aplicação", graph_titre: "Histórico Temp & Filtração", lbl_graph_barres: "Filtração (h)", alert_ios: "No iPhone: Adicionar au ecrã principal.", alert_pwa_fail: "Adicionar au ecrã principal manualmente.", lbl_ip_boitier: "Endereço IP do dispositivo: ", btn_sync_ntp: "Forçar NTP 🔄", alert_ntp_ok: "Sincronização NTP solicitada!", alert_ntp_err: "Erro na sincronização."
            },
            el: { titre_journal: "Αρχείο συμβάντων", notice_gratuite: "Το PoolTimer ZR δεν επιτρέπεται να πωλείται: τα σχέδια και το πρόγραμμα παρέχονται δωρεάν.",
                lbl_heure_actuelle: "Τρέχουσα Ώρα", lbl_mode_actuel: "Λειτουργία", lbl_temp_eau: "Θερμοκρασία", lbl_statut_pompe: "Αντλία", lbl_temps_filtration: "Φιλτράρισμα", lbl_signal_wifi: "Σήμα WiFi", titre_prog: "Ρύθμιση", lbl_modif_heure: "Έναρξη: ", lbl_modif_gmt: "Ζώνη (GMT): ", btn_enregistrer: "Αποθήκευση", titre_cmd: "Χειροκίνητες Εντολές", btn_auto: "Αυτόματο", btn_force_on: "Αναγκαστική (2ω)", btn_force_off: "Διακοπή", alert_save: "Αποθηκεύτηκε!", "ALLUMÉE": "ΑΝΑΜΜΕΝΗ", "ÉTEINTE": "ΣΒΗΣΤΗ", "AUTOMATIQUE": "ΑΥΤΟΜΑΤΟ", "MARCHE FORCÉE": "ΑΝΑΓΚΑΣΤΙΚΗ", "ARRÊT FORCÉ": "ΔΙΑΚΟΠΗ", "SÉCURITÉ HORS-GEL": "ΠΡΟΣΤΑΣΙΑ ΠΑΓΕΤΟΥ", "TEST NOCTURNE HORS-GEL (5 min)": "ΕΛΕΓΧΟΣ ΠΑΓΕΤΟΥ", "REPRISE APRÈS COUPURE": "ΕΠΑΝΑΦΟΡΑ ΡΕΥΜΑΤΟΣ ⚡", "Excellent": "Εξαιρετικό 📶", "Bon": "Καλό 📡", "Moyen": "Μέτριο ⚠️", "Faible": "Ασθενές ❌", "Déconnecté": "Αποσυνδέθηκε", lbl_periode: "Περιφέρεια: ", "Hors-gel": "🚨 Παγετός", "Hivernage": "❄️ Χειμερινή", "Saison de baignade": "🏊 Κολύμβηση", "Canicule": "🔥 Καύσωνας", titre_lum: "Φωτισμός", btn_lum_on: "Φώτα ON 💡", btn_lum_off: "Φώτα OFF 🌑", btn_install: "📲 Εγκατάσταση", graph_titre: "Ιστορικό Θερμ & Φιλτραρίσματος", lbl_graph_barres: "Φιλτράρισμα (h)", alert_ios: "Σε iPhone: Προσθήκη στην οθόνη αφετηρίας.", alert_pwa_fail: "Προσθήκη στην οθόνη αφετηρίας χειροκίνητα.", lbl_ip_boitier: "Διεύθυνση IP συσκευής: ", btn_sync_ntp: "Συγχρονισμός NTP 🔄", alert_ntp_ok: "Ζητήθηκε συγχρονισμός NTP!", alert_ntp_err: "Σφάλμα συγχρονισμού."
            }
        };

        if ('serviceWorker' in navigator) { navigator.serviceWorker.register('/sw.js'); }
        window.addEventListener('beforeinstallprompt', (e) => { e.preventDefault(); deferredPrompt = e; });

        function executerInstallation() {
            const isIos = /iPad|iPhone|iPod/.test(navigator.userAgent) && !window.MSStream;
            const lang = dictionnaire[langueActuelle];
            if (isIos) { alert(lang.alert_ios); return; }
            if (deferredPrompt) {
                deferredPrompt.prompt();
                deferredPrompt.userChoice.then(() => { deferredPrompt = null; });
            } else { alert(lang.alert_pwa_fail); }
        }

        function basculerLangue() {
            let indexCourant = ordreLangues.indexOf(langueActuelle);
            let prochainIndex = (indexCourant + 1) % ordreLangues.length;
            langueActuelle = ordreLangues[prochainIndex];
            document.getElementById('btn-lang-id').innerText = drapeauxLangues[langueActuelle] || '🇫🇷 FR';
            traduireInterface();
        }

        function traduireInterface() {
            const lang = dictionnaire[langueActuelle];
            document.getElementById('lbl_heure_actuelle').innerText = lang.lbl_heure_actuelle;
            document.getElementById('lbl_mode_actuel').innerText = lang.lbl_mode_actuel;
            document.getElementById('lbl_temp_eau').innerText = lang.lbl_temp_eau;
            document.getElementById('lbl_statut_pompe').innerText = lang.lbl_statut_pompe;
            document.getElementById('lbl_temps_filtration').innerText = lang.lbl_temps_filtration;
            document.getElementById('lbl_signal_wifi').innerText = lang.lbl_signal_wifi;
            document.getElementById('titre_prog').innerText = lang.titre_prog;
            document.getElementById('titre_journal').innerText = lang.titre_journal;
            actualiserBoutonJournal();
            document.getElementById('notice_gratuite').innerText = lang.notice_gratuite;
            document.getElementById('lbl_modif_heure').innerText = lang.lbl_modif_heure;
            document.getElementById('lbl_modif_gmt').innerText = lang.lbl_modif_gmt;
            document.getElementById('btn_enregistrer').innerText = lang.btn_enregistrer;
            document.getElementById('titre_cmd').innerText = lang.titre_cmd;
            document.getElementById('btn_auto').innerText = lang.btn_auto;
            document.getElementById('btn_force_on').innerText = lang.btn_force_on;
            document.getElementById('btn_force_off').innerText = lang.btn_force_off;
            document.getElementById('statut').innerText = lang[dernierStatutBrut] || dernierStatutBrut;
            document.getElementById('mode').innerText = lang[dernierModeBrut] || dernierModeBrut;
            document.getElementById('btn_force_off').disabled = (dernierModeBrut !== 'MARCHE FORCÉE');
            document.getElementById('lbl_periode_id').innerText = lang.lbl_periode;
            document.getElementById('titre_lum').innerText = lang.titre_lum;
            document.getElementById('graph_titre').innerText = lang.graph_titre;
            document.getElementById('lbl_ip_boitier_id').innerText = lang.lbl_ip_boitier;
            document.getElementById('btn_ntp').innerText = lang.btn_sync_ntp;
            
            if (monGraphique) {
                monGraphique.data.datasets[0].label = lang.lbl_temp_eau;
                monGraphique.data.datasets[1].label = lang.lbl_graph_barres;
                monGraphique.update();
            }
            majBoutonLumiere();
        }

        function majBoutonLumiere() {
            const lang = dictionnaire[langueActuelle];
            const btn = document.getElementById('btn_lumiere');
            if (lumiereAllumee) { btn.innerText = lang.btn_lum_off; btn.className = "btn btn-red"; } 
            else { btn.innerText = lang.btn_lum_on; btn.className = "btn btn-green"; }
        }

        function libelleGraphique(horodatage) {
            if (typeof horodatage !== 'string') return '';
            const m = horodatage.match(/^(\d{2}\/\d{2})\s+(\d{2})/);
            return m ? m[1] + ' ' + m[2] + 'h' : horodatage;
        }

        function initialiserGraphique(heures, valeurs, jours, filtration) {
            const ctx = document.getElementById('chartTemp').getContext('2d');
            const lang = dictionnaire[langueActuelle];
            
            monGraphique = new Chart(ctx, {
                data: {
                    labels: heures, 
                    datasets: [
                        {
                            type: 'line',
                            label: lang.lbl_temp_eau,
                            data: valeurs,
                            borderColor: '#0288d1',
                            backgroundColor: 'rgba(2, 136, 209, 0.05)',
                            borderWidth: 2,
                            tension: 0.35,
                            pointRadius: 0,
                            pointHoverRadius: 0,
                            pointHitRadius: 8,
                            fill: true,
                            yAxisID: 'yTemp'
                        },
                        {
                            type: 'bar',
                            label: lang.lbl_graph_barres,
                            data: filtration.map((val, i) => ({ x: jours[i], y: val })), 
                            backgroundColor: 'rgba(76, 175, 80, 0.4)',
                            borderColor: '#4caf50',
                            borderWidth: 1,
                            barPercentage: 0.5,
                            yAxisID: 'yFilt'
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    scales: {
                        x: {
                            grid: { display: false },
                            ticks: {
                                autoSkip: true,
                                maxTicksLimit: 6,
                                callback: function(value, index) {
                                    const labels = this.chart.data.labels || [];
                                    const courant = libelleGraphique(labels[index]);
                                    if (index > 0 && courant === libelleGraphique(labels[index - 1])) return '';
                                    return courant;
                                },
                                maxRotation: 45,
                                minRotation: 0
                            }
                        },
                        yTemp: {
                            type: 'linear',
                            position: 'left',
                            title: { display: true, text: '°C', color: '#0288d1' },
                            ticks: { color: '#0288d1' }
                        },
                        yFilt: {
                            type: 'linear',
                            position: 'right',
                            title: { display: true, text: 'Heures', color: '#4caf50' },
                            min: 0,
                            max: 24,
                            grid: { display: false },
                            ticks: { color: '#4caf50' }
                        }
                    }
                }
            });
        }

        function actualiserBoutonJournal() {
            const bouton = document.getElementById('btn_journal');
            const ouvert = !document.getElementById('journal_contenu').hidden;
            const textes = {
                fr: ['📋 Afficher le journal des événements', '✖ Masquer le journal des événements'],
                en: ['📋 Show event log', '✖ Hide event log'],
                it: ['📋 Mostra registro eventi', '✖ Nascondi registro eventi'],
                de: ['📋 Ereignisprotokoll anzeigen', '✖ Ereignisprotokoll ausblenden'],
                es: ['📋 Mostrar registro de eventos', '✖ Ocultar registro de eventos'],
                pt: ['📋 Mostrar registo de eventos', '✖ Ocultar registo de eventos'],
                el: ['📋 Εμφάνιση αρχείου συμβάντων', '✖ Απόκρυψη αρχείου συμβάντων']
            };
            bouton.textContent = (textes[langueActuelle] || textes.fr)[ouvert ? 1 : 0];
            bouton.setAttribute('aria-expanded', String(ouvert));
        }

        function basculerJournal() {
            const contenu = document.getElementById('journal_contenu');
            contenu.hidden = !contenu.hidden;
            actualiserBoutonJournal();
            if (!contenu.hidden) chargerJournal();
        }

        function chargerJournal() {
            if (document.getElementById('journal_contenu').hidden) return;
            fetch('/events').then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
            .then(data => {
                const liste = document.getElementById('journal_liste');
                liste.replaceChildren();
                const evenements = Array.isArray(data.events) ? data.events : [];
                if (!evenements.length) { liste.textContent = 'Aucun événement enregistré.'; return; }
                evenements.slice().reverse().forEach(evt => {
                    const ligne = document.createElement('div');
                    ligne.className = 'journal-ligne';
                    ligne.textContent = String(evt);
                    liste.appendChild(ligne);
                });
            }).catch(() => { document.getElementById('journal_liste').textContent = 'Journal indisponible.'; });
        }

        function chargerHistoriqueEtInit() {
            fetch('/graph-data').then(res => res.json()).then(data => {
                initialiserGraphique(data.heures, data.valeurs, data.jours, data.filtration);
            });
        }

        setInterval(function() {
            fetch('/data').then(response => response.json()).then(data => {
                dernierStatutBrut = data.statut; dernierModeBrut = data.mode; lumiereAllumee = data.lumiere;
                document.getElementById('heure').innerText = data.heure;
                document.getElementById('temp_eau').innerText = data.temp_eau + " °C";
                document.getElementById('duree').innerText = data.duree + " h";
                document.getElementById('ip_adresse').innerText = data.ip_esp; 
                
                const lang = dictionnaire[langueActuelle];
                let texteQualite = lang[data.wifi_txt] || data.wifi_txt;
                document.getElementById('wifi_signal').innerText = data.wifi_pct + "% (" + texteQualite + ")";
                document.getElementById('saison_val').innerText = lang[data.saison_txt] || data.saison_txt;
                document.getElementById('txt_heure_dem_val').innerText = data.heure_prog + " h";
                document.getElementById('txt_gmt_val').innerText = (data.gmt_effectif >= 0 ? "+" : "") + data.gmt_effectif + (data.heure_ete ? " (été)" : " (hiver)");
                document.getElementById('statut').innerText = lang[dernierStatutBrut] || dernierStatutBrut;
                document.getElementById('mode').innerText = lang[dernierModeBrut] || dernierModeBrut;
                document.getElementById('btn_force_off').disabled = (dernierModeBrut !== 'MARCHE FORCÉE');
                majBoutonLumiere();

                fetch('/graph-data').then(res => res.json()).then(gData => {
                    if (monGraphique) {
                        monGraphique.data.labels = gData.heures;
                        monGraphique.data.datasets[0].data = gData.valeurs;
                        monGraphique.data.datasets[1].data = gData.filtration.map((val, i) => ({ x: gData.jours[i], y: val }));
                        monGraphique.update('none'); 
                    }
                });
            });
        }, 3000);

        function changerMode(nouveauMode) {
            fetch('/set-mode?mode=' + nouveauMode).then(r => { if (!r.ok) return r.text().then(t => { throw new Error(t); }); }).then(() => {}).catch(e => alert(e.message));
        }
        function basculerLumiere() { fetch('/toggle-light'); }
        function enregistrerConfig() {
            var heureSelectionnee = document.getElementById('select_heure').value;
            var gmtSelectionne = document.getElementById('select_gmt').value;
            fetch('/set-config?h=' + heureSelectionnee + '&gmt=' + gmtSelectionne).then(response => {
                if(response.ok) { alert((dictionnaire[langueActuelle]).alert_save); }
            });
        }
        function forcerNTP() {
            const lang = dictionnaire[langueActuelle];
            fetch('/sync-ntp').then(response => {
                if(response.ok) { alert(lang.alert_ntp_ok); } 
                else { alert(lang.alert_ntp_err); }
            });
        }
        
        window.onload = function() {
            let langueNavigateur = (navigator.language || navigator.userLanguage).substring(0, 2).toLowerCase();
            if (ordreLangues.includes(langueNavigateur)) {
                langueActuelle = langueNavigateur;
            } else {
                langueActuelle = 'en';
            }
            document.getElementById('btn-lang-id').innerText = drapeauxLangues[langueActuelle] || '🇫🇷 FR';
            traduireInterface();
            chargerHistoriqueEtInit();
            setInterval(() => { if (!document.getElementById('journal_contenu').hidden) chargerJournal(); }, 10000);
        };
    </script>
</head>
<body>
    <div class="container">
        <div class="lang-container"><button id="btn-lang-id" class="btn-lang" onclick="basculerLangue()">🇬🇧 EN</button></div>
        <div class="logo-container"><img class="logo-img" src="https://lh3.googleusercontent.com/d/1Bxb00531-MNfYPLHYNCvubKgXI_dP7go" alt="PoolTimer ZR Logo"></div>
        <h1>PoolTimer ZR V 2.0</h1>
        <div class="grid">
            <div class="card"><h3 id="lbl_heure_actuelle">Heure Actuelle</h3><div id="heure" class="valeur">--:--:--</div></div>
            <div class="card">
                <h3 id="lbl_mode_actuel">Mode Actuel</h3>
                <div id="mode" class="valeur">--</div>
                <div class="periode-texte"><span id="lbl_periode_id">Période : </span><span id="saison_val" style="font-weight:bold;">--</span></div>
            </div>
            <div class="card card-full"><h3 id="lbl_temp_eau">Température de l'Eau</h3><div id="temp_eau" class="valeur">-- °C</div></div>
            <div class="card"><h3 id="lbl_statut_pompe">État Pompe</h3><div id="statut" class="valeur">--</div></div>
            <div class="card"><h3 id="lbl_temps_filtration">Temps de Filtration</h3><div id="duree" class="valeur">-- h</div></div>
            <div class="card card-purple"><h3 id="lbl_signal_wifi">Signal WiFi</h3><div id="wifi_signal" class="valeur valeur-purple">-- %</div></div>
        </div>

        <div class="graph-box">
            <h3 id="graph_titre" style="margin: 5px 0 10px 0; font-size: 0.9em; color: #5a738e; text-transform: uppercase;">Suivi Température & Filtration</h3>
            <div style="height: 240px; position: relative;">
                <canvas id="chartTemp"></canvas>
            </div>
        </div>

        <div class="section">
            <h2 id="titre_lum">Éclairage Piscine</h2>
            <button id="btn_lumiere" class="btn btn-green" onclick="basculerLumiere()">Allumer les projecteurs 💡</button>
        </div>

        <div class="section">
            <h2 id="titre_cmd">Commandes Manuelles (Marche Forcée)</h2>
            <button id="btn_auto" class="btn btn-blue" onclick="changerMode('AUTO')">Mode Auto</button>
            <button id="btn_force_on" class="btn btn-green" onclick="changerMode('FORCE_ON')">Marche Forcée (2h)</button>
            <button id="btn_force_off" class="btn btn-red" onclick="changerMode('FORCE_OFF')">Arrêt Forcé</button>
        </div>

        <div class="section">
            <h2 id="titre_prog">Configuration Système</h2>
            <p>
                <span id="txt_heure_dem_lbl">Start : </span><span id="txt_heure_dem_val" style="font-weight:bold; color:#0288d1;">--</span> | 
                <span id="txt_gmt_lbl">GMT : </span><span id="txt_gmt_val" style="font-weight:bold; color:#0288d1;">--</span>
            </p>
            <div class="row-control">
                <label id="lbl_modif_heure" for="select_heure">Démarrage : </label>
                <select id="select_heure">
                    <script>for(var i=0; i<24; i++) { document.write('<option value="' + i + '">' + i + 'h</option>'); }</script>
                </select>
                <label id="lbl_modif_gmt" for="select_gmt">Fuseau (GMT) : </label>
                <select id="select_gmt" class="select-gmt">
                    <script>for(var i=-11; i<=12; i++) { var sign = i >= 0 ? "+" : ""; document.write('<option value="' + i + '">GMT ' + sign + i + '</option>'); }</script>
                </select>
            </div>
            <button id="btn_enregistrer" class="btn btn-blue" onclick="enregistrerConfig()">Enregistrer</button>
            <div style="margin-top: 15px;">
                <button id="btn_ntp" class="btn btn-blue" onclick="forcerNTP()">Forcer la synchro NTP 🔄</button>
            </div>
        </div>

        <div class="section">
            <h2 id="titre_journal">Journal des événements</h2>
            <button type="button" id="btn_journal" class="btn btn-blue" onclick="basculerJournal()" aria-expanded="false" aria-controls="journal_contenu">📋 Afficher le journal des événements</button>
            <div id="journal_contenu" hidden style="margin-top: 12px;">
                <div id="journal_liste" class="journal-liste" aria-live="polite">Chargement...</div>
            </div>
        </div>
        <div class="notice-gratuite" id="notice_gratuite">PoolTimer ZR est un projet libre et gratuit, conçu dans un esprit de partage. Ses plans et son programme sont mis gratuitement à disposition de tous. Toute commercialisation du PoolTimer ZR est interdite.</div>
        
        <div style="margin-top: 25px; font-size: 0.8em; color: #a0aec0;">
            <span id="lbl_ip_boitier_id">Adresse IP du boîtier : </span><span id="ip_adresse" style="font-weight: bold;">--.--.--.--</span>
        </div>
    </div>
</body>
</html>
)rawliteral";

void saveParamsCallback () {
  Serial.println("WiFiManager settings saved!");
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);
  pinMode(LED_WIFI, OUTPUT);
  digitalWrite(LED_WIFI, LOW);
  
  sensors.begin();
  chargerHistoriquesFlash();
  chargerJournal();

  preferences.begin("piscine", false); 
  heureDemarrage = preferences.getInt("h_start", 8); 
  tempEauActuelle = preferences.getFloat("t_eau", 20.0);
  temperatureAffichee = preferences.getFloat("t_aff", tempEauActuelle);
  temperatureAfficheeValide = isfinite(temperatureAffichee) && temperatureAffichee > -20.0f && temperatureAffichee < 60.0f;
  if (!temperatureAfficheeValide) temperatureAffichee = tempEauActuelle;
  modeHivernage = tempEauActuelle < 12.0f;
  int gmtChoisi = preferences.getInt("gmt_offset", 1);
  gmtHiver = constrain(gmtChoisi, -11, 12); 
  unsigned long msRestantes = preferences.getULong("ms_restantes", 0);
  preferences.end();

  if (msRestantes > 0) {
    filtrationEnCours = true;
    repriseApresCoupure = true;
    debutFiltrationMs = millis();
    dureeTotaleFiltrationMs = msRestantes;
    dureeAfficheeInterface = (float)dureeTotaleFiltrationMs / 3600000.0;
    digitalWrite(RELAY_PIN, HIGH);
  } else {
    dureeAfficheeInterface = calculerTempsFiltration(tempEauActuelle);
  }

  char txt_heure_demarrage[6];
  sprintf(txt_heure_demarrage, "%d", heureDemarrage);
  WiFiManagerParameter custom_heure_demarrage("h_start", "Default start hour (0-23)", txt_heure_demarrage, 6);
  wm.addParameter(&custom_heure_demarrage);

  char txt_gmt[6];
  sprintf(txt_gmt, "%d", gmtChoisi);
  WiFiManagerParameter custom_gmt_offset("gmt_offset", "Timezone / GMT offset", txt_gmt, 6);
  wm.addParameter(&custom_gmt_offset);

  wm.setSaveParamsCallback(saveParamsCallback);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  if(!wm.autoConnect("Pool_Timer_CONFIG")) {
      ESP.restart();
  }

  preferences.begin("piscine", false);
  int h_saisi = atoi(custom_heure_demarrage.getValue());
  if (h_saisi >= 0 && h_saisi <= 23) {
    heureDemarrage = h_saisi;
    preferences.putInt("h_start", heureDemarrage);
  }
  int gmt_saisi = atoi(custom_gmt_offset.getValue());
  if (gmt_saisi >= -11 && gmt_saisi <= 12) {
    gmtChoisi = gmt_saisi;
    gmtHiver = gmtChoisi;
    preferences.putInt("gmt_offset", gmtChoisi);
  }
  preferences.end();

  wifiEstConnecte = (WiFi.status() == WL_CONNECTED); 
  synchroniserHorloge();

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  Serial.print("ESP32 Connecté avec succès ! Adresse IP : ");
  Serial.println(WiFi.localIP());

  relancerMDNS();
  
  server.on("/graph-data", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc;
    JsonArray heuresArr = doc["heures"].to<JsonArray>();
    JsonArray valeursArr = doc["valeurs"].to<JsonArray>();
    JsonArray joursArr = doc["jours"].to<JsonArray>();
    JsonArray filtrationArr = doc["filtration"].to<JsonArray>();
    
    for (int i = 0; i < nbPointsActuels; i++) {
      int pos = (indexHistorique - nbPointsActuels + i + NB_POINTS_HISTORIQUE) % NB_POINTS_HISTORIQUE;
      heuresArr.add(historiqueHeures[pos]);
      valeursArr.add(String(historiqueTemps[pos], 1));
    }
    
    for (int i = 0; i < nbJoursActuels; i++) {
      int pos = (indexJours - nbJoursActuels + i + NB_JOURS_HISTORIQUE) % NB_JOURS_HISTORIQUE;
      joursArr.add(historiqueJours[pos]);
      filtrationArr.add(String(historiqueFiltration[pos], 1));
    }
    
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", index_html);
  });

  server.on("/manifest.json", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", manifest_json);
  });

  server.on("/sw.js", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/javascript", sw_js);
  });

  server.on("/sync-ntp", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    synchroniserHorloge();
    Serial.println("Synchronisation NTP et fuseau horaire France forcés.");
    request->send(200, "text/plain", "OK");
  });

  server.on("/events", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc;
    doc["sequence"] = sequenceEvenement;
    JsonArray arr = doc["events"].to<JsonArray>();
    for (int i = 0; i < nombreEvenements; i++) {
      int index = (prochainEvenement - nombreEvenements + i + MAX_EVENEMENTS) % MAX_EVENEMENTS;
      arr.add(journalEvenements[index]);
    }
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/data", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc;
    doc["heure"] = getHeureComplete();
    doc["temp_eau"] = String(temperatureAffichee, 1);
    doc["defaut_sonde"] = defautSonde;
    doc["mode_securite"] = defautSonde;
    doc["event_seq"] = sequenceEvenement;
    doc["statut"] = digitalRead(RELAY_PIN) ? "ALLUMÉE" : "ÉTEINTE";
    doc["lumiere"] = digitalRead(LIGHT_PIN) ? true : false;
    doc["heure_prog"] = heureDemarrage; 
    
    struct tm timeinfo;
    bool heureValide = getLocalTime(&timeinfo, 100);
    doc["gmt_offset"] = gmtHiver; 
    doc["gmt_effectif"] = gmtHiver + ((heureValide && timeinfo.tm_isdst > 0) ? 1 : 0);
    doc["heure_ete"] = heureValide && timeinfo.tm_isdst > 0;

    doc["ip_esp"] = WiFi.localIP().toString(); 

    int pctWifi = getQualiteWifi();
    doc["wifi_pct"] = pctWifi;
    if (pctWifi >= 80) doc["wifi_txt"] = "Excellent";
    else if (pctWifi >= 60) doc["wifi_txt"] = "Bon";
    else if (pctWifi >= 40) doc["wifi_txt"] = "Moyen";
    else if (pctWifi > 0) doc["wifi_txt"] = "Faible";
    else doc["wifi_txt"] = "Déconnecté";

    if (defautSonde) doc["saison_txt"] = "Defaut sonde - continu";
    else if (modeActuel == MODE_HORS_GEL || tempEauActuelle <= 3.0) doc["saison_txt"] = "Hors-gel";
    else if (modeHivernage) doc["saison_txt"] = "Hivernage";
    else if (tempEauActuelle >= 27.0) doc["saison_txt"] = "Canicule";
    else doc["saison_txt"] = "Saison de baignade";

    if(modeActuel == MODE_AUTO) {
      if (defautSonde) doc["mode"] = "SECURITE CONTINUE (SONDE)";
      else if (repriseApresCoupure) doc["mode"] = "REPRISE APRÈS COUPURE";
      else if (cycleControleNocturneEnCours) doc["mode"] = "TEST NOCTURNE HORS-GEL (5 min)";
      else doc["mode"] = "AUTOMATIQUE";
    }
    else if(modeActuel == MODE_FORCE_ON) doc["mode"] = "MARCHE FORCÉE";
    else if(modeActuel == MODE_FORCE_OFF) doc["mode"] = "ARRÊT FORCÉ";
    else if(modeActuel == MODE_HORS_GEL) doc["mode"] = "SÉCURITÉ HORS-GEL";

    doc["duree"] = String(dureeAfficheeInterface, 1);
    
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/set-mode", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("mode")) {
      request->send(400, "text/plain", "Mode manquant");
      return;
    }
    String m = request->getParam("mode")->value();
    if (defautSonde || modeActuel == MODE_HORS_GEL || tempEauActuelle <= 3.0f) {
      request->send(409, "text/plain", "Protection active (Défaut sonde ou Hors-gel)");
      return;
    }
    if (m == "FORCE_ON") {
      filtrationEnCours = false;
      cycleControleNocturneEnCours = false;
      repriseApresCoupure = false;
      dureeTotaleFiltrationMs = 0;
      preferences.begin("piscine", false);
      preferences.putULong("ms_restantes", 0);
      preferences.end();
      modeActuel = MODE_FORCE_ON;
      journaliser("MARCHE_FORCEE_DEBUT");
      debutMarcheForceeMs = millis();
      digitalWrite(RELAY_PIN, HIGH);
    } else if (m == "FORCE_OFF") {
      if (modeActuel != MODE_FORCE_ON) {
        request->send(409, "text/plain", "Arret disponible uniquement en marche forcee");
        return;
      }
      modeActuel = MODE_AUTO;
      journaliser("MARCHE_FORCEE_ARRETEE");
      debutMarcheForceeMs = 0;
      digitalWrite(RELAY_PIN, LOW);
      dureeAfficheeInterface = 0.0f;
    } else if (m == "AUTO") {
      if (modeActuel == MODE_FORCE_ON) {
        journaliser("MARCHE_FORCEE_ANNULEE");
        digitalWrite(RELAY_PIN, LOW);
        debutMarcheForceeMs = 0;
      }
      modeActuel = MODE_AUTO;
    } else {
      request->send(400, "text/plain", "Mode inconnu");
      return;
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/toggle-light", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    digitalWrite(LIGHT_PIN, !digitalRead(LIGHT_PIN));
    request->send(200, "text/plain", "OK");
  });

  server.on("/set-config", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request){
    bool modif = false;
    preferences.begin("piscine", false);
    if (request->hasParam("h")) {
      int h = request->getParam("h")->value().toInt();
      if (h >= 0 && h <= 23) { heureDemarrage = h; preferences.putInt("h_start", heureDemarrage); modif = true; }
    }
    if (request->hasParam("gmt")) {
      int gmt = request->getParam("gmt")->value().toInt();
      if (gmt >= -11 && gmt <= 12) { gmtHiver = gmt; preferences.putInt("gmt_offset", gmt); modif = true; }
    }
    preferences.end();
    if(modif) { 
      synchroniserHorloge();
    }
    request->send(200, "text/plain", "OK");
  });

  server.begin();
}

void loop() {
  static unsigned long derniereTentativeConnexion = 0;
  const unsigned long intervalleReconnexion = 15000;
  bool connecte = (WiFi.status() == WL_CONNECTED);
  if (!connecte) {
    if (wifiEstConnecte) Serial.println("WiFi perdu : reconnexion en cours");
    wifiEstConnecte = false;
    digitalWrite(LED_WIFI, LOW);
    if (derniereTentativeConnexion == 0 ||
        millis() - derniereTentativeConnexion >= intervalleReconnexion) {
      derniereTentativeConnexion = millis();
      WiFi.reconnect();
    }
  } else if (!wifiEstConnecte) {
    wifiEstConnecte = true;
    derniereTentativeConnexion = 0;
    Serial.println("WiFi reconnecte : " + WiFi.localIP().toString());
    synchroniserHorloge();
    relancerMDNS();
  }

  if (wifiEstConnecte) {
    unsigned long actuelMillis = millis();
    if (actuelMillis - precedentMillisLed >= intervalleLed) {
      precedentMillisLed = actuelMillis;
      etatLed = !etatLed;
      digitalWrite(LED_WIFI, etatLed);
    }
  }

  if (digitalRead(RELAY_PIN) == HIGH) {
    if (dernierCalculCumulMs == 0) dernierCalculCumulMs = millis();
    cumulFiltrationJourMs += (millis() - dernierCalculCumulMs);
    dernierCalculCumulMs = millis();
  } else {
    dernierCalculCumulMs = 0;
  }

  if (filtrationEnCours && (millis() - precedentMillisSauvegarde >= 30000)) {
    precedentMillisSauvegarde = millis();
    unsigned long tempsEcoule = millis() - debutFiltrationMs;
    if (dureeTotaleFiltrationMs > tempsEcoule) {
      unsigned long msRestantes = dureeTotaleFiltrationMs - tempsEcoule;
      preferences.begin("piscine", false);
      preferences.putULong("ms_restantes", msRestantes);
      preferences.end();
    }
  }

  int heureActuelle = getHeureActuelle();
  int minuteActuelle = getMinuteActuelle();
  int jourActuel = getJourActuel();
  if (heureActuelle == -1 && !repriseApresCoupure) { return; } 

  static String dateCumulFiltration = "";
  if (jourActuel != -1 && dernierJourEnregistre == -1) {
    dateCumulFiltration = getJourShort();
    if (nbJoursActuels > 0) {
      int precedent = (indexJours - 1 + NB_JOURS_HISTORIQUE) % NB_JOURS_HISTORIQUE;
      if (historiqueJours[precedent] == dateCumulFiltration) {
        cumulFiltrationJourMs = (unsigned long)(historiqueFiltration[precedent] * 3600000.0f + 0.5f);
      }
    }
    dernierJourEnregistre = jourActuel;
  } else if (jourActuel != -1 && jourActuel != dernierJourEnregistre) {
    if (dateCumulFiltration.length()) {
      ajouterFiltrationAuGraphique((float)cumulFiltrationJourMs / 3600000.0f, dateCumulFiltration);
      sauvegarderFiltration();
    }
    cumulFiltrationJourMs = 0;
    dernierCalculCumulMs = millis();
    dernierJourEnregistre = jourActuel;
    dateCumulFiltration = getJourShort();
  }

  if (jourActuel != -1) {
    ajouterFiltrationAuGraphique((float)cumulFiltrationJourMs / 3600000.0f, dateCumulFiltration);
  }

  static bool pompePrecedemmentActivePourSauvegarde = false;
  bool pompeActivePourSauvegarde = (digitalRead(RELAY_PIN) == HIGH);
  if (historiqueFiltrationModifie &&
      ((pompePrecedemmentActivePourSauvegarde && !pompeActivePourSauvegarde) ||
       millis() - dernierEnregistrementFiltrationMs >= INTERVALLE_SAUVEGARDE_FILTRATION)) {
    sauvegarderFiltration();
  }
  pompePrecedemmentActivePourSauvegarde = pompeActivePourSauvegarde;

  static bool pompeEtaitEnMarche = false;
  static unsigned long debutCirculationMs = 0;
  static bool premierReleveApresDemarrage = false;
  static unsigned long derniereTentativeGraphMs = 0;
  static unsigned long dernierDiagnosticGraphMs = 0;
  bool pompeEnMarche = (digitalRead(RELAY_PIN) == HIGH);
  unsigned long maintenantGraph = millis();

  if (pompeEnMarche && !pompeEtaitEnMarche) {
    debutCirculationMs = maintenantGraph;
    premierReleveApresDemarrage = false;
    dernierReleveGraphMs = 0;
    derniereTentativeGraphMs = 0;
  }
  if (!pompeEnMarche && pompeEtaitEnMarche && temperatureAfficheeValide) {
    preferences.begin("piscine", false);
    preferences.putFloat("t_aff", temperatureAffichee);
    preferences.end();
  }
  if (!pompeEnMarche) {
    premierReleveApresDemarrage = false;
    dernierReleveGraphMs = 0;
  } else if (maintenantGraph - debutCirculationMs >= DELAI_STABILISATION_EAU &&
             (!premierReleveApresDemarrage ||
              maintenantGraph - dernierReleveGraphMs >= INTERVALLE_RELEVE_GRAPH) &&
             (derniereTentativeGraphMs == 0 ||
              maintenantGraph - derniereTentativeGraphMs >= 10000UL)) {
    derniereTentativeGraphMs = maintenantGraph;
    acquerirTemperature();
    struct tm dateHistorique;
    bool heureValide = getLocalTime(&dateHistorique, 10) && dateHistorique.tm_year >= 124;
    bool mesureValide = !defautSonde && dernierEchantillonValideMs != 0 &&
                        millis() - dernierEchantillonValideMs < 5000UL &&
                        isfinite(tempEauActuelle);
    if (heureValide && mesureValide) {
      temperatureAffichee = tempEauActuelle;
      temperatureAfficheeValide = true;
      ajouterAuGraphique(temperatureAffichee, getHeureJourShort());
      dernierReleveGraphMs = millis();
      premierReleveApresDemarrage = true;
    }
  }
  pompeEtaitEnMarche = pompeEnMarche;

  // --- ACQUISITION CONTINUE 24h/24 DE LA TEMPERATURE ---
  static unsigned long precedentMillisCapteur = 0;
  if (millis() - precedentMillisCapteur >= 2000) { 
    precedentMillisCapteur = millis();
    acquerirTemperature(); 
    
    if (pompeEnMarche && !defautSonde && dernierEchantillonValideMs != 0 &&
        millis() - dernierEchantillonValideMs < 5000UL) {
      temperatureAffichee = tempEauActuelle;
      temperatureAfficheeValide = true;
    }
  }

  // --- GESTION HORS-GEL 24h/24 ET TEMPORISATION DE SORTIE (1h après > 4°C) ---
  if (!defautSonde) {
    if (tempEauActuelle <= 3.0f) {
      if (modeActuel != MODE_HORS_GEL) {
        filtrationEnCours = false;
        cycleControleNocturneEnCours = false;
        repriseApresCoupure = false;
        debutMarcheForceeMs = 0;
        dureeTotaleFiltrationMs = 0;
        preferences.begin("piscine", false);
        preferences.putULong("ms_restantes", 0);
        preferences.end();
        journaliser("HORS_GEL_ACTIVE_24H24");
      }
      modeActuel = MODE_HORS_GEL;
      temporisationHorsGelEnCours = false; 
      digitalWrite(RELAY_PIN, HIGH);
    } 
    else if (modeActuel == MODE_HORS_GEL) {
      if (tempEauActuelle > 4.0f) {
        if (!temporisationHorsGelEnCours) {
          temporisationHorsGelEnCours = true;
          debutDepassement4C_Ms = millis();
          journaliser("HORS_GEL_DEPASSEMENT_4C_TEMPO_1H_DEBUT");
        }
      }

      if (temporisationHorsGelEnCours) {
        if (millis() - debutDepassement4C_Ms >= TEMPO_SORTIE_HORS_GEL) {
          journaliser("HORS_GEL_TERMINE_APRES_TEMPO_1H");
          modeActuel = MODE_AUTO;
          temporisationHorsGelEnCours = false;
          digitalWrite(RELAY_PIN, LOW);
        } else {
          digitalWrite(RELAY_PIN, HIGH);
        }
      } else {
        digitalWrite(RELAY_PIN, HIGH);
      }
    }
  }

  // --- MODIFICATION : DEFAUT SONDE EN MARCHE CONTINUE 24h/24 ---
  if (defautSonde) {
    digitalWrite(RELAY_PIN, HIGH); // La pompe tourne en continu
    dureeAfficheeInterface = 24.0f; // Indique 24h en marche sur l'interface
    return; 
  }

  if (heureActuelle == 0) { controleNocturneFaitAujourdhui = false; }
  struct tm dateCycle;
  bool dateCycleValide = getLocalTime(&dateCycle, 10);
  bool cycleSaisonnierDejaLance = dateCycleValide &&
      dernierJourCycleSaisonnier == dateCycle.tm_yday &&
      derniereAnneeCycleSaisonnier == dateCycle.tm_year;
  if (dateCycleValide && (jourRepriseSaison != dateCycle.tm_yday || anneeRepriseSaison != dateCycle.tm_year)) {
    repriseSaisonConfirmee = false;
  }
  if (tempEauActuelle > 5.0 && !cycleControleNocturneEnCours) {
    dernierControleHiverMs = 0;
  }

  switch (modeActuel) {
    case MODE_FORCE_ON:
      if (millis() - debutMarcheForceeMs >= DUREE_MARCHE_FORCEE) {
        modeActuel = MODE_AUTO;
        journaliser("MARCHE_FORCEE_2H_TERMINEE");
        debutMarcheForceeMs = 0;
        digitalWrite(RELAY_PIN, LOW);
        dureeAfficheeInterface = 0.0f;
      } else {
        digitalWrite(RELAY_PIN, HIGH);
        dureeAfficheeInterface = (float)(DUREE_MARCHE_FORCEE - (millis() - debutMarcheForceeMs)) / 3600000.0f;
      }
      break;

    case MODE_FORCE_OFF:
      modeActuel = MODE_AUTO;
      digitalWrite(RELAY_PIN, LOW);
      dureeAfficheeInterface = 0.0f;
      break;

    case MODE_HORS_GEL:
      break;

    case MODE_AUTO:
      bool controleQuotidien = (heureActuelle == 5 && !controleNocturneFaitAujourdhui);
      bool controleFroid = (dernierControleHiverMs != 0 &&
          millis() - dernierControleHiverMs >= INTERVALLE_CONTROLE_FROID &&
          tempEauActuelle > 3.0 && tempEauActuelle <= 5.0);
      if (!filtrationEnCours && !cycleControleNocturneEnCours &&
          modeActuel == MODE_AUTO && (controleQuotidien || controleFroid)) {
        float tempsTheorique = calculerTempsFiltration(tempEauActuelle);
        if (modeHivernage) { 
          cycleControleNocturneEnCours = true;
          controleHiverSupplementaire = controleFroid && !controleQuotidien;
          debutFiltrationMs = millis();
          dernierControleHiverMs = debutFiltrationMs;
          if (controleQuotidien) controleNocturneFaitAujourdhui = true;
          ReleveFaitPourCeCycle = false;
          digitalWrite(RELAY_PIN, HIGH);
          journaliser(controleHiverSupplementaire ? "CONTROLE_HIVER_SUPPLEMENTAIRE_DEBUT" : "CONTROLE_HIVER_05H_DEBUT");
        }
      }
      if (cycleControleNocturneEnCours) {
        unsigned long tempsEcouleNocturne = millis() - debutFiltrationMs;
        dureeAfficheeInterface = 0.1; 
        
        if (tempsEcouleNocturne >= DELAI_STABILISATION_EAU) {
          acquerirTemperature();
          if (!defautSonde && dernierEchantillonValideMs != 0 && millis() - dernierEchantillonValideMs < 5000UL) {
            temperatureAffichee = tempEauActuelle;
            temperatureAfficheeValide = true;
            ajouterAuGraphique(temperatureAffichee, getHeureJourShort());
            preferences.begin("piscine", false);
            preferences.putFloat("t_eau", tempEauActuelle);
            preferences.putFloat("t_aff", temperatureAffichee);
            preferences.end();
            journaliser("CONTROLE_HIVER_MESURE_VALIDEE;" + String(tempEauActuelle, 1) + "C");
          } else {
            journaliser("CONTROLE_HIVER_MESURE_INVALIDE");
          }
          ReleveFaitPourCeCycle = true;
          actualiserSaisonApresStabilisation();
          if (dateCycleValide && !modeHivernage) {
            repriseSaisonConfirmee = true;
            jourRepriseSaison = dateCycle.tm_yday;
            anneeRepriseSaison = dateCycle.tm_year;
          }
          cycleControleNocturneEnCours = false;
          journaliser("CONTROLE_HIVER_TERMINE_5MIN");
          controleNocturneFaitAujourdhui = controleNocturneFaitAujourdhui || !controleHiverSupplementaire;
          controleHiverSupplementaire = false;
          dureeAfficheeInterface = 0.0; 
          if (tempEauActuelle > 3.0) { digitalWrite(RELAY_PIN, LOW); }
        }
      }

      if (!filtrationEnCours && !cycleControleNocturneEnCours && (heureActuelle == heureDemarrage ||
           (heureActuelle > heureDemarrage && repriseSaisonConfirmee)) &&
          !repriseApresCoupure && dateCycleValide && !cycleSaisonnierDejaLance) {
        float dureeInitiale = modeHivernage ? 0.0f : calculerTempsFiltration(tempEauActuelle);
        if (dureeInitiale > 0.0) {
          dernierJourCycleSaisonnier = dateCycle.tm_yday;
          derniereAnneeCycleSaisonnier = dateCycle.tm_year;
          filtrationEnCours = true;
          journaliser("FILTRATION_SAISON_DEBUT");
          repriseSaisonConfirmee = false;
          debutFiltrationMs = millis();
          ReleveFaitPourCeCycle = false;
          releveDebutCycleFait = false; 
          digitalWrite(RELAY_PIN, HIGH);

          if (premierDemarrage) {
            premierDemarrage = false;
          }
          dureeTotaleFiltrationMs = dureeInitiale * 3600000;
          dureeAfficheeInterface = dureeInitiale;
        } else {
          dureeAfficheeInterface = 0.0;
        }
      }

      if (filtrationEnCours) {
        unsigned long tempsEcoule = millis() - debutFiltrationMs;
        
        if (!repriseApresCoupure) {
          if (!releveDebutCycleFait && tempsEcoule >= DELAI_STABILISATION_EAU) {
            acquerirTemperature();
            actualiserSaisonApresStabilisation();
            releveDebutCycleFait = true;
            preferences.begin("piscine", false);
            preferences.putFloat("t_eau", tempEauActuelle);
            preferences.end();
          }
          if (releveDebutCycleFait) {
            float tempsCalculerActuel = modeHivernage ? 0.0f : calculerTempsFiltration(tempEauActuelle);
            dureeTotaleFiltrationMs = (unsigned long)(tempsCalculerActuel * 3600000.0f);
            dureeAfficheeInterface = tempsCalculerActuel;
          }
        } else {
          dureeAfficheeInterface = (float)(dureeTotaleFiltrationMs - tempsEcoule) / 3600000.0;
          if (dureeAfficheeInterface < 0) dureeAfficheeInterface = 0;
        }

        if (dureeTotaleFiltrationMs == 0) {
          digitalWrite(RELAY_PIN, LOW);
          filtrationEnCours = false;
          journaliser("FILTRATION_SAISON_STOP");
          repriseApresCoupure = false;
          dureeAfficheeInterface = 0.0;
          preferences.begin("piscine", false);
          preferences.putULong("ms_restantes", 0);
          preferences.end();
          break;
        }

        if (!ReleveFaitPourCeCycle && (dureeTotaleFiltrationMs > 30000) && (tempsEcoule >= (dureeTotaleFiltrationMs - 30000))) {
          acquerirTemperature(); 
          ReleveFaitPourCeCycle = true;
          preferences.begin("piscine", false);
          preferences.putFloat("t_eau", tempEauActuelle);
          preferences.end();
        }

        if (tempsEcoule >= dureeTotaleFiltrationMs) {
          digitalWrite(RELAY_PIN, LOW);
          filtrationEnCours = false;
          journaliser("FILTRATION_SAISON_TERMINEE");
          repriseApresCoupure = false;
          preferences.begin("piscine", false);
          preferences.putULong("ms_restantes", 0);
          preferences.end();
        }
      }
      break;
  }
}