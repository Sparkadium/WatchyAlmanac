/*
 * WatchyDict — Compressed Dictionary + Photo Viewer for Watchy V2
 * 
 * 69,457 words with full definitions in 2MB via block compression.
 * Uses raw deflate + ESP32 ROM miniz for on-the-fly decompression.
 * 
 * Buttons (Watchy V2):
 *   VIEW: TR=search TL(tap)=random TL(hold)=menu BR=next BL=prev
 *   SEARCH: BR=right BL=down TR=add/GO TL=del/exit  (grid has a-z + '-')
 *   MATCHES: BR=down BL=up TR=select TL=back
 *   PICTURES: BR=next BL=prev TR=back TL=delete
 *   MENU: BR/BL=nav TR=select TL=back
 *   SET TIME&DATE: BR/BL=+/- (hold=fast) TR=next/confirm TL=back to clock
 *   TSUMEGO: see tsumego.h (cursor nav, tap=stone, hold=pass, menu)
 *   SKY: TL=exit BR=+30min BL=-30min TR=now (see sky.h)
 *   CLOCK: big time + date + sun rise/set; TR=set time TL=back
 *
 * Menu: Time, View Photos, Tsumego, Gazetteer<->Dictionary, Sky
 * Deep-sleep wake returns to whatever mode was active (RTC memory).
 *
 * Partition: Custom 640KB APP / 3.25MB SPIFFS (partitions.csv)
 *   app0   0x10000  0xA0000
 *   spiffs 0xB0000  0x340000   <- SPIFFS flash offset is 0xB0000 now!
 * SPIFFS contents: dictionary.cdb (fat) + gazetteer.cdb + problems.bin
 *                  + img_XX.bin photos
 * Image build: python spiffsgen.py 0x340000 data spiffs.bin
 *                     --page-size 256 --block-size 4096
 */

#include <GxEPD2_BW.h>
#include <Wire.h>
#include <SPIFFS.h>

// ESP32 ROM has tinfl (miniz) decompressor built in — declare it directly
extern "C" {
    size_t tinfl_decompress_mem_to_mem(void *pOut_buf, size_t out_buf_len,
        const void *pSrc_buf, size_t src_buf_len, int flags);
}
#define TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <driver/gpio.h>

// ── Power management ──
// Light sleep between loop polls: idle current ~40mA -> ~1-3mA, buttons
// wake in <1ms. BENCH-TEST FLAG: set to 0 to fall back to plain delay()
// (also set 0 when debugging — Serial output garbles during naps).
#define USE_LIGHT_SLEEP 1
// PCF8563 INT -> GPIO 27 (verified against stock Watchy V2 config.h;
// pin block matches this board: DC=10 RES=9 CS=5 BUSY=19, btns 26/25/4)
#define RTC_INT_PIN 27
#include <vector>
#include "DictEntry.h"
#include "tsumego.h"
#include "sky.h"

// NOTE: no function definitions above this line! The Arduino IDE injects
// its auto-generated prototypes at the FIRST function definition in the
// file; if that sits above the includes, prototypes returning DictEntry
// land before the type exists ("'DictEntry' does not name a type").
inline void cpuFast(){setCpuFrequencyMhz(240);}
inline void cpuSlow(){setCpuFrequencyMhz(80);}

TsumegoTrainer tsumego;
SkyView sky;

// ── Display ──
#define DISPLAY_CS 5
#define DISPLAY_DC 10
#define DISPLAY_RES 9
#define DISPLAY_BUSY 19
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(DISPLAY_CS, DISPLAY_DC, DISPLAY_RES, DISPLAY_BUSY));

// ── Buttons ──
#define BTN_TR 35
#define BTN_TL 25
#define BTN_BR 4
#define BTN_BL 26
#define DEBOUNCE_MS 80
#define SETTLE_MS 10

// ── Files ──
#define DICT_FILE "/dictionary.cdb"
#define STATE_FILE "/lastword.txt"

// ── Modes ──
enum AppMode { MODE_VIEW, MODE_SEARCH, MODE_MATCHES, MODE_SETTIME, MODE_PICTURES, MODE_MENU, MODE_TSUMEGO, MODE_SKY, MODE_CLOCK };
AppMode appMode = MODE_VIEW;

// ── Reference files (dictionary + gazetteer share one engine) ──
const char* REF_FILES[2] = {"/dictionary.cdb", "/gazetteer.cdb"};
const char* REF_NAMES[2] = {"Dictionary", "Gazetteer"};
int curRef = 0;

// ── Dictionary: compressed block index ──
struct BlockIdx { char firstWord[32]; uint32_t offset, compSize, rawSize; };
#define MAX_BLOCKS 240
#define COMP_BUF_SZ 13000
#define DECOMP_BUF_SZ 33000  // splitter guarantees blocks <= 32768; margin kept
BlockIdx blkIndex[MAX_BLOCKS];
int blkCount = 0, totalEntries = 0;
uint8_t compBuf[COMP_BUF_SZ];
uint8_t decBuf[DECOMP_BUF_SZ];
int cachedBlk = -1;

// ── View state ──
String currentWord = "", currentDef = "";
int curBlk = 0, curLine = 0;
int defPage = 0, defTotalPages = 1;
#define DEF_LINES_PER_PAGE 11
#define DEF_LINE_HEIGHT 13

// ── Search ──
String searchQuery = "";
int charIndex = 0, matchScrollIndex = 0;
std::vector<String> currentMatches;
const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz-";
#define CHAR_COUNT 27
#define DONE_INDEX 27
#define GRID_COLS 6
#define GRID_ROWS 5
#define GRID_X 10
#define GRID_Y 48
#define CELL_W 30
#define CELL_H 18
#define GRID_REGION_Y GRID_Y
#define GRID_REGION_H (GRID_ROWS * CELL_H)
#define HOLD_DELAY 400
#define REPEAT_RATE 100
int heldButton = -1;
unsigned long holdStart = 0, lastRepeat = 0;

// ── Time ──
int setTimeField = 0, setHours = 0, setMinutes = 0;
int setDay = 1, setMonth = 1, setYear = 2026;

// ── Photos ──
#define IMG_SIZE 5000
#define MAX_IMAGES 10
int currentImageIndex = 0, totalImages = 0;

// ── Deep-sleep wake state (RTC memory: survives deep sleep, clears on
//    power loss / reflash). Wake restores the mode you fell asleep in. ──
RTC_DATA_ATTR uint8_t wakeMode = 0;     // 0 dict 1 photos 2 tsumego 3 gazetteer 4 sky
RTC_DATA_ATTR uint8_t wakePicIdx = 0;
RTC_DATA_ATTR char wakeGazWord[32] = {0};

// ── Location config (/location.txt in SPIFFS, key=value):
//      lat=45.3475
//      lon=-75.7566
//      utc=-5            standard-time UTC offset, hours
//      dst=NA            NONE | NA | EU
//      tz=est,edt        optional display labels
//    Defaults (Ottawa) live in sky.h. Cached in RTC memory so the clock's
//    minute-tick wake gets the right city without mounting SPIFFS. ──
#define LOC_MAGIC 0x4C4F4331   // "LOC1"
RTC_DATA_ATTR uint32_t locMagic = 0;
RTC_DATA_ATTR double locLat, locLon;
RTC_DATA_ATTR int8_t locBase, locRule;
RTC_DATA_ATTR char locTzStd[6], locTzDst[6];

