/* Display: the game's 320x200 8-bit frame -> window, through OpenGL with a small shader.
 *
 * Filters  0 Sharp   : "sharp bilinear" - integer-scaled pixels, only the pixel edges blended,
 *                      so pixels stay crisp and evenly sized at any window size
 *          1 Pixel   : nearest neighbour
 *          2 Soft    : plain bilinear
 *          3 Smooth HQ: Scale3x (EPX) edge smoothing on the CPU, then sharp bilinear
 * Aspect   0 4:3 as on a CRT (letterboxed)   1 Wide: stretched to the window
 *          2 Wide panorama: centre kept at 4:3, the stretch grows toward the sides
 * Falls back to GDI (nearest, 4:3 / stretch) when OpenGL 2 is not available.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void logmsg(const char *fmt, ...);

/* ---- OpenGL 2.0 entry points (Windows' gl.h only covers 1.1) */
typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CLAMP_TO_EDGE 0x812F
typedef GLuint (APIENTRY *PFNCREATESHADER)(GLenum);
typedef void (APIENTRY *PFNSHADERSOURCE)(GLuint, GLsizei, const GLchar **, const GLint *);
typedef void (APIENTRY *PFNCOMPILESHADER)(GLuint);
typedef void (APIENTRY *PFNGETSHADERIV)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFNGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (APIENTRY *PFNCREATEPROGRAM)(void);
typedef void (APIENTRY *PFNATTACHSHADER)(GLuint, GLuint);
typedef void (APIENTRY *PFNLINKPROGRAM)(GLuint);
typedef void (APIENTRY *PFNGETPROGRAMIV)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFNUSEPROGRAM)(GLuint);
typedef GLint (APIENTRY *PFNGETUNIFORMLOCATION)(GLuint, const GLchar *);
typedef void (APIENTRY *PFNUNIFORM1I)(GLint, GLint);
typedef void (APIENTRY *PFNUNIFORM1F)(GLint, GLfloat);
typedef void (APIENTRY *PFNUNIFORM2F)(GLint, GLfloat, GLfloat);
typedef BOOL (WINAPI *PFNSWAPINTERVAL)(int);
static PFNCREATESHADER glCreateShader; static PFNSHADERSOURCE glShaderSource; static PFNCOMPILESHADER glCompileShader;
static PFNGETSHADERIV glGetShaderiv; static PFNGETSHADERINFOLOG glGetShaderInfoLog; static PFNCREATEPROGRAM glCreateProgram;
static PFNATTACHSHADER glAttachShader; static PFNLINKPROGRAM glLinkProgram; static PFNGETPROGRAMIV glGetProgramiv;
static PFNUSEPROGRAM glUseProgram; static PFNGETUNIFORMLOCATION glGetUniformLocation; static PFNUNIFORM1I glUniform1i;
static PFNUNIFORM1F glUniform1f; static PFNUNIFORM2F glUniform2f;

static const char *vs_src =
    "varying vec2 uv;\n"
    "void main() { uv = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";
static const char *fs_src =
    "uniform sampler2D tex;\n"
    "uniform vec2 tex_size;\n"      /* texture size in texels */
    "uniform vec2 out_size;\n"      /* size of the picture on screen in pixels */
    "uniform int sharp;\n"          /* 1: sharp bilinear, 0: sample as is (nearest or bilinear by texture filter) */
    "uniform float pano;\n"         /* panorama: slope at the centre (1 = linear) */
    "varying vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = uv;\n"
    "  if (pano < 0.999) { float t = p.x * 2.0 - 1.0; p.x = 0.5 + 0.5 * t * (pano + (1.0 - pano) * t * t); }\n"
    "  if (sharp == 1) {\n"
    "    vec2 texel = p * tex_size;\n"
    "    vec2 scale = max(floor(out_size / tex_size), vec2(1.0));\n"
    "    vec2 fl = floor(texel);\n"
    "    vec2 d = fract(texel) - 0.5;\n"
    "    vec2 range = 0.5 - 0.5 / scale;\n"
    "    vec2 f = (d - clamp(d, -range, range)) * scale + 0.5;\n"
    "    p = (fl + f) / tex_size;\n"
    "  }\n"
    "  gl_FragColor = texture2D(tex, p);\n"
    "}\n";

