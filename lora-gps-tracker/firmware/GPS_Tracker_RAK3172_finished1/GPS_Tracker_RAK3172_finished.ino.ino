/*******************************************************************************
 * GPS-Tracker  –  RAK3172 (RUI3)  +  u-blox SAM-M10Q  +  LIS3DH
 * Firmware v7.9e  (= v7.9c + fester SF7, ADR aus)
 * -----------------------------------------------------------------------------
 * Autor: Dominik Walker (Maturaarbeit KMSU)
 *Code wurde mit Hilfe von KI omptimiert
 *******************************************************************************/

#include <Wire.h>

/*==============================================================================
 *  1) KONFIGURATION – hier alles einstellen
 *============================================================================*/

/* ---- Sendeintervall / GPS ---- */
#define SEND_INTERVAL_S        300     // Standard-Intervall Normalmodus (300 = 5 min)
#define MODE_NORMAL            0
#define MODE_CONTINUOUS        1
#define MODE_THEFT             2
#define DEFAULT_MODE           MODE_NORMAL
#define DEFAULT_HEARTBEAT_H    12      // Diebstahlschutz: Lebenszeichen alle x Stunden
#define CONFIG_FPORT           10
#define MIN_INTERVAL_NORMAL_S  60
#define MIN_INTERVAL_CONT_S    15
#define GPS_CONT_TIMEOUT_S     10      // Dauer-Tracking: max. Wartezeit auf neuen Fix
#define GPS_FIX_TIMEOUT_S      60      // max. Suchzeit fuer einen Fix (Warmstart dank V_BCKP: meist < 5 s)
#define SEND_WITHOUT_FIX       1       // 1 = auch ohne Fix senden (nur Batterie)
#define GPS_MIN_SATS           6       // ab so vielen Satelliten wird ueberhaupt gewertet
#define GPS_MIN_FIX_TYPE       3       // 2 = 2D-Fix reicht, 3 = nur 3D-Fix (empfohlen)
#define GPS_MAX_HACC_MM        2500    // max. akzeptierte horiz. Ungenauigkeit in MILLIMETERN
                                        // (2500 = 2.5 m). Der SAM-M10Q schafft realistisch
                                        // ca. 1.2-2 m, 1 m wird praktisch nie erreicht ->
                                        // dann sucht er jedes Mal bis zum Timeout (Akku!).
                                        // Die Mittelung danach verbessert die Position weiter.

/* ---- NEU: Genauigkeit ---- */
#define GPS_SETTLE_S           2       // nach dem ersten guten Fix noch so lange weiter messen
                                        // und mitteln (0 = aus, sofort senden wie bisher)
#define GPS_MOVING_SPEED_CMS   50      // ab dieser Geschwindigkeit (cm/s) gilt der Tracker als
                                        // "in Bewegung" -> nicht mitteln, letzte Position nehmen
#define GPS_DYN_MODEL          0       // Bewegungsmodell: 0 = Portable (Standard),
                                        // 3 = Fussgaenger, 4 = Auto, 6 = Flugzeug <1g
#define GPS_USE_SBAS           1       // 1 = EGNOS-Korrekturdaten nutzen (Europa)

/* ---- Energiesparen ---- */
#define USE_LPM                1       // RUI3 Low-Power-Modus. Falls der Tracker beim
                                        // GPS haengen bleibt, testweise auf 0 setzen.

/* ---- Beschleunigungssensor ---- */
#define USE_ACCEL              1       // 0 = LIS3DH komplett aus (Power-Down)
#define ACCEL_SEND_XYZ         0       // 0 = Sensor nur fuer Wake-on-Motion, keine Messwerte im Payload
#define ACCEL_WAKE_ON_MOTION   1       // 1 = Bewegung weckt den Tracker
#define ACCEL_THRESHOLD_MG     250     // Auslöseschwelle in mg (16 mg Schritte)
#define MOTION_MIN_INTERVAL_S  60      // Sperrzeit nach Bewegungs-Uplink
#define ACCEL_DURATION_SAMPLES 2       // Bewegung muss mind. so viele Messungen (je 100 ms)
                                        // ueber der Schwelle liegen -> filtert Klopfen/Vibration

/* ---- LoRaWAN ---- */
#define LORAWAN_BAND           4       // 4 = EU868 (5 = US915, 6 = AU915)
#define LORAWAN_USE_OTAA       1       // 1 = OTAA, 0 = ABP
#define LORAWAN_ADR            0       // 0 = ADR aus -> fester SF (besser fuer mobile Tracker)
#define LORAWAN_DR             5       // EU868: 5=SF7 4=SF8 3=SF9 2=SF10 1=SF11 0=SF12
#define LORAWAN_TXP            0       // 0 = max. Sendeleistung (EU868: 16 dBm)
#define LORAWAN_FPORT          2
#define LORAWAN_CONFIRMED      0       // 0 = unconfirmed, spart Airtime/Batterie
#define JOIN_TIMEOUT_S         120

#define USE_FIXED_CHANNEL      0
#define FIXED_CHANNEL_MASK     0x0001      // nur relevant, wenn oben auf 1
#define FIXED_CHANNEL_FREQ     868100000   // nur relevant, wenn oben auf 1
#define USE_CHS_API            0           // nur relevant, wenn oben auf 1

/* ---- OTAA-Keys (MSB, so wie sie im Netzwerkserver stehen) ---- */
// Eigene Schlüssel aus der TTN-Konsole eintragen. Hier aus Sicherheitsgründen auf 0 gesetzt.
uint8_t node_device_eui[8]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t node_app_eui[8]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t node_app_key[16]    = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* ---- ABP-Keys (nur wenn LORAWAN_USE_OTAA = 0) ---- */
uint8_t node_dev_addr[4]    = {0x00, 0x00, 0x00, 0x00};
uint8_t node_nwk_skey[16]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t node_app_skey[16]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* ---- Debug-Ausgabe über UART2 ---- */
#define DEBUG_ENABLE           1
#if DEBUG_ENABLE
  #define DBG(...)    Serial.printf(__VA_ARGS__)
  #define DBGLN(x)    Serial.println(x)
#else
  #define DBG(...)
  #define DBGLN(x)
#endif

/*==============================================================================
 *  2) PINS (laut Schema)
 *============================================================================*/
#define PIN_GPS_EN   PA0     // LOW = GPS eingeschaltet (P-MOSFET Q3)
#define PIN_WAKE     PA8     // INT1 LIS3DH (active LOW), nur interner Pull-up
#define PIN_VBAT     PB3     // ADC, Spannungsteiler R12/R13

#define GPS_BAUD     9600    // GETESTET & bestaetigt korrekt (NICHT 38400!)

/*==============================================================================
 *  3) BATTERIEMESSUNG
 *  Teiler: R12 = 470k (oben, an Vout) / R13 = 1M (unten, an GND)
 *  V_ADC = V_BAT * 1000 / (470 + 1000) = V_BAT * 0.6803
 *  V_BAT = V_ADC * 1.4700
 *============================================================================*/
#define VBAT_R_TOP        470000.0f
#define VBAT_R_BOTTOM    1000000.0f
#define VBAT_DIVIDER     ((VBAT_R_TOP + VBAT_R_BOTTOM) / VBAT_R_BOTTOM)  // 1.47
#define ADC_VREF_MV       3300.0f
#define ADC_MAX_COUNTS    4095.0f   // 12 Bit. Falls Werte 4x zu klein: 1023.0f
#define VBAT_CALIB        1.000f    // Feinabgleich mit dem Multimeter
#define ADC_SAMPLES       16

/* NEU: Die ADC-Referenz des RAK3172 ist seine eigene Versorgung (VDD), nicht
 * fix 3.3 V. Weicht VDD ab (LDO-Toleranz, LDO geht bei leerem Akku in Dropout),
 * stimmt die Rechnung nicht mehr - die Spannung wird dann zu HOCH angezeigt,
 * je leerer der Akku ist. Mit 1 wird VDD intern gemessen und eingerechnet. */
