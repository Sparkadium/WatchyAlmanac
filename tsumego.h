/*
 * tsumego.h — Go tsumego trainer module for WatchyDict
 *
 * Engine transplanted from WatchyTsumego.ino (standalone). Reads
 * /problems.bin (TSU2/TSU1, produced by tsumego_pack.py) from the
 * host's already-mounted SPIFFS.
 *
 * Interface contract with WatchyDict.ino:
 *   begin(display) — first call: open /problems.bin, parse header +
 *                    set table, restore NVS progress, resume at the
 *                    saved problem, full-refresh render. Later calls
 *                    (re-entering from the menu): just re-render —
 *                    mid-puzzle state survives within a power session.
 *                    Returns false if problems.bin missing/invalid.
 *   poll(display)  — non-blocking loop tick. Own raw button polling
 *                    (edge/hold/repeat). Returns 0 idle, 1 activity,
 *                    2 exit-to-dictionary request.
 *
 * Buttons while playing:
 *   TR = cursor next (hold to repeat)   BR = cursor down (hold)
 *   TL tap = place stone, TL hold = pass
 *   BL tap = undo turn,   BL hold = menu
 * Menu adds: go to problem (scrubber), sets, next unsolved, random,
 *   show solution, exit to dictionary.
 *
 * Changes vs standalone (intentional):
 *   - No deep sleep / RTC_DATA_ATTR resume: the host owns sleep and
 *     always wakes into dictionary view. Progress (solved bitmap +
 *     current problem) persists in NVS namespace "tsumego"; after a
 *     sleep or reboot the current problem restarts from its opening
 *     position.
 *   - No LittleFS fallback or partition diagnostic screen: the host
 *     mounts SPIFFS and reports load failure itself.
 *   - Pins hardcoded (same Watchy V2 hardware as host): TL=25 TR=35
 *     BL=26 (extra debounce, ADC2) BR=4.
 */

#ifndef TSUMEGO_H
#define TSUMEGO_H

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include "esp_system.h"

typedef GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> TsuDisplay;

class TsumegoTrainer {
public:
    // ------------------------------------------------------------ interface

    bool begin(TsuDisplay &display) {
        dsp = &display;
        if (!initialized) {
            binFile = SPIFFS.open("/problems.bin", "r");
            if (!binFile || binFile.size() < 6) return false;

            uint8_t hdr[7];  binFile.seek(0);  binFile.read(hdr, 7);
            bool v2 = memcmp(hdr, "TSU2", 4) == 0;
            if (!v2 && memcmp(hdr, "TSU1", 4) != 0) { binFile.close(); return false; }
            problemCount = hdr[4] | (hdr[5] << 8);
            if (problemCount == 0) { binFile.close(); return false; }
            offsetsBase = 6;  nSets = 0;
            if (v2) {
                uint8_t total = hdr[6];
                uint32_t p = 7;
                for (uint8_t i = 0; i < total; i++) {
                    binFile.seek(p);
                    uint8_t ln;  binFile.read(&ln, 1);
                    if (nSets < 16) {
                        uint8_t rd = ln > 24 ? 24 : ln;
                        binFile.read((uint8_t*)setNames[nSets], rd);
                        setNames[nSets][rd] = 0;
                        binFile.seek(p + 1 + ln);
                        uint8_t se[2];  binFile.read(se, 2);
                        setStarts[nSets] = se[0] | (se[1] << 8);
                        nSets++;
                    }
                    p += 1 + (uint32_t)ln + 2;
                }
                offsetsBase = p;
            }

            prefs.begin("tsumego");
            solvedCount = prefs.getUShort("solved", 0);
            memset(solvedMap, 0, sizeof(solvedMap));
            prefs.getBytes("map", solvedMap, sizeof(solvedMap));

            uint16_t cur = prefs.getUShort("cur", 0);
            if (cur >= problemCount) cur = 0;
            if (!loadGo(cur)) { binFile.close(); prefs.end(); return false; }
            initialized = true;
        }
        // re-entry from the host menu: full refresh clears the old screen
        render(true);
        return true;
    }