static HDC gl_dc; static HGLRC gl_rc; static GLuint prog, tex_id; static int gl_ok, tex_w, tex_h, tex_filter = -1;
static GLint u_tex, u_tex_size, u_out_size, u_sharp, u_pano;
static uint32_t *rgba;

static int load_gl(void) {
#define L(n, T) if (!(n = (T)(void *)wglGetProcAddress(#n))) return 0
    L(glCreateShader, PFNCREATESHADER); L(glShaderSource, PFNSHADERSOURCE); L(glCompileShader, PFNCOMPILESHADER);
    L(glGetShaderiv, PFNGETSHADERIV); L(glGetShaderInfoLog, PFNGETSHADERINFOLOG); L(glCreateProgram, PFNCREATEPROGRAM);
    L(glAttachShader, PFNATTACHSHADER); L(glLinkProgram, PFNLINKPROGRAM); L(glGetProgramiv, PFNGETPROGRAMIV);
    L(glUseProgram, PFNUSEPROGRAM); L(glGetUniformLocation, PFNGETUNIFORMLOCATION); L(glUniform1i, PFNUNIFORM1I);
    L(glUniform1f, PFNUNIFORM1F); L(glUniform2f, PFNUNIFORM2F);
#undef L
    return 1;
}
static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type); GLint ok = 0;
    glShaderSource(s, 1, &src, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); logmsg("shader error: %s\n", log); return 0; }
    return s;
}

void video_init(HWND hwnd) {
    gl_dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    int pf = ChoosePixelFormat(gl_dc, &pfd);
    if (!pf || !SetPixelFormat(gl_dc, pf, &pfd) || !(gl_rc = wglCreateContext(gl_dc)) || !wglMakeCurrent(gl_dc, gl_rc)) {
        logmsg("video: OpenGL not available, using GDI\n"); return;
    }
    if (!load_gl()) { logmsg("video: OpenGL 2 not available, using GDI\n"); return; }
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return;
    prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { logmsg("video: shader link failed, using GDI\n"); return; }
    u_tex = glGetUniformLocation(prog, "tex"); u_tex_size = glGetUniformLocation(prog, "tex_size");
    u_out_size = glGetUniformLocation(prog, "out_size"); u_sharp = glGetUniformLocation(prog, "sharp");
    u_pano = glGetUniformLocation(prog, "pano");
    PFNSWAPINTERVAL swap = (PFNSWAPINTERVAL)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap) swap(0);          /* never block the game loop on vsync */
    glGenTextures(1, &tex_id);
    rgba = malloc(960 * 600 * 4);
    gl_ok = 1;
    logmsg("video: OpenGL %s\n", (const char *)glGetString(GL_RENDERER));
}

/* Scale3x (EPX 3x): smooths diagonal edges of pixel art without blurring */
static void scale3x(const uint32_t *s, uint32_t *d) {
    for (int y = 0; y < 200; y++) {
        const uint32_t *r0 = s + (y > 0 ? y - 1 : y) * 320, *r1 = s + y * 320, *r2 = s + (y < 199 ? y + 1 : y) * 320;
        uint32_t *o0 = d + (y * 3) * 960, *o1 = o0 + 960, *o2 = o1 + 960;
        for (int x = 0; x < 320; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x < 319 ? x + 1 : x;
            uint32_t A = r0[xl], B = r0[x], C = r0[xr], D = r1[xl], E = r1[x], F = r1[xr], G = r2[xl], H = r2[x], I = r2[xr];
            uint32_t e0 = E, e1 = E, e2 = E, e3 = E, e5 = E, e6 = E, e7 = E, e8 = E;
            if (B != H && D != F) {
                e0 = D == B ? D : E;
                e1 = (D == B && E != C) || (B == F && E != A) ? B : E;
                e2 = B == F ? F : E;
                e3 = (D == B && E != G) || (D == H && E != A) ? D : E;
                e5 = (B == F && E != I) || (H == F && E != C) ? F : E;
                e6 = D == H ? D : E;
                e7 = (D == H && E != I) || (H == F && E != G) ? H : E;
                e8 = H == F ? F : E;
            }
            o0[x * 3] = e0; o0[x * 3 + 1] = e1; o0[x * 3 + 2] = e2;
            o1[x * 3] = e3; o1[x * 3 + 1] = E;  o1[x * 3 + 2] = e5;
            o2[x * 3] = e6; o2[x * 3 + 1] = e7; o2[x * 3 + 2] = e8;
        }
    }
}