#define VBAT_USE_VDD_COMP 0   // AUS: api.system.bat.get() liefert beim RAK3172 nicht
                              // VDD (gemessen 4200 mV) -> unbrauchbar, kostet nur Zeit

/*==============================================================================
 *  4) LIS3DH – Register
 *============================================================================*/
#define LIS3DH_ADDR        0x18
#define LIS3DH_WHO_AM_I    0x0F
#define LIS3DH_CTRL_REG0   0x1E   // Bit7 SDO_PU_DISC: interner Pull-up an SDO/SA0
#define LIS3DH_CTRL_REG1   0x20
#define LIS3DH_CTRL_REG2   0x21
#define LIS3DH_CTRL_REG3   0x22
#define LIS3DH_CTRL_REG4   0x23
#define LIS3DH_CTRL_REG5   0x24
#define LIS3DH_CTRL_REG6   0x25
#define LIS3DH_REFERENCE   0x26
#define LIS3DH_OUT_X_L     0x28
#define LIS3DH_INT1_CFG    0x30
#define LIS3DH_INT1_SRC    0x31
#define LIS3DH_INT1_THS    0x32
#define LIS3DH_INT1_DUR    0x33

/*==============================================================================
 *  5) GLOBALE VARIABLEN
 *============================================================================*/
typedef struct {
    bool     valid;        // erfuellt die Genauigkeits-Kriterien (siehe unten)
    int32_t  lat_1e7;      // Grad * 10^7  (Google-Format, z.B. 47.1234567)
    int32_t  lon_1e7;
    int16_t  alt_m;
    uint8_t  sats;
    uint8_t  fixType;      // 0=kein Fix, 2=2D, 3=3D, ...
    uint32_t hacc_mm;      // horizontale Genauigkeit in mm (0 = unbekannt)
    uint8_t  hour, minute, second;
    bool     sbas;         // Loesung mit Korrekturdaten (EGNOS) berechnet
    bool     moving;       // Geschwindigkeit ueber GPS_MOVING_SPEED_CMS
    bool     averaged;     // gesendete Position ist ein Mittelwert
    uint32_t first_valid_ms; // Zeitpunkt des ersten guten Fixes (0 = noch keiner)
} gps_fix_t;

/* Mittelwert-Bildung (relativ zur ersten Position, damit nichts ueberlaeuft) */
static int32_t  avg_ref_lat, avg_ref_lon;
static int64_t  avg_sum_lat, avg_sum_lon, avg_sum_hmsl;
static uint16_t avg_n;

static gps_fix_t  g_fix;
static uint16_t   g_batt_mv      = 0;
static int16_t    g_acc_mg[3]    = {0, 0, 0};
static bool       g_accel_ok     = false;
static bool       g_joined       = false;
static volatile bool g_motion    = false;   // vom Interrupt gesetzt
static bool       g_motion_uplink = false;
static volatile bool g_tx_done   = false;
static uint32_t   g_last_motion_ms = 0;
static uint32_t   g_uplink_count = 0;

/* NEU: Konfiguration (im Flash gespeichert) */
#define CFG_MAGIC  0xA6
typedef struct {
    uint8_t  magic;
    uint8_t  mode;
    uint16_t interval_s;
    uint8_t  heartbeat_h;
    uint8_t  thresh_16mg;      // Bewegungsschwelle in 16-mg-Schritten
} tracker_cfg_t;

static tracker_cfg_t g_cfg;
static volatile bool g_cfg_pending = false;   // im Downlink-Callback gesetzt
static tracker_cfg_t g_cfg_new;
static bool          g_gps_on = false;         // Dauer-Tracking: GPS bleibt an
static bool          g_quick_next = false;     // nach Modus-Wechsel bald bestaetigen

/* UBX-Protokoll Zustand (siehe Abschnitt 9) */
enum UbxState { U_SYNC1, U_SYNC2, U_CLASS, U_ID, U_LEN1, U_LEN2, U_PAYLOAD, U_CKA, U_CKB };
static UbxState  u_state = U_SYNC1;
static uint8_t   u_class, u_id;
static uint16_t  u_len, u_pidx;
static uint8_t   u_payload[92];  // reicht fuer NAV-PVT (92 Byte Payload)
static uint8_t   u_cka, u_ckb, u_rolla, u_rollb;
static volatile bool s_ack_received = false;
static uint8_t        s_ack_class = 0, s_ack_id = 0;

/*==============================================================================
 *  6) HILFSFUNKTIONEN
 *============================================================================*/
static void report(const char *what, bool ok)
{
    DBG("  %-22s : %s\r\n", what, ok ? "OK" : "FAIL");
}

/* Koordinate als Text im Google-Format, z.B. "47.1234567" – ohne float-printf */
static void format_coord(int32_t v_1e7, char *out, size_t out_len)
{
    const char *sign = "";
    if (v_1e7 < 0) { sign = "-"; v_1e7 = -v_1e7; }
    snprintf(out, out_len, "%s%ld.%07ld",
             sign, (long)(v_1e7 / 10000000L), (long)(v_1e7 % 10000000L));
}

/*==============================================================================
 *  7) BATTERIE
 *============================================================================*/
static uint16_t read_battery_mv(void)
{
    /* 1) Tatsaechliche Referenzspannung (VDD des RAK3172) bestimmen */
    float vref_mv = ADC_VREF_MV;
#if VBAT_USE_VDD_COMP
    float vdd = api.system.bat.get();          // RUI3: misst VDD intern (VREFINT)
    if (vdd > 1.8f && vdd < 3.7f) vref_mv = vdd * 1000.0f;   // nur plausible Werte
    else DBG("  VDD-Messung unplausibel (%u mV) - nehme %u mV\r\n",
             (unsigned)(vdd * 1000.0f), (unsigned)ADC_VREF_MV);
    analogReadResolution(12);                  // sicherheitshalber wieder 12 Bit
#endif

    for (uint8_t i = 0; i < 4; i++) { (void)analogRead(PIN_VBAT); delay(5); }

    /* 3) Messen, sortieren, Ausreisser oben/unten weglassen */
    uint16_t s[ADC_SAMPLES];
    for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
        s[i] = (uint16_t)analogRead(PIN_VBAT);
        delay(5);
    }
    for (uint8_t i = 1; i < ADC_SAMPLES; i++) {           // Insertion-Sort
        uint16_t v = s[i]; int8_t j = i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    uint32_t sum = 0;
    for (uint8_t i = ADC_SAMPLES / 4; i < ADC_SAMPLES * 3 / 4; i++) sum += s[i];
    float raw   = (float)sum / (float)(ADC_SAMPLES / 2);

    float v_adc = raw * vref_mv / ADC_MAX_COUNTS;
    float v_bat = v_adc * VBAT_DIVIDER * VBAT_CALIB;

    DBG("Batterie: VDD %u mV | ADC roh %u (min %u, max %u) -> %u mV am Pin -> %u mV Akku\r\n",
        (unsigned)vref_mv, (unsigned)raw, s[0], s[ADC_SAMPLES - 1],
        (unsigned)v_adc, (unsigned)v_bat);
    return (uint16_t)v_bat;
}

/*==============================================================================
 *  8) LIS3DH
 *============================================================================*/
static bool lis_write(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(LIS3DH_ADDR);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        DBG("  LIS3DH I2C Fehler beim Schreiben (Reg 0x%02X, Code %u)\r\n", reg, err);
        return false;
    }
    return true;
}

static uint8_t lis_read(uint8_t reg)
{
    Wire.beginTransmission(LIS3DH_ADDR);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
        DBG("  LIS3DH I2C Fehler beim Adressieren (Reg 0x%02X, Code %u)\r\n", reg, err);
        return 0;
    }
    Wire.requestFrom((uint8_t)LIS3DH_ADDR, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    DBGLN("  LIS3DH: keine Antwort beim Lesen erhalten");
    return 0;
}

