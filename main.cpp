// Maritime day/night lab (OpenGL 1.x + GLUT, macOS).
// Modular draws: drawSkybox, drawOcean, drawLighthouse, drawShip, drawBirds, drawCrew,
// plus drawDistantShips for horizon depth. Timing: glutDisplayFunc, glutIdleFunc,
// glutTimerFunc; double-buffered window (GLUT_DOUBLE).

#include <GLUT/glut.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// -----------------------------------------------------------------------------
// Global configuration constants
// -----------------------------------------------------------------------------
const int kWindowWidth = 1200;
const int kWindowHeight = 700;

const float kCycleIntervalSec = 15.0f;      // Auto toggle day/night every 15s (lab spec)
const float kTransitionDurationSec = 4.0f;  // Smooth blend duration for sky, lights, and environment
const float kWaveSpeed = 0.9f;
// Swell + chop must read from the orbit camera (~35 m); earlier amplitudes were too subtle vs. mesh extent.
const float kWaveAmplitude = 0.52f;
const float kWaveFrequency = 0.105f;
const float kBeamRotationSpeed = 28.0f;     // Degrees per second

// Fake earth curvature: quadratic drop vs world XZ distance (tuned for ~260m mesh — subtle, not a bowl).
const float kHorizonCurvature = 0.000045f;

const float kPi = 3.1415926535f;

// Lighthouse layout (Stockcake-style reference): white tapered masonry tower, black lantern &
// gallery, dark conical roof, red lamp core. Positions are local Y inside drawLighthouse()
// after the island translate; world position = kLhWorld* below.
const float kLhWorldX = -30.0f;
const float kLhWorldY0 = -4.0f;
const float kLhWorldZ = -2.0f;
// Tower foot sits on the crest of a single domed island (not loose scattered rocks).
const float kLhTowerBaseY = 1.42f;
const float kLhTowerSegH[3] = {5.0f, 5.0f, 4.8f};
const float kLhGalleryY = kLhTowerBaseY + kLhTowerSegH[0] + kLhTowerSegH[1] + kLhTowerSegH[2];
const float kLhDeckTopY = kLhGalleryY + 0.12f;
const float kLhLanternHalfH = 0.62f;
const float kLhLanternCenterY = kLhDeckTopY + kLhLanternHalfH;
const float kLhLanternWorldY = kLhWorldY0 + kLhLanternCenterY;
const float kLhRoofBaseY = kLhDeckTopY + kLhLanternHalfH * 2.0f + 0.08f;

// -----------------------------------------------------------------------------
// Runtime animation + state variables
// -----------------------------------------------------------------------------
float gSceneTimeSec = 0.0f;
float gLastFrameTimeSec = 0.0f;
float gWavePhase = 0.0f;
float gBeamAngleDeg = 0.0f;

// Blend factor: 0.0 means full day, 1.0 means full night.
float gDayNightBlend = 0.0f;
bool gTargetNight = false;

// Main ship horizontal drift (slow movement over the ocean surface).
float gShipDriftX = 0.0f;

// Automatic cinematic camera phase.
float gCameraPhase = 0.0f;

// -----------------------------------------------------------------------------
// Forward declarations (modular draw + input)
// -----------------------------------------------------------------------------
void drawSkybox();
void drawOcean();
void drawLighthouse();
void drawShip();
void drawSloop(float dayNightT);
void drawBirds();
void drawCrew(float dayNightT);
void drawDistantShips(float dayNightT);
void drawLighthouseBeamCone();

// -----------------------------------------------------------------------------
// Utility helpers
// -----------------------------------------------------------------------------
float clamp01(float v) {
    return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

void setColorLerp(float dr, float dg, float db, float nr, float ng, float nb, float t) {
    glColor3f(lerp(dr, nr, t), lerp(dg, ng, t), lerp(db, nb, t));
}

void setVec4Lerp(GLfloat out[4], const GLfloat day[4], const GLfloat night[4], float t) {
    out[0] = lerp(day[0], night[0], t);
    out[1] = lerp(day[1], night[1], t);
    out[2] = lerp(day[2], night[2], t);
    out[3] = lerp(day[3], night[3], t);
}

float smoothStep(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

// Pull RGB toward luminance (0 = unchanged, 1 = grayscale).
void desaturateRGB(float& r, float& g, float& b, float amount) {
    amount = clamp01(amount);
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    r = lerp(r, y, amount);
    g = lerp(g, y, amount);
    b = lerp(b, y, amount);
}

// Simple filmic shoulder + mild gamma for less synthetic, clipped gradients.
void filmicRGB(float& r, float& g, float& b, float exposure) {
    auto tone = [exposure](float x) {
        const float ex = x * exposure;
        const float mapped = ex / (1.0f + ex);
        return std::pow(clamp01(mapped), 1.0f / 1.10f);
    };
    r = tone(r);
    g = tone(g);
    b = tone(b);
}

// Small deterministic hull vertex offsets so silhouettes are not perfectly CAD-straight.
static void hullWobbleAt(float x, float y, float z, float* ox, float* oy, float* oz) {
    const float k = 0.036f;
    *ox = x + k * std::sin(x * 2.05f + y * 1.22f + z * 0.74f + 0.31f);
    *oy = y + k * std::sin(y * 1.98f + z * 1.15f + x * 0.63f + 1.07f);
    *oz = z + k * std::sin(z * 2.11f + x * 0.88f + y * 0.97f + 2.19f);
}

// 32-bit mix — decently uncorrelated coordinates for procedural stars.
static unsigned int starHashMix(unsigned int x) {
    x ^= x >> 16;
    x *= 2654435761u;
    x ^= x >> 13;
    x *= 2246822519u;
    x ^= x >> 16;
    return x;
}

// Independent X/Y in normalized sky quad (avoids “three lines” from bitmask overlap + i, i+3, …).
static void randomStarXY(int i, float* sx, float* sy) {
    const unsigned int seed = starHashMix(static_cast<unsigned int>(i) * 374761393u + 668265263u);
    const unsigned int ax = starHashMix(seed);
    const unsigned int ay = starHashMix(seed ^ 0x9e3779b9u);
    *sx = (static_cast<float>(ax & 0xffffffu) / float(0xffffff)) * 0.94f + 0.03f;
    *sy = (static_cast<float>(ay & 0xffffffu) / float(0xffffff)) * 0.68f + 0.14f;
}

// Wide Milky Way ribbon along a curved path (triangle strip, noise-driven width/brightness).
static void drawGalacticRibbon2D(float nightBlend, float tw) {
    if (nightBlend < 0.04f) {
        return;
    }
    const int N = 52;
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= N; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(N);
        const float px = 0.06f + 0.88f * u + 0.035f * std::sin(u * 6.8f + tw * 0.7f);
        const float py = 0.76f - 0.56f * u + 0.04f * std::sin(u * 9.3f - tw * 0.4f) + 0.02f * std::sin(u * 21.0f);
        const float uu = std::min(1.0f, u + 0.012f);
        const float px2 = 0.06f + 0.88f * uu + 0.035f * std::sin(uu * 6.8f + tw * 0.7f);
        const float py2 = 0.76f - 0.56f * uu + 0.04f * std::sin(uu * 9.3f - tw * 0.4f) + 0.02f * std::sin(uu * 21.0f);
        float tx = px2 - px, ty = py2 - py;
        const float tlen = std::sqrt(tx * tx + ty * ty);
        if (tlen > 1.0e-4f) {
            tx /= tlen;
            ty /= tlen;
        }
        const float nx = -ty, ny = tx;
        const float w = (0.014f + 0.018f * (0.5f + 0.5f * std::sin(u * 24.0f + tw * 1.1f)))
                        * (0.35f + 0.65f * std::sin(u * kPi));
        const float dens =
            (0.12f + 0.38f * std::abs(std::sin(u * 27.0f + tw * 0.9f))) * (0.55f + 0.45f * std::sin(u * kPi * 0.5f + 0.2f));
        const float a = dens * nightBlend;
        glColor4f(0.50f, 0.54f * (0.94f + 0.06f * std::sin(u * 8.0f)), 0.78f, a * 0.55f);
        glVertex2f(px + nx * w, py + ny * w);
        glColor4f(0.42f, 0.46f, 0.70f, a * 0.38f);
        glVertex2f(px - nx * w * 0.85f, py - ny * w * 0.85f);
    }
    glEnd();
}

// Bright star: diffraction spikes + compact core (reads “optics / camera”, not a point sprite line).
static void drawBrightStarWithSpikes2D(float x, float y, float r, float g, float b, float alpha) {
    const float spike = 0.022f + 0.010f * alpha;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glLineWidth(1.15f);
    glBegin(GL_LINES);
    glColor4f(r, g, b, alpha * 0.35f);
    glVertex2f(x - spike * 1.7f, y);
    glVertex2f(x + spike * 1.7f, y);
    glVertex2f(x, y - spike * 1.7f);
    glVertex2f(x, y + spike * 1.7f);
    glColor4f(r, g, b, alpha * 0.22f);
    glVertex2f(x - spike, y - spike);
    glVertex2f(x + spike, y + spike);
    glVertex2f(x - spike, y + spike);
    glVertex2f(x + spike, y - spike);
    glEnd();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(r, g, b, alpha);
    drawFilledCircle(x, y, 0.0038f + alpha * 0.0045f, 14);
    glColor4f(1.0f, 1.0f, 1.0f, alpha * 0.65f);
    drawFilledCircle(x, y, 0.0016f, 10);
}

// Layered field: faint haze, clusters, magnitude mix, bright spikes, occasional meteor.
static void drawStarFieldAdvanced2D(float starAlpha) {
    if (starAlpha < 0.05f) {
        return;
    }
    const float sa = starAlpha;
    const float twt = gSceneTimeSec;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Distant “haze” of unresolved stars (very dim, dense).
    glPointSize(1.0f);
    glBegin(GL_POINTS);
    for (int i = 0; i < 820; ++i) {
        const unsigned int h = starHashMix(static_cast<unsigned int>(i + 9102) * 2246822519u + 374123u);
        if ((h & 15u) == 0u) {
            continue;
        }
        float sx, sy;
        randomStarXY(i + 15000, &sx, &sy);
        if (sy < 0.26f) {
            continue;
        }
        const float tw = 0.35f + 0.65f * std::sin(twt * (0.7f + static_cast<float>((h >> 17) % 5u) * 0.09f) + static_cast<float>(i) * 0.13f);
        const float br = sa * tw * 0.38f;
        glColor4f(0.82f * br, 0.86f * br, 1.0f * br, br * 1.15f);
        glVertex2f(sx, sy);
    }
    glEnd();

    // Galactic clusters: local overdensity, not regular grids.
    for (int c = 0; c < 18; ++c) {
        const unsigned int hc = starHashMix(static_cast<unsigned int>(c) * 50154281u + 14009u);
        const float cxx =
            (static_cast<float>(starHashMix(hc) & 0xffffffu) / float(0xffffff)) * 0.88f + 0.06f;
        const float cyy =
            (static_cast<float>(starHashMix(hc ^ 101u) & 0xffffffu) / float(0xffffff)) * 0.58f + 0.18f;
        if (cyy < 0.24f) {
            continue;
        }
        glPointSize(1.4f);
        glBegin(GL_POINTS);
        for (int k = 0; k < 48; ++k) {
            const unsigned int hk = starHashMix(hc + static_cast<unsigned int>(k) * 12582917u);
            const float ang = (static_cast<float>(hk & 0xffffu) / 65536.0f) * (2.0f * kPi);
            const float rad = (static_cast<float>((hk >> 8) & 0xfffu) / float(0xfff)) * 0.055f;
            const float sx = cxx + std::cos(ang) * rad;
            const float sy = cyy + std::sin(ang) * rad * 0.88f;
            if (sy < 0.20f || sx < 0.02f || sx > 0.98f) {
                continue;
            }
            const float mag = 0.45f + 0.55f * static_cast<float>((hk >> 20) % 7u) / 6.0f;
            float rr = lerp(0.75f, 1.0f, static_cast<float>((hk >> 24) % 6u) / 5.0f);
            const float gg = lerp(0.82f, 1.0f, static_cast<float>((hk >> 10) % 5u) / 4.0f);
            const float bb = lerp(0.92f, 1.0f, static_cast<float>((hk >> 5) % 4u) / 3.0f);
            const float br = sa * mag * 0.92f;
            glColor4f(rr * br, gg * br, bb * br, br);
            glVertex2f(sx, sy);
        }
        glEnd();
    }

    // General mid-field (magnitude distribution).
    for (int pass = 0; pass < 4; ++pass) {
        const float psz = 1.15f + static_cast<float>(pass) * 0.42f;
        glPointSize(psz);
        glBegin(GL_POINTS);
        for (int i = 0; i < 480; ++i) {
            const unsigned int h = starHashMix(static_cast<unsigned int>(i) * 747796405u + 2891336453u);
            if (static_cast<int>(h % 4u) != pass) {
                continue;
            }
            const int magBin = static_cast<int>((h >> 14) % 32u);
            if (magBin < 5 && pass > 1) {
                continue;
            }
            float sx, sy;
            randomStarXY(i, &sx, &sy);
            if (sy < 0.30f && (h & 6u) != 0u) {
                continue;
            }
            const float tw =
                0.52f + 0.48f * std::sin(twt * (0.85f + static_cast<float>((h >> 8) % 7u) * 0.07f) + static_cast<float>(i) * 0.69f);
            const float br = sa * tw * (0.55f + 0.45f * static_cast<float>(magBin) / 31.0f);
            const float warm = static_cast<float>((h >> 11) % 13u) / 12.0f;
            glColor4f(lerp(0.76f, 1.0f, warm) * br, lerp(0.82f, 1.0f, 1.0f - warm * 0.38f) * br,
                      lerp(0.93f, 1.0f, 0.62f + warm * 0.22f) * br, br);
            glVertex2f(sx, sy);
        }
        glEnd();
    }

    // Few dozen bright stars with optical spikes.
    for (int j = 0; j < 34; ++j) {
        float bx, by;
        randomStarXY(24000 + j * 131, &bx, &by);
        const unsigned int hj = starHashMix(static_cast<unsigned int>(j) * 3628270291u + 10594353u);
        if (by < 0.22f) {
            continue;
        }
        const float pul = 0.38f + 0.62f * std::sin(twt * 1.15f + static_cast<float>(j) * 1.47f);
        const float wa = static_cast<float>((hj >> 18) % 11u) / 10.0f;
        drawBrightStarWithSpikes2D(bx, by, lerp(0.82f, 1.0f, wa), lerp(0.88f, 1.0f, 1.0f - wa * 0.25f), 1.0f,
                                   0.62f * sa * pul);
    }

    // Occasional meteor / shooting star (short streak, fades along path).
    {
        const float cyc = std::fmod(twt, 31.0f);
        if (cyc < 0.42f) {
            const float u = cyc / 0.42f;
            const float x0 = 0.92f - u * 0.62f;
            const float y0 = 0.68f + u * 0.10f;
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glLineWidth(2.0f);
            glBegin(GL_LINE_STRIP);
            for (int s = 0; s <= 10; ++s) {
                const float ff = static_cast<float>(s) / 10.0f;
                const float xa = x0 + ff * 0.10f;
                const float ya = y0 - ff * 0.05f;
                glColor4f(1.0f, 0.95f, 0.88f, 0.55f * sa * (1.0f - ff * 0.92f));
                glVertex2f(xa, ya);
            }
            glEnd();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glColor4f(1.0f, 1.0f, 1.0f, 0.85f * sa * (1.0f - u));
            drawFilledCircle(x0, y0, 0.0028f, 8);
        }
    }

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
}

static void glNormalFromQuad3(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz) {
    const float ux = bx - ax, uy = by - ay, uz = bz - az;
    const float vx = cx - ax, vy = cy - ay, vz = cz - az;
    float nx = uy * vz - uz * vy;
    float ny = uz * vx - ux * vz;
    float nz = ux * vy - uy * vx;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1.0e-5f) {
        nx /= len;
        ny /= len;
        nz /= len;
    }
    glNormal3f(nx, ny, nz);
}