    int poll(TsuDisplay &display) {
        dsp = &display;
        pollBtn(bTL); pollBtn(bTR); pollBtn(bBL); pollBtn(bBR);
        bool acted = false;

        // long-press BL on any game screen opens the menu
        if (mode < MENU && bBL.state && bBL.heldMs > 700) {
            consumeRelease(bBL);
            openMenu();
            render(false);
            return 1;
        }

        switch (mode) {
        case PLAYING:
            if (bTR.pressed || bTR.repeat) { cursorNext(); acted = true; }
            if (bBR.pressed || bBR.repeat) { cursorDown(); acted = true; }
            if (bTL.released) {              // short: place stone, long: pass
                userMove(bTL.heldMs > 600 ? PASS_POS : cursor);
                acted = true;
            }
            if (bBL.released) { undoTurn(); acted = true; }
            break;

        case SOLVED:
            if (bTL.pressed) { consumeRelease(bTL);   // don't let the release
                               nextProblem();          // leak into the new problem
                               render(true);
                               return 1; }
            break;

        case OFFTREE:
        case FAILED:
            if (bTL.pressed) { consumeRelease(bTL); undoTurn(); acted = true; }
            else if (bBL.released) { undoTurn(); acted = true; }
            break;

        case REVEALED:
            if (bTL.pressed) { consumeRelease(bTL); jumpTo(probIdx);
                               render(true); return 1; }
            if (bBL.released) { jumpTo(probIdx);
                                render(true); return 1; }
            break;

        case MENU:
            if (bTR.pressed) { menuSel = (menuSel + nMenuItems - 1) % nMenuItems;
                               acted = true; }               // top button = up
            if (bBR.pressed) { menuSel = (menuSel + 1) % nMenuItems; acted = true; }
            if (bBL.released) { mode = resumeMode; acted = true; }
            if (bTL.pressed) {
                consumeRelease(bTL);
                switch (menuItems[menuSel]) {
                case 0: mode = resumeMode; acted = true; break;
                case 1: scrubVal = probIdx; mode = SCRUB; acted = true; break;
                case 2: setSel = setOf(probIdx); mode = SETMENU; acted = true; break;
                case 3: jumpNextUnsolved(); render(true); return 1;
                case 4: jumpRandom(); render(true); return 1;
                case 5: revealSolution(); return 1;
                case 6: mode = resumeMode;           // exit to dictionary;
                        return 2;                    // state resumes next entry
                }
            }
            break;

        case SETMENU:
            if (bTR.pressed) { setSel = (setSel + nSets - 1) % nSets; acted = true; }
            if (bBR.pressed) { setSel = (setSel + 1) % nSets; acted = true; }
            if (bBL.released) { mode = MENU; acted = true; }
            if (bTL.pressed) { consumeRelease(bTL); jumpToSet(setSel);
                               render(true); return 1; }
            break;

        case SCRUB: {
            int32_t step = 0;
            if (bTR.pressed) step = 1;
            if (bTR.repeat)  step = scrubStep(bTR.nRepeats);
            if (bBR.pressed) step = -1;
            if (bBR.repeat)  step = -scrubStep(bBR.nRepeats);
            if (step) {
                int32_t v = (int32_t)scrubVal + step;
                while (v < 0) v += problemCount;
                scrubVal = (uint16_t)(v % problemCount);
                acted = true;
            }
            if (bBL.released) { mode = MENU; acted = true; }
            if (bTL.pressed) { consumeRelease(bTL); jumpTo(scrubVal);
                               render(true); return 1; }
            break;
        }
        }

        if (acted) { render(false); return 1; }
        return 0;
    }

private:
    // ------------------------------------------------------------ constants

    static const uint8_t  PASS_POS = 0xFD;
    static const uint8_t  END_TREE = 0xFE;
    static const uint8_t  POP      = 0xFF;
    static const uint16_t MAX_BLOB = 2048;
    static const uint8_t  MAX_HIST = 48;
    static const uint16_t FULL_REFRESH_EVERY = 20;

    // flags byte
    static const uint8_t F_WHITE_TO_PLAY = 0x01;
    static const uint8_t F_WALL_L = 0x02;
    static const uint8_t F_WALL_T = 0x04;
    static const uint8_t F_WALL_R = 0x08;
    static const uint8_t F_WALL_B = 0x10;

    enum Mode : uint8_t { PLAYING, OFFTREE, FAILED, SOLVED, REVEALED,
                          MENU, SETMENU, SCRUB };