static bool lis_read_block(uint8_t reg, uint8_t *buf, uint8_t len)
{
    Wire.beginTransmission(LIS3DH_ADDR);
    Wire.write(reg | 0x80);            // Auto-Increment
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
        DBG("  LIS3DH I2C Fehler beim Adressieren (Code %u)\r\n", err);
        return false;
    }
    uint8_t got = Wire.requestFrom((uint8_t)LIS3DH_ADDR, len);
    if (got != len) {
        DBG("  LIS3DH: nur %u von %u Bytes erhalten\r\n", got, len);
        while (Wire.available()) Wire.read();   // Rest verwerfen, Bus sauber halten
        return false;
    }
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static bool lis3dh_init(void)
{
    uint8_t id = lis_read(LIS3DH_WHO_AM_I);
    DBG("LIS3DH WHO_AM_I: 0x%02X (erwartet 0x33)\r\n", id);
    if (id != 0x33) return false;

    lis_write(LIS3DH_CTRL_REG1, 0x00);   // erst mal Power-Down

    bool pu_ok = lis_write(LIS3DH_CTRL_REG0, 0x90);
    DBG("LIS3DH SA0-Pull-up abgeschaltet: %s (CTRL_REG0 = 0x%02X)\r\n",
        pu_ok ? "OK" : "FEHLER", lis_read(LIS3DH_CTRL_REG0));
    lis_write(LIS3DH_CTRL_REG4, 0x80);   // BDU=1, +/-2g
    lis_write(LIS3DH_CTRL_REG6, 0x02);   // INT1 active LOW (interner Pull-up an PA8)
    return true;
}

/* Messmodus: Normal Mode, 50 Hz */
static void lis3dh_start_measure(void)
{
    lis_write(LIS3DH_CTRL_REG1, 0x47);   // ODR=50Hz, LPen=0, XYZ ein
    delay(50);                           // Einschwingen
}

static void lis3dh_read_mg(int16_t *xyz)
{
    uint8_t b[6];
    if (!lis_read_block(LIS3DH_OUT_X_L, b, 6)) {
        xyz[0] = xyz[1] = xyz[2] = 0;
        return;
    }
    for (uint8_t i = 0; i < 3; i++) {
        int16_t raw = (int16_t)((b[i * 2 + 1] << 8) | b[i * 2]);
        xyz[i] = (int16_t)((raw >> 6) * 4);   // Normal Mode, 10 Bit, 4 mg/LSB
    }
}

static void wake_isr(void);   // weiter unten definiert

/* Wake-on-Motion: 10 Hz Low-Power, Hochpass, INT1 -> Wake_Pin */
static void lis3dh_enable_wake(void)
{
    /* v7.9b: Reihenfolge korrigiert, damit der Sensor nicht faelschlich weckt.
     * 1) Interrupt-Ausgang AUS und Pin-Interrupt abhaengen, waehrend konfiguriert wird
     * 2) Sensor starten und einschwingen lassen
     * 3) ERST DANN den Hochpass auf die aktuelle Lage setzen (REFERENCE lesen) -
     *    vorher sah der Filter beim Start die Erdbeschleunigung (1 g) als
     *    "Sprung" und loeste sofort einen falschen Bewegungs-Alarm aus
     * 4) anstehende Interrupts loeschen, dann Ausgang wieder einschalten */
    detachInterrupt(PIN_WAKE);
    lis_write(LIS3DH_CTRL_REG3, 0x00);   // INT1-Pin vorerst AUS
    lis_write(LIS3DH_INT1_CFG, 0x00);    // Interrupt-Logik vorerst AUS
    lis_write(LIS3DH_CTRL_REG2, 0x01);   // HPM=00 (Reset per REFERENCE), HP nur fuer INT1
    lis_write(LIS3DH_CTRL_REG4, 0x80);   // BDU, +/-2g
    lis_write(LIS3DH_CTRL_REG5, 0x00);   // nicht gelatcht
    lis_write(LIS3DH_CTRL_REG6, 0x02);   // INT1 active LOW
    lis_write(LIS3DH_INT1_THS, (uint8_t)(g_cfg.thresh_16mg & 0x7F));  // 16 mg/LSB
    lis_write(LIS3DH_INT1_DUR, ACCEL_DURATION_SAMPLES);  // Mindestdauer (x 100 ms bei 10 Hz)
    lis_write(LIS3DH_CTRL_REG1, 0x2F);   // ODR=10Hz, LPen=1, XYZ an -> Sensor laeuft

    delay(350);                          // einschwingen: mehrere Messungen abwarten
    (void)lis_read(LIS3DH_REFERENCE);    // Hochpass JETZT auf aktuelle Lage setzen
    delay(250);                          // Filter-Ausgang auf ~0 kommen lassen
    (void)lis_read(LIS3DH_INT1_SRC);     // evtl. anstehenden Interrupt loeschen

    lis_write(LIS3DH_INT1_CFG, 0x2A);    // OR: X_H | Y_H | Z_H
    lis_write(LIS3DH_CTRL_REG3, 0x40);   // erst jetzt: I1_IA1 -> INT1-Pin
    (void)lis_read(LIS3DH_INT1_SRC);

    g_motion = false;                    // Flag von der Einrichtung verwerfen
    attachInterrupt(PIN_WAKE, wake_isr, FALLING);
}

static void lis3dh_powerdown(void)
{
    lis_write(LIS3DH_CTRL_REG0, 0x90);   // SA0-Pull-up sicher AUS (siehe lis3dh_init)
    lis_write(LIS3DH_CTRL_REG1, 0x00);   // ca. 0.5 uA
    lis_write(LIS3DH_CTRL_REG3, 0x00);   // INT1-Pin abschalten
}

static void wake_isr(void)
{
    g_motion = true;
}

/*==============================================================================
 *  9) GPS - u-blox UBX-Protokoll (UBX-NAV-PVT)
 *  Liefert Lat/Lon direkt als int32 * 1e-7 Grad, exakt im Format, das wir
 *  fuer den Payload brauchen - kein NMEA-Text-Parsing mit Grad/Minuten-
 *  Umrechnung mehr noetig, dadurch weniger Fehlerquellen.
 *============================================================================*/
#define UBX_SYNC1        0xB5
#define UBX_SYNC2        0x62
#define UBX_CLASS_NAV    0x01
#define UBX_ID_NAV_PVT   0x07
#define UBX_CLASS_ACK    0x05
#define UBX_ID_ACK_ACK   0x01
#define UBX_NAV_PVT_LEN  92
#define GPS_SOFTSTART        1        // 0 = hart einschalten wie bisher
#define GPIOA_BSRR   (*(volatile uint32_t *)0x48000018UL)   // STM32WL GPIOA
#define GPS_EN_SET   (1UL << 0)                             // PA0 HIGH -> Q3 aus
#define GPS_EN_RESET (1UL << 16)                            // PA0 LOW  -> Q3 an

static void gps_softstart(void)
{
    for (uint8_t stage = 0; stage < 6; stage++) {          // Pulse werden laenger
        for (uint16_t i = 0; i < 120; i++) {
            __asm volatile ("cpsid i" ::: "memory");       // Puls nicht durch IRQ verlaengern
            GPIOA_BSRR = GPS_EN_RESET;                     // Q3 kurz AN
            for (uint8_t n = 0; n < stage; n++) __asm volatile ("nop");
            GPIOA_BSRR = GPS_EN_SET;                       // Q3 wieder AUS
            __asm volatile ("cpsie i" ::: "memory");
            for (volatile uint16_t d = 0; d < 400; d++) { }   // ~20-40 us Pause: LDO laedt nach
        }
    }
}

