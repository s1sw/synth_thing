// Polysynth
// =========

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_render.h>
#include <SDL2/SDL_scancode.h>
#include <unordered_map>
#include <SDL2/SDL_audio.h>
#ifdef _WIN32
#define _USE_MATH_DEFINES
#endif
#include <math.h>
#include <SDL2/SDL_ttf.h>
#ifdef _WIN32
#include <RtMidi.h>
#else
#include <rtmidi/RtMidi.h>
#endif
#include <thread>



// Types
// =====
enum Waveform {
    W_Sine,
    W_Saw,
    W_Square,
    W_Triangle,
    W_Count
};

enum class OctaveMode {
    Single,
    Double,
    Triple,
    Quadruple
};

struct PolyphonicVoice {
    int note;
    bool finishedPlaying;
    double freq;
    double volume;
    double pressTime;
    double releaseTime;
};

struct ADSRCurve {
    double attackTime = 0.01;
    double releaseTime = 0.1;
    double sustainAmount = 0.8;
    double decayTime = 0.65;
};

double min(double a, double b) {
    return a > b ? b : a;
}

double max(double a, double b) {
    return a > b ? a : b;
}

double clamp(double val, double mi, double ma) {
    return max(min(val, ma), mi);
}

double lerp(double from, double to, double amt) {
    return from + ((to - from) * amt);
}

int roundToInt(float val) {
    return (int)(val < 0.0f ? val - 0.5f : val + 0.5f);
}

// Gets the ADS attenuation based on time since note press.
double getADSAttenuation(ADSRCurve curve, double time) {
    double decayStart = curve.attackTime;

    double decayProgress = clamp((time - decayStart) / curve.decayTime, 0.0, 1.0); 
    double decayed = lerp(1.0, curve.sustainAmount, decayProgress);

    return clamp(time / curve.attackTime, 0.0, 1.0) * decayed;
}

double getRAttenuation(ADSRCurve curve, double time) {
    return clamp(1.0 - (time / curve.releaseTime), 0.0, 1.0) * curve.sustainAmount;
}

const static int NUM_VOICES = 16;
PolyphonicVoice voices[NUM_VOICES];

// Unison settings
bool unisonDetune = false;
int unisonOrder = 16;
float unisonDetuneAmount = 0.0025f;

// Various other synth settings
float crushBits = 16.0f;
bool enableBitcrush = false;
//bool enableOctaveDoubling = false;
OctaveMode octaveMode = OctaveMode::Single;
bool goofyUnison = false;
bool enableCompressor = false;
float lpQ = 0.5f;
bool lpEnabled = false;

// DSP timer. Updated upon buffer completion 
double timeAccumulator = 0.0;
bool hasClipped = false;
float maxAmplitude = 0.0f;
float volume = 1.0f;
double pitchBendAmt = 0.0;

Waveform currWaveFunc = W_Sine;

float bitcrush(float value, float bits) {
    float distinctValues = powf(2.0f, bits);

    float crushedVal = (value + 1.0f) * 0.5f;
    int intVal = roundToInt(crushedVal * distinctValues);

    return ((float)intVal) / distinctValues;
}

float squarify(float value) {
    return value > 0.0f ? 1.0f : -1.0f;
}

// Wave functions
// ==============

typedef float (*WaveFunc)(double, double);

float saw(double t, double freq) {
    return (fmod(t * freq, 1.0) * 2.0) - 1.0;
}

float sine(double t, double freq) {
    return sin(t * M_PI * freq * 2.0);
}

float square(double t, double freq) {
    return sine(t, freq) > 0.0f ? 1.0f : -1.0f;
}

float triangle(double t, double freq) {
    return abs(saw(t, freq)) * 2.0f - 1.0f;
}

// Maps waveform enum to wave function
WaveFunc waveFuncs[W_Count] = {
    sine,
    saw,
    square,
    triangle
};

// Utilities
// ========

// MIDI pitch to frequency
float pitch(float p) {
    return powf(1.059460646483f, p - 69.0f) * 440.0f;
}

