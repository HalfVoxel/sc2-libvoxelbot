#include <libvoxelbot/utilities/renderer.h>
#include <libvoxelbot/utilities/influence.h>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>

using namespace std;

namespace {

SDL_Rect CreateRect(int x, int y, int w, int h) {
    SDL_Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}
}  // namespace

struct Pixel {
    uint8_t b, g, r;
};

static vector<Pixel> colormap = { { 4, 0, 0 }, { 5, 0, 1 }, { 6, 1, 1 }, { 8, 1, 1 }, { 10, 1, 2 }, { 12, 2, 2 }, { 14, 2, 2 }, { 16, 2, 3 }, { 18, 3, 4 }, { 21, 3, 4 }, { 23, 4, 5 }, { 25, 4, 6 }, { 27, 5, 7 }, { 29, 6, 8 }, { 32, 6, 9 }, { 34, 7, 10 }, { 36, 7, 11 }, { 38, 8, 12 }, { 41, 8, 13 }, { 43, 9, 14 }, { 45, 9, 16 }, { 48, 10, 17 }, { 50, 10, 18 }, { 53, 11, 20 }, { 55, 11, 21 }, { 58, 11, 22 }, { 60, 12, 24 }, { 62, 12, 25 }, { 65, 12, 27 }, { 67, 12, 28 }, { 70, 12, 30 }, { 72, 12, 31 }, { 74, 12, 33 }, { 77, 12, 35 }, { 79, 12, 36 }, { 81, 12, 38 }, { 83, 11, 40 }, { 85, 11, 42 }, { 87, 11, 43 }, { 89, 11, 45 }, { 91, 10, 47 }, { 93, 10, 49 }, { 94, 10, 51 }, { 96, 10, 52 }, { 97, 9, 54 }, { 98, 9, 56 }, { 99, 9, 58 }, { 100, 9, 59 }, { 101, 9, 61 }, { 102, 9, 63 }, { 103, 10, 64 }, { 104, 10, 66 }, { 105, 10, 68 }, { 105, 10, 69 }, { 106, 11, 71 }, { 107, 11, 73 }, { 107, 12, 74 }, { 108, 12, 76 }, { 108, 13, 78 }, { 108, 13, 79 }, { 109, 14, 81 }, { 109, 14, 83 }, { 109, 15, 84 }, { 110, 15, 86 }, { 110, 16, 87 }, { 110, 17, 89 }, { 110, 17, 91 }, { 110, 18, 92 }, { 111, 18, 94 }, { 111, 19, 95 }, { 111, 20, 97 }, { 111, 20, 99 }, { 111, 21, 100 }, { 111, 21, 102 }, { 111, 22, 103 }, { 111, 23, 105 }, { 111, 23, 107 }, { 111, 24, 108 }, { 111, 24, 110 }, { 111, 25, 111 }, { 110, 25, 113 }, { 110, 26, 115 }, { 110, 27, 116 }, { 110, 27, 118 }, { 110, 28, 119 }, { 110, 28, 121 }, { 109, 29, 123 }, { 109, 29, 124 }, { 109, 30, 126 }, { 109, 31, 127 }, { 108, 31, 129 }, { 108, 32, 130 }, { 108, 32, 132 }, { 107, 33, 134 }, { 107, 33, 135 }, { 107, 34, 137 }, { 106, 34, 138 }, { 106, 35, 140 }, { 105, 36, 142 }, { 105, 36, 143 }, { 105, 37, 145 }, { 104, 37, 146 }, { 104, 38, 148 }, { 103, 38, 150 }, { 102, 39, 151 }, { 102, 40, 153 }, { 101, 40, 154 }, { 101, 41, 156 }, { 100, 41, 158 }, { 100, 42, 159 }, { 99, 43, 161 }, { 98, 43, 162 }, { 98, 44, 164 }, { 97, 45, 165 }, { 96, 45, 167 }, { 95, 46, 169 }, { 95, 46, 170 }, { 94, 47, 172 }, { 93, 48, 173 }, { 92, 49, 175 }, { 92, 49, 176 }, { 91, 50, 178 }, { 90, 51, 179 }, { 89, 51, 181 }, { 88, 52, 182 }, { 87, 53, 184 }, { 86, 54, 185 }, { 85, 54, 187 }, { 85, 55, 188 }, { 84, 56, 190 }, { 83, 57, 191 }, { 82, 58, 193 }, { 81, 59, 194 }, { 80, 60, 196 }, { 79, 60, 197 }, { 78, 61, 198 }, { 77, 62, 200 }, { 76, 63, 201 }, { 75, 64, 203 }, { 74, 65, 204 }, { 72, 66, 205 }, { 71, 67, 207 }, { 70, 68, 208 }, { 69, 69, 209 }, { 68, 70, 211 }, { 67, 72, 212 }, { 66, 73, 213 }, { 65, 74, 214 }, { 64, 75, 216 }, { 62, 76, 217 }, { 61, 77, 218 }, { 60, 79, 219 }, { 59, 80, 220 }, { 58, 81, 221 }, { 57, 82, 223 }, { 56, 84, 224 }, { 54, 85, 225 }, { 53, 86, 226 }, { 52, 88, 227 }, { 51, 89, 228 }, { 50, 90, 229 }, { 48, 92, 230 }, { 47, 93, 231 }, { 46, 95, 232 }, { 45, 96, 233 }, { 43, 98, 234 }, { 42, 99, 235 }, { 41, 101, 235 }, { 40, 102, 236 }, { 38, 104, 237 }, { 37, 105, 238 }, { 36, 107, 239 }, { 35, 109, 240 }, { 33, 110, 240 }, { 32, 112, 241 }, { 31, 113, 242 }, { 30, 115, 242 }, { 28, 117, 243 }, { 27, 118, 244 }, { 26, 120, 244 }, { 24, 122, 245 }, { 23, 123, 246 }, { 22, 125, 246 }, { 20, 127, 247 }, { 19, 129, 247 }, { 18, 130, 248 }, { 16, 132, 248 }, { 15, 134, 249 }, { 14, 136, 249 }, { 12, 137, 249 }, { 11, 139, 250 }, { 10, 141, 250 }, { 9, 143, 250 }, { 8, 145, 251 }, { 7, 146, 251 }, { 7, 148, 251 }, { 6, 150, 252 }, { 6, 152, 252 }, { 6, 154, 252 }, { 6, 156, 252 }, { 7, 158, 252 }, { 7, 160, 253 }, { 8, 161, 253 }, { 9, 163, 253 }, { 10, 165, 253 }, { 12, 167, 253 }, { 13, 169, 253 }, { 15, 171, 253 }, { 17, 173, 253 }, { 19, 175, 253 }, { 20, 177, 253 }, { 22, 179, 253 }, { 24, 181, 253 }, { 27, 183, 252 }, { 29, 185, 252 }, { 31, 186, 252 }, { 33, 188, 252 }, { 35, 190, 252 }, { 38, 192, 251 }, { 40, 194, 251 }, { 43, 196, 251 }, { 45, 198, 251 }, { 48, 200, 250 }, { 50, 202, 250 }, { 53, 204, 250 }, { 56, 206, 249 }, { 58, 208, 249 }, { 61, 210, 248 }, { 64, 212, 248 }, { 67, 214, 247 }, { 70, 216, 247 }, { 73, 218, 246 }, { 76, 220, 246 }, { 80, 222, 245 }, { 83, 224, 245 }, { 86, 226, 244 }, { 90, 228, 244 }, { 94, 229, 244 }, { 97, 231, 243 }, { 101, 233, 243 }, { 105, 235, 243 }, { 109, 237, 242 }, { 113, 238, 242 }, { 117, 240, 242 }, { 122, 241, 242 }, { 126, 243, 243 }, { 130, 244, 243 }, { 134, 246, 244 }, { 138, 247, 244 }, { 142, 249, 245 }, { 146, 250, 246 }, { 150, 251, 247 }, { 154, 252, 249 }, { 158, 253, 250 }, { 162, 254, 251 }, { 165, 255, 253 } };