void cacheLocation(){
    locLat=skyLat; locLon=skyLon; locBase=(int8_t)skyBaseUtc; locRule=(int8_t)skyDstRule;
    strncpy(locTzStd,skyTzStd,5); locTzStd[5]=0;
    strncpy(locTzDst,skyTzDst,5); locTzDst[5]=0;
    locMagic=LOC_MAGIC;
}
void restoreLocationFromRTC(){
    if(locMagic!=LOC_MAGIC)return;
    skyLat=locLat; skyLon=locLon; skyBaseUtc=locBase; skyDstRule=locRule;
    strncpy(skyTzStd,locTzStd,5); skyTzStd[5]=0;
    strncpy(skyTzDst,locTzDst,5); skyTzDst[5]=0;
}
void loadLocation(){
    File f=SPIFFS.open("/location.txt","r");
    if(f){
        while(f.available()){
            String line=f.readStringUntil('\n');line.trim();
            int eq=line.indexOf('=');
            if(eq<1)continue;
            String k=line.substring(0,eq);k.trim();k.toLowerCase();
            String v=line.substring(eq+1);v.trim();
            if(k=="lat")skyLat=v.toFloat();
            else if(k=="lon")skyLon=v.toFloat();
            else if(k=="utc")skyBaseUtc=v.toInt();
            else if(k=="dst"){
                String r=v;r.toLowerCase();
                if(r=="na")skyDstRule=1;
                else if(r=="eu")skyDstRule=2;
                else skyDstRule=0;
            }else if(k=="tz"){
                int c=v.indexOf(',');
                if(c>0){
                    v.substring(0,c).toCharArray(skyTzStd,6);
                    v.substring(c+1).toCharArray(skyTzDst,6);
                }
            }
        }
        f.close();
    }
    cacheLocation();
}
uint8_t imgBuffer[IMG_SIZE];

// ── Menu / Sleep ──
#define LONG_PRESS_MS 1500
// Per-mode idle timeout before deep sleep. Clock sleeps fast — the RTC
// minute alarm keeps the face ticking through deep sleep anyway.
uint32_t sleepTimeoutMs(){
    switch(appMode){
        case MODE_MENU:     return 20000;
        case MODE_CLOCK:    return 15000;
        case MODE_SETTIME:  return 45000;
        case MODE_PICTURES: return 45000;
        case MODE_SKY:      return 45000;
        case MODE_TSUMEGO:  return 90000;
        default:            return 60000;   // dictionary/gazetteer reading
    }
}
#define MENU_ITEMS 5
int menuIndex = 0;
const char* menuLabels[] = {"Time", "View Photos", "Tsumego", "Gazetteer", "Sky"};
unsigned long lastActivity = 0;

// Forward declarations
void showWordOfTheDay();
void drawViewScreen();
void drawSearchScreen();
void drawSearchScreenPartial();
void drawPickerPartial();
void drawMatchSelectScreen();
void drawSetTimeScreen(bool full=false);
void drawClockScreen(bool full=false);
void enterClockMode();
void enterSetTimeMode();
void handleButtonsClock();
void drawPictureScreen();
void drawMenuScreen();
void drawMessage(const String &msg);
DictEntry binarySearch(String query);
DictEntry getEntryInBlock(int lineIdx);
DictEntry getNextEntry();
DictEntry getPrevEntry();
DictEntry getRandomEntry();
String loadStateWord();
void showEntry(DictEntry e);
void exitToDictionary();

// ── RTC ──
#define RTC_ADDR 0x51
uint8_t decToBcd(int v){return(uint8_t)((v/10*16)+(v%10));}
String getTimeString(){
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x02);Wire.endTransmission();
    Wire.requestFrom(RTC_ADDR,3);if(Wire.available()<3)return"??:??";
    Wire.read()&0x7F;uint8_t m=Wire.read()&0x7F,h=Wire.read()&0x3F;
    char b[6];sprintf(b,"%02d:%02d",(h>>4)*10+(h&0xF),(m>>4)*10+(m&0xF));return String(b);
}
void readCurrentTime(int&h,int&m){
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x02);Wire.endTransmission();
    Wire.requestFrom(RTC_ADDR,3);if(Wire.available()<3){h=0;m=0;return;}
    Wire.read();uint8_t mn=Wire.read()&0x7F,hr=Wire.read()&0x3F;
    h=(hr>>4)*10+(hr&0xF);m=(mn>>4)*10+(mn&0xF);
}
void writeTime(int h,int m){
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x02);
    Wire.write(0x00);Wire.write(decToBcd(m));Wire.write(decToBcd(h));
    Wire.endTransmission();
}
// PCF8563 date registers: 0x05 day, 0x06 weekday, 0x07 month, 0x08 year(00-99)
void readCurrentDate(int&d,int&mo,int&y){
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x05);Wire.endTransmission();
    Wire.requestFrom(RTC_ADDR,4);
    if(Wire.available()<4){d=1;mo=1;y=2026;return;}
    uint8_t db=Wire.read()&0x3F;Wire.read();uint8_t mb=Wire.read()&0x1F,yb=Wire.read();
    d=(db>>4)*10+(db&0xF);mo=(mb>>4)*10+(mb&0xF);y=2000+(yb>>4)*10+(yb&0xF);
    if(d<1||d>31)d=1;if(mo<1||mo>12)mo=1;if(y<2020||y>2099)y=2026;
}
void writeDate(int d,int mo,int y){
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x05);
    Wire.write(decToBcd(d));Wire.write(0x00);
    Wire.write(decToBcd(mo));           // century bit 0 = 20xx
    Wire.write(decToBcd(y-2000));
    Wire.endTransmission();
}
// ── PCF8563 minute alarm (drives the watchface tick through deep sleep).
//    Register semantics verified against Rtc_Pcf8563 lib (stock Watchy):
//    0x09 alarm-minute, bit7=0 ENABLES (inverted); status2 0x01:
//    AIE=0x02, AF=0x08; flags clear by writing 0. ──
uint8_t rtcReadStatus2(){
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x01);Wire.endTransmission();
    Wire.requestFrom(RTC_ADDR,1);
    return Wire.available()?Wire.read():0;
}
void rtcArmMinuteAlarm(){
    int h,m;readCurrentTime(h,m);
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x09);
    Wire.write(decToBcd((m+1)%60)&0x7F);        // minute alarm ENABLED
    Wire.write(0x80);Wire.write(0x80);Wire.write(0x80); // hr/day/wkday off
    Wire.endTransmission();
    uint8_t s2=(rtcReadStatus2()&~0x08)|0x02;   // clear AF, set AIE
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x01);Wire.write(s2);Wire.endTransmission();
}
void rtcDisarmAlarm(){
    uint8_t s2=rtcReadStatus2()&~(0x08|0x02);   // clear AF and AIE
    Wire.beginTransmission(RTC_ADDR);Wire.write(0x01);Wire.write(s2);Wire.endTransmission();
}


// ════════════════════════════
//  COMPRESSED DICTIONARY
// ════════════════════════════