static void gps_power(bool on)
{
    /* GPS_EN LOW = P-MOSFET Q3 leitet = GPS bekommt Strom */
#if GPS_SOFTSTART
    if (on) gps_softstart();
#endif
    digitalWrite(PIN_GPS_EN, on ? LOW : HIGH);
}

#define EXTI_RTSR1   (*(volatile uint32_t *)0x58000800UL)
#define EXTI_FTSR1   (*(volatile uint32_t *)0x58000804UL)
#define EXTI_C1IMR1  (*(volatile uint32_t *)0x58000880UL)
#define SYSCFG_EXTICR2 (*(volatile uint32_t *)0x4001000CUL)
static void diag_exti7(void)
{
    DBG("  Diagnose EXTI7: IMR=%lu RT=%lu FT=%lu Port=%lu (1=PB)  PB6/PB7-MODER=%lX\r\n",
        (unsigned long)((EXTI_C1IMR1 >> 7) & 1UL), (unsigned long)((EXTI_RTSR1 >> 7) & 1UL),
        (unsigned long)((EXTI_FTSR1 >> 7) & 1UL), (unsigned long)((SYSCFG_EXTICR2 >> 12) & 7UL),
        (unsigned long)(((*(volatile uint32_t *)0x48000400UL) >> 12) & 0xFUL));
    Serial.flush();
}

static int32_t ubx_rd_i4(const uint8_t *p, uint16_t o)
{
    uint32_t v = (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8)
               | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
    return (int32_t)v;
}

/* Wertet ein empfangenes UBX-Frame aus (Checksumme wurde bereits geprueft) */
static void handle_ubx_frame(uint8_t cls, uint8_t id, uint8_t *payload, uint16_t len)
{
    /* ACK/NAK auf unseren CFG-MSG-Befehl */
    if (cls == UBX_CLASS_ACK && len >= 2) {
        s_ack_class    = payload[0];
        s_ack_id       = payload[1];
        s_ack_received = (id == UBX_ID_ACK_ACK);
        return;
    }

    if (cls == UBX_CLASS_NAV && id == UBX_ID_NAV_PVT && len == UBX_NAV_PVT_LEN) {
        uint8_t  fixType   = payload[20];
        uint8_t  flags     = payload[21];
        bool     fixOK     = (flags & 0x01) != 0;      // gnssFixOK: Loesung gueltig
        bool     diffSoln  = (flags & 0x02) != 0;      // Korrekturdaten (SBAS) verwendet
        uint8_t  numSV     = payload[23];
        int32_t  lon_raw   = ubx_rd_i4(payload, 24);   // 1e-7 Grad
        int32_t  lat_raw   = ubx_rd_i4(payload, 28);   // 1e-7 Grad
        /* FIX HOEHE: Offset 32 ist die Hoehe ueber dem ELLIPSOID (WGS84).
         * In der Schweiz liegt diese ca. 48-50 m ueber der Meereshoehe
         * (Geoid-Undulation) -> genau dein Fehler von ~50 m.
         * Offset 36 = hMSL = Hoehe ueber Meer, wie auf jeder Karte. */
        int32_t  hmsl_mm   = ubx_rd_i4(payload, 36);
        uint32_t hacc_mm   = (uint32_t)ubx_rd_i4(payload, 40);
        uint32_t vacc_mm   = (uint32_t)ubx_rd_i4(payload, 44);
        int32_t  gspeed    = ubx_rd_i4(payload, 60);   // mm/s

        g_fix.sats    = numSV;
        g_fix.fixType = fixType;
        g_fix.hacc_mm = hacc_mm;
        g_fix.hour    = payload[8];
        g_fix.minute  = payload[9];
        g_fix.second  = payload[10];

        DBG("  NAV-PVT: Fix %u%s%s  Sat %u  hAcc %lu mm  vAcc %lu mm  v %ld cm/s\r\n",
            fixType, fixOK ? " OK" : " --", diffSoln ? " SBAS" : "", numSV,
            (unsigned long)hacc_mm, (unsigned long)vacc_mm, (long)(gspeed / 10));

        if (fixType >= 2 && fixOK) {
            g_fix.lat_1e7 = lat_raw;
            g_fix.lon_1e7 = lon_raw;
            g_fix.alt_m   = (int16_t)((hmsl_mm + (hmsl_mm >= 0 ? 500 : -500)) / 1000);
            g_fix.sbas    = diffSoln;
        }

        bool goodEnough = fixOK
                        && (fixType >= GPS_MIN_FIX_TYPE)
                        && (numSV >= GPS_MIN_SATS)
                        && (hacc_mm <= (uint32_t)GPS_MAX_HACC_MM);
        if (goodEnough) {
            if (!g_fix.valid) g_fix.first_valid_ms = millis();
            g_fix.valid  = true;
            g_fix.moving = (gspeed > (int32_t)GPS_MOVING_SPEED_CMS * 10);

            /* Alle guten Epochen aufsummieren -> Mittelwert reduziert das
             * typische "Herumwandern" der Position um 1-3 m. */
            if (avg_n == 0) { avg_ref_lat = lat_raw; avg_ref_lon = lon_raw; }
            avg_sum_lat  += (int64_t)(lat_raw - avg_ref_lat);
            avg_sum_lon  += (int64_t)(lon_raw - avg_ref_lon);
            avg_sum_hmsl += (int64_t)hmsl_mm;
            avg_n++;
        }
    }
}
static void gps_parse_bytes(void)
{
    while (Serial1.available()) {
        uint8_t b = (uint8_t)Serial1.read();

        switch (u_state) {
          case U_SYNC1:
            if (b == UBX_SYNC1) u_state = U_SYNC2;
            break;

          case U_SYNC2:
            u_state = (b == UBX_SYNC2) ? U_CLASS : U_SYNC1;
            break;

          case U_CLASS:
            u_class = b; u_rolla = b; u_rollb = u_rolla;
            u_state = U_ID;
            break;

          case U_ID:
            u_id = b; u_rolla += b; u_rollb += u_rolla;
            u_state = U_LEN1;
            break;

          case U_LEN1:
            u_len = b; u_rolla += b; u_rollb += u_rolla;
            u_state = U_LEN2;
            break;

          case U_LEN2:
            u_len |= ((uint16_t)b << 8);
            u_rolla += b; u_rollb += u_rolla;
            u_pidx = 0;
            if (u_len == 0)                     u_state = U_CKA;
            else if (u_len > sizeof(u_payload)) u_state = U_SYNC1; // zu gross -> verwerfen
            else                                 u_state = U_PAYLOAD;
            break;

          case U_PAYLOAD:
            u_payload[u_pidx++] = b;
            u_rolla += b; u_rollb += u_rolla;
            if (u_pidx >= u_len) u_state = U_CKA;
            break;

          case U_CKA:
            u_cka = b; u_state = U_CKB;
            break;

          case U_CKB:
            u_ckb = b;
            if (u_cka == u_rolla && u_ckb == u_rollb) {
                handle_ubx_frame(u_class, u_id, u_payload, u_len);
            }
            u_state = U_SYNC1;
            break;
        }
    }
}

static void send_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    uint8_t cka = 0, ckb = 0;
    uint8_t hdr[4] = { cls, id, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };

    Serial1.write(UBX_SYNC1);
    Serial1.write(UBX_SYNC2);
    for (uint8_t i = 0; i < 4; i++) {
        Serial1.write(hdr[i]);
        cka += hdr[i]; ckb += cka;
    }
    for (uint16_t i = 0; i < len; i++) {
        Serial1.write(payload[i]);
        cka += payload[i]; ckb += cka;
    }
    Serial1.write(cka);
    Serial1.write(ckb);
}