    struct Btn {
        uint8_t pin;  uint16_t debounce;
        bool state = false;  uint32_t tChange = 0, tRepeat = 0, tPress = 0;
        bool pressed = false, released = false, repeat = false;
        uint32_t heldMs = 0;   // live while held; frozen at release
        uint16_t nRepeats = 0; // hold-repeat count, for scrub acceleration
    };
    // Watchy V2 pins; BL (GPIO 26) is ADC2-adjacent: longer debounce
    Btn bTL{25, 30}, bTR{35, 30}, bBL{26, 80}, bBR{4, 30};

    // ------------------------------------------------------------ state

    TsuDisplay *dsp = nullptr;
    bool initialized = false;

    Preferences prefs;
    File binFile;
    uint16_t problemCount = 0;
    uint32_t offsetsBase = 6;                // TSU1: 6; TSU2: after set table
    uint8_t  nSets = 0;
    char     setNames[16][25];
    uint16_t setStarts[16];
    uint8_t  solvedMap[1664];                // in-RAM copy of the NVS bitmap

    // menu state
    uint8_t  menuSel = 0, setSel = 0, nMenuItems = 0;
    uint8_t  menuItems[7];
    uint16_t scrubVal = 0;
    Mode     resumeMode = PLAYING;

    uint8_t  blob[MAX_BLOB];
    uint16_t blobLen = 0;
    uint8_t  flags = 0, W = 0, H = 0;
    uint16_t treeStart = 0;                  // offset of root child list in blob

    uint8_t  board[252];                     // 0 empty, 1 black, 2 white
    uint8_t  toPlay = 1;                     // color of the human (1 or 2)

    uint16_t probIdx = 0;
    uint8_t  hist[MAX_HIST];                 // played positions, user+engine
    uint8_t  histLen = 0;
    uint16_t curList = 0;                    // blob offset of current child list
    uint8_t  cursor = 0;
    Mode     mode = PLAYING;
    int16_t  koPos = -1;
    int16_t  lastMove = -1;
    uint8_t  offtreePos = 0;

    uint16_t partialCount = 0;
    uint16_t solvedCount = 0;

    // flood-fill scratch
    uint8_t grp[252];  uint8_t grpN = 0;
    bool seen[252];
    uint8_t solPath[32];

    // ------------------------------------------------------------ problems

    bool loadGo(uint16_t idx) {
        if (idx >= problemCount) return false;
        binFile.seek(offsetsBase + 4UL * idx);
        uint32_t off;  binFile.read((uint8_t*)&off, 4);
        uint32_t next = 0;
        if (idx + 1 < problemCount) binFile.read((uint8_t*)&next, 4);
        else next = binFile.size();
        blobLen = min((uint32_t)MAX_BLOB, next - off);
        binFile.seek(off);
        binFile.read(blob, blobLen);

        uint16_t p = 0;
        flags = blob[p++];  W = blob[p++];  H = blob[p++];
        memset(board, 0, sizeof(board));
        uint8_t nb = blob[p++];
        for (uint8_t i = 0; i < nb; i++) board[blob[p++]] = 1;
        uint8_t nw = blob[p++];
        for (uint8_t i = 0; i < nw; i++) board[blob[p++]] = 2;
        treeStart = p;

        toPlay = (flags & F_WHITE_TO_PLAY) ? 2 : 1;
        curList = treeStart;
        histLen = 0;  mode = PLAYING;  koPos = -1;  lastMove = -1;
        probIdx = idx;

        // start cursor near the middle of the region, on an empty point
        cursor = (H / 2) * W + W / 2;
        if (board[cursor]) cursorNext();
        return true;
    }

    // ------------------------------------------------------------ go rules

    bool openSide(int c, int r) {            // off-region neighbor = liberty?
        if (c < 0)  return !(flags & F_WALL_L);
        if (r < 0)  return !(flags & F_WALL_T);
        if (c >= W) return !(flags & F_WALL_R);
        if (r >= H) return !(flags & F_WALL_B);
        return false;
    }