// Frequency offset for a given voice
float getDetune(float voiceIdx, float detune) {
    float perVoiceDetune = detune / unisonOrder;

    // This sounds really cool. It's obviously wrong,
    // but it might be useful later on as an effect!
    if (goofyUnison)
        return (voiceIdx - (unisonOrder / 2)) * perVoiceDetune;

    //return (voiceIdx - (unisonOrder / 2)) * detune;
    return voiceIdx * (sinf(voiceIdx / unisonOrder) * detune);
}

float getUnisonVoicePan(float voiceIdx) {
    return (voiceIdx / unisonOrder) * 2.0f - 1.0f;
}

float panToLVol(float pan) {
    return pan < 0.0f ? 1.0f : 1.0f - pan;
}

float panToRVol(float pan) {
    return pan > 0.0f ? 1.0f : 1.0f + pan;
}

// Performs unison detuning on a given wave function
void doUnisonDetune(float& lOut, float& rOut, double t, double freq, WaveFunc waveFunc) {
    for (int i = 0; i < unisonOrder; i++) {
        // For the cool effect mentioned above, should be unisonDetuneAmount / unisonOrder
        float voiceSample;
        if (!goofyUnison)
            voiceSample = waveFunc(t, freq + (freq * getDetune(i, unisonDetuneAmount)));
        else
            voiceSample = waveFunc(t, freq + (freq * getDetune(i, unisonDetuneAmount / unisonOrder)));

        float pan = getUnisonVoicePan(i);
        lOut += voiceSample * panToLVol(pan);
        rOut += voiceSample * panToRVol(pan);
    }

    lOut /= (float)unisonOrder;
    rOut /= (float)unisonOrder;
}


// Polyphonic voice utilities
// ==========================
int getFreeVoiceIdx() {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].finishedPlaying)
            return i;
    }

    return 0;
}

int getVoiceWithNote(int note) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].note == note && voices[i].volume > 0.0)
            return i;
    }
    
    return -1;
}

bool noteAlreadyDown(int note) {
    for (int i = 0; i < NUM_VOICES; i++) {
        if (voices[i].volume > 0.0 && voices[i].note == note)
            return true;
    }

    return false;
}

float lowpass(float& accum, float val, float q) {
    accum = accum - (q * (accum - val));
    return accum;
}

float compressor(float& accum, float val, float magic) {
    // Figure out the peak volume
    // Maximise it
    float peak = abs(lowpass(val, accum, magic));

    return (1.0 - peak) * val;
}

// Core synth function!
// Generates a pair of audio samples for a given voice index.
float lLpAccum = 0.0f;
float rLpAccum = 0.0f;
void getVoiceSample(float& lOut, float& rOut, int voiceIdx, double sampleTime) {
    auto& v = voices[voiceIdx];
    double freq = pitch(v.note + pitchBendAmt);

    lOut = 0.0f;
    rOut = 0.0f;
    if (v.finishedPlaying) {
        return;
    }

    double waveSamp = 0.0;

    if (unisonDetune) {
        doUnisonDetune(lOut, rOut, sampleTime, freq, waveFuncs[currWaveFunc]);
    } else {
        lOut = waveFuncs[currWaveFunc](sampleTime,  freq);
        rOut = lOut;
    }

    double attenuation = 1.0;
    ADSRCurve curve;

    if (v.volume > 0.25) {
        attenuation = getADSAttenuation(curve, sampleTime - v.pressTime);
    } else {
        attenuation = getRAttenuation(curve, sampleTime - v.releaseTime);
        if (attenuation == 0.0 || sampleTime > v.releaseTime + curve.releaseTime) {
            v.finishedPlaying = true;
        }
    }

    lOut *= attenuation;
    rOut *= attenuation;

    lOut *= volume;
    rOut *= volume;

    if (enableBitcrush) {
        lOut = bitcrush(lOut, crushBits);
        rOut = bitcrush(rOut, crushBits);
    }

    if (lpEnabled) {
        lOut = lowpass(lLpAccum, lOut, lpQ);
        rOut = lowpass(rLpAccum, rOut, lpQ);
    }

    if (enableCompressor) {
        lOut = compressor(lLpAccum, lOut, 0.05);
        rOut = compressor(rLpAccum, rOut, 0.05);
    }
}

