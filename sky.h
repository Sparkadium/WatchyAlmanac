/*
 * sky.h — star chart / moon / sun module for WatchyDict.
 *
 * Renders the sky over a configurable location (default Ottawa) on the 200x200
 * 1-bit e-ink panel: azimuthal all-sky chart (zenith centre, horizon
 * rim, N up, E left — correct for looking UP), 904 stars to V=4.5,
 * constellation stick figures, moon phase, sunrise/sunset.
 *
 * Astronomy math lives in sky_math.h (pure C, verified against astropy
 * — see VERIFICATION.md). Star/line data lives in stars.h (generated
 * from the Yale Bright Star Catalogue + d3-celestial by make_stars.py;
 * epoch FK5 J2033). No hand-entered coordinates anywhere.
 *
 * Interface (same contract as tsumego.h):
 *   begin(display, y, mo, d, hh, mm)  — local civil time from the RTC.
 *   poll(display) -> 0 idle, 1 activity, 2 exit.
 * Buttons: TL exit, BR +30 min, BL -30 min, TR back to now.
 */

#ifndef SKY_H
#define SKY_H

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include "sky_math.h"
#include "stars.h"

typedef GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> SkyDisplay;

/* Location + timezone: defaults are Ottawa; overridden at boot from
   /location.txt in SPIFFS (see loadLocation() in the sketch). */
double skyLat = 45.3475, skyLon = -75.7566;
int skyBaseUtc = -5;          /* standard-time UTC offset, hours */
int skyDstRule = 1;           /* 0 none, 1 North America, 2 EU */
char skyTzStd[6] = "est", skyTzDst[6] = "edt";

#define SKY_CX 100
#define SKY_CY 96
#define SKY_R  74

class SkyView {
public:
    bool begin(SkyDisplay &display, int y, int mo, int d, int hh, int mm) {
        dsp = &display;
        bY = y; bMo = mo; bD = d; bH = hh; bM = mm;
        offsetMin = 0;
        partials = 0;
        render(true);
        return true;
    }

    int poll(SkyDisplay &display) {
        dsp = &display;
        if (pressed(25, 80)) return 2;                       /* TL: exit */
        if (pressed(4, 80))  { offsetMin += 30; render(false); return 1; }
        if (pressed(26, 140)) { offsetMin -= 30; render(false); return 1; }
        if (pressed(35, 80)) { offsetMin = 0; render(false); return 1; }
        return 0;
    }

private:
    SkyDisplay *dsp = nullptr;
    int bY = 2026, bMo = 1, bD = 1, bH = 0, bM = 0;
    int32_t offsetMin = 0;
    uint16_t partials = 0;

    /* simple blocking press check (discrete 30-min steps; no repeat) */
    bool pressed(int pin, int debounceMs) {
        if (digitalRead(pin) != HIGH) return false;
        delay(debounceMs);
        if (digitalRead(pin) != HIGH) return false;
        unsigned long to = millis() + 3000;
        while (digitalRead(pin) == HIGH && millis() < to) delay(10);
        delay(30);
        return true;
    }

    /* ---- civil date arithmetic (local), minutes offset ---- */
    static int daysInMonth(int y, int m) {
        static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
        return dm[m - 1];
    }
    void viewTime(int &y, int &mo, int &d, int &hh, int &mm) {
        y = bY; mo = bMo; d = bD;
        int32_t t = (int32_t)bH * 60 + bM + offsetMin;
        while (t < 0) {
            t += 1440;
            if (--d < 1) { if (--mo < 1) { mo = 12; y--; } d = daysInMonth(y, mo); }
        }
        while (t >= 1440) {
            t -= 1440;
            if (++d > daysInMonth(y, mo)) { d = 1; if (++mo > 12) { mo = 1; y++; } }
        }
        hh = t / 60; mm = t % 60;
    }

