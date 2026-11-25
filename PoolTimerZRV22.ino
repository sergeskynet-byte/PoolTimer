// PoolTimer ZR v2.2 - Version allégée de la v2.0 (même fonctionnement, mêmes adresses web, compatible appli Android)
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "time.h"
#include <esp_system.h>

// --- Configuration des broches ---
#define RELAY_PIN 2       // Relais Filtration
#define LIGHT_PIN 4       // Relais Éclairage
#define ONE_WIRE_BUS 5    // Capteur Température
#define LED_WIFI 8        // LED Statut WiFi

// --- Nom du boîtier sur le réseau (http://NOM.local). Sur une 2e carte de test, mettre un nom SANS le mot
//     "pooltimer" (ex. "carte-essai"), sinon la recherche de l'appli peut tomber sur elle ---
#define NOM_APPAREIL "pooltimer"
//#define NOM_APPAREIL "essai"

// --- Objets ---
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
AsyncWebServer server(80);
WiFiManager wm;
Preferences prefs;        // ouvert une seule fois dans setup()

// --- Constantes de temps ---
const unsigned long DELAI_STABILISATION_EAU = 300000UL;        // 5 min
const unsigned long INTERVALLE_RELEVE_GRAPH = 7200000UL;       // 2 h
const unsigned long INTERVALLE_CONTROLE_FROID = 14400000UL;    // 4 h
const unsigned long TEMPO_SORTIE_HORS_GEL = 3600000UL;         // 1 h
const unsigned long DUREE_MARCHE_FORCEE = 7200000UL;           // 2 h
const unsigned long INTERVALLE_SAUVEGARDE_FILTRATION = 60000UL;
const uint8_t SEUIL_CONFIRMATION_SONDE = 3;
const float ENTREE_HIVER = 11.5f, SORTIE_HIVER = 12.5f;

// --- Température ---
float tempEauActuelle = 20.0;
float temperatureAffichee = 20.0;
bool temperatureAfficheeValide = false;
bool defautSonde = false;
uint8_t lecturesInvalides = 0, lecturesValides = 0;
unsigned long dernierEchantillonValideMs = 0;
bool modeHivernage = false;

// --- Filtration ---
enum ModeFiltration { MODE_AUTO, MODE_FORCE_ON, MODE_HORS_GEL };
ModeFiltration modeActuel = MODE_AUTO;
int heureDemarrage = 8;     // 0-23 : heure fixe ; 24 : mode solaire (filtration centrée sur 13h30)
const int HEURE_SOLAIRE = 24;
const int CENTRE_SOLAIRE_MIN = 13 * 60 + 30;   // milieu de la filtration : 13h30
const int DEBUT_SOLAIRE_MIN_MIN = 8 * 60;      // jamais avant 8h00
const int HEURE_SECOURS_SOLAIRE = 8;           // heure de démarrage hors baignade (canicule...)
int dureeSolaireMin = 360;                     // durée du dernier cycle (minutes), conservée en NVS ("d_sol")
int gmtHiver = 1;
int regleHeureEte = 1;      // 0 aucune, 1 Europe, 2 Amérique du Nord, 3 Australie
bool filtrationEnCours = false;
bool repriseApresCoupure = false;
unsigned long debutFiltrationMs = 0;
unsigned long dureeTotaleFiltrationMs = 0;
float dureeAfficheeInterface = 0.0;
bool releveDebutCycleFait = false, releveFinCycleFait = false;
int codeDernierCycle = -1;          // jour du dernier cycle de saison (année*1000 + jour de l'année), conservé en NVS
bool repriseVerifiee = false;
bool repriseSaisonConfirmee = false;
int jourRepriseSaison = -1, anneeRepriseSaison = -1;
unsigned long debutMarcheForceeMs = 0;
unsigned long cumulFiltrationJourMs = 0, dernierCalculCumulMs = 0;

// --- Hivernage / hors-gel ---
bool cycleControleNocturneEnCours = false;
bool controleNocturneFaitAujourdhui = false;
bool controleHiverSupplementaire = false;
unsigned long dernierControleHiverMs = 0;
bool temporisationHorsGelEnCours = false;
unsigned long debutDepassement4C_Ms = 0;

bool wifiEstConnecte = false;

// --- Commandes reçues par le serveur web, exécutées par loop() (évite les accès simultanés) ---
enum Commande : uint8_t { CMD_AUCUNE, CMD_FORCE_ON, CMD_FORCE_OFF, CMD_AUTO };
volatile uint8_t commandeEnAttente = CMD_AUCUNE;
volatile bool resynchroHorlogeDemandee = false;

// --- Historiques : buffers circulaires de taille fixe (pas d'objets String en mémoire) ---
template <typename T, int N>
struct Anneau {
  T elements[N];
  int prochain = 0, nb = 0;
  T& operator[](int i) { return elements[(prochain - nb + i + N) % N]; }  // i = 0 : le plus ancien
  T& dernier() { return elements[(prochain - 1 + N) % N]; }
  T& ajouter() {
    T& e = elements[prochain];
    prochain = (prochain + 1) % N;
    if (nb < N) nb++;
    return e;
  }
};

struct PointTemp { char date[16]; float temp; };               // "jj/mm hh:mm:ss"
struct PointJour { char jour[8];  float heures; };             // "jj/mm"
struct Evenement {
  uint32_t seq; char date[16]; char msg[48];
  uint32_t ms;        // millis() à la création : sert à retrouver l'heure réelle quand le NTP arrive
  bool attente;       // true tant que la date est inconnue ("--/-- --:--:--")
};
int evenementsEnAttente = 0;

Anneau<PointTemp, 360> histoTemp;
Anneau<PointJour, 30>  histoFilt;
Anneau<Evenement, 200> journal;
uint32_t sequenceEvenement = 0;

bool flashOK = false;
bool historiqueFiltrationModifie = false;
unsigned long dernierEnregistrementFiltrationMs = 0;

// ======================================================================
//  Heure
// ======================================================================
void formaterHeure(char* buf, size_t taille, const char* format, const char* siInconnue) {
  struct tm t;
  if (getLocalTime(&t, 50)) strftime(buf, taille, format, &t);
  else strlcpy(buf, siInconnue, taille);
}

void synchroniserHorloge() {
  char tz[64];
  int h = 1 + gmtHiver;                     // Europe : changement à 01:00 UTC partout
  if (h < 0) h += 24;
  switch (regleHeureEte) {
    case 0:  snprintf(tz, sizeof(tz), "STD%d", -gmtHiver); break;
    case 2:  snprintf(tz, sizeof(tz), "STD%dDST%d,M3.2.0/2,M11.1.0/2", -gmtHiver, -(gmtHiver + 1)); break;
    case 3:  snprintf(tz, sizeof(tz), "STD%dDST%d,M10.1.0/2,M4.1.0/3", -gmtHiver, -(gmtHiver + 1)); break;
    default: snprintf(tz, sizeof(tz), "STD%dDST%d,M3.5.0/%d,M10.5.0/%d", -gmtHiver, -(gmtHiver + 1), h, h + 1); break;
  }
  configTzTime(tz, "pool.ntp.org", "time.nist.gov");
}

// Puissance d'émission réduite : sur l'ESP32-C3 SuperMini, la pleine puissance (19,5 dBm)
// dégrade souvent le signal (antenne trop proche du quartz). Si le signal devient trop faible,
// essayer WIFI_POWER_11dBm ou WIFI_POWER_13dBm.
const wifi_power_t PUISSANCE_WIFI = WIFI_POWER_8_5dBm;

void reglerRadio() {
  WiFi.setTxPower(PUISSANCE_WIFI);
  WiFi.setSleep(false);   // radio toujours active : réponses plus rapides (boîtier alimenté sur secteur)
}

const char* raisonRedemarrage() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "MISE_SOUS_TENSION";
    case ESP_RST_SW:       return "REDEMARRAGE_LOGICIEL";
    case ESP_RST_PANIC:    return "PLANTAGE";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "CHIEN_DE_GARDE";
    case ESP_RST_BROWNOUT: return "BAISSE_DE_TENSION";
    case ESP_RST_EXT:      return "BOUTON_RESET";
    case ESP_RST_USB:      return "RESET_USB";          // flashage ou bouton RST avec l'USB branché
    default:               return "AUTRE";
  }
}

void relancerMDNS() {
  MDNS.end();
  if (MDNS.begin(NOM_APPAREIL)) MDNS.addService("http", "tcp", 80);
}

// ======================================================================
//  Fichiers LittleFS (format CSV identique à la v2.0)
// ======================================================================
bool ecrireFichier(const char* chemin, bool (*contenu)(File&)) {
  if (!flashOK) return false;
  String tmp = String(chemin) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  bool ok = contenu(f);
  f.close();
  if (!ok || !LittleFS.rename(tmp, chemin)) { LittleFS.remove(tmp); return false; }
  return true;
}

void lireFichier(const char* chemin, void (*ligneLue)(String&)) {
  if (!flashOK || !LittleFS.exists(chemin)) return;
  File f = LittleFS.open(chemin, "r");
  while (f && f.available()) {
    String ligne = f.readStringUntil('\n');
    ligne.trim();
    if (ligne.length()) ligneLue(ligne);
  }
  if (f) f.close();
}

bool ecrireTemperatures(File& f) {
  bool ok = true;
  for (int i = 0; i < histoTemp.nb; i++) ok &= f.printf("%s;%.2f\n", histoTemp[i].date, histoTemp[i].temp) > 0;
  return ok;
}

bool ecrireFiltration(File& f) {
  bool ok = true;
  for (int i = 0; i < histoFilt.nb; i++) ok &= f.printf("%s;%.3f\n", histoFilt[i].jour, histoFilt[i].heures) > 0;
  return ok;
}

bool ecrireJournal(File& f) {
  bool ok = true;
  for (int i = 0; i < journal.nb; i++)
    ok &= f.printf("%lu;%s;%s\n", (unsigned long)journal[i].seq, journal[i].date, journal[i].msg) > 0;
  return ok;
}

void lireLigneTemperature(String& l) {
  int sep = l.lastIndexOf(';');
  if (sep <= 0) return;
  PointTemp& p = histoTemp.ajouter();
  strlcpy(p.date, l.substring(0, sep).c_str(), sizeof(p.date));
  p.temp = l.substring(sep + 1).toFloat();
}

void lireLigneFiltration(String& l) {
  int sep = l.lastIndexOf(';');
  if (sep <= 0) return;
  PointJour& p = histoFilt.ajouter();
  strlcpy(p.jour, l.substring(0, sep).c_str(), sizeof(p.jour));
  p.heures = l.substring(sep + 1).toFloat();
}