int currentSampleRate = 44100;
int bufSize = 512;

float lastBufferL[1024];
float lastBufferR[1024];

int nChannels = 2;
void audioCallback(void*, Uint8* data, int len) {
    float* stream = (float*)data;
    int sampleLen = len / sizeof(float);
    hasClipped = false;
    maxAmplitude = 0.0f;

    for (int i = 0; i < sampleLen; i += nChannels) {
        double sampleTime = (i / nChannels / (double)currentSampleRate) + timeAccumulator;
        stream[i] = 0.0;
        stream[i + 1] = 0.0;

        for (int j = 0; j < NUM_VOICES; j++) {
            // oversampling

            float l, r;
            getVoiceSample(l, r, j, sampleTime);
            l *= 0.25f;
            r *= 0.25f;
            stream[i] += l;
            stream[i + 1] += r;
        } 

        lastBufferL[i / nChannels] = stream[i];
        lastBufferR[i / nChannels] = stream[i + 1];

        if (stream[i] > 1.0 || stream[i] < -1.0) {
            hasClipped = true;
        }

        maxAmplitude = max(abs(stream[i]), maxAmplitude);
    }

    timeAccumulator += sampleLen / nChannels / (double)currentSampleRate;
}

const std::unordered_map<SDL_Scancode, int> freqs = {
    { SDL_SCANCODE_Z, 48 },
    { SDL_SCANCODE_S, 49 },
    { SDL_SCANCODE_X, 50 },
    { SDL_SCANCODE_D, 51 },
    { SDL_SCANCODE_C, 52 },
    { SDL_SCANCODE_V, 53 },
    { SDL_SCANCODE_G, 54 },
    { SDL_SCANCODE_B, 55 },
    { SDL_SCANCODE_H, 56 },
    { SDL_SCANCODE_N, 57 },
    { SDL_SCANCODE_J, 58 },
    { SDL_SCANCODE_M, 59 },
    { SDL_SCANCODE_COMMA, 60 },
    { SDL_SCANCODE_L, 61 },
    { SDL_SCANCODE_PERIOD, 62 },
    { SDL_SCANCODE_SEMICOLON, 63 },
    { SDL_SCANCODE_SLASH, 64 },

    { SDL_SCANCODE_Q, 60 },
    { SDL_SCANCODE_2, 61 },
    { SDL_SCANCODE_W, 62 },
    { SDL_SCANCODE_3, 63 },
    { SDL_SCANCODE_E, 64 },
    { SDL_SCANCODE_R, 65 },
    { SDL_SCANCODE_5, 66 },
    { SDL_SCANCODE_T, 67 },
    { SDL_SCANCODE_6, 68 },
    { SDL_SCANCODE_Y, 69 },
    { SDL_SCANCODE_7, 70 },
    { SDL_SCANCODE_U, 71 },
    { SDL_SCANCODE_I, 72 },
    { SDL_SCANCODE_9, 73 },
    { SDL_SCANCODE_O, 74 },
    { SDL_SCANCODE_0, 75 },
    { SDL_SCANCODE_P, 76 },
    { SDL_SCANCODE_LEFTBRACKET, 77 },
    { SDL_SCANCODE_EQUALS, 78 },
    { SDL_SCANCODE_RIGHTBRACKET, 79 }
};

void setNoteOn(int note, double currTime) {
    if (noteAlreadyDown(note))
        return;

    int voiceSlot = getFreeVoiceIdx();
    auto& v = voices[voiceSlot];
    v.note = note;
    v.freq = pitch(v.note);
    v.volume = 1.0;
    v.pressTime = currTime;
    v.finishedPlaying = false;

    //printf("note on: %i (at %f)\n", v.note, v.pressTime);
}