bool loadBlockIndex() {
    File f = SPIFFS.open(REF_FILES[curRef], "r");
    if (!f) return false;
    char magic[4]; f.read((uint8_t*)magic, 4);
    if (memcmp(magic, "WCDB", 4) != 0) { f.close(); return false; }
    uint32_t bc, ec;
    f.read((uint8_t*)&bc, 4); f.read((uint8_t*)&ec, 4);
    blkCount = min((int)bc, MAX_BLOCKS); totalEntries = ec;
    for (int i = 0; i < blkCount; i++) {
        f.read((uint8_t*)blkIndex[i].firstWord, 32);
        f.read((uint8_t*)&blkIndex[i].offset, 4);
        f.read((uint8_t*)&blkIndex[i].compSize, 4);
        f.read((uint8_t*)&blkIndex[i].rawSize, 4);
    }
    f.close();
    Serial.printf("Dict: %d blocks, %d entries\n", blkCount, totalEntries);
    return true;
}

bool decompressBlock(int idx) {
    if (idx == cachedBlk) return true;
    if (idx < 0 || idx >= blkCount) return false;
    File f = SPIFFS.open(REF_FILES[curRef], "r");
    if (!f) return false;
    f.seek(blkIndex[idx].offset);
    f.read(compBuf, blkIndex[idx].compSize);
    f.close();
    cpuFast();
    size_t out = tinfl_decompress_mem_to_mem(
        decBuf, blkIndex[idx].rawSize, compBuf, blkIndex[idx].compSize, 0);
    cpuSlow();
    if (out == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
    cachedBlk = idx;
    return true;
}

// Switch between dictionary and gazetteer: reload index, drop cache.
// Returns false (and restores the previous reference) if the file is
// missing or malformed.
bool indexLoaded = false;
bool switchRef(int newRef) {
    if (newRef == curRef && indexLoaded) return true;
    if (!SPIFFS.exists(REF_FILES[newRef])) return false;
    int prev = curRef;
    curRef = newRef;
    cachedBlk = -1;
    cpuFast();
    bool ok = loadBlockIndex();
    cpuSlow();
    if (!ok) {
        curRef = prev; cachedBlk = -1;
        cpuFast(); indexLoaded = loadBlockIndex(); cpuSlow();
        return false;
    }
    indexLoaded = true;
    curBlk = 0; curLine = 0;
    return true;
}

// Lazy dictionary index: loaded on first need, not at boot — waking into
// photos/tsumego/sky/clock never touches it.
bool ensureIndex() {
    if (indexLoaded) return true;
    cpuFast();
    bool ok = loadBlockIndex();
    cpuSlow();
    if (ok) indexLoaded = true;
    return ok;
}

// Single exit point back to the dictionary from other modes; loads the
// index and restores the bookmarked word if this is the first visit.
void exitToDictionary() {
    appMode = MODE_VIEW;
    if (!indexLoaded) {
        if (!ensureIndex()) { drawMessage("Bad dict format!"); delay(2000); return; }
        String sw = loadStateWord();
        DictEntry e; e.found = false;
        if (sw.length() > 0) e = binarySearch(sw);
        if (e.found) { showEntry(e); drawViewScreen(); }
        else showWordOfTheDay();
    } else drawViewScreen();
}

// Find which block would contain a word
int findBlock(String query) {
    query.toLowerCase();
    int lo = 0, hi = blkCount - 1, result = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        String fw = String(blkIndex[mid].firstWord); fw.toLowerCase();
        if (query.compareTo(fw) >= 0) { result = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return result;
}

// Get entry at lineIdx within currently decompressed block
DictEntry getEntryInBlock(int lineIdx) {
    DictEntry e; e.found = false;
    int pos = 0, line = 0;
    int sz = blkIndex[cachedBlk].rawSize;
    while (pos < sz) {
        int end = pos;
        while (end < sz && decBuf[end] != '\n') end++;
        if (line == lineIdx) {
            int tab = pos;
            while (tab < end && decBuf[tab] != '\t') tab++;
            if (tab < end) {
                e.word = ""; for (int i = pos; i < tab; i++) e.word += (char)decBuf[i];
                e.definition = ""; for (int i = tab+1; i < end; i++) e.definition += (char)decBuf[i];
                e.found = true;
            }
            return e;
        }
        pos = end + 1; line++;
    }
    return e;
}

// Count lines in currently decompressed block
int linesInBlock() {
    int count = 0, sz = blkIndex[cachedBlk].rawSize;
    for (int i = 0; i < sz; i++) if (decBuf[i] == '\n') count++;
    return count;
}

DictEntry binarySearch(String query) {
    DictEntry result; result.found = false;
    query.toLowerCase();
    int blk = findBlock(query);
    // Check this block and possibly the previous one
    for (int b = max(0, blk - 1); b <= min(blk + 1, blkCount - 1); b++) {
        if (!decompressBlock(b)) continue;
        int pos = 0, line = 0;
        int sz = blkIndex[b].rawSize;
        while (pos < sz) {
            int end = pos;
            while (end < sz && decBuf[end] != '\n') end++;
            int tab = pos;
            while (tab < end && decBuf[tab] != '\t') tab++;
            if (tab < end) {
                String w = ""; for (int i = pos; i < tab; i++) w += (char)decBuf[i];
                String wl = w; wl.toLowerCase();
                if (wl == query) {
                    result.word = w;
                    result.definition = ""; for (int i = tab+1; i < end; i++) result.definition += (char)decBuf[i];
                    result.found = true;
                    curBlk = b; curLine = line;
                    return result;
                }
                if (wl > query) return result; // past it
            }
            pos = end + 1; line++;
        }
    }
    return result;
}

std::vector<String> prefixSearch(String prefix, int maxResults = 10) {
    std::vector<String> results;
    prefix.toLowerCase();
    if (prefix.length() == 0) return results;
    int blk = findBlock(prefix);
    for (int b = blk; b < blkCount && (int)results.size() < maxResults; b++) {
        if (!decompressBlock(b)) break;
        int pos = 0, sz = blkIndex[b].rawSize;
        while (pos < sz && (int)results.size() < maxResults) {
            int end = pos;
            while (end < sz && decBuf[end] != '\n') end++;
            int tab = pos;
            while (tab < end && decBuf[tab] != '\t') tab++;
            if (tab < end) {
                String w = ""; for (int i = pos; i < tab; i++) w += (char)decBuf[i];
                String wl = w; wl.toLowerCase();
                if (wl.startsWith(prefix)) results.push_back(w);
                else if (wl > prefix && !wl.startsWith(prefix)) return results;
            }
            pos = end + 1;
        }
    }
    return results;
}

DictEntry getNextEntry() {
    if (!decompressBlock(curBlk)) { DictEntry e; e.found=false; return e; }
    int nl = curLine + 1;
    int nb = curBlk;
    if (nl >= linesInBlock()) { nb++; nl = 0; if (nb >= blkCount) nb = 0; }
    if (!decompressBlock(nb)) { DictEntry e; e.found=false; return e; }
    DictEntry e = getEntryInBlock(nl);
    if (e.found) { curBlk = nb; curLine = nl; }
    return e;
}

DictEntry getPrevEntry() {
    int nl = curLine - 1;
    int nb = curBlk;
    if (nl < 0) {
        nb--; if (nb < 0) nb = blkCount - 1;
        if (!decompressBlock(nb)) { DictEntry e; e.found=false; return e; }
        nl = linesInBlock() - 1;
    }
    if (!decompressBlock(nb)) { DictEntry e; e.found=false; return e; }
    DictEntry e = getEntryInBlock(nl);
    if (e.found) { curBlk = nb; curLine = nl; }
    return e;
}

DictEntry getRandomEntry() {
    int rb = esp_random() % blkCount;
    if (!decompressBlock(rb)) { DictEntry e; e.found=false; return e; }
    int rl = esp_random() % linesInBlock();
    DictEntry e = getEntryInBlock(rl);
    if (e.found) { curBlk = rb; curLine = rl; }
    return e;
}


// ════════════════════════════
//  IMAGE MANAGEMENT
// ════════════════════════════

String getImageFilename(int i){char b[16];sprintf(b,"/img_%02d.bin",i);return String(b);}
int countImages(){int c=0;for(int i=0;i<MAX_IMAGES;i++){if(SPIFFS.exists(getImageFilename(i)))c++;else break;}return c;}
bool loadImage(int i){File f=SPIFFS.open(getImageFilename(i),"r");if(!f)return false;int r=f.read(imgBuffer,IMG_SIZE);f.close();return(r==IMG_SIZE);}
void deleteImage(int i){SPIFFS.remove(getImageFilename(i));for(int j=i+1;j<MAX_IMAGES;j++){String s=getImageFilename(j);if(!SPIFFS.exists(s))break;SPIFFS.rename(s,getImageFilename(j-1));}totalImages=countImages();if(currentImageIndex>=totalImages&&totalImages>0)currentImageIndex=totalImages-1;}


// ════════════════════════════
//  DISPLAY
// ════════════════════════════

int drawWrappedText(const String &text, int x, int startY, int maxW, int lh, int skip=0, int maxL=999) {
    int y=startY,tl=0,dl=0; String rem=text;
    while(rem.length()>0){
        int fc=0;int16_t tx,ty;uint16_t tw,th;
        for(int i=1;i<=(int)rem.length();i++){
            display.getTextBounds(rem.substring(0,i).c_str(),x,startY,&tx,&ty,&tw,&th);
            if((int)tw>maxW)break;fc=i;
        }
        if(fc==0)fc=1;
        if(fc<(int)rem.length()){int ls=rem.lastIndexOf(' ',fc);if(ls>0)fc=ls+1;}
        String line=rem.substring(0,fc);line.trim();tl++;
        if(tl>skip&&dl<maxL){display.setCursor(x,y);display.print(line);y+=lh;dl++;}
        rem=rem.substring(fc);rem.trim();
    }
    return tl;
}

void drawGrid(){
    display.setFont(&FreeMono9pt7b);
    for(int ci=0;ci<=DONE_INDEX;ci++){
        int r=ci/GRID_COLS,c=ci%GRID_COLS,x=GRID_X+c*CELL_W,y=GRID_Y+r*CELL_H;
        if(ci==charIndex){display.fillRect(x,y,CELL_W-2,CELL_H-2,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);}
        else display.setTextColor(GxEPD_BLACK);
        if(ci<CHAR_COUNT){char b[2]={(char)(ALPHABET[ci]-32),0};display.setCursor(x+8,y+13);display.print(b);}
        else if(ci==DONE_INDEX){display.setCursor(x+3,y+13);display.print("GO");}
        display.setTextColor(GxEPD_BLACK);
    }
}

void drawPickerPartial(){
    display.setPartialWindow(0,GRID_REGION_Y,200,GRID_REGION_H);display.firstPage();
    do{display.fillRect(0,GRID_REGION_Y,200,GRID_REGION_H,GxEPD_WHITE);drawGrid();}while(display.nextPage());
}

void drawSearchScreen(){
    display.setFullWindow();display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);
        display.fillRect(0,0,200,16,GxEPD_BLACK);display.setFont(NULL);display.setTextColor(GxEPD_WHITE);
        display.setCursor(4,4);display.print("SEARCH");display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSansBold9pt7b);display.setCursor(4,38);
        if(searchQuery.length()==0)display.print("_");else display.print(searchQuery);
        if(searchQuery.length()>0){int16_t tx,ty;uint16_t tw,th;display.getTextBounds(searchQuery.c_str(),4,38,&tx,&ty,&tw,&th);display.drawLine(4+tw+2,28,4+tw+2,42,GxEPD_BLACK);}
        drawGrid();
        int bg=GRID_Y+GRID_REGION_H+2;
        if(searchQuery.length()>0&&currentMatches.size()>0){
            display.setFont(NULL);display.setCursor(4,bg);display.print("Matches:");
            display.setFont(&FreeMono9pt7b);int my=bg+16;
            for(int i=0;i<min((int)currentMatches.size(),3);i++){display.setCursor(8,my);String m=currentMatches[i];if((int)m.length()>18)m=m.substring(0,18);display.print(m);my+=14;}
            if((int)currentMatches.size()>3){display.setFont(NULL);display.setCursor(8,my);display.printf("+ %d more",(int)currentMatches.size()-3);}
        }else if(searchQuery.length()>0){display.setFont(&FreeMono9pt7b);display.setCursor(8,bg+16);display.print("(no matches)");}
        else{display.setFont(&FreeSansBold12pt7b);String ts=getTimeString();int16_t tx,ty;uint16_t tw,th;display.getTextBounds(ts.c_str(),0,0,&tx,&ty,&tw,&th);display.setCursor((200-tw)/2,bg+20);display.print(ts);}
        display.setFont(NULL);display.setCursor(4,192);display.print("TR:Add TL:Del BL/BR:Move");
    }while(display.nextPage());
}