void lireLigneJournal(String& l) {
  int s1 = l.indexOf(';'), s2 = l.indexOf(';', s1 + 1);
  if (s1 <= 0 || s2 < 0) return;
  Evenement& e = journal.ajouter();
  e.ms = 0;
  e.attente = false;
  e.seq = l.substring(0, s1).toInt();
  strlcpy(e.date, l.substring(s1 + 1, s2).c_str(), sizeof(e.date));
  strlcpy(e.msg, l.substring(s2 + 1).c_str(), sizeof(e.msg));
  if (e.seq > sequenceEvenement) sequenceEvenement = e.seq;
}

void sauvegarderFiltration() {
  if (!flashOK) return;
  if (ecrireFichier("/filtration.csv", ecrireFiltration)) {
    historiqueFiltrationModifie = false;
    dernierEnregistrementFiltrationMs = millis();
  } else Serial.println("Erreur sauvegarde filtration LittleFS");
}

// ======================================================================
//  Journal, graphiques
// ======================================================================
void journaliser(const char* message) {
  Evenement& e = journal.ajouter();
  e.seq = ++sequenceEvenement;
  formaterHeure(e.date, sizeof(e.date), "%d/%m %H:%M:%S", "--/-- --:--:--");
  strlcpy(e.msg, message, sizeof(e.msg));
  e.ms = millis();
  e.attente = (e.date[0] == '-');
  if (e.attente) evenementsEnAttente++;
  Serial.printf("EVENT %lu;%s;%s\n", (unsigned long)e.seq, e.date, e.msg);
  ecrireFichier("/evenements.csv", ecrireJournal);
}

// Quand l'heure NTP devient disponible, date les événements créés avant (démarrage, connexion Wi-Fi...)
void corrigerDatesJournal() {
  time_t maintenant = time(nullptr);
  uint32_t msMaintenant = millis();
  for (int i = 0; i < journal.nb; i++) {
    Evenement& e = journal[i];
    if (!e.attente) continue;
    time_t t = maintenant - (msMaintenant - e.ms) / 1000;
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(e.date, sizeof(e.date), "%d/%m %H:%M:%S", &lt);
    e.attente = false;
  }
  evenementsEnAttente = 0;
  ecrireFichier("/evenements.csv", ecrireJournal);
}

void ajouterAuGraphique(float temp) {
  char date[16];
  formaterHeure(date, sizeof(date), "%d/%m %H:%M:%S", "--/-- --:--:--");
  bool memeDate = histoTemp.nb > 0 && strcmp(histoTemp.dernier().date, date) == 0;
  PointTemp& p = memeDate ? histoTemp.dernier() : histoTemp.ajouter();
  strlcpy(p.date, date, sizeof(p.date));
  p.temp = temp;
  if (flashOK && !ecrireFichier("/temperatures.csv", ecrireTemperatures))
    Serial.println("Erreur sauvegarde temperatures LittleFS");
}

void ajouterFiltrationAuGraphique(float heures, const char* jour) {
  if (histoFilt.nb > 0 && strcmp(histoFilt.dernier().jour, jour) == 0) {
    if (histoFilt.dernier().heures == heures) return;   // valeur inchangée : rien à réécrire en flash
    histoFilt.dernier().heures = heures;
  } else {
    PointJour& p = histoFilt.ajouter();
    strlcpy(p.jour, jour, sizeof(p.jour));
    p.heures = heures;
  }
  historiqueFiltrationModifie = true;
}

// ======================================================================
//  Sonde et calculs
// ======================================================================
void traiterMesure(float t);

// Mesure non bloquante : la conversion est lancée, puis lue ~0,75 s plus tard (une mesure toutes les 2 s).
// Renvoie true quand une nouvelle lecture vient d'être traitée.
bool gererSonde() {
  static unsigned long debutConversionMs = 0;
  static bool conversionEnCours = false;
  if (!conversionEnCours) {
    if (debutConversionMs != 0 && millis() - debutConversionMs < 2000UL) return false;
    sensors.requestTemperatures();          // rend la main immédiatement
    debutConversionMs = millis();
    conversionEnCours = true;
    return false;
  }
  if (millis() - debutConversionMs < (unsigned long)sensors.millisToWaitForConversion(sensors.getResolution()) + 50UL)
    return false;
  conversionEnCours = false;
  traiterMesure(sensors.getTempCByIndex(0));
  return true;
}

void traiterMesure(float t) {
  bool valide = isfinite(t) && t != DEVICE_DISCONNECTED_C && t > -20.0f && t < 60.0f && fabsf(t - 85.0f) > 0.01f;
  if (!valide) {
    lecturesValides = 0;
    if (lecturesInvalides < 255) lecturesInvalides++;
    if (!defautSonde && lecturesInvalides >= SEUIL_CONFIRMATION_SONDE) {
      defautSonde = true;
      journaliser("DEFAUT_SONDE;Mode securite continu (24h/24)");
    }
    return;
  }
  lecturesInvalides = 0;
  if (lecturesValides < 255) lecturesValides++;
  if (defautSonde) {
    if (lecturesValides < SEUIL_CONFIRMATION_SONDE) return;
    defautSonde = false;
    journaliser("SONDE_RETABLIE;Retour au mode automatique");
  }
  tempEauActuelle = t;
  dernierEchantillonValideMs = millis();
}

bool mesureRecente() {
  return !defautSonde && dernierEchantillonValideMs != 0 && millis() - dernierEchantillonValideMs < 5000UL;
}

void actualiserSaisonApresStabilisation() {
  if (defautSonde) return;
  bool ancien = modeHivernage;
  if (modeHivernage && tempEauActuelle >= SORTIE_HIVER) modeHivernage = false;
  else if (!modeHivernage && tempEauActuelle <= ENTREE_HIVER) modeHivernage = true;
  if (ancien != modeHivernage) journaliser(modeHivernage ? "PASSAGE_HIVERNAGE" : "PASSAGE_SAISON");
}

// Minute de la journée (0-1439) à laquelle le cycle de saison démarre.
int demarrageMinutes() {
  if (heureDemarrage != HEURE_SOLAIRE) return heureDemarrage * 60;
  if (modeHivernage || tempEauActuelle >= 27.0f) return HEURE_SECOURS_SOLAIRE * 60;   // solaire : baignade seulement
  int d = CENTRE_SOLAIRE_MIN - dureeSolaireMin / 2;
  d = (d / 5) * 5;
  return constrain(d, DEBUT_SOLAIRE_MIN_MIN, CENTRE_SOLAIRE_MIN);
}

float calculerTempsFiltration(float t) {
  if (t < 12.0f) return 0.0f;
  if (t < 14.0f) return 3.0f;
  if (t < 16.0f) return 4.0f;
  if (t < 20.0f) return 6.0f;
  if (t < 24.0f) return 8.0f;
  if (t < 27.0f) return 10.0f;
  float d = 10.0f + (t - 27.0f) * 3.0f;
  return d > 23.916f ? 23.916f : d;
}

int getQualiteWifi() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

// --- Arrêts de cycle (code auparavant répété à plusieurs endroits) ---
void annulerCycles() {            // avant marche forcée ou hors-gel
  filtrationEnCours = false;
  cycleControleNocturneEnCours = false;
  repriseApresCoupure = false;
  dureeTotaleFiltrationMs = 0;
  prefs.putULong("ms_restantes", 0);
}

void finCycleSaison(const char* evenement) {
  digitalWrite(RELAY_PIN, LOW);
  filtrationEnCours = false;
  repriseApresCoupure = false;
  prefs.putULong("ms_restantes", 0);
  journaliser(evenement);
}

void executerCommande() {
  uint8_t cmd = commandeEnAttente;
  if (cmd == CMD_AUCUNE) return;
  commandeEnAttente = CMD_AUCUNE;
  if (defautSonde || modeActuel == MODE_HORS_GEL || tempEauActuelle <= 3.0f) return;   // protections prioritaires
  if (cmd == CMD_FORCE_ON) {
    annulerCycles();
    modeActuel = MODE_FORCE_ON;
    journaliser("MARCHE_FORCEE_DEBUT");
    debutMarcheForceeMs = millis();
    digitalWrite(RELAY_PIN, HIGH);
  } else if (cmd == CMD_FORCE_OFF) {
    if (modeActuel != MODE_FORCE_ON) return;
    modeActuel = MODE_AUTO;
    journaliser("MARCHE_FORCEE_ARRETEE");
    debutMarcheForceeMs = 0;
    digitalWrite(RELAY_PIN, LOW);
    dureeAfficheeInterface = 0.0f;
  } else if (cmd == CMD_AUTO) {
    if (modeActuel == MODE_FORCE_ON) {
      journaliser("MARCHE_FORCEE_ANNULEE");
      digitalWrite(RELAY_PIN, LOW);
      debutMarcheForceeMs = 0;
    }
    modeActuel = MODE_AUTO;
  }
}

// --- Textes envoyés à la page et à l'appli (inchangés) ---
const char* texteMode() {
  if (modeActuel == MODE_FORCE_ON) return "MARCHE FORCÉE";
  if (modeActuel == MODE_HORS_GEL) return "SÉCURITÉ HORS-GEL";
  if (defautSonde) return "SECURITE CONTINUE (SONDE)";
  if (repriseApresCoupure) return "REPRISE APRÈS COUPURE";
  if (cycleControleNocturneEnCours) return "TEST NOCTURNE HORS-GEL (5 min)";
  return "AUTOMATIQUE";
}

const char* texteSaison() {
  if (defautSonde) return "Defaut sonde - continu";
  if (modeActuel == MODE_HORS_GEL || tempEauActuelle <= 3.0) return "Hors-gel";
  if (modeHivernage) return "Hivernage";
  if (tempEauActuelle >= 27.0) return "Canicule";
  return "Saison de baignade";
}

const char* texteWifi(int pct) {
  if (pct >= 80) return "Excellent";
  if (pct >= 60) return "Bon";
  if (pct >= 40) return "Moyen";
  if (pct > 0) return "Faible";
  return "Déconnecté";
}