void setNoteOff(int note, double currTime) { 
    while (true) {
        int voiceSlot = getVoiceWithNote(note);

        if (voiceSlot == -1) {
            break;
        }

        auto& v = voices[voiceSlot];
        v.volume = 0.0;
        v.releaseTime = currTime;

        //printf("note off: %i at %f (started at %f)\n", note, v.releaseTime, v.pressTime);
    }
}


SDL_Renderer* renderer;
SDL_Window* window;
TTF_Font* font;

void drawOctaveMode(int o) {
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    SDL_Rect rect;
    rect.w = 5;
    rect.h = 5;
    rect.x = 20 + (10 * o);
    rect.y = 450;
    SDL_RenderFillRect(renderer, &rect);
}

struct Label {
    Label(const char* text, SDL_Color color = SDL_Color { 255, 255, 255 }) {
        surface = TTF_RenderText_Solid(font, text, color);
        texture = SDL_CreateTextureFromSurface(renderer, surface);
    }

    void draw(int x, int y) {
        SDL_Rect rct;
        rct.w = surface->w;
        rct.h = surface->h;
        rct.x = x;
        rct.y = y;

        SDL_RenderCopy(renderer, texture, nullptr, &rct);
    }

    ~Label() {
        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
    }

    SDL_Surface* surface;
    SDL_Texture* texture;
};

// val is assumed to go 0-1!
void drawDial(int x, int y, float val, int radius) {
    int fX = x + (cosf(val * M_PI) * radius);
    int fY = y - (sinf(val * M_PI) * radius);

    SDL_RenderDrawLine(renderer, x, y, fX, fY);
}


int offset = 0;