// Partial refresh version — no flash, used when typing letters
void drawSearchScreenPartial(){
    display.setPartialWindow(0,0,200,200);display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);
        display.fillRect(0,0,200,16,GxEPD_BLACK);display.setFont(NULL);display.setTextColor(GxEPD_WHITE);
        display.setCursor(4,4);display.print("SEARCH");display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSansBold9pt7b);display.setCursor(4,38);
        if(searchQuery.length()==0)display.print("_");else display.print(searchQuery);
        if(searchQuery.length()>0){int16_t tx,ty;uint16_t tw,th;display.getTextBounds(searchQuery.c_str(),4,38,&tx,&ty,&tw,&th);display.drawLine(4+tw+2,28,4+tw+2,42,GxEPD_BLACK);}
        drawGrid();
        int bg=GRID_Y+GRID_REGION_H+2;
        if(searchQuery.length()>0&&currentMatches.size()>0){
            display.setFont(NULL);display.setCursor(4,bg);display.print("Matches:");
            display.setFont(&FreeMono9pt7b);int my=bg+16;
            for(int i=0;i<min((int)currentMatches.size(),3);i++){display.setCursor(8,my);String m=currentMatches[i];if((int)m.length()>18)m=m.substring(0,18);display.print(m);my+=14;}
            if((int)currentMatches.size()>3){display.setFont(NULL);display.setCursor(8,my);display.printf("+ %d more",(int)currentMatches.size()-3);}
        }else if(searchQuery.length()>0){display.setFont(&FreeMono9pt7b);display.setCursor(8,bg+16);display.print("(no matches)");}
        else{display.setFont(&FreeSansBold12pt7b);String ts=getTimeString();int16_t tx,ty;uint16_t tw,th;display.getTextBounds(ts.c_str(),0,0,&tx,&ty,&tw,&th);display.setCursor((200-tw)/2,bg+20);display.print(ts);}
        display.setFont(NULL);display.setCursor(4,192);display.print("TR:Add TL:Del BL/BR:Move");
    }while(display.nextPage());
}