float waveHeight(float x, float z) {
    // Layered wave groups: long swells + faster chop so lighting catches moving crests.
    const float w1 = std::sin((x * kWaveFrequency) + gWavePhase);
    const float w2 = std::sin((z * (kWaveFrequency * 1.35f)) + gWavePhase * 1.25f);
    const float w3 = std::sin(((x + z) * (kWaveFrequency * 0.7f)) + gWavePhase * 0.78f);
    const float w4 = std::sin(((x * 1.8f - z) * (kWaveFrequency * 0.42f)) + gWavePhase * 0.51f);
    const float w5 = std::sin((x * kWaveFrequency * 3.9f) + gWavePhase * 1.55f);
    const float w6 = std::sin((z * kWaveFrequency * 3.5f) - gWavePhase * 1.38f);
    const float primary = (w1 * 0.38f + w2 * 0.30f + w3 * 0.20f + w4 * 0.12f) * (kWaveAmplitude * 1.25f);
    const float chop = (w5 * 0.55f + w6 * 0.45f) * (kWaveAmplitude * 0.42f);
    return primary + chop;
}

float waveHeightAtWorld(float wx, float wz) {
    // World XZ distance from origin — curvature lowers far water toward the horizon.
    const float distSq = wx * wx + wz * wz;
    const float horizonDrop = kHorizonCurvature * distSq;
    return waveHeight(wx, wz - 8.0f) - 4.0f - horizonDrop;
}

void drawFilledCircle(float cx, float cy, float radius, int segments) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(cx, cy, 0.0f);
    for (int i = 0; i <= segments; ++i) {
        const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(segments);
        glVertex3f(cx + std::cos(a) * radius, cy + std::sin(a) * radius, 0.0f);
    }
    glEnd();
}

void drawFilledEllipse(float cx, float cy, float rx, float ry, int segments) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(cx, cy, 0.0f);
    for (int i = 0; i <= segments; ++i) {
        const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(segments);
        glVertex3f(cx + std::cos(a) * rx, cy + std::sin(a) * ry, 0.0f);
    }
    glEnd();
}

// One horizontal cloud layer: wavy stratus sheet (triangle strip in 2D), not circular puffs.
static void drawCloudStratum(float yMid, float halfThick, float cycles, float drift,
                             float cr, float cg, float cb, float alphaStrength, float cloudVis, int segments) {
    const float twoPi = 2.0f * kPi;
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= segments; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(segments);
        const float xang = x * cycles * twoPi + drift;
        const float n1 = std::sin(xang);
        const float n2 = 0.48f * std::sin(xang * 2.13f - drift * 0.9f + 1.1f);
        const float n3 = 0.32f * std::sin(x * 37.0f + drift * 14.0f);
        const float warp = n1 + n2 + n3;
        const float yTop = yMid + halfThick * (0.92f + 0.38f * warp);
        const float yBot = yMid - halfThick * (0.88f - 0.32f * warp);
        const float edge = std::sin(x * kPi);
        const float edgeAtten = edge * edge;
        const float aBase = alphaStrength * (0.28f + 0.72f * edgeAtten) * cloudVis;
        glColor4f(cr * 1.02f, cg * 1.02f, cb * 1.04f, aBase * 0.55f);
        glVertex2f(x, yTop);
        glColor4f(cr * 0.42f, cg * 0.44f, cb * 0.52f, aBase * 0.92f);
        glVertex2f(x, yBot);
    }
    glEnd();
}

// Richer night sky: zenith depth, Milky Way band, airglow — still fixed-pipeline 2D.
static void drawNightSkyRich2D(float nightBlend) {
    if (nightBlend < 0.03f) {
        return;
    }
    const float nb = nightBlend;
    const float tw = gSceneTimeSec * 0.015f;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Extra zenith violet / indigo (on top of base gradient).
    glBegin(GL_QUADS);
    glColor4f(0.12f, 0.08f, 0.26f, 0.42f * nb);
    glVertex2f(0.0f, 1.0f);
    glVertex2f(1.0f, 1.0f);
    glColor4f(0.04f, 0.05f, 0.12f, 0.0f);
    glVertex2f(1.0f, 0.42f);
    glVertex2f(0.0f, 0.42f);
    glEnd();

    // Curved galaxy ribbon (reads as structure vs. scattered ellipses alone).
    drawGalacticRibbon2D(nb, tw);

    // Milky Way: soft diagonal streaks of overlapping ellipses (static + slow drift).
    for (int k = 0; k < 14; ++k) {
        const float fk = static_cast<float>(k);
        const float ox = 0.12f + std::fmod(fk * 0.173f + tw * 0.08f, 0.76f);
        const float oy = 0.36f + 0.42f * std::sin(fk * 0.71f + tw * 0.5f) + 0.08f * std::sin(fk * 1.1f);
        const float rx = 0.10f + 0.04f * std::sin(fk * 0.9f + tw);
        const float ry = 0.014f + 0.006f * std::sin(fk * 1.3f);
        const float a = (0.038f + 0.022f * std::sin(fk * 2.1f + tw)) * nb;
        glColor4f(0.48f, 0.52f, 0.72f, a);
        drawFilledEllipse(ox, oy, rx, ry, 20);
        glColor4f(0.62f, 0.58f, 0.75f, a * 0.45f);
        drawFilledEllipse(ox + 0.012f, oy - 0.008f, rx * 0.55f, ry * 1.2f, 14);
    }

    // Horizon airglow / last light (very thin band).
    glBegin(GL_QUADS);
    glColor4f(0.12f, 0.14f, 0.28f, 0.0f);
    glVertex2f(0.0f, 0.50f);
    glVertex2f(1.0f, 0.50f);
    glColor4f(0.20f, 0.22f, 0.38f, 0.18f * nb);
    glVertex2f(1.0f, 0.36f);
    glVertex2f(0.0f, 0.36f);
    glEnd();

    // Faint nocturnal haze streaks (not day clouds — silvery, sparse).
    const float hazeVis = nb * 0.22f;
    drawCloudStratum(0.58f, 0.018f, 2.0f, tw * 1.2f + 2.0f, 0.42f, 0.44f, 0.52f, 0.18f, hazeVis, 36);
    drawCloudStratum(0.68f, 0.012f, 2.8f, tw * 0.9f + 4.0f, 0.38f, 0.40f, 0.50f, 0.14f, hazeVis, 32);

    glDisable(GL_BLEND);
}