/* frame: 320x200 palette indices; pal: 256 x RGB (6-bit VGA values) */
void video_present(HWND hwnd, const uint8_t *frame, const uint8_t pal[256][3], int filter, int aspect) {
    RECT r; GetClientRect(hwnd, &r);
    int w = r.right, h = r.bottom;
    if (w <= 0 || h <= 0) return;
    /* picture rectangle */
    int dw = w, dh = h;
    if (aspect == 0) { dw = w; dh = w * 3 / 4; if (dh > h) { dh = h; dw = h * 4 / 3; } }
    int dx = (w - dw) / 2, dy = (h - dh) / 2;
    double pano = 1.0;
    if (aspect == 2 && dh > 0) { pano = (4.0 / 3.0) / ((double)dw / dh); if (pano > 1) pano = 1; }

    if (!gl_ok) {           /* GDI fallback */
        static BITMAPINFO *bi;
        if (!bi) { bi = calloc(1, sizeof(BITMAPINFOHEADER) + 1024); bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                   bi->bmiHeader.biWidth = 320; bi->bmiHeader.biHeight = -200; bi->bmiHeader.biPlanes = 1; bi->bmiHeader.biBitCount = 8; }
        for (int i = 0; i < 256; i++) { bi->bmiColors[i].rgbRed = (BYTE)(pal[i][0] << 2 | pal[i][0] >> 4);
            bi->bmiColors[i].rgbGreen = (BYTE)(pal[i][1] << 2 | pal[i][1] >> 4); bi->bmiColors[i].rgbBlue = (BYTE)(pal[i][2] << 2 | pal[i][2] >> 4); }
        HDC dc = GetDC(hwnd);
        if (dx > 0) { RECT a = {0, 0, dx, h}, b = {dx + dw, 0, w, h}; FillRect(dc, &a, GetStockObject(BLACK_BRUSH)); FillRect(dc, &b, GetStockObject(BLACK_BRUSH)); }
        if (dy > 0) { RECT a = {0, 0, w, dy}, b = {0, dy + dh, w, h}; FillRect(dc, &a, GetStockObject(BLACK_BRUSH)); FillRect(dc, &b, GetStockObject(BLACK_BRUSH)); }
        SetStretchBltMode(dc, filter == 2 ? HALFTONE : COLORONCOLOR);
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, 320, 200, frame, bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);
        return;
    }

    uint32_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = 0xFF000000u | (uint32_t)(pal[i][2] << 2 | pal[i][2] >> 4) << 16 | (uint32_t)(pal[i][1] << 2 | pal[i][1] >> 4) << 8 | (uint32_t)(pal[i][0] << 2 | pal[i][0] >> 4);
    static uint32_t base[320 * 200];
    for (int i = 0; i < 320 * 200; i++) base[i] = lut[frame[i]];
    int tw = 320, th = 200; const uint32_t *pix = base;
    if (filter == 3) { scale3x(base, rgba); tw = 960; th = 600; pix = rgba; }

    glBindTexture(GL_TEXTURE_2D, tex_id);
    int want = (filter == 1) ? GL_NEAREST : GL_LINEAR;
    if (tw != tex_w || th != tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tex_w = tw; tex_h = th; tex_filter = -1;
    } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    if (want != tex_filter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, want);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, want);
        tex_filter = want;
    }
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glViewport(dx, h - dy - dh, dw, dh);
    glUseProgram(prog);
    glUniform1i(u_tex, 0);
    glUniform2f(u_tex_size, (float)tw, (float)th);
    glUniform2f(u_out_size, (float)dw, (float)dh);
    glUniform1i(u_sharp, (filter == 0 || filter == 3) ? 1 : 0);
    glUniform1f(u_pano, (float)pano);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, 1);
    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glEnd();
    SwapBuffers(gl_dc);
}