MapRenderer::MapRenderer(const char* title, int x, int y, int w, int h, unsigned int flags) {
    int init_result = SDL_Init(SDL_INIT_VIDEO);
    if (init_result) {
        const char* error = SDL_GetError();
        std::cerr << "SDL_Init failed with error: " << error << std::endl;
        exit(1);
    }

    window = SDL_CreateWindow(title, x, y, w, h, flags == 0 ? SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE : flags);
    assert(window);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    assert(renderer);

    // Clear window to black.
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
}

void MapRenderer::shutdown() {
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    window = nullptr;
    renderer = nullptr;
}

const static int InfluenceMapScale = 2;

void MapRenderer::renderInfluenceMap(const InfluenceMap& influenceMap, int x0, int y0) {
    renderImageGrayscale((double*)influenceMap.weights.data(), influenceMap.w, influenceMap.h, InfluenceMapScale * x0 * (influenceMap.w + 5), InfluenceMapScale * y0 * (influenceMap.h + 5), InfluenceMapScale, false);
}

void MapRenderer::renderInfluenceMapNormalized(const InfluenceMap& influenceMap, int x0, int y0) {
    renderImageGrayscale((double*)influenceMap.weights.data(), influenceMap.w, influenceMap.h, InfluenceMapScale * x0 * (influenceMap.w + 5), InfluenceMapScale * y0 * (influenceMap.h + 5), InfluenceMapScale, true);
}