// Moody overcast: layered horizon deck + wavy strata + faint backlit rim (2D sky plane).
static void drawMoodyCloudscape2D(float sunX, float sunY, float sunVis, float cloudVis) {
    const float time = gSceneTimeSec * 0.03458f + gWavePhase * 0.018f;

    // Dense dark band just above horizon — continuous deck, compressed toward horizon.
    {
        float dr = 0.09f, dg = 0.105f, db = 0.14f;
        desaturateRGB(dr, dg, db, 0.12f);
        glBegin(GL_QUADS);
        glColor4f(dr * 0.85f, dg * 0.85f, db * 0.88f, 0.72f * cloudVis);
        glVertex2f(0.0f, 0.36f);
        glVertex2f(1.0f, 0.36f);
        glColor4f(dr * 1.25f, dg * 1.25f, db * 1.22f, 0.12f * cloudVis);
        glVertex2f(1.0f, 0.58f);
        glVertex2f(0.0f, 0.58f);
        glEnd();
    }

    // Broken stratus layers (stacked horizontal sheets).
    const int numStrata = 10;
    for (int s = 0; s < numStrata; ++s) {
        const float fi = static_cast<float>(s) / static_cast<float>(numStrata - 1);
        const float yM = 0.48f + fi * 0.40f;
        const float hTh = 0.012f + fi * 0.026f;
        const float cycles = 1.15f + fi * 3.2f;
        const float drift = time * (0.55f + fi * 0.35f) + static_cast<float>(s) * 1.83f;
        const float liftTowardSun =
            sunVis > 0.08f ? (1.0f - std::fabs(sunX - 0.5f) * 0.4f) * 0.04f * sunVis : 0.0f;
        float cr = lerp(0.72f, 0.93f, fi);
        float cg = lerp(0.74f, 0.93f, fi);
        float cb = lerp(0.78f, 0.96f, fi);
        desaturateRGB(cr, cg, cb, 0.25f + fi * 0.08f);
        const float alphaStr = lerp(0.42f, 0.22f, fi) * (0.75f + 0.25f * sunVis);
        drawCloudStratum(yM + liftTowardSun, hTh, cycles, drift, cr, cg, cb, alphaStr, cloudVis, 48);
    }

    // Silver-lining / blown-out rim near sun (elongated horizontal glow, not a puff).
    if (sunVis > 0.06f) {
        for (int b = 0; b < 6; ++b) {
            const float bb = static_cast<float>(b);
            const float yy = sunY + 0.025f * bb + 0.012f * std::sin(time * 1.4f + bb);
            const float halfW = 0.22f + bb * 0.045f;
            const float sx0 = clamp01(sunX - halfW);
            const float sx1 = clamp01(sunX + halfW);
            const float bright = (0.11f - bb * 0.014f) * cloudVis * sunVis;
            glBegin(GL_QUADS);
            glColor4f(0.94f, 0.93f, 0.90f, bright);
            glVertex2f(sx0, yy);
            glVertex2f(sx1, yy);
            glColor4f(0.92f, 0.92f, 0.94f, 0.0f);
            glVertex2f(sx1, yy + 0.022f + bb * 0.004f);
            glVertex2f(sx0, yy + 0.022f + bb * 0.004f);
            glEnd();
        }
    }

    // Very faint vertical light streaks (god-ray hint) — low contrast, fixed pipeline friendly.
    if (sunVis > 0.08f) {
        for (int r = 0; r < 8; ++r) {
            const float u = 0.08f + static_cast<float>(r) * 0.106f;
            const float sway = 0.028f * std::sin(time * 0.7f + static_cast<float>(r) * 1.1f);
            const float xC = clamp01(sunX + u - 0.45f + sway);
            const float beam = (0.03f + 0.02f * std::sin(time * 1.2f + static_cast<float>(r))) * cloudVis * sunVis;
            glBegin(GL_QUADS);
            glColor4f(0.88f, 0.90f, 0.94f, beam);
            glVertex2f(xC - 0.004f, sunY);
            glVertex2f(xC + 0.004f, sunY);
            glColor4f(0.75f, 0.78f, 0.85f, 0.0f);
            glVertex2f(xC + 0.016f, 0.34f);
            glVertex2f(xC - 0.016f, 0.34f);
            glEnd();
        }
    }
}

void drawCrewMember(float x, float y, float z, float t, bool captain) {
    glPushMatrix();
    glTranslatef(x, y, z);

    // Body
    if (captain) {
        setColorLerp(0.18f, 0.18f, 0.22f, 0.55f, 0.55f, 0.65f, t);
    } else {
        setColorLerp(0.24f, 0.24f, 0.28f, 0.45f, 0.45f, 0.55f, t);
    }
    glPushMatrix();
    glScalef(0.34f, 0.78f, 0.24f);
    glutSolidCube(1.0);
    glPopMatrix();

    // Head
    setColorLerp(0.85f, 0.72f, 0.58f, 0.70f, 0.64f, 0.60f, t);
    glTranslatef(0.0f, 0.55f, 0.0f);
    glutSolidSphere(0.19, 12, 12);

    // Hat (captain only)
    if (captain) {
        setColorLerp(0.08f, 0.08f, 0.10f, 0.18f, 0.18f, 0.22f, t);
        glTranslatef(0.0f, 0.14f, 0.0f);
        glScalef(0.45f, 0.10f, 0.45f);
        glutSolidCube(1.0);
    }
    glPopMatrix();
}

// Crew on the tall-ship deck (simple 3D shapes for lab demo).
void drawCrew(float dayNightT) {
    drawCrewMember(0.8f, 1.58f, 0.85f, dayNightT, false);
    drawCrewMember(-1.0f, 1.58f, -0.55f, dayNightT, false);
    drawCrewMember(3.2f, 1.58f, 0.45f, dayNightT, false);
    drawCrewMember(-5.2f, 1.62f, 0.15f, dayNightT, true);
}

void drawShipRiggingLine(float x1, float y1, float z1, float x2, float y2, float z2, float t) {
    glDisable(GL_LIGHTING);
    glColor3f(lerp(0.12f, 0.22f, t), lerp(0.10f, 0.24f, t), lerp(0.08f, 0.28f, t));
    glBegin(GL_LINES);
    glVertex3f(x1, y1, z1);
    glVertex3f(x2, y2, z2);
    glEnd();
    glEnable(GL_LIGHTING);
}

// Procedural cloth wrinkles (fixed pipeline): multi-scale sines, gentle billow + creases.
static float sailWrinkle3(float u, float v, float w, float phaseRad, float amplitudeScale) {
    const float s1 = std::sin(4.5f * u + 2.4f * v + phaseRad);
    const float s2 = std::sin(8.8f * u - 5.9f * v - phaseRad * 0.71f);
    const float s3 = std::sin(1.9f * w + 3.2f * (u - v) + phaseRad * 1.08f);
    return amplitudeScale * (0.024f * s1 + 0.015f * s2 + 0.009f * s3);
}

// Barycentric-subdivided triangle sail in XY (z = wrinkle), corner A,B,C as weights (i,j,k) with i+j+k=N.
static void drawWrinkledTriSail(float ax, float ay, float bx, float by, float cx, float cy, int N, float phaseRad,
                                float pr, float pg, float pb, float pa,
                                float fr, float fg, float fb, float fa, float tr, float tg, float tb, float ta) {
    const float ymin = std::min(ay, std::min(by, cy));
    const float ymax = std::max(ay, std::max(by, cy));
    const float yspan = std::max(1.0e-3f, ymax - ymin);
    const float invN = 1.0f / static_cast<float>(N);
    auto emitVtx = [&](int ia, int ib, int ic) {
        const float px = invN * (ia * ax + ib * bx + ic * cx);
        const float py = invN * (ia * ay + ib * by + ic * cy);
        const float u = ia * invN;
        const float vv = ib * invN;
        const float wc = ic * invN;
        const float dz = sailWrinkle3(u, vv, u + vv + wc * 0.5f, phaseRad, 1.0f);
        const float bt = clamp01((py - ymin) / yspan);
        const float cr = lerp(fr, tr, bt);
        const float cg = lerp(fg, tg, bt);
        const float cb = lerp(fb, tb, bt);
        const float cal = lerp(fa, ta, bt);
        glColor4f(cr * pr, cg * pg, cb * pb, cal * pa);
        glVertex3f(px, py, dz);
    };
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N - i; ++j) {
            const int k = N - i - j;
            if (k < 1) {
                continue;
            }
            glBegin(GL_TRIANGLES);
            emitVtx(i, j, k);
            emitVtx(i + 1, j, k - 1);
            emitVtx(i, j + 1, k - 1);
            glEnd();
        }
    }
}