/* Sendet einen Befehl und wartet auf ACK (bis zu 5 Versuche) */
static bool send_ubx_wait_ack(uint8_t cls, uint8_t id, const uint8_t *payload,
                              uint16_t len, uint8_t ack_cls, uint8_t ack_id)
{
    for (uint8_t attempt = 1; attempt <= 5; attempt++) {
        s_ack_received = false;
        s_ack_class = s_ack_id = 0;
        send_ubx(cls, id, payload, len);

        uint32_t start = millis();
        while ((millis() - start) < 300) {
            gps_parse_bytes();
            if (s_ack_class == ack_cls && s_ack_id == ack_id) {
                if (s_ack_received) return true;
                DBGLN("  GPS hat den Befehl abgelehnt (NAK)");
                return false;
            }
        }
        delay(150);
    }
    return false;
}

/* Haengt einen Konfigurationswert (Key + Wert) an einen UBX-CFG-VALSET an */
static uint16_t valset_add(uint8_t *buf, uint16_t i, uint32_t key, uint32_t val, uint8_t size)
{
    buf[i++] = key; buf[i++] = key >> 8; buf[i++] = key >> 16; buf[i++] = key >> 24;
    for (uint8_t b = 0; b < size; b++) buf[i++] = (uint8_t)(val >> (8 * b));
    return i;
}


static bool gps_configure(void)
{
    uint8_t p[4 + 3 * 5];
    uint16_t i = 0;
    p[i++] = 0x00;          // Version
    p[i++] = 0x01;          // Layer: RAM
    p[i++] = 0x00; p[i++] = 0x00;
    i = valset_add(p, i, 0x20910007UL, 1, 1);               // CFG-MSGOUT-UBX_NAV_PVT_UART1 = 1
    i = valset_add(p, i, 0x20110021UL, GPS_DYN_MODEL, 1);   // CFG-NAVSPG-DYNMODEL
    i = valset_add(p, i, 0x10310020UL, GPS_USE_SBAS, 1);    // CFG-SIGNAL-SBAS_ENA

    if (send_ubx_wait_ack(0x06, 0x8A, p, i, 0x06, 0x8A)) {
        DBGLN("  GPS konfiguriert (NAV-PVT, Dyn-Modell, SBAS)");
        return true;
    }

    /* Fallback: nur NAV-PVT einschalten wie in der alten Version */
    uint8_t msg[3] = { UBX_CLASS_NAV, UBX_ID_NAV_PVT, 0x01 };
    /* Hinweis: Das ACK enthaelt die Klasse/ID des BESTAETIGTEN Befehls (CFG-MSG
     * = 0x06/0x01), nicht NAV-PVT - die alte Version hat darum nie ein ACK erkannt. */
    if (send_ubx_wait_ack(0x06, 0x01, msg, sizeof(msg), 0x06, 0x01)) {
        DBGLN("  NAV-PVT aktiviert (Fallback CFG-MSG, ohne SBAS/Dyn-Modell)");
        return true;
    }
    DBGLN("  WARNUNG: kein ACK vom GPS erhalten - NAV-PVT evtl. nicht aktiv");
    return false;
}

#define GPS_UART_PINS_SAFE   0          // alte pinMode-Variante: AUS lassen!


#define GPS_UART_PARK        0          // AUS: verursachte Haenger bei Serial1.begin()
                                        // im 2. Zyklus (v7.2/v7.3). Nicht einschalten!

#define GPIOB_MODER   (*(volatile uint32_t *)0x48000400UL)   // STM32WL GPIOB
#define GPIOB_PUPDR   (*(volatile uint32_t *)0x4800040CUL)
#define UART1_PIN_MASK  ((3UL << (6 * 2)) | (3UL << (7 * 2)))  // PB6 + PB7

static uint32_t s_moder_saved = 0, s_pupdr_saved = 0;
static bool     s_pins_parked = false;

static void gps_uart_pins_park(void)
{
#if GPS_UART_PARK
    if (s_pins_parked) return;
    s_moder_saved = GPIOB_MODER & UART1_PIN_MASK;
    s_pupdr_saved = GPIOB_PUPDR & UART1_PIN_MASK;
    GPIOB_PUPDR &= ~UART1_PIN_MASK;          // keine Pull-Widerstaende
    GPIOB_MODER |=  UART1_PIN_MASK;          // Analog-Modus = hochohmig
    s_pins_parked = true;
    DBG("  UART-Pins geparkt (vorher MODER=%lX PUPDR=%lX, jetzt MODER=%lX, 3C000=analog)\r\n",
        (unsigned long)(s_moder_saved >> 0), (unsigned long)(s_pupdr_saved >> 0),
        (unsigned long)(GPIOB_MODER & UART1_PIN_MASK));
#endif
}

static void gps_uart_pins_unpark(void)
{
#if GPS_UART_PARK
    if (!s_pins_parked) return;
    GPIOB_MODER = (GPIOB_MODER & ~UART1_PIN_MASK) | s_moder_saved;
    GPIOB_PUPDR = (GPIOB_PUPDR & ~UART1_PIN_MASK) | s_pupdr_saved;
    s_pins_parked = false;
#endif
}

static void gps_uart_pins_safe(void)
{
#if GPS_UART_PINS_SAFE
    pinMode(PB6, INPUT_PULLDOWN);
    pinMode(PB7, INPUT_PULLDOWN);
#endif
}

#define GPS_UART_KEEP_OPEN   1   // 1 = neue Methode, 0 = alt (end/begin, ~+200 uA)

#define USART1_CR1   (*(volatile uint32_t *)0x40013800UL)   // STM32WL USART1
#define USART1_ISR   (*(volatile uint32_t *)0x4001381CUL)
#define USART1_ICR   (*(volatile uint32_t *)0x40013820UL)
#define RCC_APB2ENR  (*(volatile uint32_t *)0x58000060UL)   // STM32WL RCC
#define RCC_USART1EN (1UL << 14)
#define USART_CR1_UE (1UL << 0)
#define USART_ISR_TC (1UL << 6)

static bool     g_uart1_open = false;
static bool     g_uart1_suspended = false;
static uint32_t s_u_moder = 0, s_u_pupdr = 0;

static void gps_uart_start(void)
{
#if GPS_UART_KEEP_OPEN
    if (g_uart1_suspended) {
        GPIOB_MODER = (GPIOB_MODER & ~UART1_PIN_MASK) | s_u_moder;    // Pins zurueck
        GPIOB_PUPDR = (GPIOB_PUPDR & ~UART1_PIN_MASK) | s_u_pupdr;
        g_uart1_suspended = false;
    }
    Serial1.begin(GPS_BAUD);
    g_uart1_open = true;
    uint32_t t = millis(); uint32_t n = 0;
    while (millis() - t < 1500 && n < 20) { while (Serial1.available()) { Serial1.read(); n++; } }
    if (n == 0) {
        DBGLN("  UART1 stumm -> zweiter Startversuch");
        Serial1.end();
        Serial1.begin(GPS_BAUD);
    } else {
        DBGLN("  UART1 laeuft");
    }
    while (Serial1.available()) Serial1.read();   // Muell vom GPS-Start verwerfen
#else
    Serial1.begin(GPS_BAUD);
#endif
}

static void gps_uart_stop(void)
{
#if GPS_UART_KEEP_OPEN
    if (!g_uart1_open || g_uart1_suspended) return;
    Serial1.flush();                                            // Senden abwarten
    Serial1.end();                                              // UART sauber schliessen
    /* Danach bleibt TX (PB6) auf HIGH und wuerde das stromlose GPS speisen
     * (+~190 uA) -> Pins hochohmig schalten, Zustand fuer begin() merken. */
    s_u_moder = GPIOB_MODER & UART1_PIN_MASK;
    s_u_pupdr = GPIOB_PUPDR & UART1_PIN_MASK;
    GPIOB_PUPDR &= ~UART1_PIN_MASK;
    GPIOB_MODER |=  UART1_PIN_MASK;
    g_uart1_suspended = true;
#else
    Serial1.end();
#endif
}

static void gps_off(void)
{
    if (!g_gps_on) return;
    gps_uart_stop();                    // UART anhalten, TX/RX hochohmig
    gps_power(false);                   // GPS_EN = HIGH -> P-MOSFET sperrt
    g_gps_on = false;
    DBGLN("GPS aus (GPS_EN = HIGH)");
}