void eventLoop() {
    bool exit = false;

    SDL_Surface* nameMsg = TTF_RenderText_Solid(font, "Someone Somewhere's Super Simple Software Synthesiser", SDL_Color { 255, 255, 255 });
    SDL_Texture* nameTex = SDL_CreateTextureFromSurface(renderer, nameMsg);

    SDL_Surface* crushLabelMsg = TTF_RenderText_Solid(font, "bitcrush", SDL_Color { 255, 255, 255 });
    SDL_Texture* crushLabelTex = SDL_CreateTextureFromSurface(renderer, crushLabelMsg);

    Label vuLabel { "vu meter", SDL_Color { 255, 255, 255 }};
    Label waveformLabel { "waveform", SDL_Color { 255, 255, 255 }};
    Label octaveModeLabel { "octave mode", SDL_Color { 255, 255, 255 }};
    Label clipLabel { "clipping!", SDL_Color { 255, 0, 0 }};
    Label lowpassLabel { "lowpass", SDL_Color { 255, 255, 255 }};
    Label* crushBitsLabel = new Label { "16.0" };
    double lastCrushBits = crushBits;

    double lastTime = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    timeAccumulator = lastTime;

    while (!exit) {
        SDL_Event evt;

        double currTime = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
        double deltaTime = currTime - lastTime;
        lastTime = currTime;

        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT)
                exit = true;

            if (evt.type == SDL_MOUSEBUTTONDOWN) {
                if (evt.button.button == SDL_BUTTON_RIGHT) {
                    currWaveFunc = (Waveform)(currWaveFunc + 1);

                    // wrap around
                    if (currWaveFunc == W_Count)
                        currWaveFunc = W_Sine;
                }
            }

            if (evt.type == SDL_KEYDOWN) { 
                if (evt.key.keysym.scancode == SDL_SCANCODE_KP_2) {
                    offset -= 12;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_8) {
                    offset += 12;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_9) {
                    unisonDetune = !unisonDetune;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_PLUS) {
                    unisonDetuneAmount += 0.0001f;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_MINUS) {
                    unisonDetuneAmount -= 0.0001f;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) {
                    enableBitcrush = !enableBitcrush;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_0) {
                    enableCompressor = !enableCompressor;  
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_7) {
                    crushBits += 0.1f;
                    crushBits = clamp(crushBits, 1.0, 31.0);
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_1) {
                    crushBits -= 0.1f;
                    crushBits = clamp(crushBits, 1.0, 31.0);
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_MULTIPLY) {
                    octaveMode = (OctaveMode)((int)octaveMode + 1);
                    if ((int)octaveMode > (int)OctaveMode::Quadruple) {
                        octaveMode = OctaveMode::Single;
                    }
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_DIVIDE) {
                    goofyUnison = !goofyUnison;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_UP) {
                    volume += 0.1f;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_DOWN) {
                    volume -= 0.1f;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_5) {
                    lpEnabled = !lpEnabled;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_3) {
                    lpQ += 0.01f;
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_KP_PERIOD) {
                    lpQ -= 0.01f;
                }
                lpQ = clamp(lpQ, 0.0f, 1.0f);
            }

            auto noteIt = freqs.find(evt.key.keysym.scancode);

            if (noteIt == freqs.end())
                continue;
            
            int note = noteIt->second + offset;

            if (evt.type == SDL_KEYDOWN) {
                setNoteOn(note, currTime);
                if (octaveMode == OctaveMode::Double) {
                    setNoteOn(note + 12, currTime);
                } else if (octaveMode == OctaveMode::Triple) {
                    setNoteOn(note + 12, currTime);
                    setNoteOn(note + 24, currTime);
                } else if (octaveMode == OctaveMode::Quadruple) {
                    setNoteOn(note + 12, currTime);
                    setNoteOn(note + 24, currTime);
                    setNoteOn(note + 36, currTime);
                }
            } else if (evt.type == SDL_KEYUP) {
                setNoteOff(note, currTime);

                if (octaveMode == OctaveMode::Double) {
                    setNoteOff(note + 12, currTime);
                } else if (octaveMode == OctaveMode::Triple) {
                    setNoteOff(note + 12, currTime);
                    setNoteOff(note + 24, currTime);
                } else if (octaveMode == OctaveMode::Quadruple) {
                    setNoteOff(note + 12, currTime);
                    setNoteOff(note + 24, currTime);
                    setNoteOff(note + 36, currTime);
                }
            }
        }

        int wWidth, wHeight;
        SDL_GetWindowSize(window, &wWidth, &wHeight);

        int numVoiceTilesX = wWidth / 72;

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

        SDL_RenderClear(renderer);
        // visualise voices
        for (int i = 0; i < NUM_VOICES; i++) {
            auto& v = voices[i];
            ADSRCurve curve;
            double vAttenuation = getADSAttenuation(curve, currTime - v.pressTime);
            
            if (v.volume == 0.0) {
                vAttenuation = getRAttenuation(curve, currTime - v.releaseTime);
            }

            SDL_SetRenderDrawColor(renderer, 0, 50, vAttenuation * 255, 255);
            SDL_Rect r;
            r.w = 64;
            r.h = 64;
            int tileX = i % numVoiceTilesX;
            int tileY = i / numVoiceTilesX;
            r.x = tileX * 72;
            r.y = 20 + tileY * 72;
            SDL_RenderFillRect(renderer, &r);

            if (v.volume == 0.0) continue;

            SDL_Rect r2;
            r2.w = 5;
            r2.h = 30;
            r2.x = v.note * 6;
            r2.y = 400;
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer, &r2);
        }

        // visualise waveform
        SDL_Point wPoints[128];

        for (int i = 0; i < 128; i++) {
            wPoints[i].x = i + 40;
            wPoints[i].y = (waveFuncs[currWaveFunc](2.0, i / 128.0) * 30) + 400;
        }

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawLines(renderer, wPoints, 128);
        waveformLabel.draw(40, 400 - 50); 

        static SDL_Point* oscPointsL = (SDL_Point*)malloc(sizeof(SDL_Point) * bufSize);
        static SDL_Point* oscPointsR = (SDL_Point*)malloc(sizeof(SDL_Point) * bufSize);

        for (int i = 0; i < bufSize; i++) {
            oscPointsL[i].x = i + 40;
            oscPointsL[i].y = (lastBufferL[i] * 100) + 480;
        }


        for (int i = 0; i < bufSize; i++) {
            oscPointsR[i].x = i + 40;
            oscPointsR[i].y = (lastBufferR[i] * 100) + 580;
        }

        SDL_RenderDrawLines(renderer, oscPointsL, bufSize);
        SDL_RenderDrawLines(renderer, oscPointsR, bufSize);

        // clipping indicator
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        SDL_Rect clipRect;
        clipRect.x = 64;
        clipRect.y = 500;
        clipRect.w = 64;
        clipRect.h = 64;
        if (hasClipped) {
            SDL_RenderFillRect(renderer, &clipRect);
            clipLabel.draw(64, 594);
        }

        if (unisonDetune) {
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
            SDL_Rect unisonRect;
            unisonRect.x = 128 + 4;
            unisonRect.y = 500;
            unisonRect.w = 64;
            unisonRect.h = 16 * (unisonDetuneAmount * 300);
            SDL_RenderFillRect(renderer, &unisonRect);
        }

        if (enableBitcrush) {
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 0);
            SDL_Rect labelRect;
            labelRect.x = 20;
            labelRect.y = 706;
            labelRect.w = crushLabelMsg->w;
            labelRect.h = crushLabelMsg->h;
            SDL_RenderCopy(renderer, crushLabelTex, nullptr, &labelRect);

            if (lastCrushBits != crushBits) {
                delete crushBitsLabel;
                char buf[5];
                sprintf(buf, "%.1f", crushBits);
                crushBitsLabel = new Label{buf};
                lastCrushBits = crushBits;
            }

            crushBitsLabel->draw(20, 756);

            drawDial(20 + 15, 756, (crushBits - 1.0f) / 15.0f, 30);
        }

        if (lpEnabled) {
            lowpassLabel.draw(50, 756);
            drawDial(50 + 15, 756, lpQ, 30);
        }

        {
            if (hasClipped)
                SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
            else
                SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
            SDL_Rect volRect;
            volRect.x = 128 + 4 + 72 + 72;
            volRect.y = 500 + (16 * 16);
            volRect.w = 64;
            volRect.h = -(16 * 16 * maxAmplitude);

            vuLabel.draw(128 + 4 + 72 + 72, 480);
            SDL_RenderFillRect(renderer, &volRect);
        }

        // Draw title
        SDL_Rect titleRect;
        titleRect.x = 5;
        titleRect.y = 2;
        titleRect.w = nameMsg->w;
        titleRect.h = nameMsg->h; 
        SDL_RenderCopy(renderer, nameTex, nullptr, &titleRect);

        // Visualise octave mode
        octaveModeLabel.draw(20, 430);
        for (int i = 0; i < (int)octaveMode + 1; i++) {
            drawOctaveMode(i);
        }

        SDL_RenderPresent(renderer);
    }
}