// Square sail sheet (port side of mast +X normal — tall-ship style).
static void drawSquareSail(float xm, float yTop, float zHalfWidth, float drop, float t) {
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const float yB = yTop - drop;
    // Weathered canvas: dusty gray-cream, slightly sheer; vertical gradient (sun-bleached aloft).
    float tr = lerp(0.82f, 0.63f, t);
    float tg = lerp(0.80f, 0.65f, t);
    float tb = lerp(0.72f, 0.66f, t);
    desaturateRGB(tr, tg, tb, 0.38f);
    const float al = lerp(0.74f, 0.68f, t);
    const float xBase = xm + 0.07f;
    const float phase = gWavePhase * 0.38f + xm * 0.19f + yTop * 0.11f + zHalfWidth * 0.07f;
    const int ny = 14;
    const int nz = 16;
    for (int j = 0; j < ny; ++j) {
        glBegin(GL_TRIANGLE_STRIP);
        for (int k = 0; k <= nz; ++k) {
            const float fk = static_cast<float>(k) / static_cast<float>(nz);
            const float zz = lerp(-zHalfWidth, zHalfWidth, fk);
            for (int jr = 0; jr < 2; ++jr) {
                const float fj = static_cast<float>(j + jr) / static_cast<float>(ny);
                const float yy = lerp(yB, yTop, fj);
                const float u = (yy - yB) / drop;
                const float v = (zz + zHalfWidth) / std::max(1.0e-4f, 2.0f * zHalfWidth);
                const float disp = sailWrinkle3(u, v, fj + fk, phase, 1.0f);
                const float cr = lerp(tr * 0.88f, tr * 1.02f, fj);
                const float cg = lerp(tg * 0.86f, tg * 1.02f, fj);
                const float cb = lerp(tb * 0.84f, tb * 1.01f, fj);
                const float ca = lerp(al * 0.92f, al, fj);
                glColor4f(cr, cg, cb, ca);
                glVertex3f(xBase + disp, yy, zz);
            }
        }
        glEnd();
    }
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

static void drawYard(float xm, float y, float zHalfWidth, float t) {
    glPushMatrix();
    glTranslatef(xm, y, 0.0f);
    setColorLerp(0.32f, 0.22f, 0.12f, 0.22f, 0.18f, 0.14f, t);
    glScalef(0.14f, 0.14f, zHalfWidth * 2.0f + 0.2f);
    glutSolidCube(1.0);
    glPopMatrix();
}

static void drawMastWithSquares(float xm, float mastHalfH, int yardCount, const float* yardYRelDeck,
                                float zSailHalf, float t) {
    const float deckY = 1.38f;
    glPushMatrix();
    glTranslatef(xm, deckY + mastHalfH, 0.0f);
    setColorLerp(0.34f, 0.24f, 0.13f, 0.24f, 0.20f, 0.15f, t);
    glScalef(0.24f, mastHalfH * 2.0f, 0.24f);
    glutSolidCube(1.0);
    glPopMatrix();
    for (int i = 0; i < yardCount; ++i) {
        const float yy = deckY + yardYRelDeck[i];
        drawYard(xm, yy, zSailHalf, t);
        drawSquareSail(xm, yy - 0.05f, zSailHalf, 1.35f, t);
    }
}

// -----------------------------------------------------------------------------
// Lighting setup
// -----------------------------------------------------------------------------
void setupLighting() {
    const float t = smoothStep(gDayNightBlend);

    // Day and night ambient models.
    const GLfloat ambientDay[4]   = {0.44f, 0.46f, 0.45f, 1.0f};
    const GLfloat ambientNight[4] = {0.12f, 0.14f, 0.22f, 1.0f};
    GLfloat ambientModel[4];
    setVec4Lerp(ambientModel, ambientDay, ambientNight, t);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambientModel);

    // Main directional light: behaves like sun by day and moon by night.
    const GLfloat mainDiffuseDay[4]   = {0.90f, 0.86f, 0.76f, 1.0f};
    const GLfloat mainDiffuseNight[4] = {0.34f, 0.40f, 0.56f, 1.0f};
    const GLfloat mainSpecDay[4]      = {0.46f, 0.44f, 0.40f, 1.0f};
    const GLfloat mainSpecNight[4]    = {0.18f, 0.22f, 0.34f, 1.0f};
    GLfloat mainDiffuse[4];
    GLfloat mainSpec[4];
    setVec4Lerp(mainDiffuse, mainDiffuseDay, mainDiffuseNight, t);
    setVec4Lerp(mainSpec, mainSpecDay, mainSpecNight, t);

    const GLfloat mainPos[4] = {35.0f, 45.0f, 20.0f, 0.0f};  // Directional
    glLightfv(GL_LIGHT0, GL_POSITION, mainPos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, mainDiffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, mainSpec);
    glEnable(GL_LIGHT0);

    // Cool hemispheric fill to soften hard contrasts and mimic sky bounce.
    const GLfloat fillPos[4] = {-18.0f, 26.0f, -30.0f, 0.0f};
    const GLfloat fillDay[4] = {0.22f, 0.28f, 0.34f, 1.0f};
    const GLfloat fillNight[4] = {0.08f, 0.10f, 0.16f, 1.0f};
    GLfloat fillDiff[4];
    setVec4Lerp(fillDiff, fillDay, fillNight, t);
    glLightfv(GL_LIGHT3, GL_POSITION, fillPos);
    glLightfv(GL_LIGHT3, GL_DIFFUSE, fillDiff);
    glLightfv(GL_LIGHT3, GL_SPECULAR, fillNight);
    glEnable(GL_LIGHT3);

    // Lighthouse spotlight: visible only at night.
    const float nightStrength = smoothStep(gDayNightBlend);
    if (nightStrength > 0.02f) {
        const GLfloat beamPos[4] = {kLhWorldX, kLhLanternWorldY, kLhWorldZ, 1.0f};

        const float a = gBeamAngleDeg * (kPi / 180.0f);
        GLfloat beamDir[3] = {std::cos(a), -0.10f, std::sin(a)};

        const GLfloat beamDiffuse[4] = {
            0.95f * nightStrength,
            0.95f * nightStrength,
            0.88f * nightStrength,
            1.0f
        };
        const GLfloat beamSpec[4] = {
            0.85f * nightStrength,
            0.85f * nightStrength,
            0.78f * nightStrength,
            1.0f
        };

        glLightfv(GL_LIGHT1, GL_POSITION, beamPos);
        glLightfv(GL_LIGHT1, GL_SPOT_DIRECTION, beamDir);
        glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, 18.0f);
        glLightf(GL_LIGHT1, GL_SPOT_EXPONENT, 24.0f);
        glLightf(GL_LIGHT1, GL_CONSTANT_ATTENUATION, 0.9f);
        glLightf(GL_LIGHT1, GL_LINEAR_ATTENUATION, 0.03f);
        glLightf(GL_LIGHT1, GL_QUADRATIC_ATTENUATION, 0.0008f);
        glLightfv(GL_LIGHT1, GL_DIFFUSE, beamDiffuse);
        glLightfv(GL_LIGHT1, GL_SPECULAR, beamSpec);
        glEnable(GL_LIGHT1);
    } else {
        glDisable(GL_LIGHT1);
    }

    // Red lantern glow (omnidirectional point in glass house — visible day + night like reference).
    const float lanternGlow = lerp(0.45f, 1.0f, nightStrength);
    const GLfloat redPos[4] = {kLhWorldX, kLhLanternWorldY, kLhWorldZ, 1.0f};
    const GLfloat redDiff[4] = {
        1.5f * lanternGlow,
        0.18f * lanternGlow,
        0.10f * lanternGlow,
        1.0f
    };
    const GLfloat redSpec[4] = {0.6f * lanternGlow, 0.1f * lanternGlow, 0.08f * lanternGlow, 1.0f};
    glLightfv(GL_LIGHT2, GL_POSITION, redPos);
    glLightfv(GL_LIGHT2, GL_DIFFUSE, redDiff);
    glLightfv(GL_LIGHT2, GL_SPECULAR, redSpec);
    glLightf(GL_LIGHT2, GL_CONSTANT_ATTENUATION, 1.0f);
    glLightf(GL_LIGHT2, GL_LINEAR_ATTENUATION, 0.045f);
    glLightf(GL_LIGHT2, GL_QUADRATIC_ATTENUATION, 0.002f);
    glEnable(GL_LIGHT2);

    // Atmospheric fog adds depth and reduces the toy/cartoon flatness.
    GLfloat fogColor[4] = {
        lerp(0.68f, 0.10f, t),
        lerp(0.79f, 0.13f, t),
        lerp(0.88f, 0.20f, t),
        1.0f
    };
    desaturateRGB(fogColor[0], fogColor[1], fogColor[2], 0.30f);
    // Exponential-squared fog yields softer atmospheric rolloff than linear ramps.
    glFogi(GL_FOG_MODE, GL_EXP2);
    glFogfv(GL_FOG_COLOR, fogColor);
    // Slightly lighter fog so nearer swells keep contrast (was washing out wave shading).
    glFogf(GL_FOG_DENSITY, lerp(0.0065f, 0.0105f, t));
}