    /* project one equatorial point; returns false if below horizon */
    bool project(double raDeg, double decDeg, double jd, int &px, int &py) {
        double alt, az;
        sky_altaz(raDeg, decDeg, jd, skyLat, skyLon, &alt, &az);
        if (alt < 0) return false;
        double r = (90.0 - alt) / 90.0 * SKY_R;
        px = SKY_CX - (int)lround(r * sin(az * D2R));   /* E on the left */
        py = SKY_CY - (int)lround(r * cos(az * D2R));
        return true;
    }

    void render(bool full) {
        setCpuFrequencyMhz(240);   // double-precision trig is software FP
        if (!full && ++partials >= 12) { full = true; partials = 0; }
        int y, mo, d, hh, mm;
        viewTime(y, mo, d, hh, mm);
        int utcOff = sky_utcOffset(y, mo, d, hh, skyBaseUtc, skyDstRule);
        double jd = sky_julian(y, mo, d, hh + mm / 60.0 - utcOff);

        /* sun rise/set for the viewed local date */
        double riseU, setU;
        int sunOk = sky_sunRiseSet(y, mo, d, skyLat, skyLon, &riseU, &setU);
        int offNoon = sky_utcOffset(y, mo, d, 12, skyBaseUtc, skyDstRule);
        double riseL = sunOk ? fmod(riseU + offNoon + 48.0, 24.0) : 0;
        double setL  = sunOk ? fmod(setU  + offNoon + 48.0, 24.0) : 0;

        int wax; double illum = sky_moonIllum(jd, &wax);
        int phase = sky_moonPhaseIdx(jd);
        static const char *phaseName[8] = {
            "new moon", "wax crescent", "first quarter", "wax gibbous",
            "full moon", "wan gibbous", "last quarter", "wan crescent" };
        static const char *monName[12] = {
            "jan","feb","mar","apr","may","jun",
            "jul","aug","sep","oct","nov","dec" };

        if (full) dsp->setFullWindow();
        else      dsp->setPartialWindow(0, 0, 200, 200);
        dsp->firstPage();
        do {
            dsp->fillScreen(GxEPD_WHITE);
            dsp->setFont(NULL);
            dsp->setTextSize(1);
            dsp->setTextColor(GxEPD_BLACK);

            /* header: date, time, offset marker when previewing */
            char hdr[36];
            snprintf(hdr, sizeof(hdr), "%s %d  %02d:%02d %s%s",
                     monName[mo - 1], d, hh, mm,
                     utcOff != skyBaseUtc ? skyTzDst : skyTzStd,
                     offsetMin ? "*" : "");
            dsp->setCursor((200 - (int)strlen(hdr) * 6) / 2, 4);
            dsp->print(hdr);

            /* sky disk */
            dsp->fillCircle(SKY_CX, SKY_CY, SKY_R, GxEPD_BLACK);

            /* dotted 45-degree altitude ring */
            for (int a = 0; a < 360; a += 8)
                dsp->drawPixel(SKY_CX + (int)lround(SKY_R * 0.5 * cos(a * D2R)),
                               SKY_CY + (int)lround(SKY_R * 0.5 * sin(a * D2R)),
                               GxEPD_WHITE);

            /* constellation lines (both ends above horizon) */
            for (int i = 0; i < N_LINES; i++) {
                SkyLine L;
                memcpy_P(&L, &SKY_LINES[i], sizeof(SkyLine));
                int x1, y1, x2, y2;
                if (!project(L.ra1 * 360.0 / 65535.0, L.dec1 * 90.0 / 32000.0,
                             jd, x1, y1)) continue;
                if (!project(L.ra2 * 360.0 / 65535.0, L.dec2 * 90.0 / 32000.0,
                             jd, x2, y2)) continue;
                dsp->drawLine(x1, y1, x2, y2, GxEPD_WHITE);
            }

            /* stars, by magnitude class */
            for (int i = 0; i < N_STARS; i++) {
                SkyStar s;
                memcpy_P(&s, &SKY_STARS[i], sizeof(SkyStar));
                int px, py;
                if (!project(s.ra * 360.0 / 65535.0, s.dec * 90.0 / 32000.0,
                             jd, px, py)) continue;
                switch (s.cls) {
                    case 0: dsp->fillCircle(px, py, 2, GxEPD_WHITE); break;
                    case 1: dsp->fillCircle(px, py, 1, GxEPD_WHITE); break;
                    case 2: dsp->drawPixel(px, py, GxEPD_WHITE);
                            dsp->drawPixel(px + 1, py, GxEPD_WHITE); break;
                    default: dsp->drawPixel(px, py, GxEPD_WHITE); break;
                }
            }

            /* cardinal letters just inside the rim */
            dsp->setTextColor(GxEPD_WHITE);
            dsp->setCursor(SKY_CX - 2, SKY_CY - SKY_R + 3);  dsp->print("N");
            dsp->setCursor(SKY_CX - 2, SKY_CY + SKY_R - 10); dsp->print("S");
            dsp->setCursor(SKY_CX - SKY_R + 3, SKY_CY - 3);  dsp->print("E");
            dsp->setCursor(SKY_CX + SKY_R - 8, SKY_CY - 3);  dsp->print("W");
            dsp->setTextColor(GxEPD_BLACK);

            /* bottom bar: moon + sun */
            dsp->drawLine(6, 176, 194, 176, GxEPD_BLACK);
            drawMoonGlyph(12, 187, 7, phase);
            char mtxt[28];
            snprintf(mtxt, sizeof(mtxt), "%s %d%%",
                     phaseName[phase], (int)lround(illum * 100));
            dsp->setCursor(24, 183);
            dsp->print(mtxt);
            char stxt[24];
            if (sunOk)
                snprintf(stxt, sizeof(stxt), "^%02d:%02d v%02d:%02d",
                         (int)riseL, (int)lround((riseL - (int)riseL) * 60) % 60,
                         (int)setL,  (int)lround((setL  - (int)setL)  * 60) % 60);
            else
                snprintf(stxt, sizeof(stxt), "sun: --");
            dsp->setCursor(198 - (int)strlen(stxt) * 6, 192);
            dsp->print(stxt);
        } while (dsp->nextPage());
        setCpuFrequencyMhz(80);
    }