static bool gps_get_fix(bool keep_on, uint16_t settle_s, uint16_t timeout_s)
{
    memset(&g_fix, 0, sizeof(g_fix));
    avg_n = 0; avg_sum_lat = avg_sum_lon = avg_sum_hmsl = 0;

    if (!g_gps_on) {
        u_state = U_SYNC1;   // Parser fuer diesen Zyklus zuruecksetzen
        DBGLN("GPS an (Sanftanlauf), suche Fix ...");
        gps_power(true);
        delay(100);                  // kurz warten, bevor die UART aktiv wird
        gps_uart_start();            // UART (wieder) aktivieren
        delay(300);
        bool cfg_ok = gps_configure();
        DBG("  Konfiguration %s, warte auf Fix ...\r\n",
            cfg_ok ? "OK" : "OHNE ANTWORT (UART stumm?)");
        g_gps_on = true;
    } else {
        DBGLN("GPS laeuft bereits (Dauer-Tracking), warte auf neue Position ...");
        while (Serial1.available()) Serial1.read();   // alte Daten verwerfen
        u_state = U_SYNC1;
    }

    uint32_t start    = millis();
    uint32_t last_dbg = start;

    while ((millis() - start) < (uint32_t)timeout_s * 1000UL) {
        gps_parse_bytes();
        if (g_fix.valid) {
            if (g_fix.moving) break;
            if ((millis() - g_fix.first_valid_ms) >= (uint32_t)settle_s * 1000UL) break;
        }

        if (millis() - last_dbg > 5000) {
            last_dbg = millis();
            DBG("  ... %lus, Satelliten: %u%s\r\n",
                (unsigned long)((millis() - start) / 1000), g_fix.sats,
                g_fix.valid ? " (mittle ...)" : "");
        }
    }

    if (!keep_on) gps_off();

    if (g_fix.valid && !g_fix.moving && avg_n >= 2) {
        g_fix.lat_1e7  = avg_ref_lat + (int32_t)(avg_sum_lat / avg_n);
        g_fix.lon_1e7  = avg_ref_lon + (int32_t)(avg_sum_lon / avg_n);
        int32_t hmsl   = (int32_t)(avg_sum_hmsl / avg_n);
        g_fix.alt_m    = (int16_t)((hmsl + (hmsl >= 0 ? 500 : -500)) / 1000);
        g_fix.averaged = true;
        DBG("Mittelwert aus %u Messungen\r\n", avg_n);
    }

    if (g_fix.valid) {
        char slat[16], slon[16];
        format_coord(g_fix.lat_1e7, slat, sizeof(slat));
        format_coord(g_fix.lon_1e7, slon, sizeof(slon));
        DBG("FIX nach %lus | Sats %u | hAcc %lum | Hoehe %dm ue.M.%s\r\n",
            (unsigned long)((millis() - start) / 1000), g_fix.sats,
            (unsigned long)(g_fix.hacc_mm / 1000), g_fix.alt_m,
            g_fix.sbas ? " | SBAS" : "");
        DBG("Google: https://www.google.com/maps?q=%s,%s\r\n", slat, slon);
    } else if (g_fix.fixType >= 2) {
        DBG("TIMEOUT, aber Position vorhanden (nicht genau genug): "
            "Sats %u  hAcc %lum  (Grenze: FixType>=%u, Sats>=%u, hAcc<=%um)\r\n",
            g_fix.sats, (unsigned long)(g_fix.hacc_mm / 1000),
            (unsigned)GPS_MIN_FIX_TYPE, (unsigned)GPS_MIN_SATS, (unsigned)(GPS_MAX_HACC_MM / 1000));
    } else {
        DBGLN("KEIN FIX (Timeout, gar keine Position berechnet)");
    }
    return g_fix.valid;
}

/*==============================================================================
 * 10) LoRaWAN
 *============================================================================*/
static void join_cb(int32_t status)
{
    g_joined = (status == 0);
    DBG("Join-Callback: %s\r\n", g_joined ? "beigetreten" : "fehlgeschlagen");
}

static void send_cb(int32_t status)
{
    g_tx_done = true;
    DBG("TX-Callback: status %ld\r\n", (long)status);
}

/*------------------------------------------------------------------------------
 * Konfiguration: pruefen, laden, speichern
 *----------------------------------------------------------------------------*/
static const char *mode_name(uint8_t m)
{
    return m == MODE_CONTINUOUS ? "Dauer-Tracking" : m == MODE_THEFT ? "Diebstahlschutz" : "Normal";
}

static void cfg_sanitize(tracker_cfg_t *c)
{
    if (c->mode > MODE_THEFT) c->mode = MODE_NORMAL;
    uint16_t min_iv = (c->mode == MODE_CONTINUOUS) ? MIN_INTERVAL_CONT_S : MIN_INTERVAL_NORMAL_S;
    if (c->interval_s < min_iv) c->interval_s = min_iv;
    if (c->heartbeat_h == 0)    c->heartbeat_h = 1;      // nie ganz ohne Lebenszeichen!
    if (c->thresh_16mg == 0)    c->thresh_16mg = 1;
    if (c->thresh_16mg > 0x7F)  c->thresh_16mg = 0x7F;
    c->magic = CFG_MAGIC;
}

static void cfg_defaults(tracker_cfg_t *c)
{
    c->mode        = DEFAULT_MODE;
    c->interval_s  = SEND_INTERVAL_S;
    c->heartbeat_h = DEFAULT_HEARTBEAT_H;
    c->thresh_16mg = ACCEL_THRESHOLD_MG / 16;
    cfg_sanitize(c);
}

static void cfg_load(void)
{
    tracker_cfg_t c;
    if (api.system.flash.get(0, (uint8_t *)&c, sizeof(c)) && c.magic == CFG_MAGIC) {
        cfg_sanitize(&c);
        g_cfg = c;
        DBGLN("Konfiguration aus Flash geladen.");
    } else {
        cfg_defaults(&g_cfg);
        DBGLN("Keine gespeicherte Konfiguration - Standardwerte.");
    }
    DBG("Modus: %s | Intervall %u s | Heartbeat %u h | Schwelle %u mg\r\n",
        mode_name(g_cfg.mode), g_cfg.interval_s, g_cfg.heartbeat_h, g_cfg.thresh_16mg * 16);
}

static void cfg_save(void)
{
    bool ok = api.system.flash.set(0, (uint8_t *)&g_cfg, sizeof(g_cfg));
    report("Konfig im Flash gesp.", ok);
}

/* Downlink empfangen - hier nur auswerten und merken, angewendet wird im loop() */
static void recv_cb(SERVICE_LORA_RECEIVE_T *data)
{
    DBG("Downlink auf Port %d, %d Byte:", data->Port, data->BufferSize);
    for (int i = 0; i < data->BufferSize; i++) DBG(" %02X", data->Buffer[i]);
    DBGLN("");

    if (data->Port == CONFIG_FPORT && data->BufferSize >= 6 && data->Buffer[0] == 0x01) {
        tracker_cfg_t c;
        c.mode        = data->Buffer[1];
        c.interval_s  = ((uint16_t)data->Buffer[2] << 8) | data->Buffer[3];
        c.heartbeat_h = data->Buffer[4];
        c.thresh_16mg = data->Buffer[5];
        cfg_sanitize(&c);
        g_cfg_new     = c;
        g_cfg_pending = true;
    }
}