// -----------------------------------------------------------------------------
// Scene drawing: skybox/background
// -----------------------------------------------------------------------------
void drawSkybox() {
    const float t = smoothStep(gDayNightBlend);

    float topR = lerp(0.42f, 0.06f, t);
    float topG = lerp(0.70f, 0.10f, t);
    float topB = lerp(0.88f, 0.18f, t);
    float botR = lerp(0.72f, 0.10f, t);
    float botG = lerp(0.84f, 0.14f, t);
    float botB = lerp(0.92f, 0.22f, t);
    desaturateRGB(topR, topG, topB, 0.34f);
    desaturateRGB(botR, botG, botB, 0.30f);

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    // Draw sky as a camera-independent backdrop to remove visible panel edges.
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glBegin(GL_QUADS);
    glColor3f(topR, topG, topB);
    glVertex2f(0.0f, 1.0f);
    glVertex2f(1.0f, 1.0f);
    glColor3f(botR, botG, botB);
    glVertex2f(1.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glEnd();

    // Horizon haze band helps separate sky and ocean for depth.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    {
        float hr = 0.76f, hg = 0.82f, hb = 0.90f;
        float lr = 0.24f, lg = 0.28f, lb = 0.36f;
        desaturateRGB(hr, hg, hb, 0.28f);
        desaturateRGB(lr, lg, lb, 0.22f);
        glBegin(GL_QUADS);
        glColor4f(hr, hg, hb, 0.20f * (1.0f - t));
        glVertex2f(0.0f, 0.47f);
        glVertex2f(1.0f, 0.47f);
        glColor4f(lr, lg, lb, 0.20f * t);
        glVertex2f(1.0f, 0.40f);
        glVertex2f(0.0f, 0.40f);
        glEnd();
    }
    glDisable(GL_BLEND);

    // Night-only depth: Milky Way, airglow, faint high haze (under sun/moon so bodies stay sharp).
    drawNightSkyRich2D(t);

    // Sun position used by clouds (backlit rim) and by the disk below.
    const float sunVisibility = 1.0f - t;
    const float sunX = 0.78f - t * 0.24f;
    const float sunY = 0.80f - t * 0.58f;

    // Daytime: layered stratus / storm deck (wavy sheets), not ellipse puffs.
    const float cloudVisibility = (1.0f - t);
    if (cloudVisibility > 0.04f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        drawMoodyCloudscape2D(sunX, sunY, sunVisibility, cloudVisibility);
        glDisable(GL_BLEND);
    }

    // Day/night celestial transition with vertical travel (sun sets, moon rises).
    if (sunVisibility > 0.01f) {
        const float sv = sunVisibility;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // Wide soft corona (warm, yellow-orange).
        glColor4f(1.0f, 0.78f, 0.28f, 0.14f * sv);
        drawFilledCircle(sunX, sunY, 0.118f, 48);
        glColor4f(1.0f, 0.86f, 0.38f, 0.28f * sv);
        drawFilledCircle(sunX, sunY, 0.095f, 52);
        // Main photosphere — yellow–gold toward sunset (sv low → more orange).
        {
            const float u = 1.0f - sv;
            const float sr = lerp(1.0f, 0.98f, u);
            const float sg = lerp(0.82f, 0.52f, u);
            const float sb = lerp(0.20f, 0.18f, u);
            glColor4f(sr, sg, sb, 0.96f * sv);
        }
        drawFilledCircle(sunX, sunY, 0.082f, 56);
        // Hotter core (more yellow-white).
        glColor4f(1.0f, 0.94f, 0.58f, 0.88f * sv);
        drawFilledCircle(sunX, sunY, 0.052f, 40);
        glDisable(GL_BLEND);
    }

    // Star field: clustered depth, magnitude passes, spiky bright stars, rare meteor.
    const float starAlpha = t;
    if (starAlpha > 0.05f) {
        drawStarFieldAdvanced2D(starAlpha);
    }

    const float moonVisibility = t;
    if (moonVisibility > 0.01f) {
        const float moonX = 0.22f + t * 0.30f;
        const float moonY = 0.28f + t * 0.52f;
        const float moonR = 0.036f;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // Full disk: cool silver-white (minimal desat — not the sun’s yellow).
        glColor4f(0.94f * moonVisibility, 0.96f * moonVisibility, 1.0f * moonVisibility, 0.97f * moonVisibility);
        drawFilledCircle(moonX, moonY, moonR, 48);
        // Carve a crescent: dark terminator disk offset — gives a pointed lit limb.
        glColor4f(0.06f * moonVisibility, 0.08f * moonVisibility, 0.12f * moonVisibility, 0.93f * moonVisibility);
        drawFilledCircle(moonX + 0.021f, moonY + 0.003f, moonR * 0.92f, 48);
        // Sharp bright limb (pointer) on the lit crescent — reads unlike the sun disk.
        glColor4f(1.0f * moonVisibility, 1.0f * moonVisibility, 1.0f * moonVisibility, 0.55f * moonVisibility);
        drawFilledEllipse(moonX - moonR * 0.55f, moonY + moonR * 0.08f, moonR * 0.055f, moonR * 0.12f, 20);
        glDisable(GL_BLEND);
    }


    // Very subtle edge vignette helps break the "flat render" feel.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glColor4f(0.05f, 0.06f, 0.08f, 0.00f);
    glVertex2f(0.10f, 0.90f);
    glVertex2f(0.90f, 0.90f);
    glVertex2f(0.90f, 0.10f);
    glVertex2f(0.10f, 0.10f);
    glColor4f(0.05f, 0.06f, 0.08f, 0.14f * (0.70f + 0.30f * t));
    glVertex2f(0.0f, 1.0f);
    glVertex2f(1.0f, 1.0f);
    glVertex2f(1.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glEnd();
    glDisable(GL_BLEND);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

// Birds crossing the sky (visible during daytime; hidden at night).
void drawBirds() {
    const float dayVis = 1.0f - smoothStep(gDayNightBlend);
    if (dayVis < 0.03f) {
        return;
    }

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.8f);

    for (int i = 0; i < 12; ++i) {
        const float flock = static_cast<float>(i / 4);
        const float speed = 8.5f + static_cast<float>(i % 4) * 1.8f + flock * 0.8f;
        const float pathOffset = static_cast<float>(i) * 36.0f + flock * 22.0f;
        const float x = std::fmod(gSceneTimeSec * speed + pathOffset, 180.0f) - 90.0f;
        const float y = 18.0f + flock * 4.2f + std::sin(gSceneTimeSec * 0.9f + i * 0.7f) * (0.5f + flock * 0.2f);
        const float z = -24.0f - static_cast<float>(i % 5) * 5.0f - flock * 2.5f;
        const float flap = 0.45f + 0.35f * std::sin(gSceneTimeSec * 6.0f + i * 0.7f);
        const float wingSpan = 0.55f + flap * 0.15f;

        glColor4f(0.12f, 0.12f, 0.14f, 0.88f * dayVis);
        glPushMatrix();
        glTranslatef(x, y, z);
        glRotatef(-10.0f + static_cast<float>(i % 3) * 5.0f + flock * 2.0f, 0.0f, 0.0f, 1.0f);
        glBegin(GL_LINE_STRIP);
        glVertex3f(-wingSpan, 0.0f, 0.0f);
        glVertex3f(-wingSpan * 0.35f, 0.12f * flap, 0.0f);
        glVertex3f(0.0f, 0.05f * flap, 0.0f);
        glVertex3f(wingSpan * 0.35f, 0.12f * flap, 0.0f);
        glVertex3f(wingSpan, 0.0f, 0.0f);
        glEnd();
        glPopMatrix();
    }

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

// -----------------------------------------------------------------------------
// Scene drawing: ocean
// -----------------------------------------------------------------------------
void drawOcean() {
    const float t = smoothStep(gDayNightBlend);
    // Large radial disc + horizon curvature + fog reads as endless open ocean.
    const int radialBands = 72;
    const int angleSteps = 112;
    const float maxRadius = 260.0f;
    const float angleStep = (2.0f * kPi) / static_cast<float>(angleSteps);

    {
        float wr = lerp(0.13f, 0.09f, t);
        float wg = lerp(0.44f, 0.26f, t);
        float wb = lerp(0.68f, 0.38f, t);
        desaturateRGB(wr, wg, wb, 0.36f);
        glColor3f(wr, wg, wb);
    }

    auto oceanHeightMesh = [](float wx, float wzMesh) {
        const float wZworld = wzMesh + 8.0f;
        const float distSq = wx * wx + wZworld * wZworld;
        const float horizonDrop = kHorizonCurvature * distSq;
        return waveHeight(wx, wzMesh) - 4.0f - horizonDrop;
    };
    auto cloudShadow = [&](float wx, float wzWorld) {
        const float n1 = std::sin(wx * 0.045f + gSceneTimeSec * 0.08f);
        const float n2 = std::sin(wzWorld * 0.038f - gSceneTimeSec * 0.06f + 1.4f);
        const float n3 = std::sin((wx + wzWorld) * 0.024f + gSceneTimeSec * 0.03f + 2.7f);
        const float shade = 0.86f + 0.14f * clamp01((n1 * 0.45f + n2 * 0.35f + n3 * 0.20f) * 0.5f + 0.5f);
        return shade;
    };
    for (int r = 0; r < radialBands; ++r) {
        const float r1 = (static_cast<float>(r) / radialBands) * maxRadius;
        const float r2 = (static_cast<float>(r + 1) / radialBands) * maxRadius;
        glBegin(GL_TRIANGLE_STRIP);
        for (int a = 0; a <= angleSteps; ++a) {
            const float ang = a * angleStep;
            const float c = std::cos(ang);
            const float s = std::sin(ang);

            const float wx1 = r1 * c;
            const float wz1 = r1 * s;
            const float wx2 = r2 * c;
            const float wz2 = r2 * s;

            const float nGain = 2.45f;
            float dX = oceanHeightMesh(wx1 + 0.4f, wz1) - oceanHeightMesh(wx1 - 0.4f, wz1);
            float dZ = oceanHeightMesh(wx1, wz1 + 0.4f) - oceanHeightMesh(wx1, wz1 - 0.4f);
            glNormal3f(-dX * nGain, 1.0f, -dZ * nGain);
            {
                float wr = lerp(0.13f, 0.09f, t);
                float wg = lerp(0.44f, 0.26f, t);
                float wb = lerp(0.68f, 0.38f, t);
                desaturateRGB(wr, wg, wb, 0.36f);
                const float shade = cloudShadow(wx1, wz1 + 8.0f);
                const float slope2 = dX * dX + dZ * dZ;
                const float facet = 1.0f + 0.14f * clamp01(slope2 * 6.5f);
                glColor3f(wr * shade * facet, wg * shade * facet, wb * shade * (0.96f + 0.06f * facet));
            }
            glVertex3f(wx1, oceanHeightMesh(wx1, wz1), wz1 + 8.0f);
            dX = oceanHeightMesh(wx2 + 0.4f, wz2) - oceanHeightMesh(wx2 - 0.4f, wz2);
            dZ = oceanHeightMesh(wx2, wz2 + 0.4f) - oceanHeightMesh(wx2, wz2 - 0.4f);
            glNormal3f(-dX * nGain, 1.0f, -dZ * nGain);
            {
                float wr = lerp(0.13f, 0.09f, t);
                float wg = lerp(0.44f, 0.26f, t);
                float wb = lerp(0.68f, 0.38f, t);
                desaturateRGB(wr, wg, wb, 0.36f);
                const float shade = cloudShadow(wx2, wz2 + 8.0f);
                const float slope2 = dX * dX + dZ * dZ;
                const float facet = 1.0f + 0.14f * clamp01(slope2 * 6.5f);
                glColor3f(wr * shade * facet, wg * shade * facet, wb * shade * (0.96f + 0.06f * facet));
            }
            glVertex3f(wx2, oceanHeightMesh(wx2, wz2), wz2 + 8.0f);
        }
        glEnd();
    }

    // Soft sun/moon sparkle streaks reduce the matte CG look of flat water.
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    const float sparkleStrength = lerp(0.16f, 0.10f, t);
    for (int i = 0; i < 140; ++i) {
        const float fi = static_cast<float>(i);
        const float ang = 0.05f * std::sin(gSceneTimeSec * 0.11f + fi * 0.7f) + fi * 0.048f;
        const float rr = 6.0f + std::fmod(fi * 3.6f + gSceneTimeSec * 4.0f, 138.0f);
        const float wx = rr * std::sin(ang) * 0.55f + 8.0f;
        const float wz = rr * std::cos(ang) - 16.0f;
        const float y = waveHeightAtWorld(wx, wz) + 0.015f;
        const float streak = 0.45f + 0.55f * std::sin(gWavePhase * 2.4f + fi * 1.37f);
        const float a = sparkleStrength * streak;
        glColor4f(0.95f, 0.92f, 0.82f, a);
        glBegin(GL_LINES);
        glVertex3f(wx - 0.16f, y, wz);
        glVertex3f(wx + 0.16f, y, wz + 0.28f);
        glEnd();
    }
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);

    // Foam streaks in polar rings so accents follow the horizon, not a grid.
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int ring = 0; ring < 8; ++ring) {
        const float baseR = 22.0f + ring * 18.0f;
        for (int seg = 0; seg < 48; ++seg) {
            const float j = static_cast<float>(seg) + gWavePhase * 0.35f + ring * 7.3f;
            const float ang = (j / 48.0f) * (2.0f * kPi);
            const float rJ = baseR + 1.2f * std::sin(gWavePhase * 0.4f + seg * 0.31f + ring);
            const float wx = rJ * std::cos(ang);
            const float wzMesh = rJ * std::sin(ang);
            const float wZworld = wzMesh + 8.0f;
            const float y = waveHeightAtWorld(wx, wZworld) + 0.03f;
            const float alpha = 0.10f + 0.06f * std::sin(gWavePhase + seg * 0.3f + ring);
            {
                float fr = 0.84f, fg = 0.90f, fb = 0.94f;
                desaturateRGB(fr, fg, fb, 0.22f);
                glColor4f(fr, fg, fb, alpha * (0.45f + 0.55f * (1.0f - t)));
            }
            const float dx = 0.45f * std::cos(ang + kPi * 0.5f);
            const float dz = 0.45f * std::sin(ang + kPi * 0.5f);
            glBegin(GL_LINES);
            glVertex3f(wx - dx, y, wZworld - dz);
            glVertex3f(wx + dx, y, wZworld + dz);
            glEnd();
        }
    }
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

// Smaller background ships for depth (silhouettes on the horizon).
void drawDistantShips(float dayNightT) {
    struct ShipSlot {
        float x;
        float z;
        float scale;
        float yawDeg;
        float pathPhase;
    };
    const ShipSlot slots[] = {
        {-10.0f, -24.0f, 0.40f, 14.0f, 0.2f},
        {14.0f, -36.0f, 0.34f, -6.0f, 1.1f},
        {-24.0f, -30.0f, 0.36f, 18.0f, 0.7f},
        {30.0f, -26.0f, 0.30f, -4.0f, 1.6f},
        {4.0f, -42.0f, 0.28f, 2.0f, 2.2f},
    };

    for (const ShipSlot& s : slots) {
        const float sway = std::sin(gSceneTimeSec * 0.45f + s.pathPhase) * 1.4f;
        const float x = s.x + sway;
        const float z = s.z + std::cos(gSceneTimeSec * 0.12f + s.pathPhase) * 2.0f;
        const float yBase = waveHeightAtWorld(x, z) - 0.8f * s.scale;
        const float bob = std::sin(gWavePhase * 0.6f + s.pathPhase) * 0.35f * s.scale;

        glPushMatrix();
        glTranslatef(x, yBase + bob, z);
        glRotatef(s.yawDeg, 0.0f, 1.0f, 0.0f);
        glScalef(s.scale, s.scale, s.scale);

        // Distant atmospheric fade: compress contrast toward fog color by range.
        const float haze = clamp01((-z - 22.0f) / 24.0f);
        const float baseR = lerp(0.12f, 0.07f, dayNightT);
        const float baseG = lerp(0.10f, 0.07f, dayNightT);
        const float baseB = lerp(0.09f, 0.08f, dayNightT);
        const float fogR = lerp(0.68f, 0.10f, dayNightT);
        const float fogG = lerp(0.79f, 0.13f, dayNightT);
        const float fogB = lerp(0.88f, 0.20f, dayNightT);
        glColor3f(lerp(baseR, fogR, haze * 0.42f), lerp(baseG, fogG, haze * 0.42f), lerp(baseB, fogB, haze * 0.42f));
        glBegin(GL_QUADS);
        glNormal3f(0.0f, 0.2f, 1.0f);
        glVertex3f(-5.8f, -0.25f,  1.4f);
        glVertex3f( 5.6f, -0.05f,  1.2f);
        glVertex3f( 4.8f,  1.0f,   1.0f);
        glVertex3f(-5.2f,  1.1f,   1.1f);
        glNormal3f(0.0f, 0.2f, -1.0f);
        glVertex3f(-5.8f, -0.25f, -1.4f);
        glVertex3f(-5.2f,  1.1f,  -1.1f);
        glVertex3f( 4.8f,  1.0f,  -1.0f);
        glVertex3f( 5.6f, -0.05f, -1.2f);
        glEnd();

        // Gold side ports (tiny rings).
        for (float gx = -3.8f; gx < 4.6f; gx += 1.25f) {
            for (int si = 0; si < 2; ++si) {
                const float side = (si == 0) ? -1.0f : 1.0f;
                glPushMatrix();
                glTranslatef(gx, 0.25f, side * 1.38f);
                glRotatef(side * 90.0f, 0.0f, 1.0f, 0.0f);
                glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
                setColorLerp(0.88f, 0.68f, 0.16f, 0.45f, 0.36f, 0.14f, dayNightT);
                glutSolidTorus(0.04f, 0.10f, 6, 12);
                glPopMatrix();
            }
        }

        // Masts and square sails (reduced detail for distance).
        const float mx[3] = {-1.9f, 0.8f, 3.5f};
        const float mh[3] = {4.8f, 5.7f, 4.1f};
        for (int m = 0; m < 3; ++m) {
            glPushMatrix();
            glTranslatef(mx[m], 1.0f + mh[m] * 0.5f, 0.0f);
            setColorLerp(0.30f, 0.21f, 0.12f, 0.18f, 0.16f, 0.13f, dayNightT);
            glScalef(0.18f, mh[m], 0.18f);
            glutSolidCube(1.0);
            glPopMatrix();

            glDisable(GL_LIGHTING);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            float dr = lerp(0.78f, 0.30f, dayNightT);
            float dg = lerp(0.76f, 0.32f, dayNightT);
            float db = lerp(0.70f, 0.36f, dayNightT);
            desaturateRGB(dr, dg, db, 0.38f);
            const float da = lerp(0.58f, 0.48f, dayNightT);
            for (int y = 0; y < 2; ++y) {
                const float yTop = 2.4f + y * 1.55f + m * 0.35f;
                const float zW = 1.25f - m * 0.12f;
                const float xm = mx[m] + 0.06f;
                const float yLo = yTop - 1.20f;
                const float phaseD = gWavePhase * 0.28f + xm * 0.4f + static_cast<float>(y + m * 3) * 0.31f;
                const int dny = 5;
                const int dnz = 8;
                for (int j = 0; j < dny; ++j) {
                    glBegin(GL_TRIANGLE_STRIP);
                    for (int k = 0; k <= dnz; ++k) {
                        const float fk = static_cast<float>(k) / static_cast<float>(dnz);
                        const float zz = lerp(-zW, zW, fk);
                        for (int jr = 0; jr < 2; ++jr) {
                            const float fj = static_cast<float>(j + jr) / static_cast<float>(dny);
                            const float yy = lerp(yLo, yTop, fj);
                            const float disp = sailWrinkle3(
                                fj, fk, fj + fk * 0.4f + static_cast<float>(m) * 0.15f, phaseD, 0.52f);
                            const float bt = fj;
                            glColor4f(lerp(dr * 0.9f, dr * 1.02f, bt), lerp(dg * 0.88f, dg * 1.02f, bt),
                                      lerp(db * 0.86f, db * 1.0f, bt), lerp(da * 0.92f, da, bt));
                            glVertex3f(xm + disp, yy, zz);
                        }
                    }
                    glEnd();
                }
            }
            glDisable(GL_BLEND);
            glEnable(GL_LIGHTING);
        }

        // Very thin rigging hints.
        drawShipRiggingLine(mx[1], 6.2f, 0.0f, 5.6f, 0.8f, 0.0f, dayNightT);
        drawShipRiggingLine(mx[1], 6.2f, 0.0f, -5.6f, 0.8f, 0.0f, dayNightT);

        glPopMatrix();
    }
}

// -----------------------------------------------------------------------------
// Scene drawing: lighthouse
// -----------------------------------------------------------------------------
void drawLighthouse() {
    const float dn = smoothStep(gDayNightBlend);
    GLUquadric* q = gluNewQuadric();
    gluQuadricNormals(q, GLU_SMOOTH);

    glPushMatrix();
    glTranslatef(kLhWorldX, kLhWorldY0, kLhWorldZ);

    // ---- Single cohesive rocky island (wide submerged footprint + domed landmass) ----
    // Submerged / tide zone — reads as one body with the water surface cutting across it.
    glPushMatrix();
    glTranslatef(0.0f, -0.35f, 0.0f);
    setColorLerp(0.16f, 0.15f, 0.14f, 0.09f, 0.09f, 0.11f, dn);
    glScalef(14.0f, 1.25f, 12.0f);
    glutSolidSphere(1.0, 28, 20);
    glPopMatrix();

    // Main dome (the island proper)
    glPushMatrix();
    glTranslatef(0.0f, 1.05f, 0.0f);
    setColorLerp(0.28f, 0.24f, 0.21f, 0.14f, 0.13f, 0.15f, dn);
    glScalef(10.2f, 2.5f, 8.8f);
    glutSolidSphere(1.0, 36, 24);
    glPopMatrix();

    // Asymmetric secondary hump (natural coastline — still one continuous landform)
    glPushMatrix();
    glTranslatef(3.4f, 0.72f, 2.3f);
    setColorLerp(0.25f, 0.22f, 0.19f, 0.13f, 0.12f, 0.14f, dn);
    glScalef(5.0f, 1.65f, 4.6f);
    glutSolidSphere(1.0, 22, 16);
    glPopMatrix();
    glPushMatrix();
    glTranslatef(-3.0f, 0.58f, -2.1f);
    setColorLerp(0.24f, 0.21f, 0.19f, 0.12f, 0.11f, 0.13f, dn);
    glScalef(4.6f, 1.45f, 4.2f);
    glutSolidSphere(1.0, 20, 14);
    glPopMatrix();

    // Flat summit pad where the tower meets the rock (clear “built on an island” read)
    glPushMatrix();
    glTranslatef(0.0f, kLhTowerBaseY - 0.18f, 0.0f);
    glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
    setColorLerp(0.30f, 0.26f, 0.23f, 0.16f, 0.15f, 0.17f, dn);
    gluDisk(q, 0.0f, 3.65f, 36, 1);
    glPopMatrix();

    // A few larger rocks on the shoreline (sat ON the dome, not floating separately)
    for (int i = 0; i < 8; ++i) {
        const float a = (2.0f * kPi * static_cast<float>(i)) / 8.0f + 0.35f;
        const float pr = 4.6f + 0.35f * std::sin(static_cast<float>(i) * 1.1f);
        const float x = std::cos(a) * pr;
        const float z = std::sin(a) * pr;
        const float y = 0.55f + 0.12f * std::sin(a * 2.3f);
        glPushMatrix();
        glTranslatef(x, y, z);
        glRotatef(18.0f * static_cast<float>(i), 0.3f, 1.0f, 0.1f);
        setColorLerp(0.20f, 0.17f, 0.15f, 0.11f, 0.10f, 0.12f, dn);
        glScalef(1.5f + 0.2f * static_cast<float>(i % 3), 1.1f, 1.7f);
        glutSolidCube(1.0);
        glPopMatrix();
    }

    // ---- White masonry tower (tapered stacked cylinders + subtle banding) ----
    const float r0[4] = {2.75f, 2.52f, 2.30f, 2.08f};
    glPushMatrix();
    glTranslatef(0.0f, kLhTowerBaseY, 0.0f);
    glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
    for (int seg = 0; seg < 3; ++seg) {
        const float h = kLhTowerSegH[seg];
        const float noise = 0.02f * std::sin(static_cast<float>(seg) * 2.1f + gSceneTimeSec * 0.01f);
        setColorLerp(
            0.94f + noise, 0.93f + noise, 0.90f + noise,
            0.68f + noise, 0.69f + noise, 0.74f + noise,
            dn
        );
        gluCylinder(q, r0[seg], r0[seg + 1], h, 42, 10);
        // Mortar line ring at segment joint
        glPushMatrix();
        glTranslatef(0.0f, 0.0f, h - 0.04f);
        setColorLerp(0.78f, 0.76f, 0.72f, 0.52f, 0.53f, 0.58f, dn);
        gluCylinder(q, r0[seg + 1] + 0.02f, r0[seg + 1] + 0.02f, 0.06f, 42, 1);
        glPopMatrix();
        glTranslatef(0.0f, 0.0f, h);
    }
    glPopMatrix();

    // Small dark door (foot of tower)
    glPushMatrix();
    glTranslatef(2.05f, kLhTowerBaseY + 1.0f, 0.0f);
    glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
    setColorLerp(0.12f, 0.11f, 0.10f, 0.08f, 0.08f, 0.09f, dn);
    glScalef(0.85f, 2.0f, 0.22f);
    glutSolidCube(1.0);
    glPopMatrix();

    // Tower windows (small rectangular recesses — reference photo)
    for (int row = 0; row < 4; ++row) {
        for (int w = 0; w < 3; ++w) {
            const float y = kLhTowerBaseY + 2.2f + static_cast<float>(row) * 3.0f;
            const float ang = (40.0f + static_cast<float>(w) * 52.0f + static_cast<float>(row) * 13.0f) * (kPi / 180.0f);
            const float rad = 2.22f - static_cast<float>(row) * 0.07f;
            glPushMatrix();
            glTranslatef(std::cos(ang) * rad, y, std::sin(ang) * rad);
            glRotatef(ang * (180.0f / kPi), 0.0f, 1.0f, 0.0f);
            setColorLerp(0.10f, 0.10f, 0.11f, 0.22f, 0.26f, 0.34f, dn);
            glScalef(0.14f, 0.55f, 0.12f);
            glutSolidCube(1.0);
            glPopMatrix();
        }
    }

    // ---- Black metal gallery deck (ring under lantern) ----
    glPushMatrix();
    glTranslatef(0.0f, kLhGalleryY, 0.0f);
    glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
    setColorLerp(0.07f, 0.07f, 0.08f, 0.12f, 0.12f, 0.14f, dn);
    gluDisk(q, 2.05f, 2.95f, 40, 1);
    glPopMatrix();

    // Deck thickness
    glPushMatrix();
    glTranslatef(0.0f, kLhGalleryY + 0.06f, 0.0f);
    setColorLerp(0.09f, 0.09f, 0.10f, 0.14f, 0.14f, 0.16f, dn);
    glScalef(5.7f, 0.12f, 5.7f);
    glutSolidCube(1.0);
    glPopMatrix();

    // Gallery railing posts + top rail torus
    const int postCount = 20;
    for (int i = 0; i < postCount; ++i) {
        const float a = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(postCount);
        glPushMatrix();
        glTranslatef(std::cos(a) * 2.84f, kLhGalleryY + 0.18f, std::sin(a) * 2.84f);
        setColorLerp(0.05f, 0.05f, 0.06f, 0.10f, 0.10f, 0.11f, dn);
        glScalef(0.10f, 0.52f, 0.10f);
        glutSolidCube(1.0);
        glPopMatrix();
    }
    glPushMatrix();
    glTranslatef(0.0f, kLhGalleryY + 0.50f, 0.0f);
    glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    setColorLerp(0.06f, 0.06f, 0.07f, 0.11f, 0.11f, 0.12f, dn);
    glutSolidTorus(0.045f, 2.78f, 8, 28);
    glPopMatrix();

    // ---- Black lantern housing (metal) with glass panels ----
    glPushMatrix();
    glTranslatef(0.0f, kLhLanternCenterY, 0.0f);
    setColorLerp(0.05f, 0.05f, 0.055f, 0.08f, 0.08f, 0.09f, dn);
    glScalef(2.2f, 1.24f, 2.2f);
    glutSolidCube(1.0);
    glPopMatrix();

    // Glass strips (dark tinted)
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < 4; ++i) {
        glPushMatrix();
        glTranslatef(0.0f, kLhLanternCenterY, 0.0f);
        glRotatef(static_cast<float>(i) * 90.0f, 0.0f, 1.0f, 0.0f);
        glTranslatef(1.09f, 0.0f, 0.0f);
        glColor4f(0.35f, 0.45f, 0.55f, lerp(0.42f, 0.55f, dn));
        glScalef(0.08f, 1.1f, 1.5f);
        glutSolidCube(1.0);
        glPopMatrix();
    }
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);

    // Red lamp volume (core glow — matches reference beacon)
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glPushMatrix();
    glTranslatef(0.0f, kLhLanternCenterY, 0.0f);
    glColor4f(1.0f, 0.18f, 0.08f, 0.55f + 0.35f * (1.0f - dn));
    glutSolidSphere(0.38, 18, 14);
    glPopMatrix();
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);

    // ---- Dark conical roof + small spire ----
    glPushMatrix();
    glTranslatef(0.0f, kLhRoofBaseY, 0.0f);
    setColorLerp(0.04f, 0.04f, 0.045f, 0.09f, 0.09f, 0.10f, dn);
    glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
    glutSolidCone(1.48, 2.35, 26, 10);
    glPopMatrix();
    glPushMatrix();
    glTranslatef(0.0f, kLhRoofBaseY + 2.35f + 0.12f, 0.0f);
    setColorLerp(0.03f, 0.03f, 0.035f, 0.08f, 0.08f, 0.09f, dn);
    glutSolidSphere(0.18, 14, 12);
    glPopMatrix();

    gluDeleteQuadric(q);
    glPopMatrix();
}