enum MsgType {
    M_NoteOff = 0, 
    M_NoteOn = 1,
    M_AftertouchPolyphonic = 2,
    M_ControlChange = 3,
    M_ProgramChange = 4,
    M_AftertouchChannel = 5,
    M_PitchBend = 6
};

void midiCallback(double deltatime, std::vector< unsigned char >* message, void* userData) {
    unsigned int nBytes = message->size();

    const unsigned char channelMask = 0b00001111;
    const unsigned char typeMask = 0b01110000;

    uint8_t channel = message->at(0) & channelMask;
    uint8_t type = (message->at(0) & typeMask) >> 4;
    printf("msg: channel %i, type %i\n", channel, type);

    if (nBytes == 3) {
        if (type == M_NoteOn) {
            int newNote = message->at(1);
            int newVel = message->at(2);

            double currTime = timeAccumulator;
            if (message->at(2) != 0) {
                setNoteOn(message->at(1) + offset, currTime);
                int oOffset = 0;
                for (int i = 0; i < (int)octaveMode; i++) {
                    oOffset += 12;
                    setNoteOn(message->at(1) + oOffset + offset, currTime);
                }
            } 
            printf("midi note on! velocity: %i, note: %i\n", newVel, newNote);
        }

        if (type == M_NoteOff) {
            setNoteOff(message->at(1) + offset, timeAccumulator);
            int oOffset = 0;
            for (int i = 0; i < (int)octaveMode; i++) {
                oOffset += 12;
                setNoteOff(message->at(1) + oOffset + offset, timeAccumulator);
            }
        }

        if (type == M_ControlChange) {
            printf("set cc %i to %i\n", message->at(1), message->at(2));

            if (message->at(1) == 28) {
                crushBits = 16.0f - ((message->at(2) / 127.0f) * 16.0f);
            }

            if (message->at(1) == 21) {
                volume = (message->at(2) / 127.0f) * 2.0f;
            }

            if (message->at(1) == 22) {
                unisonDetuneAmount = (message->at(2) / 127.0f) * 0.005f;
            }
        }

        if (type == M_PitchBend) {
            // pitch bend uses 14 bits for some reason
            int val = (message->at(1) & 0b01111111) |
                      ((message->at(2) & 0b01111111) << 7);
            pitchBendAmt = (((double)val) / 16384.0) - 0.5;
            printf("pitch bend: %f\n", pitchBendAmt);

        }
    }
}