/* Neue Konfiguration uebernehmen (im loop aufgerufen) */
static void cfg_apply_pending(void)
{
    if (!g_cfg_pending) return;
    g_cfg_pending = false;
    bool changed = memcmp(&g_cfg, &g_cfg_new, sizeof(g_cfg)) != 0;
    g_cfg = g_cfg_new;
    DBG("NEUE KONFIG: %s | Intervall %u s | Heartbeat %u h | Schwelle %u mg\r\n",
        mode_name(g_cfg.mode), g_cfg.interval_s, g_cfg.heartbeat_h, g_cfg.thresh_16mg * 16);
    if (changed) cfg_save();
    if (g_cfg.mode != MODE_CONTINUOUS) gps_off();
    g_quick_next = true;    // bald einen Uplink schicken, der den neuen Modus bestaetigt
}

/* Feste Frequenz erzwingen – kein Frequency Hopping */
static void lorawan_fix_channel(void)
{
#if USE_FIXED_CHANNEL
    uint16_t mask = FIXED_CHANNEL_MASK;
    report("Kanalmaske", api.lorawan.mask.set(&mask));
  #if USE_CHS_API
    report("Single-Channel (CHS)", api.lorawan.chs.set(FIXED_CHANNEL_FREQ));
  #endif
#endif
}

static bool lorawan_setup(void)
{
    /* In den LoRaWAN-Modus wechseln (Beispielcode lief im P2P-Modus!) */
    if (api.lorawan.nwm.get() != 1) {
        DBGLN("Wechsle in den LoRaWAN-Modus, Neustart ...");
        api.lorawan.nwm.set();   // 1 = LoRaWAN-Modus (0 = P2P)
        api.system.reboot();
    }

    DBGLN("LoRaWAN-Konfiguration:");
    report("Band", api.lorawan.band.set(LORAWAN_BAND));
    report("Class A", api.lorawan.deviceClass.set(0));
    report("ADR", api.lorawan.adr.set(LORAWAN_ADR));
    report("Datarate (Startwert)", api.lorawan.dr.set(LORAWAN_DR));
    report("TX-Power", api.lorawan.txp.set(LORAWAN_TXP));
    report("Unconfirmed", api.lorawan.cfm.set(LORAWAN_CONFIRMED));
    lorawan_fix_channel();

#if LORAWAN_USE_OTAA
    report("Join-Mode OTAA", api.lorawan.njm.set(1));
    report("DevEUI", api.lorawan.deui.set(node_device_eui, 8));
    report("AppEUI", api.lorawan.appeui.set(node_app_eui, 8));
    report("AppKey", api.lorawan.appkey.set(node_app_key, 16));
#else
    report("Join-Mode ABP", api.lorawan.njm.set(0));
    report("DevAddr", api.lorawan.daddr.set(node_dev_addr, 4));
    report("NwkSKey", api.lorawan.nwkskey.set(node_nwk_skey, 16));
    report("AppSKey", api.lorawan.appskey.set(node_app_skey, 16));
#endif

    api.lorawan.registerJoinCallback(join_cb);
    api.lorawan.registerSendCallback(send_cb);
    api.lorawan.registerRecvCallback(recv_cb);

    DBGLN("Join laeuft ...");
    api.lorawan.join();

    uint32_t start = millis();
    while (!api.lorawan.njs.get()) {
        if ((millis() - start) > (uint32_t)JOIN_TIMEOUT_S * 1000UL) {
            DBGLN("Join-Timeout! Tracker sendet trotzdem weiter (Retry).");
            return false;
        }
        delay(500);
    }
    g_joined = true;
    DBGLN("Join erfolgreich.");
    lorawan_fix_channel();          // nach dem Join erneut fixieren (CFList!)
    return true;
}

static uint8_t build_payload(uint8_t *buf)
{
    uint8_t i = 0;
    uint8_t flags = 0;

    if (g_fix.fixType >= 2)  flags |= 0x01;   // irgendeine Position vorhanden
    if (g_fix.valid)         flags |= 0x10;   // Position ist genau genug
    if (g_fix.sbas)          flags |= 0x20;   // NEU: mit EGNOS-Korrektur berechnet
    if (g_fix.averaged)      flags |= 0x40;   // NEU: Position ist ein Mittelwert
    if (g_motion_uplink)     flags |= 0x02;
#if USE_ACCEL
    if (g_accel_ok)         flags |= 0x04;
  #if ACCEL_SEND_XYZ
    if (g_accel_ok)         flags |= 0x08;
  #endif
#endif

    buf[i++] = flags;
    buf[i++] = g_fix.sats;

    uint32_t hacc_m_full = (g_fix.hacc_mm + 999UL) / 1000UL;   // aufrunden: 1.4 m -> 2 m
    uint16_t hacc_m = (hacc_m_full > 65535UL) ? 65535 : (uint16_t)hacc_m_full;
    buf[i++] = (uint8_t)(hacc_m >> 8);
    buf[i++] = (uint8_t)(hacc_m);

    buf[i++] = (uint8_t)(g_fix.lat_1e7 >> 24);
    buf[i++] = (uint8_t)(g_fix.lat_1e7 >> 16);
    buf[i++] = (uint8_t)(g_fix.lat_1e7 >> 8);
    buf[i++] = (uint8_t)(g_fix.lat_1e7);

    buf[i++] = (uint8_t)(g_fix.lon_1e7 >> 24);
    buf[i++] = (uint8_t)(g_fix.lon_1e7 >> 16);
    buf[i++] = (uint8_t)(g_fix.lon_1e7 >> 8);
    buf[i++] = (uint8_t)(g_fix.lon_1e7);

    buf[i++] = (uint8_t)(g_fix.alt_m >> 8);
    buf[i++] = (uint8_t)(g_fix.alt_m);

    buf[i++] = (uint8_t)(g_batt_mv >> 8);
    buf[i++] = (uint8_t)(g_batt_mv);

#if USE_ACCEL && ACCEL_SEND_XYZ
    if (g_accel_ok) {
        for (uint8_t a = 0; a < 3; a++) {
            buf[i++] = (uint8_t)(g_acc_mg[a] >> 8);
            buf[i++] = (uint8_t)(g_acc_mg[a]);
        }
    }
#endif

    /* NEU: aktive Konfiguration, damit die Webseite den Modus anzeigen kann */
    buf[i++] = g_cfg.mode;
    buf[i++] = (uint8_t)(g_cfg.interval_s >> 8);
    buf[i++] = (uint8_t)(g_cfg.interval_s);
    buf[i++] = g_cfg.heartbeat_h;
    buf[i++] = g_cfg.thresh_16mg;
    return i;
}

static void send_uplink(void)
{
    uint8_t payload[32];
    uint8_t len = build_payload(payload);

    DBG("Uplink #%lu (%u Byte):", (unsigned long)++g_uplink_count, len);
    for (uint8_t i = 0; i < len; i++) DBG(" %02X", payload[i]);
    DBGLN("");

    if (!api.lorawan.njs.get()) {
        DBGLN("Nicht gejoint – neuer Join-Versuch.");
        api.lorawan.join();
        delay(10000);
        if (!api.lorawan.njs.get()) return;
        lorawan_fix_channel();
    }

    lorawan_fix_channel();          // vor jedem Uplink Kanal fixieren

#if !LORAWAN_ADR
    /* Fester SF: vor jedem Uplink sicherstellen, dass er auch wirklich gilt */
    api.lorawan.dr.set(LORAWAN_DR);
    api.lorawan.txp.set(LORAWAN_TXP);
#endif
    {
        unsigned dr  = (unsigned)api.lorawan.dr.get();
        unsigned txp = (unsigned)api.lorawan.txp.get();
        if (dr <= 5) DBG("Datarate DR%u = SF%u, TX-Power-Index %u (0 = max.)\r\n", dr, 12 - dr, txp);
        else         DBG("Datarate DR%u, TX-Power-Index %u (0 = max.)\r\n", dr, txp);
    }

    g_tx_done = false;

    bool ok = api.lorawan.send(len, payload, LORAWAN_FPORT, LORAWAN_CONFIRMED);
    DBG("Sendeauftrag: %s\r\n", ok ? "OK" : "FAIL");

    uint32_t start = millis();
    while (!g_tx_done && (millis() - start) < 15000UL) delay(50);

    start = millis();
    while (!g_cfg_pending && (millis() - start) < 3000UL) delay(50);
}