void drawViewScreen(){
    display.setFullWindow();display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);display.setFont(&FreeSansBold9pt7b);display.setTextColor(GxEPD_BLACK);
        display.setCursor(4,18);display.print(currentWord);display.drawLine(4,24,196,24,GxEPD_BLACK);
        display.setFont(&FreeMono9pt7b);
        int sl=defPage*DEF_LINES_PER_PAGE;
        int tl=drawWrappedText(currentDef,4,40,192,DEF_LINE_HEIGHT,sl,DEF_LINES_PER_PAGE);
        defTotalPages=(tl+DEF_LINES_PER_PAGE-1)/DEF_LINES_PER_PAGE;if(defTotalPages<1)defTotalPages=1;
        display.setFont(NULL);display.setCursor(4,192);
        if(defTotalPages>1)display.printf("TR:Srch TL:Rand Hold:Menu p%d/%d",defPage+1,defTotalPages);
        else display.print("TR:Srch TL:Rand Hold:Menu");
    }while(display.nextPage());
}

void drawMatchSelectScreen(){
    display.setFullWindow();display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);display.fillRect(0,0,200,16,GxEPD_BLACK);display.setFont(NULL);display.setTextColor(GxEPD_WHITE);
        display.setCursor(4,4);display.printf("MATCHES: %s",searchQuery.c_str());display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSans9pt7b);int y=36,si=max(0,matchScrollIndex-3),sc=min((int)currentMatches.size()-si,8);
        for(int i=0;i<sc;i++){int idx=si+i;String m=currentMatches[idx];if((int)m.length()>20)m=m.substring(0,20);
            if(idx==matchScrollIndex){display.fillRect(0,y-14,200,19,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);display.setCursor(6,y);display.print("> "+m);display.setTextColor(GxEPD_BLACK);}
            else{display.setCursor(10,y);display.print(m);}y+=19;}
        display.setFont(NULL);display.setCursor(4,192);display.print("TR:Pick TL:Back BL/BR:Nav");
    }while(display.nextPage());
}

void drawMatchSelectPartial(){
    display.setPartialWindow(0,0,200,200);display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);display.fillRect(0,0,200,16,GxEPD_BLACK);display.setFont(NULL);display.setTextColor(GxEPD_WHITE);
        display.setCursor(4,4);display.printf("MATCHES: %s",searchQuery.c_str());display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSans9pt7b);int y=36,si=max(0,matchScrollIndex-3),sc=min((int)currentMatches.size()-si,8);
        for(int i=0;i<sc;i++){int idx=si+i;String m=currentMatches[idx];if((int)m.length()>20)m=m.substring(0,20);
            if(idx==matchScrollIndex){display.fillRect(0,y-14,200,19,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);display.setCursor(6,y);display.print("> "+m);display.setTextColor(GxEPD_BLACK);}
            else{display.setCursor(10,y);display.print(m);}y+=19;}
        display.setFont(NULL);display.setCursor(4,192);display.print("TR:Pick TL:Back BL/BR:Nav");
    }while(display.nextPage());
}

int setTimePartials=0;

// ── Clock face ──
int clockLastMin=-1;
RTC_DATA_ATTR int clockPartials=0;   // survives minute ticks: full refresh every 30
unsigned long clockLastPoll=0;
void drawClockScreen(bool full){
    int h,m,d,mo,y;
    readCurrentTime(h,m);readCurrentDate(d,mo,y);
    clockLastMin=m;
    if(!full&&++clockPartials>=30){full=true;}
    if(full){display.setFullWindow();clockPartials=0;}
    else display.setPartialWindow(0,0,200,200);
    static const char* dowName[7]={"sun","mon","tue","wed","thu","fri","sat"};
    static const char* monName[12]={"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};
    int dow=sky_dowSakamoto(y,mo,d);
    cpuFast();                          // NOAA sun math is software double FP
    double riseU,setU;
    int sunOk=sky_sunRiseSet(y,mo,d,skyLat,skyLon,&riseU,&setU);
    cpuSlow();
    int off=sky_utcOffset(y,mo,d,12,skyBaseUtc,skyDstRule);
    double riseL=sunOk?fmod(riseU+off+48.0,24.0):0;
    double setL=sunOk?fmod(setU+off+48.0,24.0):0;
    display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);display.setTextColor(GxEPD_BLACK);
        char tb[6];sprintf(tb,"%02d:%02d",h,m);
        display.setFont(&FreeSansBold24pt7b);
        int16_t tx,ty;uint16_t tw,th;
        display.getTextBounds(tb,0,0,&tx,&ty,&tw,&th);
        display.setCursor((200-tw)/2-tx,88);display.print(tb);
        char db[24];sprintf(db,"%s %s %d %d",dowName[dow],monName[mo-1],d,y);
        display.setFont(&FreeSans9pt7b);
        display.getTextBounds(db,0,0,&tx,&ty,&tw,&th);
        display.setCursor((200-tw)/2-tx,122);display.print(db);
        display.setFont(&FreeMono9pt7b);
        char sb[24];
        if(sunOk)sprintf(sb,"^%02d:%02d v%02d:%02d",
            (int)riseL,(int)lround((riseL-(int)riseL)*60)%60,
            (int)setL,(int)lround((setL-(int)setL)*60)%60);
        else sprintf(sb,"sun: --");
        display.getTextBounds(sb,0,0,&tx,&ty,&tw,&th);
        display.setCursor((200-tw)/2-tx,158);display.print(sb);
        display.setFont(NULL);display.setCursor(4,192);display.print("TR:Set  TL:Back");
    }while(display.nextPage());
}
void enterClockMode(){appMode=MODE_CLOCK;clockLastPoll=millis();drawClockScreen(true);}
void handleButtonsClock(){
    if(buttonPressed(BTN_TR)){enterSetTimeMode();return;}
    if(buttonPressed(BTN_TL)){exitToDictionary();return;}
    // minute tick: redraw when the RTC minute rolls over (partial, no
    // flash). Doesn't touch lastActivity, so idle sleep still happens.
    if(millis()-clockLastPoll>2000){
        clockLastPoll=millis();
        int h,m;readCurrentTime(h,m);
        if(m!=clockLastMin)drawClockScreen();
    }
}
void drawSetTimeScreen(bool full){
    if(!full&&++setTimePartials>=15){full=true;}
    if(full){display.setFullWindow();setTimePartials=0;}
    else display.setPartialWindow(0,0,200,200);
    display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);display.fillRect(0,0,200,16,GxEPD_BLACK);display.setFont(NULL);display.setTextColor(GxEPD_WHITE);
        display.setCursor(4,4);display.print("SET TIME & DATE");display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSansBold12pt7b);char hb[3],mb[3];sprintf(hb,"%02d",setHours);sprintf(mb,"%02d",setMinutes);int sx=60,ty2=60;
        if(setTimeField==0){display.fillRect(sx-4,ty2-20,38,28,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);}
        display.setCursor(sx,ty2);display.print(hb);display.setTextColor(GxEPD_BLACK);display.setCursor(sx+32,ty2);display.print(":");
        if(setTimeField==1){display.fillRect(sx+46,ty2-20,38,28,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);}
        display.setCursor(sx+48,ty2);display.print(mb);display.setTextColor(GxEPD_BLACK);
        // date row: day / month / year
        int dy=110;char db[3],mo2[3],yb[5];sprintf(db,"%02d",setDay);sprintf(mo2,"%02d",setMonth);sprintf(yb,"%04d",setYear);
        if(setTimeField==2){display.fillRect(24,dy-20,38,28,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);}
        display.setCursor(28,dy);display.print(db);display.setTextColor(GxEPD_BLACK);display.setCursor(60,dy);display.print("/");
        if(setTimeField==3){display.fillRect(72,dy-20,38,28,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);}
        display.setCursor(76,dy);display.print(mo2);display.setTextColor(GxEPD_BLACK);display.setCursor(108,dy);display.print("/");
        if(setTimeField==4){display.fillRect(118,dy-20,68,28,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);}
        display.setCursor(122,dy);display.print(yb);display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMono9pt7b);display.setCursor(20,150);
        static const char* fn[5]={"Set hours","Set minutes","Set day","Set month","Set year"};
        display.print(fn[setTimeField]);
        display.setFont(NULL);display.setCursor(4,192);display.print("TR:Next TL:Cancel BL/BR:+/-");
    }while(display.nextPage());
}