    /* 8-phase moon glyph, 1-bit: black disk + white lit region.
       Waxing lights the right side (as seen from the N hemisphere). */
    void drawMoonGlyph(int cx, int cy, int r, int phase) {
        dsp->fillCircle(cx, cy, r, GxEPD_BLACK);
        switch (phase) {
            case 0: break;                                    /* new: all dark */
            case 4:                                           /* full: all lit */
                dsp->fillCircle(cx, cy, r - 1, GxEPD_WHITE);
                break;
            case 2:                                           /* first quarter */
                dsp->fillRect(cx, cy - r + 1, r, 2 * r - 1, GxEPD_WHITE);
                break;
            case 6:                                           /* last quarter */
                dsp->fillRect(cx - r + 1, cy - r + 1, r, 2 * r - 1, GxEPD_WHITE);
                break;
            case 1:                                           /* wax crescent */
                dsp->fillRect(cx, cy - r + 1, r, 2 * r - 1, GxEPD_WHITE);
                dsp->fillCircle(cx + 2, cy, r - 2, GxEPD_BLACK);
                break;
            case 3:                                           /* wax gibbous */
                dsp->fillRect(cx, cy - r + 1, r, 2 * r - 1, GxEPD_WHITE);
                dsp->fillCircle(cx - 2, cy, r - 2, GxEPD_WHITE);
                break;
            case 5:                                           /* wan gibbous */
                dsp->fillRect(cx - r + 1, cy - r + 1, r, 2 * r - 1, GxEPD_WHITE);
                dsp->fillCircle(cx + 2, cy, r - 2, GxEPD_WHITE);
                break;
            case 7:                                           /* wan crescent */
                dsp->fillRect(cx - r + 1, cy - r + 1, r, 2 * r - 1, GxEPD_WHITE);
                dsp->fillCircle(cx - 2, cy, r - 2, GxEPD_BLACK);
                break;
        }
        dsp->drawCircle(cx, cy, r, GxEPD_BLACK);
    }
};

#endif