/*==============================================================================
 * 12) SETUP
 *============================================================================*/
void setup()
{
    Serial.begin(115200);
    delay(2000);

    DBGLN("");
    DBGLN("==============================");
    DBGLN(" RAK3172 GPS-Tracker  v7.9c");
    DBGLN("==============================");
    analogReadResolution(12);

    /* GPS sofort ausschalten (HIGH = P-MOSFET sperrt) */
    pinMode(PIN_GPS_EN, OUTPUT);
    gps_power(false);
    gps_uart_pins_safe();

    /* RUI3 Low-Power-Modus explizit einschalten. Ist er aus, bleibt der
     * Prozessor bei sleep.all() wach und braucht mehrere mA. */
#if USE_LPM
    report("Low-Power-Modus", api.system.lpm.set(1));
#else
    api.system.lpm.set(0);
    DBGLN("  Low-Power-Modus AUS (USE_LPM = 0)");
#endif
    DBG("  LPM aktuell: %u (1 = an)\r\n", (unsigned)api.system.lpm.get());

    pinMode(PIN_WAKE, INPUT_PULLUP);
    attachInterrupt(PIN_WAKE, wake_isr, FALLING);

    Wire.begin();
    delay(50);

#if USE_ACCEL
    g_accel_ok = lis3dh_init();
    if (!g_accel_ok) DBGLN("LIS3DH nicht gefunden – laeuft ohne Sensor weiter.");
#else
    lis3dh_powerdown();
    g_accel_ok = false;
    DBGLN("Beschleunigungssensor im Code deaktiviert (Power-Down).");
#endif

    cfg_load();

    lorawan_setup();

    g_motion = false;
    DBGLN("Setup fertig.\r\n");
}

/*==============================================================================
 * 13) LOOP
 *============================================================================*/
static void safe_sleep(uint32_t ms)
{
    pinMode(PIN_GPS_EN, OUTPUT);
    digitalWrite(PIN_GPS_EN, HIGH);
    delay(2);
    DBG("  vor Sleep:  GPS_EN = %s\r\n", digitalRead(PIN_GPS_EN) ? "HIGH" : "LOW (!)");
    Serial.flush();

    api.system.sleep.all(ms);

    DBG("  nach Sleep: GPS_EN = %s%s\r\n", digitalRead(PIN_GPS_EN) ? "HIGH" : "LOW",
        digitalRead(PIN_GPS_EN) ? "" : "  <-- RUI3 hat den Pin im Schlaf veraendert!");
    pinMode(PIN_GPS_EN, OUTPUT);           // fuer den Rest des Zyklus sicherstellen
    digitalWrite(PIN_GPS_EN, g_gps_on ? LOW : HIGH);
}
static void wait_awake(uint32_t ms)
{
    uint32_t start = millis();
    while ((millis() - start) < ms) {
        gps_parse_bytes();
        if (g_cfg_pending) return;
        delay(20);
    }
}

void loop()
{
    uint32_t cycle_start = millis();
    cfg_apply_pending();

    g_motion_uplink = g_motion;
    g_motion = false;
    if (g_motion_uplink) {
        g_last_motion_ms = millis();
        DBGLN(">>> Aufwachen durch Bewegung");
    } else {
        DBGLN(">>> Aufwachen durch Timer");
    }
    DBG("Modus: %s\r\n", mode_name(g_cfg.mode));

    /* 1) Batterie messen (vor dem GPS, damit die Messung nicht belastet ist) */
    g_batt_mv = read_battery_mv();

    /* 2) Beschleunigung messen */
#if USE_ACCEL && ACCEL_SEND_XYZ
    if (g_accel_ok) {
        lis3dh_start_measure();
        lis3dh_read_mg(g_acc_mg);
        DBG("Accel: X %d  Y %d  Z %d mg\r\n", g_acc_mg[0], g_acc_mg[1], g_acc_mg[2]);
    }
#endif

    /* 3) GPS-Fix holen - je nach Modus */
    bool have_fix;
    if (g_cfg.mode == MODE_CONTINUOUS) {
        have_fix = gps_get_fix(true, 0, g_gps_on ? GPS_CONT_TIMEOUT_S : GPS_FIX_TIMEOUT_S);
    } else if (g_motion_uplink) {
        /* Bewegung (Diebstahl!): schnell senden, nicht 20 s mitteln */
        have_fix = gps_get_fix(false, 0, GPS_FIX_TIMEOUT_S);
    } else {
        have_fix = gps_get_fix(false, GPS_SETTLE_S, GPS_FIX_TIMEOUT_S);
    }

    /* 4) Senden */
    if (have_fix || SEND_WITHOUT_FIX) {
        send_uplink();
    } else {
        DBGLN("Kein Fix – Uplink uebersprungen.");
    }

    /* Ein Downlink kommt direkt nach dem Uplink an -> sofort uebernehmen */
    cfg_apply_pending();

    /* 5) Schlafzeit je nach Modus */
    uint32_t sleep_ms;
    bool     motion_wake;
    if (g_cfg.mode == MODE_THEFT) {
        sleep_ms    = (uint32_t)g_cfg.heartbeat_h * 3600UL * 1000UL;   // nur Lebenszeichen
        motion_wake = true;
    } else if (g_cfg.mode == MODE_CONTINUOUS) {
        sleep_ms    = (uint32_t)g_cfg.interval_s * 1000UL;
        motion_wake = false;
    } else {
        sleep_ms    = (uint32_t)g_cfg.interval_s * 1000UL;
        motion_wake = ACCEL_WAKE_ON_MOTION;
    }
    if (g_quick_next) {                 // neuen Modus rasch per Uplink bestaetigen
        g_quick_next = false;
        sleep_ms = 10000UL;
        DBGLN("Neuer Modus aktiv - Bestaetigung in 10 s");
    }

    uint32_t elapsed = millis() - cycle_start;
    if (g_cfg.mode != MODE_THEFT)       // Intervall = Abstand zwischen Uplinks
        sleep_ms = (elapsed >= sleep_ms) ? 1000UL : (sleep_ms - elapsed);

    /* 6) Beschleunigungssensor vorbereiten */
#if USE_ACCEL
    if (g_accel_ok && motion_wake) lis3dh_enable_wake();
    else                           lis3dh_powerdown();
#else
    lis3dh_powerdown();
#endif
    if (g_cfg.mode == MODE_THEFT && !g_accel_ok)
        DBGLN("WARNUNG: Diebstahlschutz ohne LIS3DH - nur Lebenszeichen!");

    /* 7) Warten bzw. schlafen */
    if (g_cfg.mode == MODE_CONTINUOUS) {
        DBG("Dauer-Tracking: naechste Position in %lu s\r\n\r\n", (unsigned long)(sleep_ms / 1000));
        wait_awake(sleep_ms);
        return;
    }

    DBG("Deep Sleep fuer %lu s ... (GPS_EN=%s)\r\n\r\n", (unsigned long)(sleep_ms / 1000),
        digitalRead(PIN_GPS_EN) ? "HIGH, GPS aus" : "LOW, GPS AN!");
    Serial.flush();

    /* Sperrzeit: kurz nach einem Bewegungs-Uplink nicht sofort wieder wecken */
    uint32_t guard = (uint32_t)MOTION_MIN_INTERVAL_S * 1000UL;
    if (g_motion_uplink && guard > 0) {
        uint32_t g = (guard < sleep_ms) ? guard : sleep_ms;
        detachInterrupt(PIN_WAKE);
        safe_sleep(g);
        attachInterrupt(PIN_WAKE, wake_isr, FALLING);
        g_motion = false;
        sleep_ms -= g;
    }

    if (sleep_ms > 0) safe_sleep(sleep_ms);
}