void fancyThread(RtMidiOut* launchkeyOut) {
    while (true) {
        for (int i = 0; i < 8; i++) {
            bool on = ((maxAmplitude - (i * 0.125f)) > 0);
            int vel = on ? 21 : 0;

            if (i >= 5) {
                vel = on ? 13 : 0;
            }

            if (i == 7) {
                vel = on ? 5 : 0;
            }
            unsigned char msg2[] = { 0b10010000, i + 96, vel };
            launchkeyOut->sendMessage(msg2, 3);
        }

        for (int i = 0; i < 8; i++) {
            bool on = ((maxAmplitude - (i * 0.125f)) > 0);
            int vel = on ? 21 : 0;

            if (i >= 5) {
                vel = on ? 13 : 0;
            }

            if (i == 7) {
                vel = on ? 5 : 0;
            }
            unsigned char msg2[] = { 0b10010000, i + 112, vel };
            launchkeyOut->sendMessage(msg2, 3);
        }

        SDL_Delay(50);
    }
}

int main(int argc, char** argv) {
    SDL_Init(SDL_INIT_EVERYTHING);
    TTF_Init();
    font = TTF_OpenFont("font.ttf", 20);
    SDL_AudioSpec want;
    want.format = AUDIO_F32;
    want.samples = bufSize;
    want.freq = 44100; 
    want.userdata = nullptr;
    want.channels = nChannels;
    want.callback = audioCallback;
    want.silence = 0;

    SDL_AudioSpec got;

    auto devId = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

    if (devId == 0) {
        fprintf(stderr, "failed to open audio device: %s\n", SDL_GetError()); 
    }

    bufSize = got.samples;
    currentSampleRate = got.freq;
    nChannels = got.channels;

    window = SDL_CreateWindow("win", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    RtMidiIn* midiin = new RtMidiIn();
    RtMidiOut* launchkeyOut = new RtMidiOut();

    if (midiin->getPortCount() > 1) {
        for (int i = 0; i < midiin->getPortCount(); i++) {
            printf("port %i: %s\n", i, midiin->getPortName(i).c_str());
        }
        midiin->openPort(1);
        midiin->setCallback(midiCallback);
    } else {
        fprintf(stderr, "no midi ports!");
    }

    if (launchkeyOut->getPortCount() > 1) {
        for (int i = 0; i < launchkeyOut->getPortCount(); i++) {
            printf("out port %i: %s\n", i, launchkeyOut->getPortName(i).c_str());
        }
        launchkeyOut->openPort(4);

        unsigned char msg[] = { 0b10011111, 12, 0b01111111 };

        launchkeyOut->sendMessage(msg, 3);

        std::thread([&]() {fancyThread(launchkeyOut); }).detach();
    }

    SDL_PauseAudioDevice(devId, 0);
    eventLoop();

    unsigned char msg[] = { 0b10011111, 12, 0 };
    launchkeyOut->sendMessage(msg, 3);

    return 0;
}