void drawPictureScreen(){
    display.setFullWindow();display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);
        if(totalImages==0){display.setFont(&FreeSansBold9pt7b);display.setTextColor(GxEPD_BLACK);display.setCursor(20,80);display.print("No photos yet!");display.setFont(&FreeMono9pt7b);display.setCursor(10,110);display.print("Upload via SPIFFS");display.setFont(NULL);display.setCursor(4,192);display.print("TR: Back");}
        else{if(loadImage(currentImageIndex))display.drawBitmap(0,0,imgBuffer,200,200,GxEPD_BLACK);}
    }while(display.nextPage());
}

void drawMenuScreen(){
   display.setPartialWindow(0, 0, 200, 200);
display.firstPage();
    do{
        display.fillScreen(GxEPD_WHITE);display.fillRect(0,0,200,16,GxEPD_BLACK);display.setFont(NULL);display.setTextColor(GxEPD_WHITE);
        display.setCursor(4,4);display.print("MENU");display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeSans9pt7b);int y=52;
        for(int i=0;i<MENU_ITEMS;i++){
            const char* lbl=(i==3)?REF_NAMES[1-curRef]:menuLabels[i];
            if(i==menuIndex){display.fillRect(0,y-16,200,24,GxEPD_BLACK);display.setTextColor(GxEPD_WHITE);display.setCursor(10,y);display.printf("> %s",lbl);display.setTextColor(GxEPD_BLACK);}
            else{display.setCursor(14,y);display.print(lbl);}y+=26;}
        display.setFont(NULL);display.setCursor(4,192);display.print("TR:Select TL:Back BL/BR:Nav");
    }while(display.nextPage());
}

void drawMessage(const String &msg){
    display.setFullWindow();display.firstPage();
    do{display.fillScreen(GxEPD_WHITE);display.setFont(&FreeSansBold9pt7b);display.setTextColor(GxEPD_BLACK);display.setCursor(10,100);display.print(msg);}
    while(display.nextPage());
}


// ════════════════════════════
//  STATE / REDIRECT
// ════════════════════════════

void saveState(){if(curRef!=0)return;File f=SPIFFS.open(STATE_FILE,"w");if(f){f.println(currentWord);f.close();}}
String loadStateWord(){File f=SPIFFS.open(STATE_FILE,"r");if(f){String s=f.readStringUntil('\n');s.trim();f.close();return s;}return"";}

void showEntry(DictEntry e){
    if(!e.found)return;
    String def=e.definition;
    if(def.startsWith(">")){
        String target=def.substring(1);target.trim();
        DictEntry resolved=binarySearch(target);
        if(resolved.found){currentWord=e.word+" -> "+resolved.word;currentDef=resolved.definition;}
        else{currentWord=e.word;currentDef="see: "+target;}
    }else{currentWord=e.word;currentDef=def;}
    saveState();defPage=0;
}


// ════════════════════════════
//  SEARCH / MATCH LOGIC
// ════════════════════════════

void updateMatches(){if(searchQuery.length()>0)currentMatches=prefixSearch(searchQuery,10);else currentMatches.clear();matchScrollIndex=0;}
void enterSearchMode(){appMode=MODE_SEARCH;searchQuery="";charIndex=0;currentMatches.clear();matchScrollIndex=0;drawSearchScreen();}
void exitSearchMode(){exitToDictionary();}
void enterMatchSelectMode(){if(currentMatches.size()==0)return;appMode=MODE_MATCHES;matchScrollIndex=0;drawMatchSelectScreen();}
void selectMatch(int i){if(i<0||i>=(int)currentMatches.size())return;DictEntry e=binarySearch(currentMatches[i]);if(e.found){showEntry(e);appMode=MODE_VIEW;drawViewScreen();}}
void executeSearch(){if(searchQuery.length()==0)return;DictEntry e=binarySearch(searchQuery);if(e.found){showEntry(e);appMode=MODE_VIEW;drawViewScreen();return;}if(currentMatches.size()>0){enterMatchSelectMode();return;}drawMessage("Not found:\n"+searchQuery);delay(1500);drawSearchScreenPartial();}
void enterSetTimeMode(){readCurrentTime(setHours,setMinutes);readCurrentDate(setDay,setMonth,setYear);setTimeField=0;appMode=MODE_SETTIME;drawSetTimeScreen(true);}
void confirmSetTime(){writeTime(setHours,setMinutes);writeDate(setDay,setMonth,setYear);drawMessage("Time set!");delay(800);enterClockMode();}
void enterPictureMode(){totalImages=countImages();currentImageIndex=0;appMode=MODE_PICTURES;drawPictureScreen();}


// ════════════════════════════
//  BUTTONS
// ════════════════════════════

bool buttonPressed(int pin){
    delay(SETTLE_MS);if(digitalRead(pin)==HIGH){delay(DEBOUNCE_MS);
    int cr=(pin==BTN_BL)?3:1;for(int i=0;i<cr;i++){if(digitalRead(pin)!=HIGH)return false;if(i<cr-1)delay(20);}
    unsigned long to=millis()+3000;while(digitalRead(pin)==HIGH&&millis()<to)delay(10);
    delay(30);lastActivity=millis();return true;}return false;
}

void handleButtonsView(){
    delay(SETTLE_MS);
    if(digitalRead(BTN_TL)==HIGH){delay(DEBOUNCE_MS);if(digitalRead(BTN_TL)==HIGH){
        unsigned long ps=millis();
        while(digitalRead(BTN_TL)==HIGH){if(millis()-ps>LONG_PRESS_MS){while(digitalRead(BTN_TL)==HIGH)delay(10);lastActivity=millis();menuIndex=0;appMode=MODE_MENU;drawMenuScreen();return;}delay(10);}
        lastActivity=millis();showWordOfTheDay();return;}}
    if(buttonPressed(BTN_TR)){enterSearchMode();return;}
    if(buttonPressed(BTN_BR)){if(defPage<defTotalPages-1){defPage++;drawViewScreen();}else{DictEntry e=getNextEntry();if(e.found){showEntry(e);drawViewScreen();}}return;}
    if(buttonPressed(BTN_BL)){if(defPage>0){defPage--;drawViewScreen();}else{DictEntry e=getPrevEntry();if(e.found){showEntry(e);drawViewScreen();}}return;}
}