    // flood group at pos; returns liberty count, fills grp[] (grpN)
    int floodLibs(uint8_t pos) {
        uint8_t color = board[pos];
        memset(seen, 0, sizeof(seen));
        grpN = 0;
        int libs = 0;
        uint8_t stack[252];  int sp = 0;
        stack[sp++] = pos;  seen[pos] = true;
        while (sp) {
            uint8_t q = stack[--sp];
            grp[grpN++] = q;
            int c = q % W, r = q / W;
            const int dc[] = {1, -1, 0, 0}, dr[] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int nc = c + dc[k], nr = r + dr[k];
                if (nc < 0 || nr < 0 || nc >= W || nr >= H) {
                    if (openSide(nc, nr)) libs += 100;   // open board = ample liberties
                    continue;
                }
                uint8_t np = nr * W + nc;
                if (board[np] == 0) libs++;
                else if (board[np] == color && !seen[np]) {
                    seen[np] = true;  stack[sp++] = np;
                }
            }
        }
        return libs;
    }

    // try to play `color` at pos. Returns false if illegal. Updates ko.
    // fromTree: tree moves are authoritative -- some tsumego lines encode
    // ko sequences without threats, so the ko ban applies to off-tree only.
    bool applyMove(uint8_t pos, uint8_t color, bool fromTree) {
        if (pos == PASS_POS) { koPos = -1; lastMove = -1; return true; }
        if (board[pos] != 0) return false;
        if (!fromTree && (int16_t)pos == koPos) return false;
        board[pos] = color;
        uint8_t enemy = 3 - color;
        int captured = 0;  uint8_t capPos = 0;
        int c = pos % W, r = pos / W;
        const int dc[] = {1, -1, 0, 0}, dr[] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int nc = c + dc[k], nr = r + dr[k];
            if (nc < 0 || nr < 0 || nc >= W || nr >= H) continue;
            uint8_t np = nr * W + nc;
            if (board[np] == enemy && floodLibs(np) == 0) {
                for (uint8_t i = 0; i < grpN; i++) board[grp[i]] = 0;
                captured += grpN;  capPos = grp[0];
            }
        }
        if (floodLibs(pos) == 0) {             // suicide
            board[pos] = 0;
            return false;
        }
        koPos = (captured == 1 && grpN == 1 && floodLibs(pos) == 1)
                ? capPos : -1;
        lastMove = pos;
        return true;
    }

    // ------------------------------------------------------------ tree walk

    uint16_t skipSubtree(uint16_t nodeOff) { // past node's POP
        uint16_t p = nodeOff + 2;
        while (blob[p] != POP) p = skipSubtree(p);
        return p + 1;
    }

    // find node with pos in child list; 0 = not found
    uint16_t findChild(uint16_t list, uint8_t pos) {
        uint16_t p = list;
        while (blob[p] != POP && blob[p] != END_TREE) {
            if (blob[p] == pos) return p;
            p = skipSubtree(p);
        }
        return 0;
    }

    bool terminal(uint16_t nodeOff) { return blob[nodeOff + 2] == POP; }

    // ------------------------------------------------------------ game flow

    void pushHist(uint8_t pos) { if (histLen < MAX_HIST) hist[histLen++] = pos; }

    void replay() {                          // rebuild board+tree from hist
        memset(board, 0, sizeof(board));
        uint16_t q = 3;
        uint8_t nb = blob[q++];
        for (uint8_t i = 0; i < nb; i++) board[blob[q++]] = 1;
        uint8_t nw = blob[q++];
        for (uint8_t i = 0; i < nw; i++) board[blob[q++]] = 2;
        curList = treeStart;  koPos = -1;  lastMove = -1;
        uint8_t color = toPlay;
        for (uint8_t i = 0; i < histLen; i++) {
            applyMove(hist[i], color, true);
            uint16_t n = findChild(curList, hist[i]);
            if (n) curList = n + 2;
            color = 3 - color;
        }
        mode = PLAYING;
    }

    void userMove(uint8_t pos) {
        uint16_t node = findChild(curList, pos);
        if (!node) {                           // off-tree: show it, mark wrong
            if (!applyMove(pos, toPlay, false)) return; // illegal: ignore press
            offtreePos = pos;
            mode = OFFTREE;
            return;
        }
        if (!applyMove(pos, toPlay, true)) return;
        pushHist(pos);
        curList = node + 2;
        if (blob[node + 1] == 1) { solve(); return; }
        if (terminal(node))      { mode = FAILED; return; }

        // engine reply: mainline child
        uint16_t reply = node + 2;
        applyMove(blob[reply], 3 - toPlay, true);
        pushHist(blob[reply]);
        curList = reply + 2;
        if (blob[reply + 1] == 1) { solve(); return; }
        if (terminal(reply))      { mode = FAILED; return; }
    }

    void undoTurn() {
        if (mode == OFFTREE) { replay(); return; }   // off-tree wasn't in hist
        if (histLen == 0) return;
        if (histLen & 1) histLen -= 1;               // lone user move
        else             histLen -= 2;               // user move + engine reply
        replay();
    }

    bool isSolved(uint16_t i) { return solvedMap[i >> 3] & (1 << (i & 7)); }

    uint16_t countSolved(uint16_t a, uint16_t b) {
        uint16_t n = 0;
        for (uint16_t i = a; i < b; i++) if (isSolved(i)) n++;
        return n;
    }

    uint8_t setOf(uint16_t idx) {
        uint8_t s = 0;
        for (uint8_t i = 0; i < nSets; i++) if (setStarts[i] <= idx) s = i;
        return s;
    }

    uint16_t setEnd(uint8_t s) {
        return (s + 1 < nSets) ? setStarts[s + 1] : problemCount;
    }

    void solve() {
        mode = SOLVED;
        if (!isSolved(probIdx)) {
            solvedMap[probIdx >> 3] |= 1 << (probIdx & 7);
            prefs.putBytes("map", solvedMap, (problemCount + 7) / 8);
            solvedCount++;
            prefs.putUShort("solved", solvedCount);
        }
    }

    // ------------------------------------------------------- show solution

    int8_t findRight(uint16_t list, uint8_t depth) {
        if (depth >= 32) return -1;
        uint16_t p = list;
        while (blob[p] != POP && blob[p] != END_TREE) {
            solPath[depth] = blob[p];
            if (blob[p + 1] == 1) return depth + 1;
            int8_t r = findRight(p + 2, depth + 1);
            if (r > 0) return r;
            p = skipSubtree(p);
        }
        return -1;
    }

    void revealSolution() {
        loadGo(probIdx);                       // fresh board
        int8_t n = findRight(treeStart, 0);
        if (n <= 0) { render(true); return; }
        mode = REVEALED;                       // hides the cursor during playback
        render(true);
        uint8_t color = toPlay;
        for (int8_t i = 0; i < n; i++) {
            delay(800);
            applyMove(solPath[i], color, true);
            color = 3 - color;
            render(false);
        }
    }

    // ------------------------------------------------------------ menu jumps

    void jumpTo(uint16_t idx) {
        loadGo(idx);
        prefs.putUShort("cur", probIdx);
    }

    void jumpToSet(uint8_t s) {
        uint16_t a = setStarts[s], b = setEnd(s), t = a;
        for (uint16_t i = a; i < b; i++) if (!isSolved(i)) { t = i; break; }
        jumpTo(t);
    }

    void jumpNextUnsolved() {
        for (uint16_t k = 1; k <= problemCount; k++) {
            uint16_t i = (probIdx + k) % problemCount;
            if (!isSolved(i)) { jumpTo(i); return; }
        }
        jumpTo((probIdx + 1) % problemCount);  // everything solved: just advance
    }

    void jumpRandom() {
        for (uint8_t t = 0; t < 64; t++) {
            uint16_t i = esp_random() % problemCount;
            if (!isSolved(i)) { jumpTo(i); return; }
        }
        jumpTo(esp_random() % problemCount);
    }

    void openMenu() {
        resumeMode = mode;
        nMenuItems = 0;
        menuItems[nMenuItems++] = 0;           // resume
        menuItems[nMenuItems++] = 1;           // go to problem
        if (nSets > 1) menuItems[nMenuItems++] = 2;  // sets
        menuItems[nMenuItems++] = 3;           // next unsolved
        menuItems[nMenuItems++] = 4;           // random
        menuItems[nMenuItems++] = 5;           // show solution
        menuItems[nMenuItems++] = 6;           // exit to dictionary
        menuSel = 0;
        mode = MENU;
    }

    int32_t scrubStep(uint16_t n) {
        if (n < 6)  return 1;
        if (n < 15) return 10;
        if (n < 30) return 100;
        return 1000;
    }

    void nextProblem() { jumpTo((probIdx + 1) % problemCount); }

    // ------------------------------------------------------------ cursor

    void cursorNext() {
        for (uint16_t i = 0; i < (uint16_t)W * H; i++) {
            cursor = (cursor + 1) % (W * H);
            if (board[cursor] == 0 && (int16_t)cursor != koPos) return;
        }
    }
    void cursorDown() {
        for (uint16_t i = 0; i < (uint16_t)W * H; i++) {
            cursor = (cursor + W) % (W * H);
            if (cursor < W) cursor = (cursor + 1) % W;  // wrapped: shift column
            if (board[cursor] == 0 && (int16_t)cursor != koPos) return;
        }
    }

    // ------------------------------------------------------------ render

    void drawBoard() {
        int pitch = min(24, min(196 / W, 164 / H));
        int x0 = (200 - (W - 1) * pitch) / 2;
        int y0 = 20 + (164 - (H - 1) * pitch) / 2;
        int ext = pitch / 2;                   // continuation stub on open sides

        for (uint8_t c = 0; c < W; c++) {
            int x = x0 + c * pitch;
            int yA = (flags & F_WALL_T) ? y0 : y0 - ext;
            int yB = (flags & F_WALL_B) ? y0 + (H - 1) * pitch
                                        : y0 + (H - 1) * pitch + ext;
            dsp->drawLine(x, yA, x, yB, GxEPD_BLACK);
        }
        for (uint8_t r = 0; r < H; r++) {
            int y = y0 + r * pitch;
            int xA = (flags & F_WALL_L) ? x0 : x0 - ext;
            int xB = (flags & F_WALL_R) ? x0 + (W - 1) * pitch
                                        : x0 + (W - 1) * pitch + ext;
            dsp->drawLine(xA, y, xB, y, GxEPD_BLACK);
        }
        // thick board edges on wall sides
        if (flags & F_WALL_L) dsp->drawLine(x0-1, y0-1, x0-1, y0+(H-1)*pitch+1, GxEPD_BLACK);
        if (flags & F_WALL_T) dsp->drawLine(x0-1, y0-1, x0+(W-1)*pitch+1, y0-1, GxEPD_BLACK);
        if (flags & F_WALL_R) dsp->drawLine(x0+(W-1)*pitch+1, y0-1, x0+(W-1)*pitch+1, y0+(H-1)*pitch+1, GxEPD_BLACK);
        if (flags & F_WALL_B) dsp->drawLine(x0-1, y0+(H-1)*pitch+1, x0+(W-1)*pitch+1, y0+(H-1)*pitch+1, GxEPD_BLACK);

        int rad = pitch / 2 - 1;  if (rad < 3) rad = 3;
        for (uint8_t r = 0; r < H; r++)
            for (uint8_t c = 0; c < W; c++) {
                uint8_t v = board[r * W + c];
                if (!v) continue;
                int x = x0 + c * pitch, y = y0 + r * pitch;
                if (v == 1) dsp->fillCircle(x, y, rad, GxEPD_BLACK);
                else {
                    dsp->fillCircle(x, y, rad, GxEPD_WHITE);
                    dsp->drawCircle(x, y, rad, GxEPD_BLACK);
                }
            }
        if (lastMove >= 0 && board[lastMove]) {
            int x = x0 + (lastMove % W) * pitch, y = y0 + (lastMove / W) * pitch;
            uint16_t inv = board[lastMove] == 1 ? GxEPD_WHITE : GxEPD_BLACK;
            dsp->drawRect(x - 2, y - 2, 5, 5, inv);
        }
        if (mode == PLAYING) {                 // cursor: 4 corner ticks
            int x = x0 + (cursor % W) * pitch, y = y0 + (cursor / W) * pitch;
            int t = rad;
            dsp->drawLine(x-t, y-t, x-t+3, y-t, GxEPD_BLACK);
            dsp->drawLine(x-t, y-t, x-t, y-t+3, GxEPD_BLACK);
            dsp->drawLine(x+t, y-t, x+t-3, y-t, GxEPD_BLACK);
            dsp->drawLine(x+t, y-t, x+t, y-t+3, GxEPD_BLACK);
            dsp->drawLine(x-t, y+t, x-t+3, y+t, GxEPD_BLACK);
            dsp->drawLine(x-t, y+t, x-t, y+t-3, GxEPD_BLACK);
            dsp->drawLine(x+t, y+t, x+t-3, y+t, GxEPD_BLACK);
            dsp->drawLine(x+t, y+t, x+t, y+t-3, GxEPD_BLACK);
        }
    }

    void drawMenu() {
        static const char *labels[7] = { "resume", "go to problem...",
            "sets...", "next unsolved", "random problem", "show solution",
            "exit to dictionary" };
        dsp->setCursor(4, 4);
        dsp->printf("menu       %u/%u solved", solvedCount, problemCount);
        for (uint8_t i = 0; i < nMenuItems; i++) {
            dsp->setCursor(14, 30 + i * 20);
            dsp->print(i == menuSel ? "> " : "  ");
            dsp->print(labels[menuItems[i]]);
        }
        dsp->setCursor(4, 190);
        dsp->print("TR/BR:move TL:ok BL:back");
    }

    void drawSetMenu() {
        dsp->setCursor(4, 4);
        dsp->print("jump to set");
        for (uint8_t i = 0; i < nSets; i++) {
            dsp->setCursor(4, 24 + i * 16);
            dsp->print(i == setSel ? "> " : "  ");
            dsp->print(setNames[i]);
        }
        dsp->setCursor(4, 190);
        dsp->printf("%u/%u solved here",
                     countSolved(setStarts[setSel], setEnd(setSel)),
                     setEnd(setSel) - setStarts[setSel]);
    }

    void drawScrub() {
        dsp->setCursor(4, 4);
        dsp->print("go to problem");
        dsp->setTextSize(3);
        dsp->setCursor(50, 70);
        dsp->printf("%u", scrubVal + 1);
        dsp->setTextSize(1);
        dsp->setCursor(4, 120);
        if (nSets) dsp->print(setNames[setOf(scrubVal)]);
        if (isSolved(scrubVal)) dsp->print("  [solved]");
        dsp->setCursor(4, 190);
        dsp->print("TR:+ BR:- hold:fast TL:go");
    }

    void drawFrame() {
        dsp->fillScreen(GxEPD_WHITE);
        dsp->setTextColor(GxEPD_BLACK);
        dsp->setFont(NULL);
        dsp->setTextSize(1);
        if (mode == MENU)    { drawMenu();    return; }
        if (mode == SETMENU) { drawSetMenu(); return; }
        if (mode == SCRUB)   { drawScrub();   return; }
        dsp->setCursor(4, 4);
        dsp->printf("P%03u%s  %s to play", probIdx + 1,
                     isSolved(probIdx) ? "*" : "",
                     toPlay == 1 ? "black" : "white");
        drawBoard();
        dsp->setCursor(4, 190);
        switch (mode) {
        case PLAYING: dsp->printf("moves %u   %u solved",
                                   histLen, solvedCount); break;
        case OFFTREE: dsp->print(lastMove < 0 && histLen == 0 ?
                        "X pass is not it  BL:undo" :
                        "X not the move  BL:undo"); break;
        case FAILED:  dsp->print("X refuted  BL:undo"); break;
        case SOLVED:  dsp->print("* SOLVED *  TL:next"); break;
        case REVEALED: dsp->print("solution  TL:retry"); break;
        default: break;
        }
    }

    void render(bool full) {
        if (!full && ++partialCount >= FULL_REFRESH_EVERY) {
            full = true;  partialCount = 0;
        }
        if (full) dsp->setFullWindow();
        else      dsp->setPartialWindow(0, 0, 200, 200);
        dsp->firstPage();
        do { drawFrame(); } while (dsp->nextPage());
    }

    // ------------------------------------------------------------ buttons

    void pollBtn(Btn &b) {
        b.pressed = b.released = b.repeat = false;
        bool raw = digitalRead(b.pin) == HIGH;
        uint32_t now = millis();
        if (raw != b.state && now - b.tChange > b.debounce) {
            b.state = raw;  b.tChange = now;
            if (raw) { b.pressed = true;  b.tRepeat = now + 400;
                       b.tPress = now;   b.heldMs = 0;  b.nRepeats = 0; }
            else     { b.released = true; b.heldMs = now - b.tPress; }
        }
        if (b.state) { b.heldMs = now - b.tPress;
                       if (now > b.tRepeat) { b.repeat = true; b.nRepeats++;
                                              b.tRepeat = now + 120; } }
    }

    void consumeRelease(Btn &b) {          // swallow the release of a hold
        while (digitalRead(b.pin) == HIGH) delay(10);
        b.state = false;  b.pressed = b.released = b.repeat = false;
        b.tChange = millis();
    }
};

#endif