// -----------------------------------------------------------------------------
// Scene drawing: ship with hierarchical parts
// -----------------------------------------------------------------------------
void drawShip() {
    const float t = smoothStep(gDayNightBlend);
    const float shipX = 19.0f + gShipDriftX;
    const float shipZ = 10.0f;
    const float shipY = waveHeightAtWorld(shipX, shipZ) + 1.2f;
    const float bob = std::sin(gWavePhase * 0.55f) * 0.26f;

    glPushMatrix();
    glTranslatef(shipX, shipY + bob, shipZ);
    glRotatef(-18.0f, 0.0f, 1.0f, 0.0f);
    glRotatef(std::sin(gWavePhase * 0.7f) * 4.2f, 0.0f, 0.0f, 1.0f);
    glRotatef(std::sin(gWavePhase * 0.5f + 0.7f) * 2.8f, 1.0f, 0.0f, 0.0f);

    // Tall ship / galleon (reference: dark hull, gold waist band, square-rigged masts).
    const float dcy = 1.38f;
    float p1x, p1y, p1z, p2x, p2y, p2z, p3x, p3y, p3z, p4x, p4y, p4z;
    float p5x, p5y, p5z, p6x, p6y, p6z, p7x, p7y, p7z, p8x, p8y, p8z;
    hullWobbleAt(-7.0f, -0.35f, 2.25f, &p1x, &p1y, &p1z);
    hullWobbleAt(6.8f, -0.15f, 1.95f, &p2x, &p2y, &p2z);
    hullWobbleAt(5.8f, dcy, 1.75f, &p3x, &p3y, &p3z);
    hullWobbleAt(-6.2f, dcy + 0.1f, 1.95f, &p4x, &p4y, &p4z);
    hullWobbleAt(-7.0f, -0.35f, -2.25f, &p5x, &p5y, &p5z);
    hullWobbleAt(-6.2f, dcy + 0.1f, -1.95f, &p6x, &p6y, &p6z);
    hullWobbleAt(5.8f, dcy, -1.75f, &p7x, &p7y, &p7z);
    hullWobbleAt(6.8f, -0.15f, -1.95f, &p8x, &p8y, &p8z);

    setColorLerp(0.09f, 0.08f, 0.07f, 0.05f, 0.05f, 0.06f, t);
    glBegin(GL_QUADS);
    glNormalFromQuad3(p1x, p1y, p1z, p2x, p2y, p2z, p4x, p4y, p4z);
    glVertex3f(p1x, p1y, p1z);
    glVertex3f(p2x, p2y, p2z);
    glVertex3f(p3x, p3y, p3z);
    glVertex3f(p4x, p4y, p4z);
    glNormalFromQuad3(p5x, p5y, p5z, p8x, p8y, p8z, p6x, p6y, p6z);
    glVertex3f(p5x, p5y, p5z);
    glVertex3f(p6x, p6y, p6z);
    glVertex3f(p7x, p7y, p7z);
    glVertex3f(p8x, p8y, p8z);
    glNormalFromQuad3(p6x, p6y, p6z, p4x, p4y, p4z, p7x, p7y, p7z);
    glVertex3f(p6x, p6y, p6z);
    glVertex3f(p4x, p4y, p4z);
    glVertex3f(p3x, p3y, p3z);
    glVertex3f(p7x, p7y, p7z);
    glNormalFromQuad3(p5x, p5y, p5z, p8x, p8y, p8z, p1x, p1y, p1z);
    glVertex3f(p5x, p5y, p5z);
    glVertex3f(p8x, p8y, p8z);
    glVertex3f(p2x, p2y, p2z);
    glVertex3f(p1x, p1y, p1z);
    glEnd();

    // Subtle horizontal strakes / weather strips on hull sides (fake planking / water staining).
    {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        const int hullBands = 11;
        const float hr = lerp(0.09f, 0.05f, t);
        const float hg = lerp(0.08f, 0.05f, t);
        const float hb = lerp(0.07f, 0.06f, t);
        for (int b = 0; b < hullBands; ++b) {
            const float h0 = static_cast<float>(b) / static_cast<float>(hullBands);
            const float h1 = static_cast<float>(b + 1) / static_cast<float>(hullBands);
            const float shade = (b % 2 == 0) ? 1.05f : 0.89f;
            glColor3f(hr * shade, hg * shade, hb * shade);
            for (int zi = 0; zi < 2; ++zi) {
                const bool port = (zi == 0);
                float xL0, yL0, zL0, xR0, yR0, zR0, xL1, yL1, zL1, xR1, yR1, zR1;
                if (port) {
                    xL0 = lerp(p1x, p4x, h0);
                    yL0 = lerp(p1y, p4y, h0);
                    zL0 = lerp(p1z, p4z, h0);
                    xR0 = lerp(p2x, p3x, h0);
                    yR0 = lerp(p2y, p3y, h0);
                    zR0 = lerp(p2z, p3z, h0);
                    xL1 = lerp(p1x, p4x, h1);
                    yL1 = lerp(p1y, p4y, h1);
                    zL1 = lerp(p1z, p4z, h1);
                    xR1 = lerp(p2x, p3x, h1);
                    yR1 = lerp(p2y, p3y, h1);
                    zR1 = lerp(p2z, p3z, h1);
                } else {
                    xL0 = lerp(p5x, p6x, h0);
                    yL0 = lerp(p5y, p6y, h0);
                    zL0 = lerp(p5z, p6z, h0);
                    xR0 = lerp(p8x, p7x, h0);
                    yR0 = lerp(p8y, p7y, h0);
                    zR0 = lerp(p8z, p7z, h0);
                    xL1 = lerp(p5x, p6x, h1);
                    yL1 = lerp(p5y, p6y, h1);
                    zL1 = lerp(p5z, p6z, h1);
                    xR1 = lerp(p8x, p7x, h1);
                    yR1 = lerp(p8y, p7y, h1);
                    zR1 = lerp(p8z, p7z, h1);
                }
                glBegin(GL_QUADS);
                glNormalFromQuad3(xL0, yL0, zL0, xR0, yR0, zR0, xL1, yL1, zL1);
                glVertex3f(xL0, yL0, zL0);
                glVertex3f(xR0, yR0, zR0);
                glVertex3f(xR1, yR1, zR1);
                glVertex3f(xL1, yL1, zL1);
                glEnd();
            }
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Gold “gun port” rings along both sides (decorative band like the reference photo).
    for (float gx = -4.8f; gx < 5.5f; gx += 0.68f) {
        for (int si = 0; si < 2; ++si) {
            const float side = (si == 0) ? -1.0f : 1.0f;
            glPushMatrix();
            glTranslatef(gx, 0.42f, side * 2.18f);
            glRotatef(side * 90.0f, 0.0f, 1.0f, 0.0f);
            glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
            setColorLerp(0.90f, 0.72f, 0.18f, 0.55f, 0.45f, 0.15f, t);
            glutSolidTorus(0.028f, 0.112f, 8, 18);
            glPopMatrix();
        }
    }

    // High squared stern (sterncastle).
    glPushMatrix();
    glTranslatef(-6.4f, 2.15f, 0.0f);
    glRotatef(0.42f, 0.06f, 1.0f, 0.03f);
    glTranslatef(0.014f, -0.011f, 0.007f);
    setColorLerp(0.09f, 0.07f, 0.06f, 0.06f, 0.055f, 0.055f, t);
    glScalef(2.35f, 2.25f, 2.35f);
    glutSolidCube(1.0);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(5.4f, 1.45f, 0.0f);
    glRotatef(-0.38f, 0.04f, 1.0f, -0.02f);
    glTranslatef(-0.012f, 0.009f, -0.005f);
    setColorLerp(0.10f, 0.08f, 0.07f, 0.065f, 0.055f, 0.055f, t);
    glScalef(1.8f, 1.1f, 2.0f);
    glutSolidCube(1.0);
    glPopMatrix();

    glPushMatrix();
    setColorLerp(0.08f, 0.07f, 0.06f, 0.055f, 0.05f, 0.05f, t);
    glTranslatef(7.2f + 0.028f, 0.55f - 0.015f, 0.011f);
    glRotatef(90.0f, 0.0f, 0.0f, 1.0f);
    glRotatef(0.55f, 0.0f, 0.0f, 1.0f);
    glutSolidCone(0.85f, 2.0f, 14, 8);
    glPopMatrix();

    glPushMatrix();
    setColorLerp(0.48f, 0.32f, 0.18f, 0.28f, 0.22f, 0.18f, t);
    glTranslatef(-0.2f + 0.013f, dcy + 0.08f - 0.006f, -0.007f);
    glRotatef(0.28f, 0.0f, 0.0f, 1.0f);
    glScalef(12.0f, 0.22f, 3.15f);
    glutSolidCube(1.0);
    glPopMatrix();

    setColorLerp(0.28f, 0.20f, 0.11f, 0.20f, 0.17f, 0.14f, t);
    for (int side = -1; side <= 1; side += 2) {
        glPushMatrix();
        glTranslatef(0.0f, dcy + 0.42f + 0.004f * static_cast<float>(side), side * 1.72f);
        glRotatef(0.22f * static_cast<float>(side), 1.0f, 0.0f, 0.0f);
        glScalef(11.0f, 0.11f, 0.11f);
        glutSolidCube(1.0);
        glPopMatrix();
    }

    const float foreYards[]  = {3.35f, 4.75f, 6.15f};
    const float mainYards[]  = {3.1f, 4.55f, 6.05f, 7.55f};
    const float mizzenYards[] = {2.85f, 4.2f, 5.55f};
    drawMastWithSquares(-2.35f, 4.35f, 3, foreYards, 2.45f, t);
    drawMastWithSquares(0.95f, 5.15f, 4, mainYards, 2.75f, t);
    drawMastWithSquares(4.15f, 3.65f, 3, mizzenYards, 2.15f, t);

    const float fmx[] = {-2.35f, 0.95f, 4.15f};
    const float fmy[] = {dcy + 9.0f, dcy + 10.6f, dcy + 7.8f};
    for (int m = 0; m < 3; ++m) {
        drawShipRiggingLine(fmx[m], fmy[m], 0.0f, 7.8f, dcy + 0.2f, 0.0f, t);
        drawShipRiggingLine(fmx[m], fmy[m], 0.0f, -6.8f, dcy + 0.2f, 0.0f, t);
        drawShipRiggingLine(fmx[m], fmy[m], 0.0f, fmx[m], dcy + 0.15f,  2.35f, t);
        drawShipRiggingLine(fmx[m], fmy[m], 0.0f, fmx[m], dcy + 0.15f, -2.35f, t);
        drawShipRiggingLine(fmx[m], fmy[m], 0.0f, -1.5f, dcy + 0.15f,  2.1f, t);
        drawShipRiggingLine(fmx[m], fmy[m], 0.0f, -1.5f, dcy + 0.15f, -2.1f, t);
    }

    glDisable(GL_LIGHTING);
    glPushMatrix();
    glTranslatef(0.95f, dcy + 10.9f, 0.0f);
    glRotatef(std::sin(gSceneTimeSec * 0.7f) * 8.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_TRIANGLES);
    glColor3f(0.85f, 0.12f, 0.10f);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glVertex3f(0.0f, 1.0f, 0.0f);
    glVertex3f(0.45f, 0.5f, 0.0f);
    glColor3f(0.92f, 0.78f, 0.15f);
    glVertex3f(0.0f, 1.0f, 0.0f);
    glVertex3f(0.0f, 2.0f, 0.0f);
    glVertex3f(0.5f, 1.5f, 0.0f);
    glColor3f(0.95f, 0.95f, 0.92f);
    glVertex3f(0.0f, 2.0f, 0.0f);
    glVertex3f(0.0f, 2.7f, 0.0f);
    glVertex3f(0.42f, 2.35f, 0.0f);
    glEnd();
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-1.2f, dcy + 6.4f, 2.35f);
    glRotatef(-12.0f, 0.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS);
    glColor3f(0.92f, 0.82f, 0.18f);
    glVertex3f(0.0f, -0.35f, 0.0f);
    glVertex3f(2.6f, -0.35f, 0.0f);
    glVertex3f(2.6f, 0.0f, 0.0f);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glColor3f(0.18f, 0.62f, 0.28f);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glVertex3f(2.6f, 0.0f, 0.0f);
    glVertex3f(2.6f, 0.35f, 0.0f);
    glVertex3f(0.0f, 0.35f, 0.0f);
    glColor3f(0.78f, 0.16f, 0.12f);
    glVertex3f(0.0f, 0.35f, 0.0f);
    glVertex3f(2.6f, 0.35f, 0.0f);
    glVertex3f(2.6f, 0.7f, 0.0f);
    glVertex3f(0.0f, 0.7f, 0.0f);
    glEnd();
    glPopMatrix();
    glEnable(GL_LIGHTING);

    glPushMatrix();
    glTranslatef(-5.8f, dcy + 0.55f, 0.0f);
    glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
    setColorLerp(0.40f, 0.26f, 0.14f, 0.28f, 0.24f, 0.20f, t);
    glutWireTorus(0.055f, 0.38f, 10, 20);
    glPopMatrix();

    drawCrew(t);

    glPopMatrix();
}

// -----------------------------------------------------------------------------
// Small modern sloop (reference: single mast, triangular mainsail + jib).
// -----------------------------------------------------------------------------
void drawSloop(float dayNightT) {
    const float t = dayNightT;
    const float x = 5.8f + gShipDriftX * 0.42f;
    const float z = 12.2f;
    const float y = waveHeightAtWorld(x, z) + 0.48f;
    const float bob = std::sin(gWavePhase * 0.68f) * 0.11f;

    glPushMatrix();
    glTranslatef(x, y + bob, z);
    glRotatef(42.0f, 0.0f, 1.0f, 0.0f);
    glRotatef(std::sin(gWavePhase * 0.55f + 1.2f) * 2.5f, 0.0f, 0.0f, 1.0f);

    // Low dark hull + faint side bands (matches tall-ship planking treatment).
    glPushMatrix();
    setColorLerp(0.12f, 0.11f, 0.11f, 0.08f, 0.08f, 0.09f, t);
    glRotatef(0.32f, 0.02f, 1.0f, 0.05f);
    glScalef(3.618f, 0.614f, 1.246f);
    glutSolidCube(1.0);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    const int sloBands = 6;
    const float br0 = lerp(0.12f, 0.08f, t);
    const float bg0 = lerp(0.11f, 0.08f, t);
    const float bb0 = lerp(0.11f, 0.09f, t);
    for (int b = 0; b < sloBands; ++b) {
        const float v0 = static_cast<float>(b) / static_cast<float>(sloBands);
        const float v1 = static_cast<float>(b + 1) / static_cast<float>(sloBands);
        const float y0 = lerp(-0.31f, 0.31f, v0);
        const float y1 = lerp(-0.31f, 0.31f, v1);
        const float sh = (b % 2 == 0) ? 1.04f : 0.91f;
        glColor3f(br0 * sh, bg0 * sh, bb0 * sh);
        for (int zi = 0; zi < 2; ++zi) {
            const float zf = (zi == 0) ? 1.0f : -1.0f;
            const float zz = zf * (0.628f + 0.006f * std::sin(static_cast<float>(b) * 0.85f + static_cast<float>(zi)));
            glBegin(GL_QUADS);
            glNormal3f(0.0f, 0.0f, zf);
            glVertex3f(-1.78f + 0.005f * std::sin(static_cast<float>(b)), y0, zz);
            glVertex3f(1.78f + 0.004f * std::sin(static_cast<float>(b) + 1.0f), y0, zz);
            glVertex3f(1.78f + 0.004f * std::sin(static_cast<float>(b + 1) + 1.0f), y1, zz);
            glVertex3f(-1.78f + 0.005f * std::sin(static_cast<float>(b + 1)), y1, zz);
            glEnd();
        }
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPopMatrix();

    glPushMatrix();
    setColorLerp(0.34f, 0.24f, 0.14f, 0.22f, 0.19f, 0.16f, t);
    glTranslatef(0.15f, 0.55f, 0.0f);
    glScalef(0.14f, 3.8f, 0.14f);
    glutSolidCube(1.0);
    glPopMatrix();

    glPushMatrix();
    setColorLerp(0.34f, 0.24f, 0.14f, 0.22f, 0.19f, 0.16f, t);
    glTranslatef(0.1f, 1.65f, 0.0f);
    glScalef(2.9f, 0.1f, 0.1f);
    glutSolidCube(1.0);
    glPopMatrix();

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float pr = lerp(0.80f, 0.62f, t);
    float pg = lerp(0.78f, 0.64f, t);
    float pb = lerp(0.70f, 0.66f, t);
    desaturateRGB(pr, pg, pb, 0.42f);
    const float pa = lerp(0.72f, 0.66f, t);
    const float phS = gWavePhase * 0.36f + 0.55f;
    drawWrinkledTriSail(0.12f, 0.65f, 0.12f, 2.35f, 2.55f, 1.55f, 9, phS, pr, pg, pb, pa, 0.9f, 0.88f, 0.86f,
                        0.94f, 1.04f, 1.04f, 1.02f, 1.0f);
    drawWrinkledTriSail(-0.6f, 1.85f, 0.1f, 0.7f, 0.1f, 2.0f, 8, phS + 2.05f, pr, pg, pb, pa, 0.88f, 0.86f,
                        0.84f, 0.92f, 1.02f, 1.02f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);

    drawShipRiggingLine(0.12f, 2.35f, 0.0f, 2.0f, 0.55f, 0.4f, t);
    drawShipRiggingLine(0.12f, 2.35f, 0.0f, -0.5f, 0.55f, 0.0f, t);

    // Tiny figures for scale
    drawCrewMember(0.3f, 0.58f, 0.35f, t, false);
    drawCrewMember(-0.4f, 0.58f, -0.25f, t, false);

    glPopMatrix();
}

// -----------------------------------------------------------------------------
// Visible lighthouse cone mesh (night only)
// -----------------------------------------------------------------------------
void drawLighthouseBeamCone() {
    const float intensity = smoothStep(gDayNightBlend);
    if (intensity < 0.02f) {
        return;
    }

    // Use blending to simulate a translucent light beam.
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);

    glPushMatrix();
    glTranslatef(kLhWorldX, kLhLanternWorldY, kLhWorldZ);
    glRotatef(-gBeamAngleDeg, 0.0f, 1.0f, 0.0f);
    glRotatef(96.0f, 0.0f, 0.0f, 1.0f);

    // Warm beam tinted slightly coral (reference mixes red lantern + sea spray).
    glColor4f(1.0f, 0.88f, 0.72f, 0.10f * intensity);
    glutSolidCone(2.0, 56.0, 56, 8);
    // Outer soft halo cone
    glColor4f(0.92f, 0.94f, 1.00f, 0.05f * intensity);
    glutSolidCone(3.4, 60.0, 56, 8);

    glPopMatrix();

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

// -----------------------------------------------------------------------------
// Core render routine
// -----------------------------------------------------------------------------
void display() {
    const float t = smoothStep(gDayNightBlend);

    float clearR = lerp(0.58f, 0.08f, t);
    float clearG = lerp(0.76f, 0.12f, t);
    float clearB = lerp(0.90f, 0.22f, t);
    desaturateRGB(clearR, clearG, clearB, 0.32f);
    filmicRGB(clearR, clearG, clearB, 1.04f);
    glClearColor(clearR, clearG, clearB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    // Automatic cinematic orbit/truck camera.
    const float cx = 1.0f + std::sin(gCameraPhase * 0.40f) * 2.2f;
    const float cy = 2.7f + 0.6f * t + std::sin(gCameraPhase * 0.65f) * 0.55f;
    const float cz = 2.0f + std::cos(gCameraPhase * 0.27f) * 1.8f;
    const float yaw = 0.10f + std::sin(gCameraPhase * 0.22f) * 0.25f;
    const float elev = 0.20f + std::sin(gCameraPhase * 0.41f) * 0.05f;
    const float camDist = 35.5f + std::sin(gCameraPhase * 0.18f) * 2.2f;
    const float ch = std::cos(elev);
    const float ex = cx + camDist * ch * std::sin(yaw);
    const float ey = cy + camDist * std::sin(elev);
    const float ez = cz + camDist * ch * std::cos(yaw);
    gluLookAt(ex, ey, ez, cx, cy, cz, 0.0f, 1.0f, 0.0f);

    setupLighting();
    drawSkybox();
    drawOcean();
    drawDistantShips(t);
    drawLighthouse();
    drawShip();
    drawSloop(t);
    drawBirds();
    drawLighthouseBeamCone();

    glutSwapBuffers();
}

void reshape(int w, int h) {
    if (h == 0) h = 1;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, static_cast<double>(w) / static_cast<double>(h), 0.1, 300.0);
    glMatrixMode(GL_MODELVIEW);
}

void idle() {
    const float now = 0.001f * static_cast<float>(glutGet(GLUT_ELAPSED_TIME));
    float dt = now - gLastFrameTimeSec;
    gLastFrameTimeSec = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;  // Clamp for stability on frame hiccups.

    gSceneTimeSec += dt;
    gCameraPhase += dt;
    gWavePhase += dt * kWaveSpeed * 3.0f;

    // Main ship slow horizontal sway (movement over the wavy surface; rocking stays inside drawShip).
    gShipDriftX = 13.0f * std::sin(gSceneTimeSec * 0.080f);

    // Smoothly approach target day/night state.
    const float target = gTargetNight ? 1.0f : 0.0f;
    const float step = dt / kTransitionDurationSec;
    if (gDayNightBlend < target) {
        gDayNightBlend = clamp01(gDayNightBlend + step);
    } else if (gDayNightBlend > target) {
        gDayNightBlend = clamp01(gDayNightBlend - step);
    }

    // Lighthouse beam rotation is meaningful at night (lab spec).
    const float nightAmt = smoothStep(gDayNightBlend);
    if (nightAmt > 0.12f) {
        gBeamAngleDeg += kBeamRotationSpeed * dt * nightAmt;
        if (gBeamAngleDeg > 360.0f) gBeamAngleDeg -= 360.0f;
    }

    glutPostRedisplay();
}

void timerToggle(int value) {
    (void)value;
    gTargetNight = !gTargetNight;
    glutTimerFunc(static_cast<unsigned int>(kCycleIntervalSec * 1000.0f), timerToggle, 0);
}

void initGL() {
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_LIGHT2);
    glEnable(GL_LIGHT3);
    glEnable(GL_FOG);
    glEnable(GL_NORMALIZE);
    glShadeModel(GL_SMOOTH);

    // Material setup allows glColor to affect ambient and diffuse response.
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    const GLfloat spec[4] = {0.42f, 0.42f, 0.45f, 1.0f};
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 44.0f);

    gLastFrameTimeSec = 0.001f * static_cast<float>(glutGet(GLUT_ELAPSED_TIME));
}

int main(int argc, char** argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(kWindowWidth, kWindowHeight);
    glutCreateWindow("Maritime Day/Night Simulation - OpenGL GLUT");

    initGL();

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    glutTimerFunc(static_cast<unsigned int>(kCycleIntervalSec * 1000.0f), timerToggle, 0);

    glutMainLoop();
    return EXIT_SUCCESS;
}