// ======================================================================
//  Fichiers Web (HTML, PWA)
// ======================================================================
const char manifest_json[] PROGMEM = R"rawliteral({
  "name": "PoolTimer ZR v2.2",
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
<meta name="apple-mobile-web-app-title" content="PoolTimer">
<link rel="apple-touch-icon" href="https://img.icons8.com/fluency/192/swimming-pool.png">
<link rel="manifest" href="/manifest.json">
<title>PoolTimer ZR V 2.2</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; background: #f4f7f9; color: #333; margin: 0; padding: 20px; }
.container { max-width: 600px; margin: auto; background: white; padding: 25px; border-radius: 15px; box-shadow: 0 4px 15px rgba(0,0,0,0.05); position: relative; }
.lang-container { position: absolute; top: 15px; right: 20px; }
.btn-lang { background: #fff; border: 1px solid #ccc; padding: 5px 10px; border-radius: 20px; cursor: pointer; font-weight: bold; font-size: 0.85em; box-shadow: 0 2px 5px rgba(0,0,0,0.05); }
.btn-lang:hover { background: #ebf5fb; border-color: #0288d1; }
.logo-img { display: block; width: 100%; max-width: 320px; margin: 10px auto 15px; border-radius: 8px; }
h1 { color: #0288d1; margin: 5px 0 25px; font-size: 1.8em; }
.grid { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-bottom: 25px; }
.card { background: #ebf5fb; padding: 15px; border-radius: 10px; }
.card-full { grid-column: span 2; background: #e3f2fd; }
.card-purple { grid-column: span 2; background: #f3e5f5; }
h3 { margin: 0 0 10px; font-size: 0.9em; color: #5a738e; text-transform: uppercase; }
.valeur { font-size: 1.4em; font-weight: bold; color: #01579b; }
.card-purple .valeur { color: #4a148c; }
.periode-texte { font-size: 0.8em; color: #555; margin-top: 5px; }
.section { margin-top: 25px; padding-top: 20px; border-top: 2px dashed #ebf5fb; }
.row-control { margin: 15px 0; display: flex; justify-content: center; align-items: center; gap: 10px; flex-wrap: wrap; }
.btn { padding: 12px 24px; margin: 5px; font-size: 1em; border: none; border-radius: 5px; cursor: pointer; transition: 0.3s; color: white; font-weight: bold; }
.btn-blue { background: #0288d1; } .btn-blue:hover { background: #01579b; }
.btn-red { background: #e53935; } .btn-red:hover { background: #b71c1c; }
.btn-green { background: #4caf50; } .btn-green:hover { background: #1b5e20; }
.btn:disabled { opacity: 0.5; cursor: default; }
.bleu { font-weight: bold; color: #0288d1; }
select { padding: 10px; font-size: 1em; border-radius: 5px; border: 1px solid #ccc; width: 100px; text-align: center; }
.notice-gratuite { margin: 22px auto 0; padding: 16px; border-radius: 12px; background: #eaf6ff; border: 1px solid #b8ddf7; color: #174b6e; line-height: 1.55; font-size: .9em; }
.journal-liste { text-align: left; max-height: 270px; overflow-y: auto; padding: 10px; border-radius: 9px; background: #f5f9fc; border: 1px solid #dce7ef; margin-top: 12px; }
.journal-ligne { padding: 8px 5px; border-bottom: 1px solid #dce7ef; font-size: .82em; overflow-wrap: anywhere; }
.journal-ligne:last-child { border-bottom: 0; }
.graph-box { border: 1px solid #e1e8ed; padding: 10px; border-radius: 10px; margin-top: 15px; }
.pied { margin-top: 25px; font-size: 0.8em; color: #a0aec0; }
</style>
<script>
const ordreLangues = ['fr', 'en', 'it', 'de', 'es', 'pt', 'el'];
const drapeaux = { fr: '🇬🇧 EN', en: '🇮🇹 IT', it: '🇩🇪 DE', de: '🇪🇸 ES', es: '🇵🇹 PT', pt: '🇬🇷 EL', el: '🇫🇷 FR' };
let langue = 'en', donnees = null, graphique = null, selectsInitialises = false;

// Clés de statut = texte français exact envoyé par l'ESP32
const dictionnaire = {
  fr: { titre_journal: "Journal des événements", notice_gratuite: "PoolTimer ZR est un projet libre et gratuit, conçu dans un esprit de partage. Ses plans et son programme sont mis gratuitement à disposition de tous. Toute commercialisation du PoolTimer ZR est interdite.",
    lbl_heure_actuelle: "Heure Actuelle", lbl_mode_actuel: "Mode Actuel", lbl_temp_eau: "Température", lbl_statut_pompe: "État Pompe", lbl_temps_filtration: "Temps de Filtration", lbl_signal_wifi: "Signal WiFi", titre_prog: "Configuration Système", lbl_start: "Démarrage : ", lbl_modif_heure: "Démarrage : ", lbl_modif_gmt: "Fuseau (GMT) : ", btn_enregistrer: "Enregistrer", titre_cmd: "Commandes Manuelles", btn_auto: "Mode Auto", btn_force_on: "Marche Forcée (2h)", btn_force_off: "Arrêt Forcé", alert_save: "Paramètres sauvegardés !",
    "REPRISE APRÈS COUPURE": "REPRISE EN COURS ⚡", "SECURITE CONTINUE (SONDE)": "SÉCURITÉ CONTINUE (SONDE)",
    "Excellent": "Excellent 📶", "Bon": "Bon 📡", "Moyen": "Moyen ⚠️", "Faible": "Faible ❌", lbl_periode: "Période : ",
    "Hors-gel": "🚨 Hors-gel", "Hivernage": "❄️ Hivernage", "Saison de baignade": "🏊 Saison de baignade", "Canicule": "🔥 Canicule", "Defaut sonde - continu": "⚠️ Défaut sonde - marche continue",
    titre_lum: "Éclairage Piscine", btn_lum_on: "Allumer les projecteurs 💡", btn_lum_off: "Éteindre les projecteurs 🌑", graph_titre: "Suivi Température & Filtration", lbl_graph_barres: "Filtration (h)", lbl_axe_heures: "Heures",
    lbl_ip_boitier: "Adresse IP du boîtier : ", btn_sync_ntp: "Forcer la synchro NTP 🔄", alert_ntp_ok: "Synchronisation NTP demandée !", alert_ntp_err: "Erreur lors de la synchronisation.", ete: "été", hiver: "hiver",
    journal_afficher: "📋 Afficher le journal des événements", journal_masquer: "✖ Masquer le journal des événements", journal_vide: "Aucun événement enregistré.", journal_indispo: "Journal indisponible.", journal_chargement: "Chargement...", lbl_modif_dst: "Heure d'été : ", dst_europe: "Europe", dst_amerique: "Amérique du Nord", dst_australie: "Australie", dst_aucune: "Aucune" },
  en: { titre_journal: "Event log", notice_gratuite: "PoolTimer ZR may not be sold: its plans and program are provided free of charge.",
    lbl_heure_actuelle: "Current Time", lbl_mode_actuel: "Current Mode", lbl_temp_eau: "Temperature", lbl_statut_pompe: "Pump Status", lbl_temps_filtration: "Filtration Runtime", lbl_signal_wifi: "WiFi Signal Quality", titre_prog: "System Configuration", lbl_start: "Start: ", lbl_modif_heure: "Start Hour: ", lbl_modif_gmt: "Timezone (GMT): ", btn_enregistrer: "Save Configuration", titre_cmd: "Manual Override", btn_auto: "Auto Mode", btn_force_on: "Forced ON (2h)", btn_force_off: "Forced OFF", alert_save: "Settings saved!",
    "ALLUMÉE": "RUNNING", "ÉTEINTE": "STOPPED", "AUTOMATIQUE": "AUTOMATIC", "MARCHE FORCÉE": "FORCED ON", "SÉCURITÉ HORS-GEL": "ANTI-FREEZE", "TEST NOCTURNE HORS-GEL (5 min)": "NIGHTLY TEST", "REPRISE APRÈS COUPURE": "POWER RECOVERY ⚡", "SECURITE CONTINUE (SONDE)": "CONTINUOUS SAFETY (SENSOR)",
    "Excellent": "Excellent 📶", "Bon": "Good 📡", "Moyen": "Fair ⚠️", "Faible": "Weak ❌", "Déconnecté": "Disconnected", lbl_periode: "Period: ",
    "Hors-gel": "🚨 Anti-freeze", "Hivernage": "❄️ Winterizing", "Saison de baignade": "🏊 Pool Season", "Canicule": "🔥 Heatwave", "Defaut sonde - continu": "⚠️ Sensor fault - continuous run",
    titre_lum: "Pool Lighting", btn_lum_on: "Turn Lights ON 💡", btn_lum_off: "Turn Lights OFF 🌑", graph_titre: "Temperature & Filtration Logs", lbl_graph_barres: "Filtration (h)", lbl_axe_heures: "Hours",
    lbl_ip_boitier: "Device IP Address: ", btn_sync_ntp: "Force NTP Sync 🔄", alert_ntp_ok: "NTP synchronization requested!", alert_ntp_err: "Error during synchronization.", ete: "summer", hiver: "winter",
    journal_afficher: "📋 Show event log", journal_masquer: "✖ Hide event log", journal_vide: "No events recorded.", journal_indispo: "Log unavailable.", journal_chargement: "Loading...", lbl_modif_dst: "Daylight saving: ", dst_europe: "Europe", dst_amerique: "North America", dst_australie: "Australia", dst_aucune: "None" },
  it: { titre_journal: "Registro eventi", notice_gratuite: "PoolTimer ZR non può essere venduto: schemi e programma sono forniti gratuitamente.",
    lbl_heure_actuelle: "Ora Corrente", lbl_mode_actuel: "Modalità Corrente", lbl_temp_eau: "Temperatura", lbl_statut_pompe: "Stato Pompa", lbl_temps_filtration: "Tempo di Filtrazione", lbl_signal_wifi: "Segnale WiFi", titre_prog: "Configurazione", lbl_start: "Avvio: ", lbl_modif_heure: "Avvio: ", lbl_modif_gmt: "Fuso (GMT): ", btn_enregistrer: "Salva", titre_cmd: "Comandi Manuali", btn_auto: "Modalità Auto", btn_force_on: "Forzato ACCESO (2h)", btn_force_off: "Arresto Forzato", alert_save: "Impostazioni salvate!",
    "ALLUMÉE": "ACCESA", "ÉTEINTE": "SPENTA", "AUTOMATIQUE": "AUTOMATICO", "MARCHE FORCÉE": "MARCIA FORZATA", "SÉCURITÉ HORS-GEL": "ANTIGELO", "TEST NOCTURNE HORS-GEL (5 min)": "TEST NOTTURNO", "REPRISE APRÈS COUPURE": "RIPRISTINO ENERGIA ⚡", "SECURITE CONTINUE (SONDE)": "SICUREZZA CONTINUA (SONDA)",
    "Excellent": "Eccellente 📶", "Bon": "Buono 📡", "Moyen": "Medio ⚠️", "Faible": "Debole ❌", "Déconnecté": "Disconnesso", lbl_periode: "Periodo: ",
    "Hors-gel": "🚨 Antigelo", "Hivernage": "❄️ Svernamento", "Saison de baignade": "🏊 Stagione Balneare", "Canicule": "🔥 Ondata di Calore", "Defaut sonde - continu": "⚠️ Guasto sonda - funzionamento continuo",
    titre_lum: "Illuminazione Piscina", btn_lum_on: "Accendi i fari 💡", btn_lum_off: "Spegni i fari 🌑", graph_titre: "Cronologia Temp & Filtrazione", lbl_graph_barres: "Filtrazione (h)", lbl_axe_heures: "Ore",
    lbl_ip_boitier: "Indirizzo IP del dispositivo: ", btn_sync_ntp: "Forza NTP 🔄", alert_ntp_ok: "Sincronizzazione NTP richiesta!", alert_ntp_err: "Errore di sincronizzazione.", ete: "estate", hiver: "inverno",
    journal_afficher: "📋 Mostra registro eventi", journal_masquer: "✖ Nascondi registro eventi", journal_vide: "Nessun evento registrato.", journal_indispo: "Registro non disponibile.", journal_chargement: "Caricamento...", lbl_modif_dst: "Ora legale: ", dst_europe: "Europa", dst_amerique: "Nord America", dst_australie: "Australia", dst_aucune: "Nessuna" },
  de: { titre_journal: "Ereignisprotokoll", notice_gratuite: "PoolTimer ZR darf nicht verkauft werden: Baupläne und Programm werden kostenlos bereitgestellt.",
    lbl_heure_actuelle: "Aktuelle Uhrzeit", lbl_mode_actuel: "Aktueller Modus", lbl_temp_eau: "Temperatur", lbl_statut_pompe: "Pumpenstatus", lbl_temps_filtration: "Filtrationszeit", lbl_signal_wifi: "WiFi Signalstärke", titre_prog: "Systemkonfiguration", lbl_start: "Start: ", lbl_modif_heure: "Startzeit: ", lbl_modif_gmt: "Zeitzone (GMT): ", btn_enregistrer: "Speichern", titre_cmd: "Manuelle Steuerung", btn_auto: "Auto-Modus", btn_force_on: "Dauer-EIN (2h)", btn_force_off: "Dauer-AUS", alert_save: "Einstellungen gespeichert!",
    "ALLUMÉE": "IN BETRIEB", "ÉTEINTE": "GESTOPPT", "AUTOMATIQUE": "AUTOMATISCH", "MARCHE FORCÉE": "ZWANGSLAUF", "SÉCURITÉ HORS-GEL": "FROSTSCHUTZ", "TEST NOCTURNE HORS-GEL (5 min)": "FROSTTEST", "REPRISE APRÈS COUPURE": "STROM-WIEDERHERSTELLUNG ⚡", "SECURITE CONTINUE (SONDE)": "DAUERSICHERHEIT (FÜHLER)",
    "Excellent": "Ausgezeichnet 📶", "Bon": "Gut 📡", "Moyen": "Mittel ⚠️", "Faible": "Schwach ❌", "Déconnecté": "Getrennt", lbl_periode: "Zeitraum: ",
    "Hors-gel": "🚨 Frostschutz", "Hivernage": "❄️ Überwinterung", "Saison de baignade": "🏊 Badesaison", "Canicule": "🔥 Hitzewelle", "Defaut sonde - continu": "⚠️ Fühlerfehler - Dauerbetrieb",
    titre_lum: "Poolbeleuchtung", btn_lum_on: "Scheinwerfer EIN 💡", btn_lum_off: "Scheinwerfer AUS 🌑", graph_titre: "Temperatur & Filterverlauf", lbl_graph_barres: "Filtration (h)", lbl_axe_heures: "Stunden",
    lbl_ip_boitier: "Geräte-IP-Adresse: ", btn_sync_ntp: "NTP Sync erzwingen 🔄", alert_ntp_ok: "NTP-Synchronisierung angefordert!", alert_ntp_err: "Fehler bei der Synchronisierung.", ete: "Sommer", hiver: "Winter",
    journal_afficher: "📋 Ereignisprotokoll anzeigen", journal_masquer: "✖ Ereignisprotokoll ausblenden", journal_vide: "Keine Ereignisse gespeichert.", journal_indispo: "Protokoll nicht verfügbar.", journal_chargement: "Wird geladen...", lbl_modif_dst: "Sommerzeit: ", dst_europe: "Europa", dst_amerique: "Nordamerika", dst_australie: "Australien", dst_aucune: "Keine" },
  es: { titre_journal: "Registro de eventos", notice_gratuite: "PoolTimer ZR no se puede vender: los planos y el programa se proporcionan gratuitamente.",
    lbl_heure_actuelle: "Hora Actual", lbl_mode_actuel: "Modo Actual", lbl_temp_eau: "Temperatura", lbl_statut_pompe: "Estado de la Bomba", lbl_temps_filtration: "Tiempo de Filtración", lbl_signal_wifi: "Señal WiFi", titre_prog: "Configuración", lbl_start: "Inicio: ", lbl_modif_heure: "Inicio: ", lbl_modif_gmt: "Zona (GMT): ", btn_enregistrer: "Guardar", titre_cmd: "Mandos Manuales", btn_auto: "Modo Auto", btn_force_on: "Marcha Forzada (2h)", btn_force_off: "Parada Forzada", alert_save: "¡Configuración guardada!",
    "ALLUMÉE": "ENCENDIDA", "ÉTEINTE": "APAGADA", "AUTOMATIQUE": "AUTOMÁTICO", "MARCHE FORCÉE": "MARCHA FORZADA", "SÉCURITÉ HORS-GEL": "ANTI-HELADAS", "TEST NOCTURNE HORS-GEL (5 min)": "TEST ANTI-HELADAS", "REPRISE APRÈS COUPURE": "RECUPERACIÓN ENERGÍA ⚡", "SECURITE CONTINUE (SONDE)": "SEGURIDAD CONTINUA (SONDA)",
    "Excellent": "Excelente 📶", "Bon": "Bueno 📡", "Moyen": "Medio ⚠️", "Faible": "Débil ❌", "Déconnecté": "Desconectado", lbl_periode: "Periodo: ",
    "Hors-gel": "🚨 Anti-heladas", "Hivernage": "❄️ Invernada", "Saison de baignade": "🏊 Temporada de Baño", "Canicule": "🔥 Ola de Calor", "Defaut sonde - continu": "⚠️ Fallo de sonda - marcha continua",
    titre_lum: "Iluminación", btn_lum_on: "Encender focos 💡", btn_lum_off: "Apagar focos 🌑", graph_titre: "Historial Temp & Filtración", lbl_graph_barres: "Filtración (h)", lbl_axe_heures: "Horas",
    lbl_ip_boitier: "Dirección IP del dispositivo: ", btn_sync_ntp: "Forzar NTP 🔄", alert_ntp_ok: "¡Sincronización NTP solicitada!", alert_ntp_err: "Error en la sincronización.", ete: "verano", hiver: "invierno",
    journal_afficher: "📋 Mostrar registro de eventos", journal_masquer: "✖ Ocultar registro de eventos", journal_vide: "Ningún evento registrado.", journal_indispo: "Registro no disponible.", journal_chargement: "Cargando...", lbl_modif_dst: "Horario de verano: ", dst_europe: "Europa", dst_amerique: "Norteamérica", dst_australie: "Australia", dst_aucune: "Ninguno" },
  pt: { titre_journal: "Registo de eventos", notice_gratuite: "PoolTimer ZR não pode ser vendido: os planos e o programa são fornecidos gratuitamente.",
    lbl_heure_actuelle: "Hora Atual", lbl_mode_actuel: "Modo Atual", lbl_temp_eau: "Temperatura", lbl_statut_pompe: "Estado da Bomba", lbl_temps_filtration: "Tempo de Filtração", lbl_signal_wifi: "Sinal WiFi", titre_prog: "Configuração", lbl_start: "Início: ", lbl_modif_heure: "Início: ", lbl_modif_gmt: "Fuso (GMT): ", btn_enregistrer: "Gravar", titre_cmd: "Comandos Manuais", btn_auto: "Modo Auto", btn_force_on: "Marcha Forçada (2h)", btn_force_off: "Paragem Forçada", alert_save: "Definições guardadas!",
    "ALLUMÉE": "LIGADA", "ÉTEINTE": "DESLIGADA", "AUTOMATIQUE": "AUTOMÁTICO", "MARCHE FORCÉE": "MARCHA FORÇADA", "SÉCURITÉ HORS-GEL": "ANTI-GELO", "TEST NOCTURNE HORS-GEL (5 min)": "TESTE ANTI-GELO", "REPRISE APRÈS COUPURE": "RECUPERAÇÃO DE ENERGIA ⚡", "SECURITE CONTINUE (SONDE)": "SEGURANÇA CONTÍNUA (SONDA)",
    "Excellent": "Excelente 📶", "Bon": "Bom 📡", "Moyen": "Médio ⚠️", "Faible": "Fraco ❌", "Déconnecté": "Desconectado", lbl_periode: "Período: ",
    "Hors-gel": "🚨 Anti-gelo", "Hivernage": "❄️ Invernada", "Saison de baignade": "🏊 Época de Banhos", "Canicule": "🔥 Onda de Calor", "Defaut sonde - continu": "⚠️ Falha da sonda - funcionamento contínuo",
    titre_lum: "Iluminação", btn_lum_on: "Ligar projetores 💡", btn_lum_off: "Desligar projetores 🌑", graph_titre: "Histórico Temp & Filtração", lbl_graph_barres: "Filtração (h)", lbl_axe_heures: "Horas",
    lbl_ip_boitier: "Endereço IP do dispositivo: ", btn_sync_ntp: "Forçar NTP 🔄", alert_ntp_ok: "Sincronização NTP solicitada!", alert_ntp_err: "Erro na sincronização.", ete: "verão", hiver: "inverno",
    journal_afficher: "📋 Mostrar registo de eventos", journal_masquer: "✖ Ocultar registo de eventos", journal_vide: "Nenhum evento registado.", journal_indispo: "Registo indisponível.", journal_chargement: "A carregar...", lbl_modif_dst: "Hora de verão: ", dst_europe: "Europa", dst_amerique: "América do Norte", dst_australie: "Austrália", dst_aucune: "Nenhuma" },
  el: { titre_journal: "Αρχείο συμβάντων", notice_gratuite: "Το PoolTimer ZR δεν επιτρέπεται να πωλείται: τα σχέδια και το πρόγραμμα παρέχονται δωρεάν.",
    lbl_heure_actuelle: "Τρέχουσα Ώρα", lbl_mode_actuel: "Λειτουργία", lbl_temp_eau: "Θερμοκρασία", lbl_statut_pompe: "Αντλία", lbl_temps_filtration: "Φιλτράρισμα", lbl_signal_wifi: "Σήμα WiFi", titre_prog: "Ρύθμιση", lbl_start: "Έναρξη: ", lbl_modif_heure: "Έναρξη: ", lbl_modif_gmt: "Ζώνη (GMT): ", btn_enregistrer: "Αποθήκευση", titre_cmd: "Χειροκίνητες Εντολές", btn_auto: "Αυτόματο", btn_force_on: "Αναγκαστική (2ω)", btn_force_off: "Διακοπή", alert_save: "Αποθηκεύτηκε!",
    "ALLUMÉE": "ΑΝΑΜΜΕΝΗ", "ÉTEINTE": "ΣΒΗΣΤΗ", "AUTOMATIQUE": "ΑΥΤΟΜΑΤΟ", "MARCHE FORCÉE": "ΑΝΑΓΚΑΣΤΙΚΗ", "SÉCURITÉ HORS-GEL": "ΠΡΟΣΤΑΣΙΑ ΠΑΓΕΤΟΥ", "TEST NOCTURNE HORS-GEL (5 min)": "ΕΛΕΓΧΟΣ ΠΑΓΕΤΟΥ", "REPRISE APRÈS COUPURE": "ΕΠΑΝΑΦΟΡΑ ΡΕΥΜΑΤΟΣ ⚡", "SECURITE CONTINUE (SONDE)": "ΣΥΝΕΧΗΣ ΑΣΦΑΛΕΙΑ (ΑΙΣΘΗΤΗΡΑΣ)",
    "Excellent": "Εξαιρετικό 📶", "Bon": "Καλό 📡", "Moyen": "Μέτριο ⚠️", "Faible": "Ασθενές ❌", "Déconnecté": "Αποσυνδέθηκε", lbl_periode: "Περίοδος: ",
    "Hors-gel": "🚨 Παγετός", "Hivernage": "❄️ Χειμερινή", "Saison de baignade": "🏊 Κολύμβηση", "Canicule": "🔥 Καύσωνας", "Defaut sonde - continu": "⚠️ Βλάβη αισθητήρα - συνεχής λειτουργία",
    titre_lum: "Φωτισμός", btn_lum_on: "Φώτα ON 💡", btn_lum_off: "Φώτα OFF 🌑", graph_titre: "Ιστορικό Θερμ & Φιλτραρίσματος", lbl_graph_barres: "Φιλτράρισμα (h)", lbl_axe_heures: "Ώρες",
    lbl_ip_boitier: "Διεύθυνση IP συσκευής: ", btn_sync_ntp: "Συγχρονισμός NTP 🔄", alert_ntp_ok: "Ζητήθηκε συγχρονισμός NTP!", alert_ntp_err: "Σφάλμα συγχρονισμού.", ete: "θερινή", hiver: "χειμερινή",
    journal_afficher: "📋 Εμφάνιση αρχείου συμβάντων", journal_masquer: "✖ Απόκρυψη αρχείου συμβάντων", journal_vide: "Δεν υπάρχουν συμβάντα.", journal_indispo: "Το αρχείο δεν είναι διαθέσιμο.", journal_chargement: "Φόρτωση...", lbl_modif_dst: "Θερινή ώρα: ", dst_europe: "Ευρώπη", dst_amerique: "Βόρεια Αμερική", dst_australie: "Αυστραλία", dst_aucune: "Καμία" }
};

const $ = id => document.getElementById(id);
// Traduction : langue choisie, sinon français, sinon texte brut reçu
const T = cle => (dictionnaire[langue] || {})[cle] || dictionnaire.fr[cle] || cle;

if ('serviceWorker' in navigator) navigator.serviceWorker.register('/sw.js');

function basculerLangue() {
  langue = ordreLangues[(ordreLangues.indexOf(langue) + 1) % ordreLangues.length];
  traduireInterface();
}

function traduireInterface() {
  $('btn-lang-id').innerText = drapeaux[langue];
  document.querySelectorAll('[data-i18n]').forEach(el => el.innerText = T(el.dataset.i18n));
  actualiserBoutonJournal();
  afficherDonnees();
  if (graphique) {
    graphique.data.datasets[0].label = T('lbl_temp_eau');
    graphique.data.datasets[1].label = T('lbl_graph_barres');
    graphique.options.scales.yFilt.title.text = T('lbl_axe_heures');
    graphique.update();
  }
}

function afficherDonnees() {
  const allumee = donnees && donnees.lumiere;
  const btn = $('btn_lumiere');
  btn.innerText = T(allumee ? 'btn_lum_off' : 'btn_lum_on');
  btn.className = 'btn ' + (allumee ? 'btn-red' : 'btn-green');
  if (!donnees) return;
  const d = donnees;
  $('heure').innerText = d.heure;
  $('temp_eau').innerText = d.temp_eau + ' °C';
  $('duree').innerText = d.duree + ' h';
  $('ip_adresse').innerText = d.ip_esp;
  if (d.uptime_s !== undefined) {
    const u = d.uptime_s, j = Math.floor(u / 86400), h = Math.floor(u % 86400 / 3600), m = Math.floor(u % 3600 / 60);
    $('diag').innerText = '⏱ ' + j + ' j ' + h + ' h ' + m + ' min · ' + d.rssi + ' dBm · RAM ' + Math.round(d.ram_libre / 1024) + ' Ko';
  }
  $('wifi_signal').innerText = d.wifi_pct + '% (' + T(d.wifi_txt) + ')';
  $('saison_val').innerText = T(d.saison_txt);
  $('txt_heure_dem_val').innerText = d.heure_prog == 24 ? '☀ ' + d.demarrage : d.heure_prog + ' h';
  $('txt_gmt_val').innerText = (d.gmt_effectif >= 0 ? '+' : '') + d.gmt_effectif + (d.dst_regle === 0 ? '' : ' (' + T(d.heure_ete ? 'ete' : 'hiver') + ')');
  $('statut').innerText = T(d.statut);
  $('mode').innerText = T(d.mode);
  $('btn_force_off').disabled = (d.mode !== 'MARCHE FORCÉE');
}

function libelleGraphique(h) {
  if (typeof h !== 'string') return '';
  const m = h.match(/^(\d{2}\/\d{2})\s+(\d{2})/);
  return m ? m[1] + ' ' + m[2] + 'h' : h;
}

function creerGraphique(g) {
  graphique = new Chart($('chartTemp').getContext('2d'), {
    data: {
      labels: g.heures,
      datasets: [
        { type: 'line', label: T('lbl_temp_eau'), data: g.valeurs, borderColor: '#0288d1', backgroundColor: 'rgba(2,136,209,0.05)',
          borderWidth: 2, tension: 0.35, pointRadius: 0, pointHoverRadius: 0, pointHitRadius: 8, fill: true, yAxisID: 'yTemp' },
        { type: 'bar', label: T('lbl_graph_barres'), data: barres(g), backgroundColor: 'rgba(76,175,80,0.4)',
          borderColor: '#4caf50', borderWidth: 1, barPercentage: 0.5, yAxisID: 'yFilt' }
      ]
    },
    options: {
      responsive: true, maintainAspectRatio: false,
      scales: {
        x: { grid: { display: false }, ticks: { autoSkip: true, maxTicksLimit: 6, maxRotation: 45, minRotation: 0,
              callback: function(v, i) {
                const l = this.chart.data.labels || [];
                const c = libelleGraphique(l[i]);
                return (i > 0 && c === libelleGraphique(l[i - 1])) ? '' : c;
              } } },
        yTemp: { type: 'linear', position: 'left', title: { display: true, text: '°C', color: '#0288d1' }, ticks: { color: '#0288d1' } },
        yFilt: { type: 'linear', position: 'right', min: 0, max: 24, grid: { display: false },
                 title: { display: true, text: T('lbl_axe_heures'), color: '#4caf50' }, ticks: { color: '#4caf50' } }
      }
    }
  });
}

const barres = g => g.filtration.map((v, i) => ({ x: g.jours[i], y: v }));

// Le graphique ne change qu'au plus toutes les minutes : inutile de le recharger toutes les 3 s
function chargerGraphique() {
  fetch('/graph-data').then(r => r.json()).then(g => {
    if (!graphique) return creerGraphique(g);
    graphique.data.labels = g.heures;
    graphique.data.datasets[0].data = g.valeurs;
    graphique.data.datasets[1].data = barres(g);
    graphique.update('none');
  }).catch(() => {});
}

function chargerDonnees() {
  fetch('/data').then(r => r.json()).then(d => {
    donnees = d;
    if (!selectsInitialises) {   // présélectionne les réglages actuels
      $('select_heure').value = d.heure_prog;
      $('select_gmt').value = d.gmt_effectif - (d.heure_ete ? 1 : 0);
      if (d.dst_regle !== undefined) $('select_dst').value = d.dst_regle;
      selectsInitialises = true;
    }
    afficherDonnees();
  }).catch(() => {});
}

function journalOuvert() { return !$('journal_contenu').hidden; }

function actualiserBoutonJournal() {
  $('btn_journal').textContent = T(journalOuvert() ? 'journal_masquer' : 'journal_afficher');
  $('btn_journal').setAttribute('aria-expanded', String(journalOuvert()));
}

function basculerJournal() {
  $('journal_contenu').hidden = journalOuvert();
  actualiserBoutonJournal();
  chargerJournal();
}

function chargerJournal() {
  if (!journalOuvert()) return;
  const liste = $('journal_contenu');
  fetch('/events').then(r => { if (!r.ok) throw new Error(); return r.json(); }).then(data => {
    const evts = Array.isArray(data.events) ? data.events : [];
    liste.replaceChildren();
    if (!evts.length) { liste.textContent = T('journal_vide'); return; }
    evts.slice().reverse().forEach(e => {
      const ligne = document.createElement('div');
      ligne.className = 'journal-ligne';
      ligne.textContent = String(e);
      liste.appendChild(ligne);
    });
  }).catch(() => { liste.textContent = T('journal_indispo'); });
}

function changerMode(m) {
  fetch('/set-mode?mode=' + m).then(r => { if (!r.ok) return r.text().then(t => { throw new Error(t); }); })
    .then(() => setTimeout(chargerDonnees, 1500)).catch(e => alert(e.message));
}
function basculerLumiere() { fetch('/toggle-light').then(chargerDonnees); }
function enregistrerConfig() {
  fetch('/set-config?h=' + $('select_heure').value + '&gmt=' + $('select_gmt').value + '&dst=' + $('select_dst').value)
    .then(r => { if (r.ok) alert(T('alert_save')); chargerDonnees(); });
}
function forcerNTP() {
  fetch('/sync-ntp').then(r => alert(T(r.ok ? 'alert_ntp_ok' : 'alert_ntp_err'))).catch(() => alert(T('alert_ntp_err')));
}

window.onload = function() {
  const nav = (navigator.language || 'en').substring(0, 2).toLowerCase();
  langue = ordreLangues.includes(nav) ? nav : 'en';
  for (let i = 0; i < 24; i++) $('select_heure').add(new Option(i + 'h', i));
  $('select_heure').add(new Option('☀ Solaire', 24));
  for (let i = -11; i <= 12; i++) $('select_gmt').add(new Option('GMT ' + (i >= 0 ? '+' : '') + i, i));
  traduireInterface();
  chargerDonnees();
  chargerGraphique();
  setInterval(chargerDonnees, 3000);
  setInterval(chargerGraphique, 60000);
  setInterval(chargerJournal, 10000);
};
</script>
</head>
<body>
<div class="container">
  <div class="lang-container"><button id="btn-lang-id" class="btn-lang" onclick="basculerLangue()">🇬🇧 EN</button></div>
  <img class="logo-img" src="https://lh3.googleusercontent.com/d/1Bxb00531-MNfYPLHYNCvubKgXI_dP7go" alt="PoolTimer ZR Logo">
  <h1>PoolTimer ZR V 2.2</h1>
  <div class="grid">
    <div class="card"><h3 data-i18n="lbl_heure_actuelle"></h3><div id="heure" class="valeur">--:--:--</div></div>
    <div class="card"><h3 data-i18n="lbl_mode_actuel"></h3><div id="mode" class="valeur">--</div>
      <div class="periode-texte"><span data-i18n="lbl_periode"></span><b id="saison_val">--</b></div></div>
    <div class="card card-full"><h3 data-i18n="lbl_temp_eau"></h3><div id="temp_eau" class="valeur">-- °C</div></div>
    <div class="card"><h3 data-i18n="lbl_statut_pompe"></h3><div id="statut" class="valeur">--</div></div>
    <div class="card"><h3 data-i18n="lbl_temps_filtration"></h3><div id="duree" class="valeur">-- h</div></div>
    <div class="card card-purple"><h3 data-i18n="lbl_signal_wifi"></h3><div id="wifi_signal" class="valeur">-- %</div></div>
  </div>

  <div class="graph-box">
    <h3 data-i18n="graph_titre"></h3>
    <div style="height: 240px; position: relative;"><canvas id="chartTemp"></canvas></div>
  </div>

  <div class="section">
    <h2 data-i18n="titre_lum"></h2>
    <button id="btn_lumiere" class="btn btn-green" onclick="basculerLumiere()"></button>
  </div>

  <div class="section">
    <h2 data-i18n="titre_cmd"></h2>
    <button class="btn btn-blue" data-i18n="btn_auto" onclick="changerMode('AUTO')"></button>
    <button class="btn btn-green" data-i18n="btn_force_on" onclick="changerMode('FORCE_ON')"></button>
    <button id="btn_force_off" class="btn btn-red" data-i18n="btn_force_off" onclick="changerMode('FORCE_OFF')" disabled></button>
  </div>

  <div class="section">
    <h2 data-i18n="titre_prog"></h2>
    <p><span data-i18n="lbl_start"></span><span id="txt_heure_dem_val" class="bleu">--</span> |
       GMT : <span id="txt_gmt_val" class="bleu">--</span></p>
    <div class="row-control">
      <label for="select_heure" data-i18n="lbl_modif_heure"></label><select id="select_heure"></select>
      <label for="select_gmt" data-i18n="lbl_modif_gmt"></label><select id="select_gmt"></select>
      <label for="select_dst" data-i18n="lbl_modif_dst"></label>
      <select id="select_dst" style="width: auto;">
        <option value="1" data-i18n="dst_europe"></option><option value="2" data-i18n="dst_amerique"></option>
        <option value="3" data-i18n="dst_australie"></option><option value="0" data-i18n="dst_aucune"></option>
      </select>
    </div>
    <button class="btn btn-blue" data-i18n="btn_enregistrer" onclick="enregistrerConfig()"></button>
    <div style="margin-top: 15px;"><button class="btn btn-blue" data-i18n="btn_sync_ntp" onclick="forcerNTP()"></button></div>
  </div>

  <div class="section">
    <h2 data-i18n="titre_journal"></h2>
    <button type="button" id="btn_journal" class="btn btn-blue" onclick="basculerJournal()" aria-expanded="false" aria-controls="journal_contenu"></button>
    <div id="journal_contenu" class="journal-liste" aria-live="polite" hidden></div>
  </div>
  <div class="notice-gratuite" data-i18n="notice_gratuite"></div>
  <div class="pied"><span data-i18n="lbl_ip_boitier"></span><b id="ip_adresse">--.--.--.--</b><br><span id="diag"></span></div>
</div>
</body>
</html>
)rawliteral";

// ======================================================================
//  SETUP
// ======================================================================
void setup() {
  Serial.begin(115200);
  for (int broche : {RELAY_PIN, LIGHT_PIN, LED_WIFI}) {
    pinMode(broche, OUTPUT);
    digitalWrite(broche, LOW);
  }

  sensors.begin();
  sensors.setWaitForConversion(false);   // lecture de la sonde sans bloquer le programme
  flashOK = LittleFS.begin(true);   // formate la zone LittleFS seulement si elle est illisible (ex. changement de schéma de partition)
  if (flashOK) {
    lireFichier("/temperatures.csv", lireLigneTemperature);
    lireFichier("/filtration.csv", lireLigneFiltration);
    lireFichier("/evenements.csv", lireLigneJournal);
  } else {
    Serial.println("[LFS] ECHEC : historiques conserves en RAM uniquement.");
  }

  prefs.begin("piscine", false);
  heureDemarrage = constrain(prefs.getInt("h_start", 8), 0, HEURE_SOLAIRE);
  dureeSolaireMin = constrain(prefs.getInt("d_sol", 360), 30, 24 * 60);
  tempEauActuelle = prefs.getFloat("t_eau", 20.0);
  temperatureAffichee = prefs.getFloat("t_aff", tempEauActuelle);
  temperatureAfficheeValide = isfinite(temperatureAffichee) && temperatureAffichee > -20.0f && temperatureAffichee < 60.0f;
  if (!temperatureAfficheeValide) temperatureAffichee = tempEauActuelle;
  modeHivernage = tempEauActuelle < 12.0f;
  int gmtLu = prefs.getInt("gmt_offset", 1);
  regleHeureEte = constrain(prefs.getInt("dst_regle", 1), 0, 3);
  gmtHiver = constrain(gmtLu, -11, 12);
  unsigned long msRestantes = prefs.getULong("ms_restantes", 0);
  codeDernierCycle = prefs.getInt("j_cycle", -1);

  // Reprise d'un cycle interrompu par une coupure de courant
  if (msRestantes > 0) {
    filtrationEnCours = true;
    repriseApresCoupure = true;
    debutFiltrationMs = millis();
    dureeTotaleFiltrationMs = msRestantes;
    dureeAfficheeInterface = msRestantes / 3600000.0f;
    digitalWrite(RELAY_PIN, HIGH);
  } else {
    dureeAfficheeInterface = calculerTempsFiltration(tempEauActuelle);
  }

  // Portail WiFiManager avec les deux réglages
  char txtHeure[6], txtGmt[6];
  snprintf(txtHeure, sizeof(txtHeure), "%d", heureDemarrage);
  snprintf(txtGmt, sizeof(txtGmt), "%d", gmtLu);
  WiFiManagerParameter paramHeure("h_start", "Default start hour (0-23, 24 = solar)", txtHeure, 6);
  WiFiManagerParameter paramGmt("gmt_offset", "Timezone / GMT offset", txtGmt, 6);
  wm.addParameter(&paramHeure);
  wm.addParameter(&paramGmt);
  wm.setSaveParamsCallback([]() { Serial.println("WiFiManager settings saved!"); });
  WiFi.setHostname(NOM_APPAREIL);          // nom affiché par la box (et souvent http://pooltimer ou http://pooltimer.home)
  wm.setHostname(NOM_APPAREIL);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(PUISSANCE_WIFI);       // dès la connexion initiale
  // Portail de configuration limité à 3 min : après une coupure de courant, la box peut redémarrer
  // plus lentement que l'ESP32. On démarre alors sans Wi-Fi ; loop() se reconnectera tout seul.
  wm.setConnectTimeout(15);       // 15 s maximum par tentative de connexion
  wm.setConnectRetries(3);        // 3 tentatives avant d'ouvrir le portail (la 1re échoue souvent sur ESP32-C3)
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("Pool_Timer_CONFIG")) {
    Serial.println("WiFi indisponible : demarrage sans reseau, reconnexion automatique ensuite");
    WiFi.mode(WIFI_STA);
    WiFi.begin();   // identifiants Wi-Fi déjà enregistrés
  }

  int h = atoi(paramHeure.getValue());
  if (h >= 0 && h <= HEURE_SOLAIRE) { heureDemarrage = h; prefs.putInt("h_start", h); }
  int g = atoi(paramGmt.getValue());
  if (g >= -11 && g <= 12) { gmtHiver = g; prefs.putInt("gmt_offset", g); }

  wifiEstConnecte = (WiFi.status() == WL_CONNECTED);
  synchroniserHorloge();
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  reglerRadio();
  Serial.print("ESP32 Connecté avec succès ! Adresse IP : ");
  Serial.println(WiFi.localIP());
  relancerMDNS();

  // --- Pages statiques ---
  server.on("/", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
  });
  server.on("/manifest.json", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", manifest_json);
  });
  server.on("/sw.js", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/javascript", sw_js);
  });

  // --- Données (page web + appli Android : noms des champs inchangés) ---
  server.on("/data", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc;
    struct tm t;
    bool heureValide = getLocalTime(&t, 100);
    bool ete = heureValide && t.tm_isdst > 0;
    char heure[10];
    if (heureValide) strftime(heure, sizeof(heure), "%H:%M:%S", &t);
    else strcpy(heure, "--:--:--");
    int pctWifi = getQualiteWifi();

    doc["heure"] = heure;
    doc["temp_eau"] = String(temperatureAffichee, 1);
    doc["defaut_sonde"] = defautSonde;
    doc["statut"] = digitalRead(RELAY_PIN) ? "ALLUMÉE" : "ÉTEINTE";
    doc["lumiere"] = digitalRead(LIGHT_PIN) == HIGH;
    doc["heure_prog"] = heureDemarrage;
    { int dm = demarrageMinutes(); char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", dm / 60, dm % 60); doc["demarrage"] = hm; }
    doc["gmt_effectif"] = gmtHiver + (ete ? 1 : 0);
    doc["heure_ete"] = ete;
    doc["dst_regle"] = regleHeureEte;
    doc["uptime_s"] = millis() / 1000;             // diagnostic : temps depuis le dernier démarrage
    doc["ram_libre"] = ESP.getFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    doc["ip_esp"] = WiFi.localIP().toString();
    doc["wifi_pct"] = pctWifi;
    doc["wifi_txt"] = texteWifi(pctWifi);
    doc["saison_txt"] = texteSaison();
    doc["mode"] = texteMode();
    doc["duree"] = String(dureeAfficheeInterface, 1);
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/graph-data", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc;
    JsonArray heures = doc["heures"].to<JsonArray>();
    JsonArray valeurs = doc["valeurs"].to<JsonArray>();
    JsonArray jours = doc["jours"].to<JsonArray>();
    JsonArray filtration = doc["filtration"].to<JsonArray>();
    for (int i = 0; i < histoTemp.nb; i++) {
      heures.add((char*)histoTemp[i].date);
      valeurs.add(String(histoTemp[i].temp, 1));
    }
    for (int i = 0; i < histoFilt.nb; i++) {
      jours.add((char*)histoFilt[i].jour);
      filtration.add(String(histoFilt[i].heures, 1));
    }
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/events", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc;
    doc["sequence"] = sequenceEvenement;              // utilisé par l'appli pour ses notifications
    JsonArray arr = doc["events"].to<JsonArray>();
    char ligne[96];
    for (int i = 0; i < journal.nb; i++) {
      snprintf(ligne, sizeof(ligne), "%lu;%s;%s", (unsigned long)journal[i].seq, journal[i].date, journal[i].msg);
      arr.add(ligne);
    }
    serializeJson(doc, *response);
    request->send(response);
  });

  // --- Commandes ---
  server.on("/set-mode", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("mode")) { request->send(400, "text/plain", "Mode manquant"); return; }
    String m = request->getParam("mode")->value();
    if (defautSonde || modeActuel == MODE_HORS_GEL || tempEauActuelle <= 3.0f) {
      request->send(409, "text/plain", "Protection active (Défaut sonde ou Hors-gel)");
      return;
    }
    if (m == "FORCE_ON") commandeEnAttente = CMD_FORCE_ON;
    else if (m == "AUTO") commandeEnAttente = CMD_AUTO;
    else if (m == "FORCE_OFF") {
      if (modeActuel != MODE_FORCE_ON && commandeEnAttente != CMD_FORCE_ON) {
        request->send(409, "text/plain", "Arret disponible uniquement en marche forcee");
        return;
      }
      commandeEnAttente = CMD_FORCE_OFF;
    } else {
      request->send(400, "text/plain", "Mode inconnu");
      return;
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/toggle-light", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    digitalWrite(LIGHT_PIN, !digitalRead(LIGHT_PIN));
    request->send(200, "text/plain", "OK");
  });

  server.on("/set-config", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    bool modif = false;
    if (request->hasParam("h")) {
      int h = request->getParam("h")->value().toInt();
      if (h >= 0 && h <= HEURE_SOLAIRE) { heureDemarrage = h; prefs.putInt("h_start", h); modif = true; }
    }
    if (request->hasParam("gmt")) {
      int g = request->getParam("gmt")->value().toInt();
      if (g >= -11 && g <= 12) { gmtHiver = g; prefs.putInt("gmt_offset", g); modif = true; }
    }
    if (request->hasParam("dst")) {
      int r = request->getParam("dst")->value().toInt();
      if (r >= 0 && r <= 3) { regleHeureEte = r; prefs.putInt("dst_regle", r); modif = true; }
    }
    if (modif) resynchroHorlogeDemandee = true;
    request->send(200, "text/plain", "OK");
  });

  server.on("/sync-ntp", WebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    resynchroHorlogeDemandee = true;
    request->send(200, "text/plain", "OK");
  });

  server.begin();

  // Trace chaque démarrage dans le journal (visible dans la page web et l'appli)
  char evt[48];
  snprintf(evt, sizeof(evt), "DEMARRAGE;%s", raisonRedemarrage());
  journaliser(evt);
  if (wifiEstConnecte) {
    snprintf(evt, sizeof(evt), "WIFI_CONNECTE;en %lu s", millis() / 1000);
    journaliser(evt);
  }
}

// ======================================================================
//  LOOP
// ======================================================================
void loop() {
  // --- WiFi : reconnexion et LED ---
  static unsigned long derniereTentativeConnexion = 0;
  static unsigned long debutCoupureWifiMs = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiEstConnecte) {
      Serial.println("WiFi perdu : reconnexion en cours");
      debutCoupureWifiMs = millis();
    }
    wifiEstConnecte = false;
    digitalWrite(LED_WIFI, LOW);
    if (derniereTentativeConnexion == 0 || millis() - derniereTentativeConnexion >= 15000UL) {
      derniereTentativeConnexion = millis();
      WiFi.reconnect();
    }
  } else if (!wifiEstConnecte) {
    wifiEstConnecte = true;
    derniereTentativeConnexion = 0;
    Serial.println("WiFi reconnecte : " + WiFi.localIP().toString());
    reglerRadio();
    synchroniserHorloge();
    relancerMDNS();
    if (debutCoupureWifiMs != 0) {      // trace la coupure dans le journal, avec sa durée
      char evt[48];
      snprintf(evt, sizeof(evt), "WIFI_RETABLI;coupure de %lu s", (millis() - debutCoupureWifiMs) / 1000);
      journaliser(evt);
      debutCoupureWifiMs = 0;
    } else {                             // première connexion, après un démarrage sans Wi-Fi
      char evt[48];
      snprintf(evt, sizeof(evt), "WIFI_CONNECTE;en %lu s", millis() / 1000);
      journaliser(evt);
    }
  }
  static unsigned long dernierClignotement = 0;
  static bool etatLed = LOW;
  if (wifiEstConnecte && millis() - dernierClignotement >= 1000UL) {
    dernierClignotement = millis();
    etatLed = !etatLed;
    digitalWrite(LED_WIFI, etatLed);
  }

  // --- Cumul du temps de pompe de la journée ---
  if (digitalRead(RELAY_PIN) == HIGH) {
    if (dernierCalculCumulMs == 0) dernierCalculCumulMs = millis();
    cumulFiltrationJourMs += millis() - dernierCalculCumulMs;
    dernierCalculCumulMs = millis();
  } else {
    dernierCalculCumulMs = 0;
  }

  // --- Sauvegarde du temps restant (reprise après coupure) ---
  static unsigned long derniereSauvegardeReprise = 0;
  if (filtrationEnCours && millis() - derniereSauvegardeReprise >= 30000UL) {
    derniereSauvegardeReprise = millis();
    unsigned long ecoule = millis() - debutFiltrationMs;
    if (dureeTotaleFiltrationMs > ecoule) prefs.putULong("ms_restantes", dureeTotaleFiltrationMs - ecoule);
  }

  // --- Commandes reçues de la page web ou de l'appli ---
  if (resynchroHorlogeDemandee) {
    resynchroHorlogeDemandee = false;
    synchroniserHorloge();
    Serial.println("Synchronisation NTP demandée.");
  }
  executerCommande();

  // Sécurités actives en permanence, même si l'heure n'est pas encore connue (panne d'internet)
  // --- Acquisition continue de la température (toutes les 2 s, sans blocage) ---
  if (gererSonde()) {
    if (digitalRead(RELAY_PIN) == HIGH && mesureRecente()) {
      temperatureAffichee = tempEauActuelle;
      temperatureAfficheeValide = true;
    }
  }

  // --- Hors-gel 24h/24, sortie 1 h après être repassé au-dessus de 4 °C ---
  if (!defautSonde) {
    if (tempEauActuelle <= 3.0f) {
      if (modeActuel != MODE_HORS_GEL) {
        annulerCycles();
        debutMarcheForceeMs = 0;
        journaliser("HORS_GEL_ACTIVE_24H24");
      }
      modeActuel = MODE_HORS_GEL;
      temporisationHorsGelEnCours = false;
      digitalWrite(RELAY_PIN, HIGH);
    } else if (modeActuel == MODE_HORS_GEL) {
      if (tempEauActuelle > 4.0f && !temporisationHorsGelEnCours) {
        temporisationHorsGelEnCours = true;
        debutDepassement4C_Ms = millis();
        journaliser("HORS_GEL_DEPASSEMENT_4C_TEMPO_1H_DEBUT");
      }
      if (temporisationHorsGelEnCours && millis() - debutDepassement4C_Ms >= TEMPO_SORTIE_HORS_GEL) {
        journaliser("HORS_GEL_TERMINE_APRES_TEMPO_1H");
        modeActuel = MODE_AUTO;
        temporisationHorsGelEnCours = false;
        digitalWrite(RELAY_PIN, LOW);
      } else {
        digitalWrite(RELAY_PIN, HIGH);
      }
    }
  }

  // --- Défaut sonde : pompe en marche continue ---
  if (defautSonde) {
    digitalWrite(RELAY_PIN, HIGH);
    dureeAfficheeInterface = 24.0f;
  }

  // --- Marche forcée (2 h) : minuterie indépendante de l'heure ---
  if (!defautSonde && modeActuel == MODE_FORCE_ON) {
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
  }

  // --- Heure (une seule lecture par tour) ---
  struct tm maintenant;
  bool heureValide = getLocalTime(&maintenant, 50);
  int heureActuelle = heureValide ? maintenant.tm_hour : -1;
  int minuteJour = heureValide ? maintenant.tm_hour * 60 + maintenant.tm_min : -1;
  int minuteDemarrage = demarrageMinutes();
  if (heureValide && evenementsEnAttente > 0) corrigerDatesJournal();
  if (!heureValide && !repriseApresCoupure) return;

  // --- Historique journalier de filtration ---
  static int dernierJourEnregistre = -1;
  static char dateCumulFiltration[8] = "";
  if (heureValide) {
    if (dernierJourEnregistre == -1) {
      strftime(dateCumulFiltration, sizeof(dateCumulFiltration), "%d/%m", &maintenant);
      if (histoFilt.nb > 0 && strcmp(histoFilt.dernier().jour, dateCumulFiltration) == 0)
        cumulFiltrationJourMs = (unsigned long)(histoFilt.dernier().heures * 3600000.0f + 0.5f);
      dernierJourEnregistre = maintenant.tm_mday;
    } else if (maintenant.tm_mday != dernierJourEnregistre) {
      if (dateCumulFiltration[0]) {
        ajouterFiltrationAuGraphique(cumulFiltrationJourMs / 3600000.0f, dateCumulFiltration);
        sauvegarderFiltration();
      }
      cumulFiltrationJourMs = 0;
      dernierCalculCumulMs = millis();
      dernierJourEnregistre = maintenant.tm_mday;
      strftime(dateCumulFiltration, sizeof(dateCumulFiltration), "%d/%m", &maintenant);
    }
    ajouterFiltrationAuGraphique(cumulFiltrationJourMs / 3600000.0f, dateCumulFiltration);
  }

  bool pompeEnMarche = (digitalRead(RELAY_PIN) == HIGH);
  static bool pompeEtaitEnMarche = false;
  if (historiqueFiltrationModifie &&
      ((pompeEtaitEnMarche && !pompeEnMarche) ||
       millis() - dernierEnregistrementFiltrationMs >= INTERVALLE_SAUVEGARDE_FILTRATION)) {
    sauvegarderFiltration();
  }

  // --- Relevés pour le graphique (après 5 min de circulation, puis toutes les 2 h) ---
  static unsigned long debutCirculationMs = 0, dernierReleveGraphMs = 0, derniereTentativeGraphMs = 0;
  static bool premierReleveFait = false;
  unsigned long maintenantGraph = millis();
  if (pompeEnMarche && !pompeEtaitEnMarche) {
    debutCirculationMs = maintenantGraph;
    premierReleveFait = false;
    dernierReleveGraphMs = 0;
    derniereTentativeGraphMs = 0;
  }
  if (!pompeEnMarche && pompeEtaitEnMarche && temperatureAfficheeValide) {
    prefs.putFloat("t_aff", temperatureAffichee);
  }
  if (!pompeEnMarche) {
    premierReleveFait = false;
    dernierReleveGraphMs = 0;
  } else if (maintenantGraph - debutCirculationMs >= DELAI_STABILISATION_EAU &&
             (!premierReleveFait || maintenantGraph - dernierReleveGraphMs >= INTERVALLE_RELEVE_GRAPH) &&
             (derniereTentativeGraphMs == 0 || maintenantGraph - derniereTentativeGraphMs >= 10000UL)) {
    derniereTentativeGraphMs = maintenantGraph;
    if (heureValide && maintenant.tm_year >= 124 && mesureRecente() && isfinite(tempEauActuelle)) {
      temperatureAffichee = tempEauActuelle;
      temperatureAfficheeValide = true;
      ajouterAuGraphique(temperatureAffichee);
      dernierReleveGraphMs = millis();
      premierReleveFait = true;
    }
  }
  pompeEtaitEnMarche = pompeEnMarche;

  if (defautSonde) return;   // marche continue : pas de cycle automatique

  if (heureActuelle == 0) controleNocturneFaitAujourdhui = false;
  int aujourdhui = heureValide ? maintenant.tm_year * 1000 + maintenant.tm_yday : -1;
  bool cycleSaisonnierDejaLance = heureValide && codeDernierCycle == aujourdhui;
  if (heureValide && (jourRepriseSaison != maintenant.tm_yday || anneeRepriseSaison != maintenant.tm_year))
    repriseSaisonConfirmee = false;
  if (tempEauActuelle > 5.0 && !cycleControleNocturneEnCours) dernierControleHiverMs = 0;

  if (modeActuel != MODE_AUTO) return;   // marche forcée et hors-gel : gérés plus haut

  // --- Reprise après coupure : un cycle d'un jour précédent n'est terminé qu'avant l'heure programmée ---
  if (repriseApresCoupure && heureValide && !repriseVerifiee) {
    repriseVerifiee = true;
    if (codeDernierCycle != aujourdhui && minuteJour >= minuteDemarrage) {
      finCycleSaison("REPRISE_ABANDONNEE_CYCLE_DU_JOUR");
      dureeAfficheeInterface = 0.0f;
    }
  }

  // --- Hivernage : contrôle de 5 min à 5 h, et toutes les 4 h si l'eau est entre 3 et 5 °C ---
  bool controleQuotidien = (heureActuelle == 5 && !controleNocturneFaitAujourdhui);
  bool controleFroid = dernierControleHiverMs != 0 &&
      millis() - dernierControleHiverMs >= INTERVALLE_CONTROLE_FROID &&
      tempEauActuelle > 3.0 && tempEauActuelle <= 5.0;
  if (!filtrationEnCours && !cycleControleNocturneEnCours && modeHivernage && (controleQuotidien || controleFroid)) {
    cycleControleNocturneEnCours = true;
    controleHiverSupplementaire = controleFroid && !controleQuotidien;
    debutFiltrationMs = millis();
    dernierControleHiverMs = debutFiltrationMs;
    if (controleQuotidien) controleNocturneFaitAujourdhui = true;
    releveFinCycleFait = false;
    digitalWrite(RELAY_PIN, HIGH);
    journaliser(controleHiverSupplementaire ? "CONTROLE_HIVER_SUPPLEMENTAIRE_DEBUT" : "CONTROLE_HIVER_05H_DEBUT");
  }
  if (cycleControleNocturneEnCours) {
    dureeAfficheeInterface = 0.1;
    if (millis() - debutFiltrationMs >= DELAI_STABILISATION_EAU) {
      if (mesureRecente()) {
        temperatureAffichee = tempEauActuelle;
        temperatureAfficheeValide = true;
        ajouterAuGraphique(temperatureAffichee);
        prefs.putFloat("t_eau", tempEauActuelle);
        prefs.putFloat("t_aff", temperatureAffichee);
        char msg[48];
        snprintf(msg, sizeof(msg), "CONTROLE_HIVER_MESURE_VALIDEE;%.1fC", tempEauActuelle);
        journaliser(msg);
      } else {
        journaliser("CONTROLE_HIVER_MESURE_INVALIDE");
      }
      releveFinCycleFait = true;
      actualiserSaisonApresStabilisation();
      if (heureValide && !modeHivernage) {
        repriseSaisonConfirmee = true;
        jourRepriseSaison = maintenant.tm_yday;
        anneeRepriseSaison = maintenant.tm_year;
      }
      cycleControleNocturneEnCours = false;
      journaliser("CONTROLE_HIVER_TERMINE_5MIN");
      controleNocturneFaitAujourdhui = controleNocturneFaitAujourdhui || !controleHiverSupplementaire;
      controleHiverSupplementaire = false;
      dureeAfficheeInterface = 0.0;
      if (tempEauActuelle > 3.0) digitalWrite(RELAY_PIN, LOW);
    }
  }

  // --- Saison : démarrage du cycle à l'heure programmée ---
  // Rattrapage : si l'heure programmée est passée sans que le démarrage ait pu être décidé aujourd'hui
  // (coupure de courant, heure inconnue, marche forcée en cours...), le cycle démarre maintenant.
  static int jourDecisionDemarrage = -1;
  bool rattrapage = minuteJour >= minuteDemarrage + 60 &&
                    (jourDecisionDemarrage != aujourdhui || repriseSaisonConfirmee);
  if (!filtrationEnCours && !cycleControleNocturneEnCours &&
      ((minuteJour >= minuteDemarrage && minuteJour < minuteDemarrage + 60) || rattrapage) &&
      !repriseApresCoupure && heureValide && !cycleSaisonnierDejaLance) {
    jourDecisionDemarrage = aujourdhui;
    float dureeInitiale = modeHivernage ? 0.0f : calculerTempsFiltration(tempEauActuelle);
    if (dureeInitiale > 0.0) {
      codeDernierCycle = aujourdhui;
      prefs.putInt("j_cycle", codeDernierCycle);
      filtrationEnCours = true;
      journaliser("FILTRATION_SAISON_DEBUT");
      repriseSaisonConfirmee = false;
      debutFiltrationMs = millis();
      releveFinCycleFait = false;
      releveDebutCycleFait = false;
      digitalWrite(RELAY_PIN, HIGH);
      dureeTotaleFiltrationMs = (unsigned long)(dureeInitiale * 3600000.0f);
      dureeAfficheeInterface = dureeInitiale;
    } else {
      dureeAfficheeInterface = 0.0;
    }
  }

  if (!filtrationEnCours) return;

  // --- Cycle de filtration en cours ---
  unsigned long tempsEcoule = millis() - debutFiltrationMs;
  if (!repriseApresCoupure) {
    // Après 5 min de circulation : mise à jour de la saison, puis durée recalculée en continu
    if (!releveDebutCycleFait && tempsEcoule >= DELAI_STABILISATION_EAU) {
      actualiserSaisonApresStabilisation();
      releveDebutCycleFait = true;
      prefs.putFloat("t_eau", tempEauActuelle);
    }
    if (releveDebutCycleFait) {
      float duree = modeHivernage ? 0.0f : calculerTempsFiltration(tempEauActuelle);
      dureeTotaleFiltrationMs = (unsigned long)(duree * 3600000.0f);
      dureeAfficheeInterface = duree;
      // Mode solaire : la durée du jour fixe l'heure de démarrage du lendemain
      if (heureDemarrage == HEURE_SOLAIRE && duree > 0.0f && tempEauActuelle < 27.0f) {
        int dm = constrain((int)(duree * 60.0f + 0.5f), 30, 24 * 60);
        if (dm != dureeSolaireMin) { dureeSolaireMin = dm; prefs.putInt("d_sol", dm); }
      }
    }
  } else {
    long resteMs = (long)dureeTotaleFiltrationMs - (long)tempsEcoule;
    dureeAfficheeInterface = resteMs > 0 ? resteMs / 3600000.0f : 0.0f;
  }

  if (dureeTotaleFiltrationMs == 0) {
    finCycleSaison("FILTRATION_SAISON_STOP");
    dureeAfficheeInterface = 0.0;
    return;
  }

  // 30 s avant la fin : mémorise la température pour le prochain démarrage
  if (!releveFinCycleFait && dureeTotaleFiltrationMs > 30000 && tempsEcoule >= dureeTotaleFiltrationMs - 30000) {
    releveFinCycleFait = true;
    prefs.putFloat("t_eau", tempEauActuelle);
  }

  if (tempsEcoule >= dureeTotaleFiltrationMs) finCycleSaison("FILTRATION_SAISON_TERMINEE");
}
