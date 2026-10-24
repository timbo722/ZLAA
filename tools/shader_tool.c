/*
 * shader_tool.c - translate one dumped NVN shader to SPIR-V, for testing the translator
 * without running the game. Dumps come from LAA_DUMP_SHADERS=dir (NAME.code and NAME.ctrl,
 * the shader and its NVN control section).
 *
 *   shader_tool <shader.code> [out.spv]
 *
 * Prints what the shader uses, or why it couldn't be translated (exit status 1).
 */
#include "render/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <shader.code> [out.spv]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t code[1 << 20];
    size_t size = fread(code, 1, sizeof code, f);
    fclose(f);
    int stage = SHADER_COMPUTE;
    uint32_t magic, sph0;
    memcpy(&magic, code, 4);
    if (magic == 0x12345678) {                           /* graphics: the header says which stage */
        memcpy(&sph0, code + 0x30, 4);
        static const int by_type[16] = { [1] = SHADER_VERTEX, [2] = SHADER_TESS_CONTROL, [3] = SHADER_TESS_EVAL,
                                         [4] = SHADER_GEOMETRY, [5] = SHADER_FRAGMENT };
        stage = by_type[(sph0 >> 10) & 0xF];
    }
    /* its constants: where the control section says, inside the code */
    uint32_t c_off = 0, c_size = 0;
    char ctrl_path[4096];
    snprintf(ctrl_path, sizeof ctrl_path, "%.*s.ctrl", (int)(strlen(argv[1]) - 5), argv[1]);
    FILE *cf = fopen(ctrl_path, "rb");
    static uint8_t control[0x1000];
    int have_control = 0;
    if (cf) {
        if (fread(control, 1, sizeof control, cf) == sizeof control) { shader_constants(control, &c_off, &c_size); have_control = 1; }
        fclose(cf);
    }
    if (c_off > size || c_size > size - c_off) c_off = c_size = 0;
    ShaderInfo info;
    if (shader_translate(code, size, stage, code + c_off, c_size, have_control ? control : NULL, SHADER_GL_DEPTH, 0, &info) != 0) {
        printf("%s: FAIL (%s)\n", argv[1], info.error);
        return 1;
    }
    printf("%s: ok, %u words, cbufs %#x, %u textures, inputs %#x, outputs %#x\n", argv[1], info.words,
           info.cbuf_mask, info.ntextures, info.inputs, info.outputs);
    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        if (!o) { perror(argv[2]); return 2; }
        fwrite(info.spirv, 4, info.words, o);
        fclose(o);
    }
    shader_free(&info);
    return 0;
}