void moveRight(){int r=charIndex/GRID_COLS,c=charIndex%GRID_COLS;int mc=(r==GRID_ROWS-1)?(DONE_INDEX%GRID_COLS):(GRID_COLS-1);c++;if(c>mc)c=0;charIndex=r*GRID_COLS+c;}
void moveDown(){int r=charIndex/GRID_COLS,c=charIndex%GRID_COLS;r++;if(r>=GRID_ROWS||r*GRID_COLS+c>DONE_INDEX)r=0;charIndex=r*GRID_COLS+c;}

void handleButtonsSearch(){
    delay(SETTLE_MS);bool brH=(digitalRead(BTN_BR)==HIGH),blH=(digitalRead(BTN_BL)==HIGH);
    if(blH){delay(20);blH=(digitalRead(BTN_BL)==HIGH);}int an=brH?BTN_BR:(blH?BTN_BL:-1);
    if(an!=-1){if(heldButton!=an){delay(DEBOUNCE_MS);if(digitalRead(an)==HIGH){heldButton=an;holdStart=millis();lastRepeat=millis();if(an==BTN_BR)moveRight();else moveDown();drawPickerPartial();}}
    else{unsigned long now=millis();if(now-holdStart>HOLD_DELAY&&now-lastRepeat>REPEAT_RATE){if(an==BTN_BR)moveRight();else moveDown();drawPickerPartial();lastRepeat=now;}}return;}
    heldButton=-1;
    if(buttonPressed(BTN_TR)){if(charIndex==DONE_INDEX){if(currentMatches.size()>0)enterMatchSelectMode();else executeSearch();}else{searchQuery+=ALPHABET[charIndex];charIndex=0;updateMatches();drawSearchScreenPartial();}return;}
    if(buttonPressed(BTN_TL)){if(searchQuery.length()>0){searchQuery=searchQuery.substring(0,searchQuery.length()-1);updateMatches();drawSearchScreenPartial();}else exitSearchMode();return;}
}

void handleButtonsMatches(){
    if(buttonPressed(BTN_BR)){if(matchScrollIndex<(int)currentMatches.size()-1){matchScrollIndex++;drawMatchSelectPartial();}return;}
    if(buttonPressed(BTN_BL)){if(matchScrollIndex>0){matchScrollIndex--;drawMatchSelectPartial();}return;}
    if(buttonPressed(BTN_TR)){selectMatch(matchScrollIndex);return;}
    if(buttonPressed(BTN_TL)){appMode=MODE_SEARCH;drawSearchScreenPartial();return;}
}
void bumpSetField(int dir){
    switch(setTimeField){
        case 0:setHours=(setHours+24+dir)%24;break;
        case 1:
            if(dir==5||dir==-5)setMinutes=((setMinutes/5+(dir>0?1:-1))%12+12)%12*5; // snap to :00/:05/...
            else setMinutes=(setMinutes+60+dir)%60;
            break;
        case 2:setDay=((setDay-1+31+dir)%31)+1;break;
        case 3:setMonth=((setMonth-1+12+dir)%12)+1;break;
        case 4:setYear+=dir;if(setYear<2020)setYear=2099;if(setYear>2099)setYear=2020;break;
    }
}
// Hold-to-repeat: bump immediately, then keep bumping while held.
// Each partial redraw (~300ms) is the natural repeat interval; after
// 5 repeats, minutes and year accelerate to steps of 5.
void bumpHold(int pin,int dir){
    delay(DEBOUNCE_MS);
    int cr=(pin==BTN_BL)?3:1;                 // GPIO26 extra debounce
    for(int i=0;i<cr;i++){if(digitalRead(pin)!=HIGH)return;if(i<cr-1)delay(20);}
    lastActivity=millis();
    int reps=0;
    for(;;){
        int step=dir;
        if(reps>=5&&(setTimeField==1||setTimeField==4))step=dir*5;
        bumpSetField(step);
        drawSetTimeScreen();                  // partial: ~300ms, no flash
        if(digitalRead(pin)!=HIGH)break;
        // brief hold check before first repeat so a tap = single step
        if(reps==0){unsigned long t=millis()+250;
            while(millis()<t){if(digitalRead(pin)!=HIGH)break;delay(10);}
            if(digitalRead(pin)!=HIGH)break;}
        reps++;
    }
    delay(30);lastActivity=millis();
}
void handleButtonsSetTime(){
    delay(SETTLE_MS);
    if(digitalRead(BTN_BR)==HIGH){bumpHold(BTN_BR,+1);return;}
    if(digitalRead(BTN_BL)==HIGH){bumpHold(BTN_BL,-1);return;}
    if(buttonPressed(BTN_TR)){if(setTimeField<4){setTimeField++;drawSetTimeScreen();}else confirmSetTime();return;}
    if(buttonPressed(BTN_TL)){enterClockMode();return;}
}
void handleButtonsPictures(){
    if(buttonPressed(BTN_BR)){if(totalImages>0){currentImageIndex=(currentImageIndex+1)%totalImages;drawPictureScreen();}return;}
    if(buttonPressed(BTN_BL)){if(totalImages>0){currentImageIndex=(currentImageIndex+totalImages-1)%totalImages;drawPictureScreen();}return;}
    if(buttonPressed(BTN_TR)){exitToDictionary();return;}
    if(buttonPressed(BTN_TL)){if(totalImages>0){deleteImage(currentImageIndex);drawPictureScreen();}return;}
}
void handleButtonsMenu(){
    if(buttonPressed(BTN_BR)){menuIndex=(menuIndex+1)%MENU_ITEMS;drawMenuScreen();return;}
    if(buttonPressed(BTN_BL)){menuIndex=(menuIndex+MENU_ITEMS-1)%MENU_ITEMS;drawMenuScreen();return;}
    if(buttonPressed(BTN_TR)){
    switch(menuIndex){
        case 0:enterClockMode();break;
        case 1:enterPictureMode();break;
        case 2:
            if(tsumego.begin(display)){appMode=MODE_TSUMEGO;}
            else{drawMessage("No problems.bin!");delay(1500);exitToDictionary();}
            break;
        case 3:
            if(switchRef(1-curRef)){
                appMode=MODE_VIEW;
                DictEntry e=getRandomEntry();
                if(e.found){showEntry(e);drawViewScreen();}else showWordOfTheDay();
            }else{drawMessage("No gazetteer.cdb!");delay(1500);exitToDictionary();}
            break;
        case 4:{
            int dh,dm,dd,dmo,dy;
            readCurrentTime(dh,dm);readCurrentDate(dd,dmo,dy);
            sky.begin(display,dy,dmo,dd,dh,dm);
            appMode=MODE_SKY;
            break;
        }
    }
    return;
}
    if(buttonPressed(BTN_TL)){exitToDictionary();return;}
}