void MapRenderer::renderMatrix1BPP(const char* bytes, int w_mat, int h_mat, int off_x, int off_y, int px_w, int px_h) {
    if (!window) return;
    assert(renderer);
    assert(window);

    SDL_Rect rect = CreateRect(0, 0, px_w, px_h);
    for (int y = 0; y < h_mat; ++y) {
        for (int x = 0; x < w_mat; ++x) {
            rect.x = off_x + (int(x) * rect.w);
            rect.y = off_y + (int(y) * rect.h);

            int index = x + y * w_mat;
            unsigned char mask = 1 << (7 - (index % 8));
            unsigned char data = bytes[index / 8];
            bool value = (data & mask) != 0;

            if (value)
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            else
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

            SDL_RenderFillRect(renderer, &rect);
        }
    }
}

void MapRenderer::renderMatrix8BPPHeightMap(const char* bytes, int w_mat, int h_mat, int off_x, int off_y, int px_w, int px_h) {
    if (!window) return;
    assert(renderer);
    assert(window);

    SDL_Rect rect = CreateRect(0, 0, px_w, px_h);
    for (int y = 0; y < h_mat; ++y) {
        for (int x = 0; x < w_mat; ++x) {
            rect.x = off_x + (int(x) * rect.w);
            rect.y = off_y + (int(y) * rect.h);

            // Renders the height map in grayscale [0-255]
            int index = x + y * w_mat;
            SDL_SetRenderDrawColor(renderer, bytes[index], bytes[index], bytes[index], 255);
            SDL_RenderFillRect(renderer, &rect);
        }
    }
}

void MapRenderer::renderMatrix8BPPPlayers(const char* bytes, int w_mat, int h_mat, int off_x, int off_y, int px_w, int px_h) {
    if (!window) return;
    assert(renderer);
    assert(window);

    SDL_Rect rect = CreateRect(0, 0, px_w, px_h);
    for (int y = 0; y < h_mat; ++y) {
        for (int x = 0; x < w_mat; ++x) {
            rect.x = off_x + (int(x) * rect.w);
            rect.y = off_y + (int(y) * rect.h);

            int index = x + y * w_mat;
            switch (bytes[index]) {
                case 0:
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                    break;
                case 1:
                    // Self.
                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                    break;
                case 2:
                    // Enemy.
                    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
                    break;
                case 3:
                    // Neutral.
                    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
                    break;
                case 4:
                    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
                    break;
                case 5:
                    SDL_SetRenderDrawColor(renderer, 0, 255, 255, 255);
                    break;
                default:
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            }
            SDL_RenderFillRect(renderer, &rect);
        }
    }
}

void MapRenderer::renderImageRGB(const char* bytes, int width, int height, int off_x, int off_y, int scale) {
    if (!window) return;
    assert(renderer);
    assert(window);

    SDL_Surface* surface = SDL_CreateRGBSurfaceFrom((void*)bytes, width, height, 24, 3 * width, 0xFF0000, 0x00FF00, 0x0000FF, 0x000000);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);

    SDL_Rect dstRect = CreateRect(off_x, off_y, width * scale, height * scale);
    SDL_RenderCopyEx(renderer, texture, NULL, &dstRect, 0, NULL, SDL_FLIP_VERTICAL);

    SDL_DestroyTexture(texture);
}

static vector<Pixel> converted;
void MapRenderer::renderImageGrayscale(const double* values, int width, int height, int off_x, int off_y, int scale, bool normalized) {
    if (!window) return;
    converted.resize(width * height);
    double multiplier = normalized ? 256.0 : 256.0 / 3.0;
    for (int i = 0; i < width * height; i++) {
        if (isfinite(values[i])) {
            converted[i] = colormap[max(min((int)(values[i] * multiplier), 255), 0)];
        } else {
            if (values[i] < 0) {
                // Neg inf
                converted[i] = { 0, 0, 200 };
            } else if (values[i] > 0) {
                // Pos inf
                converted[i] = { 200, 0, 0 };
            } else {
                // NaN
                converted[i] = { 255, 0, 255 };
            }
        }
    }

    // Make a border
    for (int x = 0; x < width; x++) {
        converted[0 * width + x] = converted[width * (height - 1) + x] = { 200, 200, 200 };
    }
    for (int y = 0; y < height; y++) {
        converted[y * width + 0] = converted[width * y + width - 1] = { 200, 200, 200 };
    }

    renderImageRGB((const char*)converted.data(), width, height, off_x, off_y, scale);
}

void MapRenderer::present() {
    if (!window) return;
    assert(renderer);
    assert(window);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            shutdown();
            return;
        }
    }

    SDL_RenderPresent(renderer);
    SDL_RenderClear(renderer);
}