void handleButtons(){
    switch(appMode){
        case MODE_VIEW:handleButtonsView();break;case MODE_SEARCH:handleButtonsSearch();break;
        case MODE_MATCHES:handleButtonsMatches();break;case MODE_SETTIME:handleButtonsSetTime();break;
        case MODE_PICTURES:handleButtonsPictures();break;case MODE_MENU:handleButtonsMenu();break;
        case MODE_CLOCK:handleButtonsClock();break;
        case MODE_TSUMEGO:{
            // Tsumego does its own raw polling (hold-to-pass, accelerating
            // scrubber), so it can't use the blocking buttonPressed() helper.
            // poll() returns: 0 = idle, 1 = button activity, 2 = exit request.
            int r=tsumego.poll(display);
            if(r>0)lastActivity=millis();
            if(r==2){exitToDictionary();}
            break;
        }
        case MODE_SKY:{
            int r=sky.poll(display);
            if(r>0)lastActivity=millis();
            if(r==2){exitToDictionary();}
            break;
        }
    }
}


// ════════════════════════════
//  WORD OF THE DAY / SLEEP
// ════════════════════════════

void showWordOfTheDay(){
    for(int a=0;a<5;a++){DictEntry e=getRandomEntry();if(e.found){if(e.definition.startsWith(">")&&a<4)continue;showEntry(e);drawViewScreen();return;}}
}

void enterDeepSleep(){
    // remember where we are so wake returns here
    switch(appMode){
        case MODE_PICTURES: wakeMode=1; wakePicIdx=(uint8_t)currentImageIndex; break;
        case MODE_TSUMEGO:  wakeMode=2; break;   // exact problem resumes via NVS
        case MODE_SKY:      wakeMode=4; break;
        case MODE_CLOCK:
        case MODE_SETTIME:  wakeMode=5; break;   // wake shows the clock (watchface!)
        default:
            wakeMode=(curRef==1)?3:0;
            if(curRef==1){strncpy(wakeGazWord,currentWord.c_str(),31);wakeGazWord[31]=0;}
            break;
    }
    display.hibernate();               // e-ink controller to deep sleep —
                                       // without this it drains the battery
    // Wake sources are GLOBAL state: the 250ms nap timer armed by the
    // light-sleep loop would otherwise fire 250ms into deep sleep and
    // boot us into the dictionary. Clear everything, then arm only ours.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if(wakeMode==5){                   // clock: minute tick wakes us (ext0)
        rtcArmMinuteAlarm();
        esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_INT_PIN,0); // INT active low
    }else{
        rtcDisarmAlarm();
    }
    uint64_t mask=(1ULL<<BTN_TR)|(1ULL<<BTN_TL)|(1ULL<<BTN_BR)|(1ULL<<BTN_BL);
    esp_sleep_enable_ext1_wakeup(mask,ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();
}


// ════════════════════════════
//  SETUP & LOOP
// ════════════════════════════

// Increase loop task stack (tinfl decompressor needs ~11KB on stack)
SET_LOOP_TASK_STACK_SIZE(32768);

void setup(){
    cpuSlow();                          // 80MHz baseline; bursts for heavy ops
    Serial.begin(115200);delay(100);
    pinMode(BTN_TR,INPUT);pinMode(BTN_TL,INPUT);pinMode(BTN_BR,INPUT);pinMode(BTN_BL,INPUT);
    Wire.begin(21,22);
    esp_sleep_wakeup_cause_t wk=esp_sleep_get_wakeup_cause();
    display.init(0,!(wk==ESP_SLEEP_WAKEUP_EXT1||wk==ESP_SLEEP_WAKEUP_EXT0));
    display.setRotation(0);display.setTextWrap(false);

    // ── RTC minute tick (ext0): redraw the clock and go straight back to
    //    sleep. No SPIFFS, no index — awake for well under a second. ──
    if(wk==ESP_SLEEP_WAKEUP_EXT0){
        restoreLocationFromRTC();    // right city for sunrise, no SPIFFS
        appMode=MODE_CLOCK;
        drawClockScreen(false);          // partial; full every 30th via RTC counter
        enterDeepSleep();                // re-arms the next minute, never returns
    }

#if USE_LIGHT_SLEEP
    gpio_wakeup_enable((gpio_num_t)BTN_TR,GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_TL,GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_BR,GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_BL,GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();
#endif

    if(!SPIFFS.begin(true)){drawMessage("SPIFFS Error!");while(1)delay(1000);}
    if(!SPIFFS.exists(DICT_FILE)){drawMessage("No dictionary.cdb!");while(1)delay(1000);}
    loadLocation();                  // /location.txt or Ottawa defaults
    // dictionary index loads lazily (ensureIndex) — waking into photos,
    // tsumego, sky, or the clock skips it entirely

    totalImages=countImages();

    // ── restore the mode we fell asleep in (EXT1 = button wake) ──
    bool restored=false;
    if(wk!=ESP_SLEEP_WAKEUP_UNDEFINED){   // any wake from deep sleep (fresh boot = UNDEFINED)
        switch(wakeMode){
            case 1:  // photo viewer, same photo
                if(totalImages>0){
                    currentImageIndex=min((int)wakePicIdx,totalImages-1);
                    appMode=MODE_PICTURES;drawPictureScreen();restored=true;
                }
                break;
            case 2:  // tsumego resumes its NVS-saved problem
                if(tsumego.begin(display)){appMode=MODE_TSUMEGO;restored=true;}
                break;
            case 3:  // gazetteer, same country if possible
                if(switchRef(1)){
                    appMode=MODE_VIEW;
                    DictEntry e;e.found=false;
                    if(wakeGazWord[0])e=binarySearch(String(wakeGazWord));
                    if(!e.found)e=getRandomEntry();
                    if(e.found){showEntry(e);drawViewScreen();restored=true;}
                }
                break;
            case 4:{ // sky chart at the current RTC time
                int dh,dm,dd,dmo,dy;
                readCurrentTime(dh,dm);readCurrentDate(dd,dmo,dy);
                sky.begin(display,dy,dmo,dd,dh,dm);
                appMode=MODE_SKY;restored=true;
                break;
            }
            case 5:  // clock face at the current time
                enterClockMode();restored=true;
                break;
        }
    }

    if(!restored){
        if(!ensureIndex()){drawMessage("Bad dict format!");while(1)delay(1000);}
        String savedWord=loadStateWord();
        if(savedWord.length()>0){
            DictEntry e=binarySearch(savedWord);
            if(e.found){showEntry(e);drawViewScreen();}
            else showWordOfTheDay();
        }else showWordOfTheDay();
    }
    lastActivity=millis();
}

void loop(){
    handleButtons();
#if USE_LIGHT_SLEEP
    // nap until a button (GPIO wake, <1ms) or the 250ms tick; idle
    // current drops from ~40mA to low single digits
    esp_sleep_enable_timer_wakeup(250000ULL);
    esp_light_sleep_start();
#else
    delay(10);
#endif
    if(millis()-lastActivity>sleepTimeoutMs()){
        if(digitalRead(BTN_TR)==LOW&&digitalRead(BTN_TL)==LOW&&digitalRead(BTN_BR)==LOW&&digitalRead(BTN_BL)==LOW)
            enterDeepSleep();
    }
}
